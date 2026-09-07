'use strict';

/**
 * format-truth.test.js - regression tests written during the round-1 review of
 * codex-watcher.js and frame.js, each re-derived from the vendored Codex source
 * rather than from docs/codex-state-format.md or the synthetic fixture.
 *
 * Every test in this file FAILS against the code as reviewed. They encode what
 * the Codex source actually says; do not weaken them to make them pass.
 */

const test = require('node:test');
const assert = require('node:assert');

const { SessionState, contextFill } = require('../src/codex-watcher');

const W = 258400; // model_context_window observed for gpt-5-codex
const T0 = Date.parse('2026-09-07T04:20:00.000Z');

function line(offsetMs, type, payload) {
  return {
    timestamp: new Date(T0 + offsetMs).toISOString(),
    type,
    payload
  };
}

function usage(totalTokens, outputTokens) {
  return {
    input_tokens: totalTokens - outputTokens,
    cached_input_tokens: 0,
    cache_write_input_tokens: 0,
    output_tokens: outputTokens,
    reasoning_output_tokens: 0,
    total_tokens: totalTokens
  };
}

function addUsage(a, b) {
  return {
    input_tokens: a.input_tokens + b.input_tokens,
    cached_input_tokens: a.cached_input_tokens + b.cached_input_tokens,
    cache_write_input_tokens: a.cache_write_input_tokens + b.cache_write_input_tokens,
    output_tokens: a.output_tokens + b.output_tokens,
    reasoning_output_tokens: a.reasoning_output_tokens + b.reasoning_output_tokens,
    total_tokens: a.total_tokens + b.total_tokens
  };
}

/**
 * BLOCKER. Context fill must be computed from the LIVE context size, which is
 * `info.last_token_usage` (token_count) / `payload.usage` (token_usage_record),
 * not from the accumulated session total.
 *
 * Source, all three Codex call sites, all of which pass `last_token_usage`:
 *   tui/src/chatwidget.rs:1149-1151
 *   tui/src/status/card.rs:360,364
 *   tui/src/chatwidget/status_controls.rs:421,425
 * and the doc comment on the accessor itself, tui/src/token_usage.rs:37-38:
 *   "For `last_token_usage`, this is the latest active context size; for
 *    `total_token_usage`, this is the accumulated session total."
 *
 * The accumulated total grows without bound because every response re-sends the
 * whole conversation, so driving the ring from it pins CTX at a static 100%,
 * which is the one thing the ring must never show.
 */
test('context fill tracks the live context size, not the accumulated session total', () => {
  const s = new SessionState();
  s.apply(line(0, 'session_meta', { id: 'thread-1', history_mode: 'paginated' }), T0);
  s.apply(
    line(1000, 'event_msg', {
      type: 'task_started',
      turn_id: 't1',
      started_at: Math.floor((T0 + 1000) / 1000),
      model_context_window: W
    }),
    T0
  );

  // Six responses. Each leaves ~60k tokens live in the window (a fifth of it),
  // while the session total climbs to 360k, well past the window itself.
  const live = usage(60000, 1000);
  let cumulative = usage(0, 0);
  for (let i = 1; i <= 6; i += 1) {
    cumulative = addUsage(cumulative, live);
    s.apply(
      line(i * 10000, 'event_msg', {
        type: 'token_count',
        info: {
          total_token_usage: cumulative,
          last_token_usage: live,
          model_context_window: W
        },
        rate_limits: null
      }),
      T0
    );
  }

  const expected = contextFill(60000, W); // 0.19
  const snap = s.derive(T0 + 70000, true);

  assert.notStrictEqual(
    snap.ctxFill,
    1,
    'the ring must not pin at a static 100% just because the session is long'
  );
  assert.strictEqual(
    snap.ctxFill,
    expected,
    'ctxFill must come from last_token_usage.total_tokens (60000), not total_token_usage.total_tokens (360000)'
  );
});

/**
 * BLOCKER, same defect on the other feed. `token_usage_record` carries three
 * TokenUsage objects (protocol/src/protocol.rs:2239-2247): `usage` is
 * "Best-effort Responses API usage observed for one completed response" and is
 * the analogue of `last_token_usage`; `turn_token_usage` and
 * `thread_token_usage` are running totals. The watcher prefers
 * `thread_token_usage`, so the same 100% pin happens with no token_count event.
 */
test('token_usage_record drives context fill from payload.usage, not thread_token_usage', () => {
  const s = new SessionState();
  s.apply(line(0, 'session_meta', { id: 'thread-1', history_mode: 'paginated' }), T0);
  s.apply(
    line(1000, 'event_msg', {
      type: 'task_started',
      turn_id: 't1',
      started_at: Math.floor((T0 + 1000) / 1000),
      model_context_window: W
    }),
    T0
  );

  const live = usage(60000, 1000);
  let thread = usage(0, 0);
  for (let i = 1; i <= 6; i += 1) {
    thread = addUsage(thread, live);
    s.apply(
      line(i * 10000, 'token_usage_record', {
        thread_id: 'thread-1',
        turn_id: 't1',
        session_id: 'thread-1',
        root_turn_id: 't1',
        response_id: `resp_${i}`,
        usage: live,
        turn_token_usage: thread,
        thread_token_usage: thread
      }),
      T0
    );
  }

  const snap = s.derive(T0 + 70000, true);
  assert.strictEqual(
    snap.ctxFill,
    contextFill(60000, W),
    'ctxFill must come from payload.usage.total_tokens, not thread_token_usage.total_tokens'
  );
});

/**
 * MAJOR. `AgentMessageItem` has no `text` field. Source, items.rs:150-152:
 *   pub struct AgentMessageItem { pub id: String, pub content: Vec<AgentMessageContent>, ... }
 * and items.rs:124-126:
 *   pub enum AgentMessageContent { Text { text: String } }
 *
 * codex-watcher.js reads `item.text`, which never matches, so the
 * `item_completed` branch that is supposed to pick up the assistant's last
 * message in paginated mode is dead. The synthetic fixture agrees with the code
 * rather than with the source, which is why nothing caught it.
 */
test('item_completed AgentMessage is read from content[], the shape items.rs actually defines', () => {
  const s = new SessionState();
  s.apply(
    line(0, 'event_msg', {
      type: 'item_completed',
      thread_id: 'thread-1',
      turn_id: 't1',
      item: {
        type: 'AgentMessage',
        id: 'item-1',
        content: [{ type: 'Text', text: 'Created hello.txt and listed the directory.' }]
      },
      started_at_ms: T0,
      completed_at_ms: T0
    }),
    T0
  );

  assert.strictEqual(
    s.derive(T0, true).lastAgentMessage,
    'Created hello.txt and listed the directory.'
  );
});

/**
 * MAJOR. Nothing clears `pendingApprovals` except a turn boundary, so once a
 * waiting state latches the device keeps pulsing amber while the agent is
 * demonstrably working again. There is no durable "answer" event to key off
 * (rollout/src/policy.rs drops the whole approval family), so continued fresh
 * activity on the same turn is the only available evidence that the human
 * already answered. Latching through it presents a stale guess as the hero
 * "visibly impatient" state.
 */
test('waiting clears once the turn is demonstrably moving again', () => {
  const s = new SessionState();
  s.apply(line(0, 'session_meta', { id: 'thread-1' }), T0);
  s.apply(
    line(1000, 'event_msg', {
      type: 'task_started',
      turn_id: 't1',
      started_at: Math.floor((T0 + 1000) / 1000),
      model_context_window: W
    }),
    T0
  );
  s.apply(
    line(2000, 'event_msg', {
      type: 'exec_approval_request',
      kind: 'command',
      call_id: 'call-1',
      approval_id: 'appr-1',
      turn_id: 't1'
    }),
    T0
  );
  assert.strictEqual(s.derive(T0 + 2500, true).state, 'waiting', 'the request itself is waiting');

  // Human approved. Codex never writes the decision, but it does write the work
  // that follows it.
  for (let i = 1; i <= 5; i += 1) {
    s.apply(
      line(2000 + i * 3000, 'event_msg', {
        type: 'item_completed',
        thread_id: 'thread-1',
        turn_id: 't1',
        item: { type: 'CommandExecution', id: `cmd-${i}` },
        started_at_ms: T0 + 2000 + i * 3000,
        completed_at_ms: T0 + 2000 + i * 3000
      }),
      T0
    );
  }

  assert.strictEqual(
    s.derive(T0 + 17500, true).state,
    'busy',
    'five completed commands after the request means the human already answered'
  );
});

/**
 * MINOR. RateLimitWindow.resets_at is Option<i64> (protocol.rs:2367-2376), so a
 * legitimate fresh snapshot can omit it. frame.js requires a finite resets_at
 * and silently discards the window, which throws away real quota data. The
 * freshness guard on observedAtMs is what protects against a stale 100%; the
 * reset time is not load-bearing for that.
 */
test('a fresh quota window with a null resets_at is still usable', () => {
  const { pickQuotaWindow } = require('../src/frame');
  const nowMs = T0;
  const picked = pickQuotaWindow(
    {
      observedAtMs: nowMs - 30 * 1000,
      primary: { used_percent: 41, window_minutes: 300, resets_at: null },
      secondary: null
    },
    nowMs
  );
  assert.ok(picked, 'resets_at is optional in the Rust struct, not a validity signal');
  assert.strictEqual(picked.window.used_percent, 41);
});
