# Ambient mode, personalization, and the buttons

This device is a gift for people who may never plug it into a work machine and
may never run the host helper at all. So the host software is now a bonus, not
a requirement: on USB power alone, with nothing talking to it, the unit has to
be an object worth leaving switched on indefinitely.

Everything below lives in `src/main.cpp`. `tools/units.txt` and
`tools/flash-all.sh` carry the per-unit names.

## 1. Ambient mode

### When it is on

| Condition | Mode |
|---|---|
| No protocol line has ever been accepted since boot | ambient |
| A line was accepted, and it was less than 5 min ago | live |
| A line was accepted, and the last one was 5 min ago or more | ambient |

`hostEverSpoke` latches on the first accepted line and never clears, so the two
ambient paths are "never had a host" and "had one, lost it". Both land on the
same screen. `AMBIENT_AFTER_MS` is 300000.

The previous behaviour at 5 minutes was a forced `ST_SLEEP`: a nearly black
screen at backlight duty 20. On a desk with no host software that is what the
unit looked like permanently, which is the thing this change exists to fix.
`ST_SLEEP` is still reachable, but only because a host explicitly sends
`{"state":"sleep"}`.

### What it draws

Composition, all of it moving, none of it an error message:

- A faint full ring track at 70% of `C_TRACK`, so the shape still reads as a
  ring rather than a lone floating arc.
- A comet: five arc segments totalling 76 degrees, each one 19% dimmer than the
  one ahead of it, drawn with half a degree of overlap so no hairline of track
  colour shows between them. It makes one lap every 24 seconds, about 15
  degrees per second, which reads as drifting rather than spinning.
- A brightness breathe over the whole comet, 6500ms, sine, 45% to 90%.
- A colour drift over 45 seconds through three stops: `C_IDLE` (#3AA0FF),
  `C_AMB_TEAL` (#2ED3C6), `C_BOOT` (#5B8CFF), then back. Two of those are
  design-spec 2.3 blues. The teal is new: design-spec has no "no host attached"
  state, so there was nothing in its table to borrow, and without a third stop
  the drift reads as one flat blue for 45 seconds.
- Two text lines under the ring. With a name baked in: the owner's name on the
  label line in Font2, the product name below it in Font0. Without one: the
  product name on the label line, the unit id below it.

There is deliberately no centre text, no "waiting for host", no error, and no
still frame. The comet keeps moving even at the trough of the breathe.

### Burn-in

Two things guard against it.

- **Drift.** The entire composition, ring and both text lines, rides a
  two-axis sine: 9px amplitude on a 97s period horizontally, 5px on a 61s
  period vertically. The periods do not divide into each other, so the path
  does not retrace itself, and every text edge is smeared across several pixels
  within a couple of minutes. `gOx`/`gOy` are applied by `drawRing`,
  `drawText`, and the ambient renderer alike.
- **Dimming.** Ambient holds the full backlight tier for its first 60 seconds
  (`AMBIENT_BRIGHT_MS`), so a freshly plugged-in unit looks like it is showing
  you something, then settles to the dim tier (duty 70) for as long as it stays
  in ambient. It never reaches the sleep tier (duty 20): staying readable is
  the entire point. Button 2 overrides this at any time.

### The handoff, both directions

One cross-fade mechanism serves the boot-to-ambient, ambient-to-live, and
live-to-ambient transitions. On a mode flip, `modeChangeMs` is stamped and the
accent colour the outgoing mode last rendered is kept in `xfadeFrom`. For the
next 700ms (`MODE_XFADE_MS`) every colour that reaches the panel is run through
`xf()`, which blends from a dimmed `xfadeFrom` to the colour as authored, and
all text is scaled from black. Over the same 700ms the drift offset winds up
(entering ambient) or unwinds to exactly zero (entering live), so the live
layout still lands on the pixel-exact geometry of design-spec 2.1.

`setup()` stamps `modeChangeMs` at the end of the boot hold with
`xfadeFrom = C_BOOT`, so the boot screen fades into whichever mode is correct
rather than cutting to it. A board plugged into an already-running host has
`hostEverSpoke` latched during the boot hold, so it fades straight into live.

The moment a valid line arrives in ambient, `hostEverSpoke` is set and the very
next `updateMode()` flips to live, so the transition starts on the same tick as
the ack.

## 2. Personalization

`-DUNIT_NAME` is a new compile-time flag alongside the existing `-DUNIT_ID`.
The `UNIT_ID` behaviour is unchanged: still unset in `platformio.ini`, still
defaulted to 0 in `main.cpp`, and 0 still renders as `UNIT -- <MAC suffix>`
rather than a `UNIT 00` that would read like a real serial number.

```sh
PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex Rivera\""' pio run
```

The quotes are part of the macro body, and PlatformIO shlex-splits the
environment variable, which is why the inner pair has to be backslash-escaped.
`tools/flash-all.sh` does that quoting for you.

Unset, or set to an empty string, is a supported configuration, not an error:

| | Boot screen | Ambient screen |
|---|---|---|
| Name set | `codex companion` / **Alex Rivera** / `UNIT 03` | **Alex Rivera** / `codex companion` |
| No name | `codex companion` / `UNIT 03` | `codex companion` / `UNIT 03` |

The unit id is still on the boot screen even when a name is set, so
assembly-day identification never depends on remembering who got which name.

### `tools/units.txt`

One name per line. Line N is unit N, counting only non-comment lines, so the
first non-comment line is unit 1. Blank lines count and hold a slot open, which
is how you leave a single unit unnamed without renumbering everyone below it.
The file ships with 14 placeholder names (`Placeholder 01` through
`Placeholder 14`); `flash-all.sh` prints a warning to stderr and writes
`placeholder-name` into `tools/fleet-log.tsv` if it flashes one, so a forgotten
edit is loud rather than silent.

Backslashes and double quotes are stripped from a name before it reaches the
compiler. Names are drawn with the built-in LovyanGFX bitmap fonts, which have
no glyphs beyond ASCII, so non-ASCII will not render.

### `tools/flash-all.sh`

Unchanged behaviour, all still in place: it matches the board by USB hwid
`303A:1001` rather than by first-`/dev/cu.usbmodem*` (there were two other USB
serial devices attached to this machine during testing, so that filter earns
its keep), removes the stale `main.cpp.o` so a build cannot reuse the previous
unit's flags, uploads, and verifies by reading `hello tdisplay-s3` back off the
port before logging the unit as good.

New: it reads the name for each unit from `tools/units.txt`, echoes it before
building, appends `-DUNIT_NAME` to the build flags when there is one, and adds
two columns to `tools/fleet-log.tsv` (the name, and `ok` / `placeholder-name` /
`no-greeting`). `NAMES_FILE=/some/other/file` overrides the source. A missing
names file or a blank slot is not an error; that board simply builds with no
`-DUNIT_NAME`.

## 3. Buttons

### Button 1, GPIO 0 (the BOOT button): acknowledge, then brightness

New: a press while the rendered state is `waiting`, and the device is in live
mode and not already acknowledged, sets `waitAcked`. The impatient pulse stops:
the ring holds a steady `C_WAIT` amber at a constant 62% level, constant stroke
width, no breathe, and the 10s escalation to red is suppressed for as long as
the ack stands. The colour still says a prompt is pending; it just stops
nagging. If the auto ladder had dimmed the panel, the ack also wakes it to full
brightness, since an ack is an interaction.

The ack clears on the next state change, in both of the ways a state change can
happen: the parser clears it whenever an accepted line moves `st.state`, and
the renderer clears it whenever the effective state is no longer `waiting`,
which covers the 30s staleness fallback pulling the display off `waiting`
without any line arriving. The next `waiting` therefore starts impatient again.

Everything button 1 did before is intact. A press when there is nothing to
acknowledge, and a second press when a prompt is already acknowledged, both
fall through to the original behaviour: wake to full brightness if the auto
ladder had dimmed the panel, otherwise step the brightness 255 -> 160 -> 70 ->
20 -> 255. The button is never a dead key.

### Button 2, GPIO 14: brightness

Previously unused. Now `INPUT_PULLUP`, active low, same debounce, and it does
exactly one thing: one press is one brightness step. Unlike button 1 it never
wakes-then-cycles, which is what makes it predictable as the dedicated
brightness control. It sets the manual pin, so a hand-picked level survives a
live data stream until the auto tier itself changes.

Pin 14 is from `docs/hardware-recon.md` line 34, which cites
`vendor/T-Display-S3/examples/factory/pin_config.h:53`, and it is already
`#define`d in `src/LGFX_TDisplayS3.hpp`. The `INPUT_PULLUP` wiring matches
every vendor example that reads it (`T-Display-S3-Queue`, `BLE-Sender`,
`Piano-Debug`).

## 4. What a human still has to look at

I drove this board over the serial port and can only report what that port
proved. Every visual claim below is unverified by me and needs an eye on the
panel.

Ambient:

- [ ] The comet reads as calm and pleasant, not as a loading spinner or a
      fault indicator. 24s per lap is a guess at "calm"; it may want to be
      slower.
- [ ] The colour drift is perceptible over 45s without being distracting, and
      the teal stop does not look out of family with the two blues.
- [ ] The five comet segments read as one object with a tail, with no visible
      seams between the arcs.
- [ ] The owner name is legible at desk distance in Font2, and a long name
      (say 20 characters) does not run off the 320px panel. Nothing truncates
      the ambient name in firmware.
- [ ] The drift is invisible while you watch it, and visible if you compare
      two photos a minute apart.
- [ ] The dim step at 60 seconds is a settle, not a "did it just break".
      Confirm duty 70 is still comfortably readable in a lit office.
- [ ] It still looks alive after an hour, and after a night.

Transitions:

- [ ] Boot screen to ambient is a fade, not a cut.
- [ ] Ambient to live on the first protocol line is smooth, and the live
      layout lands centred, with no residual drift offset.
- [ ] Live back to ambient after 5 minutes of silence is a fade, and the
      screen never goes dark on the way.

Personalization:

- [ ] Boot screen with a name: three lines, correctly spaced, nothing
      overlapping at y=70 / y=106 / y=132.
- [ ] Boot screen without a name: two lines, unchanged from before.

Buttons (I cannot press them):

- [ ] In `waiting`, one press of button 1 visibly quiets the pulse to a steady
      amber ring.
- [ ] An acknowledged wait does not turn red at 10s.
- [ ] The ack drops on the next state change and the following `waiting`
      pulses again.
- [ ] A second press of button 1 during an acknowledged wait steps the
      brightness rather than doing nothing.
- [ ] Button 2 steps the brightness one tier per press, all four tiers
      reachable in a loop.
- [ ] Neither button is fouled by the case.

Live states, which I confirmed only as protocol acks:

- [ ] `sleep`, `idle`, `busy`, `waiting`, `done` each look right on the panel,
      and `done` still flashes once and cross-fades into idle.
