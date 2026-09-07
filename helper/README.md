# codex-companion

A small desk object that shows what your Codex CLI session is doing.

A LILYGO T-Display-S3 sits on your desk. This package runs quietly on your Mac,
watches `~/.codex/`, and streams the current state of your Codex session to the
board over its USB cable. A colored ring, a number in the middle, a label.

No WiFi. No account. No network of any kind. The board only ever hears what
comes down the USB cable, and this helper only ever reads files Codex already
writes to your own disk. Nothing is uploaded anywhere.

## Install

Requires Node 20 or newer (`node --version` to check).

Plug the board into any USB port. The tarball that came with it is
self-contained: `serialport` and its prebuilt binding are bundled inside it, so
this installs with no network access at all.

```bash
npm install -g ./codex-companion-1.0.0.tgz
codex-companion install
```

That copies the package to `~/.codex-companion/app`, sets it to start
automatically when you log in, and starts the background service. You can close
the terminal; it keeps running. `codex-companion` on its own does the install
and then streams in the foreground too.

`codex-companion --help` and `codex-companion --version` print and exit; they
change nothing.

This package is not on the public npm registry, so nothing can fetch it by
name: install from the tarball or from a checkout, never by package name. Build
a fresh tarball with `npm pack` from this directory if you ever need one; it
bundles the dependency, so the result installs offline.

## What the display means

**The ring** is how full your context window is, from Codex's own numbers, using
Codex's own formula. Empty ring is a fresh session. Full ring means you are
about to run out of room and should think about `/compact` or a new session.

**The color** is what the agent is doing:

| Color | State | Meaning |
|---|---|---|
| dim | `sleep` | nothing has happened for five minutes |
| soft | `idle` | a session exists, no turn running |
| slow breathing | `busy` | the agent is working |
| **amber pulse** | `waiting` | it is waiting on **you** |
| green flash | `done` | the turn just finished |

The amber pulse is the one to care about. It gets faster the more work the
agent had been doing, so a session that stops dead on an approval prompt gets
visibly impatient at you from across the desk.

Codex does not write approval prompts to disk, so most of the time `waiting` is
inferred from a turn that has gone quiet. When it is inferred rather than
observed, the bottom line ends in `(guess)`. An amber pulse with no `(guess)`
means the approval request itself was seen.

**The label** tells you what the number in the middle is:

- `CTX` - percent of the context window used. This is the normal mode.
- `TIME` - elapsed time on the current turn. Shown when Codex has not reported
  any token accounting yet, which is normal for the first few seconds.
- `QUOTA` - percent of your rate-limit window used. Takes the ring over from
  `CTX` when a fresh rate-limit reading is at 80% or more, which is the point
  where the quota, not the context window, is the number you need. It also
  fills in when Codex has reported no token accounting at all. A reading older
  than 15 minutes is never shown, and neither is a window whose reset time has
  already passed, so the ring can never sit at a meaningless permanent 100%.

`tps` is tokens per second, derived from the change in output tokens over wall
time. It is an average across the interval, including time spent running
commands, not an instantaneous rate.

## Commands

```
codex-companion              install, then run in the foreground
codex-companion run          stream live state to the device
codex-companion install      install and enable autostart
codex-companion uninstall    stop autostart and remove everything
codex-companion status       is it installed, is it running
codex-companion demo         cycle through every state, no Codex needed
codex-companion doctor       ports, device match, session file, derived state
```

Useful flags:

```
--port /dev/tty.usbmodemXXXX   skip auto-detection and use this port
--dry-run                      print frames to the terminal, open no port
--loop                         (demo) repeat forever
--codex-home /path             watch a different $CODEX_HOME
--no-heuristic                 never infer "waiting" from a stalled turn
--global                       (install) npm install -g this package (the local
                               directory, never the registry: the name is not
                               published) instead of copying into
                               ~/.codex-companion/app. Needs a writable global
                               npm prefix; without one, npm fails with EACCES
                               and the plain install is the better option.
-h, --help                     print usage and exit, changing nothing
--version                      print the version and exit, changing nothing
```

## Uninstall

```bash
codex-companion uninstall
```

That stops the background service, removes the LaunchAgent, deletes
`~/.codex-companion` and deletes `~/Library/Logs/codex-companion.log`. If it was
installed with `--global` it also runs `npm uninstall -g codex-companion`.
Unplug the board and you are back to where you started. Nothing is left in
`~/.codex` because nothing was ever written there.

## Troubleshooting

**The display never changes / says nothing.**
Run `codex-companion doctor`. It prints every serial port it can see, which one
it picked, which session file it is reading, and the state it derived. That one
command answers almost every question.

**`device NOT FOUND`.**
The board did not turn up as a USB serial port. In order:

1. Check the cable. A charge-only USB-C cable is the single most common cause;
   it powers the board but carries no data, so no port ever appears.
2. Try the other USB port on the board. The T-Display-S3 has two.
3. Run `codex-companion doctor` and look at the port list. If you can see a
   likely port but the matcher did not pick it, pass it explicitly:
   `codex-companion run --port /dev/tty.usbmodemXXXX`.

Auto-detection looks for Espressif's USB vendor id `303a` first, then falls back
to matching Espressif/LILYGO/T-Display in the manufacturer string. If two boards
are plugged in it picks one and tells you, and `--port` settles it.

**`session (none found)`.**
Codex has not written a session file yet. That is fine: the service keeps
running and picks the file up the moment it appears. Run `codex` once, then
re-run doctor. If you set `CODEX_HOME`, pass the same value with `--codex-home`.

**It worked, then stopped after a `brew upgrade node` (or an `nvm` change).**
The LaunchAgent names a specific node binary. `install` prefers a stable alias
such as `/opt/homebrew/bin/node` and warns when it cannot find one. If it warned,
re-run `codex-companion install` after any node upgrade.

**It is stuck on `busy` and never shows amber.**
Expected, if your Codex session runs with `approval_policy = "never"` (which is
what `codex exec` uses by default). Such a session cannot ever be waiting for
your approval, so the companion never claims it is.

The reverse case is worth knowing about: Codex deliberately does **not** write
approval prompts to the session file, so with any other approval policy the
`waiting` state is inferred from a turn that has gone silent, not observed
directly. It is a good guess, not a fact. `--no-heuristic` turns it off.

**`npm warn install-scripts ... @serialport/bindings-cpp`.**
Harmless, and you only see it when installing from a source checkout: the
shipped tarball bundles the module already built. The prebuilt native binding is
resolved when the module loads, not by that install script, and it covers Intel
and Apple Silicon in one file. Nothing
is compiled and no Xcode toolchain is needed. If serial really does fail to
load, `npm rebuild @serialport/bindings-cpp` will build it from source (that
one does need Xcode Command Line Tools).

**Where are the logs?**
`~/Library/Logs/codex-companion.log`.

## Linux and Windows

Primary support is macOS. The rest is best-effort.

**Linux**: `install` writes a `systemd --user` unit and enables it. If opening
the port fails with a permission error, add yourself to the serial group and log
out and back in:

```bash
sudo usermod -a -G dialout "$USER"   # or the uucp group, on some distros
```

**Windows**: `install` prints the two steps to add a Startup shortcut. Windows
10 and 11 bind the built-in CDC driver on their own, so the board should appear
as a COM port with no driver install.

## How it works

The helper tails the newest `rollout-*.jsonl` under `$CODEX_HOME/sessions/`,
which Codex flushes one whole line at a time. Turn boundaries come from the
`task_started` / `task_complete` events, the context window size from
`model_context_window`, and token totals from `token_count` and
`token_usage_record`. Context fill uses Codex's own baseline-adjusted formula so
the percentage agrees with what the Codex TUI shows.

Each frame is one JSON object on one line at 115200 baud:

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
```

Any field may be omitted, and the board keeps the last value it saw. A frame
goes out when something changes, at most every 250 ms, and at least every 2
seconds as a heartbeat. If the board hears nothing for 30 seconds it dims the
last frame it had; after 5 minutes it sleeps. Unplug it and plug it back in and
the helper reconnects on its own.

## Development

```bash
npm install
npm test
```

Tests are `node:test` with no extra dependencies, and none of them open a real
serial port. The package name is recorded in `package.json`; `codex-companion`
was verified free on the npm registry before it was chosen.
