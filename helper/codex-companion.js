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
 * THE TWO PATHS IT MAY WRITE TO
 *   1. the serial port, which is the whole point
 *   2. <CODEX_HOME>/hooks.json, and only from install-hook / uninstall-hook
 *   There is no third. No temp file, no cache, no log, no lock file, no state
 *   between runs. `test/paths.test.js` asserts this against the source.
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

// --- the fields the firmware grew after this program was first written ------
//
// Every one of them is mirrored here with the firmware's own limit, and the
// rule that decides what a bad value costs. There are exactly two rules and
// the firmware is the reason for both:
//
//   ENUM fields (state, face, reset, firstrun, play, stats, focus) make the
//   WHOLE line malformed on the device when the value is not in the list, so a
//   value we cannot vouch for is dropped from the frame here rather than sent.
//   Sending it would silently discard the other fields riding with it.
//
//   VALUE fields (ring, tps, say, saysecs, dnd, name, time, tokens, focusmins)
//   are clamped or sanitised by the device and can never break a line, so the
//   worst a bad one costs is a field that does nothing.
//
// Symbols and numbers below are firmware/src/main.cpp's, named the same way:
// OWNER_NAME_MAX, SAY_MAX, SAY_MAX_S, SAY_DEFAULT_S, CLOCK_MIN_EPOCH,
// FOCUS_MIN_MIN, FOCUS_MAX_MIN, and the registry in firmware/src/Faces.cpp.

/** `name`: OWNER_NAME_MAX. ASCII only; the panel fonts have no other glyphs. */
const NAME_MAX = 24;

/** `say`: SAY_MAX characters, SAY_DEFAULT_S seconds, SAY_MAX_S the ceiling. */
const SAY_MAX = 48;
const SAY_DEFAULT_SECS = 30;
const SAY_MAX_SECS = 3600;

/** `focusmins`: FOCUS_MIN_MIN..FOCUS_MAX_MIN, clamped rather than rejected. */
const FOCUS_MIN_MINUTES = 1;
const FOCUS_MAX_MINUTES = 180;

/** `time`: CLOCK_MIN_EPOCH. Below 2020-01-01 the device ignores the field. */
const CLOCK_MIN_EPOCH = 1577836800;

/** The faces `firmware/src/Faces.cpp` registers, matched case insensitively. */
const FACE_NAMES = ['rounded', 'bear', 'arc'];

/** The rest of the string enums, each exactly as the firmware spells them. */
const PLAY_VERBS = ['on', 'off', 'hop'];
const STATS_VERBS = ['on', 'off'];
const FOCUS_VERBS = ['show', 'hide', 'start', 'pause', 'reset'];
const RESET_VERBS = ['factory', 'firstrun'];
const FIRSTRUN_VERBS = ['play'];

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
/*
 * Null-prototyped on purpose. A plain object literal answers EVENT_STATE
 * ['__proto__'] with Object.prototype and EVENT_STATE['constructor'] with a
 * function, both truthy, so a payload naming either as its hook_event_name
 * would produce a frame for an event that does not exist. The event name comes
 * straight off stdin, so it is exactly the kind of value that must not be able
 * to reach an inherited property.
 */
const EVENT_STATE = Object.assign(Object.create(null), {
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
});

/** The only safe way to read EVENT_STATE with a string that came off stdin. */
function stateForEvent(eventName) {
  if (typeof eventName !== 'string') return null;
  const s = EVENT_STATE[eventName];
  return typeof s === 'string' && DEVICE_STATES.has(s) ? s : null;
}

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

/**
 * Everything below this line is macOS-only, and nothing else in this file said
 * so: the port list is /dev/cu.* names, the identity check shells out to
 * ioreg, and the tty is configured with BSD `stty -f`. On Linux the same board
 * enumerates as /dev/ttyACM0 and works perfectly, so the honest answer there
 * is "this program cannot find it, name it yourself", not the charge-only
 * cable remedy that every not-found path would otherwise print.
 */
const IS_MAC = process.platform === 'darwin';
const PLATFORM_NOTE =
  'This program finds a board by looking for macOS callout ports ' +
  `(/dev/cu.usbmodem*) and this is ${process.platform}, so it will never ` +
  'find one. The board itself is fine on any OS: name its port and detection ' +
  'is skipped entirely, e.g. --port /dev/ttyACM0.';

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
      maxBuffer: 32 * 1024 * 1024,
      stdio: ['ignore', 'pipe', 'ignore']
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

  // Answered before anything is scanned, deliberately. `ioreg` walks the whole
  // USB tree in a subprocess, and this function runs on every single Codex
  // hook event; spending that to build a list we are about to throw away was
  // pure waste on the one path that has to stay out of a person's way.
  if (o.port) {
    return {
      chosen: o.port,
      matched: [o.port],
      ports: o.ports || listCalloutPorts(),
      reason: `--port ${o.port} (identity not checked, you asked for this one)`
    };
  }

  const ports = o.ports || listCalloutPorts();
  const usb = parseIoreg(o.ioreg === undefined ? readIoreg() : o.ioreg);

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

/**
 * Sleep without yielding to the event loop, with no dependency and no busy
 * wait. Atomics.wait blocks the thread on a futex; a spin loop on Date.now()
 * would burn a core for the same wall clock. Used only where the surrounding
 * code is already synchronous: the EAGAIN retry, and doctor's read window.
 */
const SLEEP_LOCK = new Int32Array(new SharedArrayBuffer(4));
function sleepSync(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return;
  try {
    Atomics.wait(SLEEP_LOCK, 0, 0, ms);
  } catch {
    /* wait is disallowed on some threads; falling through just means no nap */
  }
}

class Link {
  /**
   * @param {string} portPath
   * @param {{read?:boolean}} [opts] read:true also opens for reading, which
   *   only `doctor` and `selftest` do, to hear the board answer.
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
      execFileSync('/bin/stty', ['-f', this.path, BAUD, 'raw', '-echo'], {
        timeout: 5000,
        // Never let a helper tool's noise reach our caller's stderr: a hook
        // that chatters on stderr is a hook that looks like it failed.
        stdio: ['ignore', 'ignore', 'ignore']
      });
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
      // Checked every pass, not only after an exception. A writeSync that
      // legally returns 0 throws nothing, so a deadline tested solely in the
      // catch block would leave this loop spinning on a stalled tty forever.
      if (Date.now() >= until) return false;
      let wrote = 0;
      try {
        wrote = fs.writeSync(this.fd, buf, off, buf.length - off);
      } catch (e) {
        // EAGAIN: the kernel's tty buffer is full. Give the cable a moment,
        // but never past the deadline; a desk toy may not stall a turn. The
        // nap is what stops "give it a moment" meaning "spin a core for 200ms".
        if (e.code === 'EAGAIN' || e.code === 'EWOULDBLOCK') {
          sleepSync(2);
          continue;
        }
        return false;
      }
      if (wrote > 0) off += wrote;
      else sleepSync(2);
    }
    return true;
  }

  /**
   * Non-blocking read of whatever the board has said. Only doctor and selftest
   * use this. Returns '' both when there is nothing to read and when the port
   * is write-only, which are the same thing from a caller's point of view.
   */
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

  /**
   * Read for `ms`, napping between polls rather than spinning.
   *
   * The board's transmit path runs one message behind (docs/protocol.md 3.3),
   * so a caller that wants to hear about line N writes a bare `{}` keepalive
   * afterwards to flush it. `nudge` does that once per 250ms of waiting: `{}`
   * is the documented no-op, so it cannot change anything on the board it is
   * listening to.
   */
  readFor(ms, nudge) {
    const until = Date.now() + (Number.isFinite(ms) ? ms : 0);
    let seen = '';
    let nextNudge = 0;
    while (Date.now() < until) {
      if (nudge && Date.now() >= nextNudge) {
        this.writeLine('{}\n', 100);
        nextNudge = Date.now() + 250;
      }
      seen += this.readAvailable();
      sleepSync(10);
    }
    return seen + this.readAvailable();
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

/**
 * Every reason a port that exists still will not open, and what to do about
 * each. Pure function of an errno, so it is testable with no port at all.
 *
 * @param {string} code an fs error `code`, e.g. 'EACCES'
 * @returns {{code:string, what:string, fix:string}}
 */
function portOpenAdvice(code) {
  switch (code) {
    case 'EACCES':
    case 'EPERM':
      return {
        code,
        what: 'permission denied on the port',
        fix:
          'macOS ships these ttys as crw-rw-rw-, so this is almost always a ' +
          'privacy prompt that was declined, or a port owned by root. Check ' +
          '`ls -l <port>`; if it is not world-writable, replug the board. ' +
          'Never chmod a device node to work around it.'
      };
    case 'EBUSY':
    case 'EAGAIN':
      return {
        code,
        what: 'the port is open in another program',
        fix:
          'Quit `pio device monitor`, `screen`, the Arduino serial monitor or ' +
          'another copy of this one, then try again. `lsof <port>` names the ' +
          'holder. Note that a macOS callout port (/dev/cu.*) does not lock: ' +
          'two programs can open it at once and neither is told, so a second ' +
          'holder usually does NOT arrive as this errno. That is why doctor ' +
          'asks lsof directly rather than waiting for an error.'
      };
    case 'ENOENT':
      return {
        code,
        what: 'the port vanished between being listed and being opened',
        fix: 'The board was unplugged, or it reset. Replug it and re-run.'
      };
    case 'ENXIO':
      return {
        code,
        what: 'the tty exists but has no device behind it',
        fix: 'A stale node from a board that went away. Replug the board.'
      };
    default:
      return {
        code: code || '(no code)',
        what: 'the port would not open',
        fix: 'Replug the board, then try a different USB port and cable.'
      };
  }
}

/**
 * Which other processes have this port open, by pid. Never this one.
 *
 * This exists because the obvious mechanism does not work. A macOS callout
 * device (/dev/cu.*) does not lock: opening one that another program already
 * holds succeeds, and both sides then read from the same stream and each get
 * a fraction of the answers. Verified on a real board, two opens of
 * /dev/cu.usbmodem1101 from one process, both returning a descriptor. So the
 * EBUSY branch of portOpenAdvice cannot fire for this device class, and the
 * only thing that can see a second holder from here is lsof.
 *
 * Read-only, like ioreg and stty, and any failure is reported as "nobody",
 * never as an error: lsof exits non-zero when nothing holds the file, which
 * is the ordinary case.
 *
 * @returns {number[]} pids, without duplicates and without our own
 */
function portHolders(portPath) {
  let out = '';
  try {
    out = execFileSync('/usr/sbin/lsof', ['-t', '--', String(portPath)], {
      encoding: 'utf8',
      timeout: 5000,
      stdio: ['ignore', 'pipe', 'ignore']
    });
  } catch (e) {
    // Exit 1 with no output is "no holders". Anything else is lsof being
    // unhappy, and an unhappy diagnostic tool must not become a finding.
    out = e && e.stdout ? String(e.stdout) : '';
  }
  const pids = [];
  for (const word of String(out).split(/\s+/)) {
    const pid = Number(word);
    if (!Number.isInteger(pid) || pid <= 0) continue;
    if (pid === process.pid) continue;
    if (!pids.includes(pid)) pids.push(pid);
  }
  return pids;
}

/**
 * Try to open a port and report exactly what happened. Opens and closes; it
 * writes nothing at all, so it is safe to call on a board mid-session.
 * @returns {{ok:boolean, advice:null|{code:string,what:string,fix:string}}}
 */
function probePort(portPath, opts) {
  let link = null;
  try {
    link = new Link(portPath, opts).open();
    return { ok: true, advice: null };
  } catch (e) {
    return { ok: false, advice: portOpenAdvice(e && e.code) };
  } finally {
    if (link) link.close();
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

/**
 * Printable ASCII only, whitespace collapsed, trimmed, then cut to `max`.
 *
 * This is the firmware's own `saySanitize` rule, applied on this side so that
 * what a person types and what the panel shows are decided in one place they
 * can see. 0x20..0x7E survives; tabs and newlines become spaces; control
 * bytes and everything >= 0x80 are dropped rather than mangled, because the
 * LovyanGFX bitmap fonts on the board have no glyphs past ASCII and there is
 * nothing honest to draw for an accent or an emoji.
 *
 * Cutting by characters is also cutting by bytes once nothing but ASCII is
 * left, which is what keeps this inside the firmware's byte-sized buffers.
 */
function sanitizeAscii(value, max) {
  if (typeof value !== 'string') return '';
  let out = '';
  let pendingSpace = false;
  for (const ch of value) {
    const c = ch.codePointAt(0);
    if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) {
      if (out.length > 0) pendingSpace = true;
      continue;
    }
    if (c < 0x20 || c > 0x7e) continue;
    if (pendingSpace) {
      out += ' ';
      pendingSpace = false;
    }
    out += ch;
    if (out.length >= max) break;
  }
  // The cap can land immediately after a space, and a stored owner name with a
  // trailing space is a name nobody typed.
  return out.slice(0, max).replace(/\s+$/, '');
}

/**
 * What sanitizing did to a string, so the CLI can say which thing happened.
 *
 * "we dropped your emoji" and "we cut your name in half" are different pieces
 * of news and a person needs to be told the right one; the old single boolean
 * reported the second as the first.
 */
function asciiNotes(value, max) {
  if (typeof value !== 'string') return { dropped: false, truncated: false };
  const trimmed = value.trim();
  const uncapped = sanitizeAscii(trimmed, Number.MAX_SAFE_INTEGER);
  return {
    dropped: uncapped !== trimmed,
    truncated: uncapped.length > max
  };
}

/**
 * Clamp a generated display string to a firmware buffer, counting BYTES.
 *
 * center, label and sub are built by this program and are ASCII in practice,
 * but `String.slice` counts UTF-16 units, so a stray non-ASCII character would
 * make a string that passes a character check and still overruns the board's
 * `char[N]`. Cutting on a byte budget, and never mid-codepoint, removes the
 * whole question.
 */
function clampBytes(value, maxBytes) {
  if (typeof value !== 'string') return '';
  if (Buffer.byteLength(value) <= maxBytes) return value;
  let out = '';
  let used = 0;
  for (const ch of value) {
    const n = Buffer.byteLength(ch);
    if (used + n > maxBytes) break;
    out += ch;
    used += n;
  }
  return out;
}

/** One of `allowed`, matched case insensitively, or null. Never throws. */
function pickEnum(value, allowed) {
  if (typeof value !== 'string') return null;
  const want = value.trim().toLowerCase();
  for (const a of allowed) {
    if (a === want) return a;
  }
  return null;
}

/**
 * An integer inside [lo, hi], or null when there is no honest answer.
 *
 * Booleans and empty strings are refused rather than coerced. `Number('')` is
 * 0, which is finite, so a `--secs ""` typed by a person would otherwise
 * become a real zero, and zero is the message card's hold-forever sentinel.
 */
function clampInt(value, lo, hi) {
  if (typeof value === 'boolean') return null;
  if (typeof value === 'string' && value.trim() === '') return null;
  const n = Number(value);
  if (!Number.isFinite(n)) return null;
  return Math.min(Math.max(Math.round(n), lo), hi);
}

/**
 * Unix seconds shifted into the host's own timezone.
 *
 * The board's `time` field is deliberately LOCAL rather than UTC, because the
 * only question it is asked is "has midnight happened where the owner is
 * sitting", and a device with no timezone database cannot answer that from
 * UTC. getTimezoneOffset() is minutes BEHIND UTC (420 for UTC-7), so
 * subtracting it shifts forward into local time. It is read fresh every time,
 * so a laptop carried across a timezone, or through a daylight-saving change,
 * corrects itself on the next hook event with nothing stored anywhere.
 */
function localUnixSeconds(nowMs) {
  return Math.floor((nowMs - new Date(nowMs).getTimezoneOffset() * 60000) / 1000);
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

  // The two numbers the board's stats screen cannot work out for itself. It
  // counts turns and times them off the state edges it already sees; it cannot
  // know what day it is where you are sitting, and it will not guess a token
  // total by integrating a rate sampled at hook events.
  //
  // Both are optional on the wire. A board running older firmware ignores
  // them, and a board running this one is complete without them: the daily
  // figures simply run from boot and the token ones stay at zero.
  //
  // `totalTokens` is the CURRENT session's cumulative usage, which restarts
  // when a new session does. The board banks differences rather than the value
  // itself, so a restart costs nothing and needs nothing stored here. Sending
  // it on every frame is what keeps this program stateless.
  f.time = localUnixSeconds(nowMs);
  if (Number.isFinite(metrics.totalTokens) && metrics.totalTokens >= 0) {
    f.tokens = Math.round(metrics.totalTokens);
  }
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
  const state = stateForEvent(eventName);
  if (!state) return null;
  // No metrics means no numbers, so no numbers go on the wire. The protocol
  // says an omitted field leaves the board's last value alone, which is the
  // whole reason `run` and the hook compose instead of fighting; emitting a
  // placeholder ring of 0.15 with a '--' centre and a TIME label instead
  // stamped that over whatever `run` had just sent, on every single hook
  // event, and pinned a permanently meaningless 15% ring when the hook was
  // on its own. `--no-metrics` sends state only, which is what it says.
  const frame =
    metrics === null || metrics === undefined
      ? { state, time: localUnixSeconds(nowMs) }
      : Object.assign({ state }, metricFields(metrics, nowMs));
  if (state === 'waiting') {
    // No tool name, no command text: the screen says only that you are the
    // one holding things up, and your terminal says what for.
    frame.sub = 'your turn';
  }
  return frame;
}

/**
 * Turn a frame object into the exact wire line, or into nothing.
 *
 * This is the only place a field becomes bytes, so it is the only place that
 * has to know the firmware's limits, and it is deliberately stricter than the
 * firmware is. The two rules from section 1:
 *
 *   - a bad ENUM would make the device drop the whole line, taking the good
 *     fields with it, so a bad enum is dropped from the frame here
 *   - a bad VALUE is clamped or sanitised to something the device will take
 *
 * Either way what comes out is a line the board can always accept, which is
 * what lets a caller treat a returned line as sent and an omitted field as a
 * host-side rejection rather than as a mystery on the panel.
 */
function frameToLine(frame) {
  const src = frame && typeof frame === 'object' ? frame : {};
  const f = {};

  // --- enums. Present only when we can name the exact accepted spelling.
  if (typeof src.state === 'string' && DEVICE_STATES.has(src.state)) f.state = src.state;
  const face = pickEnum(src.face, FACE_NAMES);
  if (face) f.face = face;
  const play = pickEnum(src.play, PLAY_VERBS);
  if (play) f.play = play;
  const stats = pickEnum(src.stats, STATS_VERBS);
  if (stats) f.stats = stats;
  const focus = pickEnum(src.focus, FOCUS_VERBS);
  if (focus) f.focus = focus;
  const reset = pickEnum(src.reset, RESET_VERBS);
  if (reset) f.reset = reset;
  const firstrun = pickEnum(src.firstrun, FIRSTRUN_VERBS);
  if (firstrun) f.firstrun = firstrun;

  // --- numbers and strings, clamped to the firmware's buffers.
  if (Number.isFinite(src.ring)) f.ring = round2(clamp01(src.ring));
  if (typeof src.center === 'string') f.center = clampBytes(src.center, CENTER_MAX);
  if (typeof src.label === 'string') f.label = clampBytes(src.label, LABEL_MAX);
  if (typeof src.sub === 'string') f.sub = clampBytes(src.sub, SUB_MAX);
  if (Number.isFinite(src.tps) && src.tps > 0) f.tps = Math.round(src.tps * 10) / 10;

  // The board ignores a `time` that is not a plausible wall clock and a
  // negative `tokens`, but neither should ever reach it from here.
  if (Number.isFinite(src.time) && src.time >= CLOCK_MIN_EPOCH) f.time = Math.floor(src.time);
  if (Number.isFinite(src.tokens) && src.tokens >= 0) f.tokens = Math.floor(src.tokens);

  // `name` and `say` are typed by a person, so they are sanitised rather than
  // rejected: an emoji costs you the emoji, not the line. An empty string is
  // meaningful for both (it clears), so both are emitted when the KEY is
  // present, not when the value is truthy.
  if (typeof src.name === 'string') f.name = sanitizeAscii(src.name, NAME_MAX);
  if (typeof src.say === 'string') f.say = sanitizeAscii(src.say, SAY_MAX);

  // 0 is the "hold until cleared" sentinel, so it must survive; a negative
  // value is a host arithmetic bug and the device would fall back to the
  // default, which we do here instead so the wire says what will happen.
  if (src.saysecs !== undefined && src.saysecs !== null) {
    const secs = clampInt(src.saysecs, 0, SAY_MAX_SECS);
    if (secs !== null) f.saysecs = Number(src.saysecs) < 0 ? SAY_DEFAULT_SECS : secs;
  }
  if (typeof src.dnd === 'boolean') f.dnd = src.dnd;
  if (src.focusmins !== undefined && src.focusmins !== null) {
    const mins = clampInt(src.focusmins, FOCUS_MIN_MINUTES, FOCUS_MAX_MINUTES);
    if (mins !== null) f.focusmins = mins;
  }

  let line = JSON.stringify(f) + '\n';
  if (Buffer.byteLength(line) <= MAX_LINE_BYTES) return line;

  // Over the cap the board discards the line WHOLE, so something has to go.
  // Least load-bearing first, and `state` is never in the list: the one field
  // this device exists to show is the one field worth arriving alone.
  for (const field of ['sub', 'say', 'center', 'label', 'name', 'tokens', 'time']) {
    delete f[field];
    line = JSON.stringify(f) + '\n';
    if (Buffer.byteLength(line) <= MAX_LINE_BYTES) break;
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
    if (transcriptPath && typeof transcriptPath === 'string') {
      // Named but unreadable: show no numbers rather than another session's.
      return fs.existsSync(transcriptPath) ? readMetrics(transcriptPath, nowMs) : null;
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

/**
 * How long the hook is willing to wait for its own payload, and how much of it
 * it is willing to hold.
 *
 * Codex writes one small object and closes the pipe, so in practice both are
 * unreachable. They exist because the old implementation was
 * `fs.readFileSync(0)`, which has no way out: a caller that opens the pipe and
 * never closes it wedges the hook, synchronously, where no timer can reach it,
 * and a caller that streams megabytes gets to choose how much memory a desk
 * toy allocates. Neither is a thing a hook may let happen to a session.
 */
const STDIN_DEADLINE_MS = 1500;
const STDIN_MAX_BYTES = 1024 * 1024;

/**
 * Read stdin to EOF, to the byte cap, or to the deadline, whichever is first.
 * Always drains: an unread pipe can wedge the caller. Never rejects.
 * @returns {Promise<string>}
 */
function readStdinAsync(timeoutMs, maxBytes) {
  return new Promise((resolve) => {
    if (process.stdin.isTTY) {
      resolve('');
      return;
    }
    const cap = Number.isFinite(maxBytes) ? maxBytes : STDIN_MAX_BYTES;
    const chunks = [];
    let total = 0;
    let settled = false;
    let timer = null;

    const finish = () => {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      process.stdin.removeListener('data', onData);
      process.stdin.removeListener('end', finish);
      process.stdin.removeListener('error', finish);
      try {
        process.stdin.pause();
      } catch {
        /* already gone */
      }
      let text = '';
      try {
        text = Buffer.concat(chunks, total).toString('utf8');
      } catch {
        /* out of memory is not a thing to report from a hook */
      }
      resolve(text);
    };

    const onData = (d) => {
      const room = cap - total;
      if (room <= 0) {
        finish();
        return;
      }
      const take = d.length <= room ? d : d.subarray(0, room);
      chunks.push(take);
      total += take.length;
      if (total >= cap) finish();
    };

    timer = setTimeout(finish, Number.isFinite(timeoutMs) ? timeoutMs : STDIN_DEADLINE_MS);
    process.stdin.on('data', onData);
    process.stdin.once('end', finish);
    process.stdin.once('error', finish);
    try {
      process.stdin.resume();
    } catch {
      finish();
    }
  });
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

/**
 * Always resolves 0, and everything inside is wrapped, because the only
 * promise this program makes to a Codex session is that it cannot break it.
 * A throw here would become an unhandled rejection and a non-zero exit, which
 * a user would see as their own turn failing because of a desk toy.
 */
async function cmdHook(opts) {
  try {
    const payload = readHookPayload(await readStdinAsync(STDIN_DEADLINE_MS, STDIN_MAX_BYTES));
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
  } catch {
    /* no device, a busy port, a bad payload, a bug in here: all the same */
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

/**
 * Candidate node binaries, best first. A hooks.json entry has to name a node
 * binary, and naming the one we happen to be running names a version-specific
 * path like /opt/homebrew/Cellar/node/26.7.0/bin/node, which stops working at
 * the next `brew upgrade node`. A stable alias survives that.
 */
const NODE_ALIASES = ['/opt/homebrew/bin/node', '/usr/local/bin/node', '/usr/bin/node'];

/**
 * Pick the node binary to bake into hooks.json.
 * @returns {{path:string, stable:boolean}}
 */
function stableNodePath(candidates) {
  for (const candidate of candidates || NODE_ALIASES) {
    try {
      const v = execFileSync(candidate, ['--version'], {
        encoding: 'utf8',
        timeout: 5000,
        stdio: ['ignore', 'pipe', 'ignore']
      }).trim();
      const major = Number((v.match(/^v(\d+)/) || [])[1]);
      if (Number.isFinite(major) && major >= 20) return { path: candidate, stable: true };
    } catch {
      /* not there, or not runnable: try the next one */
    }
  }
  return { path: process.execPath, stable: false };
}

/**
 * POSIX single quotes around one argument.
 *
 * Codex hands a command handler to a login shell: build_command in
 * vendor/codex/codex-rs/hooks/src/engine/command_runner.rs passes the whole
 * command line as one argument to $SHELL (or /bin/sh) with `-lc`. Double
 * quotes are therefore not safe here, whatever JSON.stringify escapes for:
 * sh still expands $, a backtick and a backslash inside them, so a perfectly
 * ordinary folder named with a `$` would silently break the hook forever and
 * nothing in doctor could see it. Single quotes make the shell take every
 * byte literally; the one character that cannot survive them is a single
 * quote, which is written as the four characters '\'' .
 */
function shellQuote(value) {
  return `'${String(value).split("'").join("'\\''")}'`;
}

/** The exact command string we register. */
function hookCommand(nodeBin, script) {
  const node = nodeBin || stableNodePath().path;
  return `${shellQuote(node)} ${shellQuote(script || selfPath())} hook`;
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
  // Trailing flags are allowed because the README documents one: appending
  // `--no-metrics` to the registered command is a supported edit, and an
  // anchor that stopped at `hook` made install, uninstall and doctor disagree
  // about whether such a handler was ours. Uninstall then left it behind and
  // install added a second group beside it, so one event fired twice.
  return /codex-companion(\.js)?["']?\s+hook(\s+--[\w-]+(\s+[^\s-][^\s]*)?)*\s*$/.test(
    h.command.trim()
  );
}

/**
 * The trailing flags somebody appended to a registered command.
 *
 * The README documents exactly one such edit, `--no-metrics`, so a rewrite of
 * a stale command has to carry it forward rather than quietly undo a choice
 * the owner made. Returns '' for the ordinary case.
 */
function hookCommandFlags(command) {
  const m = /codex-companion(?:\.js)?["']?\s+hook((?:\s+--[\w-]+(?:\s+[^\s-][^\s]*)?)*)\s*$/.exec(
    String(command || '').trim()
  );
  return m && m[1] ? m[1].trim() : '';
}

/**
 * Why we will not merge into this parsed hooks.json, or null for "go ahead".
 *
 * Repairing a shape we did not write means throwing somebody's value away
 * without telling them, so this refuses instead, the same way an unparseable
 * file already does. The two shapes that used to be silently destroyed: a
 * non-object root (an array root serialized back out with our events dropped,
 * and install-hook still printed "Registered for: ..."), and an event key
 * holding an object rather than the array Codex writes (replaced wholesale).
 *
 * @returns {string|null} what was found, phrased to go after "is not the
 *   shape Codex writes:"
 */
function hooksFileProblem(existing, events) {
  if (existing === null || existing === undefined) return null;
  if (typeof existing !== 'object' || Array.isArray(existing)) {
    return `its top level is ${describeJson(existing)}, not an object`;
  }
  const hooks = existing.hooks;
  if (hooks !== undefined && (typeof hooks !== 'object' || hooks === null || Array.isArray(hooks))) {
    return `its "hooks" value is ${describeJson(hooks)}, not an object`;
  }
  if (!hooks) return null;
  for (const eventName of events || INSTALLED_EVENTS) {
    const groups = hooks[eventName];
    if (groups !== undefined && !Array.isArray(groups)) {
      return `hooks.${eventName} is ${describeJson(groups)}, not an array`;
    }
  }
  return null;
}

/** A parsed JSON value named the way a person would name it. */
function describeJson(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'an array';
  if (typeof v === 'object') return 'an object';
  if (typeof v === 'string') return 'a string';
  if (typeof v === 'number') return 'a number';
  if (typeof v === 'boolean') return 'a boolean';
  return typeof v;
}

/**
 * Merge our handlers into an existing hooks.json object.
 *
 * A handler of ours that is already there but names a command we would no
 * longer write gets that command REPLACED, not skipped. Skipping is what made
 * doctor's remedy for a stale hook command a lie: `brew upgrade node` moves
 * the binary, doctor said "re-run install-hook, it rewrites the command in
 * place", install-hook said "Already registered; nothing changed", and the
 * dead path survived byte for byte. Only the command string is touched, so a
 * timeout somebody tuned by hand is left alone.
 *
 * Call hooksFileProblem() first: this assumes the shapes it walks are the
 * ones Codex writes.
 *
 * @returns {{next:object, added:string[], updated:string[]}}
 */
function mergeHooks(existing, command, events) {
  const next = existing && typeof existing === 'object' ? deepCopy(existing) : {};
  if (!next.hooks || typeof next.hooks !== 'object' || Array.isArray(next.hooks)) next.hooks = {};
  const added = [];
  const updated = [];
  for (const eventName of events || INSTALLED_EVENTS) {
    if (!Array.isArray(next.hooks[eventName])) next.hooks[eventName] = [];
    const groups = next.hooks[eventName];
    let found = false;
    let changed = false;
    for (const g of groups) {
      if (!g || !Array.isArray(g.hooks)) continue;
      for (const h of g.hooks) {
        if (!isOurHandler(h)) continue;
        found = true;
        const flags = hookCommandFlags(h.command);
        const want = flags ? `${command} ${flags}` : command;
        if (h.command !== want) {
          h.command = want;
          changed = true;
        }
      }
    }
    if (found) {
      if (changed) updated.push(eventName);
      continue;
    }
    groups.push({ hooks: [hookHandler(command, eventName)] });
    added.push(eventName);
  }
  return { next, added, updated };
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
  const problem = hooksFileProblem(existing);
  if (problem !== null) {
    say(`${file} is not the shape Codex writes: ${problem}.`);
    say('Refusing to touch it: merging would throw that value away without');
    say('telling you. Move the file aside, or fix it by hand, then run this');
    say('again. Codex writes {"hooks": {"<EventName>": [ ... ]}}.');
    return 1;
  }
  const node = stableNodePath();
  const command = hookCommand(node.path);
  const { next, added, updated } = mergeHooks(existing, command);
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

  // Say what the file now holds, not what we intended it to hold. Reporting
  // "Registered for: ..." over a write that registered nothing is the exact
  // failure this line exists to make impossible.
  const landed = registeredEvents(readJsonFile(file));
  const lost = added.concat(updated).filter((e) => !landed.includes(e));
  if (lost.length > 0) {
    say(`Wrote ${file}, but it does not register ${lost.join(', ')}.`);
    say('That should not be possible. Do not rely on this hook: run');
    say('`codex-companion doctor` and look at the hooks.json section.');
    return 1;
  }
  if (added.length) say(`Registered for: ${added.join(', ')}`);
  if (updated.length) say(`Rewrote the stale command for: ${updated.join(', ')}`);
  if (!added.length && !updated.length) say('Already registered; nothing changed.');
  say('');
  if (!node.stable) {
    say('Note: no stable node alias was found, so this names the exact binary');
    say(`      ${node.path}. Re-run install-hook after upgrading node.`);
    say('');
  }
  say('Codex will not run a new hook until you approve it. Start Codex: it');
  say('prompts with "Hooks need review", and /hooks lists them at any time.');
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

/**
 * True for a setting whose VALUE makes the amber "waiting" light unreachable,
 * wherever in the file it was found.
 *
 * `sandbox_mode` is deliberately not here. It used to be, and it was wrong:
 * the sandbox is not the approval gate, `approval_policy` is.
 * vendor/codex/codex-rs/core/src/exec_policy.rs returns Decision::Prompt for
 * AskForApproval::UnlessTrusted whatever the FileSystemSandboxKind is, and
 * assess_patch_safety in core/src/safety.rs returns AskUser for UnlessTrusted
 * before it consults a sandbox at all. Telling somebody to change
 * danger-full-access on that evidence is a wrong answer delivered in this
 * report's loudest voice.
 */
function wouldSilenceApprovals(entry) {
  return (
    !!entry &&
    ((entry.key === 'approval_policy' && entry.value === 'never') ||
      (entry.key === 'approvals_reviewer' && entry.value === 'auto_review'))
  );
}

/**
 * True for a setting that makes the amber light unreachable for THIS run.
 *
 * Section matters. A key inside `[profiles.yolo]` applies when that profile is
 * selected and not otherwise, so keeping a yolo profile beside a prompting
 * default is a perfectly ordinary thing to do and must not fail the report.
 * Those are reported separately, as a warning that says "only when that
 * profile is selected".
 */
function silencesApprovals(entry) {
  return !!entry && entry.section === '(top level)' && wouldSilenceApprovals(entry);
}

/**
 * The hook-trust records Codex keeps in config.toml.
 *
 * Codex stores one entry per registered handler under
 * `[hooks.state."<hooks file>:<event>:<group index>:<handler index>"]`, holding
 * `enabled` and `trusted_hash`. Struct:
 * `vendor/codex/codex-rs/config/src/hook_config.rs`, `HookStateToml`; the key
 * shape is the one in that module's own deserialization test
 * (`hooks_tests.rs`, `hooks_toml_deserializes_inline_events_and_state_map`).
 *
 * A handler with no entry has never been approved. One with `enabled = false`
 * was approved and then switched off. From outside they look identical (the
 * hook simply never fires) and they are completely different things to fix,
 * which is the whole reason this reads the file rather than guessing.
 *
 * Shallow on purpose: this is not a TOML parser, and `/hooks` inside Codex is
 * the authority. It reports what it found and says so.
 */
function scanHookState(text, hooksPath) {
  const out = [];
  let current = null;
  for (const raw of String(text || '').split('\n')) {
    const line = raw.trim();
    if (line.startsWith('#')) continue;
    const sec = line.match(/^\[hooks\.state\.(?:"([^"]*)"|'([^']*)'|([^\]]*))\]/);
    if (sec) {
      current = { key: sec[1] || sec[2] || sec[3] || '', enabled: null, trusted: false };
      out.push(current);
      continue;
    }
    if (line.startsWith('[')) {
      current = null;
      continue;
    }
    if (!current) continue;
    if (/^enabled\s*=\s*true\b/.test(line)) current.enabled = true;
    else if (/^enabled\s*=\s*false\b/.test(line)) current.enabled = false;
    else if (/^trusted_hash\s*=\s*['"]/.test(line)) current.trusted = true;
  }
  return hooksPath ? out.filter((e) => e.key.includes(hooksPath)) : out;
}

/**
 * Pull the quoted arguments back out of a command we wrote into hooks.json.
 *
 * Both quotings are read. `hookCommand` writes POSIX single quotes now, and
 * double quotes are what it wrote before that, so a hooks.json installed by
 * an earlier copy of this file is still understood rather than silently
 * reported as having no paths in it.
 *
 * @returns {string[]}
 */
function commandParts(command) {
  const text = String(command || '');
  const out = [];
  let i = 0;
  while (i < text.length) {
    if (text[i] === "'") {
      let value = '';
      i += 1;
      while (i < text.length) {
        if (text[i] !== "'") {
          value += text[i];
          i += 1;
          continue;
        }
        // The four characters '\'' are how a single quote survives; anything
        // else that is a quote ends the argument.
        if (text.startsWith("'\\''", i)) {
          value += "'";
          i += 4;
          continue;
        }
        i += 1;
        break;
      }
      out.push(value);
      continue;
    }
    if (text[i] === '"') {
      const m = /^"(?:[^"\\]|\\.)*"/.exec(text.slice(i));
      if (m) {
        try {
          out.push(JSON.parse(m[0]));
        } catch {
          /* not something we wrote; ignore it rather than guess */
        }
        i += m[0].length;
        continue;
      }
    }
    i += 1;
  }
  return out;
}

/**
 * A doctor report.
 *
 * Every line is one of three verdicts, and every verdict that is not `ok`
 * carries the exact thing to do about it. A diagnosis with no remedy is just
 * an error message with better manners, and the person reading this is a
 * recipient with a gift that is not working, not the author.
 */
function makeReport() {
  const rows = [];
  const add = (level, label, detail, fix) => {
    rows.push({ level, label, detail, fix: fix || null });
  };
  return {
    rows,
    heading: (text) => rows.push({ level: 'heading', label: text }),
    plain: (label, detail) => add('plain', label, detail),
    ok: (label, detail) => add('ok', label, detail),
    warn: (label, detail, fix) => add('warn', label, detail, fix),
    fail: (label, detail, fix) => add('fail', label, detail, fix),
    failures: () => rows.filter((r) => r.level === 'fail').length,
    warnings: () => rows.filter((r) => r.level === 'warn').length
  };
}

const VERDICT = { ok: 'ok  ', warn: 'WARN', fail: 'FAIL', plain: '    ' };

function printReport(report, width) {
  const w = width || 66;
  for (const row of report.rows) {
    if (row.level === 'heading') {
      say('');
      say(row.label);
      continue;
    }
    const label = String(row.label).padEnd(15);
    say(`  ${VERDICT[row.level]} ${label} ${row.detail === undefined ? '' : row.detail}`);
    if (!row.fix) continue;
    for (const chunk of wrapText(row.fix, w)) say(`       -> ${chunk}`);
  }
}

/** Greedy wrap, so a remedy reads as prose instead of running off the screen. */
function wrapText(text, width) {
  const words = String(text || '').split(/\s+/).filter(Boolean);
  const lines = [];
  let line = '';
  for (const word of words) {
    if (line && line.length + 1 + word.length > width) {
      lines.push(line);
      line = word;
    } else {
      line = line ? `${line} ${word}` : word;
    }
  }
  if (line) lines.push(line);
  return lines.length ? lines : [''];
}

/**
 * Every realistic way this can be broken on somebody else's desk, checked by
 * name, in the order they have to be true.
 *
 * Exits 0 by default even when it finds problems, because a report that fails
 * the shell is a report people stop running. `--strict` is there for anyone
 * who wants it in a script.
 */
function cmdDoctor(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  const r = makeReport();

  r.heading('WHERE THIS IS RUNNING');
  r.plain('node', `${process.version} on ${process.platform}`);
  r.plain('companion', selfPath());
  if (!IS_MAC) {
    // First, and a FAIL, because every device row underneath it is about to
    // fail for this reason and blame the cable while doing it.
    r.fail('platform', `${process.platform}, and this program is macOS-only`, PLATFORM_NOTE);
  }

  // ---------------------------------------------------------------- device
  r.heading('DEVICE');
  const pick = choosePort(opts);
  r.plain('serial ports', pick.ports.length ? pick.ports.join(', ') : '(none)');

  if (!pick.chosen && pick.ports.length === 0) {
    r.fail(
      'device',
      'NOT FOUND: no serial ports at all',
      'The commonest cause by a wide margin is a charge-only USB-C cable: it ' +
        'powers the board perfectly and carries no data, so no port ever ' +
        'appears. Try a cable you have moved files over, then the other USB ' +
        'port on the board. If the screen is dark too, it is the power, not ' +
        'the data.'
    );
  } else if (!pick.chosen && pick.matched.length > 1) {
    r.fail(
      'device',
      `${pick.matched.length} boards match: ${pick.matched.join(', ')}`,
      `Two devices report the same USB id and only you can say which one is ` +
        `on your desk. Name it: --port ${pick.matched[0]} . Or unplug the ` +
        `other ESP32-S3, since every one of them enumerates as ` +
        `${hex(ESP_VENDOR_ID)}:${hex(ESP_PRODUCT_ID)} with the same ` +
        `description.`
    );
  } else if (!pick.chosen) {
    r.fail(
      'device',
      `NOT FOUND: no port reports USB ${hex(ESP_VENDOR_ID)}:${hex(ESP_PRODUCT_ID)}`,
      'The ports listed above are something else (a USB power meter, a phone, ' +
        'a dongle). Replug the board and re-run. If a port here is obviously ' +
        'the board, name it with --port and this check is skipped.'
    );
  } else if (opts.port) {
    // Deliberately not `ok`. A named port is taken entirely on trust: nothing
    // has checked that it is a board, or that it exists at all, and the row
    // below this one is free to fail with ENOENT. The whole design of this
    // report is that a verdict word can be believed at a glance, so it is not
    // spent on something nobody looked at.
    r.plain('device', pick.chosen);
    r.plain('', pick.reason);
  } else {
    r.ok('device', pick.chosen);
    r.plain('', pick.reason);
  }

  if (pick.chosen) {
    const probe = probePort(pick.chosen, { read: true });
    if (probe.ok) {
      r.ok('port opens', 'read and write');
    } else {
      r.fail('port opens', `${probe.advice.what} (${probe.advice.code})`, probe.advice.fix);
    }

    // Opening cleanly proves nothing about being alone on the cable, because
    // macOS callout ports do not lock. A second reader steals a share of every
    // answer and announces itself with no error anywhere, which reads as a
    // flaky board. Named here rather than inferred from an errno that this
    // device class never produces.
    const holders = portHolders(pick.chosen);
    if (holders.length > 0) {
      r.warn(
        'port sharing',
        `${holders.length} other process(es) hold this port: ${holders.join(', ')}`,
        'A /dev/cu.* port does not lock, so two programs can hold it at once ' +
          'and the bytes coming back are split between them. Quit `pio device ' +
          'monitor`, `screen`, the Arduino serial monitor or another copy of ' +
          'this program, then re-run. `ps -p <pid>` names each holder.'
      );
    }

    if (probe.ok) {
      let link = null;
      try {
        link = new Link(pick.chosen, { read: true }).open();
        // The board's transmit path runs one message behind, so hearing
        // anything at all takes a keepalive to flush it: docs/protocol.md 3.3.
        const seen = link.readFor(900, true);
        const hello = /hello\s+tdisplay-s3/i.test(seen);
        const acked = /\bok\b/.test(seen);
        if (hello) r.ok('handshake', 'greeting seen');
        else if (acked) r.ok('handshake', 'acked our keepalive');
        else {
          r.fail(
            'handshake',
            'the port opened but the board never answered',
            'The greeting is only sent once, a couple of seconds after boot, ' +
              'so a board that has been up a while answers with `ok` instead. ' +
              'No answer at all means it is not this firmware, or it is wedged: ' +
              'unplug it, plug it back in, and run doctor again within a minute.'
          );
        }
        const name = seen.match(/name="([^"]*)"/);
        if (name) {
          r.plain('board name', name[1] ? name[1] : '(none stored yet)');
        }
      } catch (e) {
        const advice = portOpenAdvice(e && e.code);
        r.fail('handshake', `${advice.what} (${advice.code})`, advice.fix);
      } finally {
        if (link) link.close();
      }
    }
  }

  // ----------------------------------------------------------------- codex
  r.heading('CODEX');
  let codexVersion = null;
  try {
    codexVersion = execFileSync('codex', ['--version'], {
      encoding: 'utf8',
      timeout: 10000,
      stdio: ['ignore', 'pipe', 'ignore']
    }).trim();
  } catch {
    /* not on PATH, which is a finding rather than an error */
  }
  if (codexVersion) {
    r.ok('codex', codexVersion);
  } else {
    r.fail(
      'codex',
      'not on this PATH',
      'The Codex CLI is the thing this hooks into, and without it the device ' +
        'still runs its own ambient mode forever. `command -v codex` from the ' +
        'shell you actually run Codex in: if it is there and not here, run ' +
        'this from that shell. If it is nowhere, install the Codex CLI (it ' +
        'ships as the npm package @openai/codex) and re-run.'
    );
  }
  r.plain('CODEX_HOME', home);

  const hooksPath = path.join(home, 'hooks.json');
  const hooksFile = readJsonFile(hooksPath);
  let registered = [];
  if (!fs.existsSync(hooksPath)) {
    r.fail(
      'hooks.json',
      'not present',
      `Nothing is registered, so no event can reach the board. Run: ` +
        `node ${selfPath()} install-hook`
    );
  } else if (hooksFile === null) {
    r.fail(
      'hooks.json',
      `${hooksPath} exists but is not valid JSON`,
      'Codex cannot read it either, so every hook in it is dead. This ' +
        'program refuses to touch a file it cannot parse: fix the JSON by ' +
        'hand (a trailing comma is the usual culprit), or move it aside and ' +
        'run install-hook to write a fresh one.'
    );
  } else {
    r.plain('hooks.json', hooksPath);
    registered = registeredEvents(hooksFile);
    if (registered.length === 0) {
      r.fail(
        'our hook',
        'NOT registered',
        `The file is there and it is somebody else's. Run: node ${selfPath()} ` +
          `install-hook . It merges in and touches nothing that is already there.`
      );
    } else {
      const missing = INSTALLED_EVENTS.filter((e) => !registered.includes(e));
      if (missing.length === 0) {
        r.ok('our hook', `registered for ${registered.join(', ')}`);
      } else {
        r.warn(
          'our hook',
          `registered for ${registered.join(', ')}`,
          `Missing ${missing.join(', ')}. ` +
            (missing.includes('PermissionRequest')
              ? 'PermissionRequest is the one that matters: without it the ' +
                'amber "your turn" light can never appear, which is the whole ' +
                'point of the object. '
              : '') +
            `Re-run install-hook to add what is missing.`
        );
      }

      // A registered command that no longer resolves is the failure nobody
      // suspects: `brew upgrade node` moves the binary, and moving this
      // folder moves the script. Codex runs it, the shell cannot find it, and
      // the hook fails silently forever.
      const broken = [];
      for (const eventName of registered) {
        for (const group of hooksFile.hooks[eventName] || []) {
          for (const handler of (group && group.hooks) || []) {
            if (!isOurHandler(handler)) continue;
            for (const part of commandParts(handler.command)) {
              if (!path.isAbsolute(part)) continue;
              if (!fs.existsSync(part) && !broken.includes(part)) broken.push(part);
            }
          }
        }
      }
      if (broken.length === 0) {
        r.ok('hook command', 'the node binary and this script both still exist');
      } else {
        r.fail(
          'hook command',
          `names ${broken.length} path(s) that no longer exist: ${broken.join(', ')}`,
          'Codex runs the command and the shell cannot find it, so the hook ' +
            'fails silently every time. This happens when node is upgraded or ' +
            'when this folder is moved. Re-run install-hook from where the ' +
            'file lives now; it rewrites the command in place.'
        );
      }
    }
  }

  // ------------------------------------------------------------ hook trust
  let configText = '';
  try {
    configText = fs.readFileSync(path.join(home, 'config.toml'), 'utf8');
  } catch {
    /* no config at all is a perfectly normal state */
  }
  if (registered.length > 0) {
    const state = scanHookState(configText, hooksPath);
    const disabled = state.filter((e) => e.enabled === false);
    const trusted = state.filter((e) => e.trusted && e.enabled !== false);
    if (disabled.length > 0) {
      r.fail(
        'hook trust',
        `${disabled.length} handler(s) are registered but switched OFF`,
        'config.toml records enabled = false for them, which means somebody ' +
          'approved this hook and then disabled it. It will never fire. Turn ' +
          'it back on with the /hooks command inside Codex.'
      );
    } else if (trusted.length > 0) {
      r.ok('hook trust', `config.toml records trust for ${trusted.length} handler(s)`);
    } else {
      r.warn(
        'hook trust',
        'not trusted yet',
        'Codex will not run a hook it has not been told to trust, so this is ' +
          'registered and inert. Start Codex: it prompts with "Hooks need ' +
          'review". /hooks lists them at any time and is the authority; this ' +
          'line is only reading config.toml.'
      );
    }
  }

  // -------------------------------------------------------------- approvals
  r.heading('APPROVALS');
  const settings = scanConfigToml(configText);
  const silencing = settings.filter(silencesApprovals);
  const inProfiles = settings.filter((s) => wouldSilenceApprovals(s) && !silencesApprovals(s));
  if (silencing.length === 0) {
    r.ok('approvals', 'nothing in config.toml suppresses approval prompts');
  } else {
    r.fail(
      'approvals',
      'WARNING: this config can never raise an approval prompt',
      `The amber "waiting" light is the one thing this device exists for, and ` +
        `it is driven by Codex's PermissionRequest event. With ` +
        silencing.map((s) => `[${s.section}] ${s.key} = "${s.value}"`).join(' and ') +
        ` that event never fires, so the board will look broken while working ` +
        `perfectly. Change the setting, or use a profile that asks, and the ` +
        `light comes back. Nothing needs reinstalling.`
    );
  }
  // Printed whether or not the check passed. The line that decides the
  // question is `approval_policy`, and a reader who is being told to change
  // something needs to see it in order to disagree with us.
  for (const s of settings) r.plain('', `[${s.section}] ${s.key} = "${s.value}"`);
  if (inProfiles.length > 0) {
    r.warn(
      'approval profiles',
      `${inProfiles.length} profile setting(s) would not prompt`,
      inProfiles.map((s) => `[${s.section}] ${s.key} = "${s.value}"`).join(' and ') +
        ` applies only when that profile is selected, so it is not a problem ` +
        `on its own. If the amber light never appears, check which profile ` +
        `you are actually running under.`
    );
  }

  // ---------------------------------------------------------------- session
  r.heading('SESSION');
  const newest = newestRolloutFile(path.join(home, 'sessions'));
  if (!newest) {
    r.warn(
      'session',
      'none found yet',
      'The ring, the percentage and the stopwatch are read out of the rollout ' +
        'file Codex writes per session, and there is not one here yet. Run ' +
        'codex once and re-run doctor. The state colours work without it.'
    );
  } else {
    const now = Date.now();
    const m = readMetrics(newest.path, now);
    r.plain('session', newest.path);
    r.plain(
      'metrics',
      `ctx ${m.ctxFill === null ? '(unknown)' : `${Math.round(m.ctxFill * 100)}%`}` +
        `, window ${m.contextWindow === null ? '(unknown)' : m.contextWindow}` +
        `, turn ${m.turnOpen ? 'open' : 'closed'}`
    );
    r.plain('frame', frameToLine(metricFields(m, now)).trim());
  }

  printReport(r);

  const failed = r.failures();
  const warned = r.warnings();
  say('');
  if (failed === 0 && warned === 0) {
    say('Everything checks out.');
  } else {
    say(
      `${failed} problem(s) and ${warned} warning(s). Each one above is ` +
        `followed by what to do about it.`
    );
  }
  say('The device itself needs none of this: with no host at all it runs its');
  say('own ambient mode forever. Every finding here is about the extras.');
  return opts.strict && failed > 0 ? 1 : 0;
}

// --------------------------------------------------------------------------
// 9. One-shot commands: everything the firmware grew a field for
//
// Each of these is one line down the cable and then the port is closed again.
// Every field the firmware documents has a door here, because the alternative
// is a person googling `printf` quoting rules at their desk. What they do NOT
// have is a door that changes the device permanently by accident: `reset` is
// deliberately absent, and stays a `printf` in the flashing runbook.
// --------------------------------------------------------------------------

/**
 * Say why nothing could be opened, and return 1.
 *
 * When --port named a specific tty, choosePort's `reason` is the wrong
 * sentence: it explains why identities were NOT checked ("you asked for this
 * one"), which tells a person nothing about a port that exists and will not
 * open. The errno does, and the fix for each one is already written in
 * portOpenAdvice, so ask for it. doctor has always done this; these paths
 * simply never did.
 */
function sayNoDevice(opts) {
  if (opts.port) {
    const probe = probePort(opts.port, opts);
    if (!probe.ok && probe.advice) {
      say(`no device: ${opts.port}: ${probe.advice.what} (${probe.advice.code})`);
      for (const chunk of wrapText(probe.advice.fix, 72)) say(chunk);
      say('Run `codex-companion doctor` for what to do about that.');
      return 1;
    }
  }
  say(`no device: ${choosePort(opts).reason}`);
  say('Run `codex-companion doctor` for what to do about that.');
  return 1;
}

/**
 * Send one frame and say what actually went out.
 *
 * What is printed is the serialized line, not what was typed, so the clamping
 * and the sanitising are visible: `say "caf<e-acute>"` prints `{"say":"caf"}` and the
 * person can see why the panel is missing a letter.
 */
function sendOnce(opts, frame) {
  const line = frameToLine(frame);
  if (opts.dryRun) {
    say(line.trimEnd());
    say('--dry-run: nothing was written.');
    return 0;
  }
  const link = openLink(opts);
  if (!link) {
    if (!IS_MAC && !opts.port) {
      for (const chunk of wrapText(PLATFORM_NOTE, 72)) say(chunk);
      return 0;
    }
    return sayNoDevice(opts);
  }
  try {
    if (!link.writeFrame(frame, 500)) {
      // Not "is something else holding it": a /dev/cu.* port does not lock, so
      // a second holder is a reader that steals answers and cannot make our
      // write fail. A failed write means the board stopped draining the tty.
      say('the port would not take the line. The board stopped reading it:');
      say(`unplug ${link.path} and plug it back in, then run \`codex-companion doctor\`.`);
      const holders = portHolders(link.path);
      if (holders.length > 0) {
        say(
          `(${holders.length} other process(es) also hold it: ${holders.join(', ')}. ` +
            'That splits the answers, not the writes.)'
        );
      }
      return 1;
    }
  } finally {
    link.close();
  }
  say(line.trimEnd());
  return 0;
}

/** Everything after the subcommand, joined the way a shell splits it. */
function restText(opts) {
  return (opts.args || []).join(' ');
}

/**
 * Tell the person what the board is going to do to their text, before it does
 * it. Silence when nothing was changed, which is the usual case.
 */
function asciiComplaints(raw, max, applied, noun) {
  if (!applied) return [];
  const notes = asciiNotes(raw, max);
  const out = [];
  if (notes.dropped) {
    out.push(`note: the panel fonts are ASCII only, so accents and emoji are`);
    out.push(`      dropped and runs of whitespace collapse to one.`);
  }
  if (notes.truncated) {
    out.push(`note: the board holds ${max} characters, so the ${noun} was cut.`);
  }
  if (out.length) out.push('      The line below is exactly what it will hold.');
  return out;
}

function cmdSay(opts) {
  const raw = restText(opts);
  if (!opts.clear && !raw) {
    say('usage: codex-companion say "back in five" [--secs 300]');
    say('       codex-companion say --clear');
    say(`Printable ASCII, up to ${SAY_MAX} characters. --secs 0 holds it until`);
    say('it is cleared; either button on the board takes it down too.');
    return 1;
  }
  const text = opts.clear ? '' : raw;
  for (const note of asciiComplaints(raw, SAY_MAX, text, 'message')) say(note);
  const frame = { say: text };
  if (opts.secs !== undefined) frame.saysecs = opts.secs;
  return sendOnce(opts, frame);
}

function cmdName(opts) {
  const raw = restText(opts);
  if (!opts.clear && !raw) {
    say('usage: codex-companion name "Alex Rivera"');
    say('       codex-companion name --clear');
    say(`Up to ${NAME_MAX} characters of ASCII. It is stored on the board, so it`);
    say('survives unplugging, and it comes back in the greeting on the next boot.');
    return 1;
  }
  const text = opts.clear ? '' : raw;
  for (const note of asciiComplaints(raw, NAME_MAX, text, 'name')) say(note);
  const code = sendOnce(opts, { name: text });
  if (code === 0 && !opts.dryRun) {
    say('Unplug and replug to prove it stuck: the greeting reads it back.');
  }
  return code;
}

function cmdFace(opts) {
  const want = (opts.args || [])[0];
  const face = pickEnum(want, FACE_NAMES);
  if (!face) {
    say(`usage: codex-companion face <${FACE_NAMES.join('|')}>`);
    if (want) say(`"${want}" is not one of them, and sending it would make the board`);
    if (want) say('drop the whole line, so it was not sent.');
    say('The choice is stored on the board and survives unplugging.');
    return 1;
  }
  return sendOnce(opts, { face });
}

/**
 * The board counts turns and times them itself, but it cannot know what day it
 * is where you are sitting, so the daily figures have no midnight until
 * somebody tells it one. This is that. The hook path already sends it on every
 * event; this is for a board that is being driven by hand.
 */
function cmdTime(opts) {
  const now = Date.now();
  const frame = { time: localUnixSeconds(now) };
  if (opts.args && opts.args.length) {
    const tokens = clampInt(opts.args[0], 0, Number.MAX_SAFE_INTEGER);
    if (tokens !== null) frame.tokens = tokens;
  }
  const code = sendOnce(opts, frame);
  if (code === 0 && !opts.dryRun) {
    say(`local ${new Date(now).toLocaleString()}: the stats screen says TODAY`);
    say('rather than SINCE BOOT from here, and rolls over at your midnight.');
  }
  return code;
}

function cmdDnd(opts) {
  const want = (opts.args || [])[0];
  if (want !== 'on' && want !== 'off') {
    say('usage: codex-companion dnd on|off');
    say('The same sign the two buttons put up. It is never persisted, and any');
    say('single press on the board takes it down.');
    return 1;
  }
  return sendOnce(opts, { dnd: want === 'on' });
}

function cmdStats(opts) {
  const verb = pickEnum((opts.args || [])[0], STATS_VERBS);
  if (!verb) {
    say(`usage: codex-companion stats <${STATS_VERBS.join('|')}>`);
    say('The same screen a tap on button 1 opens. It is refused while a prompt');
    say('is pending, because nothing may cover a question.');
    return 1;
  }
  return sendOnce(opts, { stats: verb });
}

function cmdFocus(opts) {
  const verb = pickEnum((opts.args || [])[0], FOCUS_VERBS);
  if (!verb) {
    say(`usage: codex-companion focus <${FOCUS_VERBS.join('|')}> [--mins 25]`);
    say(`The pomodoro on the two buttons. --mins is ${FOCUS_MIN_MINUTES} to`);
    say(`${FOCUS_MAX_MINUTES} and is stored on the board. The clock is never`);
    say('refused; only its screen is, and only while a prompt is pending.');
    return 1;
  }
  const frame = { focus: verb };
  if (opts.mins !== undefined) frame.focusmins = opts.mins;
  return sendOnce(opts, frame);
}

function cmdPlay(opts) {
  const verb = pickEnum((opts.args || [])[0], PLAY_VERBS);
  if (!verb) {
    say(`usage: codex-companion play <${PLAY_VERBS.join('|')}>`);
    say('The toy, which is a two-second hold on button 1 at the desk. "hop" is');
    say('the jump, and it exists so the game can be exercised with no finger.');
    return 1;
  }
  return sendOnce(opts, { play: verb });
}

function cmdFirstrun(opts) {
  const code = sendOnce(opts, { firstrun: 'play' });
  if (code === 0 && !opts.dryRun) {
    say('8.6 seconds. It never spends an armed board: only a real first power-on');
    say('marks itself used, so this is safe to run on a unit you are about to give.');
  }
  return code;
}

// --------------------------------------------------------------------------
// 10. selftest: the whole path, against a real board
//
// doctor answers "is this set up". This answers "does it work", which is a
// different question and the one worth asking before handing somebody a
// present. Every check here is a real line down the real cable and a real
// answer read back off it.
//
// It writes nothing persistent. The only fields it sends that outlive the run
// are the day's token counters, which are RAM on the device and reset on the
// next power cycle; it deliberately never touches `name`, `face` or the
// records, and it never sends `reset`.
// --------------------------------------------------------------------------

/** A probe string that could only appear on the wire if a bad line were taken. */
const REJECT_PROBE = 'ZZPROBE';

/**
 * The two token totals selftest sends, and the difference it expects to see.
 * Small enough that the pair cannot cross a milestone rung on its own, so a
 * unit being checked before it is wrapped does not celebrate anything.
 */
const TOKEN_PROBE_BASE = 1000;
const TOKEN_PROBE_STEP = 12345;

/** `tokens=<n>` out of a stats notice, or null. */
function tokensIn(text) {
  const m = String(text).match(/tokens=(\d+)/);
  return m ? Number(m[1]) : null;
}

function cmdSelftest(opts) {
  const rows = [];
  const record = (name, pass, detail, fix) => {
    rows.push({ name, pass });
    say(`${pass ? 'PASS' : 'FAIL'}  ${String(name).padEnd(34)} ${detail || ''}`);
    if (!pass && fix) for (const chunk of wrapText(fix, 62)) say(`      -> ${chunk}`);
    return pass;
  };
  // Every pid ever seen holding this port. Seeded when the port opens and
  // topped up in `verdict`, because one sample taken at t=0 cannot see a rival
  // that arrives at t=5 and then takes a share of the answer to every check
  // after it. Without the second look, that contention is printed as ten
  // hardware failures with ten remedies blaming the board.
  let portPath = null;
  let holdersSeen = [];
  const verdict = () => {
    const failed = rows.filter((row) => !row.pass).length;
    if (failed > 0 && portPath) {
      // One extra lsof, and only on the path that has already failed.
      const all = holdersSeen.slice();
      for (const pid of portHolders(portPath)) if (!all.includes(pid)) all.push(pid);
      if (all.length > 0) {
        say('');
        say(
          `NOTE  ${all.length} other process(es) held ${portPath} during this ` +
            `run: ${all.join(', ')}.`
        );
        say('      A /dev/cu.* port does not lock, so some of the failures');
        say('      above may be theirs and not a fault in the board. Quit them');
        say('      (`ps -p <pid>` names each one) and run this again.');
      }
    }
    say('');
    say(`${rows.length} checks, ${rows.length - failed} passed, ${failed} failed.`);
    if (failed === 0) {
      say('The board answered every line this program can send. It is good to give.');
    } else {
      say('Run `codex-companion doctor` for the setup side of the same question.');
    }
    return failed === 0 ? 0 : 1;
  };

  say('codex-companion selftest: driving a real board over the real cable.');
  say('');

  const pick = choosePort(opts);
  if (
    !record(
      'a board is on the cable',
      Boolean(pick.chosen),
      pick.chosen || 'no board',
      pick.reason
    )
  ) {
    return verdict();
  }

  let link = null;
  try {
    link = new Link(pick.chosen, { read: true }).open();
  } catch (e) {
    const advice = portOpenAdvice(e && e.code);
    record('the port opens for reading and writing', false, advice.what, advice.fix);
    return verdict();
  }
  record('the port opens for reading and writing', true, pick.chosen);
  portPath = pick.chosen;

  // Not a check, because it is not a fault in the board. macOS callout ports
  // do not lock, so a second reader is invisible to every errno this program
  // can see, and it takes a share of every answer below. Said once, before any
  // of the failures it would cause.
  const holders = portHolders(pick.chosen);
  holdersSeen = holders;
  if (holders.length > 0) {
    say('');
    say(
      `NOTE  ${holders.length} other process(es) also hold ${pick.chosen}: ` +
        `${holders.join(', ')}.`
    );
    say('      A /dev/cu.* port does not lock, so the answers below are being');
    say('      split between us and them. Any failure here may be theirs. Quit');
    say('      them (`ps -p <pid>` names each one) and run this again.');
    say('');
  }

  /**
   * Write these lines, then listen for a fixed window. Used where the check is
   * that something did NOT come back, which cannot be cut short.
   */
  const exchange = (lines, waitMs) => {
    for (const line of lines) link.writeLine(line, 500);
    return link.readFor(waitMs === undefined ? 500 : waitMs, true);
  };

  /**
   * Write these lines, then listen until `pattern` shows up or the deadline.
   *
   * A fixed window is the wrong shape for a board whose transmit path runs one
   * message behind: too short and a healthy board fails on a slow frame, too
   * long and eighteen checks take a minute. Waiting for the answer instead
   * makes a passing check fast and a failing one honest about how long it
   * really waited.
   */
  const expect = (lines, pattern, maxMs) => {
    for (const line of lines) link.writeLine(line, 500);
    const until = Date.now() + (maxMs === undefined ? 3000 : maxMs);
    let seen = '';
    while (Date.now() < until) {
      seen += link.readFor(120, true);
      if (pattern.test(seen)) break;
    }
    return seen;
  };
  /** The first answer line that starts with `prefix`, for the detail column. */
  const firstLine = (text, prefix) => {
    for (const line of String(text).split(/\r?\n/)) {
      if (line.trim().startsWith(prefix)) return line.trim();
    }
    return '';
  };

  try {
    // 1. It is talking at all.
    // Five seconds, not one: a board that was plugged in a moment ago spends
    // its first two on the boot screen and its next nine on the out-of-the-box
    // sequence, and neither is a fault. This is the only check that waits that
    // long, and only when there is nothing to hear.
    const hello = expect(['{}\n'], /hello\s+tdisplay-s3|\bok\b/, 5000);
    // Both tests once, so the detail column can name the third case. Computing
    // it off the greeting alone printed `ack` next to a FAIL when nothing at
    // all had come back, which is the first row a wrong --port ever shows.
    const sawGreeting = /hello\s+tdisplay-s3/i.test(hello);
    const sawAck = /\bok\b/.test(hello);
    const answered = record(
      'the board answers',
      sawGreeting || sawAck,
      sawGreeting ? 'greeting' : sawAck ? 'ack' : 'silence',
      'The port opened but nothing came back. The greeting is sent once a ' +
        'couple of seconds after boot and every accepted line is acked after ' +
        'it, so silence means this is not the companion firmware, or the ' +
        'board is wedged. Unplug it and plug it back in.'
    );
    // Everything below reads an answer back, so against something that is not
    // answering they would all fail for the same reason, slowly, and bury the
    // one finding that matters under fifteen copies of it.
    if (!answered) {
      say('');
      say('Nothing is answering on that port, so the rest was not attempted.');
      return verdict();
    }

    // 2. Every state this program can put on the wire, acked.
    //    Ends on idle deliberately: leaving a prompt pending would make every
    //    modal check below refuse, exactly as the firmware promises.
    const stateLines = ['sleep', 'idle', 'busy', 'waiting', 'done', 'idle'].map((state) =>
      frameToLine({
        state,
        ring: 0.5,
        center: '50%',
        label: 'CTX',
        sub: state === 'waiting' ? 'your turn' : 'self test'
      })
    );
    const stateOut = exchange(stateLines.concat(['{}\n', '{}\n']), 900);
    const acks = (stateOut.match(/^\s*ok\s*$/gm) || []).length;
    record(
      'all five states are accepted',
      acks >= stateLines.length,
      `${acks} acks for ${stateLines.length} frames`,
      'The board acks every line it accepts. Fewer acks than frames means it ' +
        'is rejecting frames this program built, which is a protocol mismatch ' +
        'between this file and the firmware on that board.'
    );

    // 2b. Close everything before any of the screen checks begin.
    //
    //     Every one of the modal screens below is entered idempotently: the
    //     firmware's `*Enter` functions return early when the screen is
    //     already up, and an early return prints no notice. So a board left
    //     with the toy or the sign up by an earlier command fails the check
    //     that opens it while passing the check that closes it, which reads
    //     like a fault in the firmware and is not one. Starting from a known
    //     shut board is the difference between a test and a coin toss.
    exchange(
      [
        frameToLine({ say: '' }),
        frameToLine({ dnd: false }),
        frameToLine({ stats: 'off' }),
        frameToLine({ focus: 'hide' }),
        frameToLine({ play: 'off' })
      ],
      400
    );

    // 3. A bad enum must take the WHOLE line with it. If the board applied one
    //    field of a line it should have dropped, the probe text reaches the
    //    panel and the board says so, which is the only way to see it from here.
    const badState = exchange([`{"say":"${REJECT_PROBE}A","state":"nope"}\n`], 600);
    record(
      'an unknown state drops the whole line',
      !badState.includes(`${REJECT_PROBE}A`),
      'nothing on that line was applied',
      'The board applied part of a line it should have rejected whole. Frames ' +
        'that mix a bad enum with good fields would half-apply, which is worse ' +
        'than failing.'
    );
    const badFace = exchange([`{"say":"${REJECT_PROBE}B","face":"nope"}\n`], 600);
    record(
      'an unknown face drops the whole line',
      !badFace.includes(`${REJECT_PROBE}B`),
      'nothing on that line was applied'
    );

    // 4. The message card, round trip. This is the one field where the board
    //    reports what actually reached the panel, so it proves the whole chain
    //    including the wrapping and the type size.
    //
    //    Held open rather than timed. `saysecs: 0` means "hold until
    //    something clears it", and the clear below is what takes it down.
    //    A `saysecs: 3` card cannot be used here: three seconds is also the
    //    default budget of `expect`, so on a board that answers slowly the
    //    card expires before the next check ever sends the empty message,
    //    `sayDismiss` returns early with nothing printed, and a healthy
    //    board fails the clear with an empty detail column and a remedy
    //    that says the panel is broken. A check may not outlive its own
    //    subject.
    const sayOut = expect([frameToLine({ say: 'self test', saysecs: 0 })], /say:\s*showing/);
    record(
      'a message reaches the panel',
      /say:\s*showing/.test(sayOut) && sayOut.includes('self test'),
      firstLine(sayOut, 'say:'),
      'The card is the one thing the board echoes back verbatim. No `say: ' +
        'showing` line means the text never made it to the screen.'
    );
    // `expired` is accepted as well as `cleared`, so that a card which
    // somehow went down on its own is reported as what it was rather than
    // as silence: a dismiss with no card up prints nothing at all.
    const clearOut = expect([frameToLine({ say: '' })], /say:\s*(cleared|expired)/);
    record(
      'an empty message clears the card',
      /say:\s*(cleared|expired)/.test(clearOut),
      firstLine(clearOut, 'say:'),
      'An empty message takes the card down and the board says `say: ' +
        'cleared`. Nothing at all means there was no card up by the time ' +
        'the empty message landed, because a dismiss with nothing to ' +
        'dismiss prints nothing.'
    );

    // 5. The clock, and the token counter that moves the daily figures.
    //
    //    Measured as a DIFFERENCE between two readings rather than as an
    //    absolute number, because the difference is the thing the firmware
    //    actually promises: the first total a board ever sees is a baseline
    //    that counts nothing, and any later one adds only what changed. A
    //    board that has been used all morning is already counting, so an
    //    absolute figure would be right only on a board nobody had touched,
    //    which is the one board this test would never be run on.
    //
    //    The step is small on purpose. It moves today's total by thirteen
    //    thousand tokens on a unit that is about to be given away, which
    //    cannot reach a milestone rung on its own.
    exchange(
      [frameToLine({ time: localUnixSeconds(Date.now()), tokens: TOKEN_PROBE_BASE })],
      300
    );
    const statsOn = expect([frameToLine({ stats: 'on' })], /stats:\s*(open|err)/);
    record(
      'the stats screen opens and carries its figures',
      /stats:\s*open \(host\)/.test(statsOn),
      firstLine(statsOn, 'stats:'),
      'The screen is refused while a prompt is pending, and this test leaves ' +
        'the board on `idle`, so a refusal here is a real fault rather than ' +
        'the documented one.'
    );
    record(
      'the clock reached the board',
      /day=today/.test(statsOn),
      /day=today/.test(statsOn) ? 'day=today' : 'day=since-boot',
      'The board is still counting from boot, so `time` never landed. Without ' +
        'it the daily figures never roll over at midnight and the screen says ' +
        'SINCE BOOT instead of TODAY.'
    );

    const before = tokensIn(statsOn);
    exchange([frameToLine({ stats: 'off' })], 300);
    exchange([frameToLine({ tokens: TOKEN_PROBE_BASE + TOKEN_PROBE_STEP })], 300);
    const statsAgain = expect([frameToLine({ stats: 'on' })], /stats:\s*open/);
    const after = tokensIn(statsAgain);
    record(
      'the token total banks the difference',
      before !== null && after !== null && after - before === TOKEN_PROBE_STEP,
      `${before === null ? '?' : before} -> ${after === null ? '?' : after}`,
      `Two totals were sent, ${TOKEN_PROBE_BASE} then ` +
        `${TOKEN_PROBE_BASE + TOKEN_PROBE_STEP}, so the day should have moved ` +
        `by exactly ${TOKEN_PROBE_STEP}. Moving by the whole second value ` +
        `instead would mean the board is banking totals rather than ` +
        `differences, and every number on that screen would be inflated.`
    );
    const statsOff = expect([frameToLine({ stats: 'off' })], /stats:\s*close/);
    record(
      'the stats screen closes',
      /stats:\s*close \(host\)/.test(statsOff),
      firstLine(statsOff, 'stats:')
    );

    // 6. The sign.
    const dndOn = expect([frameToLine({ dnd: true })], /dnd:\s*on/);
    record(
      'the do-not-disturb sign goes up',
      /dnd:\s*on \(host\)/.test(dndOn),
      firstLine(dndOn, 'dnd:')
    );
    const dndOff = expect([frameToLine({ dnd: false })], /dnd:\s*off/);
    record('and comes down again', /dnd:\s*off \(host\)/.test(dndOff), firstLine(dndOff, 'dnd:'));

    // 7. The focus timer's screen. The clock is left alone: starting one would
    //    leave a countdown running on a board somebody is about to be handed.
    const focusShow = expect([frameToLine({ focus: 'show', focusmins: 25 })], /focus:\s*show/);
    record(
      'the focus timer shows',
      /focus:\s*show \(host\)/.test(focusShow),
      firstLine(focusShow, 'focus:')
    );
    const focusHide = expect([frameToLine({ focus: 'hide' })], /focus:\s*hide/);
    record('and hides again', /focus:\s*hide \(host\)/.test(focusHide), firstLine(focusHide, 'focus:'));

    // 8. The toy, opened and closed from the wire.
    const playOn = expect([frameToLine({ play: 'on' })], /play:\s*hop/);
    record('the toy opens', /play:\s*hop \(host\)/.test(playOn), firstLine(playOn, 'play:'));
    const playOff = expect([frameToLine({ play: 'off' })], /play:\s*end/);
    record('and closes', /play:\s*end \(host\)/.test(playOff), firstLine(playOff, 'play:'));

    // 9. Put it back the way it was found, as far as anything here can.
    exchange(
      [
        frameToLine({ say: '' }),
        frameToLine({ dnd: false }),
        frameToLine({ stats: 'off' }),
        frameToLine({ focus: 'hide' }),
        frameToLine({ play: 'off' }),
        frameToLine({ state: 'idle', ring: 0, center: '--', label: 'CODEX', sub: '' })
      ],
      400
    );
  } finally {
    link.close();
  }

  say('');
  say('Nothing here was persisted. The board goes back to its ambient mode on');
  say('its own five minutes after the last line, or immediately on a replug.');
  return verdict();
}

// --------------------------------------------------------------------------
// 11. demo and run
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

/**
 * Ctrl-C handling for the two commands that loop.
 *
 * The first press asks the loop to finish what it is doing, which is how the
 * port gets closed and `stopped.` gets printed. A second press means the
 * person is now arguing with us, so it exits immediately. Listeners are
 * removed when the loop ends, because both of these are callable more than
 * once inside one process and an accumulating listener is a leak.
 */
function installStopHandler() {
  const state = { stopped: false };
  const onSignal = () => {
    if (state.stopped) process.exit(130);
    state.stopped = true;
  };
  process.on('SIGINT', onSignal);
  process.on('SIGTERM', onSignal);
  state.release = () => {
    process.removeListener('SIGINT', onSignal);
    process.removeListener('SIGTERM', onSignal);
  };
  return state;
}

async function cmdDemo(opts) {
  const link = opts.dryRun ? null : openLink(opts);
  if (!opts.dryRun && !link) return sayNoDevice(opts);
  const stop = installStopHandler();
  try {
    do {
      for (const frame of DEMO_FRAMES) {
        if (stop.stopped) break;
        const line = frameToLine(frame);
        say(line.trimEnd());
        if (link) link.writeFrame(frame, 200);
        await sleep(1200);
      }
    } while (opts.loop && !stop.stopped);
  } finally {
    stop.release();
    if (link) link.close();
  }
  return 0;
}

/** Reconnect backoff: start fast, give up quickly on hammering a dead bus. */
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 30000;

/**
 * Foreground metrics stream. Installs nothing; Ctrl-C ends it completely.
 *
 * Sends only the metric fields, never `state`: the board keeps whatever state
 * the hook last set, so running this alongside the hook composes instead of
 * fighting. On its own it gives you a live context ring with no hook at all.
 *
 * Survives the cable. Unplugging the board used to leave this writing into a
 * dead descriptor forever with no way back short of Ctrl-C; it now notices,
 * says so once, and retries on a backoff that doubles to half a minute. The
 * backoff is not politeness: every attempt runs `ioreg` over the whole USB
 * tree, so a one-second retry on an empty bus would be a permanent background
 * load on somebody's laptop.
 */
async function cmdRun(opts) {
  const home = opts.codexHome || codexHome(opts.env);
  let link = opts.dryRun ? null : openLink(opts);
  if (!opts.dryRun && !link) {
    if (!IS_MAC && !opts.port) {
      for (const chunk of wrapText(PLATFORM_NOTE, 72)) say(chunk);
      return 0;
    }
    return sayNoDevice(opts);
  }
  say(`watching ${path.join(home, 'sessions')}`);
  say(link ? `writing to ${link.path}` : 'dry run: not opening any port');
  say('Ctrl-C to stop. Nothing was installed.');

  const stop = installStopHandler();
  let lastLine = null;
  let lastSentAt = 0;
  let backoffMs = RECONNECT_MIN_MS;
  let retryAt = 0;
  let announcedLoss = false;

  try {
    while (!stop.stopped) {
      const now = Date.now();

      // Nothing on the far end: try to get it back, on a widening interval.
      if (!opts.dryRun && !link && now >= retryAt) {
        link = openLink(opts);
        if (link) {
          say(`device back: ${link.path}`);
          lastLine = null; // the board forgot everything, so resend it all
          backoffMs = RECONNECT_MIN_MS;
          announcedLoss = false;
        } else {
          retryAt = now + backoffMs;
          backoffMs = Math.min(backoffMs * 2, RECONNECT_MAX_MS);
        }
      }

      const newest = newestRolloutFile(path.join(home, 'sessions'));
      const metrics = newest ? readMetrics(newest.path, now) : null;
      const fields = metricFields(metrics, now);
      const line = frameToLine(fields);
      // Send on change, and at least every 2s so the board's 30s staleness
      // dimmer never trips while we are running.
      if (line !== lastLine || now - lastSentAt >= 2000) {
        if (opts.dryRun) {
          say(line.trimEnd());
        } else if (!link) {
          // Unplugged. Stay quiet rather than printing a frame per second at
          // somebody who already knows: the loss was announced once.
        } else if (!link.writeFrame(fields, 200)) {
          // A write that fails is the cable, not the data: drop the descriptor
          // and let the reconnect above own the problem.
          if (!announcedLoss) {
            say(`lost ${link.path}; retrying in the background, Ctrl-C to stop`);
            announcedLoss = true;
          }
          link.close();
          link = null;
          retryAt = now + backoffMs;
          backoffMs = Math.min(backoffMs * 2, RECONNECT_MAX_MS);
        }
        lastLine = line;
        lastSentAt = now;
      }
      await sleep(1000);
    }
  } finally {
    stop.release();
    if (link) link.close();
  }
  say('stopped.');
  return 0;
}

/** Deliberately not unref'd: this timer is what keeps the loop running. */
function sleep(ms) {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

// --------------------------------------------------------------------------
// 12. CLI
// --------------------------------------------------------------------------

const USAGE = `codex-companion - desk companion for the Codex CLI

  codex-companion                 stream context metrics in the foreground
  codex-companion run             the same thing, named
  codex-companion hook            hook entry point: reads one payload on stdin
  codex-companion install-hook    print, then merge our handlers into hooks.json
  codex-companion uninstall-hook  remove exactly those handlers again
  codex-companion doctor          device, codex, hook registration, config traps
  codex-companion selftest        drive a real board and check every answer
  codex-companion demo            drive every state, no Codex involved
  codex-companion help            this text

Talking to the board directly (none of these need Codex at all):

  codex-companion say "back in five" [--secs 300]   a message anyone can read
  codex-companion say --clear                       take it down
  codex-companion name "Alex Rivera"                stored on the board
  codex-companion face rounded|bear|arc             stored on the board
  codex-companion time [tokens]                     give the daily stats a midnight
  codex-companion dnd on|off                        the desk sign
  codex-companion stats on|off                      the numbers screen
  codex-companion focus show|hide|start|pause|reset [--mins 25]
  codex-companion play on|off|hop                   the toy
  codex-companion firstrun                          replay the out-of-the-box run

Options:
  --port /dev/cu.usbmodemXXX   use this port instead of auto-detecting
  --codex-home /path           look here instead of $CODEX_HOME or ~/.codex
  --dry-run                    print what would happen, change nothing
  --loop                       (demo) repeat until Ctrl-C
  --no-metrics                 (hook) send state only, do not read the rollout
  --secs N                     (say) hold for N seconds; 0 until cleared
  --mins N                     (focus) the timer's length, 1 to 180
  --clear                      (say, name) send the empty value
  --strict                     (doctor) exit non-zero when something is broken

There is deliberately no factory-reset command. It wipes the owner name, the
face and the records, and a thing that destructive should cost more than a
typo: printf '{"reset":"factory"}' > /dev/cu.usbmodemXXX

Nothing here installs a background service. There is no autostart. The hook
never approves or denies anything; approval prompts stay in your terminal.
`;

/** Flags that take a value, so an option and its argument stay together. */
function parseArgs(argv) {
  const opts = { env: process.env };
  const rest = [];
  const list = Array.isArray(argv) ? argv : [];
  for (let i = 0; i < list.length; i += 1) {
    const a = list[i];
    if (a === '--port') opts.port = list[++i];
    else if (a === '--codex-home') opts.codexHome = list[++i];
    // NOT clamped to 0 here. Zero is the "hold until cleared" sentinel, so
    // clamping a negative into it would turn a typo into a card that never
    // leaves. It is passed through as given and frameToLine maps anything
    // below zero to the default, which is the firmware's own rule.
    else if (a === '--secs') opts.secs = clampInt(list[++i], -1, SAY_MAX_SECS);
    else if (a === '--mins') opts.mins = clampInt(list[++i], FOCUS_MIN_MINUTES, FOCUS_MAX_MINUTES);
    else if (a === '--dry-run') opts.dryRun = true;
    else if (a === '--loop') opts.loop = true;
    else if (a === '--no-metrics') opts.noMetrics = true;
    else if (a === '--clear') opts.clear = true;
    else if (a === '--strict') opts.strict = true;
    else if (a === '-h' || a === '--help') opts.help = true;
    else rest.push(a);
  }
  // A flag with a missing value ate `undefined`; treat it as never given
  // rather than as the string "undefined" further down.
  if (opts.port === undefined) delete opts.port;
  if (opts.codexHome === undefined) delete opts.codexHome;
  if (opts.secs === null) delete opts.secs;
  if (opts.mins === null) delete opts.mins;
  opts.command = rest[0] || 'run';
  opts.args = rest.slice(1);
  return opts;
}

/** Everything user-facing goes to stdout here, except inside `hook`. */
function say(s) {
  try {
    process.stdout.write(`${s}\n`);
  } catch {
    /* the reader went away (`| head`), which is not our problem */
  }
}

const COMMANDS = Object.assign(Object.create(null), {
  hook: cmdHook,
  'install-hook': cmdInstallHook,
  'uninstall-hook': cmdUninstallHook,
  doctor: cmdDoctor,
  selftest: cmdSelftest,
  demo: cmdDemo,
  run: cmdRun,
  say: cmdSay,
  name: cmdName,
  face: cmdFace,
  time: cmdTime,
  dnd: cmdDnd,
  stats: cmdStats,
  focus: cmdFocus,
  play: cmdPlay,
  firstrun: cmdFirstrun
});

async function main(argv) {
  const opts = parseArgs(argv || []);
  if (opts.help || opts.command === 'help') {
    process.stdout.write(USAGE);
    return 0;
  }
  const fn = typeof COMMANDS[opts.command] === 'function' ? COMMANDS[opts.command] : null;
  if (!fn) {
    process.stdout.write(USAGE);
    return 1;
  }
  return fn(opts);
}

if (require.main === module) {
  // `codex-companion doctor | head` closes the pipe under us; that is normal.
  process.stdout.on('error', () => {});
  const isHook = parseArgs(process.argv.slice(2)).command === 'hook';
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
  portOpenAdvice,
  portHolders,
  probePort,
  Link,
  // frames
  metricFields,
  frameForEvent,
  frameToLine,
  formatElapsed,
  pickQuotaWindow,
  localUnixSeconds,
  sanitizeAscii,
  asciiNotes,
  clampBytes,
  pickEnum,
  clampInt,
  stateForEvent,
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
  shellQuote,
  hookHandler,
  stableNodePath,
  isOurHandler,
  mergeHooks,
  unmergeHooks,
  registeredEvents,
  hookCommandFlags,
  hooksFileProblem,
  // doctor
  scanConfigToml,
  silencesApprovals,
  wouldSilenceApprovals,
  scanHookState,
  commandParts,
  wrapText,
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
  BASELINE_TOKENS,
  NAME_MAX,
  SAY_MAX,
  SAY_MAX_SECS,
  SAY_DEFAULT_SECS,
  FOCUS_MIN_MINUTES,
  FOCUS_MAX_MINUTES,
  CLOCK_MIN_EPOCH,
  FACE_NAMES,
  PLAY_VERBS,
  STATS_VERBS,
  FOCUS_VERBS,
  RESET_VERBS,
  FIRSTRUN_VERBS
};
