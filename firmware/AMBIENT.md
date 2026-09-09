# Ambient mode, the owner name, and the buttons

This device is a gift for people who may never plug it into a work machine and
may never run the host helper at all. So the host software is a bonus, not a
requirement: on USB power alone, with nothing talking to it, the unit has to be
an object worth leaving switched on indefinitely.

Everything here is in `src/main.cpp`.

## 1. Ambient mode

### When it is on

| Condition | Mode |
|---|---|
| No protocol line has ever been accepted since boot | ambient |
| A line was accepted less than 5 min ago | live |
| A line was accepted, and the last one was 5 min ago or more | ambient |

`hostEverSpoke` latches on the first accepted line and never clears, so the two
ambient paths ("never had a host" and "had one, lost it") land on the same
screen. `AMBIENT_AFTER_MS` is 300000.

The previous behaviour at five minutes was a forced `ST_SLEEP`, a nearly black
screen at backlight duty 20. On a desk with no host software that is what the
unit looked like permanently, which is the thing this change exists to fix.
`ST_SLEEP` is still reachable, but only when a host explicitly sends
`{"state":"sleep"}`.

### What it draws

A face, the owner's name, and the product name. With no name set: the product
name on the label line and the unit id below it. No ring. Full face geometry and
the blink model are in `EYES.md`.

On top of that, two slow cycles: a brightness breathe over the whole face on a
6500ms sine, and a colour drift over 45 seconds through three stops, `C_IDLE`
(#3AA0FF), `C_AMB_TEAL` (#2ED3C6) and `C_BOOT` (#5B8CFF). Two of those are
`docs/design-spec.md` blues. The teal is new: design-spec has no "no host
attached" state, so there was nothing in its table to borrow, and without a
third stop the drift reads as one flat blue for 45 seconds.

There is deliberately no "waiting for host", no error, and no still frame. The
eyes keep moving even at the trough of the breathe.

### Burn-in

Three things guard against it.

- **Drift.** The entire composition, face and both text lines, rides a two-axis
  sine: 9px amplitude on a 97s period horizontally, 5px on a 61s period
  vertically. The periods do not divide into each other, so the path does not
  retrace itself. `gOx`/`gOy` are applied by `drawRing`, `drawFace`, `drawText`
  and the ambient renderer alike.
- **The blink.** The face is a bigger, more solid shape than the comet it
  replaced, so the drift is doing more work than it used to. The blink is the
  other half: it collapses each eye from 64px tall to a 3px lid line several
  times a minute, which smears the horizontal edges far harder than the 5px
  vertical drift ever did.
- **Dimming.** Ambient holds the full backlight tier for its first 60 seconds
  (`AMBIENT_BRIGHT_MS`), so a freshly plugged-in unit looks like it is showing
  you something, then settles to duty 70 for as long as it stays in ambient. It
  never reaches the sleep tier of duty 20: staying readable is the entire point.
  Button 2 overrides this at any time.

The drift amplitudes did not change when the face landed, which is why the
24 character name cap below, which is arithmetic from `AMB_DRIFT_AX = 9`, still
holds.

### The handoff, both directions

One cross-fade mechanism serves boot-to-ambient, ambient-to-live and
live-to-ambient. On a mode flip, `modeChangeMs` is stamped and the accent colour
the outgoing mode last rendered is kept in `xfadeFrom`. For the next 700ms
(`MODE_XFADE_MS`) every colour that reaches the panel goes through `xf()`, which
blends from a dimmed `xfadeFrom` to the colour as authored, and all text scales
up from black. Over the same 700ms the drift offset winds up (entering ambient)
or unwinds to exactly zero (entering live), so the live layout always lands on
pixel-exact geometry.

`setup()` stamps `modeChangeMs` at the end of the boot hold with
`xfadeFrom = C_BOOT`, so the boot screen fades into whichever mode is correct
rather than cutting to it. A board plugged into an already-running host has
`hostEverSpoke` latched during the boot hold and fades straight into live. The
moment a valid line arrives in ambient, the very next `updateMode()` flips to
live, so the transition starts on the same tick as the ack.

## 2. The owner name

The recipients' names are not known at flash time, so the name is **runtime
state stored on the device**, not a build flag. All 14 boards flash from one
byte-identical image and there is no per-board build flag on the batch path.
`-DUNIT_ID` still exists but is unset, and at 0 the label is derived from the
last two bytes of the chip's efuse MAC and renders as `UNIT AB12`
(`snprintf(out, cap, "UNIT %02X%02X", mac[4], mac[5])`), rather than a `UNIT 00`
that would read like a real serial number on a batch of 14 identical boards.

```sh
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemXXXX
printf '{"name":""}\n'            > /dev/cu.usbmodemXXXX   # clear it
```

Stored in Arduino `Preferences`, namespace `codexbuddy`, key `owner`.
`loadOwnerName()` runs in `setup()` before the boot screen draws, so the first
frame after power on already shows the right name.

**Nothing is written unless the value actually changed.** `setOwnerName()`
compares the incoming value against the RAM mirror and returns early when they
match. That matters: NVS lives in the same flash the firmware does, and a host
that included `"name"` in its 2000ms heartbeat would otherwise rewrite the key
about 43,000 times a day. If NVS cannot be opened for writing, the name still
applies for this power cycle and the device prints
`err: nvs open failed, name not persisted` rather than pretending it stuck.

Precedence, resolved by `activeName()`, which every screen and the boot greeting
go through: the name in NVS, then `-DUNIT_NAME` as a build-time default only,
then no name at all. `UNIT_NAME` is never written to NVS, so a stored name
always wins and clearing the stored name falls back to the baked default rather
than to nothing. The unit id stays on the boot screen even when a name is set,
so assembly-day identification never depends on remembering who got which name.

### The 24 character cap, and how it was measured

That number was measured on the board, not estimated. A temporary probe build
walked ASCII 32..126 through `textWidth()` with `fonts::Font2` at text size 1 on
the real panel and reported `fontHeight 16` and a widest glyph of `M` at
**10px** advance. (`W` is not the widest in this font, which is why the probe
scanned the whole range instead of assuming.) The ambient headline is
centre-datum and rides the burn-in drift, whose horizontal amplitude is 9px, so
the width that can never touch an edge is `320 - 2*9 = 302px`, which is 30
characters of the worst-case glyph. 24 is that ceiling with a deliberate margin:
240px worst case, 31px of clear panel each side at the drift extreme.

The cap is enforced **after** the sanitiser, which keeps printable ASCII
(0x20..0x7E) and drops everything else. That is what makes 24 a number about
characters rather than about bytes: the clamp is a byte loop, so before the
sanitiser a name written with real accents stored at half the documented length,
and one that hit the cap mid-sequence stored a dangling UTF-8 lead byte that the
panel and the boot greeting both carried. The built-in LovyanGFX bitmap fonts
have no glyphs outside that range, so an accent has nothing honest to draw
either way; it is now dropped visibly instead of mangled silently. A name with
nothing printable in it is refused rather than being read as a clear.

### The greeting carries the name

```
hello tdisplay-s3 v1 name="Alex Rivera"
```

The `hello tdisplay-s3 v1` prefix is byte-for-byte what it was. Two programs
match on it as a substring, `cmdDoctor` in `helper/codex-companion.js`
(`/hello\s+tdisplay-s3/i`) and `verify_hello` in `tools/flash-all.sh`, so the
suffix is invisible to both. `doctor` also pulls the name back out with
`/name="([^"]*)"/` and prints it as `board name`. The suffix exists so a host
can read back what a `name` line actually did, and so persistence across a power
cycle is provable over the wire instead of by eye. It is always quoted, so an
unnamed board is an unambiguous `name=""`.

The helper does not use the greeting to *find* the board. Discovery is
`readIoreg` / `parseIoreg` / `choosePort`, keyed on `ESP_VENDOR_ID` `0x303a` and
`ESP_PRODUCT_ID` `0x1001`; the greeting is only a liveness check once a port has
been chosen.

### One serial-port note that is easy to lose

`platformio.ini` sets `-DCORE_DEBUG_LEVEL=0`. The Arduino `Preferences` wrapper
calls `log_e()` when a namespace or key is missing, which is the state of every
board that has not been named yet, and on this target that log goes out of the
same USB CDC the protocol runs on. Two stray `[E][Preferences.cpp:...]
NOT_FOUND` lines per boot in the host's line stream is not acceptable for a
device whose serial port is a protocol channel. `esp_log_level_set()` does not
reach them: at this log level the Arduino HAL macros call `log_printf` directly
and never go through an esp_log tag
(`cores/esp32/esp32-hal-log.h:159`). Compiling them out is the fix. The
firmware's own `err:` lines are plain `Serial.println` and are unaffected.

## 3. The buttons

The full current model, including every hold threshold and the cross-cutting
modal rule, is in `README.md` under "The buttons". This section covers only the
two behaviours ambient mode itself depends on.

**Button 1, GPIO 0 (the BOOT button).** A press while the rendered state is
`waiting`, in live mode and not already acknowledged, sets `waitAcked`: the ring
holds a steady amber at a constant level with no breathe, and the 10s escalation
to red is suppressed for as long as the ack stands. The colour still says a
prompt is pending, it just stops nagging. If the auto ladder had dimmed the
panel, the ack also wakes it to full, since an ack is an interaction. That still
fires on the **press** edge, so it is instant.

The ack clears on the next state change, both ways it can happen: the parser
clears it whenever an accepted line moves `st.state`, and the renderer clears it
whenever the effective state is no longer `waiting`, which covers the 30s
staleness fallback pulling the display off `waiting` with no line arriving. The
next `waiting` therefore starts impatient again.

With nothing to acknowledge, the same button opens the stats screen, on the
release edge. It still wakes a dimmed panel, so the "poke the dim board and it
lights up" gesture survives. The brightness fallback survives too in the one
case the numbers cannot be shown: a second press during an already-acknowledged
prompt is refused by `statsEnter` and falls straight through to the old
wake-and-cycle. The button is never a dead key.

**Button 2, GPIO 14.** `INPUT_PULLUP`, active low. One press is one brightness
step, and unlike button 1 it never wakes-then-cycles, which is what makes it
predictable as the dedicated brightness control. It sets the manual pin, so a
hand-picked level survives a live data stream until the auto tier itself
changes. Pin 14 is from `docs/hardware-recon.md` line 34, which cites
`vendor/T-Display-S3/examples/factory/pin_config.h:53`, and the `INPUT_PULLUP`
wiring matches every vendor example that reads it.

**Both buttons tapped together** raises the DO NOT DISTURB sign, and any single
press takes it down. It is the one gesture on this device aimed at somebody else
in the room, and the only one that needs no host and no explanation. Two
properties are deliberate. It is **resolved on the first release**, not at its
threshold, because the same two switches held five seconds longer are the
factory reset and putting a sign up on the way to wiping a board is a surprise
the gesture never asked for. And entry and exit are **not symmetric**: two
thumbs to raise, one finger to lower. A stray press must never plant a sign on
somebody's desk, and a mode with no obvious way out is the worse of the two
failures by a distance (see `docs/prior-art.md` on the status light that latches
amber forever because no event exists to clear it).

## 4. What a human still has to look at

The serial port proved the protocol and the persistence. Every visual claim
about ambient mode, the transitions, the name on the panel and the buttons is
unverified and lives in `docs/NEEDS-EYES.md`, which is the gate before fourteen
units get flashed.
