'use strict';

/**
 * The session-file path. It exists for the numbers hooks do not carry:
 * context fill, token counts, rate limits, turn elapsed. It is never allowed
 * to be the source of the device's state, and it is never allowed to retain
 * anything you or the model typed.
 *
 * Ported from the pre-hook helper's watcher and format-truth suites, which
 * were written against codex-cli 0.153.4 and the Codex source in vendor/.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

const {
  contextFill,
  parseLine,
  readTailLines,
  newestRolloutFile,
  isMetricLine,
  readMetrics,
  codexHome,
  metricFields,
  BASELINE_TOKENS
} = require('../codex-companion');

const W = 258400; // model_context_window observed for gpt-5-codex
const T0 = Date.parse('2026-09-07T04:20:00.000Z');

function line(type, payload, tOffsetMs) {
  return JSON.stringify({
    timestamp: new Date(T0 + (tOffsetMs || 0)).toISOString(),
    type,
    payload
  });
}

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-test-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

// --- the formula ----------------------------------------------------------

test('contextFill uses Codex own baseline formula, not used/window', () => {
  // window 258400, live context 100000: Codex subtracts a 12000 baseline from
  // both sides, so the answer is 36%, not the 39% used/window would give.
  assert.strictEqual(contextFill(100000, W), 0.36);
  assert.notStrictEqual(contextFill(100000, W), Math.round((100000 / W) * 100) / 100);
});

test('contextFill is 0 at the baseline and 1 at a full window', () => {
  assert.strictEqual(contextFill(BASELINE_TOKENS, W), 0);
  assert.strictEqual(contextFill(0, W), 0);
  assert.strictEqual(contextFill(W, W), 1);
});

test('contextFill refuses to answer without both numbers', () => {
  assert.strictEqual(contextFill(null, W), null);
  assert.strictEqual(contextFill(100, null), null);
  assert.strictEqual(contextFill(100, BASELINE_TOKENS), null);
});

test('codexHome honours the CODEX_HOME override', () => {
  assert.strictEqual(codexHome({ CODEX_HOME: '/tmp/elsewhere' }), '/tmp/elsewhere');
  assert.strictEqual(codexHome({}), path.join(os.homedir(), '.codex'));
  assert.strictEqual(codexHome({ CODEX_HOME: '   ' }), path.join(os.homedir(), '.codex'));
});

// --- which number the ring is allowed to use ------------------------------

test('the ring follows the live context size, never the accumulated total', () => {
  // total_token_usage grows for the whole session and would pin the ring at
  // 100%; last_token_usage is what is actually resident in the window.
  const lines = [
    line('event_msg', { type: 'task_started', model_context_window: W }, 0),
    line(
      'event_msg',
      {
        type: 'token_count',
        info: {
          total_token_usage: { input_tokens: 900000, output_tokens: 40000, total_tokens: 940000 },
          last_token_usage: { input_tokens: 98000, output_tokens: 2000, total_tokens: 100000 },
          model_context_window: W
        }
      },
      5000
    )
  ];
  const m = readMetrics(null, T0 + 6000, lines);
  assert.strictEqual(m.contextTokens, 100000);
  assert.strictEqual(m.totalTokens, 940000);
  assert.strictEqual(m.ctxFill, 0.36);
});

test('token_usage_record drives context fill from payload.usage, not the thread total', () => {
  const lines = [
    line('event_msg', { type: 'task_started', model_context_window: W }, 0),
    line(
      'token_usage_record',
      {
        usage: { input_tokens: 49000, output_tokens: 1000, total_tokens: 50000 },
        thread_token_usage: { input_tokens: 700000, output_tokens: 30000, total_tokens: 730000 }
      },
      3000
    )
  ];
  const m = readMetrics(null, T0 + 4000, lines);
  assert.strictEqual(m.contextTokens, 50000);
  assert.strictEqual(m.totalTokens, 730000);
  assert.strictEqual(m.ctxFill, contextFill(50000, W));
});

test('a compacted record carries the token totals forward', () => {
  const lines = [
    line('event_msg', { type: 'task_started', model_context_window: W }, 0),
    line(
      'compacted',
      {
        latest_token_usage_record: {
          usage: { total_tokens: 20000 },
          thread_token_usage: { total_tokens: 500000 }
        }
      },
      1000
    )
  ];
  const m = readMetrics(null, T0 + 2000, lines);
  assert.strictEqual(m.contextTokens, 20000);
  assert.strictEqual(m.totalTokens, 500000);
});

test('tokens per second comes from consecutive usage records', () => {
  const lines = [
    line('token_usage_record', { usage: { output_tokens: 100, total_tokens: 1000 } }, 0),
    line('token_usage_record', { usage: { output_tokens: 200, total_tokens: 2000 } }, 10000)
  ];
  const m = readMetrics(null, T0 + 11000, lines);
  assert.strictEqual(Math.round(m.tps), 20); // 200 output tokens over 10 seconds
});

test('rate limits are kept as numbers with the time they were observed', () => {
  const lines = [
    line(
      'event_msg',
      {
        type: 'token_count',
        info: { model_context_window: W },
        rate_limits: {
          primary: { used_percent: 12.5, window_minutes: 300, label: 'ignored text' },
          secondary: { used_percent: 97, resets_at: 4000000000 }
        }
      },
      0
    )
  ];
  const m = readMetrics(null, T0 + 1000, lines);
  assert.strictEqual(m.rateLimits.primary.used_percent, 12.5);
  assert.strictEqual(m.rateLimits.secondary.used_percent, 97);
  assert.strictEqual(m.rateLimits.primary.label, undefined);
  assert.strictEqual(m.rateLimits.observedAtMs, T0);
});

// --- the stopwatch --------------------------------------------------------

test('an open turn measures elapsed time from its own started_at', () => {
  const lines = [
    line('event_msg', { type: 'task_started', started_at: T0 / 1000, model_context_window: W }, 0)
  ];
  const m = readMetrics(null, T0 + 90000, lines);
  assert.strictEqual(m.turnOpen, true);
  assert.strictEqual(m.elapsedSec, 90);
  assert.strictEqual(metricFields(m, T0 + 90000).sub, '1:30 elapsed');
});

test('a closed turn keeps the duration it actually took', () => {
  const lines = [
    line('event_msg', { type: 'task_started', started_at: T0 / 1000 }, 0),
    line('event_msg', { type: 'task_complete', duration_ms: 6900 }, 7000)
  ];
  const m = readMetrics(null, T0 + 60000, lines);
  assert.strictEqual(m.turnOpen, false);
  assert.strictEqual(m.elapsedSec, 6.9);
});

test('turn_started and turn_complete are accepted as aliases', () => {
  const started = readMetrics(null, T0 + 1000, [
    line('event_msg', { type: 'turn_started', started_at: T0 / 1000 }, 0)
  ]);
  assert.strictEqual(started.turnOpen, true);
  const done = readMetrics(null, T0 + 5000, [
    line('event_msg', { type: 'turn_started', started_at: T0 / 1000 }, 0),
    line('event_msg', { type: 'turn_complete', duration_ms: 2000 }, 2000)
  ]);
  assert.strictEqual(done.turnOpen, false);
});

test('an aborted turn closes the turn without inventing a completion', () => {
  const m = readMetrics(null, T0 + 5000, [
    line('event_msg', { type: 'task_started', started_at: T0 / 1000 }, 0),
    line('event_msg', { type: 'turn_aborted', duration_ms: 1200, reason: 'interrupted' }, 1200)
  ]);
  assert.strictEqual(m.turnOpen, false);
  assert.strictEqual(m.elapsedSec, 1.2);
});

// --- robustness -----------------------------------------------------------

test('blank, truncated and non-JSON lines are ignored', () => {
  assert.strictEqual(parseLine(''), null);
  assert.strictEqual(parseLine('   '), null);
  assert.strictEqual(parseLine('{"type":"event_msg","pay'), null);
  assert.strictEqual(parseLine('[1,2,3]'), null);
  assert.strictEqual(parseLine('null'), null);
  const m = readMetrics(null, T0, [
    '',
    'garbage',
    '{"type":"token_count","payl',
    line('event_msg', { type: 'token_count', info: { model_context_window: W } }, 0)
  ]);
  assert.strictEqual(m.contextWindow, W);
});

test('records with no usable timestamp fall back to now', () => {
  const m = readMetrics(null, T0, [
    JSON.stringify({ type: 'event_msg', payload: { type: 'task_started' } })
  ]);
  assert.strictEqual(m.turnStartedAtMs, T0);
});

test('readTailLines never returns a partial line', (t) => {
  const dir = tmpdir(t);
  const file = path.join(dir, 'rollout-x.jsonl');
  fs.writeFileSync(file, 'aaaa\nbbbb\ncccc\ndddd'); // no trailing newline
  const all = readTailLines(file, 1024);
  assert.deepStrictEqual(all, ['aaaa', 'bbbb', 'cccc']);
  // Clipped from the middle: the first (probably partial) line is dropped.
  const clipped = readTailLines(file, 10);
  assert.ok(!clipped.includes('aaaa'));
  assert.ok(clipped.every((l) => l.length === 4));
});

test('readTailLines on a missing file returns nothing instead of throwing', () => {
  assert.deepStrictEqual(readTailLines('/no/such/file.jsonl', 1024), []);
});

test('newestRolloutFile picks by mtime and ignores everything else', (t) => {
  const dir = tmpdir(t);
  fs.mkdirSync(path.join(dir, '2026', '09', '07'), { recursive: true });
  const older = path.join(dir, 'rollout-older.jsonl');
  const newer = path.join(dir, '2026', '09', '07', 'rollout-newer.jsonl');
  fs.writeFileSync(older, '{}\n');
  fs.writeFileSync(path.join(dir, 'notes.txt'), 'not a rollout');
  fs.writeFileSync(path.join(dir, 'other.jsonl'), 'not a rollout either');
  fs.writeFileSync(newer, '{}\n');
  fs.utimesSync(older, new Date(T0 - 100000), new Date(T0 - 100000));
  fs.utimesSync(newer, new Date(T0), new Date(T0));
  assert.strictEqual(newestRolloutFile(dir).path, newer);
});

test('newestRolloutFile on a missing directory is null, not an exception', () => {
  assert.strictEqual(newestRolloutFile('/no/such/dir'), null);
});

// --- privacy --------------------------------------------------------------

test('lines that carry prompts, replies or file contents are never parsed', () => {
  assert.strictEqual(isMetricLine('{"type":"response_item","payload":{"text":"secret"}}'), false);
  assert.strictEqual(isMetricLine('{"type":"session_meta","payload":{"instructions":"..."}}'), false);
  assert.strictEqual(isMetricLine('{"type":"world_state","payload":{}}'), false);
  assert.strictEqual(isMetricLine('{"type":"event_msg","payload":{"type":"token_count"}}'), true);
  assert.strictEqual(isMetricLine('{"type":"token_usage_record","payload":{}}'), true);
});

test('nothing readMetrics returns from a real session is a string', () => {
  // The fixture is a real captured session: it contains base instructions, a
  // user prompt and model output. If any of that could reach the device, this
  // fails, because every field the metrics path is allowed to produce is a
  // number, a boolean or null.
  const file = path.join(__dirname, 'fixtures', 'session-sample.jsonl');
  const m = readMetrics(file, Date.now());
  const walk = (v, at) => {
    if (v === null || typeof v === 'number' || typeof v === 'boolean') return;
    if (typeof v === 'object') {
      for (const k of Object.keys(v)) walk(v[k], `${at}.${k}`);
      return;
    }
    assert.fail(`metrics.${at} is a ${typeof v}: ${JSON.stringify(v)}`);
  };
  walk(m, '');
});

test('the frame built from a real session is numbers and fixed words only', () => {
  const file = path.join(__dirname, 'fixtures', 'session-sample-synthetic.jsonl');
  const m = readMetrics(file, Date.now());
  const f = metricFields(m, Date.now());
  assert.ok(['CTX', 'TIME', 'QUOTA'].includes(f.label));
  assert.match(f.center, /^(--|\d+%|\d+:\d\d(:\d\d)?)$/);
  assert.match(f.sub, /^(|\d+:\d\d(:\d\d)? elapsed)$/);
});
