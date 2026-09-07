#!/usr/bin/env node
'use strict';

/**
 * codex-companion - desk companion for the Codex CLI.
 *
 * This is the entire host program. One file, no dependencies, no build step,
 * no install script, no background service. Node 20+.
 *
 * WHAT IT DOES
 *   Writes one short JSON line down a USB cable to a LILYGO T-Display-S3 so a
 *   small screen on your desk can show what your Codex session is doing.
 *
 * WHAT IT READS
 *   - Codex hook payloads on stdin. Of those it looks at exactly two fields:
 *     `hook_event_name` and `transcript_path`. It never looks at `tool_input`,
 *     `tool_response`, `prompt`, `last_assistant_message`, `cwd`, `model` or
 *     `session_id`.
 *   - Optionally, the tail of the session rollout file Codex already writes,
 *     for numbers the hook payload does not carry: token counts, context
 *     window size, rate-limit percentages, turn start time. Only lines that
 *     contain one of the METRIC_HINTS are parsed at all, so lines carrying
 *     your prompts, the model's replies, or command output are skipped before
 *     JSON.parse ever sees them. Nothing textual is retained: every value that
 *     survives into a frame is a number, a timestamp, or a fixed state word.
 *
 * WHAT IT NEVER DOES
 *   - It never approves or denies anything. The PermissionRequest handler
 *     prints nothing on stdout and exits 0, which is Codex's documented way of
 *     declining to decide; the approval prompt still goes to you, in your
 *     terminal, exactly as it would without this program. It is also
 *     registered with "async": true, which makes Codex refuse to apply any
 *     decision from it at all (hooks/src/engine/mod.rs,
 *     can_apply_control_effects returns false for async handlers). Two
 *     independent reasons, either one sufficient.
 *   - It never opens a network connection. There is no require of http,
 *     https, net, dgram, dns, tls, and no fetch. Grep for them.
 *   - It never installs a launch agent, a daemon, a login item or a cron job.
 *     There is no autostart anywhere in this file.
 *   - It never writes to your Codex config, except the one explicit
 *     `install-hook` subcommand, which prints exactly what it is about to
 *     write, merges into any existing hooks.json rather than replacing it, and
 *     is undone exactly by `uninstall-hook`.
 *
 * The device is a gift and this program is an optional bonus. Every failure
 * path here is "do nothing and exit 0", because a broken desk toy must never
 * be able to break somebody's work.
 */

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { execFileSync } = require('node:child_process');

// --------------------------------------------------------------------------
// 1. Constants
// --------------------------------------------------------------------------

/** Espressif's USB vendor id and the T-Display-S3's native-USB product id. */
const ESP_VENDOR_ID = 0x303a; // 12346 decimal, which is how ioreg prints it
const ESP_PRODUCT_ID = 0x1001; // 4097 decimal

/** Serial line settings. The firmware calls Serial.begin(115200). */
const BAUD = '115200';

/** The firmware discards any line longer than this, whole. */
const MAX_LINE_BYTES = 512;

/** Firmware buffer sizes, minus the NUL. Clamp here so it never truncates. */
const CENTER_MAX = 15;
const LABEL_MAX = 23;
const SUB_MAX = 39;

/** The five words the firmware accepts for `state`. Anything else is dropped. */
const DEVICE_STATES = new Set(['sleep', 'idle', 'busy', 'waiting', 'done']);

/** Codex's own baseline constant, used by its context-fill formula. */
const BASELINE_TOKENS = 12000;

/** How stale a rate-limit reading may be before we stop showing it. */
const QUOTA_MAX_AGE_MS = 15 * 60 * 1000;

/** How full a rate-limit window must be before it takes the ring from CTX. */
const QUOTA_TAKEOVER_PERCENT = 80;

/** How much of the rollout tail we are willing to read for metrics. */
const TAIL_BYTES = 128 * 1024;

/**
 * A rollout line is only parsed when it contains one of these. Everything a
 * session writes that is not one of these record types (your prompts, the
 * model's replies, tool output, file contents) is skipped as raw text and
 * never becomes a JavaScript object.
 */
const METRIC_HINTS = [
  '"token_count"',
  '"token_usage_record"',
  '"compacted"',
  '"task_started"',
  '"turn_started"',
  '"task_complete"',
  '"turn_complete"',
  '"turn_aborted"'
];

/** The twelve event names Codex's hooks.json accepts, spelled exactly. */
const ALL_HOOK_EVENTS = [
  'PreToolUse',
  'PermissionRequest',
  'PostToolUse',
  'PreCompact',
  'PostCompact',
  'SessionStart',
  'SessionEnd',
  'UserPromptSubmit',
  'SubagentStart',
  'SubagentStop',
  'Stop',
  'Interrupt'
];

/**
 * Event -> device state.
 *
 * There is no PermissionResolved event in Codex, so a `waiting` indication is
 * cleared by whatever happens next: PreToolUse, PostToolUse or Stop. If
 * nothing happens next (you walked away), the device's own staleness timers
 * take over: dim at 30s, ambient at 5 minutes.
 */
const EVENT_STATE = {
  SessionStart: 'idle',
  UserPromptSubmit: 'busy',
  PreToolUse: 'busy',
  PermissionRequest: 'waiting',
  PostToolUse: 'busy',
  PreCompact: 'busy',
  PostCompact: 'busy',
  SubagentStart: 'busy',
  SubagentStop: 'busy',
  Stop: 'done',
  Interrupt: 'idle',
  SessionEnd: 'idle'
};

/** The events we register. The other four still work if you add them by hand. */
const INSTALLED_EVENTS = [
  'SessionStart',
  'UserPromptSubmit',
  'PreToolUse',
  'PermissionRequest',
  'PostToolUse',
  'Stop',
  'Interrupt',
  'SessionEnd'
];

/**
 * SessionEnd is the one event Codex always runs synchronously, and it caps the
 * timeout at 3 seconds (hooks/src/events/session_end.rs). Registering it as
 * async would only earn a "running async SessionEnd hook synchronously"
 * warning, so we ask for what actually happens.
 */
const SESSION_END_TIMEOUT_SEC = 2;
const ASYNC_TIMEOUT_SEC = 5;

// --------------------------------------------------------------------------
// 2. Finding the device
// --------------------------------------------------------------------------

/** Every callout tty macOS currently has. Returns [] on any error. */
function listCalloutPorts(dir) {
  try {
    return fs
      .readdirSync(dir || '/dev')
      .filter((n) => n.startsWith('cu.usbmodem') || n.startsWith('cu.usbserial'))
      .map((n) => path.join(dir || '/dev', n))
      .sort();
  } catch {
    return [];
  }
}

/**
 * Parse `ioreg -r -c IOUSBHostDevice -l` into [{path, vendorId, productId}].
 *
 * ioreg prints each matching USB device followed by its subtree, and the
 * serial driver that owns the tty is inside that subtree, so the vendor and
 * product ids most recently seen above an "IOCalloutDevice" line are the ids
 * of the device that tty belongs to. Nested devices (a hub's children) restate
 * their own ids before their own serial client, so the rule holds for them too.
 *
 * Pure function of the text, so it is testable without a board attached.
 */
function parseIoreg(text) {
  const out = [];
  let vendorId = null;
  let productId = null;
  for (const line of String(text || '').split('\n')) {
    let m = line.match(/"idVendor"\s*=\s*(\d+)/);
    if (m) {
      vendorId = Number(m[1]);
      continue;
    }
    m = line.match(/"idProduct"\s*=\s*(\d+)/);
    if (m) {
      productId = Number(m[1]);
      continue;
    }
    m = line.match(/"IOCalloutDevice"\s*=\s*"([^"]+)"/);
    if (m) out.push({ path: m[1], vendorId, productId });
  }
  return out;
}

/** Run ioreg. Any failure is reported as "no information", never as an error. */
function readIoreg() {
  try {
    return execFileSync('/usr/sbin/ioreg', ['-r', '-c', 'IOUSBHostDevice', '-l', '-w0'], {
      encoding: 'utf8',
      timeout: 5000,
      maxBuffer: 32 * 1024 * 1024
    });
  } catch {
    return '';
  }
}

/**
 * Decide which tty to talk to.
 *
 * Deliberately refuses to guess. Two boards that both identify as an ESP32-S3
 * is a situation only the human can settle, so it returns no port and says so.
 *
 * @returns {{chosen:string|null, matched:string[], ports:string[], reason:string}}
 */
function choosePort(opts) {
  const o = opts || {};
  const ports = o.ports || listCalloutPorts();
  const usb = parseIoreg(o.ioreg === undefined ? readIoreg() : o.ioreg);

  if (o.port) {
    return {
      chosen: o.port,
      matched: [o.port],
      ports,
      reason: `--port ${o.port} (identity not checked, you asked for this one)`
    };
  }

  const wantVid = o.vendorId === undefined ? ESP_VENDOR_ID : o.vendorId;
  const wantPid = o.productId === undefined ? ESP_PRODUCT_ID : o.productId;
  const matched = usb
    .filter((d) => d.vendorId === wantVid && d.productId === wantPid)
    .map((d) => d.path)
    .filter((p) => ports.length === 0 || ports.includes(p));

  if (matched.length === 1) {
    return {
      chosen: matched[0],
      matched,
      ports,
      reason: `${matched[0]} reports USB ${hex(wantVid)}:${hex(wantPid)}`
    };
  }
  if (matched.length > 1) {
    return {
      chosen: null,
      matched,
      ports,
      reason:
        `${matched.length} devices report USB ${hex(wantVid)}:${hex(wantPid)} ` +
        `(${matched.join(', ')}). Pass --port to say which one.`
    };
  }
  return {
    chosen: null,
    matched,
    ports,
    reason:
      ports.length === 0
        ? 'no /dev/cu.usbmodem* ports at all: check the cable (a charge-only cable powers the board but carries no data)'
        : `no port reports USB ${hex(wantVid)}:${hex(wantPid)}; saw ${ports.join(', ')}. Pass --port to override.`
  };
}

function hex(n) {
  return Number.isFinite(n) ? n.toString(16).padStart(4, '0') : '?';
}

// --------------------------------------------------------------------------
// 3. Talking to the device
//
// A USB CDC port on macOS is just a tty. `stty -f <port> 115200 raw -echo`
// configures it and fs.writeSync sends bytes. That is the whole transport;
// there is no native module and nothing to compile.
//
// Write-only by default. A write-only link cannot block waiting for a reply
// that never comes, which matters because the board's USB CDC transmit path
// runs one message behind: the `ok` for line N usually only arrives once line
// N+1 has been written. Never build a per-line handshake on that.
// --------------------------------------------------------------------------

class Link {
  /**
   * @param {string} portPath
   * @param {{read?:boolean}} [opts] read:true also opens for reading, which
   *   only `doctor` does, to confirm the board's greeting.
   */
  constructor(portPath, opts) {
    this.path = portPath;
    this.read = Boolean(opts && opts.read);
    this.fd = null;
  }

  /** Open and configure. Throws only if the port cannot be opened. */
  open() {
    const flags =
      (this.read ? fs.constants.O_RDWR : fs.constants.O_WRONLY) |
      fs.constants.O_NOCTTY |
      fs.constants.O_NONBLOCK;
    this.fd = fs.openSync(this.path, flags);
    // stty opens its own descriptor, but termios state belongs to the device,
    // so configuring it while we hold the port open is what makes it stick.
    try {
      execFileSync('/bin/stty', ['-f', this.path, BAUD, 'raw', '-echo'], { timeout: 5000 });
    } catch {
      // A port that will not take stty may still be perfectly writable; the
      // board ignores baud on native USB CDC anyway. Not worth failing over.
    }
    return this;
  }

  /**
   * Write one already-built frame object as one line.
   * Never throws, never blocks for more than `deadlineMs`.
   * @returns {boolean} true if the bytes went out
   */
  writeFrame(frame, deadlineMs) {
    return this.writeLine(frameToLine(frame), deadlineMs);
  }

  writeLine(line, deadlineMs) {
    if (this.fd === null) return false;
    const buf = Buffer.from(line, 'utf8');
    if (buf.length > MAX_LINE_BYTES) return false; // the board would drop it anyway
    const until = Date.now() + (Number.isFinite(deadlineMs) ? deadlineMs : 200);
    let off = 0;
    while (off < buf.length) {
      try {
        off += fs.writeSync(this.fd, buf, off, buf.length - off);
      } catch (e) {
        // EAGAIN: the kernel's tty buffer is full. Give the cable a moment,
        // but never past the deadline; a desk toy may not stall a turn.
        if ((e.code === 'EAGAIN' || e.code === 'EWOULDBLOCK') && Date.now() < until) continue;
        return false;
      }
    }
    return true;
  }

  /** Non-blocking read of whatever the board has said. Only doctor uses this. */
  readAvailable() {
    if (this.fd === null || !this.read) return '';
    const buf = Buffer.alloc(4096);
    try {
      const n = fs.readSync(this.fd, buf, 0, buf.length, null);
      return n > 0 ? buf.subarray(0, n).toString('utf8') : '';
    } catch {
      return '';
    }
  }

  close() {
    if (this.fd === null) return;
    try {
      fs.closeSync(this.fd);
    } catch {
      /* already gone */
    }
    this.fd = null;
  }
}

/** Open the chosen device, or return null. Never throws. */
function openLink(opts) {
  const pick = choosePort(opts);
  if (!pick.chosen) return null;
  try {
    return new Link(pick.chosen, opts).open();
  } catch {
    return null;
  }
}

// --------------------------------------------------------------------------
// 4. Building a frame
//
// The wire protocol is one JSON object per line. Every field may be omitted
// and the board keeps the last value it saw, which is what lets the hook own
// `state` while the metrics path owns the numbers. See docs/protocol.md.
// --------------------------------------------------------------------------

function clamp01(n) {
  if (!Number.isFinite(n)) return 0;
  return n < 0 ? 0 : n > 1 ? 1 : n;
}

function round2(n) {
  return Math.round(n * 100) / 100;
}

/** Seconds -> "12:34", or "1:02:03" past an hour. */
function formatElapsed(sec) {
  if (!Number.isFinite(sec) || sec < 0) sec = 0;
  const total = Math.floor(sec);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
}

/**
 * Pick the rate-limit window worth showing, or null.
 *
 * A snapshot only lands on disk when Codex emits a token_count event, so it
 * goes stale between turns. A stale window pinned at 100% would light a
 * permanent full ring, which is the one thing the ring must never do; hence
 * the freshness check and the "reset time already passed" check. Among the
 * survivors the fullest window wins, because a 12%-used 5-hour window would
 * otherwise hide a 97%-used weekly one.
 */
function pickQuotaWindow(rateLimits, nowMs) {
  if (!rateLimits || typeof rateLimits !== 'object') return null;
  const observedAt = rateLimits.observedAtMs;
  if (!Number.isFinite(observedAt)) return null;
  if (Math.abs(nowMs - observedAt) > QUOTA_MAX_AGE_MS) return null;

  let best = null;
  for (const key of ['primary', 'secondary']) {
    const w = rateLimits[key];
    if (!w || typeof w !== 'object') continue;
    if (!Number.isFinite(w.used_percent)) continue;
    if (Number.isFinite(w.resets_at) && w.resets_at * 1000 <= nowMs) continue;
    if (best === null || w.used_percent > best.window.used_percent) best = { key, window: w };
  }
  return best;
}

/**
 * Build the metric half of a frame: ring, center, label, sub, tps.
 * No `state`, deliberately: the hook owns that.
 *
 * @param {object} m   metrics from readMetrics(), or {}
 * @param {number} nowMs
 */
function metricFields(m, nowMs) {
  const metrics = m || {};
  const ctxKnown = Number.isFinite(metrics.ctxFill);
  const elapsed = Number.isFinite(metrics.elapsedSec) ? metrics.elapsedSec : null;
  const quota = pickQuotaWindow(metrics.rateLimits, nowMs);
  const quotaWins =
    quota !== null && (!ctxKnown || quota.window.used_percent >= QUOTA_TAKEOVER_PERCENT);

  const f = {};
  if (quotaWins) {
    f.label = 'QUOTA';
    f.ring = round2(clamp01(quota.window.used_percent / 100));
    f.center = `${Math.round(quota.window.used_percent)}%`;
  } else if (ctxKnown) {
    f.label = 'CTX';
    f.ring = round2(clamp01(metrics.ctxFill));
    f.center = `${Math.round(clamp01(metrics.ctxFill) * 100)}%`;
  } else if (elapsed !== null) {
    f.label = 'TIME';
    f.ring = 0.15;
    f.center = formatElapsed(elapsed);
  } else {
    f.label = 'TIME';
    f.ring = 0.15;
    f.center = '--';
  }
  f.sub = elapsed !== null ? `${formatElapsed(elapsed)} elapsed` : '';
  if (Number.isFinite(metrics.tps) && metrics.tps > 0) f.tps = Math.round(metrics.tps * 10) / 10;
  return f;
}

/**
 * The frame one hook event produces.
 * @param {string} eventName  hook_event_name, straight from the payload
 * @param {object} metrics    from readMetrics(), or null
 * @param {number} nowMs
 * @returns {object|null} null when the event is not one we render
 */
function frameForEvent(eventName, metrics, nowMs) {
  const state = EVENT_STATE[eventName];
  if (!state) return null;
  const frame = Object.assign({ state }, metricFields(metrics, nowMs));
  if (state === 'waiting') {
    // No tool name, no command text: the screen says only that you are the
    // one holding things up, and your terminal says what for.
    frame.sub = 'your turn';
  }
  return frame;
}

/** Serialize a frame, clamping every field to what the firmware can hold. */
function frameToLine(frame) {
  const f = {};
  if (frame.state && DEVICE_STATES.has(frame.state)) f.state = frame.state;
  if (Number.isFinite(frame.ring)) f.ring = round2(clamp01(frame.ring));
  if (typeof frame.center === 'string') f.center = frame.center.slice(0, CENTER_MAX);
  if (typeof frame.label === 'string') f.label = frame.label.slice(0, LABEL_MAX);
  if (typeof frame.sub === 'string') f.sub = frame.sub.slice(0, SUB_MAX);
  if (Number.isFinite(frame.tps) && frame.tps > 0) f.tps = Math.round(frame.tps * 10) / 10;
  let line = JSON.stringify(f) + '\n';
  if (Buffer.byteLength(line) > MAX_LINE_BYTES) {
    delete f.sub;
    line = JSON.stringify(f) + '\n';
  }
  return line;
}

// --------------------------------------------------------------------------
// 5. Metrics from the session file
//
// Hooks carry no numbers. Context fill, token counts, rate limits and turn
// elapsed time only exist in the rollout file Codex is already writing, so
// this reads the tail of that file and nothing else. It is a fallback for
// numbers, never for state.
// --------------------------------------------------------------------------

/** $CODEX_HOME, honouring the same override Codex itself honours. */
function codexHome(env) {
  const e = env || process.env;
  if (e.CODEX_HOME && e.CODEX_HOME.trim()) return e.CODEX_HOME.trim();
  return path.join(os.homedir(), '.codex');
}

/**
 * Codex's own context-fill formula (protocol.rs, percent_of_context_window_
 * remaining), inverted to a 0..1 fill so the ring agrees with what the Codex
 * TUI shows. Not used/window: Codex subtracts a fixed baseline from both sides.
 */
function contextFill(contextTokens, windowSize) {
  if (!Number.isFinite(contextTokens) || !Number.isFinite(windowSize)) return null;
  if (windowSize <= BASELINE_TOKENS) return null;
  const effective = windowSize - BASELINE_TOKENS;
  const usedEff = Math.max(contextTokens - BASELINE_TOKENS, 0);
  const remaining = Math.min(Math.max((effective - usedEff) / effective, 0), 1);
  return (100 - Math.round(remaining * 100)) / 100;
}

/** Parse one rollout line. Returns null for blank, partial or non-object lines. */
function parseLine(line) {
  const t = line.trim();
  if (!t || t[0] !== '{') return null;
  try {
    const obj = JSON.parse(t);
    return obj && typeof obj === 'object' ? obj : null;
  } catch {
    return null;
  }
}

/** Read the last complete lines of a file. Never throws. */
function readTailLines(file, maxBytes) {
  let fd;
  try {
    fd = fs.openSync(file, 'r');
  } catch {
    return [];
  }
  try {
    const size = fs.fstatSync(fd).size;
    const start = Math.max(0, size - (maxBytes || TAIL_BYTES));
    const len = size - start;
    if (len <= 0) return [];
    const buf = Buffer.alloc(len);
    const read = fs.readSync(fd, buf, 0, len, start);
    let text = buf.subarray(0, read).toString('utf8');
    // Clipped mid-file: the first line is probably a fragment, drop it.
    if (start > 0) {
      const nl = text.indexOf('\n');
      text = nl === -1 ? '' : text.slice(nl + 1);
    }
    const parts = text.split('\n');
    parts.pop(); // trailing '' after the last newline, or a half-written line
    return parts;
  } catch {
    return [];
  } finally {
    try {
      fs.closeSync(fd);
    } catch {
      /* ignore */
    }
  }
}

/** Newest rollout-*.jsonl under a sessions directory, or null. Never throws. */
function newestRolloutFile(dir, depth) {
  let best = null;
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return null;
  }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      if ((depth || 0) >= 6) continue;
      const inner = newestRolloutFile(full, (depth || 0) + 1);
      if (inner && (!best || inner.mtimeMs > best.mtimeMs)) best = inner;
    } else if (e.isFile() && e.name.startsWith('rollout-') && e.name.endsWith('.jsonl')) {
      try {
        const st = fs.statSync(full);
        if (!best || st.mtimeMs > best.mtimeMs) best = { path: full, mtimeMs: st.mtimeMs };
      } catch {
        /* vanished between readdir and stat */
      }
    }
  }
  return best;
}

/** True when a raw line is one of the record types we take numbers from. */
function isMetricLine(line) {
  for (const hint of METRIC_HINTS) {
    if (line.includes(hint)) return true;
  }
  return false;
}

/**
 * Read numbers out of a rollout file.
 *
 * Everything returned is a number or a timestamp. No field of the returned
 * object can hold text from a prompt, a reply, a command or a file.
 *
 * @param {string} file
 * @param {number} nowMs
 * @param {string[]} [lines] pre-read lines, for tests
 */
function readMetrics(file, nowMs, lines) {
  const raw = lines || (file ? readTailLines(file) : []);
  const m = {
    contextTokens: null,
    contextWindow: null,
    tokensIn: null,
    tokensOut: null,
    totalTokens: null,
    rateLimits: null,
    turnStartedAtMs: null,
    turnOpen: false,
    lastTurnDurationSec: null,
    tps: null,
    ctxFill: null,
    elapsedSec: null
  };
  let lastUsageAtMs = null;

  for (const line of raw) {
    if (!isMetricLine(line)) continue; // never parse a line that carries text
    const obj = parseLine(line);
    if (!obj || typeof obj.type !== 'string') continue;
    const p = obj.payload;
    if (!p || typeof p !== 'object') continue;
    const stamp = typeof obj.timestamp === 'string' ? Date.parse(obj.timestamp) : NaN;
    const at = Number.isFinite(stamp) ? stamp : nowMs;

    if (obj.type === 'token_usage_record') {
      applyUsage(m, p, at, () => lastUsageAtMs, (v) => (lastUsageAtMs = v));
    } else if (obj.type === 'compacted' && p.latest_token_usage_record) {
      applyUsage(m, p.latest_token_usage_record, at, () => lastUsageAtMs, (v) => (lastUsageAtMs = v));
    } else if (obj.type === 'event_msg') {
      applyEvent(m, p, at);
    }
  }

  m.ctxFill = contextFill(m.contextTokens, m.contextWindow);
  if (m.turnOpen && Number.isFinite(m.turnStartedAtMs)) {
    m.elapsedSec = Math.max(0, (nowMs - m.turnStartedAtMs) / 1000);
  } else if (Number.isFinite(m.lastTurnDurationSec)) {
    m.elapsedSec = m.lastTurnDurationSec;
  }
  return m;
}

/**
 * `usage` is the usage of one completed response, i.e. the live context size.
 * `thread_token_usage` is the accumulated session total and grows without
 * bound, so driving the ring from it would pin it at a static 100%.
 */
function applyUsage(m, p, at, getLast, setLast) {
  const usage = p.usage;
  const cumulative = p.thread_token_usage || p.turn_token_usage || usage;
  if (cumulative && typeof cumulative === 'object') {
    if (Number.isFinite(cumulative.input_tokens)) m.tokensIn = cumulative.input_tokens;
    if (Number.isFinite(cumulative.output_tokens)) m.tokensOut = cumulative.output_tokens;
    if (Number.isFinite(cumulative.total_tokens)) m.totalTokens = cumulative.total_tokens;
  }
  if (usage && typeof usage === 'object') {
    if (Number.isFinite(usage.total_tokens)) m.contextTokens = usage.total_tokens;
    if (Number.isFinite(usage.output_tokens)) {
      const last = getLast();
      if (last !== null && at > last) {
        const dt = (at - last) / 1000;
        if (dt > 0.05) m.tps = usage.output_tokens / dt;
      }
      setLast(at);
    }
  }
}

function applyEvent(m, p, at) {
  const kind = p.type;
  if (kind === 'task_started' || kind === 'turn_started') {
    m.turnOpen = true;
    m.turnStartedAtMs = Number.isFinite(p.started_at) ? p.started_at * 1000 : at;
    m.tps = null;
    if (Number.isFinite(p.model_context_window)) m.contextWindow = p.model_context_window;
    return;
  }
  if (kind === 'task_complete' || kind === 'turn_complete' || kind === 'turn_aborted') {
    m.turnOpen = false;
    if (Number.isFinite(p.duration_ms)) m.lastTurnDurationSec = p.duration_ms / 1000;
    else if (Number.isFinite(p.started_at) && Number.isFinite(p.completed_at)) {
      m.lastTurnDurationSec = p.completed_at - p.started_at;
    }
    return;
  }
  if (kind === 'token_count') {
    const info = p.info;
    if (info && typeof info === 'object') {
      const total = info.total_token_usage;
      if (total && typeof total === 'object') {
        if (Number.isFinite(total.input_tokens)) m.tokensIn = total.input_tokens;
        if (Number.isFinite(total.output_tokens)) m.tokensOut = total.output_tokens;
        if (Number.isFinite(total.total_tokens)) m.totalTokens = total.total_tokens;
      }
      // The ring's number: the latest active context size, not the session total.
      const last = info.last_token_usage;
      if (last && typeof last === 'object' && Number.isFinite(last.total_tokens)) {
        m.contextTokens = last.total_tokens;
      }
      if (Number.isFinite(info.model_context_window)) m.contextWindow = info.model_context_window;
    }
    if (p.rate_limits && typeof p.rate_limits === 'object') {
      m.rateLimits = {
        primary: numbersOnly(p.rate_limits.primary),
        secondary: numbersOnly(p.rate_limits.secondary),
        observedAtMs: at
      };
    }
  }
}

/** Copy only the numeric fields of a rate-limit window. */
function numbersOnly(w) {
  if (!w || typeof w !== 'object') return null;
  const out = {};
  for (const k of ['used_percent', 'window_minutes', 'resets_at', 'resets_in_seconds']) {
    if (Number.isFinite(w[k])) out[k] = w[k];
  }
  return out;
}

/**
 * Metrics for a hook invocation. `transcript_path` from the payload is the
 * rollout file for that exact session; if it is missing or unreadable we fall
 * back to the newest rollout under $CODEX_HOME, and if that fails too the
 * frame simply goes out without numbers.
 */
function metricsForHook(transcriptPath, nowMs, env) {
  try {
    if (transcriptPath && typeof transcriptPath === 'string' && fs.existsSync(transcriptPath)) {
      return readMetrics(transcriptPath, nowMs);
    }
    const newest = newestRolloutFile(path.join(codexHome(env), 'sessions'));
    return newest ? readMetrics(newest.path, nowMs) : null;
  } catch {
    return null;
  }
}

// --------------------------------------------------------------------------
// 6. The hook entry point
//
// Runs as: codex-companion hook
// Codex pipes one JSON object on stdin. We read two fields out of it, write
// one line to the board, and exit 0. Always 0: no device, a busy port, a
// malformed payload and an outright bug all end the same way, silently.
//
// Nothing is ever printed on stdout. For PermissionRequest, empty stdout with
// exit 0 is exactly Codex's "decline to decide": the approval prompt is shown
// to the human as normal. This program must never approve or deny anything.
// --------------------------------------------------------------------------

/** Read stdin to EOF. An unread pipe can wedge the caller, so always drain it. */
function readStdin() {
  if (process.stdin.isTTY) return '';
  try {
    return fs.readFileSync(0, 'utf8');
  } catch {
    return '';
  }
}

/**
 * Pull the only two fields we look at out of a hook payload.
 * @returns {{eventName:string|null, transcriptPath:string|null}}
 */
function readHookPayload(text) {
  let obj = null;
  try {
    obj = JSON.parse(text);
  } catch {
    return { eventName: null, transcriptPath: null };
  }
  if (!obj || typeof obj !== 'object') return { eventName: null, transcriptPath: null };
  return {
    eventName: typeof obj.hook_event_name === 'string' ? obj.hook_event_name : null,
    transcriptPath: typeof obj.transcript_path === 'string' ? obj.transcript_path : null
  };
}

function cmdHook(opts) {
  const payload = readHookPayload(readStdin());
  if (!payload.eventName) return 0;
  const now = Date.now();
  const metrics = opts.noMetrics ? null : metricsForHook(payload.transcriptPath, now, opts.env);
  const frame = frameForEvent(payload.eventName, metrics, now);
  if (!frame) return 0;
  const link = openLink(opts);
  if (!link) return 0;
  try {
    link.writeFrame(frame, 200);
  } finally {
    link.close();
  }
  return 0;
}

// --------------------------------------------------------------------------
// 7. hooks.json: merge in, merge out
//
// Shape (codex-rs/config/src/hook_config.rs):
//   {"description": "...",
//    "hooks": {"<EventName>": [{"matcher": "...", "hooks": [handler, ...]}]}}
// We add our own matcher group per event rather than joining one of yours, so
// removing ours can never disturb yours.
// --------------------------------------------------------------------------

/** Absolute path of this file, as the hook command must name it. */
function selfPath() {
  return __filename;
}

/** The exact command string we register. */
function hookCommand(nodeBin, script) {
  return `${JSON.stringify(nodeBin || process.execPath)} ${JSON.stringify(script || selfPath())} hook`;
}

/** Our handler object for one event. */
function hookHandler(command, eventName) {
  return {
    type: 'command',
    command,
    timeout: eventName === 'SessionEnd' ? SESSION_END_TIMEOUT_SEC : ASYNC_TIMEOUT_SEC,
    // async:true also means Codex will not let this hook's output decide an
    // approval, whatever it prints. SessionEnd is always run synchronously by
    // Codex, so asking for async there would only produce a warning.
    async: eventName !== 'SessionEnd'
  };
}

/**
 * True for a handler this program installed. Matched on the shape of the
 * command, not on an exact string, so a moved node binary still uninstalls.
 */
function isOurHandler(h) {
  if (!h || h.type !== 'command' || typeof h.command !== 'string') return false;
  return /codex-companion(\.js)?["']?\s+hook\s*$/.test(h.command.trim());
}

/**
 * Merge our handlers into an existing hooks.json object.
 * @returns {{next:object, added:string[]}}
 */
function mergeHooks(existing, command, events) {
  const next = existing && typeof existing === 'object' ? deepCopy(existing) : {};
  if (!next.hooks || typeof next.hooks !== 'object' || Array.isArray(next.hooks)) next.hooks = {};
  const added = [];
  for (const eventName of events || INSTALLED_EVENTS) {
    if (!Array.isArray(next.hooks[eventName])) next.hooks[eventName] = [];
    const groups = next.hooks[eventName];
    const already = groups.some(
      (g) => g && Array.isArray(g.hooks) && g.hooks.some((h) => isOurHandler(h))
    );
    if (already) continue;
    groups.push({ hooks: [hookHandler(command, eventName)] });
    added.push(eventName);
  }
  return { next, added };
}

/**
 * Remove exactly what mergeHooks added, and nothing else.
 * @returns {{next:object, removed:number, empty:boolean}}
 */
function unmergeHooks(existing) {
  const next = existing && typeof existing === 'object' ? deepCopy(existing) : {};
  let removed = 0;
  const hooks = next.hooks && typeof next.hooks === 'object' ? next.hooks : {};
  for (const eventName of Object.keys(hooks)) {
    const groups = Array.isArray(hooks[eventName]) ? hooks[eventName] : [];
    const kept = [];
    for (const g of groups) {
      if (!g || !Array.isArray(g.hooks)) {
        kept.push(g);
        continue;
      }
      const before = g.hooks.length;
      const handlers = g.hooks.filter((h) => !isOurHandler(h));
      removed += before - handlers.length;
      // A group we emptied was ours; a group that was already empty was yours.
      if (handlers.length === 0 && before > 0) continue;
      kept.push(Object.assign({}, g, { hooks: handlers }));
    }
    if (kept.length === 0) delete hooks[eventName];
    else hooks[eventName] = kept;
  }
  if (Object.keys(hooks).length === 0) delete next.hooks;
  else next.hooks = hooks;
  const empty = Object.keys(next).length === 0;
  return { next, removed, empty };
}

function deepCopy(o) {
  return JSON.parse(JSON.stringify(o));
}

function readJsonFile(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return null;
  }
}

/** Which of our events a hooks.json object currently registers. */
function registeredEvents(hooksFile) {
  const out = [];
  const hooks = hooksFile && hooksFile.hooks ? hooksFile.hooks : {};
  for (const eventName of ALL_HOOK_EVENTS) {
    const groups = Array.isArray(hooks[eventName]) ? hooks[eventName] : [];
    if (groups.some((g) => g && Array.isArray(g.hooks) && g.hooks.some(isOurHandler))) {
      out.push(eventName);
    }
  }
  return out;
}

function cmdInstallHook(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  const file = path.join(home, 'hooks.json');
  const existing = readJsonFile(file);
  if (fs.existsSync(file) && existing === null) {
    say(`${file} exists but is not valid JSON. Refusing to touch it.`);
    return 1;
  }
  const command = hookCommand();
  const { next, added } = mergeHooks(existing, command);
  const text = JSON.stringify(next, null, 2) + '\n';

  say(`About to write ${file}${existing ? ' (merging into the file already there)' : ''}:`);
  say('');
  say(text.trimEnd());
  say('');
  if (opts.dryRun) {
    say('--dry-run: nothing was written.');
    return 0;
  }
  fs.mkdirSync(home, { recursive: true });
  fs.writeFileSync(file, text);
  say(added.length ? `Registered for: ${added.join(', ')}` : 'Already registered; nothing changed.');
  say('');
  say('Codex will not run a new hook until you approve it: start Codex and');
  say('accept the hook review prompt. `codex-companion doctor` shows the state.');
  say('Undo all of this with `codex-companion uninstall-hook`.');
  return 0;
}

function cmdUninstallHook(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  const file = path.join(home, 'hooks.json');
  if (!fs.existsSync(file)) {
    say(`${file} does not exist. Nothing to remove.`);
    return 0;
  }
  const existing = readJsonFile(file);
  if (existing === null) {
    say(`${file} is not valid JSON. Refusing to touch it.`);
    return 1;
  }
  const { next, removed, empty } = unmergeHooks(existing);
  if (removed === 0) {
    say(`No codex-companion hooks found in ${file}. Nothing removed.`);
    return 0;
  }
  if (empty) {
    say(`Removing ${removed} handler(s); that leaves ${file} with nothing in it, so:`);
    say(`  rm ${file}`);
    if (!opts.dryRun) fs.unlinkSync(file);
  } else {
    const text = JSON.stringify(next, null, 2) + '\n';
    say(`Removing ${removed} handler(s). ${file} becomes:`);
    say('');
    say(text.trimEnd());
    if (!opts.dryRun) fs.writeFileSync(file, text);
  }
  if (opts.dryRun) say('\n--dry-run: nothing was written.');
  return 0;
}

// --------------------------------------------------------------------------
// 8. doctor
// --------------------------------------------------------------------------

/**
 * Shallow scan of config.toml for the two settings that stop any permission
 * event from ever firing. Not a TOML parser: it reports the section a match
 * was found in and leaves the judgement to the reader.
 */
function scanConfigToml(text) {
  const found = [];
  let section = '(top level)';
  for (const raw of String(text || '').split('\n')) {
    const line = raw.trim();
    if (line.startsWith('#')) continue;
    const sec = line.match(/^\[([^\]]+)\]/);
    if (sec) {
      section = sec[1];
      continue;
    }
    const kv = line.match(/^(approval_policy|sandbox_mode|approvals_reviewer)\s*=\s*"([^"]*)"/);
    if (kv) found.push({ section, key: kv[1], value: kv[2] });
  }
  return found;
}

/** True for a setting that makes the amber "waiting" light unreachable. */
function silencesApprovals(entry) {
  return (
    (entry.key === 'approval_policy' && entry.value === 'never') ||
    (entry.key === 'sandbox_mode' && entry.value === 'danger-full-access') ||
    (entry.key === 'approvals_reviewer' && entry.value === 'auto_review')
  );
}

function cmdDoctor(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  say(`node            ${process.version} on ${process.platform}`);
  say(`codex-companion ${selfPath()}`);

  // --- the board
  const pick = choosePort(opts);
  say('');
  say(`serial ports    ${pick.ports.length ? pick.ports.join(', ') : '(none)'}`);
  say(`device          ${pick.chosen || 'NOT FOUND'}`);
  say(`                ${pick.reason}`);
  if (pick.chosen) {
    // The one place this program reads from the board: open for reading and
    // see whether it greets us. The board's transmit path runs one message
    // behind, so we nudge it with a keepalive first.
    let link = null;
    try {
      link = new Link(pick.chosen, { read: true }).open();
      link.writeLine('{}\n', 200);
      const started = Date.now();
      let seen = '';
      while (Date.now() - started < 600) seen += link.readAvailable();
      link.writeLine('{}\n', 200);
      const started2 = Date.now();
      while (Date.now() - started2 < 400) seen += link.readAvailable();
      const hello = /hello\s+tdisplay-s3/i.test(seen);
      const ok = /\bok\b/.test(seen);
      say(
        `handshake       ${hello ? 'greeting seen' : ok ? 'acked our keepalive' : 'no reply (the board only greets once, right after boot)'}`
      );
      const name = seen.match(/name="([^"]*)"/);
      if (name) say(`board name      ${name[1]}`);
    } catch (e) {
      say(`handshake       could not open the port: ${e.code || e.message}`);
    } finally {
      if (link) link.close();
    }
  }

  // --- codex
  say('');
  let codexVersion = '(codex not on PATH)';
  try {
    codexVersion = execFileSync('codex', ['--version'], { encoding: 'utf8', timeout: 10000 }).trim();
  } catch {
    /* leave the default */
  }
  say(`codex           ${codexVersion}`);
  say(`CODEX_HOME      ${home}`);

  const hooksPath = path.join(home, 'hooks.json');
  const hooksFile = readJsonFile(hooksPath);
  if (!fs.existsSync(hooksPath)) {
    say(`hooks.json      not present. Run: codex-companion install-hook`);
  } else if (hooksFile === null) {
    say(`hooks.json      ${hooksPath} EXISTS BUT IS NOT VALID JSON`);
  } else {
    const events = registeredEvents(hooksFile);
    say(`hooks.json      ${hooksPath}`);
    say(
      `our hook        ${events.length ? `registered for ${events.join(', ')}` : 'NOT registered. Run: codex-companion install-hook'}`
    );
  }

  // --- hook trust. Codex will not run a hook it has not been told to trust.
  const configPath = path.join(home, 'config.toml');
  let configText = '';
  try {
    configText = fs.readFileSync(configPath, 'utf8');
  } catch {
    /* no config is a perfectly normal state */
  }
  const trusted = /^\s*\[hooks\.state[.\]]/m.test(configText) || /trusted_hash/.test(configText);
  say(
    `hook trust      ${trusted ? 'config.toml has hook trust state; check the hook review in Codex if it never fires' : 'no trust state recorded yet: start Codex once and approve the hook review'}`
  );

  // --- the number one support question
  say('');
  const settings = scanConfigToml(configText);
  const silencing = settings.filter(silencesApprovals);
  if (silencing.length === 0) {
    say('approvals       nothing in config.toml suppresses approval prompts');
    if (settings.length) {
      for (const s of settings) say(`                [${s.section}] ${s.key} = "${s.value}"`);
    }
  } else {
    say('approvals       WARNING: this config can never raise an approval prompt,');
    say('                so the amber "waiting" light will never appear and the');
    say('                device will look broken when it is working correctly:');
    for (const s of silencing) say(`                  [${s.section}] ${s.key} = "${s.value}"`);
  }

  // --- session metrics
  say('');
  const newest = newestRolloutFile(path.join(home, 'sessions'));
  if (!newest) {
    say('session         none found yet (run codex once)');
  } else {
    const m = readMetrics(newest.path, Date.now());
    say(`session         ${newest.path}`);
    say(
      `metrics         ctx ${m.ctxFill === null ? '(unknown)' : `${Math.round(m.ctxFill * 100)}%`}` +
        `, window ${m.contextWindow === null ? '(unknown)' : m.contextWindow}` +
        `, turn ${m.turnOpen ? 'open' : 'closed'}`
    );
    say(`frame          ${frameToLine(metricFields(m, Date.now())).trim()}`);
  }
  return 0;
}

// --------------------------------------------------------------------------
// 9. demo and run
// --------------------------------------------------------------------------

const DEMO_FRAMES = [
  { state: 'idle', ring: 0.08, center: '8%', label: 'CTX', sub: 'idle' },
  { state: 'busy', ring: 0.31, center: '31%', label: 'CTX', sub: '0:42 elapsed', tps: 12.5 },
  { state: 'busy', ring: 0.62, center: '62%', label: 'CTX', sub: '3:07 elapsed', tps: 31.2 },
  { state: 'waiting', ring: 0.62, center: '62%', label: 'CTX', sub: 'your turn' },
  { state: 'done', ring: 0.64, center: '64%', label: 'CTX', sub: '3:21 elapsed' },
  { state: 'idle', ring: 0.64, center: '64%', label: 'CTX', sub: '' },
  { state: 'idle', ring: 0.91, center: '91%', label: 'QUOTA', sub: 'weekly window' },
  { state: 'sleep', ring: 0, center: '--', label: 'CODEX', sub: '' }
];

async function cmdDemo(opts) {
  const link = opts.dryRun ? null : openLink(opts);
  if (!opts.dryRun && !link) {
    say(`no device: ${choosePort(opts).reason}`);
    return 1;
  }
  let stopped = false;
  process.on('SIGINT', () => {
    stopped = true;
  });
  do {
    for (const frame of DEMO_FRAMES) {
      if (stopped) break;
      const line = frameToLine(frame);
      say(line.trimEnd());
      if (link) link.writeFrame(frame, 200);
      await sleep(1200);
    }
  } while (opts.loop && !stopped);
  if (link) link.close();
  return 0;
}

/**
 * Foreground metrics stream. Installs nothing; Ctrl-C ends it completely.
 *
 * Sends only the metric fields, never `state`: the board keeps whatever state
 * the hook last set, so running this alongside the hook composes instead of
 * fighting. On its own it gives you a live context ring with no hook at all.
 */
async function cmdRun(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  const link = opts.dryRun ? null : openLink(opts);
  if (!opts.dryRun && !link) {
    say(`no device: ${choosePort(opts).reason}`);
    return 1;
  }
  say(`watching ${path.join(home, 'sessions')}`);
  say(link ? `writing to ${link.path}` : 'dry run: not opening any port');
  say('Ctrl-C to stop. Nothing was installed.');

  let stopped = false;
  const stop = () => {
    stopped = true;
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);

  let lastLine = null;
  let lastSentAt = 0;
  while (!stopped) {
    const now = Date.now();
    const newest = newestRolloutFile(path.join(home, 'sessions'));
    const metrics = newest ? readMetrics(newest.path, now) : null;
    const line = frameToLine(metricFields(metrics, now));
    // Send on change, and at least every 2s so the board's 30s staleness
    // dimmer never trips while we are running.
    if (line !== lastLine || now - lastSentAt >= 2000) {
      if (link) link.writeFrame(metricFields(metrics, now), 200);
      else say(line.trimEnd());
      lastLine = line;
      lastSentAt = now;
    }
    await sleep(1000);
  }
  if (link) link.close();
  say('stopped.');
  return 0;
}

function sleep(ms) {
  return new Promise((resolve) => {
    const t = setTimeout(resolve, ms);
    if (t.unref) t.unref();
  });
}

// --------------------------------------------------------------------------
// 10. CLI
// --------------------------------------------------------------------------

const USAGE = `codex-companion - desk companion for the Codex CLI

  codex-companion                 stream context metrics in the foreground
  codex-companion run             the same thing, named
  codex-companion hook            hook entry point: reads one payload on stdin
  codex-companion install-hook    print, then merge our handlers into hooks.json
  codex-companion uninstall-hook  remove exactly those handlers again
  codex-companion doctor          device, codex, hook registration, config traps
  codex-companion demo            drive every state, no Codex involved
  codex-companion help            this text

Options:
  --port /dev/cu.usbmodemXXX   use this port instead of auto-detecting
  --codex-home /path           look here instead of $CODEX_HOME or ~/.codex
  --dry-run                    print what would happen, change nothing
  --loop                       (demo) repeat until Ctrl-C
  --no-metrics                 (hook) send state only, do not read the rollout

Nothing here installs a background service. There is no autostart. The hook
never approves or denies anything; approval prompts stay in your terminal.
`;

function parseArgs(argv) {
  const opts = { env: process.env };
  const rest = [];
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--port') opts.port = argv[++i];
    else if (a === '--codex-home') opts.codexHome = argv[++i];
    else if (a === '--dry-run') opts.dryRun = true;
    else if (a === '--loop') opts.loop = true;
    else if (a === '--no-metrics') opts.noMetrics = true;
    else if (a === '-h' || a === '--help') opts.help = true;
    else rest.push(a);
  }
  opts.command = rest[0] || 'run';
  return opts;
}

/** Everything user-facing goes to stdout here, except inside `hook`. */
function say(s) {
  process.stdout.write(`${s}\n`);
}

async function main(argv) {
  const opts = parseArgs(argv || []);
  if (opts.help || opts.command === 'help') {
    process.stdout.write(USAGE);
    return 0;
  }
  switch (opts.command) {
    case 'hook':
      return cmdHook(opts);
    case 'install-hook':
      return cmdInstallHook(opts);
    case 'uninstall-hook':
      return cmdUninstallHook(opts);
    case 'doctor':
      return cmdDoctor(opts);
    case 'demo':
      return cmdDemo(opts);
    case 'run':
      return cmdRun(opts);
    default:
      process.stdout.write(USAGE);
      return 1;
  }
}

if (require.main === module) {
  const isHook = process.argv.includes('hook');
  Promise.resolve()
    .then(() => main(process.argv.slice(2)))
    .then((code) => {
      process.exitCode = isHook ? 0 : code || 0;
    })
    .catch((err) => {
      // A hook must never fail its caller, whatever went wrong in here.
      if (isHook) {
        process.exitCode = 0;
        return;
      }
      process.stderr.write(`codex-companion: ${(err && err.message) || err}\n`);
      process.exitCode = 1;
    });
}

module.exports = {
  // discovery
  listCalloutPorts,
  parseIoreg,
  choosePort,
  Link,
  // frames
  metricFields,
  frameForEvent,
  frameToLine,
  formatElapsed,
  pickQuotaWindow,
  // metrics
  codexHome,
  contextFill,
  parseLine,
  readTailLines,
  newestRolloutFile,
  isMetricLine,
  readMetrics,
  // hook
  readHookPayload,
  cmdHook,
  // hooks.json
  hookCommand,
  hookHandler,
  isOurHandler,
  mergeHooks,
  unmergeHooks,
  registeredEvents,
  // doctor
  scanConfigToml,
  silencesApprovals,
  // cli
  parseArgs,
  main,
  // constants worth asserting on
  ESP_VENDOR_ID,
  ESP_PRODUCT_ID,
  EVENT_STATE,
  ALL_HOOK_EVENTS,
  INSTALLED_EVENTS,
  MAX_LINE_BYTES,
  BASELINE_TOKENS
};
