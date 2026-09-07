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
| What does it write to my machine? | One file, `~/.codex/hooks.json`, and only when you run `install-hook`. |
| Can I undo it? | `uninstall-hook`, then unplug the board. Nothing else was ever written. |

Check every one of those yourself, from this directory:

```bash
ls
# codex-companion.js, its tests, a README and a package.json. That is all.

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
{"state":"waiting","ring":0.62,"center":"62%","label":"CTX","sub":"your turn"}
```

`state` is one of `sleep`, `idle`, `busy`, `waiting`, `done`. `ring` is 0 to 1.
`center`, `label` and `sub` are short strings this program clamps to 15, 23 and
39 characters, matching the firmware's buffers. There is no other traffic in
either direction, and the full specification is in `docs/protocol.md`.

Note what is **not** in that line: no tool name, no command, no file path, no
model name, no session id. The screen says that you are the one holding things
up. Your terminal says what for.

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
codex-companion demo            drive every state, no Codex involved
codex-companion help            usage
```

Options: `--port /dev/cu.usbmodemXXX`, `--codex-home /path`, `--dry-run`,
`--loop` (demo), `--no-metrics` (hook: send state only, read no files).

`run` is the no-install way to use the device: it polls the newest session file
and sends only the metric fields, never `state`. Since any field may be
omitted and the board keeps the last value it saw, running it alongside the
hook composes rather than fights. On its own it gives you a live context ring
with no hook installed at all.

---

## doctor

`doctor` answers nearly every question in one screen:

```
node            v26.7.0 on darwin
codex-companion /Users/you/helper/codex-companion.js

serial ports    /dev/cu.usbmodem0054452, /dev/cu.usbmodem101
device          /dev/cu.usbmodem101
                /dev/cu.usbmodem101 reports USB 303a:1001
handshake       acked our keepalive

codex           codex-cli 0.153.4
CODEX_HOME      /Users/you/.codex
hooks.json      not present. Run: codex-companion install-hook

approvals       nothing in config.toml suppresses approval prompts

session         /Users/you/.codex/sessions/2026/09/07/rollout-....jsonl
metrics         ctx (unknown), window 258400, turn closed
frame           {"ring":0.15,"center":"0:06","label":"TIME","sub":"0:06 elapsed"}
```

(That is real output from a machine with the board plugged in and the hook not
yet installed. `ctx (unknown)` means the last session's tail carried no token
accounting, which is normal between sessions; the label falls back to `TIME`
rather than inventing a percentage.)

The `approvals` line is the one to read. If your `config.toml` sets
`approval_policy = "never"`, `sandbox_mode = "danger-full-access"` or
`approvals_reviewer = "auto_review"`, then Codex never raises an approval
prompt, so the amber light can never appear and the device will look broken
when it is working perfectly. `doctor` prints a warning naming the setting and
the section it found it in. This is the single most common support question for
software like this, which is why it is checked by name.

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

---

## Development

```bash
npm install   # installs nothing; there are no dependencies
npm test      # 94 tests, node:test, no network, no serial port opened
```

The suite covers the `ioreg` parser and the refusal to guess between two
boards, the frame builder and its clamps, the metrics reader against real
captured session fixtures, the `hooks.json` merge and unmerge, and the hook
itself end to end: a real child process, a realistic payload on stdin for each
of the eight events, and the port pointed at an ordinary file so the exact
bytes that would go on the wire are asserted, including that none of the
payload leaked into them.

Verified against real hardware: a T-Display-S3 on `/dev/cu.usbmodem101`
accepted all eight event frames and acknowledged every one.
