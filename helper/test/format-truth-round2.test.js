'use strict';

/**
 * format-truth-round2.test.js - regression tests written during the round-2
 * review of codex-watcher.js and frame.js.
 *
 * Every test in this file FAILS against the code as reviewed. Each one is
 * re-derived from the vendored Codex source or from docs/codex-state-format.md,
 * not from the current implementation. Do not weaken them to make them pass.
 */

const test = require('node:test');
const assert = require('node:assert');

const { SessionState, DEFAULTS } = require('../src/codex-watcher');
const { buildFrame, pickQuotaWindow } = require('../src/frame');

const W = 258400; // model_context_window observed for gpt-5-codex
const T0 = Date.parse('2026-09-07T04:20:00.000Z');
const STARTED_AT = 1788754800; // Unix seconds, matches the real capture

function line(offsetMs, type, payload) {
  return { timestamp: new Date(T0 + offsetMs).toISOString(), type, payload };
}

function feed(s, l) {
  s.apply(l, Date.parse(l.timestamp));
}

function started(s) {
  feed(
    s,
    line(0, 'event_msg', {
      type: 'task_started',
      turn_id: 't1',
      started_at: STARTED_AT,
      model_context_window: W,
      collaboration_mode_kind: 'default'
    })
  );
}

function turnContext(s, offsetMs, approvalPolicy) {
  feed(
    s,
    line(offsetMs, 'turn_context', {
      turn_id: 't1',
      model: 'gpt-5-codex',
      approval_policy: approvalPolicy
    })
  );
}

/* ------------------------------------------------------------------ *
 * 1. turn_aborted is not a completed turn
 * ------------------------------------------------------------------ */

test('an interrupted turn must not render as the green done flash', () => {
  // TurnAbortedEvent (protocol/src/protocol.rs:4154) carries the same
  // started_at / completed_at / duration_ms as TurnCompleteEvent, so
  // _closeTurn() fills lastTurnCompletedAtMs for it and derive() reads that as
  // "done". design-spec part 2 reserves done for a turn that finished; a
  // Ctrl-C must not look identical to a successful one on the desk.
  const s = new SessionState();
  started(s);
  feed(
    s,
    line(12000, 'event_msg', {
      type: 'turn_aborted',
      turn_id: 't1',
      reason: 'interrupted',
      started_at: STARTED_AT,
      completed_at: STARTED_AT + 12,
      duration_ms: 12000
    })
  );

  const now = T0 + 12500;
  const snap = s.derive(now, true);
  assert.strictEqual(snap.abortedReason, 'interrupted');
  assert.notStrictEqual(
    snap.state,
    'done',
    'an aborted turn reports state "done", so an interrupt flashes green success'
  );
  assert.strictEqual(snap.state, 'idle');
});

/* ------------------------------------------------------------------ *
 * 2. the secondary rate-limit window can never reach the ring
 * ------------------------------------------------------------------ */

test('a nearly-exhausted secondary rate-limit window can take the ring', () => {
  // RateLimitSnapshot (protocol.rs:2324) carries primary AND secondary, each an
  // Option<RateLimitWindow>. pickQuotaWindow returns the first VALID window, so
  // whenever primary is present and fresh, secondary is unreachable. The weekly
  // window is the one a heavy user actually exhausts.
  const now = T0;
  const rateLimits = {
    primary: { used_percent: 12.5, window_minutes: 300, resets_at: (now + 3600e3) / 1000 },
    secondary: { used_percent: 97.4, window_minutes: 10080, resets_at: (now + 86400e3) / 1000 },
    observedAtMs: now
  };

  const picked = pickQuotaWindow(rateLimits, now);
  assert.ok(picked, 'a fresh snapshot with two windows must pick one');
  assert.strictEqual(
    picked.key,
    'secondary',
    'pickQuotaWindow returns primary at 12.5% and hides secondary at 97.4%'
  );

  const frame = buildFrame({ state: 'busy', ctxFill: 0.6, rateLimits }, { now });
  assert.strictEqual(frame.label, 'QUOTA');
  assert.strictEqual(frame.center, '97%');
});

/* ------------------------------------------------------------------ *
 * 3. the stall heuristic fires below the documented floor
 * ------------------------------------------------------------------ */

test('a long silent tool call is still busy, not a guessed approval prompt', () => {
  // docs/codex-state-format.md section 6: "STALL_S should be generous
  // (60-120 s). Long model turns and long tool calls emit nothing to the JSONL
  // ... Silence is not evidence of being stuck." DEFAULTS.stallMs is 45000, so
  // any build, test run or model turn over 45s paints the hero amber pulse
  // while the agent is working normally.
  assert.ok(
    DEFAULTS.stallMs >= 60 * 1000,
    `stallMs is ${DEFAULTS.stallMs}ms, below the 60s floor the format doc sets`
  );

  const s = new SessionState();
  started(s);
  turnContext(s, 500, 'on-request');

  const snap = s.derive(T0 + 50 * 1000, true);
  assert.strictEqual(
    snap.state,
    'busy',
    '50s of silence inside an open turn is reported as waiting'
  );
});

/* ------------------------------------------------------------------ *
 * 4. a sleeping board must not show a frozen context percentage
 * ------------------------------------------------------------------ */

test('a sleeping board shows no context percentage, not the last one frozen', () => {
  // derive() already blanks elapsedSec in sleep, for exactly this reason: "A
  // sleeping board must not display the previous turn's frozen stopwatch."
  // ctxFill gets no such treatment, so an idle desk shows yesterday's 60% in
  // the ring indefinitely, as a number the helper cannot still vouch for.
  const s = new SessionState();
  started(s);
  turnContext(s, 500, 'never');
  feed(
    s,
    line(10000, 'event_msg', {
      type: 'token_count',
      info: {
        total_token_usage: { total_tokens: 160000 },
        last_token_usage: { total_tokens: 160000 },
        model_context_window: W
      },
      rate_limits: null
    })
  );
  feed(
    s,
    line(11500, 'event_msg', {
      type: 'task_complete',
      turn_id: 't1',
      last_agent_message: 'done',
      started_at: STARTED_AT,
      completed_at: STARTED_AT + 11,
      duration_ms: 11500
    })
  );

  const now = T0 + 11500 + 6 * 60 * 1000;
  const snap = s.derive(now, true);
  assert.strictEqual(snap.state, 'sleep');
  assert.strictEqual(snap.elapsedSec, null, 'the stopwatch is already blanked');
  assert.strictEqual(
    snap.ctxFill,
    null,
    'ctxFill survives into sleep, so the ring keeps the last percentage forever'
  );

  const frame = buildFrame(snap, { now });
  assert.strictEqual(frame.center, '--');
});
