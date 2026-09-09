# codex-buddy firmware

Firmware for the Codex Desk Companion: a LILYGO T-Display-S3 (ESP32-S3, 16MB
flash, 8MB PSRAM, ST7789 170x320 8-bit parallel, native USB CDC) that shows a
live OpenAI Codex session as a pair of blinking eyes plus a colour ring and
centre text.

No WiFi, no accounts, no pairing. An optional Node host helper
(`../helper/codex-companion.js`, one file, no dependencies) streams
newline-delimited JSON over USB serial at 115200. **The device is complete
without it**: with nothing talking to it, it runs ambient mode forever.

## Layout

| Path | What |
|---|---|
| `platformio.ini` | PlatformIO project, env `tdisplays3` |
| `src/LGFX_TDisplayS3.hpp` | LovyanGFX device config (Bus_Parallel8 + Panel_ST7789 + Light_PWM) |
| `src/main.cpp` | protocol parser, state machine, blink and glance drivers, ring, text, every screen |
| `src/Face.hpp` | the face interface, the frame it is handed, the geometry contract |
| `src/Faces.cpp` | the face registry, the button-cycle order, the compile-time default |
| `src/FaceRounded.cpp` | the default face: soft rectangles with a wet highlight |
| `src/FaceBear.cpp` | a warm cream creature whose mood is in its mouth |
| `src/FaceArc.cpp` | two tapering hand-drawn strokes |
| `tools/sim.py`, `tools/flash-all.sh` | a demo stream, and the fleet flasher |
| `FACES.md` | the face interface, the geometry contract, the `face` field |
| `EYES.md` | the default face: geometry, blink model, per-state expressions |
| `AMBIENT.md` | ambient mode, the owner name, the buttons in detail |
| `FIRSTRUN.md` | the out-of-the-box sequence, its persistence, the factory reset |

Pins, panel offsets and invert flags come from `../docs/hardware-recon.md`,
which cites the vendor sources line by line. The palette comes from
`../docs/design-spec.md` part 2 verbatim, plus one ambient-only teal that part 2
has no state for; the animation timing deliberately diverges from it, and that
doc's own header says which parts of it were never built.

## Build

Pinned: platform `espressif32@7.1.1`, `lovyan03/LovyanGFX@1.2.28`,
`bblanchon/ArduinoJson@7.4.3`. Board id `lilygo-t-display-s3` ships with the
platform, so flash size, `default_16MB.csv` and `qio_opi` memory type come from
its own board JSON, and `platformio.ini` restates them so the file is
self-describing.

```sh
cd firmware
pio run
```

That is the whole build, for one board or for all fourteen. **There is no
per-unit build.** Every unit is flashed from the byte-identical image, so the
fleet has one binary and one hash. Nothing is stamped in at compile time:
neither the owner name nor the unit number.

Units still tell themselves apart. With `UNIT_ID` unset, `src/main.cpp` derives
the label from the chip's own efuse MAC and the boot screen reads `UNIT AB12`,
the last two bytes. It has to be that end of the address: the first three bytes
are the Espressif OUI and would print the same digits on all 14 boards. Owner
names are set afterwards over the wire and stored in NVS (`AMBIENT.md`).

Both build flags still exist and neither is on the batch path:

```sh
PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex Rivera\""' pio run
```

`-DUNIT_ID=<n>` renders `UNIT 03` instead of the MAC label. `-DUNIT_NAME` is a
build-time **default** owner name, outranked by anything stored in NVS. (The
quotes are part of the macro body and PlatformIO shlex-splits the variable,
which is why the inner pair is escaped; it appends to rather than replaces the
project's own flags, so `BOARD_HAS_PSRAM` and the USB CDC flags survive.)
Either flag gives that unit its own binary and its own hash, which is why
`tools/flash-all.sh` sets neither: it passes
`PLATFORMIO_BUILD_FLAGS="${UNIT_BUILD_FLAGS:-}"`, empty unless you export it.

`-DFPS_DEBUG` prints `fps: mode=... avg_ms=... max_ms=...` every three seconds
on the same USB CDC the protocol uses. **Never in a fleet image.**

## Flash

```sh
pio run -t upload                       # auto-detects the port
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
./tools/flash-all.sh                    # units 1..14
./tools/flash-all.sh 5 8                # units 5..8 only
```

Per unit the script waits up to 120s for a port whose `pio device list
--json-output` hwid contains `303A:1001` (not a `/dev/cu.usbmodem*` glob, which
would happily pick a USB power meter), builds and uploads, verifies the greeting
off the port, re-arms the first run, and appends a row to `tools/fleet-log.tsv`
(unit, USB serial, UTC timestamp, `ok` / `no-greeting` / `no-arm`).

**Arming is the last thing done to each board**, because anything that powers it
afterwards spends the flag again. It sends `{"reset":"firstrun"}` and accepts
only `reset: firstrun ok`, so a `ram-only` answer (NVS refused the write) lands
as `no-arm` and that board is not shippable. The greeting check resets the board
itself by pulsing DTR and RTS, then reads for 12 seconds while nudging with a
bare `{}` once a second, because the transmit path runs one message behind.

Three things worth knowing before flash day:

- It **aborts the whole run** if no board appears within 120s, so run it in
  sub-ranges (`1 5`, `6 10`, `11 14`) rather than letting a flaky cable at unit
  9 end the afternoon. `fleet-log.tsv` is append-only, so resuming loses nothing.
- `find_port` takes the **first** `303A:1001` match, so unplug every other
  ESP32-S3 first: they all enumerate under the same id and the same generic
  `USB JTAG/serial debug unit` description.
- If a board will not enter download mode: hold **BOOT** (button 1, GPIO 0), tap
  **RST**, release BOOT, then run the upload.

The full flash-day procedure is `../docs/wednesday-runbook.md`.

## Monitor and drive by hand

`pio device monitor -b 115200`, or `pio run -t upload -t monitor`. On boot the
device prints `hello tdisplay-s3 v1 name="Alex Rivera"`, then `ok` for every
line it accepts. Poke it directly:

```sh
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemXXXX
printf '{"state":"waiting","ring":0.62,"center":"62%%","label":"CTX"}\n' > /dev/cu.usbmodemXXXX
node ../helper/codex-companion.js demo --port /dev/cu.usbmodemXXXX --loop
```

`tools/sim.py` also exists, but needs `pyserial`, sends labels the real helper
never sends, and picks the first `/dev/cu.usbmodem*` given no `--port`. Prefer
the helper's `demo`.

## Protocol

Host to device, one JSON object per line, any field optional, the device keeps
its previous value for anything omitted, so a delta line is legal and `{}` is a
no-op keepalive. **The full field table and every device-to-host line are in
`../docs/protocol.md`**, which is the spec. In brief: `state` `ring` `center`
`label` `sub` `tps` drive the live view; `say` and `saysecs` are the message
card, `dnd` the sign, `play` the toy, `stats` the numbers, `focus` and
`focusmins` the timer; `name` and `face` persist to NVS; `reset` and `firstrun`
control the out-of-the-box sequence; `time` and `tokens` feed the daily figures.

Three rules that catch people out:

- A bad enum value for `state`, `face`, `reset`, `firstrun`, `play`, `stats` or
  `focus` makes the **whole line** malformed. It is dropped silently, with no
  `ok` and **no partial application**.
- Lines over 512 bytes are discarded whole, never truncated, so a partial write
  can never be parsed as a valid frame.
- **The USB CDC transmit path runs exactly one message behind.** The `ok` for
  line N does not reach the host until the host writes line N+1. Verified on a
  pre-ambient build of the same board too, so it is in `HWCDC`. Nothing is lost
  over a stream: treat acks as a running count, or write a `{}` to flush.

## The states

Live mode draws a face on the left and the ring on the right, with the label and
sub lines centred under both. Every state moves both.

| State | Ring | Face |
|---|---|---|
| `idle` | reported fill, low-amplitude 2400ms breathe so a live unit never looks frozen | open, blinking at the resting rate, glancing around |
| `busy` | slow 2400ms amber breathe on the reported fill | concentrating squint, gaze tracking, blinking less |
| `waiting` | amber pulse; period shortens as `tps` rises (900ms to 700ms), then flips red and halves at 10s. Button 1 acknowledges: steady amber, no pulse, no escalation | eyes wide, brows up, head hopping on every pulse peak, blinking twice as often. An ack keeps the wide eyes but stops the hop |
| `done` | green flash, 600ms, then cross-fades into idle. Armed only on the transition in, so a repeated `done` is a keepalive | a happy squint held 1600ms, hopping once on arrival |
| `sleep` | dim 4000ms breathe, full ring | shut, the lid line breathing on the same sine |

Those are the **live** view, shown only while a host is talking. With no host it
runs ambient mode instead (`AMBIENT.md`), and a brand new board plays its
out-of-the-box sequence first (`FIRSTRUN.md`). Expression detail is in
`EYES.md`.

**Staleness**, measured from the last accepted line:

- **30s** with no data: backlight drops to the dim tier, and `busy` and
  `waiting` fall back to the idle look, so a dead host cannot leave a board
  pulsing red for a prompt that no longer exists. The stored state is untouched;
  one fresh line restores it.
- **5 min** with no data: back to ambient mode.

**Backlight.** Ambient runs full for 60s then holds duty 70 and never reaches
the sleep tier. Live takes whichever is dimmer of staleness (full while fresh,
70 after 30s) and state (20 under `sleep`, 70 once `idle` has held 15s, full
otherwise).

## The buttons

Two switches, four thresholds, and every screen this device has hangs off them.

| | button 1 (GPIO 0) | button 2 (GPIO 14) | both |
|---|---|---|---|
| tap | acknowledge a prompt, else the stats screen | brightness | do not disturb |
| hold | 2s: the toy | 800ms: next face | 5s: factory reset |

One rule covers every modal screen: **button 2 leaves, and button 1 is that
screen's own action.** Where a screen has no action of its own (the message
card, the sign), button 1 leaves too. On the toy it jumps, on the stats screen
it goes on to the timer, on the timer it starts, pauses and resumes.

- The acknowledgement fires on the **press** edge, so it is instant, and marks
  the press handled so it can never also step the brightness or open something.
  Everything else on button 1 resolves on the **release**, because the press
  edge alone cannot tell a tap from a hold. Imperceptible for a tap.
- Brightness steps 255, 160, 70, 20, 0 and wraps. The last rung is a deliberate
  "not now" for a standalone owner, and the first press of **either** button
  from it restores full and does nothing else, so a board that looks dead is one
  press from being alive.
- **Every hold says how far along it is.** Past 400ms a thin bar fills along the
  top edge toward that gesture's threshold, and the factory chord keeps its own
  red bar and warning. Without it a hold was blind, and letting go early did not
  do nothing: the release ran the tap instead, so a 1.9 second reach for the toy
  arrived as the stats screen. The bar is drawn on exactly the conditions the
  gesture fires on, so it never promises something that will not happen.
- While button 1 is down, button 2's face cycle is suppressed, so travelling to
  the five second chord does not swap the face on the way. The other ordering is
  refused rather than delayed: see `FIRSTRUN.md` section 4.
- Two further holds exist only on the focus timer's own screen (button 1 resets
  the countdown, button 2 steps the length), both at the same 800ms, so the
  device still has exactly three thresholds.

**The model is full**, and the next feature must replace something in that
table, arrive on the wire only, or take a mode. If a seventh screen is ever
wanted, the coherent step is a chooser (hold button 1 for a list, tap button 2
to move down it, tap button 1 to pick), because it stops costing a gesture per
feature.

## The screens

Six modal screens, each with a protocol door as well as a gesture. Full
behaviour and the field tables are in `../docs/protocol.md`.

| Screen | Gesture | Wire | Section |
|---|---|---|---|
| the message card | none (host only) | `say`, `saysecs` | 2.9 |
| do not disturb | both buttons tapped | `dnd` | 2.10 |
| the toy | hold button 1 for 2s | `play` | 2.11 |
| stats and milestones | tap button 1 | `stats`, plus `time` and `tokens` | 2.12 |
| the focus timer | tap button 1 twice | `focus`, `focusmins` | 2.13 |
| the first run | factory reset replays it | `firstrun`, `reset` | 2.7 |

One rule cuts across all of them, and it is the whole precedence model:

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| **a pending prompt (`waiting`)** | **the prompt** |
| a message card | the card |
| the sign | the sign |
| the toy | the toy |
| the stats screen | the stats screen |
| anything else | the timer |

**A pending question outranks everything except the first run.** That is the
single failure this device is built not to have. A countdown, a sign or a
milestone is something the owner can reconstruct from memory or a phone; an
approval prompt is a thing only this panel is showing. The card, the sign and
the timer yield the panel and come back; the toy and the stats screen close
outright, because after answering a prompt somebody is back at their keyboard
and a game reappearing over their answer is the same failure in a different
costume. Every one is refused from the button and from the wire while a prompt
is pending, out loud, with `err: ... refused, prompt pending`. Every one also
reports its solved layout and its figures on the wire, because nobody can read a
panel over USB: `../docs/protocol.md` section 3.2a is the full list.

## Frame cost

`FRAME_MS` is 33, a 30fps cap. Every composition on every face holds the locked
30.3fps, and the worst frame measured anywhere in the project is 17.97ms, 54% of
the budget (`FACES.md` part 6 has the 21-scene table). The dominant cost in
every scene is the fixed ~13ms of `fillScreen` plus `pushSprite` on a 320x170
16-bit sprite; the most expensive face costs about 3ms on top. That headroom is
why there was room for three faces, a game and six modal screens.

## Hardware notes

- `PIN_POWER_ON` (GPIO 15) is driven HIGH first thing in `setup()`. The panel
  will not come up without it.
- The back buffer is a full-screen 320x170 16-bit `LGFX_Sprite` in PSRAM
  (108,800 bytes), falling back to internal RAM. If both fail it prints
  `err: sprite alloc failed, drawing direct` and renders straight to the panel at
  5fps, so the unit still shows its state instead of a black screen. That line is
  also the free per-unit PSRAM check on flash day.
- Rotation 2, landscape 320x170, `offset_x = 35`, `invert = true`. The config
  sets `offset_rotation = 1`, which LovyanGFX adds to the rotation before
  deciding whether to swap the panel's native 170x320, so **0 and 2 are
  landscape, 1 and 3 are portrait**. 2 is what every vendor config ships with.

## Fleet image hash

Every unit is flashed with the byte-identical image, so the whole fleet has one
hash. Rebuild from a clean tree and compare:

```sh
cd firmware && rm -rf .pio && pio run
shasum -a 256 .pio/build/tdisplays3/firmware.bin
```

A recipient can dump their own board and compare against the same value with
`esptool.py --port /dev/cu.usbmodem<N> read_flash 0x10000 <size> dump.bin`. The
hash changes whenever the firmware does, so record it at flashing time alongside
the fleet log rather than pinning a value here. `../docs/verification.md` is
this written for a recipient.
