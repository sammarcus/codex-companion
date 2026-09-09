# The default face

The thing on the panel is a creature, not a dashboard. It has two eyes, they
blink, and they change expression depending on what the session is doing.

This describes **`rounded`**, the default face and the one fourteen boards ship
on. It is not the only one: the drawing lives behind an interface, three designs
implement it, and which one a board draws is a runtime choice. The interface,
the geometry contract, the `face` protocol field and how to write a fourth are
in `FACES.md`.

The drawing is `src/FaceRounded.cpp`. The blink driver, the glance driver, the
state machine, the compositions and the text are in `src/main.cpp`. Nothing is a
bitmap: every eye is a rounded rect, an arc and a line, so the face scales
between ambient and live by changing four numbers, recolours by changing one,
and costs zero bytes of flash beyond the code that draws it.

## Why a face

The ring this device used to be was legible but anonymous. Fourteen people are
getting one of these on a desk that already has a monitor, a keyboard and a
plant, and a small glowing ring is furniture. A face is not: eye contact is the
one image a human reads across a room without deciding to.

It also fixes a problem the design spec called out. `docs/design-spec.md` notes
that the reference desk pets carry state in *pose*, and that recolouring a whole
ring per state was this project's substitute, "because a ring has no
pose/expression channel to lean on". The face gives that channel back. Colour is
now one of five signals instead of the only one, which is what lets `waiting`
shout and `sleep` whisper.

## Geometry

One eye is a rounded rect drawn with `fillSmoothRoundRect` (anti-aliased, so the
curve does not stair-step at this size). The corner radius is
`0.42 * min(w, h)`, and that ratio is the whole look: at 0.5 the eye is a
stadium and reads as a pill, and below about 0.3 it reads as a screen bezel.
0.42 is a soft rectangle.

A second, much smaller rounded rect sits in from the eye's top left corner,
coloured part of the way toward white. That is the highlight, and it is what
makes two rectangles look wet. It is skipped once the lid is low enough that it
would collide with the eye's own edge.

**A shut eye is never nothing.** It clamps to a 3px lid line, because a vanished
eye reads as a rendering fault and a line reads as a blink.

The ambient and live sizes are the shared `FaceGeometry` contract, and the table
lives in `FACES.md` part 2 so that every face reads one copy of it. Both eye
pairs sit well clear of the label line at y=122, including at the widest
`waiting` expression.

**The happy squint.** `done` replaces each eye with an upward bow. `fillArc`
angles are degrees with 0 at 3 o'clock increasing clockwise, so the top of a
circle is the slice that gives a `⌒`. Putting that circle's centre *below* the
eye is what makes the visible slice curve the right way, and the radius is
derived from the chord width so the bow is exactly as wide as the open eye it
replaces. The arithmetic is in `FaceRounded.cpp`.

**Eyebrows.** Two `drawWideLine` calls above the eyes. They exist for one state,
`waiting`, and they have one degree of freedom: `browTilt`, which drops the end
nearest the nose. Level and raised reads as alert. Dropped inward reads as "I am
still waiting". That is the entire escalation gesture, and it is the single
cheapest way to put an emotion on a face. Brows track the head, not the eyes:
they follow the burn-in drift and the `waiting` bounce, but not the gaze and not
the blink, because a brow that fell with the eyelid would look like the face was
melting.

## The blink

A metronome blink is the thing that reads as broken, so nothing about this is
periodic.

**Shape.** A real blink is asymmetric: the lid drops much faster than it lifts,
so the close accelerates over roughly two frames at 30fps and the open
decelerates over about twice as long. The close should be almost too fast to
see, which is exactly how a blink feels. `BLINK_CLOSE_MS` and `BLINK_OPEN_MS`
are the two numbers to move if it reads as a shutter.

**Count.** Each burst rolls once for a triple, then for a double, otherwise a
single, so roughly a quarter of blinks are multiples. The halves of a multiple
are timed from the end of the previous open.

**Interval.** Randomised per burst, from a window that depends on the state.
Ambient and idle sit at a relaxed human rate of a few seconds. `busy` blinks
less, because it is concentrating. `waiting` blinks about twice as often, which
reads as agitation. `sleep` and the `done` squint suppress it entirely, because
the eyes are already shut or already curved.

**The randomness is hardware.** `esp_random()` needs no seeding, and that
matters more than it sounds: `random()` without a seed is identical on every
boot of every board, and fourteen units blinking in lockstep on one table would
be the single most robot-like thing this device could do.

**Interaction with expression.** The blink factor multiplies the state's own lid
factor rather than replacing it, so a squinting `busy` face still blinks and a
wide `waiting` face still blinks, each from its own resting opening. That is the
`open` channel described in `FACES.md`.

## The glance

Ambient and idle only. Every few seconds the eyes look somewhere else, hold, and
come back. The magnitude is drawn from a range that never includes zero: a
scheduled glance that does not visibly go anywhere is a wasted beat. Movement is
a frame-rate independent exponential ease, so the eyes slide rather than snap.

Busy, waiting and done do not glance. A face that looks around while it is meant
to be concentrating reads as distracted, and one that looks away while demanding
an answer stops demanding it.

## Per-state expressions

All five protocol states are unchanged and so is everything that drives them.
The face is an additional read on the same state, not a new state machine.

**ambient.** Open, resting. Colour rides the existing 45 second three-stop drift
and the existing 6.5s brightness breathe. On top of that the eye height swells
slightly on the same period: two eyes holding exactly one shape between blinks
look painted on, and this is invisible as motion but is the difference between
resting and inert.

**sleep.** Shut. The lid line rides up and down and the eye narrows on the same
4s sine the ring already breathes on. That is the whole animation and it is
meant to be: a sleeping thing should be nearly still, but a genuinely frozen
panel is indistinguishable from a crashed one. `C_SLEEP` (#26314A) is nearly
invisible at the sleep backlight tier, so the lid is lifted toward white; a
closed eye still has to read as a closed eye and not as a blank screen.

**idle.** Open, level, `C_IDLE`, gently breathing. This is the state the device
spends most of its life in and it should look like it is just hanging around.

**busy.** Lids down to a concentrating squint, and the gaze tracks back and
forth across something only it can see. The horizontal and vertical scan periods
do not divide into each other, so the path never retraces itself and does not
read as a mechanism.

**waiting.** The hero state, aimed at one job: reading as "hey, you" from across
a room, where the text is illegible and the ring is a small amber dot. Eyes
wide, wider still once escalated. Eyebrows up and level, dropping toward the
nose once escalated. The whole head hops on every pulse peak, which is the part
that catches peripheral vision, because motion does and a brightness change on a
40px object mostly does not. Blinking roughly twice as often. Gaze locked
forward, never glancing away. Colour on the existing amber pulse, which already
speeds up with `tps` and already flips to `C_WAIT_HOT` at the 10s escalation.
Button 1 acknowledges: an acknowledged wait keeps the wide eyes, because a
prompt is still pending, but goes level-browed, steady, and stops hopping. That
is the face of something that has been told you know.

**done.** A happy squint held well past the 600ms ring flash, because a smile
that lasts exactly as long as a flash does not register as a smile. The head
hops once on arrival. Coming out of it, the blink machine is handed a
half-finished open, so the eyes *lift* out of the smile rather than cutting to
open, which is what a face actually does after it stops grinning.

## Composition

**Ambient** is the face centred, the owner name below it, the product name below
that, and no ring. The comet ring that used to be here is gone: the face is
148px wide and the comet was 104px across in the same place, shrinking either to
fit made both worse, and a ring around a face is a bullseye rather than a
portrait. The ring is not lost, it belongs to live mode now, where it carries
real numbers.

**Live** splits the top band in two: eyes on the left, ring on the right, both
text lines centred under both. The ring moved right and shrank, and nothing else
about it changed, because every stroke-width expression in `renderLive` is
written relative to `RING_R_OUT` and followed the shrink for free. No metric was
dropped. The two text lines are now centred on the panel rather than on the
ring, which is where they always visually were back when the ring was in the
middle.

## Frame cost

`FRAME_MS` is 33, a 30fps cap. Every scene on every face holds the locked
30.3fps, and the worst frame measured anywhere in the project is 17.97ms, 54% of
the budget. The full 21-scene table is in `FACES.md` part 6.

The dominant cost in every scene is the fixed ~13ms of `fillScreen` plus
`pushSprite` on a 320x170 16-bit sprite. Nothing drawn on top of that is close
to being the bottleneck, which is why there was room for a face at all. Ambient
got about 1.2ms *faster* when the face replaced the comet, because two
anti-aliased rounded rects are cheaper than the comet's six `fillArc` calls.

## What a human still has to look at

The serial port proved the protocol. It cannot prove any of this. The visual
checklist for the face lives in `docs/NEEDS-EYES.md` and it is the gate before
fourteen units get flashed.
