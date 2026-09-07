# The face

The thing on the panel is a creature, not a dashboard. It has two eyes, they
blink, and they change expression depending on what the session is doing.

Everything here lives in `src/main.cpp`. Nothing is a bitmap: every eye is a
rounded rect, an arc and a line, so the face scales between the two modes by
changing four numbers, recolours by changing one, and costs zero bytes of
flash beyond the code that draws it.

## 1. Why a face

The ring this device used to be was legible but anonymous. Fourteen people are
getting one of these on a desk they already share with a monitor, a keyboard
and a plant, and a small glowing ring is furniture. A face is not: eye contact
is the one image a human reads across a room without deciding to.

It also fixes the state problem the design spec called out. `docs/design-spec.md`
section 1.5 notes that the reference desk pets carry state in *pose*, and that
recolouring a whole ring per state was this project's substitute "because a
ring has no pose/expression channel to lean on". The face gives that channel
back. Colour is now one of five signals instead of the only one, which is
what makes `waiting` able to shout and `sleep` able to whisper.

## 2. Eye geometry

One eye is a rounded rect, `fillSmoothRoundRect` (anti-aliased, so the curve
does not stair-step at this size), with a corner radius of `0.42 * min(w, h)`.
That ratio is the whole look: at 0.5 the eye is a stadium and reads as a pill,
below about 0.3 it reads as a screen bezel. 0.42 is a soft rectangle.

A second, much smaller rounded rect sits at 17% / 16% in from the eye's top
left corner, sized 26% x 20% of the eye, coloured 55% of the way from the eye
colour to white. That is the highlight, and it is what makes two rectangles
look wet. It is skipped when the lid is below 55% open, where it would collide
with the eye's own edge.

A shut eye is never nothing. It clamps to a 3px lid line, because a vanished
eye reads as a rendering fault and a line reads as a blink.

| | ambient | live |
|---|---|---|
| face centre | (160, 62) | (96, 60) |
| one eye | 56 x 64 | 40 x 48 |
| gap between eyes | 36 | 26 |
| pair occupies | x 86..234, y 30..94 | x 43..149, y 36..84 |

Both sit well clear of the label line at y=122: 28px in ambient, and 20px even
at the widest expression (`waiting` escalated, 1.28x height).

### The happy squint

`done` replaces each eye with an upward bow. `fillArc` angles are degrees with
0 at 3 o'clock increasing clockwise, so 202..338 is the top of a circle. Put
that circle's centre *below* the eye and the visible slice is a `⌒`.

The chord across ±68 degrees of vertical is `2*r*sin(68°) = 1.854*r`, so
`r = w / 1.854` makes the bow exactly as wide as the open eye it replaces, and
offsetting the arc centre down by `0.6875*r` puts the bow's own vertical middle
back on the eye's centre line instead of leaving it hanging below. Stroke is
`0.30*r`.

### Eyebrows

Two `drawWideLine` calls, half-thickness 2.6px, spanning 92% of the eye's
width at `eyeCy - 0.86 * eyeH`. They exist for one state, `waiting`, and they
have one degree of freedom: `browTilt`, which drops the end nearest the nose by
up to 12px.

Level and raised reads as alert. Dropped inward reads as "I am still waiting".
That is the entire escalation gesture, and it is the single cheapest way to put
an emotion on a face.

Brows track the head, not the eyes: they follow the burn-in drift and the
`waiting` bounce, but not the gaze and not the blink. A brow that fell with the
eyelid would look like the face was melting.

## 3. The blink

A metronome blink is the thing that reads as broken, so nothing about this is
periodic.

**Shape.** A real blink is asymmetric: the lid drops much faster than it lifts.

| phase | duration | curve |
|---|---|---|
| closing | 70 ms | `1 - k²`, accelerating |
| shut | 34 ms | held |
| opening | 150 ms | `sin(k * π/2)`, decelerating |

At `FRAME_MS = 33` the close gets about two frames. That is deliberate: it
should be almost too fast to see, which is exactly how a blink feels.

**Count.** Each burst rolls once: 7% chance of a triple, another 17% of a
double (24% cumulative), otherwise a single. The halves of a multiple blink are
110ms apart, measured from the end of the previous open.

**Interval.** Randomised per burst with `esp_random()`, from a window that
depends on the state:

| state | window |
|---|---|
| ambient, idle, done-after-squint | 2.6 s .. 6.4 s |
| busy | 4.2 s .. 9.0 s (concentrating, blinks less) |
| waiting | 1.3 s .. 2.8 s (agitated, blinks more) |
| sleep | suppressed, the eyes are already shut |
| done, during the squint | suppressed, the eyes are already curved |

2.6-6.4s is a relaxed human, who blinks every 3 to 5 seconds.

`esp_random()` is the hardware RNG and needs no seeding, which matters more
than it sounds: `random()` without a seed is identical on every boot of every
board, and fourteen units blinking in lockstep on one table would be the single
most robot-like thing this device could do.

**Interaction with expression.** The blink factor multiplies the state's own
lid factor rather than replacing it, so a squinting `busy` face still blinks
and a wide `waiting` face still blinks, each from its own resting opening.

## 4. The glance

Ambient and idle only. Every 4.5 to 13 seconds the eyes look somewhere else,
hold for 0.7 to 1.6 seconds, and come back. The offset is up to 7px
horizontally and 3.5px vertically, and the magnitude is drawn from 0.55 to 1.00
of that, never zero: a scheduled glance that does not visibly go anywhere is a
wasted beat.

Movement is a frame-rate independent exponential ease with a 110ms time
constant, so the eyes slide rather than snap.

Busy, waiting and done do not glance. A face that looks around while it is
meant to be concentrating reads as distracted, and one that looks away while
demanding an answer stops demanding it.

## 5. Per-state expressions

All five protocol states are unchanged, and so is everything that drives them.
The face is an additional read on the same state, not a new state machine.

**ambient.** Open, resting. Colour rides the existing 45-second three-stop
drift (`C_IDLE` -> `C_AMB_TEAL` -> `C_BOOT`) and the existing 6.5s brightness
breathe, now 55% to 95%. On top of that the eye height swells by 5% on the same
6.5s period: two eyes holding exactly one shape between blinks look painted on,
and this is invisible as motion but is the difference between resting and
inert. Blinks and glances both on.

**sleep.** Shut. The lid line rides 3px up and down and the eye narrows to 90%
at the bottom of the same 4s sine the ring already breathes on. That is the
whole animation, and it is meant to be: a sleeping thing should be nearly
still, but a genuinely frozen panel is indistinguishable from a crashed one.
`C_SLEEP` (#26314A) is nearly invisible at the sleep backlight tier, so the lid
is lifted 42% toward white; a closed eye still has to read as a closed eye and
not as a blank screen.

**idle.** Open, level, `C_IDLE`, gently breathing on the existing 2.4s idle
period. Blinks at the resting rate, glances now and then. This is the state the
device spends most of its life in and it should look like it is just hanging
around.

**busy.** Lids down to 60%, a concentrating squint, and the gaze tracks back
and forth across something only it can see: ±6.5px on a 1700ms period
horizontally, ±2px on a 2600ms period vertically. The two periods do not divide
into each other, so the scan never retraces one fixed path and does not read as
a mechanism. Blinks less often.

**waiting.** The hero state. Everything here is aimed at one job: reading as
"hey, you" from across a room, where the text is illegible and the ring is a
small amber dot.

- eyes wide, 1.18x, and 1.28x plus 6% wider once escalated
- eyebrows up and level, dropping toward the nose once escalated
- the whole head hops up 3px (5px escalated) on every pulse peak. This is the
  part that catches peripheral vision: motion does, and a brightness change on
  a 40px object mostly does not
- blinking roughly twice as often as at rest, which reads as agitation
- gaze locked forward, never glancing away
- colour on the existing amber pulse, which already speeds up with `tps` and
  already flips to `C_WAIT_HOT` at the 10s escalation threshold

Button 1 still acknowledges. An acknowledged wait keeps the wide eyes, because
a prompt is still pending, but goes level-browed, steady, and stops hopping.
That is the face of something that has been told you know.

**done.** A happy squint held for 1600ms, well past the 600ms ring flash,
because a smile that lasts exactly as long as a flash does not register as a
smile. The head hops up 4px over the first 300ms. Coming out of it, the blink
machine is handed a half-finished open, so the eyes *lift* out of the smile
rather than cutting to open, which is what a face actually does after it stops
grinning.

## 6. Composition

### Ambient

Face centred, owner name below it, product name below that. No ring.

The comet ring is gone from ambient. The face is 148px wide and the comet was
104px across in the same place; shrinking either to fit made both worse, and a
ring around a face is a bullseye, not a portrait. The ring is not lost, it
belongs to live mode now, where it carries real numbers. Everything the comet
was in ambient to do (never a still frame, no error message, no dead-looking
object) the face does better, because its motion means something.

Ambient keeps everything else it had: the 45s colour drift, the 6.5s breathe,
the 700ms cross-fade in and out of live, the 60-second full-brightness hold
then the settle to the dim tier, the owner name, and the anti burn-in drift.

The drift is unchanged at 9px on a 97s period horizontally and 5px on a 61s
period vertically, and it now guards a bigger, more solid shape than the comet
was. Two things make that acceptable. The blink collapses each eye from 64px
tall to 3px several times a minute, which smears the horizontal edges far more
aggressively than the 5px vertical drift ever did, and the 5% height breathe
keeps them moving between blinks. The 24-character name cap is arithmetic from
`AMB_DRIFT_AX = 9`, which did not change, so it still holds.

### Live

The top band splits in two. Eyes on the left, ring on the right, both text
lines centred under both.

```
        eyes                       ring
     x 43..149                  x 198..290           y 14..106
                    +---- 49px of black ----+

                     LABEL          y=122, Font2, centred on the panel
                     sub line       y=146, Font0, centred on the panel
```

The ring moved from (160, 68) to (244, 60) and its radii shrank from 42..52 to
37..46. Nothing else about it changed: every stroke-width expression in
`renderLive` is written relative to `RING_R_OUT`, so the breathing busy stroke
and the pulsing waiting stroke followed the shrink for free. The centre readout
still sits in the hole, with `CENTER_MAX_W` now 66px instead of 76px, and the
existing Font4 / Font2 / Font0 step-down and truncation still apply.

No metric was dropped. The ring fraction, the centre text, the label and the
sub line are all still on screen, still fed by the same protocol fields.

The two text lines used to be centred on `RING_CX`. They are now centred on
`SCREEN_W / 2`, which is where they always visually were, since the ring used
to be in the middle.

## 7. Frame rate, measured

`FRAME_MS` is 33, so the target is a 30 fps cap. Measured on the real board
with a `-DFPS_DEBUG` build (a compile-time flag, never in a fleet image, since
the protocol channel has to stay clean), each row averaged over a 3-second
window:

| scene | before, avg / worst | after, avg / worst | fps |
|---|---|---|---|
| ambient | 15.94 / 16.31 ms | **14.64 / 14.88 ms** | 30.3 |
| live sleep | (not measured) | 14.05 / 14.28 ms | 30.3 |
| live idle | 14.04 / 14.30 ms | 14.12 / 15.13 ms | 30.3 |
| live busy | (not measured) | 15.14 / 15.41 ms | 30.3 |
| live waiting | 14.25 / 14.55 ms | 15.06 / 15.33 ms | 30.3 |
| live waiting, escalated | (same draw as above) | **16.87 / 17.24 ms** | 30.3 |
| live done | (not measured) | 15.27 / 17.17 ms | 30.3 |

Every scene holds a locked 30.3 fps, which is the `FRAME_MS` cap and not a
limit of the renderer. The worst frame anywhere is 17.24ms, 52% of the 33ms
budget.

The face costs about +0.9ms in idle and +2.6ms in escalated waiting, the most
expensive expression (wide eyes, both highlights, two eyebrows, a bounce).
Ambient got 1.2ms *faster*, because two anti-aliased rounded rects are cheaper
than the comet's six `fillArc` calls.

The dominant cost in every scene is the fixed ~13ms of `fillScreen` plus
`pushSprite` on a 320x170 16-bit sprite. Nothing drawn on top of that is close
to being the bottleneck, which is why there was room for a face at all.

RAM 21,320 bytes (6.5%), flash 369,997 bytes (5.6%). The eyes added nothing
measurable to either: the pre-face image was the same 6.5% / 5.6%.

## 8. What a human still has to look at

The serial port proved the protocol. It cannot prove any of this.

- [ ] **The face is cute.** This is the whole point and the one thing no tool
      here can check. Proportions, the gap between the eyes, the corner radius,
      the highlight position.
- [ ] **The blink looks organic, not mechanical.** Watch for a full minute. The
      close should be almost too fast to see and the open should feel soft. If
      it reads as a shutter, `BLINK_CLOSE_MS` and `BLINK_OPEN_MS` are the two
      numbers to move.
- [ ] **The double blink lands as charm, not as a glitch.** At 24% it happens
      roughly every fourth blink.
- [ ] **The glance reads as looking at something**, not as the picture sliding.
      7px may be too small to notice or too big to be subtle.
- [ ] **`waiting` reads as "hey, you" from across a room**, and is still not
      irritating to sit next to in an open-plan office. The hop is the new and
      riskiest part: 3px may be invisible, or it may be maddening.
- [ ] **The escalated brow tilt reads as urgency**, not as the face breaking.
- [ ] **`done` reads as a smile.** The arcs are the least certain drawing in
      here. Check the bow is not too thin and not sitting too low.
- [ ] **`busy` reads as concentrating**, not as shifty or nervous.
- [ ] **`sleep` reads as asleep**, and the lid line is actually visible at
      backlight duty 20.
- [ ] **Live mode is balanced.** Eyes left, ring right, text centred under
      both. The 49px gutter may be too tight or the ring may now look stranded.
- [ ] **The ambient face does not burn in.** Compare two photos an hour apart.
      If there is any hint of it, the lever is `AMB_DRIFT_AX` / `AMB_DRIFT_AY`.
- [ ] **It is still pleasant after a night.** A face that stares at you in a
      dark room is a different object from one that does it at noon.
