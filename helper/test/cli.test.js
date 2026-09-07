'use strict';

/**
 * The command line, as a person would use it: install into a scratch
 * CODEX_HOME, look at what landed, take it back out again.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const { scanConfigToml, silencesApprovals, parseArgs } = require('../codex-companion');

const CLI = path.join(__dirname, '..', 'codex-companion.js');

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-cli-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function run(args, opts) {
  return spawnSync(process.execPath, [CLI, ...args], {
    encoding: 'utf8',
    timeout: 30000,
    input: '',
    env: Object.assign({}, process.env, (opts && opts.env) || {})
  });
}

test('the whole program has no dependencies to install', () => {
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8'));
  assert.strictEqual(pkg.dependencies, undefined);
  assert.strictEqual(pkg.devDependencies, undefined);
  assert.strictEqual(pkg.bundleDependencies, undefined);
  assert.strictEqual(pkg.scripts.postinstall, undefined);
  assert.strictEqual(pkg.scripts.preinstall, undefined);
});

test('the program never requires a network module', () => {
  const src = fs.readFileSync(CLI, 'utf8');
  for (const mod of ['http', 'https', 'net', 'tls', 'dns', 'dgram', 'node:http', 'node:net']) {
    assert.ok(!src.includes(`require('${mod}')`), `it requires ${mod}`);
    assert.ok(!src.includes(`require("${mod}")`), `it requires ${mod}`);
  }
  assert.ok(!/\bfetch\s*\(/.test(src), 'it calls fetch');
  assert.ok(!/XMLHttpRequest|WebSocket/.test(src));
});

test('nothing in the program installs a launch agent or a login item', () => {
  const src = fs.readFileSync(CLI, 'utf8');
  for (const word of ['launchctl', 'LaunchAgents', 'systemctl', 'crontab', 'plist']) {
    assert.ok(!src.includes(word), `it mentions ${word}`);
  }
});

test('help prints and changes nothing', () => {
  const res = run(['help']);
  assert.strictEqual(res.status, 0);
  assert.match(res.stdout, /codex-companion/);
  assert.match(res.stdout, /never approves or denies/);
});

test('install-hook prints the exact file before writing it, and can be a dry run', (t) => {
  const home = tmpdir(t);
  const dry = run(['install-hook', '--codex-home', home, '--dry-run']);
  assert.strictEqual(dry.status, 0);
  assert.match(dry.stdout, new RegExp(path.join(home, 'hooks.json').replace(/[.*+?^${}()|[\]\\]/g, '\\$&')));
  assert.match(dry.stdout, /"PermissionRequest"/);
  assert.match(dry.stdout, /nothing was written/);
  assert.strictEqual(fs.existsSync(path.join(home, 'hooks.json')), false);

  const real = run(['install-hook', '--codex-home', home]);
  assert.strictEqual(real.status, 0);
  const written = fs.readFileSync(path.join(home, 'hooks.json'), 'utf8');
  // What it printed is what it wrote.
  assert.ok(real.stdout.includes(written.trimEnd()));
  const parsed = JSON.parse(written);
  assert.ok(Array.isArray(parsed.hooks.PermissionRequest));
  assert.strictEqual(parsed.hooks.PermissionRequest[0].hooks[0].async, true);
});

test('install-hook is idempotent and preserves a foreign hooks.json', (t) => {
  const home = tmpdir(t);
  const file = path.join(home, 'hooks.json');
  const foreign = {
    description: 'mine',
    hooks: { PreToolUse: [{ matcher: 'shell', hooks: [{ type: 'command', command: 'audit.sh' }] }] }
  };
  fs.writeFileSync(file, JSON.stringify(foreign, null, 2));

  run(['install-hook', '--codex-home', home]);
  const second = run(['install-hook', '--codex-home', home]);
  assert.match(second.stdout, /Already registered/);

  const after = JSON.parse(fs.readFileSync(file, 'utf8'));
  assert.strictEqual(after.description, 'mine');
  assert.strictEqual(after.hooks.PreToolUse[0].hooks[0].command, 'audit.sh');

  const un = run(['uninstall-hook', '--codex-home', home]);
  assert.strictEqual(un.status, 0);
  assert.deepStrictEqual(JSON.parse(fs.readFileSync(file, 'utf8')), foreign);
});

test('uninstall-hook deletes a file that only ever held our hooks', (t) => {
  const home = tmpdir(t);
  run(['install-hook', '--codex-home', home]);
  assert.ok(fs.existsSync(path.join(home, 'hooks.json')));
  const un = run(['uninstall-hook', '--codex-home', home]);
  assert.strictEqual(un.status, 0);
  assert.strictEqual(fs.existsSync(path.join(home, 'hooks.json')), false);
});

test('uninstall-hook on a clean machine says so and does nothing', (t) => {
  const home = tmpdir(t);
  const res = run(['uninstall-hook', '--codex-home', home]);
  assert.strictEqual(res.status, 0);
  assert.match(res.stdout, /Nothing to remove/);
});

test('neither install nor uninstall touches a hooks.json it cannot parse', (t) => {
  const home = tmpdir(t);
  const file = path.join(home, 'hooks.json');
  fs.writeFileSync(file, '{ this is not json');
  assert.strictEqual(run(['install-hook', '--codex-home', home]).status, 1);
  assert.strictEqual(run(['uninstall-hook', '--codex-home', home]).status, 1);
  assert.strictEqual(fs.readFileSync(file, 'utf8'), '{ this is not json');
});

test('doctor warns about the settings that make the device look broken', () => {
  const found = scanConfigToml(`
model = "gpt-5-codex"
approval_policy = "never"

[profiles.yolo]
sandbox_mode = "danger-full-access"
`);
  assert.deepStrictEqual(found, [
    { section: '(top level)', key: 'approval_policy', value: 'never' },
    { section: 'profiles.yolo', key: 'sandbox_mode', value: 'danger-full-access' }
  ]);
  assert.ok(found.every(silencesApprovals));
});

test('doctor does not cry wolf about ordinary settings', () => {
  const found = scanConfigToml('approval_policy = "on-request"\nsandbox_mode = "workspace-write"\n');
  assert.strictEqual(found.length, 2);
  assert.ok(!found.some(silencesApprovals));
  assert.deepStrictEqual(scanConfigToml('# approval_policy = "never"'), []);
});

test('doctor runs on a scratch home and reports every section', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(path.join(home, 'config.toml'), 'approval_policy = "never"\n');
  const res = run(['doctor', '--codex-home', home, '--port', '/dev/cu.definitely-not-here']);
  assert.strictEqual(res.status, 0);
  for (const section of ['node', 'serial ports', 'codex', 'hooks.json', 'approvals', 'session']) {
    assert.match(res.stdout, new RegExp(section));
  }
  assert.match(res.stdout, /WARNING/);
  assert.match(res.stdout, /install-hook/);
});

test('demo prints every state and needs no device in dry-run', () => {
  const res = run(['demo', '--dry-run']);
  assert.strictEqual(res.status, 0);
  for (const state of ['idle', 'busy', 'waiting', 'done', 'sleep']) {
    assert.match(res.stdout, new RegExp(`"state":"${state}"`));
  }
});

test('run in dry-run mode installs nothing and stops on SIGINT', (t) => {
  const home = tmpdir(t);
  const child = require('node:child_process').spawn(
    process.execPath,
    [CLI, 'run', '--dry-run', '--codex-home', home],
    { encoding: 'utf8' }
  );
  let out = '';
  child.stdout.on('data', (d) => {
    out += d;
  });
  return new Promise((resolve) => {
    setTimeout(() => child.kill('SIGINT'), 1500);
    child.on('exit', (code) => {
      assert.strictEqual(code, 0);
      assert.match(out, /Nothing was installed/);
      assert.match(out, /stopped/);
      assert.deepStrictEqual(fs.readdirSync(home), []);
      resolve();
    });
  });
});

test('argument parsing is boring and predictable', () => {
  const o = parseArgs(['doctor', '--port', '/dev/x', '--codex-home', '/h', '--dry-run']);
  assert.strictEqual(o.command, 'doctor');
  assert.strictEqual(o.port, '/dev/x');
  assert.strictEqual(o.codexHome, '/h');
  assert.strictEqual(o.dryRun, true);
  assert.strictEqual(parseArgs([]).command, 'run');
  assert.strictEqual(parseArgs(['--help']).help, true);
});
