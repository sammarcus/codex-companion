'use strict';

/**
 * doctor, one realistic failure at a time.
 *
 * The bar for every case below is not "it noticed" but "it said what to do".
 * A recipient running this has a present that is not working and no idea how
 * any of it fits together, so a finding with no remedy attached is a bug in
 * the report.
 */

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const {
  scanHookState,
  commandParts,
  portOpenAdvice,
  wrapText,
  hookCommand,
  invocation,
  plural,
  NPX_INVOCATION
} = require('../codex-companion');

const CLI = path.join(__dirname, '..', 'codex-companion.js');
const NO_BOARD = '/dev/cu.definitely-not-here';

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-companion-doctor-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function doctor(home, extra) {
  return spawnSync(
    process.execPath,
    [CLI, 'doctor', '--codex-home', home, '--port', NO_BOARD, ...(extra || [])],
    { encoding: 'utf8', timeout: 40000, input: '' }
  );
}

/**
 * The report with its wrapping undone: remedies are wrapped to the terminal,
 * so a phrase can legitimately straddle two lines. Asserting on the prose
 * rather than on the column it landed in keeps these tests about behaviour.
 */
function flat(stdout) {
  return String(stdout).replace(/\n\s*->\s*/g, ' ').replace(/[ \t]+/g, ' ');
}

/** Install our real hooks.json into a scratch home. */
function install(home) {
  return spawnSync(process.execPath, [CLI, 'install-hook', '--codex-home', home], {
    encoding: 'utf8',
    timeout: 30000,
    input: ''
  });
}

// ------------------------------------------------------- the pure functions

test('portOpenAdvice names every errno a real cable produces', () => {
  const cases = {
    EACCES: /permission denied/i,
    EPERM: /permission denied/i,
    EBUSY: /another program/i,
    ENOENT: /vanished/i,
    ENXIO: /no device behind/i,
    EWHATEVER: /would not open/i
  };
  for (const [code, expected] of Object.entries(cases)) {
    const advice = portOpenAdvice(code);
    assert.match(advice.what, expected, code);
    assert.ok(advice.fix.length > 20, `${code} has no remedy`);
  }
  // EBUSY is the one people hit constantly: two serial monitors at once.
  assert.match(portOpenAdvice('EBUSY').fix, /lsof|monitor|screen/i);
  assert.strictEqual(portOpenAdvice(undefined).code, '(no code)');
});

test('scanHookState reads the trust records Codex keeps in config.toml', () => {
  const toml = `
model = "gpt-5-codex"

[hooks.state."/home/me/.codex/hooks.json:permission_request:0:0"]
enabled = true
trusted_hash = "sha256:abc123"

[hooks.state."/home/me/.codex/hooks.json:pre_tool_use:0:0"]
enabled = false

[hooks.state."/somewhere/else/hooks.json:stop:0:0"]
trusted_hash = "sha256:def"

[profiles.yolo]
approval_policy = "never"
`;
  const ours = scanHookState(toml, '/home/me/.codex/hooks.json');
  assert.strictEqual(ours.length, 2, 'it picked up a state entry for another file');
  assert.strictEqual(ours[0].trusted, true);
  assert.strictEqual(ours[0].enabled, true);
  assert.strictEqual(ours[1].enabled, false);
  assert.strictEqual(ours[1].trusted, false);
  // A key belonging to a different hooks.json is somebody else's business.
  assert.strictEqual(scanHookState(toml, '/nowhere').length, 0);
  assert.deepStrictEqual(scanHookState('', '/x'), []);
  assert.deepStrictEqual(scanHookState(null, '/x'), []);
});

test('scanHookState stops at the next section, so it cannot bleed', () => {
  const toml = `
[hooks.state."/h/hooks.json:stop:0:0"]

[other]
trusted_hash = "sha256:not-ours"
`;
  const found = scanHookState(toml, '/h/hooks.json');
  assert.strictEqual(found.length, 1);
  assert.strictEqual(found[0].trusted, false);
});

test('commandParts recovers the paths out of a command we wrote', () => {
  const cmd = hookCommand('/opt/homebrew/bin/node', '/Users/me/codex-companion.js');
  assert.deepStrictEqual(commandParts(cmd), [
    '/opt/homebrew/bin/node',
    '/Users/me/codex-companion.js'
  ]);
  // The double-quoted form earlier copies of this file installed. Without it,
  // doctor's "the hook command has moved" check would go blind on a hooks.json
  // that is already on somebody's machine.
  assert.deepStrictEqual(
    commandParts('"/usr/bin/node" "/Users/me/codex-companion.js" hook'),
    ['/usr/bin/node', '/Users/me/codex-companion.js']
  );
  assert.deepStrictEqual(commandParts('audit.sh'), []);
  assert.deepStrictEqual(commandParts(null), []);
});

test('a named --port is reported without a verdict it has not earned', (t) => {
  // doctor used to print `ok device` for a path that does not exist, with the
  // very next row failing ENOENT. Every ok in this report has to mean somebody
  // checked something.
  const out = doctor(tmpdir(t)).stdout;
  assert.match(out, /^ {7}device {10}\/dev\/cu\.definitely-not-here$/m);
  assert.doesNotMatch(out, /ok {3}device/);
  assert.match(out, /FAIL port opens/);
});

test('wrapText keeps a remedy inside the terminal', () => {
  const lines = wrapText('a '.repeat(80), 30);
  assert.ok(lines.length > 1);
  for (const line of lines) assert.ok(line.length <= 30, line);
  assert.deepStrictEqual(wrapText('', 20), ['']);
});

// -------------------------------------------------------- the whole report

test('a port that will not open is diagnosed by its errno', (t) => {
  // The named port does not exist, which is exactly what an unplugged board
  // looks like a second after somebody pulled the cable.
  const res = doctor(tmpdir(t));
  assert.strictEqual(res.status, 0, 'doctor is a report and exits 0 by default');
  assert.match(res.stdout, /FAIL +port opens/);
  assert.match(res.stdout, /ENOENT/);
  assert.match(flat(res.stdout), /Replug it and re-run/);
});

test('a missing hooks.json is the next step, not a failure', (t) => {
  // The pitch is that the program is optional, so the state everybody starts
  // in cannot be the first red word they ever see from this project. It still
  // says what to do about it, and it is still counted, separately.
  const res = doctor(tmpdir(t));
  assert.match(res.stdout, /NEXT +hooks\.json {2}.*not installed yet/);
  assert.doesNotMatch(res.stdout, /FAIL +hooks\.json/);
  assert.match(res.stdout, /install-hook/);
  assert.match(res.stdout, /1 thing to do/);
});

test('the summary counts things to do apart from problems', (t) => {
  // --port names a path that does not exist, so there is exactly one real
  // problem (the port) and one thing to do (the hook).
  const res = doctor(tmpdir(t));
  assert.match(res.stdout, /1 thing to do, 1 problem\b/);
  assert.doesNotMatch(res.stdout, /problem\(s\)/);
});

test('plural writes a count nobody has to read as problem(s)', () => {
  assert.strictEqual(plural(1, 'problem'), '1 problem');
  assert.strictEqual(plural(0, 'problem'), '0 problems');
  assert.strictEqual(plural(2, 'warning'), '2 warnings');
});

test('advice run from an npx cache names the one-liner, not the cache path', () => {
  // npx unpacks into ~/.npm/_npx/<hash>/, which is an npm implementation
  // detail: unreadable to paste, and gone after `npm cache clean`. Everybody
  // who lands there typed the one-liner, so it is what they get back.
  const cached =
    '/Users/you/.npm/_npx/8a9a070704c05c28/node_modules/codex-companion/helper/codex-companion.js';
  assert.strictEqual(invocation(cached), NPX_INVOCATION);
  assert.match(NPX_INVOCATION, /^npx github:/);
  // A clone or a global install has a real path, and keeps it.
  assert.strictEqual(
    invocation('/Users/you/codex-buddy/helper/codex-companion.js'),
    'node /Users/you/codex-buddy/helper/codex-companion.js'
  );
});

test('a hooks.json that is not JSON is refused, loudly, with a reason', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(path.join(home, 'hooks.json'), '{ nope');
  const res = doctor(home);
  assert.match(res.stdout, /not valid JSON/);
  assert.match(flat(res.stdout), /trailing comma/);
});

test('a hooks.json that is somebody else s is a failure, not a crash', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'hooks.json'),
    JSON.stringify({ hooks: { Stop: [{ hooks: [{ type: 'command', command: 'audit.sh' }] }] } })
  );
  const res = doctor(home);
  assert.match(res.stdout, /FAIL +our hook/);
  assert.match(res.stdout, /install-hook/);
});

test('registered but never approved is a warning that names /hooks', (t) => {
  const home = tmpdir(t);
  install(home);
  const res = doctor(home);
  assert.match(res.stdout, /ok +our hook {2}.*registered for/);
  assert.match(res.stdout, /WARN +hook trust/);
  assert.match(flat(res.stdout), /Hooks need review/);
  assert.match(res.stdout, /\/hooks/);
});

test('registered and trusted says so instead of crying wolf', (t) => {
  const home = tmpdir(t);
  install(home);
  const hooksPath = path.join(home, 'hooks.json');
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    `[hooks.state."${hooksPath}:permission_request:0:0"]\n` +
      'enabled = true\ntrusted_hash = "sha256:abc"\n'
  );
  const res = doctor(home);
  assert.match(res.stdout, /ok +hook trust/);
  assert.doesNotMatch(res.stdout, /not trusted yet/);
});

test('approved and then switched off is its own diagnosis', (t) => {
  // This looks identical from outside to "never approved" (the hook simply
  // never runs) and it is a completely different thing to fix.
  const home = tmpdir(t);
  install(home);
  const hooksPath = path.join(home, 'hooks.json');
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    `[hooks.state."${hooksPath}:permission_request:0:0"]\nenabled = false\n`
  );
  const res = doctor(home);
  assert.match(res.stdout, /FAIL +hook trust/);
  assert.match(flat(res.stdout), /switched OFF/);
  assert.match(res.stdout, /\/hooks/);
});

test('a hook command whose node binary has moved is caught', (t) => {
  // `brew upgrade node` and moving this folder both do exactly this, and the
  // symptom is a hook that silently never fires.
  const home = tmpdir(t);
  install(home);
  const hooksPath = path.join(home, 'hooks.json');
  const parsed = JSON.parse(fs.readFileSync(hooksPath, 'utf8'));
  for (const groups of Object.values(parsed.hooks)) {
    for (const group of groups) {
      for (const handler of group.hooks) {
        handler.command = handler.command.replace(
          /^'[^']*'/,
          "'/opt/homebrew/Cellar/node/0.0.0/bin/node'"
        );
      }
    }
  }
  fs.writeFileSync(hooksPath, JSON.stringify(parsed, null, 2));
  const res = doctor(home);
  assert.match(res.stdout, /FAIL +hook command/);
  assert.match(flat(res.stdout), /no longer exist/);
  assert.match(flat(res.stdout), /node is upgraded|folder is moved/);
});

test('a config that can never raise a prompt is the loudest finding', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    'approval_policy = "never"\n\n[profiles.yolo]\nsandbox_mode = "danger-full-access"\n'
  );
  const res = doctor(home);
  assert.match(res.stdout, /WARNING/);
  assert.match(flat(res.stdout), /never raise an approval prompt/);
  assert.match(res.stdout, /approval_policy = "never"/);
  assert.match(flat(res.stdout), /look broken while working/);
});

test('a sandbox setting alone is never called an approval problem', (t) => {
  // approval_policy = "untrusted" prompts for every command that is not
  // explicitly allowed, whatever the sandbox is: exec_policy.rs returns
  // Decision::Prompt for AskForApproval::UnlessTrusted regardless of
  // FileSystemSandboxKind, and assess_patch_safety returns AskUser before it
  // looks at a sandbox at all. Failing this config told the reader to change
  // the one setting that was not deciding the question.
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    'approval_policy = "untrusted"\nsandbox_mode = "danger-full-access"\n'
  );
  const res = doctor(home);
  assert.match(res.stdout, /ok +approvals/);
  assert.doesNotMatch(flat(res.stdout), /never raise an approval prompt/);
});

test('a never inside an unused profile is a warning, not a failed config', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    'approval_policy = "untrusted"\n\n[profiles.yolo]\napproval_policy = "never"\n'
  );
  const res = doctor(home);
  assert.match(res.stdout, /ok +approvals/);
  assert.match(res.stdout, /approval profiles/);
  assert.match(flat(res.stdout), /only when that profile is selected/);
});

test('the settings that decide the question are printed even when it fails', (t) => {
  // The failing branch used to suppress the listing, so the approval_policy
  // line a reader would need in order to disagree was never shown.
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    'approval_policy = "never"\nsandbox_mode = "workspace-write"\n'
  );
  const res = doctor(home);
  assert.match(flat(res.stdout), /never raise an approval prompt/);
  assert.match(res.stdout, /approval_policy = "never"/);
  assert.match(res.stdout, /sandbox_mode = "workspace-write"/);
});

test('an ordinary config is not accused of anything', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(
    path.join(home, 'config.toml'),
    'approval_policy = "on-request"\nsandbox_mode = "workspace-write"\n'
  );
  const res = doctor(home);
  assert.match(res.stdout, /ok +approvals/);
  assert.doesNotMatch(res.stdout, /WARNING/);
});

test('--strict is the version a script can use', (t) => {
  const home = tmpdir(t);
  const loose = doctor(home);
  const strict = doctor(home, ['--strict']);
  assert.strictEqual(loose.status, 0);
  assert.strictEqual(strict.status, 1, 'no device and no hooks is not a pass');
});

test('the report keeps the sections people were told to look at', (t) => {
  const home = tmpdir(t);
  const res = doctor(home);
  for (const section of ['node', 'serial ports', 'codex', 'CODEX_HOME', 'approvals', 'session']) {
    assert.match(res.stdout, new RegExp(section), `lost the ${section} line`);
  }
  // And it never leaves without saying the device works on its own.
  assert.match(res.stdout, /ambient mode forever/);
});

test('every finding in the report carries a remedy', (t) => {
  const home = tmpdir(t);
  fs.writeFileSync(path.join(home, 'config.toml'), 'approval_policy = "never"\n');
  const res = doctor(home);
  const lines = res.stdout.split('\n');
  let checked = 0;
  for (let i = 0; i < lines.length; i += 1) {
    if (!/^ {2}(FAIL|WARN|NEXT) /.test(lines[i])) continue;
    checked += 1;
    assert.match(lines[i + 1] || '', /^ {7}-> /, `no remedy after: ${lines[i]}`);
  }
  assert.ok(checked >= 2, `expected several findings, saw ${checked}`);
});
