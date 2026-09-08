'use strict';

/**
 * The one-shot commands, as a person types them.
 *
 * Every one of these is a door onto a firmware field, so the thing worth
 * asserting is the exact bytes it puts on the wire: the port is pointed at an
 * ordinary file and the line is read back out of it. The other half is what a
 * bad argument costs, which must always be a usage message and never a line
 * the board would drop.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { SAY_DEFAULT_SECS, FOCUS_MAX_MINUTES, NAME_MAX } = require('../codex-companion');

const CLI = path.join(__dirname, '..', 'codex-companion.js');

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-cmd-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function run(args) {
  return spawnSync(process.execPath, [CLI, ...args], {
    encoding: 'utf8',
    timeout: 30000,
    input: ''
  });
}

/** Run a command with the port pointed at a file, and return the line sent. */
function sent(t, args) {
  const port = path.join(tmpdir(t), 'fake-port');
  fs.writeFileSync(port, '');
  const res = run([...args, '--port', port]);
  const first = fs.readFileSync(port, 'utf8').split('\n')[0];
  return { res, line: first, frame: first ? JSON.parse(first) : null };
}

// ------------------------------------------------------------------ say

test('say puts a sanitised, capped message on the wire', (t) => {
  const { res, frame } = sent(t, ['say', 'back in five']);
  assert.strictEqual(res.status, 0);
  assert.deepStrictEqual(frame, { say: 'back in five' });
});

test('say --secs rides along, and 0 survives as the hold sentinel', (t) => {
  assert.deepStrictEqual(sent(t, ['say', 'in a meeting', '--secs', '0']).frame, {
    say: 'in a meeting',
    saysecs: 0
  });
  assert.deepStrictEqual(sent(t, ['say', 'brb', '--secs', '300']).frame, {
    say: 'brb',
    saysecs: 300
  });
});

test('a negative --secs falls back to the default instead of holding forever', (t) => {
  // Clamping into 0 would turn a typo into a card nobody can get rid of
  // without walking to the desk.
  assert.deepStrictEqual(sent(t, ['say', 'oops', '--secs', '-5']).frame, {
    say: 'oops',
    saysecs: SAY_DEFAULT_SECS
  });
});

test('say --clear sends the empty string that takes the card down', (t) => {
  assert.deepStrictEqual(sent(t, ['say', '--clear']).frame, { say: '' });
});

test('say with nothing to say prints usage and sends nothing', (t) => {
  const { res, line } = sent(t, ['say']);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /usage: codex-companion say/);
  assert.strictEqual(line, '');
});

test('say says out loud what it had to throw away', () => {
  const res = run(['say', 'caf\u00e9 \u{1F680}', '--dry-run']);
  assert.strictEqual(res.status, 0);
  assert.match(res.stdout, /ASCII only/);
  assert.match(res.stdout, /\{"say":"caf"\}/);
});

// ----------------------------------------------------------------- name

test('name is clamped to what the board can store', (t) => {
  const { frame } = sent(t, ['name', 'a name that is very much longer than the board can hold']);
  assert.ok(frame.name.length <= NAME_MAX);
  assert.ok(!/\s$/.test(frame.name));
});

test('name --clear removes the stored name', (t) => {
  assert.deepStrictEqual(sent(t, ['name', '--clear']).frame, { name: '' });
});

test('name tells you how to prove it stuck', (t) => {
  const { res } = sent(t, ['name', 'Alex Rivera']);
  assert.strictEqual(res.status, 0);
  assert.match(res.stdout, /greeting reads it back/);
});

// ----------------------------------------------------------------- face

test('face normalises the case the firmware compares exactly', (t) => {
  assert.deepStrictEqual(sent(t, ['face', 'BeAr']).frame, { face: 'bear' });
});

test('an unknown face is refused here rather than dropped there', (t) => {
  const { res, line } = sent(t, ['face', 'dinosaur']);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /rounded\|bear\|arc/);
  assert.strictEqual(line, '', 'a line the board would discard was sent anyway');
});

// ----------------------------------------------------------------- time

test('time sends a plausible local wall clock and nothing else', (t) => {
  const { res, frame } = sent(t, ['time']);
  assert.strictEqual(res.status, 0);
  assert.deepStrictEqual(Object.keys(frame), ['time']);
  // Local, not UTC, and within a day of now whichever timezone this runs in.
  assert.ok(Math.abs(frame.time - Math.floor(Date.now() / 1000)) < 86400 + 60);
});

test('time takes an optional token total for the daily figures', (t) => {
  const { frame } = sent(t, ['time', '1209000']);
  assert.strictEqual(frame.tokens, 1209000);
  assert.ok(frame.time > 0);
});

// ------------------------------------------------- the second-door commands

test('every gesture on the board has a command that mirrors it', (t) => {
  const cases = [
    [['dnd', 'on'], { dnd: true }],
    [['dnd', 'off'], { dnd: false }],
    [['stats', 'on'], { stats: 'on' }],
    [['stats', 'off'], { stats: 'off' }],
    [['play', 'on'], { play: 'on' }],
    [['play', 'hop'], { play: 'hop' }],
    [['play', 'off'], { play: 'off' }],
    [['focus', 'show'], { focus: 'show' }],
    [['focus', 'start', '--mins', '45'], { focus: 'start', focusmins: 45 }],
    [['focus', 'reset'], { focus: 'reset' }],
    [['firstrun'], { firstrun: 'play' }]
  ];
  for (const [args, frame] of cases) {
    const got = sent(t, args);
    assert.strictEqual(got.res.status, 0, `${args.join(' ')} exited ${got.res.status}`);
    assert.deepStrictEqual(got.frame, frame, args.join(' '));
  }
});

test('--mins is clamped to the firmware range', (t) => {
  assert.strictEqual(sent(t, ['focus', 'start', '--mins', '9999']).frame.focusmins, FOCUS_MAX_MINUTES);
});

test('a bad verb prints usage and sends nothing at all', (t) => {
  for (const args of [['dnd', 'maybe'], ['stats', 'sometimes'], ['play'], ['focus', 'rewind']]) {
    const got = sent(t, args);
    assert.strictEqual(got.res.status, 1, args.join(' '));
    assert.match(got.res.stdout, /usage: codex-companion/);
    assert.strictEqual(got.line, '', `${args.join(' ')} sent something`);
  }
});

// ------------------------------------------------------------------ misc

test('there is deliberately no factory-reset command', () => {
  const res = run(['reset', 'factory', '--dry-run']);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /no factory-reset command/);
});

test('an unknown command prints usage and fails', () => {
  const res = run(['frobnicate']);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /codex-companion - desk companion/);
});

test('help lists every command that exists', () => {
  const res = run(['help']);
  assert.strictEqual(res.status, 0);
  for (const cmd of ['say', 'name', 'face', 'time', 'dnd', 'stats', 'focus', 'play', 'selftest']) {
    assert.match(res.stdout, new RegExp(`codex-companion ${cmd}\\b`), `help omits ${cmd}`);
  }
});

test('a write the board refuses blames the board, not a second holder', (t) => {
  // A macOS callout port does not lock, so a second holder is a reader that
  // steals answers and can never make our write fail. The only thing that can
  // is the far end refusing the bytes, which a FIFO nobody is reading imitates
  // exactly once its buffer is full: EAGAIN until the deadline, same as a
  // board that stopped draining the tty. The remedy has to point there.
  const fifo = path.join(tmpdir(t), 'wedged.fifo');
  const made = spawnSync('/usr/bin/mkfifo', [fifo], { encoding: 'utf8' });
  assert.strictEqual(made.status, 0, 'mkfifo failed, so this test proves nothing');
  const fd = fs.openSync(fifo, fs.constants.O_RDWR | fs.constants.O_NONBLOCK);
  t.after(() => fs.closeSync(fd));
  const block = Buffer.alloc(4096, 0x78);
  for (let i = 0; i < 4096; i += 1) {
    try {
      fs.writeSync(fd, block);
    } catch {
      break; // full: every write from here on gets EAGAIN
    }
  }

  const res = run(['say', 'hello', '--port', fifo]);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /would not take the line/);
  assert.match(res.stdout, /stopped reading/);
  assert.match(res.stdout, /doctor/);
  assert.doesNotMatch(res.stdout, /something else holding/i);
});

test('with no device a command says so and points at doctor', () => {
  const res = run(['say', 'hello', '--port', '/dev/cu.definitely-not-here']);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /no device/);
  assert.match(res.stdout, /doctor/);
  // The errno, not choosePort's explanation of why it did not check the
  // identity of a port you named yourself.
  assert.match(res.stdout, /ENOENT/);
  assert.match(res.stdout, /vanished|Replug/);
  assert.doesNotMatch(res.stdout, /identity not checked/);
});

test('a named port that will not open reports why, not "identity not checked"', (t) => {
  const dir = tmpdir(t);
  const unreadable = path.join(dir, 'no-permission');
  fs.writeFileSync(unreadable, '');
  fs.chmodSync(unreadable, 0o000);
  if (process.getuid && process.getuid() === 0) return; // root can open anything

  const res = run(['say', 'hello', '--port', unreadable]);
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /permission denied on the port/);
  assert.match(res.stdout, /EACCES/);
  assert.match(res.stdout, /Never chmod a device node/);
  assert.match(res.stdout, /doctor/);
  assert.doesNotMatch(res.stdout, /identity not checked/);
});
