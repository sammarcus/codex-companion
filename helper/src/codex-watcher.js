'use strict';

/**
 * codex-watcher.js - derive live agent state by tailing ~/.codex/sessions.
 *
 * Everything here is grounded in docs/codex-state-format.md, which was written
 * against codex-cli 0.153.4. The important constraints from that document:
 *
 *  - Every rollout record is one write_all(json + "\n") followed by flush(), so
 *    a naive line tail is safe. The first line can still be ~21 KB.
 *  - Turn boundaries are event_msg payloads "task_started" / "task_complete"
 *    (aliases "turn_started" / "turn_complete"). Match both spellings.
 *  - Approval requests are classified non-durable and are NEVER written to the
 *    rollout. We parse them anyway if they ever show up (they appear in the
 *    synthetic fixture), and otherwise fall back to a clearly-labelled stall
 *    heuristic, which is only armed when approval_policy != "never".
 *  - Long turns emit nothing at all: no exec begin/end, no item_started, no
 *    deltas. Silence is not evidence of being stuck.
 *  - Context fill uses Codex's own formula with BASELINE_TOKENS = 12000, not
 *    used/window, and it is fed the LIVE context size, never the accumulated
 *    session total. tui/src/token_usage.rs:37-38 spells the difference out:
 *    "For `last_token_usage`, this is the latest active context size; for
 *    `total_token_usage`, this is the accumulated session total." All three
 *    Codex call sites pass last_token_usage (tui/src/chatwidget.rs:1149,
 *    tui/src/status/card.rs:360, tui/src/chatwidget/status_controls.rs:421).
 *    The session total grows every response, so driving the ring from it pins
 *    it at a static 100%.
 */

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { EventEmitter } = require('node:events');

/** protocol.rs:2394 - Codex subtracts this baseline from both sides. */
const BASELINE_TOKENS = 12000;

const DEFAULTS = {
  /** how often we re-derive state and check the active file for growth */
  pollMs: 250,
  /** how often we re-pick which session file is newest */
  rescanMs: 2000,
  /** lines read from the end of a file on startup, to seed state */
  seedLines: 200,
  /** hard cap on the tail we read while seeding (world_state lines are ~20 KB) */
  seedMaxBytes: 4 * 1024 * 1024,
  /** how long "done" is held after a turn completes */
  doneMs: 5000,
  /** no events for this long, with no turn open -> idle */
  idleMs: 30 * 1000,
  /** no events for this long, with no turn open -> sleep */
  sleepMs: 5 * 60 * 1000,
  /**
   * Open turn silent for this long -> maybe waiting on a human (heuristic).
   *
   * docs/codex-state-format.md section 6 sets the floor: "STALL_S should be
   * generous (60-120 s). Long model turns and long tool calls emit nothing to
   * the JSONL ... Silence is not evidence of being stuck." exec_command_begin,
   * exec_command_end, item_started and every *_delta are classified transient
   * by rollout/src/policy.rs, so an npm install, a test run or a long reasoning
   * turn writes zero durable lines for its whole duration. 90s sits in the
   * middle of the documented band and survives a typical build, so the amber
   * pulse stays the thing it is for: the agent actually blocked on a human.
   */
  stallMs: 90 * 1000,
  /** open turn silent for this long -> the turn is stale, stop claiming busy */
  staleTurnMs: 15 * 60 * 1000,
  /**
   * A durable line this much newer than a pending approval means the human
   * already answered it. Codex never writes the decision itself (approvals are
   * classified transient by rollout/src/policy.rs), so continued activity on
   * the same turn is the only evidence available. The grace window keeps a line
   * written in the same instant as the request from clearing it immediately.
   */
  approvalGraceMs: 2000,
  /** a pending approval this old stops counting as an observed fact */
  approvalMaxAgeMs: 10 * 60 * 1000,
  /** arm the approval stall heuristic (suppressed when policy is "never") */
  approvalHeuristic: true
};

/** Resolve $CODEX_HOME, honouring the env override Codex itself uses. */
function codexHome(env) {
  const e = env || process.env;
  if (e.CODEX_HOME && e.CODEX_HOME.trim()) return e.CODEX_HOME;
  return path.join(os.homedir(), '.codex');
}

/**
 * Codex's own context-fill formula (protocol.rs:2428,
 * percent_of_context_window_remaining), inverted to a 0..1 fill.
 * Returns null when it cannot be computed.
 */
function contextFill(totalTokens, windowSize) {
  if (!Number.isFinite(totalTokens) || !Number.isFinite(windowSize)) return null;
  if (windowSize <= BASELINE_TOKENS) return null;
  const effective = windowSize - BASELINE_TOKENS;
  const usedEff = Math.max(totalTokens - BASELINE_TOKENS, 0);
  const remaining = Math.min(Math.max((effective - usedEff) / effective, 0), 1);
  const remainingPct = Math.round(remaining * 100);
  return (100 - remainingPct) / 100;
}

/** Parse a rollout line. Returns null for blank/garbage/partial lines. */
function parseLine(line) {
  const t = line.trim();
  if (!t || t[0] !== '{') return null;
  let obj;
  try {
    obj = JSON.parse(t);
  } catch {
    return null;
  }
  if (!obj || typeof obj !== 'object') return null;
  return obj;
}

/** Envelope timestamp -> ms epoch, or null. */
function envelopeMs(obj) {
  if (!obj || typeof obj.timestamp !== 'string') return null;
  const ms = Date.parse(obj.timestamp);
  return Number.isFinite(ms) ? ms : null;
}

const TURN_STARTED = new Set(['task_started', 'turn_started']);
const TURN_COMPLETE = new Set(['task_complete', 'turn_complete']);
const APPROVAL_REQUEST = new Set([
  'exec_approval_request',
  'apply_patch_approval_request',
  'request_permissions',
  'request_user_input',
  'elicitation_request'
]);

/**
 * Pull the text out of an AgentMessage turn item.
 *
 * items.rs:150-152 defines `AgentMessageItem { id, content: Vec<AgentMessageContent>, .. }`
 * and items.rs:124-126 defines `AgentMessageContent { Text { text: String } }`.
 * There is no `text` field on the item itself. Returns null when the item
 * carries nothing readable.
 */
function agentMessageText(item) {
  if (!item || !Array.isArray(item.content)) return null;
  const parts = [];
  for (const c of item.content) {
    if (c && typeof c.text === 'string' && c.text.length) parts.push(c.text);
  }
  return parts.length ? parts.join('') : null;
}

/**
 * Accumulates rollout lines into a derived-state struct.
 * Pure with respect to time: call `derive(now)` to get a snapshot.
 */
class SessionState {
  constructor(opts) {
    this.opts = Object.assign({}, DEFAULTS, opts || {});
    this.reset();
  }

  reset() {
    this.sessionId = null;
    this.cliVersion = null;
    this.model = null;
    this.approvalPolicy = null;
    this.historyMode = null;
    this.contextWindow = null;

    this.openTurnId = null;
    this.turnStartedAtMs = null;
    this.lastTurnCompletedAtMs = null;
    this.lastTurnDurationSec = null;
    this.lastTurnAborted = null;
    this.lastAgentMessage = null;
    this.lastError = null;

    this.tokensIn = null;
    this.tokensOut = null;
    this.totalTokens = null;
    /**
     * Live context size, i.e. what the ring shows. Separate from totalTokens
     * on purpose: totalTokens is the cumulative session accounting and grows
     * without bound, this one is what is actually resident in the window.
     */
    this.contextTokens = null;

    this.rateLimits = null;

    this.tps = null;
    this._lastUsageAtMs = null;
    this._lastUsageOut = null;

    this.pendingApprovals = new Map();

    this.lastEventMs = null;
    this.lastLineMs = null;
    this.sawAnyLine = false;
    this.sawSessionMeta = false;
    this._turnStartTokensOut = null;
  }

  /**
   * Feed one parsed rollout object.
   * @param {object} obj
   * @param {number} nowMs used when the record carries no usable timestamp
   */
  apply(obj, nowMs) {
    if (!obj || typeof obj.type !== 'string') return;
    this.sawAnyLine = true;

    const ts = envelopeMs(obj);
    const at = ts !== null ? ts : nowMs;
    this.lastLineMs = at;

    const p = obj.payload;
    if (!p || typeof p !== 'object') return;

    // Any durable line that is not itself an approval request is evidence the
    // turn moved on, which is the only signal we get that a human answered.
    if (!(obj.type === 'event_msg' && APPROVAL_REQUEST.has(p.type))) {
      this._clearAnsweredApprovals(at);
    }

    switch (obj.type) {
      case 'session_meta':
        this.sawSessionMeta = true;
        // session_id was added later; the deserializer back-fills it from `id`.
        this.sessionId = p.session_id || p.id || this.sessionId;
        this.cliVersion = p.cli_version || this.cliVersion;
        this.historyMode = p.history_mode || this.historyMode;
        this.lastEventMs = at;
        break;

      case 'turn_context':
        // Emitted more than once per turn: latest wins, never a counter.
        if (p.model) this.model = p.model;
        if (typeof p.approval_policy === 'string') this.approvalPolicy = p.approval_policy;
        this.lastEventMs = at;
        break;

      case 'token_usage_record':
        this._applyUsageRecord(p, at);
        this.lastEventMs = at;
        break;

      case 'compacted':
        // Token totals survive compaction via the embedded snapshot.
        if (p.latest_token_usage_record) {
          this._applyUsageRecord(p.latest_token_usage_record, at);
        }
        this.lastEventMs = at;
        break;

      case 'event_msg':
        this._applyEvent(p, at);
        break;

      default:
        // response_item, world_state, retained_context, security_risk_score,
        // inter_agent_communication*, realtime_item: activity, no state change.
        this.lastEventMs = at;
        break;
    }
  }

  _applyUsageRecord(p, at) {
    // protocol.rs:2237-2247. `usage` is "usage observed for one completed
    // response", the analogue of last_token_usage: the live context size.
    // turn_token_usage and thread_token_usage are running totals.
    const usage = p.usage;
    const cumulative = p.thread_token_usage || p.turn_token_usage || usage;
    if (cumulative && typeof cumulative === 'object') {
      if (Number.isFinite(cumulative.input_tokens)) this.tokensIn = cumulative.input_tokens;
      if (Number.isFinite(cumulative.output_tokens)) this.tokensOut = cumulative.output_tokens;
      if (Number.isFinite(cumulative.total_tokens)) this.totalTokens = cumulative.total_tokens;
    }
    if (usage && typeof usage === 'object' && Number.isFinite(usage.total_tokens)) {
      this.contextTokens = usage.total_tokens;
    }
    // Derived tokens/sec: output tokens of this response over the wall time
    // since the previous record. An interval average, not an instant rate, and
    // it includes time spent inside tool calls.
    if (usage && Number.isFinite(usage.output_tokens)) {
      if (this._lastUsageAtMs !== null && at > this._lastUsageAtMs) {
        const dt = (at - this._lastUsageAtMs) / 1000;
        if (dt > 0.05) {
          const rate = usage.output_tokens / dt;
          if (Number.isFinite(rate) && rate >= 0) this.tps = rate;
        }
      }
      this._lastUsageAtMs = at;
      this._lastUsageOut = usage.output_tokens;
    }
  }

  _applyEvent(p, at) {
    const kind = p.type;
    if (typeof kind !== 'string') return;

    if (TURN_STARTED.has(kind)) {
      this.openTurnId = p.turn_id || 'turn';
      this.turnStartedAtMs = Number.isFinite(p.started_at) ? p.started_at * 1000 : at;
      this.lastTurnCompletedAtMs = null;
      this.lastTurnAborted = null;
      this.lastError = null;
      // Earliest and most reliable place to read the window size.
      if (Number.isFinite(p.model_context_window)) this.contextWindow = p.model_context_window;
      this.pendingApprovals.clear();
      this.tps = null;
      this._lastUsageAtMs = null;
      // Baseline for turn-level tokens/sec. thread_token_usage is cumulative,
      // so the turn's own output is the delta across the turn. A null baseline
      // is only safe to read as zero when we saw this thread's session_meta,
      // i.e. we are not joining a session that was already under way.
      this._turnStartTokensOut =
        this.tokensOut !== null ? this.tokensOut : this.sawSessionMeta ? 0 : null;
      this.lastEventMs = at;
      return;
    }

    if (TURN_COMPLETE.has(kind)) {
      this._closeTurn(p, at);
      this.lastAgentMessage = p.last_agent_message || null;
      this.lastError = p.error && p.error.message ? String(p.error.message) : null;
      // Turn-level average from the cumulative-output delta across the turn.
      // TurnCompleteEvent (protocol.rs:2140-2168) carries turn_id,
      // last_agent_message, error, started_at, completed_at, duration_ms and
      // time_to_first_token_ms, and nothing else: there is no turn usage on it.
      const secs = Number.isFinite(this.lastTurnDurationSec) ? this.lastTurnDurationSec : null;
      if (secs !== null && secs > 0) {
        let turnOut = null;
        if (this._turnStartTokensOut !== null && Number.isFinite(this.tokensOut)) {
          turnOut = Math.max(this.tokensOut - this._turnStartTokensOut, 0);
        }
        if (turnOut !== null && turnOut > 0) this.tps = turnOut / secs;
      }
      this._turnStartTokensOut = null;
      this.lastEventMs = at;
      return;
    }

    if (kind === 'turn_aborted') {
      // TurnAbortedEvent (protocol/src/protocol.rs:4154) carries the same
      // started_at / completed_at / duration_ms as TurnCompleteEvent, so the
      // only thing separating a Ctrl-C from a success is this flag. Without it
      // an interrupt lights the green done flash on the recipient's desk.
      this._closeTurn(p, at, true);
      this.lastTurnAborted = typeof p.reason === 'string' ? p.reason : 'aborted';
      this.lastEventMs = at;
      return;
    }

    if (kind === 'token_count') {
      const info = p.info;
      if (info && typeof info === 'object') {
        const total = info.total_token_usage;
        if (total && typeof total === 'object') {
          if (Number.isFinite(total.input_tokens)) this.tokensIn = total.input_tokens;
          if (Number.isFinite(total.output_tokens)) this.tokensOut = total.output_tokens;
          if (Number.isFinite(total.total_tokens)) this.totalTokens = total.total_tokens;
        }
        // The ring's number: the latest active context size, not the session total.
        const last = info.last_token_usage;
        if (last && typeof last === 'object' && Number.isFinite(last.total_tokens)) {
          this.contextTokens = last.total_tokens;
        }
        if (Number.isFinite(info.model_context_window)) {
          this.contextWindow = info.model_context_window;
        }
      }
      if (p.rate_limits && typeof p.rate_limits === 'object') {
        this.rateLimits = Object.assign({}, p.rate_limits, { observedAtMs: at });
      }
      this.lastEventMs = at;
      return;
    }

    if (APPROVAL_REQUEST.has(kind)) {
      // Not persisted to the rollout by any Codex version we know of, but if a
      // future version or another feed provides it, believe it over the heuristic.
      const id = p.approval_id || p.call_id || `${kind}:${at}`;
      this.pendingApprovals.set(id, { kind, at, turnId: p.turn_id || null });
      this.lastEventMs = at;
      return;
    }

    if (kind === 'item_completed') {
      const item = p.item;
      if (item && item.type === 'AgentMessage') {
        const text = agentMessageText(item);
        if (text !== null) this.lastAgentMessage = text;
      }
      this.lastEventMs = at;
      return;
    }

    this.lastEventMs = at;
  }

  /**
   * Drop pending approvals that a later durable line has overtaken. Called for
   * every record that is not itself an approval request.
   */
  _clearAnsweredApprovals(at) {
    if (this.pendingApprovals.size === 0) return;
    const cutoff = at - this.opts.approvalGraceMs;
    for (const [id, entry] of this.pendingApprovals) {
      if (entry.at <= cutoff) this.pendingApprovals.delete(id);
    }
  }

  /**
   * Close the open turn.
   *
   * @param {object} p     the event payload
   * @param {number} at    envelope time, used when the payload has no clock
   * @param {boolean} [aborted] true for turn_aborted
   *
   * `lastTurnCompletedAtMs` is what derive() reads as "the green done flash",
   * so only a turn that actually completed may set it. An aborted turn keeps
   * its duration (the stopwatch is still true) and falls through to idle: the
   * turn is over and nothing succeeded.
   */
  _closeTurn(p, at, aborted) {
    this.openTurnId = null;
    this.lastTurnCompletedAtMs = aborted
      ? null
      : Number.isFinite(p.completed_at)
        ? p.completed_at * 1000
        : at;
    if (Number.isFinite(p.duration_ms)) {
      this.lastTurnDurationSec = p.duration_ms / 1000;
    } else if (Number.isFinite(p.started_at) && Number.isFinite(p.completed_at)) {
      this.lastTurnDurationSec = p.completed_at - p.started_at;
    }
    this.pendingApprovals.clear();
  }

  /**
   * Derive the display snapshot.
   * @param {number} nowMs
   * @param {boolean} haveFile false when no session file exists at all
   */
  derive(nowMs, haveFile) {
    const o = this.opts;
    let ctxFill = contextFill(this.contextTokens, this.contextWindow);

    const lastActivityMs = this.lastEventMs !== null ? this.lastEventMs : this.lastLineMs;
    const age = lastActivityMs === null ? Infinity : Math.max(0, nowMs - lastActivityMs);

    // An approval nobody ever answered stops counting as an observed fact, so
    // it cannot pin the amber ring for the rest of the turn.
    let livePending = 0;
    for (const entry of this.pendingApprovals.values()) {
      if (nowMs - entry.at <= o.approvalMaxAgeMs) livePending += 1;
    }

    let state;
    let heuristic = false;

    if (!haveFile || !this.sawAnyLine) {
      state = 'sleep';
    } else if (this.openTurnId) {
      if (age > o.staleTurnMs) {
        // The turn never closed and the file went quiet for a very long time.
        // Claiming "busy" here would be a lie we cannot support.
        state = 'idle';
      } else if (livePending > 0) {
        state = 'waiting';
      } else if (
        o.approvalHeuristic &&
        age > o.stallMs &&
        this.approvalPolicy !== null &&
        this.approvalPolicy !== 'never'
      ) {
        state = 'waiting';
        heuristic = true;
      } else {
        state = 'busy';
      }
    } else if (
      this.lastTurnCompletedAtMs !== null &&
      nowMs - this.lastTurnCompletedAtMs < o.doneMs
    ) {
      state = 'done';
    } else if (age >= o.sleepMs) {
      state = 'sleep';
    } else {
      state = 'idle';
    }

    // A sleeping board must not display the previous turn's frozen stopwatch,
    // and for the same reason it must not keep yesterday's context ring: after
    // five silent minutes the percentage is a number the helper can no longer
    // vouch for. Blank both and let the frame fall back to "--".
    if (state === 'sleep') ctxFill = null;

    let elapsedSec = null;
    if (state !== 'sleep') {
      if (this.openTurnId && Number.isFinite(this.turnStartedAtMs)) {
        elapsedSec = Math.max(0, (nowMs - this.turnStartedAtMs) / 1000);
      } else if (Number.isFinite(this.lastTurnDurationSec)) {
        elapsedSec = this.lastTurnDurationSec;
      }
    }

    return {
      state,
      heuristic,
      ctxFill,
      contextTokens: this.contextTokens,
      tokensIn: this.tokensIn,
      tokensOut: this.tokensOut,
      totalTokens: this.totalTokens,
      contextWindow: this.contextWindow,
      tps: this.tps,
      elapsedSec,
      rateLimits: this.rateLimits,
      sessionId: this.sessionId,
      model: this.model,
      approvalPolicy: this.approvalPolicy,
      cliVersion: this.cliVersion,
      lastAgentMessage: this.lastAgentMessage,
      lastError: this.lastError,
      abortedReason: this.lastTurnAborted,
      pendingApprovals: livePending,
      ageMs: age === Infinity ? null : age
    };
  }
}

/** Recursively list *.jsonl under a directory. Missing dir -> []. */
async function listSessionFiles(dir, out, depth) {
  out = out || [];
  depth = depth || 0;
  if (depth > 6) return out;
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch {
    return out;
  }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) {
      await listSessionFiles(full, out, depth + 1);
    } else if (e.isFile() && e.name.endsWith('.jsonl') && e.name.startsWith('rollout-')) {
      out.push(full);
    }
  }
  return out;
}

/** Newest *.jsonl by mtime under sessionsDir, or null. */
async function newestSessionFile(sessionsDir) {
  const files = await listSessionFiles(sessionsDir);
  let best = null;
  for (const f of files) {
    let st;
    try {
      st = await fsp.stat(f);
    } catch {
      continue;
    }
    if (!best || st.mtimeMs > best.mtimeMs) best = { path: f, mtimeMs: st.mtimeMs, size: st.size };
  }
  return best;
}

/**
 * Read the last `maxLines` complete lines of a file without loading all of it.
 * Returns { lines, size } where size is the byte offset the tail should resume at.
 */
async function readTailLines(file, maxLines, maxBytes) {
  let fh;
  try {
    fh = await fsp.open(file, 'r');
  } catch {
    return { lines: [], size: 0 };
  }
  try {
    const st = await fh.stat();
    const size = st.size;
    const start = Math.max(0, size - maxBytes);
    const len = size - start;
    if (len <= 0) return { lines: [], size };
    const buf = Buffer.alloc(len);
    const { bytesRead } = await fh.read(buf, 0, len, start);
    let text = buf.subarray(0, bytesRead).toString('utf8');
    // If we clipped mid-file, drop the (probably partial) first line.
    if (start > 0) {
      const nl = text.indexOf('\n');
      text = nl === -1 ? '' : text.slice(nl + 1);
    }
    const parts = text.split('\n');
    // Drop the final element unconditionally: it is either the empty string
    // after a trailing newline, or an incomplete last line the tailer will pick
    // up once the writer finishes it. Both cases want it gone.
    parts.pop();
    const lines = parts.slice(-maxLines);
    return { lines, size };
  } catch {
    return { lines: [], size: 0 };
  } finally {
    await fh.close().catch(() => {});
  }
}

/**
 * CodexWatcher - emits derived state from the newest Codex rollout file.
 *
 * Events:
 *   'state'  (snapshot)  whenever the derived snapshot changes
 *   'tick'   (snapshot)  every poll, changed or not
 *   'file'   (path|null) when the watched file changes
 *   'error'  (err)       non-fatal; the watcher keeps polling
 */
class CodexWatcher extends EventEmitter {
  constructor(opts) {
    super();
    const o = Object.assign({}, DEFAULTS, opts || {});
    this.opts = o;
    this.codexHome = o.codexHome || codexHome(o.env);
    this.sessionsDir = o.sessionsDir || path.join(this.codexHome, 'sessions');
    this.now = typeof o.now === 'function' ? o.now : () => Date.now();

    this.session = new SessionState(o);
    this.currentFile = null;
    this.position = 0;
    this.partial = '';
    this.haveFile = false;

    this._pollTimer = null;
    this._rescanTimer = null;
    this._fsWatcher = null;
    this._lastSnapshot = null;
    this._busy = false;
    this._rescanQueued = true;
    this._lastRescanMs = -Infinity;
    this._stopped = true;
  }

  async start() {
    this._stopped = false;
    await this.rescan();
    await this.poll();
    this._pollTimer = setInterval(() => {
      this.poll().catch((e) => this.emit('error', e));
    }, this.opts.pollMs);
    this._rescanTimer = setInterval(() => {
      this._rescanQueued = true;
    }, this.opts.rescanMs);
    if (this._pollTimer.unref) this._pollTimer.unref();
    if (this._rescanTimer.unref) this._rescanTimer.unref();
    this._startFsWatch();
    return this;
  }

  stop() {
    this._stopped = true;
    if (this._pollTimer) clearInterval(this._pollTimer);
    if (this._rescanTimer) clearInterval(this._rescanTimer);
    this._pollTimer = null;
    this._rescanTimer = null;
    if (this._fsWatcher) {
      try {
        this._fsWatcher.close();
      } catch {
        /* already gone */
      }
      this._fsWatcher = null;
    }
  }

  _startFsWatch() {
    // fs.watch on a missing dir throws; the rescan timer covers that case.
    try {
      this._fsWatcher = fs.watch(this.sessionsDir, { recursive: true }, (_event, name) => {
        // Every append to the rollout we are already tailing fires here, and a
        // rescan stats every rollout file under sessions/ (a dogfooding user
        // accumulates thousands). Queuing on our own file collapsed rescanMs
        // to the 250 ms poll rate and burned ~24% of a core while Codex wrote.
        // The tailer already sees those bytes via the size check in _readNew.
        if (this._isCurrentFileEvent(name)) return;
        this._rescanQueued = true;
      });
      this._fsWatcher.on('error', () => {
        // Directory removed or watch limit hit: the poll loop still works.
        try {
          this._fsWatcher.close();
        } catch {
          /* ignore */
        }
        this._fsWatcher = null;
      });
    } catch {
      this._fsWatcher = null;
    }
  }

  /**
   * True when an fs.watch event names the rollout file we are already tailing.
   * fs.watch reports a path relative to sessionsDir, so compare basenames;
   * rollout names carry a uuid and do not collide across day directories.
   * A null filename (possible on some platforms) is treated as "not ours",
   * because a missed rotation costs more than one extra rescan.
   */
  _isCurrentFileEvent(name) {
    if (!this.currentFile || typeof name !== 'string' || !name) return false;
    return path.basename(name) === path.basename(this.currentFile);
  }

  /**
   * Rescans stat every rollout file under sessions/, which on a dogfooding
   * user's tree is thousands of files and ~120 ms a call. rescanMs is the
   * budget for that; nothing may spend it faster. The one exception is having
   * no file to tail at all, where finding one promptly is the whole job.
   */
  _rescanDue(nowMs) {
    if (this.currentFile === null) return true;
    return nowMs - this._lastRescanMs >= this.opts.rescanMs;
  }

  /** Re-pick the newest session file; re-seed if it changed. */
  async rescan() {
    this._rescanQueued = false;
    this._lastRescanMs = this.now();
    const best = await newestSessionFile(this.sessionsDir);
    if (!best) {
      if (this.currentFile !== null) {
        this.currentFile = null;
        this.position = 0;
        this.partial = '';
        this.emit('file', null);
      }
      this.haveFile = false;
      if (!this._fsWatcher) this._startFsWatch();
      return;
    }
    this.haveFile = true;
    if (best.path !== this.currentFile) {
      await this._adoptFile(best.path);
    }
  }

  async _adoptFile(file) {
    this.currentFile = file;
    this.session.reset();
    this.partial = '';
    const { lines, size } = await readTailLines(
      file,
      this.opts.seedLines,
      this.opts.seedMaxBytes
    );
    const now = this.now();
    for (const line of lines) {
      const obj = parseLine(line);
      if (obj) this.session.apply(obj, now);
    }
    this.position = size;
    this.emit('file', file);
  }

  /** One poll cycle: rescan if queued, read new bytes, derive, emit. */
  async poll() {
    if (this._busy) return this._lastSnapshot;
    this._busy = true;
    try {
      // Keep the flag set when the floor blocks: the rescan is deferred, not
      // dropped, so a rotation still lands within rescanMs.
      if (this._rescanQueued && this._rescanDue(this.now())) await this.rescan();
      if (this.currentFile) await this._readNew();
      return this._emitState();
    } finally {
      this._busy = false;
    }
  }

  async _readNew() {
    let st;
    try {
      st = await fsp.stat(this.currentFile);
    } catch {
      // File vanished (rotated away, session archived). Look for another.
      this.currentFile = null;
      this.position = 0;
      this.partial = '';
      this.haveFile = false;
      this._rescanQueued = true;
      return;
    }
    if (st.size < this.position) {
      // Truncated or replaced in place: re-seed from the top.
      this.position = 0;
      this.partial = '';
      this.session.reset();
    }
    if (st.size === this.position) return;

    let fh;
    try {
      fh = await fsp.open(this.currentFile, 'r');
    } catch (e) {
      this.emit('error', e);
      return;
    }
    try {
      const len = st.size - this.position;
      const buf = Buffer.alloc(len);
      const { bytesRead } = await fh.read(buf, 0, len, this.position);
      this.position += bytesRead;
      const chunk = this.partial + buf.subarray(0, bytesRead).toString('utf8');
      const parts = chunk.split('\n');
      // Last element is whatever came after the final newline: hold it back
      // until the writer finishes the line.
      this.partial = parts.pop();
      const now = this.now();
      for (const line of parts) {
        const obj = parseLine(line);
        if (obj) this.session.apply(obj, now);
      }
    } catch (e) {
      this.emit('error', e);
    } finally {
      await fh.close().catch(() => {});
    }
  }

  /** Current derived snapshot without touching the filesystem. */
  getState() {
    return this.session.derive(this.now(), this.haveFile);
  }

  _emitState() {
    const snap = this.getState();
    this.emit('tick', snap);
    if (!snapshotsEqual(this._lastSnapshot, snap)) {
      this._lastSnapshot = snap;
      this.emit('state', snap);
    }
    return snap;
  }
}

/** Compare the fields that can change what the device shows. */
function snapshotsEqual(a, b) {
  if (!a || !b) return false;
  const keys = [
    'state',
    'ctxFill',
    'tokensIn',
    'tokensOut',
    'contextWindow',
    'sessionId',
    'model',
    'heuristic'
  ];
  for (const k of keys) {
    if (a[k] !== b[k]) return false;
  }
  const at = Number.isFinite(a.tps) ? Math.round(a.tps * 10) : null;
  const bt = Number.isFinite(b.tps) ? Math.round(b.tps * 10) : null;
  if (at !== bt) return false;
  const ae = Number.isFinite(a.elapsedSec) ? Math.floor(a.elapsedSec) : null;
  const be = Number.isFinite(b.elapsedSec) ? Math.floor(b.elapsedSec) : null;
  if (ae !== be) return false;
  return true;
}

module.exports = {
  CodexWatcher,
  SessionState,
  agentMessageText,
  contextFill,
  codexHome,
  newestSessionFile,
  listSessionFiles,
  readTailLines,
  parseLine,
  snapshotsEqual,
  BASELINE_TOKENS,
  DEFAULTS
};
