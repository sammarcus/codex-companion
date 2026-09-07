'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');

const {
  renderPlist,
  renderSystemdUnit,
  parseArgs,
  demoScript,
  xmlEscape,
  isVolatileNodeBin,
  stableNodeBin,
  DEFAULT_OK_FLAGS,
  TEMPLATE_PATH,
  LABEL,
  USAGE
} = require('../src/install');
const { buildFrame } = require('../src/frame');

const TEMPLATE = fs.readFileSync(TEMPLATE_PATH, 'utf8');

const VARS = {
  nodeBin: '/opt/homebrew/bin/node',
  installDir: '/Users/example/.codex-companion/app',
  home: '/Users/example'
};

test('the shipped template still has all three placeholders', () => {
  assert.match(TEMPLATE, /__NODE_BIN__/);
  assert.match(TEMPLATE, /__INSTALL_DIR__/);
  assert.match(TEMPLATE, /__HOME__/);
  assert.match(TEMPLATE, new RegExp(`<string>${LABEL}</string>`));
});

test('renderPlist substitutes every placeholder', () => {
  const out = renderPlist(TEMPLATE, VARS);
  assert.doesNotMatch(out, /__[A-Z_]+__/, 'no placeholder survives');
  assert.match(out, /<string>\/opt\/homebrew\/bin\/node<\/string>/);
  assert.match(out, /<string>\/Users\/example\/\.codex-companion\/app\/index\.js<\/string>/);
  assert.match(out, /<string>run<\/string>/);
  assert.match(
    out,
    /<string>\/Users\/example\/Library\/Logs\/codex-companion\.log<\/string>/
  );
  // launchd does no variable expansion, so nothing may be left relative.
  assert.doesNotMatch(out, /\$HOME/);
  assert.doesNotMatch(out, /<string>~\//);
});

test('renderPlist keeps the settings that make autostart actually work', () => {
  const out = renderPlist(TEMPLATE, VARS);
  for (const key of ['KeepAlive', 'RunAtLoad', 'ProcessType', 'EnvironmentVariables']) {
    assert.match(out, new RegExp(`<key>${key}</key>`), `${key} present`);
  }
  assert.match(out, /<key>KeepAlive<\/key>\s*<true\/>/);
  assert.match(out, /<key>RunAtLoad<\/key>\s*<true\/>/);
  assert.match(out, /<string>Background<\/string>/);
  // A LaunchAgent gets a minimal environment, so PATH has to be explicit.
  assert.match(out, /<key>PATH<\/key>/);
});

test('renderPlist refuses input that would produce a broken agent', () => {
  assert.throws(() => renderPlist('', VARS), /template is empty/);
  assert.throws(() => renderPlist(TEMPLATE, {}), /missing nodeBin/);
  assert.throws(
    () => renderPlist(TEMPLATE, Object.assign({}, VARS, { nodeBin: 'node' })),
    /must be absolute/
  );
  assert.throws(
    () => renderPlist(TEMPLATE, Object.assign({}, VARS, { installDir: './app' })),
    /must be absolute/
  );
  assert.throws(
    () => renderPlist('<plist>__SOMETHING_ELSE__</plist>', VARS),
    /unsubstituted placeholder __SOMETHING_ELSE__/
  );
});

test('the rendered plist is valid XML that plutil accepts', async (t) => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), 'codex-companion-plist-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const file = path.join(dir, `${LABEL}.plist`);
  fs.writeFileSync(file, renderPlist(TEMPLATE, VARS), 'utf8');

  const plutil = spawnSync('plutil', ['-lint', file], { encoding: 'utf8' });
  if (plutil.error && plutil.error.code === 'ENOENT') {
    t.skip('plutil is not available on this platform');
    return;
  }
  assert.strictEqual(plutil.status, 0, plutil.stdout + plutil.stderr);
  assert.match(plutil.stdout, /OK/);

  // Read it back the way launchd would, and check the parsed values.
  const conv = spawnSync('plutil', ['-convert', 'json', '-o', '-', file], { encoding: 'utf8' });
  assert.strictEqual(conv.status, 0, conv.stderr);
  const parsed = JSON.parse(conv.stdout);
  assert.strictEqual(parsed.Label, LABEL);
  assert.deepStrictEqual(parsed.ProgramArguments, [
    VARS.nodeBin,
    path.join(VARS.installDir, 'index.js'),
    'run'
  ]);
  assert.strictEqual(parsed.KeepAlive, true);
  assert.strictEqual(parsed.RunAtLoad, true);
  assert.strictEqual(parsed.WorkingDirectory, VARS.installDir);
  assert.strictEqual(parsed.StandardOutPath, `${VARS.home}/Library/Logs/codex-companion.log`);
  assert.strictEqual(parsed.StandardErrorPath, parsed.StandardOutPath);
});

test('a path with a space survives rendering as one argv element', async (t) => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), 'codex-companion-plist2-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const vars = {
    nodeBin: '/opt/homebrew/bin/node',
    installDir: '/Users/Jane Doe/.codex-companion/app',
    home: '/Users/Jane Doe'
  };
  const file = path.join(dir, 'spaced.plist');
  fs.writeFileSync(file, renderPlist(TEMPLATE, vars), 'utf8');
  const conv = spawnSync('plutil', ['-convert', 'json', '-o', '-', file], { encoding: 'utf8' });
  if (conv.error && conv.error.code === 'ENOENT') {
    t.skip('plutil is not available on this platform');
    return;
  }
  assert.strictEqual(conv.status, 0, conv.stderr);
  const parsed = JSON.parse(conv.stdout);
  assert.strictEqual(parsed.ProgramArguments.length, 3, 'the space did not split the argument');
  assert.strictEqual(parsed.ProgramArguments[1], '/Users/Jane Doe/.codex-companion/app/index.js');
});

test('the systemd unit points at an absolute node and an absolute entry point', () => {
  const unit = renderSystemdUnit(VARS);
  assert.match(unit, /^\[Unit\]/);
  assert.match(
    unit,
    /^ExecStart=\/opt\/homebrew\/bin\/node \/Users\/example\/\.codex-companion\/app\/index\.js run$/m
  );
  assert.match(unit, /^Restart=always$/m);
  assert.match(unit, /^WantedBy=default\.target$/m);
});

test('parseArgs table', () => {
  const cases = [
    { argv: [], want: { _: [], flags: {} } },
    { argv: ['run'], want: { _: ['run'], flags: {} } },
    { argv: ['run', '--dry-run'], want: { _: ['run'], flags: { 'dry-run': true } } },
    {
      argv: ['run', '--port', '/dev/tty.usbmodem101'],
      want: { _: ['run'], flags: { port: '/dev/tty.usbmodem101' } }
    },
    {
      argv: ['run', '--port=/dev/tty.usbmodem101'],
      want: { _: ['run'], flags: { port: '/dev/tty.usbmodem101' } }
    },
    {
      argv: ['install', '--global', '--codex-home', '/tmp/cx'],
      want: { _: ['install'], flags: { global: true, 'codex-home': '/tmp/cx' } }
    },
    {
      // A boolean flag must not swallow the positional that follows it.
      argv: ['demo', '--loop', 'extra'],
      want: { _: ['demo', 'extra'], flags: { loop: true } }
    },
    {
      argv: ['run', '--dry-run', '--no-heuristic', '--port', '/dev/ttyACM0'],
      want: {
        _: ['run'],
        flags: { 'dry-run': true, 'no-heuristic': true, port: '/dev/ttyACM0' }
      }
    },
    {
      // A value-taking flag at the very end has no value to take.
      argv: ['run', '--port'],
      want: { _: ['run'], flags: { port: true } }
    }
  ];
  for (const c of cases) {
    assert.deepStrictEqual(parseArgs(c.argv), c.want, JSON.stringify(c.argv));
  }
});

test('the demo script visits every state the device knows about', () => {
  const script = demoScript();
  const states = new Set(script.map((s) => buildFrame(s.state).state));
  assert.deepStrictEqual(
    [...states].sort(),
    ['busy', 'done', 'idle', 'sleep', 'waiting'],
    'demo covers the full state set'
  );
  for (const step of script) {
    assert.ok(step.ms > 0, 'every step has a duration');
    const f = buildFrame(step.state);
    assert.ok(f.ring >= 0 && f.ring <= 1, `ring in range for ${f.state}`);
    assert.doesNotThrow(() => JSON.parse(JSON.stringify(f) ));
  }
});

test('a home directory containing & still renders a plist plutil accepts', async (t) => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), 'codex-companion-amp-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const vars = {
    nodeBin: '/opt/homebrew/bin/node',
    installDir: '/Users/a&b/.codex-companion/app',
    home: '/Users/a&b'
  };
  const out = renderPlist(TEMPLATE, vars);
  assert.match(out, /a&amp;b/, 'the ampersand is escaped in the XML');
  const file = path.join(dir, 'amp.plist');
  fs.writeFileSync(file, out, 'utf8');
  const conv = spawnSync('plutil', ['-convert', 'json', '-o', '-', file], { encoding: 'utf8' });
  if (conv.error && conv.error.code === 'ENOENT') {
    t.skip('plutil is not available on this platform');
    return;
  }
  assert.strictEqual(conv.status, 0, conv.stdout + conv.stderr);
  const parsed = JSON.parse(conv.stdout);
  // The parser must hand back the ORIGINAL path, not the escaped text.
  assert.strictEqual(parsed.ProgramArguments[1], '/Users/a&b/.codex-companion/app/index.js');
  assert.strictEqual(parsed.WorkingDirectory, vars.installDir);
});

test('xmlEscape covers exactly the characters that break a plist', () => {
  assert.strictEqual(xmlEscape('/Users/a&b'), '/Users/a&amp;b');
  assert.strictEqual(xmlEscape('a<b>c'), 'a&lt;b&gt;c');
  assert.strictEqual(xmlEscape('/Users/Jane Doe'), '/Users/Jane Doe', 'spaces are fine as-is');
  // & first, so an escape is never double-escaped.
  assert.strictEqual(xmlEscape('&lt;'), '&amp;lt;');
});

test('version-pinned node paths are recognised as the trap they are', () => {
  assert.ok(isVolatileNodeBin('/opt/homebrew/Cellar/node/26.7.0/bin/node'));
  assert.ok(isVolatileNodeBin('/opt/homebrew/Cellar/node@20/20.11.1/bin/node'));
  assert.ok(isVolatileNodeBin('/Users/x/.nvm/versions/node/v20.11.1/bin/node'));
  assert.ok(isVolatileNodeBin('/Users/x/.volta/tools/image/node/20.11.1/bin/node'));
  assert.ok(!isVolatileNodeBin('/opt/homebrew/bin/node'));
  assert.ok(!isVolatileNodeBin('/usr/local/bin/node'));
  assert.ok(!isVolatileNodeBin('/usr/bin/node'));
});

test('stableNodeBin swaps a Cellar path for the alias that points at it', async (t) => {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), 'codex-companion-node-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  const real = path.join(dir, 'Cellar', 'node', '26.7.0', 'bin');
  fs.mkdirSync(real, { recursive: true });
  const realNode = path.join(real, 'node');
  fs.writeFileSync(realNode, '#!/bin/sh\n', { mode: 0o755 });

  const aliasDir = path.join(dir, 'bin');
  fs.mkdirSync(aliasDir, { recursive: true });
  const alias = path.join(aliasDir, 'node');
  fs.symlinkSync(realNode, alias);

  assert.strictEqual(stableNodeBin(realNode, [alias]), alias, 'the alias wins');
  // No alias resolves to this binary: keep what we were given rather than lie.
  const other = path.join(dir, 'nope', 'node');
  assert.strictEqual(stableNodeBin(realNode, [other]), realNode);
  // A caller already running the alias keeps it.
  assert.strictEqual(stableNodeBin(alias, [alias]), alias);
});

test('the bare-invocation branch only accepts flags that branch understands', () => {
  // `case default` installs a LaunchAgent and starts a service, so --help and
  // --version must never reach it.
  for (const f of ['help', 'h', 'version']) {
    assert.ok(!DEFAULT_OK_FLAGS.has(f), `--${f} must not be accepted by the default branch`);
  }
  for (const f of ['port', 'dry-run', 'codex-home', 'no-heuristic', 'global']) {
    assert.ok(DEFAULT_OK_FLAGS.has(f), `--${f} is a legitimate flag for a bare run`);
  }
});

test('usage text documents every subcommand', () => {
  for (const cmd of ['run', 'install', 'uninstall', 'status', 'demo', 'doctor']) {
    assert.match(USAGE, new RegExp(`codex-companion ${cmd}\\b`), `${cmd} documented`);
  }
  for (const flag of ['--port', '--dry-run', '--loop', '--global', '--codex-home',
    '--no-heuristic', '--help', '--version']) {
    assert.ok(USAGE.includes(flag), `${flag} documented`);
  }
});

test("the README's flag list matches the flags the CLI actually implements", () => {
  const readme = fs.readFileSync(path.join(__dirname, '..', 'README.md'), 'utf8');
  for (const flag of ['--port', '--dry-run', '--loop', '--global', '--codex-home',
    '--no-heuristic', '--help', '--version']) {
    assert.ok(readme.includes(flag), `${flag} is in the README`);
  }
  // The package is not published, so the README must not hand out a command
  // that 404s at the desk. The old anchored form only caught an npx line that
  // ended right after the package name, so `npx codex-companion install` sailed
  // through and shipped. Match any npx invocation at all.
  assert.ok(!/\bnpx\s+codex-companion\b/.test(readme), 'no npx install instruction');
});

test('package.json records the name the recon found free', () => {
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8'));
  assert.strictEqual(pkg.name, 'codex-companion');
  assert.strictEqual(pkg.bin['codex-companion'], 'bin/codex-companion.js');
  assert.ok(pkg.engines.node.includes('20'), 'Node >= 20, matching serialport engines');
  assert.ok(pkg.dependencies.serialport, 'serialport is a real dependency');
  // The README promises the shipped tarball installs with no network. That is
  // only true while serialport is bundled into it: without this, `npm pack`
  // emits 9 files, and an offline install dies with ENOTCACHED at the desk.
  assert.deepStrictEqual(
    pkg.bundleDependencies,
    ['serialport'],
    'serialport must be bundled or the offline install claim is false'
  );
});

test('the plist entry point exists in the package', () => {
  const root = path.join(__dirname, '..');
  assert.ok(fs.existsSync(path.join(root, 'index.js')), 'index.js is what the plist runs');
  assert.ok(fs.existsSync(path.join(root, 'bin', 'codex-companion.js')));
  const shebang = fs.readFileSync(path.join(root, 'bin', 'codex-companion.js'), 'utf8');
  assert.ok(shebang.startsWith('#!/usr/bin/env node'), 'bin has a shebang');
});
