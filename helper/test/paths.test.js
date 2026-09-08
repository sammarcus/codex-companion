'use strict';

/**
 * Where this program is allowed to write, enforced rather than promised.
 *
 * There are exactly two destinations: the serial port, and
 * <CODEX_HOME>/hooks.json from install-hook and uninstall-hook. The README
 * makes that claim to whoever has to approve installing this on a work
 * machine, so it is checked here twice: against the source, so a new call site
 * cannot appear unnoticed, and against a scratch directory, so the program is
 * watched doing it.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const CLI = path.join(__dirname, '..', 'codex-companion.js');
const SOURCE = fs.readFileSync(CLI, 'utf8');

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-paths-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

/**
 * CODEX_HOME is deliberately NOT put in the child's environment. doctor runs
 * `codex --version`, and the real Codex CLI initialises whatever CODEX_HOME
 * points at, which would be Codex writing into the scratch directory rather
 * than this program doing it. `--codex-home` is the flag under test anyway.
 */
function run(args) {
  return spawnSync(process.execPath, [CLI, ...args], {
    encoding: 'utf8',
    timeout: 30000,
    input: ''
  });
}

/** Every file under `dir`, relative and sorted. */
function tree(dir) {
  const out = [];
  const walk = (base, prefix) => {
    for (const entry of fs.readdirSync(base, { withFileTypes: true })) {
      const rel = prefix ? path.join(prefix, entry.name) : entry.name;
      if (entry.isDirectory()) walk(path.join(base, entry.name), rel);
      else out.push(rel);
    }
  };
  walk(dir, '');
  return out.sort();
}

// ----------------------------------------------------------- the source

test('there is no way to append, truncate or stream a file anywhere in here', () => {
  for (const api of [
    'appendFile',
    'createWriteStream',
    'truncate',
    'ftruncate',
    'copyFile',
    'rename',
    'rmdir',
    'rmSync',
    'symlink',
    'chmod',
    'chown',
    'utimes'
  ]) {
    assert.ok(!SOURCE.includes(`fs.${api}`), `the source calls fs.${api}`);
  }
});

test('the only writing calls are the ones that own hooks.json', () => {
  // writeFileSync and unlinkSync appear once each, and mkdirSync once, all
  // three inside install-hook / uninstall-hook. If that ever stops being
  // true, this test is the thing that says so.
  const count = (needle) => SOURCE.split(needle).length - 1;
  assert.strictEqual(count('fs.writeFileSync('), 2, 'fs.writeFileSync call sites moved');
  assert.strictEqual(count('fs.unlinkSync('), 1, 'fs.unlinkSync call sites moved');
  assert.strictEqual(count('fs.mkdirSync('), 1, 'fs.mkdirSync call sites moved');
  // And each one names hooks.json's own variable rather than a path it built.
  for (const call of ['fs.writeFileSync(file, text)', 'fs.unlinkSync(file)']) {
    assert.ok(SOURCE.includes(call), `expected ${call}`);
  }
});

test('the only other descriptor it ever opens is the port and files it reads', () => {
  // openSync appears twice: the port (O_WRONLY or O_RDWR) and the rollout
  // tail (O_RDONLY, 'r'). Nothing else opens anything.
  const opens = SOURCE.split('fs.openSync(').length - 1;
  assert.strictEqual(opens, 2, `fs.openSync call sites moved (${opens})`);
  assert.ok(SOURCE.includes("fs.openSync(file, 'r')"), 'the rollout tail is read-only');
});

test('it still cannot reach the network, whatever else it grew', () => {
  for (const mod of [
    'http',
    'https',
    'net',
    'tls',
    'dns',
    'dgram',
    'node:http',
    'node:https',
    'node:net',
    'node:tls',
    'node:dns',
    'node:dgram'
  ]) {
    assert.ok(!SOURCE.includes(`require('${mod}')`), `it requires ${mod}`);
    assert.ok(!SOURCE.includes(`require("${mod}")`), `it requires ${mod}`);
  }
  assert.ok(!/\bfetch\s*\(/.test(SOURCE), 'it calls fetch');
  assert.ok(!/XMLHttpRequest|WebSocket|http:\/\/|https:\/\//.test(SOURCE));
});

test('it still starts nothing that outlives the command you typed', () => {
  for (const word of ['launchctl', 'LaunchAgents', 'systemctl', 'crontab', 'plist', 'nohup']) {
    assert.ok(!SOURCE.includes(word), `it mentions ${word}`);
  }
  // The only programs it ever runs are these four, all read-only. lsof is
  // there because a macOS callout port does not lock, so a second program on
  // the cable can only be seen by asking.
  const execs = SOURCE.match(/execFileSync\(\s*'([^']+)'/g) || [];
  const named = SOURCE.match(/execFileSync\(\s*'([^']+)'/g).map((m) => m.split("'")[1]);
  assert.strictEqual(execs.length, named.length);
  for (const bin of named) {
    assert.ok(
      ['/usr/sbin/ioreg', '/bin/stty', '/usr/sbin/lsof', 'codex'].includes(bin),
      `it runs an unexpected program: ${bin}`
    );
  }
});

test('the whole program is still four Node built-ins and nothing else', () => {
  const required = (SOURCE.match(/require\('([^']+)'\)/g) || []).map((m) => m.split("'")[1]);
  assert.deepStrictEqual(
    [...new Set(required)].sort(),
    ['node:child_process', 'node:fs', 'node:os', 'node:path']
  );
});

// ------------------------------------------------------ watching it write

test('install-hook creates exactly one file, and uninstall removes exactly it', (t) => {
  const home = tmpdir(t);
  assert.deepStrictEqual(tree(home), []);
  assert.strictEqual(run(['install-hook', '--codex-home', home]).status, 0);
  assert.deepStrictEqual(tree(home), ['hooks.json']);
  assert.strictEqual(run(['uninstall-hook', '--codex-home', home]).status, 0);
  assert.deepStrictEqual(tree(home), []);
});

test('nothing else this program does writes anything at all', (t) => {
  const home = tmpdir(t);
  const dir = tmpdir(t);
  const port = path.join(dir, 'fake-port');
  fs.writeFileSync(port, '');

  const commands = [
    ['help'],
    ['doctor', '--codex-home', home, '--port', '/dev/cu.definitely-not-here'],
    ['demo', '--dry-run'],
    ['say', 'hello', '--port', port],
    ['name', 'Alex', '--port', port],
    ['face', 'bear', '--port', port],
    ['time', '--port', port],
    ['dnd', 'on', '--port', port],
    ['stats', 'off', '--port', port],
    ['focus', 'hide', '--port', port],
    ['play', 'off', '--port', port],
    ['firstrun', '--port', port],
    ['install-hook', '--codex-home', home, '--dry-run']
  ];
  for (const args of commands) {
    run(args);
    assert.deepStrictEqual(tree(home), [], `${args[0]} wrote into CODEX_HOME`);
  }
  // The port took the lines and nothing created a neighbour beside it. (Each
  // command opens the port fresh and writes from offset zero, which is right
  // for a tty and means a plain file only keeps the last line written.)
  assert.deepStrictEqual(tree(dir), ['fake-port']);
  const wire = fs.readFileSync(port, 'utf8').trim();
  assert.ok(wire.length > 0, 'nothing ever reached the port');
  assert.doesNotThrow(() => JSON.parse(wire.split('\n')[0]));
});

test('the package still installs nothing', () => {
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8'));
  assert.strictEqual(pkg.dependencies, undefined);
  assert.strictEqual(pkg.devDependencies, undefined);
  assert.strictEqual(pkg.optionalDependencies, undefined);
  assert.strictEqual(pkg.bundleDependencies, undefined);
  assert.strictEqual(pkg.scripts.postinstall, undefined);
  assert.strictEqual(pkg.scripts.preinstall, undefined);
  // npm writes a lock file even with nothing to lock. If one is here, the
  // thing that matters is that it locks nothing but this package itself.
  const lock = path.join(__dirname, '..', 'package-lock.json');
  if (fs.existsSync(lock)) {
    const packages = Object.keys(JSON.parse(fs.readFileSync(lock, 'utf8')).packages || {});
    assert.deepStrictEqual(packages, [''], `the lock file has dependencies: ${packages}`);
  }
});

test('the helper is still one source file', () => {
  const files = fs
    .readdirSync(path.join(__dirname, '..'))
    .filter((n) => n.endsWith('.js'))
    .sort();
  assert.deepStrictEqual(files, ['codex-companion.js']);
});
