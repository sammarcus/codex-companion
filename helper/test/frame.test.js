'use strict';

/** Frame building: metrics in, one protocol line out, clamped to what fits. */

const test = require('node:test');
const assert = require('node:assert');

const {
  metricFields,
  frameForEvent,
  frameToLine,
  formatElapsed,
  pickQuotaWindow,
  MAX_LINE_BYTES
} = require('../codex-companion');

const NOW = Date.parse('2026-09-07T04:30:00.000Z');

test('context fill drives the ring and the centre readout', () => {
  const f = metricFields({ ctxFill: 0.62, elapsedSec: 754 }, NOW);
  assert.strictEqual(f.label, 'CTX');
  assert.strictEqual(f.ring, 0.62);
  assert.strictEqual(f.center, '62%');
  assert.strictEqual(f.sub, '12:34 elapsed');
});

test('with no context number the label falls back to the stopwatch', () => {
  const f = metricFields({ ctxFill: null, elapsedSec: 42 }, NOW);
  assert.strictEqual(f.label, 'TIME');
  assert.strictEqual(f.center, '0:42');
});

test('with nothing at all the centre is two dashes, never a fake zero', () => {
  const f = metricFields({}, NOW);
  assert.strictEqual(f.center, '--');
  assert.strictEqual(f.label, 'TIME');
});

test('a nearly exhausted quota window takes the ring away from CTX', () => {
  const f = metricFields(
    {
      ctxFill: 0.2,
      rateLimits: {
        primary: { used_percent: 12 },
        secondary: { used_percent: 97 },
        observedAtMs: NOW - 1000
      }
    },
    NOW
  );
  assert.strictEqual(f.label, 'QUOTA');
  assert.strictEqual(f.center, '97%');
  assert.strictEqual(f.ring, 0.97);
});

test('a comfortable quota window leaves CTX alone', () => {
  const f = metricFields(
    { ctxFill: 0.2, rateLimits: { primary: { used_percent: 30 }, observedAtMs: NOW } },
    NOW
  );
  assert.strictEqual(f.label, 'CTX');
});

test('a stale quota reading is never shown, so the ring cannot sit at a fake 100%', () => {
  const stale = { primary: { used_percent: 100 }, observedAtMs: NOW - 60 * 60 * 1000 };
  assert.strictEqual(pickQuotaWindow(stale, NOW), null);
  const f = metricFields({ ctxFill: 0.1, rateLimits: stale }, NOW);
  assert.strictEqual(f.label, 'CTX');
});

test('a window whose reset time has already passed is discarded', () => {
  const past = {
    primary: { used_percent: 99, resets_at: Math.floor(NOW / 1000) - 60 },
    observedAtMs: NOW
  };
  assert.strictEqual(pickQuotaWindow(past, NOW), null);
});

test('a fresh window with no stated reset time is still usable', () => {
  const w = pickQuotaWindow({ primary: { used_percent: 55 }, observedAtMs: NOW }, NOW);
  assert.ok(w);
  assert.strictEqual(w.window.used_percent, 55);
});

test('formatElapsed rolls over into hours and pads', () => {
  assert.strictEqual(formatElapsed(0), '0:00');
  assert.strictEqual(formatElapsed(61), '1:01');
  assert.strictEqual(formatElapsed(3723), '1:02:03');
  assert.strictEqual(formatElapsed(-5), '0:00');
  assert.strictEqual(formatElapsed(NaN), '0:00');
});

test('each hook event maps to exactly one device state', () => {
  assert.strictEqual(frameForEvent('SessionStart', null, NOW).state, 'idle');
  assert.strictEqual(frameForEvent('UserPromptSubmit', null, NOW).state, 'busy');
  assert.strictEqual(frameForEvent('PreToolUse', null, NOW).state, 'busy');
  assert.strictEqual(frameForEvent('PermissionRequest', null, NOW).state, 'waiting');
  assert.strictEqual(frameForEvent('PostToolUse', null, NOW).state, 'busy');
  assert.strictEqual(frameForEvent('Stop', null, NOW).state, 'done');
  assert.strictEqual(frameForEvent('SessionEnd', null, NOW).state, 'idle');
  assert.strictEqual(frameForEvent('Interrupt', null, NOW).state, 'idle');
});

test('an event we do not render produces no frame at all', () => {
  assert.strictEqual(frameForEvent('SomethingNew', null, NOW), null);
  assert.strictEqual(frameForEvent('', null, NOW), null);
});

test('the waiting frame names no tool and quotes no command', () => {
  const f = frameForEvent('PermissionRequest', { ctxFill: 0.5 }, NOW);
  assert.strictEqual(f.state, 'waiting');
  assert.strictEqual(f.sub, 'your turn');
});

test('frameToLine clamps every field to the firmware buffer sizes', () => {
  const line = frameToLine({
    state: 'busy',
    ring: 5,
    center: '0123456789abcdefghij',
    label: 'x'.repeat(40),
    sub: 'y'.repeat(80),
    tps: 17.34
  });
  const f = JSON.parse(line);
  assert.strictEqual(f.ring, 1);
  assert.strictEqual(f.center.length, 15);
  assert.strictEqual(f.label.length, 23);
  assert.strictEqual(f.sub.length, 39);
  assert.strictEqual(f.tps, 17.3);
  assert.ok(line.endsWith('\n'));
});

test('frameToLine drops an unknown state rather than sending a line the board discards', () => {
  const f = JSON.parse(frameToLine({ state: 'thinking', ring: 0.5 }));
  assert.strictEqual(f.state, undefined);
  assert.strictEqual(f.ring, 0.5);
});

test('no frame this program can build exceeds the 512 byte line cap', () => {
  const line = frameToLine({
    state: 'waiting',
    ring: 0.123456789,
    center: 'x'.repeat(200),
    label: 'y'.repeat(200),
    sub: 'z'.repeat(200),
    tps: 9999.99
  });
  assert.ok(Buffer.byteLength(line) <= MAX_LINE_BYTES, `line was ${Buffer.byteLength(line)} bytes`);
});

test('the metric-only frame carries no state, so the hook keeps owning it', () => {
  const f = metricFields({ ctxFill: 0.4 }, NOW);
  assert.strictEqual(f.state, undefined);
  assert.strictEqual(JSON.parse(frameToLine(f)).state, undefined);
});

test('the stats fields ride along on every metric frame', () => {
  const f = metricFields({ ctxFill: 0.4, totalTokens: 812345 }, NOW);
  // Local, not UTC: the board asks "has midnight happened where the owner is",
  // and it has no timezone database to answer that from UTC.
  const offsetSec = new Date(NOW).getTimezoneOffset() * 60;
  assert.strictEqual(f.time, Math.floor(NOW / 1000) - offsetSec);
  assert.strictEqual(f.tokens, 812345);
});

test('a session with no token total sends no token field at all', () => {
  const f = metricFields({ ctxFill: 0.4 }, NOW);
  assert.ok(Object.prototype.hasOwnProperty.call(f, 'time'));
  assert.strictEqual(f.tokens, undefined);
});

test('frameToLine keeps time and tokens as integers and drops nonsense', () => {
  const good = JSON.parse(
    frameToLine({ state: 'busy', time: 1788850592.7, tokens: 1234567.9 })
  );
  assert.strictEqual(good.time, 1788850592);
  assert.strictEqual(good.tokens, 1234567);

  const bad = JSON.parse(
    frameToLine({ state: 'busy', time: 0, tokens: -5 })
  );
  assert.strictEqual(bad.time, undefined);
  assert.strictEqual(bad.tokens, undefined);
});
