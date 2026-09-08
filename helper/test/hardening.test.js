'use strict';

/**
 * The failure modes that do not show up as a wrong pixel.
 *
 * A desk toy that hangs a Codex turn, spins a core, or writes somewhere it was
 * never allowed to is worse than a desk toy that does nothing, so each of
 * these is about the program failing in a bounded, boring way.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const { Link, MAX_LINE_BYTES } = require('../codex-companion');

const CLI = path.join(__dirname, '..', 'codex-companion.js');

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-hard-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

// ------------------------------------------------------------- the hook

test('a hook whose stdin never closes gives up instead of hanging', (t) => {
  // The old implementation was fs.readFileSync(0), which has no way out: a
  // caller that opens the pipe and never closes it wedges the hook
  // synchronously, where no timer can reach it. Codex runs most of these
  // async with a 5 second budget, so a wedge is a leaked process per event.
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  const child = spawn(process.execPath, [CLI, 'hook', '--no-metrics', '--port', port], {
    stdio: ['pipe', 'pipe', 'pipe']
  });
  // Write a valid payload but deliberately never end the stream.
  child.stdin.write(JSON.stringify({ hook_event_name: 'Stop' }));

  return new Promise((resolve, reject) => {
    const started = Date.now();
    const killer = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new Error('the hook never exited with stdin held open'));
    }, 8000);
    child.on('exit', (code) => {
      clearTimeout(killer);
      const took = Date.now() - started;
      try {
        assert.strictEqual(code, 0, 'a hook must never fail its caller');
        assert.ok(took < 6000, `took ${took}ms to give up`);
        // It still did its job with what it had.
        assert.match(fs.readFileSync(port, 'utf8'), /"state":"done"/);
        resolve();
      } catch (e) {
        reject(e);
      } finally {
        try {
          child.stdin.end();
        } catch {
          /* already gone */
        }
      }
    });
  });
});

test('a huge payload is capped rather than swallowed whole', (t) => {
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  // 8MB of junk. It cannot parse, so nothing is sent, and the point is that
  // it exits 0 quickly instead of deciding how much memory a desk toy gets.
  const res = spawnSync(process.execPath, [CLI, 'hook', '--no-metrics', '--port', port], {
    input: `{"hook_event_name":"Stop","junk":"${'x'.repeat(8 * 1024 * 1024)}"}`,
    encoding: 'utf8',
    timeout: 20000
  });
  assert.strictEqual(res.status, 0);
  assert.strictEqual(res.stdout, '');
  assert.strictEqual(res.stderr, '');
});

test('a payload naming an inherited property produces no frame', (t) => {
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  for (const name of ['__proto__', 'constructor', 'toString']) {
    const res = spawnSync(process.execPath, [CLI, 'hook', '--no-metrics', '--port', port], {
      input: JSON.stringify({ hook_event_name: name }),
      encoding: 'utf8',
      timeout: 15000
    });
    assert.strictEqual(res.status, 0, name);
    assert.strictEqual(fs.readFileSync(port, 'utf8'), '', `${name} produced a frame`);
  }
});

// -------------------------------------------------------------- the link

test('a line the board would discard is never written', (t) => {
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  const link = new Link(port).open();
  t.after(() => link.close());
  assert.strictEqual(link.writeLine(`${'x'.repeat(MAX_LINE_BYTES + 1)}\n`, 200), false);
  assert.strictEqual(fs.readFileSync(port, 'utf8'), '');
  assert.strictEqual(link.writeLine('{}\n', 200), true);
});

test('writing to a closed link fails quietly rather than throwing', (t) => {
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  const link = new Link(port).open();
  link.close();
  assert.strictEqual(link.writeLine('{}\n', 200), false);
  link.close(); // double close is a no-op, not an error
  assert.strictEqual(link.readAvailable(), '');
});

test('a write-only link reads nothing rather than blocking on a reply', (t) => {
  // The board's transmit path runs one message behind, so a per-line
  // handshake would mis-report the last line of every burst. The default link
  // cannot even be tempted: it is not open for reading.
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, 'hello tdisplay-s3 v1 name=""\n');
  const link = new Link(port).open();
  t.after(() => link.close());
  assert.strictEqual(link.read, false);
  assert.strictEqual(link.readAvailable(), '');
});

test('readFor returns on time instead of spinning until something arrives', (t) => {
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');
  const link = new Link(port, { read: true }).open();
  t.after(() => link.close());
  const started = Date.now();
  link.readFor(300, false);
  const took = Date.now() - started;
  assert.ok(took >= 280 && took < 1500, `readFor(300) took ${took}ms`);
});

// ------------------------------------------------------------- selftest

test('selftest against something that is not a board fails and stops', (t) => {
  // The failure has to be quick and legible: a bench tool that hangs on a
  // wrong port is a bench tool nobody runs twice.
  const dir = tmpdir(t);
  const port = path.join(dir, 'not-a-board');
  fs.writeFileSync(port, '');
  const started = Date.now();
  const res = spawnSync(process.execPath, [CLI, 'selftest', '--port', port], {
    encoding: 'utf8',
    timeout: 60000,
    input: ''
  });
  assert.strictEqual(res.status, 1, 'a silent port is not a pass');
  assert.match(res.stdout, /FAIL {2}the board answers/);
  // The detail column has to agree with the verdict beside it. It used to say
  // `ack` here, next to a FAIL, because it was computed off the greeting test
  // alone and silence fell through to the weaker of the two success words.
  assert.match(res.stdout, /FAIL {2}the board answers +silence/);
  assert.doesNotMatch(res.stdout, /FAIL {2}the board answers +(ack|greeting)/);
  assert.match(res.stdout, /the rest was not attempted/);
  assert.match(res.stdout, /checks,/);
  // Seventeen more probes against something that is not talking would all
  // fail for the same reason and take half a minute to say so.
  assert.ok(Date.now() - started < 20000, 'selftest took too long to give up');
});

test('selftest with no device at all says which board it wanted', () => {
  const res = spawnSync(
    process.execPath,
    [CLI, 'selftest', '--port', '/dev/cu.definitely-not-here'],
    { encoding: 'utf8', timeout: 30000, input: '' }
  );
  assert.strictEqual(res.status, 1);
  assert.match(res.stdout, /FAIL {2}the port opens/);
  assert.match(res.stdout, /1 checks|2 checks/);
});
