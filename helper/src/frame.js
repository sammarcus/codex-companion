'use strict';

/**
 * frame.js - pure state -> protocol line.
 *
 * Protocol (host -> device), one JSON object per line:
 *   {"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
 *
 * Every field may be omitted; the device keeps the last value it saw.
 * This module is deliberately free of I/O and of Date.now(): callers pass `now`.
 */

/** Ring value used when we have no context-fill number to show. */
const PULSE_RING = 0.15;

/**
 * How fresh a rate-limit snapshot must be before we are willing to drive the
 * ring from it. Rate limits only land on disk when a `token_count` event fires,
 * so they go stale between turns. A stale snapshot pinned at 100% would light a
 * permanent full ring, which is exactly the thing we must never show.
 */
const QUOTA_MAX_AGE_MS = 15 * 60 * 1000;

/**
 * How full a rate-limit window has to be before QUOTA takes the ring away from
 * CTX. Both numbers arrive on the same event (TokenCountEvent carries `info`
 * and `rate_limits` together, protocol.rs:2318-2321), so "show quota only when
 * context is unknown" would make QUOTA mode effectively unreachable. The mode
 * has to be chosen on the quota data itself: near the limit, the quota is the
 * number you actually need.
 */
const QUOTA_TAKEOVER_PERCENT = 80;

const VALID_STATES = new Set(['sleep', 'idle', 'busy', 'waiting', 'done']);

function clamp01(n) {
  if (!Number.isFinite(n)) return 0;
  if (n < 0) return 0;
  if (n > 1) return 1;
  return n;
}

function round2(n) {
  return Math.round(n * 100) / 100;
}

function round1(n) {
  return Math.round(n * 10) / 10;
}

/** Seconds -> "12:34", or "1:02:03" past an hour. */
function formatElapsed(sec) {
  if (!Number.isFinite(sec) || sec < 0) sec = 0;
  const total = Math.floor(sec);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const pad = (n) => String(n).padStart(2, '0');
  if (h > 0) return `${h}:${pad(m)}:${pad(s)}`;
  return `${m}:${pad(s)}`;
}

/**
 * Pick the rate-limit window to display, or null.
 *
 * Rules:
 *  - the snapshot must carry an observation time and be fresh (QUOTA_MAX_AGE_MS)
 *  - the window must have a numeric used_percent
 *  - if the window states a resets_at, it must be in the future
 *
 * resets_at is Option<i64> in the Rust struct (RateLimitWindow,
 * protocol.rs:2367-2376), so a legitimate fresh snapshot may simply omit it.
 * Missing means unknown, not invalid. The observedAtMs freshness check above is
 * what actually stops a stale reading from lighting a permanent full ring.
 */
function pickQuotaWindow(rateLimits, nowMs) {
  if (!rateLimits || typeof rateLimits !== 'object') return null;

  const observedAt = rateLimits.observedAtMs;
  if (!Number.isFinite(observedAt)) return null;
  if (nowMs - observedAt > QUOTA_MAX_AGE_MS) return null;
  // A snapshot claiming to be from the future is a clock problem, not data.
  if (observedAt - nowMs > QUOTA_MAX_AGE_MS) return null;

  for (const key of ['primary', 'secondary']) {
    const w = rateLimits[key];
    if (!w || typeof w !== 'object') continue;
    if (!Number.isFinite(w.used_percent)) continue;
    // resets_at is Unix seconds, and optional. Only a stated reset time that
    // has already passed disqualifies the window.
    if (Number.isFinite(w.resets_at) && w.resets_at * 1000 <= nowMs) continue;
    return { key, window: w };
  }
  return null;
}

/**
 * Build the protocol object for one frame.
 *
 * @param {object} state    snapshot from codex-watcher (see its docs)
 * @param {object} [opts]
 * @param {number} [opts.now]        ms epoch; defaults to Date.now()
 * @param {boolean} [opts.allowQuota] set false to disable quota mode entirely
 * @returns {{state:string,ring:number,center:string,label:string,sub:string,tps?:number}}
 */
function buildFrame(state, opts) {
  const o = opts || {};
  const nowMs = Number.isFinite(o.now) ? o.now : Date.now();
  const allowQuota = o.allowQuota !== false;
  const s = state || {};

  const devState = VALID_STATES.has(s.state) ? s.state : 'sleep';

  const ctxKnown = Number.isFinite(s.ctxFill);
  const elapsed = Number.isFinite(s.elapsedSec) ? s.elapsedSec : null;

  let label;
  let ring;
  let center;

  const quota = allowQuota ? pickQuotaWindow(s.rateLimits, nowMs) : null;
  // Quota wins when it is the more urgent of the two, or when context fill is
  // not known at all. Otherwise CTX, which is the primary metric.
  const quotaWins =
    quota !== null && (!ctxKnown || quota.window.used_percent >= QUOTA_TAKEOVER_PERCENT);

  if (quotaWins) {
    label = 'QUOTA';
    ring = clamp01(quota.window.used_percent / 100);
    center = `${Math.round(quota.window.used_percent)}%`;
  } else if (ctxKnown) {
    label = 'CTX';
    ring = clamp01(s.ctxFill);
    center = `${Math.round(ring * 100)}%`;
  } else if (elapsed !== null) {
    label = 'TIME';
    ring = PULSE_RING;
    center = formatElapsed(elapsed);
  } else {
    label = 'TIME';
    ring = PULSE_RING;
    center = '--';
  }

  // A stall-derived guess must never look like an observed approval request.
  // codex-watcher flags it; if that never reached the device the amber pulse
  // would present a guess as a fact, which docs/codex-state-format.md forbids.
  const guessed = devState === 'waiting' && Boolean(s.heuristic);

  let sub;
  if (elapsed !== null) {
    sub = `${formatElapsed(elapsed)} elapsed`;
  } else if (s.model) {
    sub = String(s.model);
  } else {
    sub = '';
  }
  if (guessed) sub = sub ? `${sub} (guess)` : 'waiting? (guess)';

  const frame = {
    state: devState,
    ring: round2(ring),
    center,
    label,
    sub
  };

  if (Number.isFinite(s.tps) && s.tps > 0) {
    frame.tps = round1(s.tps);
  }

  return frame;
}

/** Build a frame and serialize it as one newline-terminated protocol line. */
function frameLine(state, opts) {
  return JSON.stringify(buildFrame(state, opts)) + '\n';
}

/** True when two frames would render identically on the device. */
function framesEqual(a, b) {
  if (!a || !b) return false;
  const keys = ['state', 'ring', 'center', 'label', 'sub', 'tps'];
  for (const k of keys) {
    if (a[k] !== b[k]) return false;
  }
  return true;
}

module.exports = {
  buildFrame,
  frameLine,
  framesEqual,
  formatElapsed,
  pickQuotaWindow,
  PULSE_RING,
  QUOTA_MAX_AGE_MS,
  QUOTA_TAKEOVER_PERCENT
};
