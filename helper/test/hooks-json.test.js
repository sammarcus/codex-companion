'use strict';

/**
 * hooks.json is somebody else's file. We add our own matcher groups to it and
 * we take exactly those back out again. These tests are the proof.
 */

const test = require('node:test');
const assert = require('node:assert');
const { execFileSync } = require('node:child_process');

const {
  mergeHooks,
  unmergeHooks,
  isOurHandler,
  hookCommand,
  hookHandler,
  stableNodePath,
  registeredEvents,
  hookCommandFlags,
  hooksFileProblem,
  commandParts,
  INSTALLED_EVENTS,
  ALL_HOOK_EVENTS
} = require('../codex-companion');

const CMD = "'/opt/homebrew/bin/node' '/Users/someone/codex-companion.js' hook";

const FOREIGN = {
  description: 'my own hooks',
  hooks: {
    PreToolUse: [
      { matcher: 'shell', hooks: [{ type: 'command', command: '/usr/local/bin/audit.sh' }] }
    ],
    Stop: [{ hooks: [{ type: 'command', command: 'say done' }] }]
  }
};

test('the event names we write are spelled exactly as Codex expects them', () => {
  const codexNames = [
    'PreToolUse',
    'PermissionRequest',
    'PostToolUse',
    'PreCompact',
    'PostCompact',
    'SessionStart',
    'SessionEnd',
    'UserPromptSubmit',
    'SubagentStart',
    'SubagentStop',
    'Stop',
    'Interrupt'
  ];
  assert.deepStrictEqual([...ALL_HOOK_EVENTS].sort(), codexNames.sort());
  for (const e of INSTALLED_EVENTS) assert.ok(codexNames.includes(e), `${e} is not a real event`);
});

test('a fresh install produces the documented shape', () => {
  const { next, added } = mergeHooks(null, CMD);
  assert.deepStrictEqual(added, INSTALLED_EVENTS);
  assert.deepStrictEqual(Object.keys(next), ['hooks']);
  const group = next.hooks.PermissionRequest[0];
  assert.strictEqual(group.matcher, undefined);
  assert.deepStrictEqual(group.hooks[0], {
    type: 'command',
    command: CMD,
    timeout: 5,
    async: true
  });
});

test('the PermissionRequest handler is async, which is what stops it deciding', () => {
  // Codex only applies an allow/deny from a handler whose execution mode is
  // Sync (hooks/src/engine/mod.rs, can_apply_control_effects).
  const h = hookHandler(CMD, 'PermissionRequest');
  assert.strictEqual(h.async, true);
});

test('SessionEnd asks for what Codex actually does: synchronous, short timeout', () => {
  // Codex forces SessionEnd handlers to run synchronously and caps the timeout
  // at 3 seconds, so registering it as async would only earn a warning.
  const h = hookHandler(CMD, 'SessionEnd');
  assert.strictEqual(h.async, false);
  assert.ok(h.timeout <= 3);
});

test('merging into an existing file leaves the existing hooks untouched', () => {
  const { next } = mergeHooks(FOREIGN, CMD);
  assert.strictEqual(next.description, 'my own hooks');
  assert.strictEqual(next.hooks.PreToolUse[0].matcher, 'shell');
  assert.strictEqual(next.hooks.PreToolUse[0].hooks[0].command, '/usr/local/bin/audit.sh');
  assert.strictEqual(next.hooks.Stop[0].hooks[0].command, 'say done');
  // ours is appended as its own group, never merged into theirs
  assert.ok(next.hooks.PreToolUse.some((g) => g.hooks.some(isOurHandler)));
  assert.ok(next.hooks.Stop.some((g) => g.hooks.some(isOurHandler)));
});

test('merging does not mutate the object it was given', () => {
  const before = JSON.stringify(FOREIGN);
  mergeHooks(FOREIGN, CMD);
  assert.strictEqual(JSON.stringify(FOREIGN), before);
});

test('merging twice adds nothing the second time', () => {
  const first = mergeHooks(FOREIGN, CMD).next;
  const second = mergeHooks(first, CMD);
  assert.deepStrictEqual(second.added, []);
  assert.deepStrictEqual(second.next, first);
});

test('uninstall removes exactly ours and puts the file back as it was', () => {
  const installed = mergeHooks(FOREIGN, CMD).next;
  const { next, removed, empty } = unmergeHooks(installed);
  assert.strictEqual(removed, INSTALLED_EVENTS.length);
  assert.strictEqual(empty, false);
  assert.deepStrictEqual(next, FOREIGN);
});

test('uninstalling from a file that only ever held ours leaves nothing behind', () => {
  const installed = mergeHooks(null, CMD).next;
  const { next, removed, empty } = unmergeHooks(installed);
  assert.strictEqual(removed, INSTALLED_EVENTS.length);
  assert.strictEqual(empty, true);
  assert.deepStrictEqual(next, {});
});

test('uninstall on a file with none of ours changes nothing', () => {
  const { next, removed } = unmergeHooks(FOREIGN);
  assert.strictEqual(removed, 0);
  assert.deepStrictEqual(next, FOREIGN);
});

test('an empty group somebody else wrote is preserved, not swept up', () => {
  const theirs = { hooks: { Stop: [{ matcher: 'x', hooks: [] }] } };
  const { next, removed } = unmergeHooks(theirs);
  assert.strictEqual(removed, 0);
  assert.deepStrictEqual(next, theirs);
});

test('our handler is recognised whatever node binary or path it was installed with', () => {
  assert.ok(isOurHandler({ type: 'command', command: CMD }));
  // The double-quoted form is what earlier copies of this file installed, and
  // a hooks.json somebody already has must still uninstall cleanly.
  assert.ok(
    isOurHandler({ type: 'command', command: '"/usr/bin/node" "/elsewhere/codex-companion.js" hook' })
  );
  assert.ok(isOurHandler({ type: 'command', command: 'codex-companion hook' }));
  assert.ok(!isOurHandler({ type: 'command', command: 'codex-companion doctor' }));
  assert.ok(!isOurHandler({ type: 'command', command: 'my-own-codex-companion-hook.sh' }));
  assert.ok(!isOurHandler({ type: 'mcp_tool', server: 's', tool: 't' }));
  assert.ok(!isOurHandler(null));
});

test('a handler somebody appended a documented flag to is still ours', () => {
  // README lists --no-metrics as a hook option, so appending it is an edit a
  // recipient is invited to make. When this was not recognised, uninstall left
  // the handler behind and install added a second group beside it, which meant
  // two lines down the cable for every event.
  assert.ok(isOurHandler({ type: 'command', command: `${CMD} --no-metrics` }));
  assert.ok(isOurHandler({ type: 'command', command: `${CMD} --port /dev/cu.usbmodem1101` }));
  const withFlag = `${CMD} --no-metrics`;
  assert.strictEqual(
    unmergeHooks(mergeHooks({}, withFlag).next).removed,
    INSTALLED_EVENTS.length
  );
  assert.deepStrictEqual(mergeHooks(mergeHooks({}, withFlag).next, CMD).added, []);
  // Still not a licence to match anything that merely mentions us.
  assert.ok(!isOurHandler({ type: 'command', command: `${CMD} --no-metrics ; rm -rf /` }));
});

test('a stable node alias is preferred over the versioned path we happen to run', () => {
  // Naming /opt/homebrew/Cellar/node/26.7.0/bin/node in hooks.json breaks at
  // the next `brew upgrade node`, which is a known way for this class of tool
  // to quietly stop working.
  const picked = stableNodePath(['/definitely/not/node', process.execPath]);
  assert.strictEqual(picked.path, process.execPath);
  assert.strictEqual(picked.stable, true);
});

test('with no usable alias it falls back to the running binary and says so', () => {
  const picked = stableNodePath(['/definitely/not/node', '/also/not/node']);
  assert.strictEqual(picked.path, process.execPath);
  assert.strictEqual(picked.stable, false);
});

test('hookCommand quotes both paths so a space in a path cannot break it', () => {
  const cmd = hookCommand('/usr/bin/node', '/Users/some one/codex-companion.js');
  assert.strictEqual(cmd, "'/usr/bin/node' '/Users/some one/codex-companion.js' hook");
  assert.ok(isOurHandler({ type: 'command', command: cmd }));
});

test('the shell Codex runs it in cannot expand anything inside the quoting', () => {
  // Codex passes the whole command line to $SHELL -lc, so this is the real
  // test: a folder named with a $ or a backtick used to be rewritten by the
  // shell into a path that does not exist, and the hook then failed silently
  // forever with doctor still reporting it fine.
  for (const dir of ['/tmp/co$(id -un)dex', '/tmp/co`id`x', '/tmp/back\\slash', "/tmp/qu'ote"]) {
    const script = `${dir}/codex-companion.js`;
    const cmd = hookCommand('/bin/echo', script);
    const out = execFileSync('/bin/sh', ['-lc', cmd], { encoding: 'utf8' }).trim();
    assert.strictEqual(out, `${script} hook`);
    assert.ok(isOurHandler({ type: 'command', command: cmd }));
    assert.deepStrictEqual(commandParts(cmd), ['/bin/echo', script]);
  }
});

test('registeredEvents reports what a hooks.json currently has of ours', () => {
  assert.deepStrictEqual(registeredEvents(FOREIGN), []);
  assert.deepStrictEqual(registeredEvents(mergeHooks(FOREIGN, CMD).next).sort(), [
    ...INSTALLED_EVENTS
  ].sort());
  assert.deepStrictEqual(registeredEvents(null), []);
});

// --------------------------------------------------------------------------
// A command that has gone stale. This is what `brew upgrade node` and moving
// this folder both do, and doctor's remedy for it is "re-run install-hook,
// it rewrites the command in place". These tests are that promise.
// --------------------------------------------------------------------------

const STALE = "'/opt/homebrew/Cellar/node/25.0.0/bin/node' '/old/place/codex-companion.js' hook";

test('a stale command of ours is rewritten, not skipped', () => {
  const stale = mergeHooks(null, STALE).next;
  const { next, added, updated } = mergeHooks(stale, CMD);
  assert.deepStrictEqual(added, [], 'nothing is missing, so nothing is added');
  assert.deepStrictEqual(updated, INSTALLED_EVENTS, 'every stale command was rewritten');
  const commands = [];
  for (const groups of Object.values(next.hooks)) {
    for (const g of groups) for (const h of g.hooks) commands.push(h.command);
  }
  assert.strictEqual(commands.length, INSTALLED_EVENTS.length);
  assert.ok(
    commands.every((c) => c === CMD),
    'no handler still names the path that moved'
  );
  assert.ok(!JSON.stringify(next).includes('25.0.0'), 'the dead path is gone from the file');
});

test('rewriting a stale command keeps a hand-appended --no-metrics', () => {
  // The README documents appending that flag, so a rewrite must not silently
  // undo a choice the owner made.
  const stale = mergeHooks(null, `${STALE} --no-metrics`).next;
  const { next, updated } = mergeHooks(stale, CMD);
  assert.deepStrictEqual(updated, INSTALLED_EVENTS);
  assert.strictEqual(next.hooks.PermissionRequest[0].hooks[0].command, `${CMD} --no-metrics`);
});

test('a command that is already current is left completely alone', () => {
  const installed = mergeHooks(null, CMD).next;
  const again = mergeHooks(installed, CMD);
  assert.deepStrictEqual(again.added, []);
  assert.deepStrictEqual(again.updated, []);
  assert.deepStrictEqual(again.next, installed);
});

test('hookCommandFlags reads back exactly the flags that were appended', () => {
  assert.strictEqual(hookCommandFlags(CMD), '');
  assert.strictEqual(hookCommandFlags(`${CMD} --no-metrics`), '--no-metrics');
  assert.strictEqual(hookCommandFlags(`${CMD} --port /dev/cu.x`), '--port /dev/cu.x');
});

// --------------------------------------------------------------------------
// Shapes we did not write. Repairing one means throwing a value away without
// telling anybody, so we refuse instead.
// --------------------------------------------------------------------------

test('a hooks.json shape we did not write is refused, not repaired', () => {
  assert.strictEqual(hooksFileProblem(null), null);
  assert.strictEqual(hooksFileProblem(undefined), null);
  assert.strictEqual(hooksFileProblem({}), null);
  assert.strictEqual(hooksFileProblem(FOREIGN), null);

  assert.match(hooksFileProblem([{ note: 'a users own file' }]), /top level is an array/);
  assert.match(hooksFileProblem('nope'), /top level is a string/);
  assert.match(hooksFileProblem({ hooks: [] }), /"hooks" value is an array/);
  assert.match(
    hooksFileProblem({ hooks: { PreToolUse: { hooks: [{ command: 'echo mine' }] } } }),
    /hooks\.PreToolUse is an object, not an array/
  );
});
