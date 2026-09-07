'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');

const {
  CodexWatcher,
  SessionState,
  agentMessageText,
  contextFill,
  codexHome,
  newestSessionFile,
  readTailLines,
  parseLine,
  BASELINE_TOKENS
} = require('../src/codex-watcher');

const FIXTURES = path.join(__dirname, 'fixtures');
const REAL = path.join(FIXTURES, 'session-sample.jsonl');
const SYNTH = path.join(FIXTURES, 'session-sample-synthetic.jsonl');

function readFixture(file) {
  return fs
    .readFileSync(file, 'utf8')
    .split('\n')
    .filter((l) => l.trim().length > 0);
}

/** Pick fixture lines by rollout type / event type. */
function pick(lines, pred) {
  for (const l of lines) {
    const o = parseLine(l);
    if (o && pred(o)) return l;
  }
  throw new Error('fixture line not found');
}

const realLines = readFixture(REAL);
const synthLines = readFixture(SYNTH);

const L_META = pick(realLines, (o) => o.type === 'session_meta');
const L_STARTED = pick(realLines, (o) => o.payload && o.payload.type === 'task_started');
const L_TURNCTX = pick(realLines, (o) => o.type === 'turn_context');
const L_ITEM = pick(realLines, (o) => o.payload && o.payload.type === 'item_completed');
const L_COMPLETE_ERR = pick(realLines, (o) => o.payload && o.payload.type === 'task_complete');

const L_USAGE = pick(synthLines, (o) => o.type === 'token_usage_record');
const L_TOKENCOUNT = pick(synthLines, (o) => o.payload && o.payload.type === 'token_count');
const L_COMPLETE_OK = pick(
  synthLines,
  (o) => o.payload && o.payload.type === 'task_complete' && o.payload.last_agent_message
);
const L_ABORTED = pick(synthLines, (o) => o.payload && o.payload.type === 'turn_aborted');
const L_EXEC_APPROVAL = pick(
  synthLines,
  (o) => o.payload && o.payload.type === 'exec_approval_request'
);

/** Real capture: task_started started_at is 1788754800 (Unix seconds). */
const T0 = 1788754800 * 1000;

async function tempHome(t) {
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), 'codex-companion-test-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

/** A watcher whose clock we own and which never starts its own timers. */
function makeWatcher(home, clock, opts) {
  return new CodexWatcher(
    Object.assign(
      {
        codexHome: home,
        now: () => clock.t
      },
      opts || {}
    )
  );
}

function sessionFilePath(home, name) {
  return path.join(
    home,
    'sessions',
    '2026',
    '09',
    '07',
    name || 'rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl'
  );
}

async function appendLines(file, lines) {
  await fsp.mkdir(path.dirname(file), { recursive: true });
  await fsp.appendFile(file, lines.map((l) => l + '\n').join(''), 'utf8');
}

test('contextFill uses Codex own formula, not used/window', () => {
  // protocol.rs:2428 with BASELINE_TOKENS = 12000.
  assert.strictEqual(BASELINE_TOKENS, 12000);
  // Synthetic fixture numbers: 14732 of 258400 is barely any fill.
  assert.strictEqual(contextFill(14732, 258400), 0.01);
  // Exactly at the baseline: nothing used yet.
  assert.strictEqual(contextFill(12000, 258400), 0);
  assert.strictEqual(contextFill(0, 258400), 0);
  // Half of the effective window.
  assert.strictEqual(contextFill(12000 + 123200, 258400), 0.5);
  // Overfull clamps rather than exceeding 1.
  assert.strictEqual(contextFill(999999, 258400), 1);
  // Unknowable inputs.
  assert.strictEqual(contextFill(null, 258400), null);
  assert.strictEqual(contextFill(1000, null), null);
  // A window at or below the baseline yields no meaningful percentage.
  assert.strictEqual(contextFill(5000, 12000), null);
});

test('codexHome honours the CODEX_HOME override', () => {
  assert.strictEqual(codexHome({ CODEX_HOME: '/tmp/elsewhere' }), '/tmp/elsewhere');
  assert.strictEqual(codexHome({ CODEX_HOME: '   ' }), path.join(os.homedir(), '.codex'));
  assert.strictEqual(codexHome({}), path.join(os.homedir(), '.codex'));
});

test('a missing sessions directory reports sleep and keeps polling', async (t) => {
  const home = await tempHome(t);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  let snap = await w.poll();
  assert.strictEqual(snap.state, 'sleep');
  assert.strictEqual(w.currentFile, null);

  // The directory appearing later must be picked up, with no restart.
  await appendLines(sessionFilePath(home), [L_META, L_STARTED]);
  w._rescanQueued = true;
  clock.t = T0 + 1000;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'busy', 'watcher recovers once the dir exists');
  w.stop();
});

test('state transitions across a whole turn, fed one line at a time', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);
  const seen = [];
  w.on('state', (s) => seen.push(s.state));

  // 1. session_meta only: a file exists but no turn has started.
  await appendLines(file, [L_META]);
  let snap = await w.poll();
  assert.strictEqual(snap.state, 'idle');
  assert.strictEqual(snap.sessionId, '01a07a18-2f55-78c3-9976-e71905ebb698');
  assert.strictEqual(snap.cliVersion, '0.153.4');

  // 2. task_started: busy, and the context window is readable immediately.
  await appendLines(file, [L_STARTED]);
  clock.t = T0 + 2000;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'busy');
  assert.strictEqual(snap.contextWindow, 258400);
  assert.strictEqual(snap.ctxFill, null, 'no token accounting has arrived yet');
  assert.ok(Math.abs(snap.elapsedSec - 2) < 0.01);

  // 3. turn_context: model and approval policy land here, not in session_meta.
  await appendLines(file, [L_TURNCTX, L_ITEM]);
  clock.t = T0 + 5000;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'busy');
  assert.strictEqual(snap.model, 'gpt-5-codex');
  assert.strictEqual(snap.approvalPolicy, 'never');

  // 4. token accounting: ctxFill and token counts appear.
  await appendLines(file, [L_USAGE]);
  clock.t = T0 + 10000;
  snap = await w.poll();
  assert.strictEqual(snap.totalTokens, 14732);
  assert.strictEqual(snap.tokensIn, 14320);
  assert.strictEqual(snap.tokensOut, 412);
  assert.strictEqual(snap.ctxFill, 0.01);
  assert.strictEqual(snap.state, 'busy');

  // 5. token_count carries the rate-limit snapshot, stamped with its own time.
  await appendLines(file, [L_TOKENCOUNT]);
  clock.t = T0 + 10500;
  snap = await w.poll();
  assert.ok(snap.rateLimits, 'rate limits captured');
  assert.strictEqual(snap.rateLimits.primary.used_percent, 12.5);
  assert.strictEqual(snap.rateLimits.observedAtMs, Date.parse('2026-09-07T04:20:10.100Z'));

  // 6. task_complete: done, holding for 5 s.
  await appendLines(file, [L_COMPLETE_OK]);
  clock.t = T0 + 12000;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'done');
  assert.strictEqual(snap.lastAgentMessage, 'Created hello.txt and listed the directory.');
  assert.strictEqual(snap.elapsedSec, 11.5, 'elapsed comes from duration_ms once the turn ends');
  // Turn-level tps. This task_complete carries no turn_token_usage, so the
  // rate comes from the cumulative-output delta across the turn: 412 tokens
  // over duration_ms 11500.
  assert.ok(Math.abs(snap.tps - 412 / 11.5) < 1e-9, `tps was ${snap.tps}`);

  // 7. done expires into idle 5 s after completion.
  clock.t = Date.parse('2026-09-07T04:20:11.000Z') + 5001;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'idle');

  // 8. five minutes of silence: sleep.
  clock.t += 5 * 60 * 1000;
  snap = await w.poll();
  assert.strictEqual(snap.state, 'sleep');

  assert.deepStrictEqual(
    seen.filter((s, i) => s !== seen[i - 1]),
    ['idle', 'busy', 'done', 'idle', 'sleep'],
    'the emitted transition sequence'
  );
  w.stop();
});

test('a failed turn still completes, and the error is surfaced', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  await appendLines(file, [L_META, L_STARTED, L_TURNCTX, L_COMPLETE_ERR]);
  clock.t = Date.parse('2026-09-07T04:20:07.491Z') + 100;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'done');
  assert.match(snap.lastError, /access token could not be refreshed/);
  assert.strictEqual(snap.elapsedSec, 6.854);
  w.stop();
});

test('turn_aborted closes the turn without claiming success', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  await appendLines(file, [L_META, L_STARTED, L_ABORTED]);
  clock.t = 1788754812 * 1000 + 500;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'done');
  assert.strictEqual(snap.abortedReason, 'interrupted');
  assert.strictEqual(snap.lastAgentMessage, null);
  w.stop();
});

test('an explicit approval request produces waiting, not busy', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  await appendLines(file, [L_META, L_STARTED, L_TURNCTX]);
  clock.t = T0 + 3000;
  assert.strictEqual((await w.poll()).state, 'busy');

  await appendLines(file, [L_EXEC_APPROVAL]);
  clock.t = T0 + 4000;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'waiting');
  assert.strictEqual(snap.pendingApprovals, 1);
  assert.strictEqual(snap.heuristic, false, 'this is observed, not inferred');

  // Answering the approval is invisible on disk; the next turn boundary clears it.
  await appendLines(file, [L_COMPLETE_OK]);
  clock.t = Date.parse('2026-09-07T04:20:11.500Z') + 100;
  assert.strictEqual((await w.poll()).state, 'done');
  w.stop();
});

test('the stall heuristic only fires when approvals are actually possible', async (t) => {
  const home = await tempHome(t);
  const clock = { t: T0 };

  // approval_policy "never" (the real capture): the heuristic stays off forever.
  const homeA = home;
  const fileA = sessionFilePath(homeA);
  const wA = makeWatcher(homeA, clock);
  await appendLines(fileA, [L_META, L_STARTED, L_TURNCTX]);
  clock.t = T0 + 5 * 60 * 1000;
  const snapA = await wA.poll();
  assert.strictEqual(snapA.approvalPolicy, 'never');
  assert.strictEqual(snapA.state, 'busy', 'never-approve sessions cannot be waiting on a human');
  wA.stop();

  // approval_policy "on-request": a long silence becomes a labelled guess.
  const homeB = await tempHome(t);
  const fileB = sessionFilePath(homeB);
  const ctxOnRequest = JSON.parse(L_TURNCTX);
  ctxOnRequest.payload.approval_policy = 'on-request';
  const clockB = { t: T0 };
  const wB = makeWatcher(homeB, clockB);
  await appendLines(fileB, [L_META, L_STARTED, JSON.stringify(ctxOnRequest)]);

  clockB.t = T0 + 10 * 1000;
  assert.strictEqual((await wB.poll()).state, 'busy', 'a short silence is just a long tool call');

  clockB.t = T0 + 60 * 1000;
  const snapB = await wB.poll();
  assert.strictEqual(snapB.state, 'waiting');
  assert.strictEqual(snapB.heuristic, true, 'flagged as inferred, never presented as fact');

  // The same session with the heuristic disabled stays busy.
  const wC = makeWatcher(homeB, clockB, { approvalHeuristic: false });
  await wC.poll();
  assert.strictEqual((await wC.poll()).state, 'busy');
  wB.stop();
  wC.stop();
});

test('an open turn that goes quiet for 15 minutes stops claiming busy', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);
  await appendLines(file, [L_META, L_STARTED, L_TURNCTX]);
  clock.t = T0 + 16 * 60 * 1000;
  assert.strictEqual((await w.poll()).state, 'idle');
  w.stop();
});

test('malformed, blank and truncated lines are ignored without breaking the tail', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  await appendLines(file, [L_META, 'not json at all', '', '   ', '{"broken":']);
  clock.t = T0 + 1000;
  assert.strictEqual((await w.poll()).state, 'idle');

  // A half-written final line, with no trailing newline yet.
  await fsp.appendFile(file, '{"timestamp":"2026-09-07T04:2', 'utf8');
  clock.t = T0 + 1500;
  assert.strictEqual((await w.poll()).state, 'idle', 'partial line is held back');

  // The writer finishes that line: it must now take effect.
  await fsp.appendFile(file, '0:00.641Z","ordinal":1,"type":"event_msg","payload":' +
    '{"type":"task_started","turn_id":"t1","started_at":1788754800,' +
    '"model_context_window":258400}}\n', 'utf8');
  clock.t = T0 + 2000;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'busy', 'the completed line is parsed on the next poll');
  assert.strictEqual(snap.contextWindow, 258400);
  w.stop();
});

test('file rotation: a newer session file wins and state resets', async (t) => {
  const home = await tempHome(t);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  const first = sessionFilePath(home, 'rollout-2026-09-07T06-20-00-aaaa.jsonl');
  await appendLines(first, [L_META, L_STARTED, L_TURNCTX, L_COMPLETE_OK]);
  clock.t = Date.parse('2026-09-07T04:20:11.500Z') + 100;
  let snap = await w.poll();
  assert.strictEqual(snap.state, 'done');
  assert.strictEqual(w.currentFile, first);

  // A brand new session starts. Make sure its mtime is strictly newer.
  const second = sessionFilePath(home, 'rollout-2026-09-07T07-00-00-bbbb.jsonl');
  await appendLines(second, [L_META, L_STARTED]);
  const later = new Date(Date.now() + 60000);
  fs.utimesSync(second, later, later);

  w._rescanQueued = true;
  clock.t = T0 + 60 * 1000;
  snap = await w.poll();
  assert.strictEqual(w.currentFile, second, 'adopted the newer file');
  assert.strictEqual(snap.state, 'busy');
  assert.strictEqual(snap.lastAgentMessage, null, 'state reset on adoption');
  w.stop();
});

test('truncation in place re-seeds instead of desyncing', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);

  await appendLines(file, [L_META, L_STARTED, L_TURNCTX, L_COMPLETE_OK]);
  clock.t = Date.parse('2026-09-07T04:20:11.500Z') + 100;
  assert.strictEqual((await w.poll()).state, 'done');

  await fsp.writeFile(file, '', 'utf8');
  clock.t += 1000;
  assert.strictEqual((await w.poll()).state, 'sleep', 'emptied file means nothing is known');

  await appendLines(file, [L_META, L_STARTED]);
  clock.t += 1000;
  assert.strictEqual((await w.poll()).state, 'busy');
  w.stop();
});

test('a deleted session file does not throw and is replaced on rescan', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };
  const w = makeWatcher(home, clock);
  const errors = [];
  w.on('error', (e) => errors.push(e));

  await appendLines(file, [L_META, L_STARTED]);
  assert.strictEqual((await w.poll()).state, 'busy');

  fs.rmSync(file);
  clock.t += 1000;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'sleep');
  assert.deepStrictEqual(errors, []);
  w.stop();
});

test('startup seeds from the tail of an existing file, not from the top', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };

  // Write the whole real session, plus the synthetic completion, before we look.
  await appendLines(file, realLines.concat([L_USAGE, L_TOKENCOUNT, L_COMPLETE_OK]));

  const w = makeWatcher(home, clock);
  clock.t = Date.parse('2026-09-07T04:20:11.500Z') + 100;
  const snap = await w.poll();

  assert.strictEqual(snap.state, 'done', 'seeded state, no replay of a stale turn');
  assert.strictEqual(snap.contextWindow, 258400);
  assert.strictEqual(snap.totalTokens, 14732);
  assert.strictEqual(snap.model, 'gpt-5-codex');
  assert.ok(w.position > 60000, 'tail resumed at EOF of a 67 KB file');
  w.stop();
});

test('seedLines caps how far back startup reads', async (t) => {
  const home = await tempHome(t);
  const file = sessionFilePath(home);
  const clock = { t: T0 };

  // 300 filler lines, then the interesting ones. With seedLines=200 the
  // session_meta at the top must be out of range.
  const filler = [];
  for (let i = 0; i < 300; i += 1) {
    filler.push(
      JSON.stringify({
        timestamp: '2026-09-07T04:19:00.000Z',
        ordinal: i,
        type: 'response_item',
        payload: { type: 'message', id: `msg_${i}` }
      })
    );
  }
  await appendLines(file, [L_META].concat(filler, [L_STARTED]));

  const w = makeWatcher(home, clock, { seedLines: 200 });
  clock.t = T0 + 1000;
  const snap = await w.poll();
  assert.strictEqual(snap.state, 'busy');
  assert.strictEqual(snap.sessionId, null, 'session_meta was outside the 200-line seed window');
  w.stop();
});

test('readTailLines never returns a partial line', async (t) => {
  const home = await tempHome(t);
  const file = path.join(home, 'tail.jsonl');
  await fsp.writeFile(file, 'aaa\nbbb\nccc\npartial-no-newline', 'utf8');
  const { lines } = await readTailLines(file, 200, 1024 * 1024);
  assert.deepStrictEqual(lines, ['aaa', 'bbb', 'ccc']);

  // A byte cap that lands mid-line must drop that clipped first line.
  const { lines: clipped } = await readTailLines(file, 200, 10);
  for (const l of clipped) {
    assert.ok(['aaa', 'bbb', 'ccc'].includes(l), `clipped tail kept a whole line: ${l}`);
  }
});

test('newestSessionFile picks by mtime and ignores non-rollout files', async (t) => {
  const home = await tempHome(t);
  const dir = path.join(home, 'sessions', '2026', '09', '07');
  await fsp.mkdir(dir, { recursive: true });
  const older = path.join(dir, 'rollout-a.jsonl');
  const newer = path.join(dir, 'rollout-b.jsonl');
  await fsp.writeFile(older, 'x\n');
  await fsp.writeFile(newer, 'y\n');
  await fsp.writeFile(path.join(dir, 'notes.txt'), 'ignore me\n');
  await fsp.writeFile(path.join(dir, 'other.jsonl'), 'ignore me too\n');
  const past = new Date(Date.now() - 60000);
  fs.utimesSync(older, past, past);

  const best = await newestSessionFile(path.join(home, 'sessions'));
  assert.strictEqual(best.path, newer);
  assert.strictEqual(await newestSessionFile(path.join(home, 'nope')), null);
});

test('SessionState tolerates records with no usable timestamp', () => {
  const s = new SessionState();
  s.apply({ type: 'event_msg', payload: { type: 'task_started', turn_id: 't' } }, T0);
  const snap = s.derive(T0 + 1000, true);
  assert.strictEqual(snap.state, 'busy');
  assert.ok(Math.abs(snap.elapsedSec - 1) < 0.01, 'falls back to the feed time');
});

test('compacted lines carry token totals forward', () => {
  const s = new SessionState();
  s.apply(JSON.parse(L_META), T0);
  s.apply(JSON.parse(L_STARTED), T0);
  s.apply(
    {
      timestamp: '2026-09-07T04:25:00.000Z',
      type: 'compacted',
      payload: {
        message: 'compacted',
        window_number: 2,
        // CompactedItem.latest_token_usage_record is a full TokenUsageRecord
        // (history/src/lib.rs:198), so it carries `usage` (the live context
        // size of the last response) as well as the running totals.
        latest_token_usage_record: {
          usage: { input_tokens: 84000, output_tokens: 2000, total_tokens: 86000 },
          thread_token_usage: { input_tokens: 90000, output_tokens: 8000, total_tokens: 98000 }
        }
      }
    },
    T0
  );
  const snap = s.derive(Date.parse('2026-09-07T04:25:01.000Z'), true);
  assert.strictEqual(snap.totalTokens, 98000, 'cumulative accounting survives compaction');
  assert.strictEqual(snap.contextTokens, 86000, 'the ring follows the live context size');
  assert.strictEqual(snap.ctxFill, contextFill(86000, 258400));
});

test('a sleeping board shows no timer, not the previous turn frozen', () => {
  const s = new SessionState();
  s.apply(JSON.parse(L_META), T0);
  s.apply(JSON.parse(L_STARTED), T0);
  s.apply(JSON.parse(L_TURNCTX), T0);
  s.apply(JSON.parse(L_COMPLETE_OK), T0);

  const done = s.derive(Date.parse('2026-09-07T04:20:11.500Z') + 100, true);
  assert.strictEqual(done.state, 'done');
  assert.strictEqual(done.elapsedSec, 11.5, 'a finished turn still reports its duration');

  const asleep = s.derive(Date.parse('2026-09-07T04:20:11.500Z') + 6 * 60 * 1000, true);
  assert.strictEqual(asleep.state, 'sleep');
  assert.strictEqual(
    asleep.elapsedSec,
    null,
    'a stale number the user cannot interpret is worse than no number'
  );
  const { buildFrame } = require('../src/frame');
  assert.strictEqual(buildFrame(asleep).center, '--');
  assert.strictEqual(buildFrame(asleep).sub, 'gpt-5-codex');
});

test('an approval nobody ever answered stops pinning the amber ring', () => {
  const s = new SessionState();
  s.apply(JSON.parse(L_META), T0);
  s.apply(JSON.parse(L_STARTED), T0);
  const ctx = JSON.parse(L_TURNCTX);
  ctx.payload.approval_policy = 'on-request';
  s.apply(ctx, T0);
  s.apply(JSON.parse(L_EXEC_APPROVAL), T0);

  const approvalAt = Date.parse('2026-09-07T04:20:09.000Z');
  const fresh = s.derive(approvalAt + 1000, true);
  assert.strictEqual(fresh.state, 'waiting');
  assert.strictEqual(fresh.pendingApprovals, 1);
  assert.strictEqual(fresh.heuristic, false, 'observed, not inferred');

  // Eleven minutes later the request is no longer evidence of anything. The
  // stall heuristic may still call it waiting, but only as a labelled guess.
  const stale = s.derive(approvalAt + 11 * 60 * 1000, true);
  assert.strictEqual(stale.pendingApprovals, 0, 'the entry expired');
  assert.strictEqual(stale.state, 'waiting');
  assert.strictEqual(stale.heuristic, true, 'downgraded from fact to guess');
});

test('agentMessageText joins the content array and ignores anything else', () => {
  assert.strictEqual(
    agentMessageText({ type: 'AgentMessage', id: 'x', content: [{ type: 'Text', text: 'hi' }] }),
    'hi'
  );
  assert.strictEqual(
    agentMessageText({ content: [{ type: 'Text', text: 'a' }, { type: 'Text', text: 'b' }] }),
    'ab'
  );
  assert.strictEqual(agentMessageText({ content: [] }), null);
  assert.strictEqual(agentMessageText({ content: [{ type: 'Text' }] }), null);
  // The shape the code used to look for is not the shape items.rs defines.
  assert.strictEqual(agentMessageText({ text: 'legacy' }), null);
  assert.strictEqual(agentMessageText(null), null);
});

test('readTailLines never stringifies past what the read returned', async (t) => {
  const home = await tempHome(t);
  const file = path.join(home, 'short.jsonl');
  await fsp.writeFile(file, 'aaa\nbbb\n', 'utf8');
  // maxBytes far larger than the file: the allocated buffer is mostly zero
  // fill, and only bytesRead of it is real.
  const { lines, size } = await readTailLines(file, 200, 4 * 1024 * 1024);
  assert.deepStrictEqual(lines, ['aaa', 'bbb']);
  assert.strictEqual(size, 8);
  for (const l of lines) {
    assert.ok(!l.includes('\u0000'), 'no NUL padding leaked into a line');
  }
});

test('tokens/sec is derived from consecutive usage records', () => {
  const s = new SessionState();
  s.apply(JSON.parse(L_META), T0);
  s.apply(JSON.parse(L_STARTED), T0);

  const rec = (iso, out, total) => ({
    timestamp: iso,
    type: 'token_usage_record',
    payload: {
      usage: { input_tokens: 100, output_tokens: out, total_tokens: total },
      thread_token_usage: { input_tokens: 100, output_tokens: out, total_tokens: total }
    }
  });

  s.apply(rec('2026-09-07T04:20:10.000Z', 100, 20000), T0);
  assert.strictEqual(s.derive(T0, true).tps, null, 'one record is not a rate');

  s.apply(rec('2026-09-07T04:20:20.000Z', 250, 30000), T0);
  assert.strictEqual(s.derive(T0, true).tps, 25, '250 output tokens over 10 s');
});

test('the watcher can start and stop against a real directory without leaking timers', async (t) => {
  const home = await tempHome(t);
  // This one runs on the real clock, so the turn has to start now, not in 2026.
  const nowMs = Date.now();
  const startedNow = JSON.stringify({
    timestamp: new Date(nowMs).toISOString(),
    ordinal: 1,
    type: 'event_msg',
    payload: {
      type: 'task_started',
      turn_id: 'live-turn',
      started_at: Math.floor(nowMs / 1000),
      model_context_window: 258400
    }
  });
  await appendLines(sessionFilePath(home), [L_META, startedNow]);
  const w = new CodexWatcher({ codexHome: home, pollMs: 20, rescanMs: 40 });
  const states = [];
  w.on('state', (s) => states.push(s.state));
  await w.start();
  await new Promise((r) => setTimeout(r, 150));
  w.stop();
  assert.ok(states.length >= 1, 'emitted at least one state');
  assert.strictEqual(states[0], 'busy');
});
