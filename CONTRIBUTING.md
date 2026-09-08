# Contributing

This is a small project with two halves that barely know about each other: a
firmware image for one specific board, and one Node file. You can work on
either without the other.

Read `docs/architecture.md` first if you want to know why any of it is shaped
the way it is. This page is the mechanics.

---

## What you need

**For the helper:** Node 20 or newer. That is the entire list. There are no
dependencies, no build step and no lockfile.

**For the firmware:** PlatformIO Core (`pio` on `PATH`) and, to see anything,
a LILYGO T-Display-S3. Everything else (the ESP32 toolchain, the Arduino core,
LovyanGFX, ArduinoJson) is fetched and pinned by `firmware/platformio.ini` on
the first build.

```sh
pio --version     # PlatformIO Core 6.2.0 or newer
node --version    # v20 or newer
```

**Optional:** `qrencode` for the printed card's QR (`brew install qrencode`),
`pyserial` for `firmware/tools/sim.py`. Neither is needed to build, flash or
test anything.

You do **not** need a board to work on the helper, and you do not need Codex
installed to work on either. `demo` drives every device state with no Codex
involved, and the test suite never opens a serial port.

---

## Build

```sh
cd firmware
pio run
```

That is the whole build, for one board or for all fourteen. It takes a couple
of seconds warm and prints a size report; anything around 6.5% flash and 6.7%
RAM is normal.

**There is no per-unit build.** Every unit is flashed from the byte-identical
image so the fleet has one binary and one hash. Nothing is stamped in at
compile time, not the owner name and not a unit number. If you find yourself
adding a build flag that varies per board, stop and read
`docs/architecture.md`, "Reversal 3".

To check you are producing the same image as everyone else:

```sh
cd firmware && rm -rf .pio && pio run
shasum -a 256 .pio/build/tdisplays3/firmware.bin
```

### Build flags worth knowing

| Flag | Effect |
|---|---|
| `-DFPS_DEBUG` | prints `fps: mode=... avg_ms=... max_ms=...` every 3 seconds on the same USB CDC the protocol uses. **Never in a fleet image**, because it dirties the protocol channel. This is how every timing number in the docs was measured |
| `-DUNIT_ID=<n>` | renders `UNIT 03` instead of the MAC-derived label |
| `-DUNIT_NAME="\"Some Name\""` | a build-time default owner name, outranked by anything in NVS |
| `-DDEFAULT_FACE="\"bear\""` | which face a board with empty NVS comes up in |

All four are passed through the environment and append to the project's own
flags rather than replacing them:

```sh
PLATFORMIO_BUILD_FLAGS="-DFPS_DEBUG" pio run -t upload
```

Any of the last three gives that image its own hash, which is why nothing on
the batch path sets them.

---

## Flash

One board:

```sh
cd firmware
pio run -t upload                                  # auto-detects the port
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

If a board will not enter download mode: hold **BOOT** (button 1, GPIO 0), tap
**RST**, release BOOT, then upload.

The whole fleet, one at a time, prompting for a board swap between each:

```sh
./tools/flash-all.sh          # units 1..14
./tools/flash-all.sh 5 8      # units 5..8 only
```

Per unit it waits for a port whose `pio device list --json-output` hwid
contains `303A:1001`, builds, uploads, verifies the greeting off the port,
re-arms the first run, and appends a row to `tools/fleet-log.tsv` (`ok`,
`no-greeting` or `no-arm`; only `ok` is shippable). Two things to know before you start a
run: it takes the **first** `303A:1001` match, so unplug every other ESP32-S3
first, and it **aborts the whole run** if no board appears within 120 seconds,
so use sub-ranges rather than letting a flaky cable at unit 9 end the
afternoon. The log is append-only, so resuming loses nothing.

`docs/wednesday-runbook.md` is the full flash-day plan.

### Talking to a flashed board

```sh
pio device monitor -b 115200

printf '{"state":"waiting","ring":0.62,"center":"62%%","label":"CTX"}\n' > /dev/cu.usbmodemXXXX
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemXXXX
printf '{"face":"bear"}\n'        > /dev/cu.usbmodemXXXX
printf '{"reset":"factory"}\n'    > /dev/cu.usbmodemXXXX
```

Or drive every state from the helper, which needs nothing installed:

```sh
node helper/codex-companion.js demo --port /dev/cu.usbmodemXXXX --loop
```

**One quirk that will waste an hour if nobody tells you.** The device's USB CDC
transmit path runs exactly one message behind: the `ok` for line N does not
reach the host until the host writes line N+1. It is in `HWCDC`, not in this
code, and it was verified on both the current and the pre-ambient firmware on
the same board. So whenever you are reading for a reply, **write an extra `{}`
line to flush it**. `{}` is the protocol's no-op keepalive and changes nothing
on the board. `verify_hello` in `firmware/tools/flash-all.sh` is the worked
example: it resets via DTR and RTS, then reads while nudging with `{}` about
once a second.

Corollary for host code: treat acks as a running count, not a per-line
handshake. A host that fails the last line of every burst is seeing this and
not a bug.

---

## Tests

Only the helper has an automated suite. The firmware's tests are a board, your
eyes, and `docs/NEEDS-EYES.md`.

```sh
cd helper
npm install    # installs nothing; there are no dependencies
npm test       # node:test, no network, no serial port opened
```

One file:

```sh
node --test test/metrics.test.js
```

The suite covers the `ioreg` port parser and its refusal to guess between two
boards, the frame builder and its clamps, the metrics reader against captured
and synthetic session fixtures, the `hooks.json` merge and unmerge, and the
hook end to end: a real child process, a realistic payload on stdin for each
of the eight registered events, and the port pointed at an ordinary file so the
exact bytes that would go on the wire can be asserted.

**Two tests are load bearing and should never be weakened.** One walks the
entire metrics object built from a real captured session and fails if any value
is a string. One spawns the real hook for every event with the port pointed at
a device that does not exist and asserts exit 0 each time. Those two encode the
promises on the printed card: nothing textual from your session reaches the
wire, and a broken desk toy can never break somebody's work. If a change makes
either awkward, the change is wrong.

### Adding a test

Drop a `*.test.js` in `helper/test/`. `npm test` globs them. Fixtures live in
`helper/test/fixtures/`; prefer the synthetic one when you are adding a case,
and read its first line before you edit it.

### Checking the helper's own claims

The claims in `helper/README.md` are written to be checked by grep rather than
believed. From `helper/`:

```sh
grep -n "require(" codex-companion.js
grep -nE "require\((.)(node:)?(http|https|net|tls|dns|dgram)" codex-companion.js
grep -nE "fetch\(|XMLHttpRequest|WebSocket" codex-companion.js
grep -niE "launchctl|launchagent|systemctl|crontab|plist|daemon" codex-companion.js
node codex-companion.js install-hook --dry-run
```

Run those after any change to the helper. If one of them starts returning
something new, you have changed what this program is, and the README, the
printed card and `docs/architecture.md` all have to change with it.

---

## Adding a face

The face is the product, it has been redrawn several times already, and it will
be redrawn again. That is why it lives behind an interface: a new face is one
new file and three lines in a registry, and it costs no reflash of the fleet to
switch between them.

`firmware/FACES.md` is the full reference. The short version:

1. **Copy `firmware/src/FaceRounded.cpp`.** It is the shortest of the three and
   the one whose channels map most directly onto the frame.
2. **Rename the class, and change `name()` and `description()`.** `name()` is
   the wire name: lowercase, 15 characters or fewer, and it is what gets stored
   in NVS, so keep it stable once boards are in the wild.
3. **Write your drawing.** You are handed a `FaceFrame` and a `FaceCanvas` and
   called once per rendered frame, after the ring and before the text. Read
   whatever you want out of the frame and ignore the rest; a face that ignores
   `state` entirely is legal and will just look the same in all five states.
4. **Export it** outside the anonymous namespace:
   `Face* faceMine() { return &gMine; }`
5. **Register it in `firmware/src/Faces.cpp`**: declare `Face* faceMine();`,
   widen the `gFaces` array, add one line to `ensure()`. The array's order is
   the button-cycle order.
6. `pio run -t upload`, then hold **button 2** until yours comes round, or send
   `{"face":"mine"}`.

Nothing else changes. Not the protocol, not the NVS layout, not `main.cpp`, and
not the greeting: the active face is deliberately **not** on the wire, because
two other programs match on that greeting string and neither should have to
learn anything new.

### The three rules a face must not break

1. **Never add the burn-in drift and never read the drift globals.** It is
   already baked into the `cx`, `cy`, `boxX` and `boxY` you are given. That is
   what lets the ambient composition wander nine pixels while the live one
   lands on pixel-exact geometry.
2. **Run every colour through `c.tint()`, including your own palette.** That is
   the ambient/live cross-fade. It is the identity function once a fade has
   settled, and a face that skips it will not fade with the rest of the screen.
3. **Stay inside the box, and clamp anything that moves.** Drawing past it
   lands on the label line.

### What a face must never do

- **Decide when to blink.** `open` is handed to you already multiplied: the
  blink driver's factor times the state's own lid expression. A face that
  blinks on its own timer gives you fourteen units blinking in lockstep, or in
  three different rhythms depending on which face is loaded. That is the
  specific bug this interface exists to prevent.
- **Animate a beat off `nowMs`.** The `waiting` pulse period moves with `tps`
  and its phase is accumulated outside, so anything meant to land with the ring
  (a hop, a bounce) has to ride `pulse`. Everything else you can compute from
  `nowMs` yourself.
- **Reach into `main.cpp`.** The state machine, the staleness timeouts, the
  blink and glance drivers, the cross-fade, the drift, the compositions, the
  ring, the backlight ladder and all text layout stay outside. Wanting one of
  them means you are asking for the wrong thing.

### Two things that will bite you

Both were learned the hard way here and are in `FACES.md` for the same reason:

- **`powf(sinf(M_PI * u), k)` is NaN at `u = 1`.** `sinf(M_PI)` is `-8.7e-8`,
  not zero, and a negative base with a fractional exponent is NaN. A NaN half
  width passes every `< minimum` test, reaches `lroundf` as garbage, and hands
  LovyanGFX a radius it never returns from: the board greets and then goes
  permanently silent. Clamp the base, and clamp the radius at both ends.
- **Anti-aliased primitives are not free.** `drawWideLine` is a per-pixel float
  job over its own bounding box. The `arc` face drawn as 88 wedges cost 24ms a
  frame against a 33ms budget that already spends 14ms on `fillScreen` plus
  `pushSprite`. The same stroke stamped as filled discs costs 3ms.

### Measure before you claim it is cheap

```sh
PLATFORMIO_BUILD_FLAGS="-DFPS_DEBUG" pio run -t upload
```

`FRAME_MS` is 33. Anything under about 25ms average holds the locked 30fps. All
three shipped faces sit between 14.5 and 18.0ms in their worst scene; put your
numbers in `FACES.md` part 6 next to theirs, worst of several 3-second windows,
per scene.

---

## House style

- **Comments explain why, not what.** The reason a number is 24 and not 30
  belongs next to it. Most of the comments in this tree are the argument for a
  decision, and several of them are the argument against the decision that came
  before.
- **A doc that names a file must name a file that exists.** Cite symbols rather
  than line numbers: `grep -n applyJsonLine firmware/src/main.cpp` still works
  after an edit, and `main.cpp:2847` does not.
- **Do not restate what another document says.** Read the code, run the
  command, quote the output. `docs/STATUS.md` got rewritten once because it
  drifted from the code it described.
- **No em dashes**, anywhere: files, comments, commit messages.
- **Mark what you have not checked.** The word UNCONFIRMED appears throughout
  `docs/open-questions.md` and it is doing real work. A visual claim nobody has
  looked at is not a fact, and `docs/NEEDS-EYES.md` exists because none of the
  automated checking here can see a panel.

## The four design laws

Every change is measured against these. They are in the project brief and they
are not negotiable.

1. **It must be delightful on USB power alone, with no host software ever
   installed.** Ambient mode is what most owners will see most of the time.
   The host program is an optional bonus.
2. **The host program stays one zero-dependency Node file.** It can never
   approve or deny anything. It installs nothing by default. No dependencies,
   no persistence, no network calls.
3. **Every unit flashes the byte-identical image.** One binary, one hash.
   Nothing compiled per unit.
4. **It must never be irritating to sit next to in an open-plan office.**

Law 4 is the one that gets forgotten and it has killed more ideas here than the
other three combined. It is why the milestone pill sits absolutely still after
its entrance, why the do-not-disturb sign only breathes, why the game borrows
the ambient blue instead of amber, and why there is no buzzer.
