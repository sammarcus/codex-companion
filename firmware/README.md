# codex-buddy firmware

Firmware for the Codex Desk Companion: a LILYGO T-Display-S3 (ESP32-S3, 16MB
flash, 8MB PSRAM, ST7789 170x320 8-bit parallel, native USB CDC) that shows a
live OpenAI Codex CLI session as a colour ring plus centre text.

No WiFi, no accounts, no pairing. A Node host helper tails `~/.codex/` and
streams newline-delimited JSON over USB serial at 115200.

## Layout

| Path | What |
|---|---|
| `platformio.ini` | PlatformIO project, env `tdisplays3` |
| `src/LGFX_TDisplayS3.hpp` | LovyanGFX device config (Bus_Parallel8 + Panel_ST7789 + Light_PWM) |
| `src/main.cpp` | protocol parser, state machine, ring renderer |
| `tools/sim.py` | streams a demo protocol sequence at a board over serial |
| `tools/flash-all.sh` | flashes units 1..14 from one image, stamping only `UNIT_ID` |
| `AMBIENT.md` | ambient mode, owner name, button behaviour |

Pins, panel offsets and invert flags all come from `../docs/hardware-recon.md`,
which cites the vendor sources line by line. The palette comes from
`../docs/design-spec.md` part 2 verbatim (all eight colours round-trip to the
RGB565 constants in `src/main.cpp`), plus one ambient-only teal that part 2 has
no state for. Animation timing is derived from part 2
but deliberately diverges for two states: `busy` breathes on part 2's 2400ms
`idle` period instead of running its 1000ms linear spinner, and `idle` is a
low-amplitude brightness breathe on the reported fill rather than part 2's
rotating 200-260 degree arc sweep. `sleep` (4000ms) and the pre-escalation
`waiting` pulse (900ms) do match. See also `../README.md`, which frames
design-spec as a doc this firmware borrows its palette and feel from, not its
exact 7-state model.

## Toolchain

Pinned, and all three resolve against what is installed locally:

- platform `espressif32@7.1.1`
- `lovyan03/LovyanGFX@1.2.28`
- `bblanchon/ArduinoJson@7.4.3`

Board id `lilygo-t-display-s3` ships with the platform (`pio boards lilygo`),
so flash size, `default_16MB.csv` and `qio_opi` memory type come from its own
board JSON; `platformio.ini` restates them so the file is self-describing.

## Build

```sh
cd firmware
pio run
```

Build one unit's image with its serial number baked in:

```sh
PLATFORMIO_BUILD_FLAGS="-DUNIT_ID=3" pio run
```

That is the whole per-board build. Owner names are **not** a build input: all
14 boards flash from one identical image and are named afterwards over the
wire, with a protocol line carrying `"name"`, which the device stores in NVS.
See "Naming a unit" below.

`-DUNIT_NAME` still exists, but only as a build-time **default** for anyone who
does want one baked in:

```sh
PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex Rivera\""' pio run
```

The quotes are part of the macro body and PlatformIO shlex-splits the
environment variable, which is why the inner pair is escaped.
`tools/flash-all.sh` does not set it.

`UNIT_ID` is not set in `platformio.ini` on purpose, and the environment
variable appends to (does not replace) the project's own build flags, so a
per-unit build keeps `BOARD_HAS_PSRAM` and the USB CDC flags intact.

A build with no `UNIT_ID` is not stamped `UNIT 00`, which would look like a
real serial number on a batch of 14 identical boards. `src/main.cpp` treats
`UNIT_ID == 0` as unset and the boot screen reads `UNIT -- <MAC>`, where
`<MAC>` is the last two bytes of the efuse MAC (`mac[4]`, `mac[5]`), so an
unstamped board is both obviously unstamped and still distinguishable from its
neighbours. It has to be that end of the address: the first three bytes are the
Espressif OUI and are identical across the whole batch, so a suffix taken from
there would print the same digits on all 14 boards.
`tools/flash-all.sh` injects `-DUNIT_ID=<n>` for units 1..14, so the batch path
never hits this.

## Flash

Single board:

```sh
pio run -t upload                       # auto-detects the port
pio run -t upload --upload-port /dev/cu.usbmodem1101
```

All 14 units, one at a time, prompting for a board swap between each. No names
are needed or accepted: every board gets the same image, and `-DUNIT_ID=<n>` is
the only thing that varies.

```sh
./tools/flash-all.sh          # units 1..14
./tools/flash-all.sh 5 8      # units 5..8 only
```

It matches the board by USB hwid `303A:1001` rather than by first
`/dev/cu.usbmodem*`, removes the stale `main.cpp.o` so a build cannot reuse the
previous unit's `UNIT_ID`, uploads, verifies the greeting off the port, and
appends a row to `tools/fleet-log.tsv` (unit, USB serial, UTC timestamp,
`ok` / `no-greeting`).

If a board will not enter download mode: hold **BOOT** (button 1, GPIO 0),
tap **RST**, release BOOT, then run the upload.

## Monitor

```sh
pio device monitor -b 115200
pio run -t upload -t monitor            # upload then attach
```

On boot the device prints its greeting, then `ok` for every line it accepts:

```
hello tdisplay-s3 v1 name="Alex Rivera"
```

The `hello tdisplay-s3 v1` prefix is unchanged and still exactly what the host
helper matches on (`/hello\s+tdisplay-s3/i`). The trailing `name="..."` is the
name the device is actually running with, so a host can read back what a
`"name"` line did without a second command. It is always quoted, so an unnamed
board is an unambiguous `name=""`.

## Naming a unit

The recipients' names are not known at flash time, so the name is runtime
state, not a build flag. Send one line:

```sh
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodem101
```

The device stores it in NVS (namespace `codexbuddy`, key `owner`), uses it from
that instant, and reloads it on every boot. Clear it with an empty string:

```sh
printf '{"name":""}\n' > /dev/cu.usbmodem101
```

Precedence, highest first:

1. the name stored in NVS
2. `-DUNIT_NAME`, if the image was built with one
3. no name: the screens show the product name and the unit id

Clearing the stored name therefore falls back to `UNIT_NAME` when there is one,
and to the unnamed screens when there is not.

Two properties worth knowing:

- **Writes are idempotent.** The device only touches flash when the value
  actually differs from what is stored, so a host that repeats `"name"` in
  every heartbeat frame costs nothing in flash wear.
- **The cap is 24 characters**, silently truncated past that. LovyanGFX Font2
  at text size 1, measured on the panel, has a widest printable-ASCII advance
  of 10px, and the ambient headline is centred while riding a +/-9px burn-in
  drift, so 320 - 2*9 = 302px of guaranteed-clear width is 30 worst-case
  characters. 24 is that ceiling with a deliberate margin: 240px worst case,
  31px of clear panel each side. Non-ASCII will not render; the built-in
  LovyanGFX bitmap fonts have no glyphs for it.

## Feeding it data by hand

```sh
python3 tools/sim.py                    # auto-picks /dev/cu.usbmodem*
python3 tools/sim.py --port /dev/cu.usbmodem1101 --loop
```

Needs `pyserial` (`python3 -m pip install --user pyserial`). Or poke a single
frame at it:

```sh
printf '{"state":"waiting","ring":0.62,"center":"62%%","label":"CTX","sub":"12:34 elapsed","tps":17.3}\n' \
  > /dev/cu.usbmodem1101
```

## Protocol

Host to device, one JSON object per line. Any field may be omitted and the
device keeps its previous value, so a delta line is legal:

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `sleep` / `idle` / `busy` / `waiting` / `done`. Any other value makes the whole line malformed: it is dropped silently, with no `ok`. |
| `ring` | number | ring fill, clamped to 0..1 |
| `center` | string | big text inside the ring, buffer clamped to 15 chars. Whatever will not fit the ring's 76px inner hole steps down a font size and is then truncated, so plan on roughly 4 chars at the large face and ~9 at the small one. |
| `label` | string | small text above the sub line, clamped to 23 chars |
| `sub` | string | smallest line at the bottom, clamped to 39 chars |
| `tps` | number | tokens/sec, clamped to 0..10000; shortens the `waiting` pulse |
| `name` | string | owner name, clamped to 24 chars. Persisted to NVS and used immediately; `""` clears it. The only field whose effect outlives the power cycle. Written to flash only when it differs from what is stored |

Device to host:

- `hello tdisplay-s3 v1 name="<active name>"` once, after the 2s boot screen.
  The prefix through `v1` is fixed; `name` is empty when the unit is unnamed
- `ok` per accepted line
- malformed lines are ignored silently, with no reply

Lines longer than 512 bytes are discarded whole rather than truncated, so a
partial write can never be parsed as a valid frame.

**Known quirk, pre-existing and not introduced by the ambient work:** the
device's USB CDC transmit path runs exactly one message behind. The `ok` for
line N does not reach the host until the host writes line N+1; with a 3 second
wait and nothing further sent, it never arrives at all. Verified on both the
current firmware and the pre-ambient firmware on the same board, so it is in
`HWCDC`, not in this code. Over the whole stream nothing is lost: the ack count
always reconciles with the number of accepted lines. A host that treats a
missing `ok` as a failure will mis-report the last line of every burst, so
either treat the acks as a running count rather than a per-line handshake, or
rely on the fact that the helper's own 2000ms keepalive keeps flushing them.

## Behaviour

| State | Ring |
|---|---|
| `idle` | ring shows the reported fill, with a low-amplitude 2400ms brightness breathe so a live unit never looks frozen |
| `busy` | slow breathe, 2400ms sine, amber; ring shows the reported fill |
| `waiting` | amber pulse on the reported fill; period shortens as `tps` rises (900ms down to 700ms), then flips to red and halves at 10s. Button 1 acknowledges it: steady amber, no pulse, no escalation, until the next state change |
| `done` | green flash, 600ms: ramps up over 120ms, decays, then cross-fades into the idle look over the last 150ms. Armed only on the transition into `done`, so a repeated `done` line is a keepalive and does not replay the flash |
| `sleep` | dim slow breathe, 4000ms, full ring |

All five of those are the **live** view, which the device shows only while a
host is actually talking to it. With no host it runs ambient mode instead; see
`AMBIENT.md`.

Staleness, measured from the last accepted line:

- **30s** with no data: backlight drops to the dim tier, and `busy` / `waiting`
  fall back to the idle look so a dead host cannot leave the board pulsing red
  for an approval prompt that no longer exists. The stored state is untouched;
  one fresh line restores it.
- **5 min** with no data: back to ambient mode. This used to force `sleep`,
  which left a unit with no host software permanently dark; `sleep` is now
  reached only when a host asks for it.

Backlight:

- ambient: full for the first 60s, then the `70` tier for as long as it lasts.
  It never reaches the `20` sleep tier.
- live, whichever of the two is dimmer: staleness (full while fresh, `70` after
  30s) and state (`20` while `sleep` is held, `70` once `idle` has been held
  15s, full otherwise)

Button 1 (GPIO 0, the BOOT button) acknowledges a pending `waiting` prompt if
there is one: the pulse settles to a steady amber and stops escalating to red
until the next state change. With nothing to acknowledge it does what it always
did, waking the display to full brightness if the auto ladder had dimmed it,
otherwise stepping 255 -> 160 -> 70 -> 20 -> 255.

Button 2 (GPIO 14) is a dedicated brightness cycle: one press, one step, no
wake-first special case. A manual level holds until the auto tier itself
changes.

## Hardware notes

- `PIN_POWER_ON` (GPIO 15) is driven HIGH first thing in `setup()`. The panel
  will not come up without it.
- The back buffer is a full-screen 320x170 16-bit `LGFX_Sprite` allocated in
  PSRAM (108,800 bytes), falling back to internal RAM if PSRAM refuses. If both
  allocations fail the firmware prints `err: sprite alloc failed, drawing
  direct` and renders straight to the panel at 5fps, so the unit still shows
  its state instead of a black screen.
- Rotation 2, landscape 320x170, `offset_x = 35`, `invert = true`. The panel
  config sets `offset_rotation = 1`, which LovyanGFX adds to the rotation
  before deciding whether to swap the panel's native 170x320: rotations **0 and
  2 are landscape, 1 and 3 are portrait**. 2 is the value every LovyanGFX
  config in the vendor SDK ships with.
