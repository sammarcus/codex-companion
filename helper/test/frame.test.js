'use strict';

const test = require('node:test');
const assert = require('node:assert');

const {
  buildFrame,
  frameLine,
  framesEqual,
  formatElapsed,
  pickQuotaWindow,
  PULSE_RING
} = require('../src/frame');

const NOW = Date.parse('2026-09-07T04:30:00.000Z');
/** resets_at values are Unix SECONDS, per RateLimitWindow. */
const FUTURE = Math.floor(NOW / 1000) + 3600;
const PAST = Math.floor(NOW / 1000) - 3600;

test('formatElapsed table', () => {
  const cases = [
    [0, '0:00'],
    [7, '0:07'],
    [59, '0:59'],
    [60, '1:00'],
    [754, '12:34'],
    [3599, '59:59'],
    [3600, '1:00:00'],
    [3723, '1:02:03'],
    [-5, '0:00'],
    [NaN, '0:00']
  ];
  for (const [input, want] of cases) {
    assert.strictEqual(formatElapsed(input), want, `formatElapsed(${input})`);
  }
});

test('buildFrame table', () => {
  const cases = [
    {
      name: 'the protocol example from the spec, exactly',
      state: { state: 'busy', ctxFill: 0.62, elapsedSec: 754, tps: 17.34 },
      want: {
        state: 'busy',
        ring: 0.62,
        center: '62%',
        label: 'CTX',
        sub: '12:34 elapsed',
        tps: 17.3
      }
    },
    {
      name: 'no ctxFill falls back to a TIME label and the 0.15 pulse ring',
      state: { state: 'busy', elapsedSec: 42 },
      want: { state: 'busy', ring: PULSE_RING, center: '0:42', label: 'TIME', sub: '0:42 elapsed' }
    },
    {
      name: 'no ctxFill and no elapsed still produces a valid frame',
      state: { state: 'sleep' },
      want: { state: 'sleep', ring: PULSE_RING, center: '--', label: 'TIME', sub: '' }
    },
    {
      name: 'model fills sub when there is no elapsed time',
      state: { state: 'idle', model: 'gpt-5-codex' },
      want: { state: 'idle', ring: PULSE_RING, center: '--', label: 'TIME', sub: 'gpt-5-codex' }
    },
    {
      name: 'ctxFill clamps above 1',
      state: { state: 'busy', ctxFill: 1.4, elapsedSec: 10 },
      want: { state: 'busy', ring: 1, center: '100%', label: 'CTX', sub: '0:10 elapsed' }
    },
    {
      name: 'ctxFill clamps below 0',
      state: { state: 'busy', ctxFill: -0.2, elapsedSec: 10 },
      want: { state: 'busy', ring: 0, center: '0%', label: 'CTX', sub: '0:10 elapsed' }
    },
    {
      name: 'an unknown state name degrades to sleep, never to garbage',
      state: { state: 'exploding' },
      want: { state: 'sleep', ring: PULSE_RING, center: '--', label: 'TIME', sub: '' }
    },
    {
      name: 'tps of zero is omitted rather than sent as a lie',
      state: { state: 'waiting', ctxFill: 0.34, elapsedSec: 71, tps: 0 },
      want: {
        state: 'waiting',
        ring: 0.34,
        center: '34%',
        label: 'CTX',
        sub: '1:11 elapsed'
      }
    },
    {
      name: 'a fresh quota snapshot with a future reset drives QUOTA mode',
      state: {
        state: 'idle',
        rateLimits: {
          observedAtMs: NOW - 60000,
          primary: { used_percent: 41, window_minutes: 300, resets_at: FUTURE }
        }
      },
      want: { state: 'idle', ring: 0.41, center: '41%', label: 'QUOTA', sub: '' }
    },
    {
      name: 'ctxFill outranks a quota window that is nowhere near its limit',
      state: {
        state: 'busy',
        ctxFill: 0.12,
        elapsedSec: 5,
        rateLimits: {
          observedAtMs: NOW,
          primary: { used_percent: 22, resets_at: FUTURE }
        }
      },
      want: { state: 'busy', ring: 0.12, center: '12%', label: 'CTX', sub: '0:05 elapsed' }
    },
    {
      // Both numbers arrive on the same TokenCountEvent, so gating QUOTA on
      // "no context fill known" made the mode unreachable in practice.
      name: 'a nearly exhausted quota takes the ring even though ctxFill is known',
      state: {
        state: 'busy',
        ctxFill: 0.03,
        elapsedSec: 5,
        rateLimits: {
          observedAtMs: NOW,
          primary: { used_percent: 98.5, resets_at: FUTURE }
        }
      },
      want: { state: 'busy', ring: 0.99, center: '99%', label: 'QUOTA', sub: '0:05 elapsed' }
    },
    {
      name: 'a stale nearly exhausted quota is still refused, ctxFill stays on the ring',
      state: {
        state: 'busy',
        ctxFill: 0.03,
        elapsedSec: 5,
        rateLimits: {
          observedAtMs: NOW - 60 * 60 * 1000,
          primary: { used_percent: 98.5, resets_at: FUTURE }
        }
      },
      want: { state: 'busy', ring: 0.03, center: '3%', label: 'CTX', sub: '0:05 elapsed' }
    },
    {
      name: 'a confirmed approval wait carries no guess marker',
      state: { state: 'waiting', heuristic: false, ctxFill: 0.34, elapsedSec: 71 },
      want: { state: 'waiting', ring: 0.34, center: '34%', label: 'CTX', sub: '1:11 elapsed' }
    },
    {
      name: 'a stall-derived waiting guess is visibly marked as one',
      state: { state: 'waiting', heuristic: true, ctxFill: 0.34, elapsedSec: 71 },
      want: {
        state: 'waiting',
        ring: 0.34,
        center: '34%',
        label: 'CTX',
        sub: '1:11 elapsed (guess)'
      }
    },
    {
      name: 'a waiting guess with nothing else to say still says it is a guess',
      state: { state: 'waiting', heuristic: true },
      want: {
        state: 'waiting',
        ring: PULSE_RING,
        center: '--',
        label: 'TIME',
        sub: 'waiting? (guess)'
      }
    },
    {
      name: 'heuristic only marks the waiting state, never busy',
      state: { state: 'busy', heuristic: true, ctxFill: 0.5, elapsedSec: 5 },
      want: { state: 'busy', ring: 0.5, center: '50%', label: 'CTX', sub: '0:05 elapsed' }
    },
    {
      name: 'a stale 100% quota never becomes a full static ring',
      state: {
        state: 'idle',
        rateLimits: {
          observedAtMs: NOW - 60 * 60 * 1000,
          primary: { used_percent: 100, resets_at: FUTURE }
        }
      },
      want: { state: 'idle', ring: PULSE_RING, center: '--', label: 'TIME', sub: '' }
    },
    {
      name: 'a quota window whose reset has already passed is not shown',
      state: {
        state: 'idle',
        rateLimits: {
          observedAtMs: NOW,
          primary: { used_percent: 100, resets_at: PAST }
        }
      },
      want: { state: 'idle', ring: PULSE_RING, center: '--', label: 'TIME', sub: '' }
    },
    {
      name: 'a quota snapshot with no observation time is not trusted',
      state: {
        state: 'idle',
        rateLimits: { primary: { used_percent: 55, resets_at: FUTURE } }
      },
      want: { state: 'idle', ring: PULSE_RING, center: '--', label: 'TIME', sub: '' }
    },
    {
      name: 'secondary window is used when primary is null',
      state: {
        state: 'idle',
        rateLimits: {
          observedAtMs: NOW,
          primary: null,
          secondary: { used_percent: 78.4, window_minutes: 10080, resets_at: FUTURE }
        }
      },
      want: { state: 'idle', ring: 0.78, center: '78%', label: 'QUOTA', sub: '' }
    }
  ];

  for (const c of cases) {
    const got = buildFrame(c.state, { now: NOW });
    assert.deepStrictEqual(got, c.want, c.name);
  }
});

test('quota mode can be disabled outright', () => {
  const state = {
    state: 'idle',
    rateLimits: { observedAtMs: NOW, primary: { used_percent: 41, resets_at: FUTURE } }
  };
  assert.strictEqual(buildFrame(state, { now: NOW }).label, 'QUOTA');
  assert.strictEqual(buildFrame(state, { now: NOW, allowQuota: false }).label, 'TIME');
});

test('a fresh window with no resets_at is kept: the field is Option<i64>', () => {
  const rl = { observedAtMs: NOW - 30000, primary: { used_percent: 41, resets_at: null } };
  const picked = pickQuotaWindow(rl, NOW);
  assert.ok(picked);
  assert.strictEqual(picked.key, 'primary');
  // Missing is unknown, not invalid. A reset time that has actually passed is
  // still a rejection.
  assert.strictEqual(
    pickQuotaWindow({ observedAtMs: NOW, primary: { used_percent: 41, resets_at: PAST } }, NOW),
    null
  );
});

test('pickQuotaWindow rejects a snapshot from the future', () => {
  const rl = {
    observedAtMs: NOW + 60 * 60 * 1000,
    primary: { used_percent: 10, resets_at: FUTURE }
  };
  assert.strictEqual(pickQuotaWindow(rl, NOW), null);
  assert.strictEqual(pickQuotaWindow(null, NOW), null);
  assert.strictEqual(pickQuotaWindow({ observedAtMs: NOW }, NOW), null);
});

test('frameLine is one newline-terminated JSON object', () => {
  const line = frameLine({ state: 'busy', ctxFill: 0.62, elapsedSec: 754, tps: 17.34 }, { now: NOW });
  assert.strictEqual(
    line,
    '{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}\n'
  );
  assert.strictEqual(line.split('\n').length, 2);
  assert.doesNotThrow(() => JSON.parse(line));
});

test('every state name in the spec survives a round trip', () => {
  for (const s of ['sleep', 'idle', 'busy', 'waiting', 'done']) {
    assert.strictEqual(buildFrame({ state: s }, { now: NOW }).state, s);
  }
});

test('framesEqual compares only what the device renders', () => {
  const a = buildFrame({ state: 'busy', ctxFill: 0.62, elapsedSec: 754 }, { now: NOW });
  const b = buildFrame({ state: 'busy', ctxFill: 0.6249, elapsedSec: 754 }, { now: NOW });
  const c = buildFrame({ state: 'idle', ctxFill: 0.62, elapsedSec: 754 }, { now: NOW });
  assert.ok(framesEqual(a, b), 'rounding to 2dp makes these the same frame');
  assert.ok(!framesEqual(a, c));
  assert.ok(!framesEqual(a, null));
});
