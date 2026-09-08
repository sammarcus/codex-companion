# codex-buddy firmware

Firmware for the Codex Desk Companion: a LILYGO T-Display-S3 (ESP32-S3, 16MB
flash, 8MB PSRAM, ST7789 170x320 8-bit parallel, native USB CDC) that shows a
live OpenAI Codex CLI session as a pair of blinking eyes plus a colour ring
and centre text.

No WiFi, no accounts, no pairing. An optional Node host helper
(`../helper/codex-companion.js`, one file, no dependencies) streams
newline-delimited JSON over USB serial at 115200. It gets its state from
Codex's own first-party hook events and reads the session rollout file only
for numbers. The device is complete without it: with no host talking to it it
runs ambient mode forever.

## Layout

| Path | What |
|---|---|
| `platformio.ini` | PlatformIO project, env `tdisplays3` |
| `src/LGFX_TDisplayS3.hpp` | LovyanGFX device config (Bus_Parallel8 + Panel_ST7789 + Light_PWM) |
| `src/main.cpp` | protocol parser, state machine, the blink/glance drivers, ring and text |
| `src/Face.hpp` | the face interface: one abstract class, the frame it is handed, the geometry contract |
| `src/Faces.cpp` | the face registry, the button-cycle order and the compile-time default |
| `src/FaceRounded.cpp` | the default face: soft rectangles with a wet highlight |
| `src/FaceBear.cpp` | a warm cream creature whose mood is in its mouth |
| `src/FaceArc.cpp` | two tapering hand-drawn strokes |
| `tools/sim.py` | streams a demo protocol sequence at a board over serial |
| `tools/flash-all.sh` | flashes units 1..14, every one from the byte-identical image |
| `FACES.md` | the face interface: how to write one, the geometry contract, the `face` field, persistence |
| `EYES.md` | the default face: geometry, blink timing model, per-state expressions, composition |
| `AMBIENT.md` | ambient mode, owner name, button behaviour |
| `FIRSTRUN.md` | the out-of-the-box sequence, how it plays once, the replay and factory-reset gestures, the flashing procedure |

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

That is the whole build, for one board or for all fourteen. **There is no
per-unit build.** Every unit is flashed from the byte-identical image, so the
fleet has one binary and one hash. Nothing is stamped in at compile time:
neither the owner name nor the unit number.

Units still tell themselves apart. With `UNIT_ID` unset, `src/main.cpp` derives
the label from the chip's own efuse MAC and the boot screen reads `UNIT AB12`,
where `AB12` is the last two bytes (`mac[4]`, `mac[5]`). It has to be that end
of the address: the first three bytes are the Espressif OUI and are identical
across the whole batch, so a suffix taken from there would print the same
digits on all 14 boards. A `UNIT 00` was deliberately avoided, since it would
read like a real serial number.

Owner names are set afterwards over the wire, with a protocol line carrying
`"name"`, which the device stores in NVS. See "Naming a unit" below.

Both build flags still exist for anyone who wants them, and neither is on the
batch path:

```sh
PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex Rivera\""' pio run
```

`-DUNIT_ID=<n>` renders `UNIT 03` instead of the MAC label. `-DUNIT_NAME` is a
build-time **default** owner name, outranked by anything stored in NVS. The
quotes around the name are part of the macro body and PlatformIO shlex-splits
the environment variable, which is why the inner pair is escaped.

`UNIT_ID` is not set in `platformio.ini` on purpose, and the environment
variable appends to (does not replace) the project's own build flags, so such a
build keeps `BOARD_HAS_PSRAM` and the USB CDC flags intact. Use either flag and
you accept that the unit now has its own binary and its own hash, which is why
`tools/flash-all.sh` sets neither. It passes
`PLATFORMIO_BUILD_FLAGS="${UNIT_BUILD_FLAGS:-}"`, empty unless you deliberately
export `UNIT_BUILD_FLAGS` yourself.

## Flash

Single board:

```sh
pio run -t upload                       # auto-detects the port
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

All 14 units, one at a time, prompting for a board swap between each. Nothing
varies between boards: no names, no unit numbers, nothing per-unit at all.
Every board gets the same image, so after unit 1 the build is a cache hit and
each later unit costs upload time only.

```sh
./tools/flash-all.sh          # units 1..14
./tools/flash-all.sh 5 8      # units 5..8 only
```

Per unit it waits up to 120s for a port whose `pio device list --json-output`
hwid contains `303A:1001` (not a first-`/dev/cu.usbmodem*` glob, which would
happily pick a USB power meter), builds and uploads with
`PLATFORMIO_BUILD_FLAGS="${UNIT_BUILD_FLAGS:-}"`, verifies the greeting off the
port, re-arms the first run, and appends a row to `tools/fleet-log.tsv` (unit,
USB serial, UTC timestamp, `ok` / `no-greeting` / `no-arm`). Arming
(`arm_firstrun`) is the last thing done to each board, because anything that
powers it afterwards spends the flag again; it sends `{"reset":"firstrun"}` and
accepts only `reset: firstrun ok`, so a `ram-only` answer, meaning NVS refused
the write, lands as `no-arm` and that board is not shippable. The greeting check resets the board itself by
pulsing DTR and RTS, then reads for up to 12 seconds while nudging with a bare
`{}` about once a second, because the device's transmit path runs one message
behind (see the known quirk under "Protocol"). On success it echoes the
greeting line, so the stored owner name is readable straight off the terminal.

The number in `UNIT n of LAST` is only the operator's place in the run, for the
log. It is not compiled into anything.

Two failure modes worth knowing before flash day: it **aborts the whole run**
if no board appears within 120s, so run it in sub-ranges (`1 5`, `6 10`,
`11 14`) rather than letting a flaky cable at unit 9 end the afternoon
(`fleet-log.tsv` is append-only, so resuming loses nothing). And `find_port`
takes the **first** `303A:1001` match, so unplug every other ESP32-S3 first:
any of them enumerates under the same id and the same generic
`USB JTAG/serial debug unit` description.

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
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemXXXX
```

The device stores it in NVS (namespace `codexbuddy`, key `owner`), uses it from
that instant, and reloads it on every boot. Clear it with an empty string:

```sh
printf '{"name":""}\n' > /dev/cu.usbmodemXXXX
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
  31px of clear panel each side.
- **Printable ASCII only.** The name goes through the same sanitiser the
  message card uses: bytes outside 0x20..0x7E are dropped, whitespace is
  collapsed and the ends are trimmed. The built-in LovyanGFX bitmap fonts have
  no glyphs outside that range, so an accent has nothing honest to draw, and
  dropping it is the only alternative to drawing mojibake. That also makes the
  cap true as written: it used to be a promise about *characters* enforced by a
  *byte* loop, so a name written with real accents stored at half the
  documented length and one that hit the cap mid-sequence stored a dangling
  UTF-8 lead byte, which the panel and the boot greeting both carried. A name
  that had something in it and sanitises to nothing is **refused**
  (`err: name refused, nothing printable in it`) rather than being treated as a
  clear: `""` is how you clear a name, and a host that sent one meant to set
  one. Send the ASCII spelling of a name you want on the panel.

## Feeding it data by hand

```sh
python3 tools/sim.py                    # auto-picks /dev/cu.usbmodem*
python3 tools/sim.py --port /dev/cu.usbmodemXXXX --loop
```

Needs `pyserial` (`python3 -m pip install --user pyserial`), which is not
installed on this machine today. Note that `sim.py` picks
`sorted(glob("/dev/cu.usbmodem*"))[0]` when given no `--port`, which on a
crowded bus resolves to whatever sorts first, not to the board. Pass `--port`.

`sim.py` also sends `APPROVE` and `DONE` as labels, which the real host helper
never does, so for at least one unit drive it with the helper instead. That
needs nothing installed:

```sh
node ../helper/codex-companion.js demo --port /dev/cu.usbmodemXXXX --loop
```

Or poke a single frame at it:

```sh
printf '{"state":"waiting","ring":0.62,"center":"62%%","label":"CTX","sub":"12:34 elapsed","tps":17.3}\n' \
  > /dev/cu.usbmodemXXXX
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
| `center` | string | big text inside the ring, buffer clamped to 15 chars. Whatever will not fit the ring's 66px inner hole steps down a font size and is then truncated, so plan on roughly 4 chars at the large face and ~9 at the small one. |
| `label` | string | small text above the sub line, clamped to 23 chars |
| `sub` | string | smallest line at the bottom, clamped to 39 chars |
| `tps` | number | tokens/sec, clamped to 0..10000; shortens the `waiting` pulse |
| `say` | string | a short message, shown large on a card that takes the whole screen. Sanitised to printable ASCII and capped at 48 chars; `""` (or anything that sanitises to nothing) clears it. Never makes a line malformed. See "Messages" below |
| `saysecs` | number | how long the card holds, in seconds. Default 30, clamped to 0..3600, `0` holds until it is cleared. A negative value falls back to the default rather than clamping into the never-expires sentinel. On a line with no `say` it re-times the card that is already up |
| `dnd` | boolean | `true` puts the DO NOT DISTURB sign up, `false` takes it down. The second door to a feature whose first door is the two-button chord below. Never makes a line malformed, never persisted, and not a state: it covers the screen and changes nothing else. See "Do not disturb" |
| `name` | string | owner name, clamped to 24 chars. Persisted to NVS and used immediately; `""` clears it. Its effect outlives the power cycle. Written to flash only when it differs from what is stored |
| `face` | string | which face to draw: `rounded`, `bear` or `arc`, case insensitive. Applied on the next rendered frame and persisted to NVS, so this one outlives the power cycle too. An unregistered name makes the whole line malformed, exactly like an unknown `state`. See `FACES.md` |
| `reset` | string | `"factory"` clears the whole NVS namespace (name, face, first-run flag); `"firstrun"` re-arms the first run only and keeps the name. Any other value makes the whole line malformed. Applied before every other field on the line |
| `firstrun` | string | `"play"` plays the out-of-the-box sequence now, without touching the stored flag. Refused during `busy` and `waiting`. Any other value makes the whole line malformed. Applied last |
| `play` | string | the toy. `"on"` opens it, `"off"` closes it, `"hop"` jumps. Any other value makes the whole line malformed, exactly like an unknown `state`. `"on"` is refused while a prompt is pending; a `"hop"` with no game up is accepted and does nothing. The second door to a feature whose first door is a button hold, and the shipped helper sends none of the three. See "Something to do" |
| `time` | number | the host's **local** wall clock in seconds: unix time plus the host's own UTC offset. Optional, never makes a line malformed, and anything below 2020-01-01 is ignored. It is what gives the daily figures a midnight; without it they run from boot and the stats screen says `SINCE BOOT`. See "Stats and milestones" |
| `tokens` | number | the current Codex session's cumulative token total. The device banks the differences, never the value, so a lower value is read as a new session. Optional and never malformed. Without it the token figures stay at zero and everything else still counts |
| `stats` | string | the stats screen. `"on"` opens it, `"off"` closes it. Any other value makes the whole line malformed, exactly like an unknown `state`. `"on"` is refused while a prompt is pending. The second door to a feature whose first door is a tap on button 1 |
| `focus` | string | the focus timer. `"show"`, `"hide"`, `"start"`, `"pause"` or `"reset"`. Any other value makes the whole line malformed, exactly like an unknown `state`. The *screen* half is refused while a prompt is pending; the clock never is. The second door to a feature whose first door is two taps of button 1. See "Focus timer" |
| `focusmins` | number | the timer's length in minutes, clamped to 1..180 and persisted to NVS. Optional, never malformed. Changing it mid-run leaves the current run on its own length |

Device to host:

- `hello tdisplay-s3 v1 name="<active name>"` once, after the 2s boot screen.
  The prefix through `v1` is fixed; `name` is empty when the unit is unnamed
- `ok` per accepted line
- malformed lines are ignored silently, with no reply
- unsolicited notices, in addition to the `ok`: `firstrun: playing (first)`,
  `firstrun: playing (replay)`, `firstrun: spent`, `reset: factory ok`,
  `reset: firstrun ok`, the `say:`, `dnd:`, `play:`, `stats:`, `focus:` and
  `milestone:` lines below, and the
  `err:` lines. These
  make the first-run lifecycle and the message card provable over the wire;
  every existing consumer matches on prefixes or substrings these do not
  collide with

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

### The first run

The very first time an owner powers a board, it plays an 8.6 second
out-of-the-box sequence instead of settling straight into ambient: dark, a
light comes up on a pair of shut eyes, they crack open, look around, find the
person in front of them and blink, and then it says whose it is. It settles
into ambient invisibly, because the last second of the sequence already **is**
the ambient composition.

It plays exactly once. The fact that it played is a byte in NVS beside the
owner name and the face, and the `nvs` partition is not one of the four regions
`pio run -t upload` writes, so re-flashing the same image does not bring it
back. Sam can power and re-flash a board as often as he likes.

Reachable again deliberately with `{"firstrun":"play"}`, which does not touch
the flag and is refused while a turn is running or a prompt is pending. **The
button gesture for this is gone**: button 1's two second hold is the toy now,
and the reasoning for that trade is under "Something to do".

Wipe a board back to out-of-the-box: **hold both buttons for 5 seconds in
ambient mode**, or send `{"reset":"factory"}`. To keep the name and only re-arm
the sequence, which is what the flashing run wants as its last step, send
`{"reset":"firstrun"}`.

Full sequence, persistence, gestures and the flashing procedure: `FIRSTRUN.md`.

### Live mode

Live mode draws a face on the left and the ring on the right, with the label
and sub lines centred under both. Every state moves both.

| State | Ring | Face |
|---|---|---|
| `idle` | ring shows the reported fill, with a low-amplitude 2400ms brightness breathe so a live unit never looks frozen | open, blinking at the resting rate, glancing around now and then |
| `busy` | slow breathe, 2400ms sine, amber; ring shows the reported fill | lids down to a concentrating squint, gaze tracking back and forth, blinking less |
| `waiting` | amber pulse on the reported fill; period shortens as `tps` rises (900ms down to 700ms), then flips to red and halves at 10s. Button 1 acknowledges it: steady amber, no pulse, no escalation, until the next state change | eyes wide, eyebrows up, head hopping on every pulse peak, blinking roughly twice as often. Escalation widens the eyes further and drops the brows toward the nose. An acknowledgement keeps the wide eyes but stops the hop and levels the brows |
| `done` | green flash, 600ms: ramps up over 120ms, decays, then cross-fades into the idle look over the last 150ms. Armed only on the transition into `done`, so a repeated `done` line is a keepalive and does not replay the flash | a happy squint held 1600ms, hopping once on arrival, then the eyes lift back open |
| `sleep` | dim slow breathe, 4000ms, full ring | shut, the lid line breathing on the same 4000ms sine |

Full detail, including the blink timing model and the measured frame cost of
all of it, is in `EYES.md`.

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
until the next state change. That still happens on the **press** edge, so an
ack is instant. With nothing to acknowledge it **opens the stats screen**, on
the release edge, because the press edge alone cannot tell a tap from a hold.
Opening the screen wakes a panel the auto ladder had dimmed, exactly as an ack
does.

That tap used to be a second brightness cycle, and it still is in the one case
where the numbers cannot be shown: while a prompt is pending, `statsEnter`
refuses and the button falls back to waking the display and stepping
255 -> 160 -> 70 -> 20 -> 255. So the button is never a dead press. The trade is
argued under "The button model, again".

**On the stats screen the same button goes on to the focus timer** rather than
closing. Button 1 is "tell me something" and there are now two things it can
tell you, so it walks them: base composition, then the numbers, then the timer.
Button 2 still closes, on every modal screen. The legend along the bottom of
the stats screen says `1 FOCUS    2 CLOSE`, which is the whole navigation model
written where somebody with no manual can find it.

Button 2 (GPIO 14) is a dedicated brightness cycle: one tap, one step, no
wake-first special case. A manual level holds until the auto tier itself
changes.

Holds, all of them well clear of each other so a slow finger cannot reach the
wrong one:

| Gesture | Threshold | Effect |
|---|---|---|
| both tapped together | 120ms, released before the FACTORY RESET warning appears at 1.5s | do not disturb on or off. See "Do not disturb" |
| button 2 held | 800ms | next face, persisted |
| button 1 held | 2s | open the toy. Refused while a prompt is pending. See "Something to do" |
| both held together | 5s | factory reset, ambient mode only, then replays the sequence to confirm itself |

Two more holds exist and neither is reachable from the base compositions: on
the **focus timer's screen**, holding button 1 resets the countdown and holding
button 2 steps the length. Both are at the same 800ms, so the device still has
exactly the three thresholds it had before (800ms, 2s, 5s). See "Focus timer".

**Every hold says how far along it is.** Past 400ms a thin bar fills along the
top edge of the panel toward that gesture's threshold, and the factory chord
keeps its own red bar and warning across the bottom. Without it a hold was
blind and letting go early did not do nothing: the release runs the TAP
instead, so a 1.9 second reach for the toy arrived as the stats screen and a
700ms reach for a face arrived as a brightness step. Both are recoverable and
neither was learnable. The bar is drawn on exactly the conditions the gesture
fires on, so it never promises something that will not happen: not while a
modal screen is up, not while the other button is also down, and not when the
screen changed under the finger after the press.

While button 1 is down, button 2's face cycle is suppressed, so travelling to
the five second combination does not swap the face on the way. The other
ordering is refused rather than delayed: if button 2 goes down first and passes
800ms alone it has spent its press on the face cycle (`button2Held`), and both
the wipe and this bar test that flag, so the chord produces a face swap and then
silence until the fingers come off and go back down together. See
`FIRSTRUN.md`.

### Messages: the say card

A short message pushed down the cable and shown large enough to read across a
room. It is the one thing on this device somebody else is meant to read.

```sh
printf '{"say":"brb"}\n'                      > /dev/cu.usbmodemXXXX
printf '{"say":"in a meeting","saysecs":0}\n' > /dev/cu.usbmodemXXXX
printf '{"say":""}\n'                         > /dev/cu.usbmodemXXXX
```

It is a sign, so it takes the whole screen: a rounded frame, the message
centred in it, and a thin bar along the bottom that drains over the hold so
the card visibly has an end. The frame and the bar borrow the colour of
whatever the card is covering, so a message during a busy turn is amber and
one on an idle desk is the drifting ambient blue. A card with no expiry has no
bar, because nothing is counting down. Nothing moves after the 220ms entrance
except that bar and the anti burn-in drift, which the card applies at full
amplitude in both modes because it is the one composition here that can
legitimately hold the same pixels for an hour.

**Type.** Four rungs of FreeSansBold, largest first, and the first one the
message fits on wins: 24pt doubled (112px, one line), 24pt (56px, two lines),
18pt (42px, three), 12pt (29px, three, and it takes whatever is left). Every
candidate line is measured with `textWidth` rather than counted, because the
faces are proportional. `brb` lands on the top rung, `in a meeting` sets as
two lines of 24pt, and `deploying to prod, do not unplug` as three of 18pt.

**Too long.** Printable ASCII survives sanitising, whitespace collapses,
control bytes and everything `>= 0x80` are dropped, and 48 characters is the
cap. Words are thrown away in exactly two places, that cap and the three-line
limit of the last rung, and either earns a measured ellipsis on the last line.
A word wider than a whole line drops a rung, and at the last rung it is
hard-broken at the last character that fits.

**It never hides a question.** While a prompt is pending (`waiting`) the card
yields the screen back to the live view and takes it again when the prompt is
answered. It yields to the first-run sequence the same way. Its clock keeps
running while it is yielding, so a message can never queue up behind a long
prompt and surprise somebody ten minutes later.

**Buttons.** While a card is up, both buttons do one thing: the first press
dismisses it, and does nothing else. That is what makes `saysecs: 0` safe to
offer, because it is the escape from an open-ended message with no host in the
room. It is not a new gesture and it adds no hold: the card is modal, and
holds start clean from the next press.

The button model is nearly full. Two switches carry a tap each, a hold each,
and one combination, and every threshold (800ms, 2s, 5s) is already chosen so
a slow finger cannot reach the wrong one. The next feature that wants a button
should take a **mode**, the way this card does, rather than a fifth hold: one
more long press and nobody will be able to remember, let alone discover, what
this thing does. Do-not-disturb is what took that advice, under "Do not
disturb" below: a mode, on the one combination that was still free, with the
same press-to-dismiss rule as this card.

The device reports what actually reached the panel, sanitised, capped and
wrapped, one entry per rendered line:

```
{"say":"deploying to prod, do not unplug"}
say: showing "deploying to | prod, do not | unplug" 30s lines=3 font=18
ok
```

**Cost**, measured on the board with a `-DFPS_DEBUG` build, worst of several
3-second windows, against the same 33ms `FRAME_MS` budget as everything else:

| Scene | avg ms | max ms | fps |
|---|---|---|---|
| one line, `24x2`, over ambient | 13.38 | 13.53 | 30.3 |
| two lines, `24`, over live | 13.40 | 13.51 | 30.3 |
| three lines, `18`, over live | 14.47 | 14.62 | 30.3 |
| ambient with no card, same build | 14.75 | 14.91 | 30.3 |
| live `waiting`, card yielding to it | 16.76 | 16.99 | 30.3 |

The card is **cheaper than the composition it replaces**: it draws two round
rects, up to three strings and a bar, where ambient draws a face. The layout
is solved once when the message is set, not per frame, so the wrapping and the
`textWidth` measurements cost nothing at all while a card is on screen.

Full behaviour, the notice list and the field table are in
`../docs/protocol.md` section 2.9.

### Do not disturb

The one screen on this device aimed at somebody **walking past**, rather than
at the owner or at a session. It is the answer to "what is this object for if
I never install anything": a desk sign that a person puts up with their thumbs
and takes down with one.

**Press both buttons together and let go. Press either button to leave.**

```sh
printf '{"dnd":true}\n'  > /dev/cu.usbmodemXXXX   # the same thing from a host
printf '{"dnd":false}\n' > /dev/cu.usbmodemXXXX
```

A violet frame 8px thick, `DO NOT` and `DISTURB` in white FreeSansBold, and
the owner's name under them when the board has one. No face, no ring, no
metric, no bar.

**Why no face.** The face is this firmware's entire vocabulary for "alive and
working on something". The surest way for the object to say *not now* is for
the creature to be absent, not for it to pull a face about it.

**Why the frame is thick.** The panel is 44mm wide. At the 24pt rung the cap
height is about 4.6mm, which is legible to something like two metres and not
much past it, so the words alone cannot carry "readable from several metres".
A 296x154 violet rectangle can. The type is the message for whoever comes to
the desk; the colour and the shape are the message for whoever is crossing the
room.

**Why violet.** It must not be mistaken for `waiting`, which is the one state
that means *look at me now*. Violet (`C_DND`, `0xA35F`, nominally `#A56BFF`)
is a hue no state on this device owns, it is nowhere near the amber and red
that do mean that, and it is the conventional away colour in the chat clients
these fourteen people stare at all day. `waiting` is also small, pulsing and
faced; this is large, still and blank. Nothing about the two reads alike.

**Why it is still.** Design law 4: it must never be irritating to sit next to,
and this is the composition most likely to be up for an hour. Nothing moves
except a 6500ms breathe on the frame (so a sign and a crashed panel are still
distinguishable) and the anti burn-in drift, which it applies at full
amplitude in both modes for the same reason the message card does. The
backlight follows the ambient ladder, not the live one: full for 60 seconds,
then a tier down for as long as the sign lasts, because a violet frame at full
duty all evening is exactly the open-plan failure law 4 exists to prevent.

**It never hides a question.** Same rule as the message card, and the device
now has exactly one rule about what covers what:

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| a pending prompt (`waiting`) | the prompt |
| a message card | the card |
| anything else | the sign |

Yielding does not cancel the sign, it covers it, and it comes back on its own
when the prompt is answered or the card expires. A card and a sign together
take two presses to clear, which is that same one rule twice: **a press takes
down whatever is on top.**

**It is not persisted, deliberately.** RAM only, like the message card and
unlike `name`, `face` and the first-run flag. Unplugging a desk companion and
plugging it back in is something a person does *at the desk*, which is the one
moment a DO NOT DISTURB sign is provably stale; and a board that arrives in
its box or comes back from a colleague's desk must not come up holding a sign
nobody in the room set. The mode is one press to leave in any case, so
persisting it would buy an edge case and cost a wrong first frame.

**Why a chord, and why entry and exit are not symmetric.** A stray press must
never put a sign up on somebody's desk, and a mode nobody can find the way out
of is the worst thing a small device can do. The prior art in this repo says
so twice: `docs/prior-art-status-light.md` on the hooks port that latches
amber with no `PermissionResolved` event to clear it, so a resolved-then-quiet
session stays amber forever; and `docs/prior-art-hardware.md` on ccdev, which
guards a freshly drawn screen for 400ms (`ASK_TAP_GUARD_MS`) and queues rather
than replaces a question, precisely so a finger already moving cannot land on
something it never read. So:

- **entry costs two thumbs**: both switches, at opposite ends of the board,
  down together for 250ms. The five second version of the same chord is
  already the factory reset and that gesture's own documentation says two
  switches held together is not something a hand does by accident. A quarter
  of a second of overlap is a squeeze, not a brush.
- **exit costs one press, either button, on the press edge**, exactly like
  dismissing a message card. A stray press on a sign costs nothing: put it
  back up.
- **it resolves on release, not at a threshold.** Every other gesture here
  fires the moment its threshold is crossed, because that feels like a button
  rather than a timeout. This one cannot, because holding the same two buttons
  five seconds longer wipes the board, and raising a sign on the way to a
  factory reset is the same "surprise the gesture never asked for" that
  already stops button 2 swapping the face on that journey.

**The button model, honestly.** It was already full. This adds a **chord tap**,
which is the only affordance left that is both discoverable and impossible to
hit by accident, and it takes over the one combination that previously did
nothing (both buttons, released before the warning at 1.5s, which used to be two
brightness steps). The coherent scheme it lands on, and the one the next
feature should be measured against:

| | button 1 (GPIO 0) | button 2 (GPIO 14) | both |
|---|---|---|---|
| tap | acknowledge a prompt, else **the stats screen** | brightness | do not disturb |
| hold | 2s: **the toy** | 800ms: next face | 5s: factory reset |

One rule cuts across all of it: **on any modal screen, button 2 leaves, and
button 1 is that screen's own action.** On the message card and the sign there
is no action to have, so button 1 leaves too, which is why neither of them
needed a gesture of its own to escape. On the toy, the stats screen and the
timer, button 1 does something instead of leaving. The full table, and the
older, simpler rule this replaced, are in "The button model, again", below the
stats section.

There is no room left. The next feature that wants the buttons has to replace
something in that table or take a `mode` the way these two did, because a
sixth threshold would make the device unlearnable.

That is exactly what the toy did: it is a mode, and it took button 1's two
second hold rather than adding one. The stats screen then did the harder
version of the same thing: it **replaced** something. See "The button model,
again", below the stats section. The table above is current.

**Cost**, measured on the board with a `-DFPS_DEBUG` build, worst of several
3-second windows, against the same 33ms `FRAME_MS` budget:

| Scene | avg ms | max ms | fps |
|---|---|---|---|
| sign, no owner name | 14.50 | 15.00 | 30.3 |
| sign, 22-character owner name | 15.31 | 15.49 | 30.3 |
| ambient, same build, for comparison | 14.78 | 14.80 | 30.6 |
| live `waiting`, sign yielding to it | 16.63 | 16.99 | 30.3 |

The sign is about as cheap as ambient and cheaper than any face: it draws
eight concentric rounded rects and up to three strings. The frame is drawn as
a stroke rather than as one filled rect minus another, which is 6,944 pixels
instead of 84,224, and the two words are measured once when the sign goes up
rather than thirty times a second forever.

The device reports the solved layout, because nobody can read a panel over
USB and the measured widths are the only proof the words fit the frame:

```
{"dnd":true}
dnd: on (host) font=24 w=186,213 max=268
ok
```

**Refused while a prompt is pending**, on the chord and on the host field
alike, exactly like the toy, the stats screen and the timer:

```
{"state":"waiting"}
{"dnd":true}
err: dnd refused, prompt pending
```

That was the one modal screen here that accepted and then hid itself. The sign
is suppressed while a question is on the panel, so it latched a mode with
nothing on screen, both buttons are gated on the sign being visible so neither
could take it down, and it appeared on its own the moment the prompt resolved.
All five modal screens now follow one rule.

Full behaviour and the field table are in `../docs/protocol.md` section 2.10.

### Something to do

A Codex turn can run for ten minutes, and for those ten minutes the person in
front of it has nothing to do. So the two buttons carry a game.

**Hold button 1 for two seconds.** The face runs along a line and hops over
blocks. **Button 1 jumps, button 2 leaves.** The best score is kept in NVS and
survives the power cycle, which is the only reason anybody comes back to a
thing like this.

```sh
printf '{"play":"on"}\n'  > /dev/cu.usbmodemXXXX   # the same three doors
printf '{"play":"hop"}\n' > /dev/cu.usbmodemXXXX   # from a host
printf '{"play":"off"}\n' > /dev/cu.usbmodemXXXX
```

It is the Chrome dinosaur on purpose. Nobody has to be told what a creature
running at a block means, and that is the whole test for a screen with two
buttons, no menu, no host and no instructions. The legend along the bottom
(`1 HOP  2 EXIT`) drops the `1 HOP` half after the first jump and comes back
whole when the run ends, so the only thing it ever explains is the thing you
have not done yet. `2 EXIT` stays up for the whole run: wanting out mid-run is
the common case on a shared desk, and the alternative was crashing on purpose.

**The runner is the active face**, drawn through the ordinary `Face` interface
at a smaller geometry. The toy hands over a box, a colour and an `open` value
exactly as the ambient and live compositions do; it knows nothing about eyes,
bears or strokes. So the bear runs as a bear and the arc runs as two strokes,
a fourth face would run the day it is written, and the creature in the game
blinks on the same shared driver as the creature on the desk. The jump and the
gait are applied to the geometry rather than asked of the face, the same way
the first run's nod is: no face has to know this screen exists.

**It ends when a question arrives.** Not covered and restored the way the
message card is: `waiting` closes it on the frame it arrives, banks the score
and hands the screen straight back to the live view. After answering a prompt
somebody is back at their keyboard, and a game reappearing over their answer
would be the same failure in a different costume. It also refuses to open over
a prompt that is already pending, from the button and from the wire.

**It pauses when it is not on top.** A message card or a sign covers it, and a
frame the toy is not drawn on is a frame it does not move, so a card arriving
mid-run cannot cost a score nobody was watching. Coming back out from under one
clears the stretch and keeps the score: without that the runner reappeared with
a block already touching it and died for something it could not have seen,
which is a real death that was measured on the bench, not a hypothetical.

**It closes itself.** Thirty seconds with nobody pressing anything and the
board goes back to being an object on a desk, because design law 1 says ambient
is what most owners see most of the time. It is the one screen here that holds
the backlight at full while it is up, which is affordable precisely because it
cannot outlast somebody's attention by more than half a minute.

**The palette is the ambient blue**, deliberately. Amber and red are the two
colours on this device that mean "look at me now", and a game must never borrow
either: at four metres this has to read as the object being off duty, not as a
state nobody recognises.

**The physics are not free numbers, and the first set was wrong.** An obstacle
sits inside the hit box for `(2 * TOY_HIT_HALF + w) / speed` seconds, and the
jump is above it for however long the parabola spends over `h + TOY_HIT_CLR`.
The second has to beat the first **at the slowest speed**, because a slower
obstacle sits in the hit box longer: going faster makes this game easier to
clear and harder to react to, which is the classic's difficulty curve and the
opposite of the intuition. The first set of constants here had a 30px hit box
against 0.41s of clearance and lost that race at every speed. A bot playing
over the cable, jumping on the exact frame, died on the first block every time
and scored zero in ninety seconds. The shipped numbers give about 1.6x margin
at the starting speed:

| | clearance | worst hit window at 115px/s | margin |
|---|---|---|---|
| tall block, h 21, w 12 | 0.482s | 32px, 0.278s | 1.73x |
| wide block, h 14, w 17 | 0.519s | 37px, 0.322s | 1.61x |

Jump 450px/s against 1500px/s/s: a 67px apex and 0.60s of airtime. Speed runs
115 to 200px/s, +4.5 per point, and the gap between blocks is 200 to 330px of
ground rather than a number of milliseconds, so getting faster never turns into
getting unfair.

**Flash wear.** The best is written only when a run beats it, which is at most
once per game and usually far less. Nothing here writes on a timer.

**The button model.** This is a mode, and it took button 1's two second hold
rather than adding a fifth threshold, so the device gained a screen and did not
gain a gesture. The hold used to replay the first run; that was the weakest
binding on the board, because every owner is shown the first run once
automatically and it keeps two other doors (`{"firstrun":"play"}`, and the
factory reset which replays it to confirm itself), while the toy is a thing
somebody reaches for every time a turn runs long. Reversing that call is one
line: see `FIRSTRUN.md` section 3.

The toy is also the one modal screen here that is **played rather than read**,
which is why it is the one exception to "any press takes down whatever is on
top". A game needs a button, so the two split: 1 plays it, 2 leaves it, both on
the press edge and neither behind a hold. Abandoning has to be cheaper than
starting, and the score is banked on the way out, so a stray press on button 2
can never cost a record.

The device reports the run on the wire, because nobody can read a panel over
USB:

```
{"play":"on"}
play: hop (host) best=52
ok
{"play":"off"}
play: end (host) score=17 best=52
ok
```

| Notice | When |
|---|---|
| `play: hop (button)` / `play: hop (host)` | the toy opened, carrying the stored best |
| `play: over score=<n> best=<n>` | a run ended in a crash. ` (new best)` is appended when it beat the stored value, which is also the only time flash is written |
| `play: end (button)` / `(host)` / `(prompt)` / `(idle)` / `(firstrun)` | the toy closed, and why |
| `err: play refused, prompt pending` | `{"play":"on"}` arrived over a pending question |

**Cost**, measured on the board with a `-DFPS_DEBUG` build while a bot played it
over the cable, worst of seven 3-second windows per face, against the same 33ms
`FRAME_MS` budget as everything else:

| Face | avg ms | max ms | fps |
|---|---|---|---|
| `rounded` | 13.14 | 14.59 | 30.3 |
| `bear` | 13.41 | 15.38 | 30.3 |
| `arc` | 14.86 | 16.58 | 30.3 |

Every face plays at the locked 30.3fps, and the toy is **cheaper than the same
face in ambient or live** (`FACES.md` part 6: 14.96 to 17.97ms), because the
creature is drawn at 46x40 instead of filling the panel and everything else on
the screen is a handful of small rectangles.

### Stats and milestones

The device has been watching every turn go past since the first firmware and
nothing was counting them. This counts them, puts them on a screen a tap away,
and marks the two moments worth marking.

**Tap button 1.** Six figures: turns today and tokens today large, then today's
longest turn, how long this sitting has been going, the longest turn ever and
the best token day. Button 2 takes it down, button 1 goes on to the focus
timer, it closes itself after twenty seconds, and a question closes it
instantly. The legend on the panel says both: `1 FOCUS    2 CLOSE`. (An earlier
version of this paragraph said any press takes it down, which contradicted the
modal-rule table below and would send anybody pressing button 1 to leave into a
25 minute pomodoro instead.)

```sh
printf '{"stats":"on"}\n'  > /dev/cu.usbmodemXXXX   # the same two doors
printf '{"stats":"off"}\n' > /dev/cu.usbmodemXXXX   # from a host
```

**What it counts itself, and what it has to be told.**

| Figure | Where it comes from |
|---|---|
| turns today | counted here, off the state machine's own edges |
| longest turn today, longest ever | timed here, from those same edges |
| this session's length | timed here, from host contact |
| tokens today, best token day | **told**, by the optional `tokens` field |
| what day it is | **told**, by the optional `time` field |

A turn opens on the first `busy` (or on a `waiting` that precedes one) and
closes on the `done` that completes it, so `busy -> waiting -> busy -> done` is
one turn and not three. A turn that ends anywhere else was abandoned and is not
counted: counting it would reward walking away from a long one.

Tokens are not derived, deliberately. The wire carries `tps` and two display
strings, and integrating a rate sampled at hook events would produce a number
nobody could reconcile with what Codex tells them. So the host sends the session
total and the device banks the differences: the first value a board ever sees is
a baseline that contributes nothing (a board plugged in mid-session did not
watch those tokens happen) and anything above it adds the difference. None of
that needs the helper to remember anything between hook invocations, which is
what keeps it a stateless one-shot program.

**More than one session can be on one cable**, which is what the arithmetic is
actually shaped around. There is one hook process per Codex session and each
one sends the cumulative total of its own session, so two sessions alternating
against a single running value measure every high against the other session's
low and bank the whole of it: on the bench, 19,000 real tokens counted as
10,210,000, and the inflated figure was written to the best-day record. So the
device keeps four lanes, one per session it has heard from, and books each
value against the lane it fits (the one giving the smallest non-negative step,
capped at 500,000 per line). The same twenty alternating pairs now count 20,019
against a true 20,019. A value that fits no lane takes a lane as a fresh
baseline and contributes nothing, which is what a genuine restart looks like
too: the first turn of a restarted session is not claimed. A daily total above
100,000,000 tokens is never promoted to the best-day record, because that
record goes to flash and no protocol verb can clear it.

**`time` is local, not UTC.** The only question the device asks of it is whether
midnight has happened where the owner is sitting, and a device with no timezone
database cannot answer that from UTC. The helper sends `Date.now()` shifted by
its own `getTimezoneOffset()`, fresh every frame, so a laptop carried across a
timezone or through a daylight-saving change corrects itself with nothing stored
anywhere. The device keeps the value with the `millis()` it arrived at and
extrapolates, so a host that stops talking does not stop the day.

**A board that is never told the time still works.** There is simply no
midnight: the daily figures run from boot and the screen says `SINCE BOOT` where
it would otherwise say `TODAY`. The first clock a board is ever told is
*adopted* rather than treated as a new day, so a host that connects at lunchtime
does not throw away the morning the board already watched.

**What is persisted, and what deliberately is not.** Records only: the longest
single turn ever and the most tokens in one day, in NVS beside the owner name,
the face and the hop best, in the same `codexbuddy` namespace, so
`{"reset":"factory"}` takes them and `{"reset":"firstrun"}` does not. Flash is
touched only when a record actually moves, from a completed turn or from
midnight, never on a timer and never per line, which is the same discipline the
hop best already follows.

The **daily** figures are RAM and do not survive a reboot. That is a decision. A
reboot is not a midnight, and the board cannot know whether counters it loaded
belong to today or to a Tuesday three weeks ago; showing yesterday's number
under the word TODAY is worse than showing a zero.

**The milestones, and the thresholds.**

| Threshold | Fires | Reads |
|---|---|---|
| a completed turn of **5 minutes** or more | once, on the frame it completes | `LONG TURN 5:12` |
| a completed turn that beats the stored longest **and is at least 60 seconds** | once | `LONGEST YET 8:40` |
| tokens today crossing **250k, 500k, 1M, 2.5M, 5M, 10M** | once per rung per day | `TOKENS TODAY 1M` |

Five minutes is the point at which somebody has stopped watching and gone to do
something else, which is exactly when a quiet "that one took a while" is welcome
and an alarm would not be. The 60-second floor stops a virgin board celebrating
its first four-second turn as a personal best. The token ladder roughly doubles
rather than stepping on a fixed grid, because a Codex day is measured in
millions and a 100k grid would fire dozens of times before lunch; six rungs is
at most six flourishes in a very heavy day, hours apart, and the ladder
deliberately ends. Past ten million the device says nothing more.

A record supersedes the long-turn flourish, so one completed turn can never fire
two, and one line that crosses several token rungs fires only the highest. One
line can still cause both kinds (a `done` that both completes a long turn and
carries the token total past a rung); the token one is newer news and replaces
the turn one, which is a two-second pill turning into a different two-second
pill rather than two of them queueing up.

**Brief and tasteful, and the second word is the hard one.** A flourish is a
pill of text that fades in over 250ms, rising seven pixels as it arrives, then
sits **absolutely still** for a second and a half and fades out. 2200ms end to
end. The stillness is the design: this is an open-plan office and a thing that
moves in the corner of somebody else's eye for two seconds is a thing they will
ask to have unplugged. The entrance is the celebration; everything after it is
just legible.

The colours are the two on this device that do not mean "look at me now": the
`done` green for a turn and the ambient teal for tokens. Amber and red belong to
a question.

It is an overlay on the live and ambient compositions and nothing else. It lands
over the two text lines, which are the least urgent band of the panel. Every
modal screen outranks it, it does not draw at all while a prompt is pending, and
its clock runs underneath a message card rather than queueing: a flourish nobody
could see is missed rather than saved up to surprise somebody ten minutes later.

**It never hides a question.** Same rule as everything else here, and the stats
screen takes the harsher half of it: a pending prompt **closes** the screen the
way it ends the toy, rather than covering it the way it covers a message card.
Opening it over a pending prompt is refused from the button and from the wire.

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| a pending prompt (`waiting`) | the prompt |
| a message card | the card |
| the sign | the sign |
| the toy | the toy |
| anything else | the stats screen |

### Focus timer

A pomodoro on the two buttons, working with no host software at all. It is the
first feature here that is about the **person** rather than about the agent,
which is why it borrows the ambient teal rather than any state colour.

**Two taps from anywhere: button 1, button 1.** The first opens the numbers,
the second opens the timer. Then:

| On the timer screen | Button 1 | Button 2 |
|---|---|---|
| stopped, nothing spent | start | close. **Hold: next length** |
| running | pause | close |
| paused mid-run | resume. **Hold: reset** | close. **Hold: next length** |
| the finish is up | close it | close it |

The lengths are `5, 15, 25, 45` minutes, cycled by the hold and **stored in
NVS** beside the owner name and the face, so a board comes back on the length
its owner picked. The hold is refused while the clock is actually running, so
the gesture can never cost somebody a pomodoro; the refusal still eats the
hold, which stops the release that follows from also closing the screen.

The same four numbers, and one thing the buttons cannot do, over the wire:

```sh
printf '{"focus":"show"}\n'         > /dev/cu.usbmodemXXXX
printf '{"focusmins":45}\n'         > /dev/cu.usbmodemXXXX   # 1..180, clamped
printf '{"focus":"start"}\n'        > /dev/cu.usbmodemXXXX
printf '{"focus":"pause"}\n'        > /dev/cu.usbmodemXXXX
printf '{"focus":"reset"}\n'        > /dev/cu.usbmodemXXXX
printf '{"focus":"hide"}\n'         > /dev/cu.usbmodemXXXX   # the clock keeps running
```

Both fields are documented in `../docs/protocol.md` 2.13, both are optional, and
an older host that sends neither is unaffected. The shipped helper sends
neither.

**The screen closes itself after sixty seconds with nothing pressed, and only
while the clock is at rest** (`FOCUS_IDLE_MS`, and the `focusHide(nowMs,
"idle")` in `focusTick`). A running countdown is never taken off the panel by a
timeout, because somebody watching a clock is not idle. It exists because two
taps of button 1 from ambient land on a stopped `25:00`, and without this a
board reached by accident parked there until somebody pressed button 2. It
prints `focus: hide (idle) left=<m:ss> stopped` on the wire when it fires,
confirmed on the bench board 2026-09-08.

**The clock and the screen are two different things.** This is the distinction
the whole feature turns on.

- The clock runs whether or not anything about it is on the panel. It survives
  the screen closing, a message card, the sign, the game, the stats screen, a
  first-run replay and a pending question. **It is never refused.**
- The screen is a modal screen like every other one here, and it **is** refused
  while a prompt is pending, exactly as the toy and the stats screen are.

So `{"focus":"start"}` during an approval prompt starts a real countdown and
leaves the question on the panel. While the clock is running with its screen out
of sight, the ambient and live compositions carry a **two pixel hairline along
the bottom edge** that drains with it, dimmer for a paused run than a running
one. It is the smallest honest thing this device can draw, and it is
deliberately not a badge: design law 4 is the reason.

#### What wins when a timer is running and the agent starts waiting

**The question wins. Always.** The timer's screen yields the panel back to the
live view exactly as the message card does, the countdown keeps running
underneath, and the screen returns the instant the prompt is answered.

The justification is the one this firmware has already given five times, and
the only reason to restate it is that a timer is the first feature here with a
plausible claim to the contrary. It does not have one. A countdown is a thing
the owner can reconstruct from memory, from a phone, from the clock on the
wall; an approval prompt is a thing only this panel is showing, and covering it
is the single failure this device is built not to have. The timer also loses
nothing by yielding, because its clock never stopped.

**The finish is the one place the timer outranks something**, and the something
is the milestone flourish. A milestone that happens behind a modal screen is
missed on purpose: nobody wants yesterday's confetti. The end of a timer is not
news about the past. It is the one thing the owner explicitly asked this device
to tell them, and nothing else ever will, so it **waits**: its own thirty
seconds do not begin until it can actually be seen. That cannot ambush anybody
much later either, because the only thing that can hold it back is a prompt,
and answering the prompt is the moment the owner is looking straight at the
panel.

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| a pending prompt (`waiting`) | the prompt |
| a message card | the card |
| the sign | the sign |
| the toy | the toy |
| the stats screen | the stats screen |
| anything else | the timer |

In every one of those rows the countdown keeps running underneath, and in every
one of them the finish waits rather than being missed. The screen is the only
thing that ever loses.

#### On the panel

The header says `FOCUS` on the left and **what Codex is doing** on the right,
in that state's own colour: `AMBIENT`, `SLEEP`, `IDLE`, `BUSY` or `DONE`. That
is the coexistence in one line, and it is why this is a companion's timer and
not a kitchen timer: the screen belongs to the person and the colour still
belongs to the agent, so one glance answers both questions. `WAITING` never
appears up there, because a pending question takes the whole panel.

Under it, the remaining time in `Font7`, the library's 48px seven-segment face,
which is the largest digit set available and the only one that reads as a timer
rather than as text. Its glyph table is `0-9`, `:`, `.` and `-`, which is
exactly a countdown's alphabet. The digits are **dimmed while the clock is
stopped**, which answers "is this thing running" with no blinking at all. Then
a bar that drains, and a legend that names both buttons.

The backlight follows the ambient ladder rather than the live one, for the same
reason the sign does: a forty-five minute countdown can legitimately sit lit on
a desk, so it is properly lit for a minute and then settles one tier down for as
long as it lasts. The finish overrides that and runs at full for its half
minute, because it is the one thing here that has to be noticed.

#### The finish

The word `FOCUS` in the `done` green over `25 MINUTES DONE`, held for thirty
seconds, then gone and the timer is back at the top of its length ready for the
next one. It fades in over 400ms, breathes on the same 2400ms period the idle
and busy states already use, and fades out. No strobe and no siren: there is no
buzzer on this board, and design law 4 would forbid one if there were. Any press
takes it down early.

It says `FOCUS` and not `DONE` on purpose. `done` is a Codex state on this
device with a colour and a face of its own, and the end of a pomodoro is not it.

**Cost**, measured on the board with a `-DFPS_DEBUG` build on 2026-09-08, on
the `arc` face, worst of several 3-second windows, against the same 33ms
`FRAME_MS` budget as everything else:

| Scene | avg ms | max ms | fps |
|---|---|---|---|
| the timer screen, clock running | 13.38 | 13.53 | 30.3 |
| the timer screen, clock stopped | 13.60 | 13.73 | 30.3 |
| the finish | 13.84 | 13.97 | 30.3 |
| live underneath, 45m run hidden and **running** | 16.30 | 16.70 | 30.3 |
| live underneath, same run hidden and **paused** | 16.32 | 16.75 | 30.3 |

Three things that table says. The timer's own screen is in the same band as the
stats screen, well under half the budget: seven-segment digits are cheap
because the library draws them and there is no face, no ring and no comet on
that composition. The running clock is not more expensive than the stopped one.
And **the hairline is free**: the last two rows are the same board a few
seconds apart with only the clock's run state differing, and they are inside
each other's noise, which is what makes it safe to leave a run going while the
screen is elsewhere.

The base composition is dearer than any timer scene because it is the whole
face, and that is the composition being measured in those two rows, not the
timer. Every scene holds the locked 30.3fps.

### The button model, again

The last section on the buttons said there was no room left, and that the next
feature had to **replace** something in the table or take a mode. This one
replaced something, so here is the accounting.

The slack was that **brightness had two controls**. Button 2 is the dedicated
one, with no special cases; button 1 also cycled it, with a wake-first case in
front. The README's stated reason for keeping the second copy was a unit whose
GPIO 14 switch might be unreachable in a case. That is the thing this feature
spent.

| | button 1 (GPIO 0) | button 2 (GPIO 14) | both |
|---|---|---|---|
| tap | acknowledge a prompt, else **the stats screen** | brightness | do not disturb |
| hold | 2s: the toy | 800ms: next face | 5s: factory reset |

Four thresholds, exactly as before. No new gesture, no fifth hold, and the table
got **simpler** to say out loud: button 1 is "tell me something", button 2 is
the panel controls.

Three things keep the trade honest:

- **The wake is not lost.** Opening the stats screen wakes a panel the auto
  ladder had dimmed, exactly as acknowledging a prompt does. Poking a dim board
  with button 1 still lights it up; it now also shows you your day.
- **The button is never dead.** The one case where the numbers cannot be shown
  is a pending prompt, and there `statsEnter` refuses and the tap falls straight
  back to the old wake-and-cycle. Nothing about the ladder changed.
- **Brightness is at most one press away from anywhere**, because any press
  takes down whatever is on top and button 2 is the very next one.

What has not changed at all: the acknowledgement still fires on the press edge,
and the hold thresholds are the same three numbers.

#### And again, for the timer

The focus timer is the sixth screen on this device and it added **zero
gestures to the base compositions**. Here is that accounting, and the honest
part first: the old cross-cutting rule did not survive it.

The rule used to be "while anything modal is on the panel, a press takes it
down and does nothing else, with the toy as the single exception, because it is
played rather than read." Two exceptions would not be a rule any more, so it was
replaced rather than exempted from. The rule now is:

> **On any modal screen, button 2 leaves. Button 1 is that screen's own action,
> and where a screen has no action, it leaves too.**

That is one sentence, it covers every screen this device has, and it is
strictly simpler to say out loud than the old one plus its exception:

| Screen | Button 1 | Button 2 |
|---|---|---|
| the message card | dismiss (no action to have) | dismiss |
| the sign | take it down (no action to have) | take it down |
| the toy | jump | leave |
| the stats screen | on to the timer | leave |
| the timer | start / pause / resume, **hold** to reset | leave, **hold** for the length |
| the finish | close it | close it |

The base compositions are untouched. Same table, same four cells, same three
thresholds:

| | button 1 (GPIO 0) | button 2 (GPIO 14) | both |
|---|---|---|---|
| tap | acknowledge a prompt, else the stats screen | brightness | do not disturb |
| hold | 2s: the toy | 800ms: next face | 5s: factory reset |

Three things keep this trade honest as well:

- **Nothing new to learn to reach it.** The timer is behind a tap somebody
  already knows, on the screen that tap already opens, with the fork printed
  along the bottom of that screen.
- **No fifth threshold.** The two holds the timer screen adds are both at
  800ms, which is the number button 2's hold has always used, and they are
  unreachable anywhere else.
- **Button 2's hold means one thing everywhere.** Step a stored choice: the
  face on the base compositions, the length on the timer. Both persist to the
  same NVS namespace.

One behavioural nuance, written down because it is invisible and would
otherwise be a mystery in six months: on the timer screen **both taps resolve
on the release**, not the press, because both buttons also carry a hold there
and the press edge cannot yet tell which one it is. Every other modal screen
still acts on the press. Nobody can feel the difference; a tap is a tap.

#### The model is now full. What the next feature should do.

Not another hold, and not another fork off button 1 either. Three glance
screens behind one button is the most a person will walk without a map, and the
next one makes it four.

The coherent next step is a **chooser**, and it is the standard idiom for a
two-button device precisely because it stops costing gestures: hold button 1 to
open a list of the screens this device has, tap button 2 to move down it, tap
button 1 to pick. That is two gestures total, both already spelled by the rule
above (button 2 moves on, button 1 acts), and the feature after it costs one
row in a list rather than a gesture nobody can discover. It also gives the
stats screen its `PRESS TO CLOSE` back, since the fork would move into the
list.

It was not built here because it touches every screen's button handling and
this firmware is due in front of fourteen people. It is the shape of the next
change, not a nice-to-have, and anything that adds a seventh screen without it
should be pushed back.

**Cost**, measured on the board with a `-DFPS_DEBUG` build, worst of several
3-second windows, against the same 33ms `FRAME_MS` budget as everything else:

| Scene | avg ms | max ms | fps |
|---|---|---|---|
| the stats screen, over live | 13.84 | 13.94 | 30.3 |
| live `busy` with a milestone pill up | 15.88 | 16.12 | 30.3 |
| live `busy`, same build, for comparison | 15.30 | 15.46 | 30.3 |
| live `idle`, same build | 14.50 | 15.32 | 30.3 |
| ambient, no host, same build | 14.89 | 14.98 | 30.3 |

The stats screen is the **cheapest composition on the device**: no face, no
ring, no comet, just two rules and eleven short strings. The milestone pill
costs about **0.6ms** on top of whatever it is covering, which is one filled
rounded rect, one stroke and two strings. The worst frame measured anywhere in
this feature is 16.12ms against the 33ms `FRAME_MS` budget, and every scene
holds the locked 30.3fps, which is the cap and not a limit of the renderer.

Two honest gaps in that table. **Stats over ambient was not measured**,
because reaching it needs the button and nothing here can press one; it is the
same drawing with the ambient accent (two `mixColor` calls) and the drift
offsets added, so it is cost-identical within the noise of the measurement.
And **`ambient+mile` is unreachable in practice**: every milestone is caused by
a protocol line, and a protocol line puts the device in live mode. The branch
exists so that it cannot become a bug if that ever changes.

The device reports every figure on the wire, because nobody can read a panel
over USB and these are numbers rather than pictures:

```
{"stats":"on"}
stats: open (host) turns=3 tokens=1209000 longest=4s session=16s best_turn=0s best_day=1209000 day=today turn=none link=up
ok
{"state":"done"}
stats: turn 5:12 turns=4 tokens=1209000 longest=312s best=312s
milestone: LONGEST YET 5:12
ok
```

| Notice | When |
|---|---|
| `stats: open (button)` / `(host)` | the screen opened, carrying every figure it is about to show, plus `turn=running\|none` and `link=up\|down`, which are the two flags that explain a figure that looks wrong |
| `stats: close (<why>)` | it closed. `why` is `button`, `host`, `prompt`, `idle` or `firstrun` |
| `stats: turn <m:ss> turns=... longest=... best=...` | a turn completed and was counted |
| `stats: new day best_turn=... best_day=...` | local midnight passed and the daily figures were zeroed |
| `milestone: LONG TURN <m:ss>` / `LONGEST YET <m:ss>` / `TOKENS TODAY <n>` | a flourish fired |
| `err: stats refused, prompt pending` | `{"stats":"on"}` arrived over a pending question |
| `err: nvs open failed, records not persisted` | a record moved but could not be written |

Full behaviour and the field table are in `../docs/protocol.md` section 2.12.

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

## Fleet image hash

Every unit is flashed with the byte-identical image, so the whole fleet has one
hash. Rebuild from a clean tree and compare:

    cd firmware && rm -rf .pio && pio run
    shasum -a 256 .pio/build/tdisplays3/firmware.bin

A recipient who wants to check what is actually on their board can dump it and
compare against the same value:

    esptool.py --port /dev/cu.usbmodem<N> read_flash 0x10000 <size> dump.bin

The hash changes whenever the firmware changes, so record it at flashing time
alongside the fleet log rather than pinning a value in this file.
