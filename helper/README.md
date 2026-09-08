# codex-companion

Lights up a small screen on your desk when your Codex session needs you.

The device is a LILYGO T-Display-S3 on the end of a USB cable. It is already
finished: plug it into any USB port and it runs its own ambient animation
forever, with no host software at all. **This program is an optional bonus.**
If you never install it, nothing is lost and the device still works.

If you do install it, the screen adds a ring showing how full your context
window is and a colour showing what your Codex session is doing. The one that
matters is the amber pulse: your agent is waiting on **you**, and you can see
it from across the room without switching windows.

---

## The short version for whoever has to approve this

| Question | Answer |
|---|---|
| How much code is there? | One file, `codex-companion.js`. There is no other source file. |
| What does it depend on? | Nothing. Four Node built-ins: `fs`, `path`, `os`, `child_process`. |
| Does it phone home? | No. It cannot: no networking module is loaded anywhere. |
| Does it install a background service? | No launch agent, no daemon, no login item, no cron. |
| Can it approve or deny a tool call? | No, for two independent reasons (below). |
| What does it read from my session? | Two fields of the hook payload, plus numbers out of the session file Codex already writes. |
| What does it write to my machine? | One file, `~/.codex/hooks.json`, and only when you run `install-hook`. A test scans the source for every other write call and fails if one appears. |
| Can I undo it? | `uninstall-hook`, then unplug the board. Nothing else was ever written. |

Check every one of those yourself, from this directory:

```bash
ls
# codex-companion.js, its tests, a README, a package.json, and the empty
# lockfile npm writes on the next line. That is all.

grep -n "require(" codex-companion.js
# 4 lines: node:fs, node:path, node:os, node:child_process

grep -nE "require\((.)(node:)?(http|https|net|tls|dns|dgram)" codex-companion.js
grep -nE "fetch\(|XMLHttpRequest|WebSocket" codex-companion.js
# both silent: it has no way to reach the network

grep -niE "launchctl|launchagent|systemctl|crontab|plist|daemon" codex-companion.js
# one hit, the comment that says it installs none of those

grep -nE "tool_input|tool_response|last_assistant_message" codex-companion.js
# comments only: those fields are named where the code says it ignores them

node codex-companion.js install-hook --dry-run   # prints the file, writes nothing
npm install                                      # "audited 1 package", because there is one
npm test
```

---

## Install

Node 20 or newer. macOS is the supported platform.

```bash
node codex-companion.js doctor          # look before you leap
node codex-companion.js install-hook    # prints the file, then writes it
```

`install-hook` prints the exact path and the exact bytes it is about to write,
merges into an existing `hooks.json` instead of replacing it, and adds only its
own matcher groups so removing them cannot disturb yours. Add `--dry-run` to
print and write nothing.

Two things it will not do. It never repairs a `hooks.json` whose shape it did
not write: an array at the top level, or an event key holding something other
than an array, earns the same refusal an unparseable file does, because
merging into one of those means throwing your value away without telling you.
And it never leaves one of its own handlers naming a path that has moved: a
stale command is rewritten in place and reported as such (`Rewrote the stale
command for: ...`), carrying a hand-appended `--no-metrics` forward. That is
what makes doctor's remedy for a moved node binary the one that works.

Codex will not run a hook it has not been told to trust. The next time you
start Codex it says **"Hooks need review"**; approve it there, or list hooks
any time with the `/hooks` slash command. Until you do, the hook is registered
and inert.

## Uninstall

```bash
node codex-companion.js uninstall-hook
```

It removes exactly the handlers it added, leaves anything else in the file
alone, and deletes `hooks.json` entirely if it held nothing else. Then unplug
the board. Nothing else was ever written anywhere.

---

## What it does

### The hook path (the real one)

Codex has a first-party hook system. `install-hook` registers this same file as
a `command` handler for eight events, and each event sets one word on the
screen:

| Codex event | Screen |
|---|---|
| `SessionStart` | idle |
| `UserPromptSubmit` | busy |
| `PreToolUse` | busy |
| `PermissionRequest` | **waiting** (amber pulse) |
| `PostToolUse` | busy |
| `Stop` | done (one green flash) |
| `Interrupt` | idle |
| `SessionEnd` | idle |

Codex has no "permission resolved" event, so the amber pulse is cleared by
whatever happens next, which is `PreToolUse`, `PostToolUse` or `Stop`. If you
walk away instead, the device's own timers handle it: it dims after 30 seconds
and returns to its ambient animation after five minutes.

### The metrics path (numbers only)

Hook payloads carry no token counts. The ring, the percentage in the middle and
the stopwatch come from the session rollout file Codex is already writing to
`~/.codex/sessions/`. That file is read, never written, and only these record
types are parsed at all:

```
token_count  token_usage_record  compacted
task_started  turn_started  task_complete  turn_complete  turn_aborted
```

Every other line, which is to say every line that carries your prompts, the
model's replies, command output or file contents, is skipped by a substring
test **before** `JSON.parse` ever sees it. See `METRIC_HINTS` and
`isMetricLine`. Everything that survives into a frame is a number, a timestamp
or one of five fixed state words. A test asserts this by walking the entire
metrics object produced from a real captured session and failing if any value
is a string.

Context fill uses Codex's own baseline-adjusted formula, not `used / window`,
so the percentage agrees with what the Codex TUI shows. When a rate-limit
window is both fresh and at 80% or more, the ring switches to it and the label
reads `QUOTA`, because at that point the quota, not the context window, is the
number you need. A stale reading is never shown, so the ring can never sit at a
meaningless permanent 100%.

### What goes down the wire

One JSON object per line, at 115200 baud. This is the whole protocol:

```json
{"state":"waiting","ring":0.62,"center":"62%","label":"CTX","sub":"your turn","time":1788850592,"tokens":1209000}
```

`state` is one of `sleep`, `idle`, `busy`, `waiting`, `done`. `ring` is 0 to 1.
`center`, `label` and `sub` are short strings this program clamps to 15, 23 and
39 characters, matching the firmware's buffers. There is no other traffic in
either direction, and the full specification is in `docs/protocol.md`.

The last two are for the board's stats screen, and both are optional: a board
running older firmware ignores them.

- **`time`** is the clock on this machine, in **local** seconds (unix time plus
  this machine's own UTC offset). The board asks it exactly one question, which
  is whether midnight has happened where you are sitting, and it has no timezone
  database to answer that from UTC. It is read fresh every time, so carrying a
  laptop across a timezone, or through a daylight-saving change, corrects itself
  on the next hook event with nothing stored anywhere. A board that is never
  told it counts from boot instead and says so on its own screen.
- **`tokens`** is this Codex session's cumulative total, the same
  `total_token_usage` the ring already reads. The board banks the differences
  rather than the value, so a new session starting over at zero costs nothing
  and this program still remembers nothing between hook invocations.

Note what is **not** in that line: no tool name, no command, no file path, no
model name, no session id. Both new fields are numbers, which is the same bar
every other value here has to clear. The screen says that you are the one
holding things up. Your terminal says what for.

---

## What it never does

**It never approves or denies anything.** Two independent mechanisms, either
one sufficient on its own:

1. The hook writes nothing to stdout and exits 0. That is Codex's documented
   way for a `PermissionRequest` handler to decline to decide and let the
   normal approval flow continue
   (`vendor/codex/codex-rs/hooks/src/events/permission_request.rs`, the module
   comment and the `trimmed_stdout.is_empty()` branch of `parse_completed`).
2. It is registered with `"async": true`. Codex only applies an allow or a deny
   from a handler whose execution mode is `Sync`
   (`hooks/src/engine/mod.rs`, `can_apply_control_effects`), so even a hook that
   did print a verdict would be ignored.

The approval prompt appears in your terminal and you answer it, exactly as you
would with this program uninstalled.

**It never blocks your session.** Every handler except `SessionEnd` is
registered async, which Codex schedules and does not wait on
(`hooks/src/engine/dispatcher.rs`). `SessionEnd` is the one event Codex always
runs synchronously, with a hard 3-second cap, so it is registered with a
2-second timeout. On top of that, every failure path inside this program is
"do nothing and exit 0": no device, a busy port, a malformed payload, an
unreadable session file, an outright bug. A test spawns the real hook for every
event with the port pointed at a device that does not exist and asserts exit 0
each time.

**It never opens a network connection**, writes outside `~/.codex/hooks.json`,
or starts anything that outlives the command you typed. The default command
runs in the foreground and Ctrl-C ends it completely.

---

## Commands

```
codex-companion                 stream context metrics in the foreground
codex-companion run             the same thing, named
codex-companion hook            hook entry point: one payload on stdin
codex-companion install-hook    print, then merge our handlers into hooks.json
codex-companion uninstall-hook  remove exactly those handlers again
codex-companion doctor          device, codex, hook registration, config traps
codex-companion selftest        drive a real board and check every answer
codex-companion demo            drive every state, no Codex involved
codex-companion help            usage
```

Options: `--port /dev/cu.usbmodemXXX`, `--codex-home /path`, `--dry-run`,
`--loop` (demo), `--no-metrics` (hook: send state only, read no files),
`--secs N` (say: hold for N seconds, 0 until cleared), `--mins N` (focus: the
timer's length, 1 to 180), `--clear` (say, name: send the empty value),
`--strict` (doctor: exit non-zero when something is broken).

`run` is the no-install way to use the device: it polls the newest session file
and sends only the metric fields, never `state`. Since any field may be
omitted and the board keeps the last value it saw, running it alongside the
hook composes rather than fights. On its own it gives you a live context ring
with no hook installed at all. It also survives the cable: unplug the board
mid-run and it says so once, then reconnects on a backoff that widens to half
a minute rather than hammering an empty USB bus.

---

## Talking to the board without Codex

The firmware has features that have nothing to do with an agent, and each one
has two doors: a gesture on the board's two buttons, and a field on the wire.
These commands are the second door, so you never have to remember `printf`
quoting at your desk. None of them needs Codex, a hook, or a session.

```bash
codex-companion say "back in five"          # a sign anyone can read across a room
codex-companion say "in a meeting" --secs 0 # hold it until it is cleared
codex-companion say --clear                 # or press either button on the board

codex-companion name "Alex Rivera"          # stored on the board, survives unplugging
codex-companion face rounded|bear|arc       # also stored on the board
codex-companion time                        # give the daily figures a midnight

codex-companion dnd on|off                  # the DO NOT DISTURB sign
codex-companion stats on|off                # the numbers screen
codex-companion focus start --mins 25       # the pomodoro
codex-companion play on|hop|off             # the toy
codex-companion firstrun                    # replay the out-of-the-box sequence
```

Three things are true of all of them:

- **What is printed is what went out.** The clamping and the sanitising happen
  here, so `say "cafe latte"` and `say` with an emoji in it print different
  lines and you can see which characters the panel will not draw. The board's
  fonts are ASCII, names cap at 24 characters and messages at 48.
- **A value the board would reject is never sent.** Fields like `face` and
  `stats` are enums, and the firmware drops the *entire line* when one is
  misspelled, taking every good field on that line with it. So a bad value
  earns a usage message here instead of a silent nothing there.
- **`--dry-run` prints the line and opens no port**, which is the safe way to
  see exactly what a command does.

There is deliberately **no factory-reset command**. It wipes the owner name,
the face and the stored records, and something that destructive should cost
more than a typo:

```bash
printf '{"reset":"factory"}\n' > /dev/cu.usbmodemXXX
```

The full field-by-field specification is `docs/protocol.md`.

---

## doctor

`doctor` is the setup half of the question: is this machine wired up. It runs
every check in the order the checks have to be true, and **every finding that
is not `ok` is followed by exactly what to do about it**, because the person
reading it has a present that is not working rather than a codebase they wrote.

```
WHERE THIS IS RUNNING
       node            v26.7.0 on darwin
       companion       /Users/you/helper/codex-companion.js

DEVICE
       serial ports    /dev/cu.usbmodem0054452, /dev/cu.usbmodem1101
  ok   device          /dev/cu.usbmodem1101
                       /dev/cu.usbmodem1101 reports USB 303a:1001
  ok   port opens      read and write
  ok   handshake       acked our keepalive

CODEX
  ok   codex           codex-cli 0.153.4
       CODEX_HOME      /Users/you/.codex
  FAIL hooks.json      not present
       -> Nothing is registered, so no event can reach the board. Run: node
       -> /Users/you/helper/codex-companion.js
       -> install-hook

APPROVALS
  ok   approvals       nothing in config.toml suppresses approval prompts

SESSION
       session         /Users/you/.codex/sessions/2026/09/07/rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl
       metrics         ctx (unknown), window 258400, turn closed
       frame           {"ring":0.15,"center":"0:06","label":"TIME","sub":"0:06 elapsed","time":1788855698}

1 problem(s) and 0 warning(s). Each one above is followed by what to do about it.
The device itself needs none of this: with no host at all it runs its
own ambient mode forever. Every finding here is about the extras.
```

(Real output from this machine, with the board plugged in and the hook not yet
installed. `ctx (unknown)` means the last session's tail carried no token
accounting, which is normal between sessions; the label falls back to `TIME`
rather than inventing a percentage.)

Every realistic failure is checked by name rather than left to be inferred:

| Finding | What it means, and what doctor tells you to do |
|---|---|
| no serial ports at all | Almost always a charge-only USB-C cable: it powers the board and carries no data, so no port ever appears |
| two boards match | Both report USB `303a:1001` and only you can say which is yours, so it refuses to guess and names the `--port` to pass |
| permission denied on the port | `EACCES`: these ttys ship world-writable, so this is a declined privacy prompt or a root-owned node. Replug rather than `chmod` a device node |
| the port is busy | `EBUSY`: quit `pio device monitor`, `screen`, or another copy of this, then re-run. `lsof` names the holder. Rare on macOS, see the row below |
| another program has the port open too | A macOS callout port (`/dev/cu.*`) does not lock: two programs can open it at once and neither is told, so this never arrives as `EBUSY`. doctor asks `lsof` and names the pids, because the symptom otherwise is a board that answers half the time |
| the board never answers | The port opened and nothing came back, so it is not this firmware or the board is wedged |
| Codex is not on this PATH | It hooks into the Codex CLI. `command -v codex` from the shell you actually run Codex in |
| `hooks.json` missing, or somebody else's | Nothing of ours is registered, so no event can reach the board: run `install-hook`, which merges in and disturbs nothing |
| `hooks.json` is not valid JSON | Codex cannot read it either, so every hook in it is dead. It refuses to touch a file it cannot parse |
| registered, never approved | Codex will not run a hook it has not been told to trust. Approve it at startup or with `/hooks` |
| **registered, then switched off** | `enabled = false` in `config.toml`. Identical symptom to the line above and a completely different fix, which is why they are separate findings |
| the hook command has moved | The registered command names a node binary or a script that no longer exists, which is what `brew upgrade node` and moving this folder both do. The hook then fails silently forever. Re-running `install-hook` rewrites it |
| a config that can never prompt | `approval_policy = "never"` or `approvals_reviewer = "auto_review"`, at the top level of `config.toml`. The amber light can never appear and the device looks broken while working perfectly. This is the single most common support question for software like this |
| the same setting inside a profile | A `never` under `[profiles.something]` applies only when that profile is selected, so it is a warning naming the profile, not a verdict on the file. `sandbox_mode` is not checked at all: the sandbox is not the approval gate, `approval_policy` is (`codex-rs/core/src/exec_policy.rs` prompts for `UnlessTrusted` whatever the sandbox kind) |

Hook trust is read out of the `[hooks.state."..."]` records Codex keeps in
`config.toml` (`HookStateToml` in `codex-rs/config/src/hook_config.rs`). The
`/hooks` command inside Codex is the authority; doctor says so.

`doctor` exits 0 even when it finds problems, because a report that fails the
shell is a report people stop running. `--strict` is the version for a script.

---

## selftest

`doctor` answers "is this set up". `selftest` answers "does it work", which is
a different question and the one worth asking before you hand somebody a
present. Every check is a real line down the real cable and a real answer read
back off it.

```
$ codex-companion selftest
codex-companion selftest: driving a real board over the real cable.

PASS  a board is on the cable            /dev/cu.usbmodem1101
PASS  the port opens for reading and writing /dev/cu.usbmodem1101
PASS  the board answers                  ack
PASS  all five states are accepted       12 acks for 6 frames
PASS  an unknown state drops the whole line nothing on that line was applied
PASS  an unknown face drops the whole line nothing on that line was applied
PASS  a message reaches the panel        say: showing "self test" hold lines=1 font=24
PASS  an empty message clears the card   say: cleared
PASS  the stats screen opens and carries its figures stats: open (host) turns=1 tokens=0 ...
PASS  the clock reached the board        day=today
PASS  the token total banks the difference 0 -> 12345
PASS  the stats screen closes            stats: close (host) turns=1 tokens=12345 ...
PASS  the do-not-disturb sign goes up    dnd: on (host) font=24 w=186,213 max=268
PASS  and comes down again               dnd: off (host)
PASS  the focus timer shows              focus: show (host) len=25m left=25:00 stopped
PASS  and hides again                    focus: hide (host) left=25:00 stopped
PASS  the toy opens                      play: hop (host) best=0
PASS  and closes                         play: end (host) score=0 best=0

Nothing here was persisted. The board goes back to its ambient mode on
its own five minutes after the last line, or immediately on a replug.

18 checks, 18 passed, 0 failed.
The board answered every line this program can send. It is good to give.
```

Three of those are worth explaining, because they are the ones that could not
be done any other way:

- **"an unknown state drops the whole line"** sends a message and a bad enum on
  one line. If the board had applied half of it, the message would appear and
  it would say so. Nothing appearing is the proof.
- **"the token total banks the difference"** sends two running totals and reads
  the screen twice, asserting the day moved by the difference and not by the
  second value. An absolute figure would only be right on a board nobody had
  ever used, which is the one board this would never be run on.
- **"a message reaches the panel"** is the only field the board echoes back
  exactly as it laid it out, wrapping and type size included, so it is the one
  check that proves the whole chain rather than just the acknowledgement. The
  card is sent with no expiry, which is what `hold` in that row means: a timed
  card can expire before the next check takes it down, and a check may not
  outlive its own subject.

It writes nothing persistent: no name, no face, no records, and never a
`reset`. The only thing it leaves behind is about thirteen thousand tokens on
the day's counter, which is RAM on the device and gone at the next power
cycle. It exits non-zero if any check fails, and it stops early if the board
never answers at all rather than failing fifteen more checks for the same
reason.

---

## Troubleshooting

**`device NOT FOUND`.** In order: the cable (a charge-only USB-C cable powers
the board but carries no data, so no port ever appears), then the other USB
port on the board, then `doctor`, which lists every serial port it can see. If
a likely port is there but was not picked, name it: `--port /dev/cu.usbmodemXXX`.

**Two devices matched.** Auto-detection matches Espressif's USB vendor id
`303a` and product id `1001`, read from `ioreg`. If two boards both match, it
refuses to guess and asks for `--port`. That refusal is deliberate.

**The hook is registered but nothing happens.** Codex will not run an untrusted
hook. Start Codex and approve the review prompt, or check `/hooks`.

**It never shows amber.** See the `approvals` line in `doctor`.

**The screen shows an old percentage.** The board keeps the last value it was
sent, dims after 30 seconds without traffic and returns to its ambient
animation after five minutes. Nothing is wrong.

**"the port would not take the line".** The board stopped draining the cable:
unplug it, plug it back in, and run `doctor`. A second program holding the port
is not the cause, whatever it may look like, because a `/dev/cu.*` port does
not lock and a second holder only ever steals answers, never writes. `EBUSY` on
opening is the separate case where something really does have it: quit the
other serial monitor, and `lsof /dev/cu.usbmodemXXX` names it.

**Answers arrive scrambled, or every other run fails somewhere different.**
Something else has the same port open. macOS callout ports do not lock, so a
second program opening `/dev/cu.usbmodemXXX` succeeds and the two of you then
split the bytes coming back between you, with no error anywhere. `doctor`
reports this as a `port sharing` warning with the pids, and `selftest` prints
it as a NOTE before its checks, and again after them if anything failed, since
a holder that arrives mid-run is invisible to the first look and is exactly
what a run of unexplainable failures usually is; `ps -p <pid>` names each
holder.

**A message came out with letters missing.** The panel fonts are ASCII, so
accents and emoji are dropped rather than drawn as mojibake, and runs of
whitespace collapse. The command prints the exact line it sent, which is what
the board will show.

**It all looks fine and you still are not sure.** `codex-companion selftest`
drives every one of these paths against the board in front of you and prints a
pass or a fail for each.

---

## Development

```bash
npm install   # installs nothing; there are no dependencies
npm test      # node:test, no network, no serial port opened; read the tally
```

| File | What it holds the line on |
|---|---|
| `cli.test.js` | the command line as a person uses it: argument parsing, a flag with its value missing, the command map, and an install into a scratch `CODEX_HOME` looked at and then taken out again |
| `ports.test.js` | the `ioreg` parser, and the refusal to guess between two boards |
| `frame.test.js` | the frame builder, its clamps and the quota takeover |
| `metrics.test.js` | the rollout reader, against real captured session fixtures |
| `hooks-json.test.js` | the `hooks.json` merge and unmerge |
| `hook.test.js` | the hook end to end: a real child process, a realistic payload for each of the eight events, and the port pointed at an ordinary file so the exact bytes are asserted, including that none of the payload leaked into them |
| `protocol-fields.test.js` | every field the firmware grew, and the rule that a value the board would reject is never put on the wire |
| `commands.test.js` | each command as a person types it, asserted on the bytes it sends and on a bad argument costing a usage message rather than a dropped line |
| `doctor.test.js` | one realistic broken machine per test, asserting that the finding **and its remedy** are both printed |
| `hardening.test.js` | the ways it could hang or spin: stdin held open, an eight megabyte payload, a closed link, a port that never answers |
| `paths.test.js` | that it still writes to exactly two places, checked against the source so a new call site cannot appear unnoticed, and against a scratch directory so it is watched doing it |

Verified against real hardware while this was written: a T-Display-S3 on
`/dev/cu.usbmodem1101` passed all eighteen `selftest` checks on three
consecutive runs, and `doctor` reported it correctly with the hook both
installed and absent.
