# Codex Desk Companion

A LILYGO T-Display-S3 (ESP32-S3, 16MB flash, 8MB PSRAM, ST7789 170x320
panel, native USB CDC, cased, non-touch) that sits on a desk and shows a
live OpenAI Codex CLI session as a color ring, center readout, and label,
reacting to agent state. A small Node host helper watches `~/.codex/` on
the same Mac and streams state updates to the device over USB serial.
Zero WiFi, zero accounts, one USB cable.

14 units, built as a gift, deadline Wednesday 2026-09-09.

This repo's own directory is named `codex-buddy` for historical reasons;
the product name is "Codex Desk Companion" and the actual, shipped npm
package name is `codex-companion` (`helper/package.json`). Expect to see
all three names in different places in this repo.

---

## Architecture

```
   Codex CLI (openai/codex, codex-cli)
         |
         |  writes session transcript, one JSON line per event,
         |  flushed on every write
         v
   ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl
         |
         |  tailed continuously (disk-only, confirmed shipped design,
         |  see helper/src/codex-watcher.js and docs/open-questions.md L4:
         |  the app-server control socket exists but isn't used yet)
         v
   +-----------------------------+
   |  Node host helper (helper/) |   npm package: codex-companion
   |  - derives state:            |   runs as a macOS launchd LaunchAgent
   |    sleep / idle / busy /     |   (Linux: systemd --user unit; Windows:
   |    waiting / done            |    manual instructions, no autostart)
   |  - "waiting" is a heuristic: |   installs to ~/.codex-companion/app,
   |    turn silent 45s+ and      |   a stable path, not an npx cache dir
   |    approval_policy != never  |
   |  - computes ring %, tps,     |
   |    elapsed (helper/src/      |
   |    frame.js)                 |
   +--------------+---------------+
                  |
                  |  USB CDC serial, 115200 baud
                  |  one JSON object per line, newline-delimited
                  |  (full spec: docs/protocol.md)
                  v
   +-----------------------------+
   |  T-Display-S3 firmware       |   firmware/ (PlatformIO, env tdisplays3)
   |  (firmware/src/main.cpp)     |   LovyanGFX full-screen PSRAM sprite
   |  - parses each line,         |   -> pushed to ST7789 panel each frame
   |    keeps last value per      |
   |    field on omission         |
   |  - renders ring + center +   |
   |    label + sub               |
   |  - autonomous no-data        |
   |    timeouts: 30s dim (70/255)|
   |    5min sleep (20/255)       |
   +--------------+---------------+
                  |
                  v
        170x320 ST7789 panel, 8-bit parallel
        (color ring, center text, label, sub-line)
```

---

## Repo layout

```
codex-buddy/
├── README.md                  this file
├── docs/
│   ├── hardware-recon.md      pin map, panel geometry, platformio.ini, USB VID/PID (static repo recon, no board attached)
│   ├── host-tooling-recon.md  npm/serialport/launchd recon, verified with real commands on this machine
│   ├── codex-state-format.md  what Codex actually writes to ~/.codex/, field-by-field, with source citations
│   ├── design-spec.md         ring UI animation spec (colors, timing, states); firmware borrows its palette/feel, not its exact 7-state model
│   ├── protocol.md            formal wire protocol spec, corrected against the real firmware/helper source
│   ├── card.md                printed QR card copy (front/back)
│   ├── make-qr.sh             renders the card's QR PNG to docs/out/ (needs qrencode, not installed on this machine as of writing)
│   ├── wednesday-runbook.md   minute-by-minute flash + package plan for 14 units, quoting the real flash-all.sh/sim.py tools
│   ├── open-questions.md      every UNCONFIRMED item, consolidated, with exact confirm steps and resolution status
│   └── out/                   generated (make-qr.sh output; not checked in)
├── firmware/                  PlatformIO project, env `tdisplays3`
│   ├── platformio.ini
│   ├── src/main.cpp           protocol parser, state machine, ring renderer
│   ├── src/LGFX_TDisplayS3.hpp LovyanGFX device config
│   ├── tools/flash-all.sh     flashes units, stamping UNIT_ID per board
│   └── tools/sim.py           streams a demo protocol sequence over serial
├── helper/
│   ├── package.json           npm name: codex-companion
│   ├── index.js               package entry point (`main` in package.json); what the
│   │                          LaunchAgent/systemd unit actually executes: `node <install>/index.js run`
│   ├── bin/codex-companion.js CLI entry point for `npx` and PATH installs (the `bin` field);
│   │                          launchd and systemd go through index.js instead
│   ├── src/codex-watcher.js   tails ~/.codex/, derives state (disk-only + heuristic)
│   ├── src/frame.js           state snapshot -> protocol JSON line
│   ├── src/device.js          serial port matching, change-detected + heartbeat send
│   ├── src/install.js         CLI subcommands: run/install/uninstall/status/doctor/demo
│   ├── templates/com.codex-companion.plist   launchd LaunchAgent template
│   └── test/fixtures/         real + synthetic ~/.codex rollout JSONL samples
└── vendor/                    gitignored: shallow clones used for recon only (T-Display-S3 SDK, LovyanGFX, codex source, claude-desktop-buddy reference)
```

---

## Quickstart: for the maker (building and flashing 14 units)

Full step-by-step for flash day is `docs/wednesday-runbook.md`. This is
the short version, commands quoted directly from `firmware/README.md`:

1. **Build the firmware.**
   ```bash
   cd firmware
   pio run
   ```
   Pin map, panel geometry, and the ring UI's colors/timing are in
   `docs/hardware-recon.md` and `docs/design-spec.md`; the shipped
   firmware's own README (`firmware/README.md`) is the closer reference
   once you're actually building.
2. **Flash all 14 units:**
   ```bash
   cd firmware
   ./tools/flash-all.sh          # units 1..14
   ./tools/flash-all.sh 5 8      # units 5..8 only
   ```
   It prompts for a board swap between each unit and bakes a unique
   `UNIT_ID` into each one, visible on its own boot screen. Full per-unit
   acceptance checklist: `docs/wednesday-runbook.md`.
3. **Publish the helper to npm** as `codex-companion` (confirmed real name
   in `helper/package.json`, matching `docs/host-tooling-recon.md`
   section 1's recommendation). Depends on `serialport@^13.0.0` (ships
   prebuilt native bindings, no compiler needed at install time,
   `docs/host-tooling-recon.md` section 2). `npm view codex-companion`
   first to check whether it's actually been published yet, see
   `docs/open-questions.md` item D1, if not, `node
   helper/bin/codex-companion.js install` works identically from a local
   checkout for testing.
4. **Print cards.** Copy in `docs/card.md`; render the QR with
   `docs/make-qr.sh` (needs `brew install qrencode`, not installed on this
   machine as of writing).

---

## Quickstart: for the recipient

1. Plug the unit into a USB port on your Mac.
2. Open a terminal, `cd` to wherever the `codex-companion-1.0.0.tgz`
   tarball that shipped with the unit landed, and run:
   ```bash
   npm install -g ./codex-companion-1.0.0.tgz
   codex-companion install
   ```
   This package is **not** on the public npm registry, so `npx
   codex-companion` will not find it: `npm view codex-companion` returned
   a registry 404 on 2026-09-07. The tarball install is the offline path
   `helper/README.md` documents, and it needs no network at all. Build a
   fresh tarball at any time with `npm pack` from `helper/` (the version
   in the filename tracks `helper/package.json`'s `"version"`, `1.0.0`
   today). If someone actually runs `npm publish` before the units go
   out, and only then, `npx codex-companion install` becomes a valid
   one-liner: re-check with `npm view codex-companion` before relying on
   it, and see `docs/open-questions.md` item D1.
3. That's it. It survives reboots (installed as a launchd LaunchAgent to a
   stable `~/.codex-companion/app` directory, not a foreground process you
   have to remember to start). Your next Codex CLI session shows up on the
   ring automatically.
4. To remove it:
   ```bash
   codex-companion uninstall
   ```

No account, no WiFi setup, no config file to edit. (On Linux, `install`
sets up a `systemd --user` unit instead of launchd; on Windows it prints
manual instructions rather than configuring autostart, confirmed in
`helper/src/install.js`, but neither has been tested on a real machine as
part of this doc pass, see `docs/open-questions.md` item D12.)

---

## Protocol reference (summary)

Full formal spec, including exact keep-last-value semantics, timeouts,
and worked examples: **`docs/protocol.md`**, corrected against the real
`firmware/src/main.cpp` and `helper/src/*.js` source.

**Host to device** (one JSON object per line, 115200 baud, any field may
be omitted and the device keeps its last value; a line over 512 bytes is
discarded whole):

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
```

| Field | Type | Notes |
|---|---|---|
| `state` | string enum | `sleep`, `idle`, `busy`, `waiting`, `done`. An unrecognized value makes the **whole line malformed**: `applyJsonLine` returns false before any other field is touched, so the line is dropped silently, gets no `ok`, and no partial update is applied. |
| `ring` | float, 0.0 to 1.0 | fraction filled, not a percentage; clamped, not rejected, if out of range |
| `center` | string | clamped to 15 chars; renders large at <= 4 chars, smaller above that |
| `label` | string | clamped to 23 chars |
| `sub` | string | clamped to 39 chars, truncated not wrapped |
| `tps` | float, 0 to 10000 | tokens/sec, a derived average, not instantaneous; clamped |

**Device to host** (plain text, not JSON):

- `hello tdisplay-s3 v1` once, after the 2000ms boot screen.
- `ok` once per accepted line (including a bare `{}` keepalive).
- Malformed lines (invalid JSON, non-object JSON, or over 512 bytes) are
  ignored silently, no response at all.

**Timeouts** (device-side, autonomous, armed from boot): no data for
30s, backlight dims to 70/255; no data for 5 minutes, backlight drops to
20/255 and rendering forces to the `sleep` look (a render-time override,
the underlying stored state is untouched). The clock these measure
against is seeded in `setup()` (`lastDataMs = millis();`), so a unit that
has never received a single line follows exactly the same schedule as one
whose host went away: dim at 30s, sleep look at 5 minutes. See
`docs/protocol.md` section 2.4.

---

## State table

| State | Meaning | Trigger (host side) | Ring behavior |
|---|---|---|---|
| `sleep` | No active session, or no data for 5+ minutes | No running Codex session, or the device's own 5-minute no-data timeout | Slow, dim, full-ring breathing (4000ms) |
| `idle` | Connected, nothing running | A session exists but is between turns (also the device's own boot default before any data arrives) | Ring shows the last reported fill with a low-amplitude 2400ms brightness breathe (`IDLE_PERIOD_MS`), so a live unit never looks frozen |
| `busy` | Agent actively working | A turn is in progress (`task_started` seen, no matching `task_complete`/`turn_aborted` yet) | Slow breathing ring (2400ms), amber |
| `waiting` | Agent is likely blocked on a human approval | **Confirmed shipped heuristic**, not a guaranteed signal: an open turn silent for 45+ seconds while `approval_policy` is known and not `"never"` (`helper/src/codex-watcher.js`, togglable with `--no-heuristic`). Approval events themselves are essentially never persisted to disk by Codex, so this is inference, not a direct read of "an approval is pending." | Amber pulse, period shortens as `tps` rises, escalates to red and halves speed at a fixed 10s threshold, no continuous ramp |
| `done` | A turn just completed | `task_complete` with no error | Exactly one 600ms green flash per transition into `done`: ramps up over 120ms (`DONE_RISE_MS`), decays, then cross-fades into the `idle` look over the last 150ms (`DONE_XFADE_MS`). The flash is armed on the state edge only, so the repeated `done` lines the helper's 2-second heartbeat sends during its ~5-second done hold are keepalives and replay nothing |

Elapsed and tokens/sec are both derived metrics the helper computes
(`helper/src/frame.js`), not values Codex reports directly.

---

## Troubleshooting

**Device shows nothing / stays on the boot screen.** Confirm the helper
process is actually running:
```bash
launchctl print gui/$(id -u)/com.codex-companion   # macOS
node helper/bin/codex-companion.js status          # cross-platform status/doctor check
```
and that it found the device's serial port (`vendorId === '303a'` with a
manufacturer/`pnpId` fallback, `helper/src/device.js`); if a unit
enumerates differently than expected, that's worth checking directly
(`docs/open-questions.md` items H4, H6).

**A bench of freshly flashed units, with no helper running, all go dim
and then dark.** Expected, and the single most likely thing to be
misread as "the boards are dead." With no host attached a unit shows the
full-brightness `CODEX` idle look for 30 seconds, dims to 70/255, and
drops to the 20/255 sleep look at 5 minutes, because the staleness clock
is seeded at boot rather than at the first received line
(`docs/protocol.md` section 2.4). Press Button 1 (GPIO 0) to force one
back to full brightness. If you want to see the bright idle look, check
within 30 seconds of plugging in.

**Device was working, then went dim.** Same ladder, same 30-second mark
(`docs/protocol.md` section 2.5), not a fault by itself. Check whether
the helper is still running and whether a Codex session is actually
active.

**Device went fully dark/dim-sleep.** Expected at 5+ minutes of no data.
If the helper is running and a session is active but this still happens,
that's a real bug worth filing.

**Setup command silently does nothing, or 404s.** Two separate causes:
plug the unit in before running the command (the installer needs to find
it over USB); or the `codex-companion` npm package hasn't actually been
published yet (`npm view codex-companion` to check,
`docs/open-questions.md` item D1), in which case `node
helper/bin/codex-companion.js install` from a local checkout works the
same way without needing the registry.

**`npm warn install-scripts ... node-gyp-build` during setup.** Expected
and harmless (`docs/host-tooling-recon.md` section 2): `serialport`'s
native binding resolves lazily at `require()` time even when the
install-time script is blocked. If it actually fails to load, `npm
install --foreground-scripts` or `npm rebuild @serialport/bindings-cpp`
(needs a C++ toolchain) is the fallback.

**LaunchAgent doesn't survive a reboot.** Check it's installed at the
stable `~/.codex-companion/app` path (confirmed default,
`helper/src/install.js`), not something pointed at an `npx` on-demand
cache directory. `launchctl print gui/$(id -u)/com.codex-companion` for
status, `launchctl bootstrap`/`bootout` (not the deprecated
`load`/`unload`) to reinstall/restart.

**On Linux or Windows, "survives reboots" doesn't seem to hold.** Linux
uses a `systemd --user` unit (should behave the same way as launchd, but
untested on a real Linux box as part of this doc pass); Windows gets
printed manual instructions only, autostart is not configured
automatically there. See `docs/open-questions.md` item D12.

**Something about the protocol itself looks wrong or undefined.** Check
`docs/open-questions.md` first, most protocol edge cases have already
been resolved by reading the real source and are cited there with a file
and line number; the few genuinely open ones (D10, D12) are marked
as such rather than guessed at.

---

## Sources

Every claim above is backed by either a real file in this repo
(`firmware/src/main.cpp`, `helper/src/*.js`, `helper/package.json`, all
read directly for this doc pass) or one of the four recon docs in `docs/`,
each of which cites its own file-and-line or command-and-output evidence:
`docs/hardware-recon.md`, `docs/host-tooling-recon.md`,
`docs/codex-state-format.md`, `docs/design-spec.md`. Anything not
independently confirmed is listed in `docs/open-questions.md` rather than
asserted here as fact.
