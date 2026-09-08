'use strict';

/**
 * The fields the firmware grew after this program was first written.
 *
 * The firmware has exactly two rules and this file is about the first one:
 * an ENUM the device does not recognise makes the WHOLE line malformed, so a
 * frame that carries one is silently dropped along with every good field
 * riding beside it. That is why `frameToLine` refuses to emit an enum it
 * cannot vouch for, and why most of the assertions below are about a field
 * being ABSENT from the wire rather than present on it.
 *
 * The limits come from firmware/src/main.cpp: OWNER_NAME_MAX, SAY_MAX,
 * SAY_MAX_S, SAY_DEFAULT_S, CLOCK_MIN_EPOCH, FOCUS_MIN_MIN, FOCUS_MAX_MIN, and
 * the registry in firmware/src/Faces.cpp.
 */

const test = require('node:test');
const assert = require('node:assert');

const {
  frameToLine,
  frameForEvent,
  sanitizeAscii,
  asciiNotes,
  clampBytes,
  pickEnum,
  clampInt,
  stateForEvent,
  localUnixSeconds,
  MAX_LINE_BYTES,
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
} = require('../codex-companion');

const wire = (frame) => JSON.parse(frameToLine(frame));

// ---------------------------------------------------------------- the enums

test('every registered face is accepted, in any case', () => {
  for (const face of FACE_NAMES) {
    assert.strictEqual(wire({ face }).face, face);
    assert.strictEqual(wire({ face: face.toUpperCase() }).face, face);
  }
  assert.strictEqual(wire({ face: '  BeAr  ' }).face, 'bear');
});

test('an unregistered face is never put on the wire', () => {
  // The board would drop the whole line, so `label` would vanish with it.
  const f = wire({ face: 'dinosaur', label: 'CTX' });
  assert.strictEqual(f.face, undefined);
  assert.strictEqual(f.label, 'CTX');
});

test('every string enum drops a value the firmware would refuse', () => {
  const cases = [
    ['play', PLAY_VERBS],
    ['stats', STATS_VERBS],
    ['focus', FOCUS_VERBS],
    ['reset', RESET_VERBS],
    ['firstrun', FIRSTRUN_VERBS]
  ];
  for (const [field, allowed] of cases) {
    for (const good of allowed) {
      assert.strictEqual(wire({ [field]: good })[field], good, `${field}=${good}`);
    }
    // Case and stray spaces are normalised rather than refused, so what
    // reaches the wire is always the exact spelling the firmware strcmps.
    for (const good of allowed) {
      assert.strictEqual(wire({ [field]: ` ${good.toUpperCase()} ` })[field], good);
    }
    for (const bad of ['', 'nope', 'on off', 42, null, {}, [], true]) {
      const f = wire({ [field]: bad, label: 'CTX' });
      assert.strictEqual(f[field], undefined, `${field} let ${JSON.stringify(bad)} through`);
      assert.strictEqual(f.label, 'CTX', 'the rest of the frame survived');
    }
  }
});

test('no garbage in any enum field can ever reach the wire', () => {
  const junk = [
    'sleep ',
    'IDLE\n',
    '__proto__',
    'constructor',
    'toString',
    0,
    -1,
    NaN,
    Infinity,
    undefined,
    null,
    [],
    {},
    () => {}
  ];
  const enums = {
    state: ['sleep', 'idle', 'busy', 'waiting', 'done'],
    face: FACE_NAMES,
    play: PLAY_VERBS,
    stats: STATS_VERBS,
    focus: FOCUS_VERBS,
    reset: RESET_VERBS,
    firstrun: FIRSTRUN_VERBS
  };
  for (const [field, allowed] of Object.entries(enums)) {
    for (const bad of junk) {
      const value = wire({ [field]: bad })[field];
      assert.ok(
        value === undefined || allowed.includes(value),
        `${field} emitted ${JSON.stringify(value)} for ${String(bad)}`
      );
    }
  }
});

// ------------------------------------------------------------- say and name

test('a message is sanitised to what the panel can actually draw', () => {
  assert.strictEqual(wire({ say: 'brb' }).say, 'brb');
  // Non-ASCII is dropped rather than mangled: the fonts have no glyphs for it.
  assert.strictEqual(wire({ say: 'caf\u00e9 cr\u00e8me \u{1F680}' }).say, 'caf crme');
  // Whitespace collapses and the ends are trimmed.
  assert.strictEqual(wire({ say: '  in   a\tmeeting\n' }).say, 'in a meeting');
  // Control bytes go.
  assert.strictEqual(wire({ say: 'ab' }).say, 'ab');
});

test('a message is capped at the firmware buffer, with no trailing space', () => {
  const long = 'word '.repeat(40);
  const said = wire({ say: long }).say;
  assert.ok(said.length <= SAY_MAX, `${said.length} characters`);
  assert.ok(!/\s$/.test(said), `trailing whitespace in ${JSON.stringify(said)}`);
});

test('an empty message is sent, because empty is how a card is cleared', () => {
  const f = wire({ say: '' });
  assert.ok(Object.prototype.hasOwnProperty.call(f, 'say'));
  assert.strictEqual(f.say, '');
  // The same for a message that sanitises away to nothing.
  assert.strictEqual(wire({ say: '\u{1F680}\u{1F680}' }).say, '');
});

test('a name is capped at the firmware buffer and never left mid-space', () => {
  const f = wire({ name: 'this name is far longer than twenty four characters' });
  assert.ok(f.name.length <= NAME_MAX, `${f.name.length} characters`);
  assert.ok(!/\s$/.test(f.name));
  assert.strictEqual(wire({ name: '' }).name, '');
});

test('asciiNotes tells truncation and dropping apart', () => {
  // The two are different pieces of news and the CLI reports the right one.
  assert.deepStrictEqual(asciiNotes('plain', 24), { dropped: false, truncated: false });
  assert.deepStrictEqual(asciiNotes('caf\u00e9', 24), { dropped: true, truncated: false });
  assert.deepStrictEqual(asciiNotes('x'.repeat(30), 24), { dropped: false, truncated: true });
  // Dropped and truncated are independent: 30 accents sanitise away to
  // nothing at all, so nothing is left to cut.
  assert.deepStrictEqual(asciiNotes('\u00e9'.repeat(30), 24), { dropped: true, truncated: false });
  assert.deepStrictEqual(asciiNotes(`\u00e9${'x'.repeat(30)}`, 24), {
    dropped: true,
    truncated: true
  });
  assert.deepStrictEqual(asciiNotes(42, 24), { dropped: false, truncated: false });
});

// ------------------------------------------------------------------ saysecs

test('saysecs keeps zero, because zero is the hold-until-cleared sentinel', () => {
  assert.strictEqual(wire({ say: 'x', saysecs: 0 }).saysecs, 0);
  assert.strictEqual(wire({ saysecs: 300 }).saysecs, 300);
  assert.strictEqual(wire({ saysecs: SAY_MAX_SECS + 10000 }).saysecs, SAY_MAX_SECS);
});

test('a negative saysecs becomes the default and never the sentinel', () => {
  // Clamping into 0 would turn a host arithmetic bug into a card that never
  // leaves the screen, which is the one thing this field must not do.
  assert.strictEqual(wire({ saysecs: -1 }).saysecs, SAY_DEFAULT_SECS);
  assert.strictEqual(wire({ saysecs: -99999 }).saysecs, SAY_DEFAULT_SECS);
});

test('a wrong-typed saysecs is left off rather than guessed at', () => {
  assert.strictEqual(wire({ saysecs: 'soon', say: 'x' }).saysecs, undefined);
  assert.strictEqual(wire({ saysecs: NaN }).saysecs, undefined);
  assert.strictEqual(wire({ saysecs: true }).saysecs, undefined);
  assert.strictEqual(wire({ say: 'x' }).saysecs, undefined);
});

// ---------------------------------------------------------------------- dnd

test('dnd goes on the wire only as a real boolean', () => {
  assert.strictEqual(wire({ dnd: true }).dnd, true);
  assert.strictEqual(wire({ dnd: false }).dnd, false);
  for (const bad of ['true', 1, 0, null, undefined, {}]) {
    assert.strictEqual(wire({ dnd: bad }).dnd, undefined, String(bad));
  }
});

// -------------------------------------------------------------- focus timer

test('focusmins is clamped rather than rejected, like the firmware does', () => {
  assert.strictEqual(wire({ focusmins: 45 }).focusmins, 45);
  assert.strictEqual(wire({ focusmins: 0 }).focusmins, FOCUS_MIN_MINUTES);
  assert.strictEqual(wire({ focusmins: 9999 }).focusmins, FOCUS_MAX_MINUTES);
  assert.strictEqual(wire({ focusmins: 25.6 }).focusmins, 26);
  assert.strictEqual(wire({ focusmins: 'long' }).focusmins, undefined);
});

test('a focus verb and a length ride the same line', () => {
  const f = wire({ focus: 'start', focusmins: 45 });
  assert.strictEqual(f.focus, 'start');
  assert.strictEqual(f.focusmins, 45);
});

// ---------------------------------------------------------- time and tokens

test('a time below the firmware epoch is left off entirely', () => {
  // The board ignores anything before 2020-01-01, so sending one is a lie
  // about having told it the time.
  assert.strictEqual(wire({ time: 0 }).time, undefined);
  assert.strictEqual(wire({ time: 1000 }).time, undefined);
  assert.strictEqual(wire({ time: CLOCK_MIN_EPOCH }).time, CLOCK_MIN_EPOCH);
  assert.strictEqual(wire({ time: -5 }).time, undefined);
});

test('localUnixSeconds is local, and is a plausible wall clock', () => {
  const now = Date.parse('2026-09-08T15:04:05.000Z');
  const offsetSec = new Date(now).getTimezoneOffset() * 60;
  assert.strictEqual(localUnixSeconds(now), Math.floor(now / 1000) - offsetSec);
  assert.ok(localUnixSeconds(now) > CLOCK_MIN_EPOCH);
});

test('tokens are whole numbers and never negative', () => {
  assert.strictEqual(wire({ tokens: 1209000.7 }).tokens, 1209000);
  assert.strictEqual(wire({ tokens: 0 }).tokens, 0);
  assert.strictEqual(wire({ tokens: -1 }).tokens, undefined);
  assert.strictEqual(wire({ tokens: 'lots' }).tokens, undefined);
});

// ------------------------------------------------------- the whole envelope

test('a frame carrying every field at once still fits one line', () => {
  const line = frameToLine({
    state: 'waiting',
    ring: 0.62,
    center: '62%',
    label: 'CTX',
    sub: 'your turn',
    tps: 17.3,
    say: 'x'.repeat(200),
    saysecs: 300,
    dnd: true,
    name: 'y'.repeat(200),
    face: 'bear',
    play: 'hop',
    stats: 'on',
    focus: 'start',
    focusmins: 45,
    reset: 'firstrun',
    firstrun: 'play',
    time: 1788850592,
    tokens: 1209000
  });
  assert.ok(Buffer.byteLength(line) <= MAX_LINE_BYTES, `${Buffer.byteLength(line)} bytes`);
  assert.ok(line.endsWith('\n'));
  assert.strictEqual(JSON.parse(line).state, 'waiting');
});

test('when a line has to shrink, the state is the last thing to go', () => {
  // A line over 512 bytes is discarded WHOLE by the board, so something has to
  // be dropped, and the one field this object exists to show is the one worth
  // arriving alone.
  const line = frameToLine({
    state: 'waiting',
    sub: 'z'.repeat(400),
    center: 'c'.repeat(400),
    label: 'l'.repeat(400),
    say: 's'.repeat(400),
    name: 'n'.repeat(400)
  });
  assert.ok(Buffer.byteLength(line) <= MAX_LINE_BYTES);
  assert.strictEqual(JSON.parse(line).state, 'waiting');
});

test('frameToLine survives being handed nonsense instead of a frame', () => {
  for (const bad of [null, undefined, 42, 'busy', [], () => {}]) {
    const line = frameToLine(bad);
    assert.ok(line.endsWith('\n'));
    assert.doesNotThrow(() => JSON.parse(line));
  }
});

// ------------------------------------------------------------ the small fry

test('sanitizeAscii and clampBytes count the units they claim to', () => {
  assert.strictEqual(sanitizeAscii('abc', 2), 'ab');
  assert.strictEqual(sanitizeAscii(null, 10), '');
  // clampBytes counts BYTES and never cuts a character in half, so a string
  // that passes a character check cannot still overrun a char[N] on the board.
  assert.strictEqual(clampBytes('abcdef', 3), 'abc');
  assert.strictEqual(Buffer.byteLength(clampBytes('\u00e9\u00e9\u00e9\u00e9', 3)) <= 3, true);
  assert.strictEqual(clampBytes('ok', 99), 'ok');
});

test('pickEnum and clampInt are the boring functions they look like', () => {
  assert.strictEqual(pickEnum(' ON ', ['on', 'off']), 'on');
  assert.strictEqual(pickEnum('maybe', ['on', 'off']), null);
  assert.strictEqual(pickEnum(5, ['on']), null);
  assert.strictEqual(clampInt('7', 1, 10), 7);
  assert.strictEqual(clampInt(99, 1, 10), 10);
  assert.strictEqual(clampInt(true, 1, 10), null);
  assert.strictEqual(clampInt('', 1, 10), null); // Number('') is 0, which is finite
  assert.strictEqual(clampInt(undefined, 1, 10), null);
});

// --------------------------------------------------- the event name lookup

test('an inherited property is never mistaken for a hook event', () => {
  // hook_event_name comes straight off stdin. On a plain object literal,
  // EVENT_STATE['__proto__'] answers with Object.prototype and
  // EVENT_STATE['constructor'] with a function, both truthy, and a frame would
  // go out for an event that does not exist.
  for (const name of ['__proto__', 'constructor', 'toString', 'hasOwnProperty', 'valueOf']) {
    assert.strictEqual(stateForEvent(name), null, name);
    assert.strictEqual(frameForEvent(name, null, Date.now()), null, name);
  }
  assert.strictEqual(stateForEvent(null), null);
  assert.strictEqual(stateForEvent(42), null);
  assert.strictEqual(stateForEvent('Stop'), 'done');
});
