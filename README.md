# Codex Desk Companion

A small creature on the end of a USB cable that watches your Codex session and
tells you when it needs you.

It is a LILYGO T-Display-S3: an ESP32-S3 with a 320x170 colour LCD, two
buttons, and a native USB port. Plug it into anything that supplies 5V. A pair
of eyes fades up out of the dark, blinks, looks around, finds you, and then
tells you whose it is. After that it sits there and breathes, drifting slowly
through two blues and a teal, blinking on a randomised interval of a few
seconds, glancing off to one side now and then the way something does when it
is waiting for you to come back.

If you also run the optional host program, those eyes start reporting. They
squint and scan when your agent is working. A ring on the right fills up with
how much of your context window is gone. And when Codex stops and asks you a
question, the eyes go wide, the brows come up, and the whole head hops on an
amber pulse until you answer, then leans on it harder and turns red if you do
not. That is the one behaviour this object exists for: you can see from across
a room that your agent is blocked on you, without switching windows to find
out.

There are fourteen of these. Every one runs the byte-identical binary.

---

## It needs no software at all

This is the first design law of the project and every other decision bends to
it. **The host program is a bonus. If you never install it, nothing is lost.**

On USB power alone, with nothing on the other end of the cable, the board is a
complete object:

- **It wakes up.** The first time it is ever powered, it plays an 8.6 second
  out-of-the-box sequence: dark, a light comes up on two shut eyes, they crack
  open, squint, open, look around, find the person in front of them, blink,
  nod, and say whose it is. It plays exactly once, and the last second of it
  already *is* the resting composition, so the settle is invisible. Re-flashing
  does not bring it back, because the flag lives in the `nvs` partition and
  `pio run -t upload` does not write that region.
- **It stays alive.** The resting animation never stops moving and never shows
  an error, a spinner, or a "waiting for host" message. It rides an anti
  burn-in drift on two sine periods that do not divide into each other (9px on
  97 seconds horizontally, 5px on 61 seconds vertically), so the path never
  retraces itself.
- **It is a desk sign.** Press both buttons together and let go: DO NOT
  DISTURB, in a violet no other state on the device owns, thick enough to read
  across a room, with your name under it. Any single press takes it down.
- **It is a toy.** Hold button 1 for two seconds and the face runs along a
  line and hops over blocks. Button 1 jumps, button 2 leaves. The best score
  survives the power cycle. A Codex turn can run for ten minutes and this is
  what you do with them.
- **It is a pomodoro.** Two taps on button 1 and you get a countdown in the
  library's 48px seven-segment face, with `FOCUS` on the left of the header and
  what Codex is doing on the right, in that state's own colour. The lengths are
  5, 15, 25 and 45 minutes and your choice is stored on the board.
- **It keeps score.** One tap on button 1 shows turns today, tokens today,
  today's longest turn, this sitting's length, the longest turn ever and the
  best token day. It quietly marks a turn over five minutes, a new personal
  best, and each token milestone, as a pill that fades in over 250ms, sits
  absolutely still for a second and a half, and fades out.
- **It has three faces.** Hold button 2 and it cycles: `rounded` (soft
  rectangles with a wet highlight), `bear` (a cream creature whose mood is in
  its mouth), `arc` (two tapering hand-drawn strokes). The choice is stored.
- **It knows its name.** Names are not compiled in. One protocol line sets it
  and the board keeps it in NVS across reboots.

The fourth design law is that it must never be irritating to sit next to in an
open-plan office, and it is the reason for a lot of the above: the milestone
pill is still after its entrance, the DO NOT DISTURB sign moves only on a
6500ms breathe, the ambient backlight settles one tier down after sixty
seconds, and there is no buzzer anywhere in the design.

---

## The optional host program

`helper/codex-companion.js`. **One file, zero dependencies, no build step, no
install script, no background service.** Node
20 or newer. macOS is the supported platform, because port discovery reads
`ioreg` and the line is configured with `stty`.

It reads state from Codex's own first-party hook system. It is not a screen
scraper and it is not a heuristic.

| Codex event | What appears on the screen |
|---|---|
| `SessionStart` | idle |
| `UserPromptSubmit` | busy |
| `PreToolUse` | busy |
| `PermissionRequest` | **waiting**, the amber pulse |
| `PostToolUse` | busy |
| `Stop` | done, one green flash |
| `Interrupt` | idle |
| `SessionEnd` | idle |

Codex has no "permission resolved" event, so the amber is cleared by whatever
happens next. If you walk away instead, the board's own timers handle it: it
dims after 30 seconds and returns to its resting animation after five minutes.

### Exactly what it reads

**From the hook payload, two fields.** `hook_event_name` and
`transcript_path`. It never reads `tool_input`, `tool_response`, `prompt`,
`last_assistant_message`, `cwd`, `model` or `session_id`. Those names appear in
the file only in the comment that says they are ignored, which is a thing you
can grep for and a thing a test asserts.

**From the session rollout file Codex already writes, numbers only.** Hook
payloads carry no token counts, so the ring, the percentage and the stopwatch
come from the tail of `~/.codex/sessions/...`. That file is opened read-only,
and a line is only handed to `JSON.parse` at all if a substring test finds one
of eight record-type hints in it:

```
token_count  token_usage_record  compacted
task_started  turn_started  task_complete  turn_complete  turn_aborted
```

Every other line, which is to say every line carrying your prompts, the model's
replies, command output or file contents, is skipped as raw text and never
becomes a JavaScript object. A test walks the entire metrics object produced
from a real captured session and fails if any value in it is a string.

### Exactly what goes down the wire

```json
{"state":"waiting","ring":0.62,"center":"62%","label":"CTX","sub":"your turn","time":1788850592,"tokens":1209000}
```

Numbers, timestamps, and one of five fixed state words. `sub` on a `waiting`
frame is always the literal string `your turn`: no tool name, no command, no
file path, no model name, no session id, ever. The screen says that you are the
one holding things up. Your terminal says what for.

### Exactly what it can never do

**It can never approve or deny anything.** Two independent mechanisms, either
one sufficient on its own:

1. The `PermissionRequest` handler writes nothing to stdout and exits 0. That
   is Codex's documented way for a handler to decline to decide and let the
   normal approval flow continue.
2. Every handler except `SessionEnd` is registered with `"async": true`, and
   Codex only applies an allow or a deny from a handler whose execution mode is
   `Sync`. In `vendor/codex/codex-rs/hooks/src/engine/mod.rs`:

   ```rust
   pub(crate) fn can_apply_control_effects(&self) -> bool {
       self.execution_mode() == HookExecutionMode::Sync
   }
   ```

   So even a hook that did print a verdict would be ignored.

The approval prompt appears in your terminal and you answer it there, exactly
as you would with this uninstalled.

**It can never block your session.** Async handlers are scheduled and not
waited on. `SessionEnd` is the one event Codex always runs synchronously, with
a hard 3-second cap, so it is registered with a 2-second timeout. On top of
that, every failure path in the program is "do nothing and exit 0": no device,
a busy port, a malformed payload, an unreadable session file, an outright bug.
A test spawns the real hook for all eight events with the port pointed at a
device that does not exist and asserts exit 0 every time.

**It can never reach the network.** There is no `require` of `http`, `https`,
`net`, `tls`, `dns` or `dgram`, and no `fetch`. Grep for them.

**It can never start anything that outlives your terminal.** No launch agent,
no daemon, no login item, no cron job, on any platform. The default command
runs in the foreground and Ctrl-C ends it completely.

**It writes exactly one file, and only when you ask.** `~/.codex/hooks.json`,
written by `install-hook`, which prints the exact path and the exact bytes
first, merges into an existing file rather than replacing it, and adds only its
own matcher groups. `uninstall-hook` removes exactly those and deletes the file
if it held nothing else. There are four write calls in the entire program and
all four are in those two functions.

**Nothing runs until you say so a second time.** Codex will not execute a hook
it has not been told to trust. The next time you start Codex it says "Hooks
need review". Until you approve it there, the hook is registered and inert.

---

## Verify every one of those claims yourself

Two minutes, from `helper/`. Every command below was run to produce the output
shown. Absolute paths in the captured output are anonymized (they read
`/Users/you/codex-buddy/...` here); nothing else about the output was edited.
The greps deliberately avoid line numbers, because this is under active
development and line numbers move while the shapes below do not.

### There is one file and it has no dependencies

```console
$ wc -l codex-companion.js

$ npm install
up to date, audited 1 package in 327ms

found 0 vulnerabilities
```

The line count is deliberately not printed here. It moves every time the file
is touched and a stale number in a document whose whole pitch is "run these
yourself" is worse than no number: run the command and read what it says. What
the two commands together prove is the claim that matters, and it does not
move: it is **one file**, and installing it installs nothing. The one audited
package is this package. There is no `dependencies` key in `package.json` at
all.

### It loads four Node built-ins and nothing else

```console
$ grep -o "require('node:[a-z_]*')" codex-companion.js
require('node:fs')
require('node:path')
require('node:os')
require('node:child_process')
```

### It has no way to reach the network

```console
$ grep -cE "require\(['\"](node:)?(http|https|net|tls|dns|dgram)['\"]\)|fetch\(|XMLHttpRequest|WebSocket" codex-companion.js
0
```

Zero. No transport, no client, no socket.

### It installs no background service

```console
$ grep -iE "launchctl|launchagent|systemctl|crontab|plist|daemon" codex-companion.js
 *   - It never installs a launch agent, a daemon, a login item or a cron job.
```

One hit, and it is the comment saying it installs none of them.

### It never touches the payload fields you care about

```console
$ grep -E "tool_input|tool_response|last_assistant_message" codex-companion.js
 *     `hook_event_name` and `transcript_path`. It never looks at `tool_input`,
 *     `tool_response`, `prompt`, `last_assistant_message`, `cwd`, `model` or
```

Two hits, both inside the header comment that names them as ignored. There is
no code that reads them.

### It writes exactly one file

```console
$ grep -oE "fs\.(writeFileSync|appendFileSync|mkdirSync|unlinkSync|rmSync)|createWriteStream" codex-companion.js | sort | uniq -c
   1 fs.mkdirSync
   1 fs.unlinkSync
   2 fs.writeFileSync
```

Four calls in the whole program. Here is where they live, and what they write:

```console
$ awk '/^function cmd(Install|Uninstall)Hook/,/^}/' codex-companion.js \
    | grep -nE "writeFileSync|mkdirSync|unlinkSync|const file"
3:  const file = path.join(home, 'hooks.json');
22:  fs.mkdirSync(home, { recursive: true });
23:  fs.writeFileSync(file, text);
38:  const file = path.join(home, 'hooks.json');
56:    if (!opts.dryRun) fs.unlinkSync(file);
62:    if (!opts.dryRun) fs.writeFileSync(file, text);
```

All four are inside `cmdInstallHook` and `cmdUninstallHook`, and in both `file`
is `hooks.json` under your `CODEX_HOME`. Nothing else in the program can write
anywhere.

### It shows you the file before it writes it

```console
$ node codex-companion.js install-hook --codex-home /tmp/scratch-codex-home --dry-run
About to write /tmp/scratch-codex-home/hooks.json:

{
  "hooks": {
    "SessionStart": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "'/opt/homebrew/bin/node' '/Users/you/codex-buddy/helper/codex-companion.js' hook",
            "timeout": 5,
            "async": true
          }
        ]
      }
    ],
```

...seven more events in the same shape, then:

```console
    "SessionEnd": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "'/opt/homebrew/bin/node' '/Users/you/codex-buddy/helper/codex-companion.js' hook",
            "timeout": 2,
            "async": false
          }
        ]
      }
    ]
  }
}

--dry-run: nothing was written.
```

`SessionEnd` is the only `"async": false` entry, and it is the only one with a
timeout under five seconds, because it is the only event Codex runs
synchronously. Drop `--codex-home` and it prints the real `$CODEX_HOME` path
instead. `--dry-run` writes nothing either way.

### It cannot damage a `hooks.json` you already have

Put a hook of your own in a scratch `CODEX_HOME` (`H=$(mktemp -d)` below),
install ours on top of it, and look at what happened to yours:

```console
$ cat $H/hooks.json
{
  "hooks": {
    "PreToolUse": [
      { "hooks": [ { "type": "command", "command": "/usr/local/bin/my-own-linter", "timeout": 10 } ] }
    ]
  }
}

$ node codex-companion.js install-hook --codex-home $H
...
$ python3 -c "import json; d=json.load(open('$H/hooks.json')); print('PreToolUse groups:', len(d['hooks']['PreToolUse'])); print([h['command'] for g in d['hooks']['PreToolUse'] for h in g['hooks']])"
PreToolUse groups: 2
['/usr/local/bin/my-own-linter', "'/opt/homebrew/bin/node' '.../codex-companion.js' hook"]
```

Yours is untouched, in its own group. Now take ours back out:

```console
$ node codex-companion.js uninstall-hook --codex-home $H
Removing 8 handler(s). .../hooks.json becomes:

{
  "hooks": {
    "PreToolUse": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "/usr/local/bin/my-own-linter",
            "timeout": 10
          }
        ]
      }
    ]
  }
}
```

Byte for byte what you started with. Removal matches on the shape of the
command, not on a literal string, so it still finds its own handlers after you
have upgraded node or moved the checkout. If the file only ever held ours, it
is deleted rather than left as an empty husk. If it is not valid JSON, both
commands refuse to touch it at all.

### It tells you exactly what it will put on the wire

```console
$ node codex-companion.js demo --dry-run
{"state":"idle","ring":0.08,"center":"8%","label":"CTX","sub":"idle"}
{"state":"busy","ring":0.31,"center":"31%","label":"CTX","sub":"0:42 elapsed","tps":12.5}
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"3:07 elapsed","tps":31.2}
{"state":"waiting","ring":0.62,"center":"62%","label":"CTX","sub":"your turn"}
{"state":"done","ring":0.64,"center":"64%","label":"CTX","sub":"3:21 elapsed"}
{"state":"idle","ring":0.64,"center":"64%","label":"CTX","sub":""}
{"state":"idle","ring":0.91,"center":"91%","label":"QUOTA","sub":"weekly window"}
{"state":"sleep","ring":0,"center":"--","label":"CODEX","sub":""}
```

That is the entire vocabulary. `CTX`, `QUOTA`, `TIME` and `CODEX` are the only
four labels the program can emit, and you can check that too:

```console
$ grep -oE "label: *'[A-Z]+'" codex-companion.js | sort | uniq -c
```

Counts are left out for the same reason as the line count above, and they are
not the point: what matters is that the only names that come back are `CODEX`,
`CTX` and `QUOTA`, plus the assignments of `'TIME'`
(`grep -c "label = 'TIME'"`), the stopwatch fallback used when a session
carries no token accounting. Four labels, no fifth.

### The tests

```console
$ npm test
```

Eleven files under `helper/test/`, run by `node --test`. The tally grows as
tests are added, so read the run in front of you rather than a number pasted
here; the line to check is `fail 0`. No test opens a network socket and no
test opens a real serial port. The suite covers the `ioreg`
parser and its deliberate refusal to guess between two matching boards, the
frame builder and every clamp, the metrics reader against real captured session
fixtures, the `hooks.json` merge and unmerge against foreign files, and the
hook end to end: a real child process, a realistic payload on stdin shaped from
the Rust structs in `vendor/codex/codex-rs/hooks/src/schema.rs` (including the
fields we deliberately ignore, so that "we ignore them" is something a test can
see), and the port pointed at an ordinary file so the exact bytes that would go
on the wire are captured and asserted.

### And a single command that answers most of the rest

```console
$ node codex-companion.js doctor
WHERE THIS IS RUNNING
       node            v26.7.0 on darwin
       companion       /Users/you/codex-buddy/helper/codex-companion.js

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
       -> /Users/you/codex-buddy/helper/codex-companion.js
       -> install-hook

APPROVALS
  ok   approvals       nothing in config.toml suppresses approval prompts

SESSION
       session         /Users/you/.codex/sessions/2026/09/07/rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl
       metrics         ctx (unknown), window 258400, turn closed
       frame           {"ring":0.15,"center":"0:06","label":"TIME","sub":"0:06 elapsed","time":1788854593}

1 problem(s) and 0 warning(s). Each one above is followed by what to do about it.
The device itself needs none of this: with no host at all it runs its
own ambient mode forever. Every finding here is about the extras.
```

Real output, with a board attached and the hook deliberately not installed, so
you can see what a finding looks like. Every `FAIL` and every warning is
followed by what to do about it. `ctx (unknown)` means the tail of that session
carried no token accounting, which is normal between sessions, and the label
falls back to `TIME` rather than inventing a percentage.

The `APPROVALS` block is the one to read. If your `config.toml` sets
`approval_policy = "never"`, `sandbox_mode = "danger-full-access"` or
`approvals_reviewer = "auto_review"`, Codex never raises an approval prompt, so
the amber light can never appear and the device will look broken while working
perfectly. `doctor` names the setting and the section it found it in.

One quirk of the `handshake` line, since you will probably meet it: the
greeting is sent once, a couple of seconds after boot, so a board that has been
plugged in a while answers `ok` to the keepalive instead and `doctor` says so
rather than pretending. Unplug and replug, then run it again within a minute.

### Install and uninstall

```bash
node codex-companion.js doctor          # look before you leap
node codex-companion.js install-hook    # prints the file, then writes it
node codex-companion.js uninstall-hook  # removes exactly what it added
```

Then unplug the board. Nothing else was ever written anywhere.

The whole command surface, from the program's own `help`:

```
codex-companion                 stream context metrics in the foreground
codex-companion run             the same thing, named
codex-companion hook            hook entry point: reads one payload on stdin
codex-companion install-hook    print, then merge our handlers into hooks.json
codex-companion uninstall-hook  remove exactly those handlers again
codex-companion doctor          device, codex, hook registration, config traps
codex-companion selftest        drive a real board and check every answer
codex-companion demo            drive every state, no Codex involved
codex-companion help            this text

Talking to the board directly (none of these need Codex at all):

  codex-companion say "back in five" [--secs 300]   a message anyone can read
  codex-companion say --clear                       take it down
  codex-companion name "Alex Rivera"                stored on the board
  codex-companion face rounded|bear|arc             stored on the board
  codex-companion time [tokens]                     give the daily stats a midnight
  codex-companion dnd on|off                        the desk sign
  codex-companion stats on|off                      the numbers screen
  codex-companion focus show|hide|start|pause|reset [--mins 25]
  codex-companion play on|off|hop                   the toy
  codex-companion firstrun                          replay the out-of-the-box run

Options:
  --port /dev/cu.usbmodemXXX   use this port instead of auto-detecting
  --codex-home /path           look here instead of $CODEX_HOME or ~/.codex
  --dry-run                    print what would happen, change nothing
  --loop                       (demo) repeat until Ctrl-C
  --no-metrics                 (hook) send state only, do not read the rollout
  --secs N                     (say) hold for N seconds; 0 until cleared
  --mins N                     (focus) the timer's length, 1 to 180
  --clear                      (say, name) send the empty value
  --strict                     (doctor) exit non-zero when something is broken
```

The nine direct-to-board subcommands are the second door to the gestures the
two buttons already carry, and none of them touches Codex. `help` prints the
factory-reset `printf` line rather than offering a command for it, on purpose.

`run` is the no-install way to use this. It polls the newest session file and
sends only the metric fields, never `state`. Because any field may be omitted
and the board keeps its last value, running it alongside the hook composes
rather than fights; on its own it gives you a live context ring with no hook
installed at all.

---

## Build and flash it yourself

PlatformIO. Three pinned dependencies, one env.

```bash
cd firmware
pio run
```

```console
RAM:   [=         ]   6.7% (used 21912 bytes from 327680 bytes)
Flash: [=         ]   6.5% (used 428177 bytes from 6553600 bytes)
========================= [SUCCESS] Took 2.43 seconds =========================
```

```console
$ pio run -t upload --upload-port /dev/cu.usbmodem1101
...
Compressed 428544 bytes to 242646...
Writing at 0x00010000... (6 %)
...
Wrote 428544 bytes (242646 compressed) at 0x00010000 in 4.2 seconds (effective 819.0 kbit/s)...
Hash of data verified.

Leaving...
Hard resetting via RTS pin...
========================= [SUCCESS] Took 9.47 seconds =========================
```

**Both consoles above are a sample, not the current image.** They were pasted
from a run on 2026-09-08 and every number in them moves with every firmware
change, exactly as the sha256 below does. Read them off the run in front of
you rather than trusting these.

Drop `--upload-port` and it auto-detects. `pio device monitor -b 115200`
attaches afterwards to watch it talk.

If a board will not enter download mode: hold BOOT (button 1, GPIO 0), tap RST,
release BOOT, then upload.

The toolchain is pinned in `firmware/platformio.ini`:

| Pin | Version |
|---|---|
| platform | `espressif32@7.1.1` |
| graphics | `lovyan03/LovyanGFX@1.2.28` |
| JSON | `bblanchon/ArduinoJson@7.4.3` |

Both libraries are fetched by PlatformIO at build time from the registry. They
are not vendored into this repository and no copy of them is redistributed
here.

### One image for the whole fleet

**There is no per-unit build.** Nothing is stamped in at compile time, not the
owner name and not a unit number, so all fourteen boards are flashed from the
byte-identical binary and the fleet has one hash.

```console
$ shasum -a 256 .pio/build/tdisplays3/firmware.bin
2b28e44cec3489a6105a4f17186f46742442ef0649221304e22247fbae2262d6  .pio/build/tdisplays3/firmware.bin
```

That value is the build as of this writing and it changes whenever the firmware
changes, so recompute it rather than trusting the number above.

You get one check for free every time you flash. `Hash of data verified` in the
upload output above is esptool reading the SHA of the region it just wrote back
off the chip and comparing, so a successful upload is already proof that the
bytes on the board are the bytes in `firmware.bin`.

To check a board you did not just flash, dump the app partition and hash it.
`esptool.py` is not on your `PATH`; it ships inside PlatformIO and needs a
python with pyserial, which PlatformIO's own interpreter has:

```bash
PIOPY=$(head -1 "$(which pio)" | sed 's/^#!//')
"$PIOPY" ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --port /dev/cu.usbmodemXXXX --after hard_reset \
  read_flash 0x10000 $(stat -f%z .pio/build/tdisplays3/firmware.bin) dump.bin
shasum -a 256 dump.bin
```

**That one is documented rather than demonstrated.** The interpreter and
`esptool.py v4.11.0` are both present and runnable here, and `chip_id` over the
same port returned `MAC: ac:a7:04:f6:51:64`, but the full-image read was
attempted twice and failed partway through both times: once with `Corrupt data,
expected 0x1000 bytes but received 0xfc9 bytes`, once with pySerial's `device
reports readiness to read but returned no data (device disconnected or multiple
access on port?)`. The port was shared with another process at the time, which
is what that second message suggests, but nobody has completed a clean dump, so
do not treat it as proven. The upload-time hash check above has been exercised
and is the one that has actually run.

Units still tell themselves apart. With no unit id compiled in, the label comes
from the chip's own efuse MAC and the boot screen reads `UNIT AB12`, where
`AB12` is `mac[4]` and `mac[5]`. It has to be that end of the address: the
first three bytes are the Espressif OUI and are identical across the whole
batch. Owner names go on afterwards, over the wire, into NVS.

`firmware/tools/flash-all.sh` does the batch: it waits for a port whose
`pio device list --json-output` hwid contains `303A:1001`, builds and uploads,
resets the board itself by pulsing DTR and RTS, reads the greeting back, and
appends a row to a fleet log.

---

## The protocol

Newline-delimited JSON over native USB CDC at 115200 baud. There is no serial
library on the host: a USB CDC port on macOS is an ordinary tty, so the helper
opens it with `fs.openSync` and configures the line with
`/bin/stty -f <port> 115200 raw -echo`. Nothing native is compiled.

**Host to device**, one JSON object per line. Any field may be omitted and the
device keeps its last value, so a delta line is legal and `{}` is a keepalive.

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3,"name":"Alex Rivera"}
```

There are 19 fields (`grep -c 'doc\["' firmware/src/main.cpp` returns 19).
These eight are the only ones the shipped helper's hook path ever emits:

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `sleep`, `idle`, `busy`, `waiting`, `done`. Any other value makes the **whole line** malformed, dropped silently with no `ok` and no partial application |
| `ring` | number | ring fill, a fraction, clamped to 0..1 |
| `center` | string | big text in the ring's hole, 15 chars |
| `label` | string | small text above the sub line, 23 chars |
| `sub` | string | bottom line, 39 chars |
| `tps` | number | tokens per second, 0..10000. Shortens the `waiting` pulse |
| `time` | number | the host's **local** wall clock in seconds, unix time plus the host's own UTC offset. The only question the device asks of it is whether midnight has happened where you are sitting, which it cannot answer from UTC with no timezone database |
| `tokens` | number | the session's cumulative token total. The device banks the differences, never the value, so a lower value reads as a new session |

The other eleven are the ones a person drives by hand: `name` and `face`
(persisted to NVS), `say` and `saysecs` (the message card), `dnd` (the sign),
`play` (the toy), `stats`, `focus` and `focusmins` (the timer), `reset` and
`firstrun`.

**Device to host**, plain text:

- `hello tdisplay-s3 v1 name="<stored name>"` once, after the 2 second boot
  screen. The prefix through `v1` is fixed and two other programs match on it.
- `ok` per accepted line.
- Malformed lines are ignored silently, with no reply.
- Unsolicited notices prefixed `say:`, `dnd:`, `play:`, `stats:`, `focus:`,
  `milestone:`, `firstrun:`, `reset:` and `err:`, so the parts of the device
  nobody can see over a cable are still provable over one.

Lines longer than 512 bytes are discarded whole rather than truncated, so a
partial write can never be parsed as a valid frame.

**One quirk worth knowing before you write your own host.** The device's USB
CDC transmit path runs exactly one message behind: the `ok` for line N does not
reach you until you write line N+1. It is in `HWCDC`, not in this firmware
(verified on both the current and the pre-ambient builds on the same board).
Nothing is lost over a stream, the ack count always reconciles. Treat acks as a
running count rather than a per-line handshake, or keep a keepalive flushing
them. `firmware/tools/flash-all.sh` nudges with a bare `{}` about once a second
for exactly this reason.

Full spec, including keep-last-value semantics field by field, the staleness
ladder and worked examples: [`docs/protocol.md`](docs/protocol.md).

### Drive it by hand

Nothing needs to be installed for any of this. A USB CDC port is an ordinary
tty, so a shell redirect is a complete host.

```bash
printf '{"state":"waiting","ring":0.62,"center":"62%%","label":"CTX","sub":"your turn"}\n' > /dev/cu.usbmodemXXXX
printf '{"say":"deploying to prod, do not unplug"}\n' > /dev/cu.usbmodemXXXX
printf '{"dnd":true}\n'                               > /dev/cu.usbmodemXXXX
printf '{"dnd":false}\n'                              > /dev/cu.usbmodemXXXX
printf '{"say":""}\n'                                 > /dev/cu.usbmodemXXXX
printf '{"name":"Alex Rivera"}\n'                     > /dev/cu.usbmodemXXXX
printf '{"face":"bear"}\n'                            > /dev/cu.usbmodemXXXX
printf '{"name":""}\n'                                > /dev/cu.usbmodemXXXX
```

Reading the other direction needs a reader that keeps the port open (the shell
closes it after each redirect, and remember the transmit path runs one message
behind, so keep a `{}` flowing). Here is a real transcript of the first five of
those lines going to a board on `/dev/cu.usbmodem1101`, with the replies it
sent back:

```
ok
say: showing "deploying to | prod, do not | unplug" 30s lines=3 font=18
ok
dnd: on (host) font=24 w=186,213 max=268
ok
dnd: off (host)
ok
say: cleared
ok
```

That is the device telling you what it actually did with your message. The
`say:` notice reports the wrap it solved, how long it will hold, how many lines
it used and which font rung won. The `dnd:` notice reports the measured pixel
widths of both words against the space available inside the frame, which is the
only proof over a cable that the words fit. Nobody can read a panel over USB,
so the firmware says out loud what it put on one.

`name` and `face` are the two fields whose effect outlives the frame, and the
greeting is how you prove it. Same board, reading the greeting before and after
setting a name, with a hardware reset (DTR and RTS pulsed) in between:

```
hello tdisplay-s3 v1 name=""
                                     <- {"name":"Alex Rivera"} then reset
hello tdisplay-s3 v1 name="Alex Rivera"
                                     <- {"name":""} then reset
hello tdisplay-s3 v1 name=""
```

The name survived a power cycle and then cleared cleanly, which is exactly what
it has to do for a fleet whose recipients are not known at flash time. The
greeting is always quoted, so an unnamed board is an unambiguous `name=""`
rather than a missing field. Writes are idempotent: the firmware compares
against its RAM mirror and only touches flash when the value actually changed,
so a host that repeats `name` in every heartbeat costs nothing in wear.

One command deliberately not run here, because it is destructive:
`{"reset":"factory"}` wipes the whole NVS namespace (your name, your face, your
hop best, your timer length) and re-arms the out-of-the-box sequence. The same
thing on the buttons is holding both for five seconds.
`{"reset":"firstrun"}` re-arms the sequence only and keeps everything else.

---

## The face system

The face is the whole product, it has been redrawn several times, and it will
be redrawn again. So it is not part of the renderer. It lives behind a small
interface, three designs implement it, and which one a board shows is runtime
state stored next to the owner name. Swapping costs a protocol line or an
800ms hold on button 2. Writing a new one costs one file and three lines in a
registry. Neither costs a reflash.

```cpp
class Face {
 public:
  virtual const char* name() const = 0;         // wire name, lowercase, <= 15
  virtual const char* description() const = 0;  // one line, for humans
  virtual void init() {}                        // once, from setup()
  virtual void draw(FaceCanvas& c, const FaceFrame& f) = 0;
};
```

`draw` is called once per rendered frame, for the active face only, after the
ring and before the text. That is the entire contract. A face has no update
call, no state that has to survive a swap, and no way to reach anything in
`main.cpp`. Swapping mid-animation is a pointer change.

It is handed a `FaceFrame`: the time, the effective state, whether the device
is in ambient mode, whether a wait has escalated or been acknowledged, `open`
(the lid channel, 0 shut to above 1 wide), `pulse` (the ring's own beat this
frame), the gaze offsets, the accent colour, and a `FaceGeometry` that
describes the space twice, once as an eye pair and once as a plain box, so a
face uses whichever half suits it.

Two of those deserve a note. **A face never computes its own blink**, because
fourteen units blinking in lockstep, or in three different rhythms depending on
which face is loaded, is the bug the interface exists to prevent: `open` is the
shared driver's factor multiplied by the state's own lid expression. And
**anything animated on the beat has to ride `pulse`**, because the `waiting`
period moves with `tps` and its phase is accumulated outside, so a hop that
recomputed the beat itself would land on a different frame from the ring.

### Writing your own

1. Copy `firmware/src/FaceRounded.cpp`, the shortest of the three.
2. Rename the class, change `name()` and `description()`.
3. Delete the drawing and write yours. Read `open`, `pulse`, `gazeX/Y`,
   `color` and `geom`; ignore anything you do not want. A face that ignores
   `state` entirely is legal and will look the same in all five.
4. Export it: `Face* faceMine() { return &gMine; }` at the bottom, outside the
   anonymous namespace.
5. Register it in `firmware/src/Faces.cpp`: declare it, widen the `gFaces`
   array, add one line to `ensure()`. The array's order is the button-cycle
   order.
6. `pio run`, flash, hold button 2 until it comes round.

Three rules, and they are not negotiable:

1. **Never add the burn-in drift.** It is already baked into `cx`, `cy`,
   `boxX` and `boxY`. That is what lets the resting composition wander by 9px
   while the live layout lands on pixel-exact geometry.
2. **Run every colour through `c.tint()`, including your own palette.** That
   is the mode cross-fade. It is the identity function once a fade has settled,
   and a face that skips it will not fade with the rest of the screen.
3. **Stay inside the box.** Clamp anything that moves, rather than trusting the
   arithmetic to come out right at both scales.

Two things that will bite, both learned the hard way here. `powf(sinf(M_PI *
u), k)` is NaN at `u = 1`, because `sinf(M_PI)` is `-8.7e-8` and a negative
base with a fractional exponent is NaN; a NaN half width passes every `<
minimum` test and hands LovyanGFX a radius it never returns from, so the board
greets and then goes permanently silent. And anti-aliased primitives are not
free: the `arc` face drawn as 88 `drawWideLine` wedges cost 24ms against a 33ms
budget, where the same stroke stamped as filled discs costs 3ms. Measure with
`PLATFORMIO_BUILD_FLAGS="-DFPS_DEBUG" pio run -t upload` before assuming.

### Every scene, measured on the real board

Worst frame in a 3-second window, milliseconds, against a 33ms `FRAME_MS`
budget:

| face | sleep | idle | busy | waiting | escalated | done | ambient |
|---|---|---|---|---|---|---|---|
| `rounded` | 14.52 | 15.40 | 15.43 | 17.04 | **17.26** | 15.39 | 14.96 |
| `bear` | 16.48 | 16.38 | 16.77 | 16.53 | 16.53 | 16.52 | 15.39 |
| `arc` | 17.68 | 17.06 | 17.64 | 17.96 | **17.97** | 17.47 | 17.00 |

All twenty-one hold a locked 30.3fps, which is the frame cap and not a limit of
the renderer. The worst frame anywhere is 54% of budget. The dominant cost in
every scene is the fixed ~13ms of `fillScreen` plus `pushSprite` on a 320x170
16-bit sprite, which is why there was room for three faces.

Full detail: [`firmware/FACES.md`](firmware/FACES.md) for the interface,
[`firmware/EYES.md`](firmware/EYES.md) for the default face's geometry and
blink model.

---

## The buttons, in full

Two switches, four thresholds, and every gesture is deliberately clear of every
other so a slow finger cannot reach the wrong one.

| | button 1 (GPIO 0) | button 2 (GPIO 14) | both |
|---|---|---|---|
| tap | acknowledge a prompt, else the stats screen | brightness | do not disturb |
| hold | 2s: the toy | 800ms: next face | 5s: factory reset |

One rule covers every modal screen: **button 2 leaves; button 1 is that
screen's own action, and where a screen has no action, it leaves too.**

| Screen | Button 1 | Button 2 |
|---|---|---|
| the message card | dismiss | dismiss |
| the sign | take it down | take it down |
| the toy | jump | leave |
| the stats screen | on to the timer | leave |
| the timer | start / pause / resume, hold to reset | leave, hold for the length |
| the timer's finish | close it | close it |

And one rule covers what covers what. Nothing on this device ever hides a
question:

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| a pending prompt (`waiting`) | **the prompt** |
| a message card | the card |
| the sign | the sign |
| the toy | the toy |
| the stats screen | the stats screen |
| anything else | the timer |

Yielding never cancels anything. A timer keeps counting under a prompt and its
screen returns the instant the prompt is answered. A message card's clock keeps
running while it yields, so a message can never queue up behind a long prompt
and surprise somebody ten minutes later.

---

## Repo layout

```
codex-buddy/
├── README.md              this file
├── CONTRIBUTING.md        how to build, test and change this repo
├── firmware/              PlatformIO project, env `tdisplays3`
│   ├── platformio.ini     three pinned dependencies, one env, no per-unit flag
│   ├── src/main.cpp       protocol parser, state machine, every composition
│   ├── src/Face.hpp       the face interface and geometry contract
│   ├── src/Faces.cpp      the registry, the cycle order, the default
│   ├── src/FaceRounded.cpp  src/FaceBear.cpp  src/FaceArc.cpp
│   ├── src/LGFX_TDisplayS3.hpp   LovyanGFX device config
│   ├── tools/flash-all.sh   batch flasher, one image, greeting verified and first run re-armed per unit
│   ├── tools/sim.py         demo protocol stream (needs pyserial; the helper does not)
│   └── README.md   FACES.md   EYES.md   AMBIENT.md   FIRSTRUN.md
├── helper/
│   ├── codex-companion.js the entire host program
│   ├── package.json       no dependencies key
│   ├── test/              eleven files, node:test, plus real captured fixtures
│   └── README.md
├── docs/
│   ├── protocol.md        the formal wire spec, corrected against both ends
│   ├── hardware-recon.md  pin map and panel geometry, cited line by line
│   ├── codex-state-format.md  what Codex writes to ~/.codex, field by field
│   ├── design-spec.md     the palette and feel this borrows from
│   ├── NEEDS-EYES.md      every visual claim no tool here can settle
│   ├── open-questions.md  every unconfirmed item, with how to confirm it
│   ├── architecture.md    why it is shaped like this, and what got reversed
│   ├── licences.md        every borrowed line and what it is licensed under
│   ├── packaging.md       what goes in the box
│   └── host-tooling-recon.md, wednesday-runbook.md, card.md, make-qr.sh,
│       STATUS.md, prior-art-hardware.md, prior-art-app-server.md,
│       prior-art-status-light.md
├── site/index.html        the one-page site the card points at
└── vendor/                git-ignored: reference clones, never redistributed
```

---

## Licences

**This repository.** `helper/package.json` declares `"license": "MIT"` for the
host program. There is no repository-root `LICENSE` file yet, so the firmware
has no explicit licence declaration. That is a real gap and it is named here
rather than papered over.

**The vendored reference material is not vendored.** `vendor/` is in
`.gitignore` and nothing under it is tracked:

```console
$ git ls-files vendor | wc -l
       0
```

Those directories are shallow clones used for research while this was written.
None of their code is redistributed here, and none of it is compiled into the
firmware or the helper. They are cited by path throughout the docs so a claim
can be followed to its source, and you can re-create them yourself:

| Path | Upstream | Licence | Used for |
|---|---|---|---|
| `vendor/codex` | github.com/openai/codex | Apache 2.0 (plus a `NOTICE`) | reading the hook engine, event schemas and the approval-control rules the helper depends on |
| `vendor/T-Display-S3` | github.com/Xinyuan-LilyGO/T-Display-S3 | MIT | pin map, panel geometry, button wiring |
| `vendor/LovyanGFX` | github.com/lovyan03/LovyanGFX | MIT AND BSD-2-Clause (FreeBSD text) | the graphics library's own config surface |
| `vendor/claude-desktop-buddy` | github.com/anthropics/claude-desktop-buddy | MIT | prior art on a similar object, written up in `docs/prior-art-*.md` |

LovyanGFX and ArduinoJson reach a build through PlatformIO's registry at build
time, pinned by version in `firmware/platformio.ini`. No copy of either lives
in this repository.

---

## What is verified, and what is not

The honest part, because a document that claims everything is proven is a
document you cannot use.

**Verified over the wire, on real hardware, while writing this document.** A
board on `/dev/cu.usbmodem1101` was rebuilt from source, flashed (`Hash of data
verified`), reset, and greeted. It accepted and acked hand-written protocol
lines for `state`, `say`, `dnd`, `name` and `face`; reported back its own
solved layouts for the message card and the sign; stored an owner name, kept it
across a hardware reset, and cleared it again. Every transcript quoted above is
that board. Separately, all eight hook event frames have been accepted and
acknowledged by real hardware, and the board also reports the toy's score and
every figure on the stats screen the same way. Every frame-cost table in this
document and in `firmware/` was measured on that board with an `-DFPS_DEBUG`
build, never in a fleet image, because the protocol channel has to stay clean.

**Verified in software.** The test suite described above, plus the `hooks.json` install
and uninstall round trip run against a scratch `CODEX_HOME` holding a foreign
hook, shown above.

**Not verified, and it is the important gap: nobody has judged the screen.**
Every visual claim here is arithmetic that has been rendered but not signed off
by an eye. Whether the face is cute, whether the blink reads as organic rather
than as a shutter, whether `waiting` reads as "hey, you" from across a room
without being maddening to sit next to, whether the ambient teal sits well
between the two blues, whether a 24-character name still clears the panel edge
at the extremes of the drift. Those are all in
[`docs/NEEDS-EYES.md`](docs/NEEDS-EYES.md), which is a gate, not a wishlist.

**Not verified end to end: a real Codex `PermissionRequest` reaching a board.**
Every link in that chain has been tested individually and the whole chain has
been driven by the test harness, with a realistic payload on stdin for each of
the eight events and the resulting bytes asserted, but never by Codex itself.
That needs an authenticated session running under an approval policy that
actually asks, with the hook installed and approved at Codex's "Hooks need
review" prompt, and a prompt left sitting. It is item N6 in
`docs/open-questions.md` and it is the single most valuable thing left to do.

Everything else that is unconfirmed, with the exact command or observation that
would settle it, is in [`docs/open-questions.md`](docs/open-questions.md)
rather than asserted anywhere as fact.
