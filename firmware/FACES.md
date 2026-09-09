# Faces

The face is the whole product. It has been redrawn several times already and it
will be redrawn again, so it is not part of the renderer: it lives behind a
small interface, three designs implement it, and which one a board shows is
runtime state stored next to the owner name.

Swapping a face costs a protocol line or a long press. Writing a new one costs
one file and three lines in a registry. Neither costs a reflash of the fleet.

| File | What |
|---|---|
| `src/Face.hpp` | the interface: `class Face`, `FaceFrame`, `FaceGeometry`, `FaceCanvas`, the registry declarations |
| `src/Faces.cpp` | the registry, the cycle order, and the compile-time default |
| `src/FaceRounded.cpp` | **`rounded`**, the default. Soft rectangles with a wet highlight |
| `src/FaceBear.cpp` | **`bear`**, a warm cream creature whose mood is in its mouth |
| `src/FaceArc.cpp` | **`arc`**, two tapering hand-drawn strokes |
| `src/main.cpp` | everything that is not a face: the drivers, the states, the compositions, the text |

## 1. The interface

```cpp
class Face {
 public:
  virtual const char* name() const = 0;         // wire name, lowercase, <= 15
  virtual const char* description() const = 0;  // one line, for humans
  virtual void init() {}                        // once, from setup()
  virtual void draw(FaceCanvas& c, const FaceFrame& f) = 0;
};
```

`draw` is called once per rendered frame for the active face only, after the
ring and before the text. That is the entire contract. A face has no update
call, no state that has to survive a swap, and no way to reach anything in
`main.cpp`: swapping faces mid-animation is a pointer change.

```cpp
struct FaceFrame {
  uint32_t nowMs, dtMs, stateAgeMs;
  StateId  state;                 // the EFFECTIVE state, after staleness
  bool     ambient, escalated, acked;
  float    open;                  // 0 shut, 1 rest, >1 wide
  float    pulse;                 // 0..1, the ring's own breathe this frame
  float    gazeX, gazeY;          // px, from the shared glance driver
  uint16_t color;                 // the accent, already levelled by the state
  FaceGeometry geom;
};
```

Two of those deserve their own paragraph.

**`open` is the whole blink channel, and a face never computes it.** It arrives
already multiplied: the blink driver's factor times the state's own lid
expression, so a `busy` squint arrives as 0.60 and a blink through that squint
arrives as 0.60 falling to 0. A face that decides when to blink is the specific
bug this interface exists to prevent, and the failure mode is concrete: fourteen
units on one table blinking in lockstep, or in three different rhythms depending
on which face is loaded.

**`pulse` is the only motion channel that is not derivable from `nowMs`.** The
`waiting` pulse period moves with `tps` and its phase is accumulated outside, so
anything a face animates on the beat (the bear's hop, the rounded face's head
bounce) has to ride `pulse` to land on the same frame the ring brightens.
Everything else a face wants to animate it can compute from `nowMs`.

`stateAgeMs` is measured from the edge that matters for each state: the `done`
arrival for `ST_DONE`, the `waiting` arrival for `ST_WAITING`, the last state
change otherwise.

**What stays outside, permanently.** The protocol parser and the state machine,
the staleness timeouts, the blink driver, the glance driver, the mode
cross-fade, the anti burn-in drift, the ambient and live compositions, the ring,
the backlight ladder, and all text layout. If a face wants one of those it is
asking for the wrong thing.

## 2. The geometry contract

`FaceGeometry` describes the same space twice, and a face uses whichever half
suits it.

```cpp
struct FaceGeometry {
  int cx, cy;              // centre of the face
  int eyeW, eyeH, gap;     // one eye, and the black between the pair
  int boxX, boxY, boxW, boxH;   // the whole rectangle the face owns
};
```

**The eye-pair convention.** The pair straddles `cx` with `gap` px between two
`eyeW x eyeH` boxes, so it is `2*eyeW + gap` wide and `eyeH` tall at full open.
A face built from two eyes should size itself off these and inherit the
ambient/live difference for free.

**The box.** For a face that is a creature rather than a pair of eyes. Nothing
else is ever drawn inside it, so a face may use every pixel; it is the region
above the two text lines, so drawing past it lands on the label.

| | ambient | live |
|---|---|---|
| `cx, cy` | 160, 62 | 96, 60 |
| `eyeW x eyeH` | 56 x 64 | 40 x 48 |
| `gap` | 36 | 26 |
| pair spans | 148px, x 86..234 | 106px, x 43..149 |
| `box` | x 0..320, y 2..118 | x 0..192, y 2..118 |

The live box stops at 192 because the ring's left edge is at
`RING_CX - RING_R_OUT` = 198. Both boxes stop at 118 because `LABEL_Y` is 122.

**Three rules, and they are not negotiable.**

1. **The burn-in drift is already baked into `cx`, `cy`, `boxX` and `boxY`.** A
   face must never add it and must never read the drift globals. That is what
   lets ambient wander by 9px while live lands on pixel-exact geometry.
2. **Run every colour through `c.tint()`, including your own palette.** That is
   the ambient/live cross-fade. It is the identity function once a fade has
   settled, and a face that skips it will not fade with the rest of the screen.
   `bear` tints its cream and its ink, not just the accent it is handed.
3. **Stay inside the box.** Clamp anything that moves. `bear` clamps its own hop
   so the ears cannot leave the top of the box, rather than trusting the
   arithmetic to come out right at every scale.

## 3. Writing a new face

1. Copy `src/FaceRounded.cpp`. It is the shortest of the three and the one whose
   channels map most directly onto the frame.
2. Rename the class and change `name()` and `description()`. The name is the
   wire name, so keep it short and lowercase.
3. Delete the drawing and write yours. Read `open`, `pulse`, `gazeX/Y`, `color`
   and `geom`; ignore anything you do not want. A face that ignores `state`
   entirely is legal and will simply look the same in all five.
4. Export it: `Face* faceMine() { return &gMine; }` at the bottom, outside the
   anonymous namespace.
5. Register it in `src/Faces.cpp`: declare `Face* faceMine();`, widen the
   `gFaces` array, and add one line to `ensure()`. The array's order is the
   button-cycle order.
6. `pio run`, flash, and hold button 2 until it comes round.

Nothing else changes. Not the protocol, not the NVS layout, not `main.cpp`.

**Two things that will bite, both learned the hard way here.**

- **`powf(sinf(M_PI * u), k)` is NaN at `u = 1`.** `sinf(M_PI)` is `-8.7e-8`,
  not zero, and a negative base with a fractional exponent is NaN. A NaN half
  width passes every `< minimum` test, reaches `lroundf` as garbage and hands
  LovyanGFX a radius that never returns: the board greets and then goes
  permanently silent. Clamp the base, and clamp the radius at both ends.
- **Anti-aliased primitives are not free.** `drawWideLine` is a per-pixel float
  job over its own bounding box. The `arc` face drawn as 88 wedges cost 24ms a
  frame against a 33ms budget that already spends 14ms on `fillScreen` and
  `pushSprite`. The same stroke stamped as filled discs costs 3ms. The general
  form recurs on this hardware: the cheap-looking call is often the expensive
  one, so measure with `-DFPS_DEBUG` before assuming.

## 4. Choosing a face

```json
{"face":"bear"}
```

The wire name of a registered face, case insensitive. Applied on the very next
rendered frame, mid-animation, no reboot. Persisted to NVS, so like `name` its
effect outlives the power cycle. A name that is not registered makes the **whole
line malformed**, exactly as an unknown `state` does: dropped silently, with no
`ok` and no partial application of the line's other fields. `face` and `state`
are both validated before anything on the line is written. Writes are
idempotent: `setFaceIndex` returns early when the value is already active.

**The device-to-host protocol is unchanged.** The greeting is still exactly
`hello tdisplay-s3 v1 name="<stored name>"`, byte for byte, and there is still
exactly one `ok` per accepted line. The active face is deliberately not on the
wire: two other programs match on that greeting and neither should have to learn
anything new.

**The button.** Hold button 2 (GPIO 14) for 800ms and the face steps to the next
one in registry order, and the choice is stored. It is on button 2 because
comparing faces is a bench activity done with the board in your hand, and
because there is no other way to do it without a host. A tap is still exactly
one brightness step. While button 1 is also down the cycle is suppressed,
because that combination is on its way to the five second factory reset.

**Where it is kept.** `Preferences`, namespace `codexbuddy`, key `face`,
alongside `owner`. The **wire name** is stored, not an index, so the registry
can be reordered without silently repointing boards that are already set.
Precedence at boot: the stored name if it is still registered, then
`DEFAULT_FACE`, then the first registered face, so a misspelled
`-DDEFAULT_FACE` cannot leave a board with nothing to draw.

```sh
PLATFORMIO_BUILD_FLAGS='-DDEFAULT_FACE="\"bear\""' pio run
```

`DEFAULT_FACE` is `"rounded"` and is not set in `platformio.ini`, so all
fourteen fleet boards come out of the box on the rounded face. Setting it is a
per-image change and gives that image its own hash, exactly like `-DUNIT_ID`.

## 5. The three faces

**`rounded`** (default) is the original, moved into the interface unchanged:
every constant, ratio and easing is the one that was in `main.cpp`. Full
geometry, the blink model and the per-state expressions are in `EYES.md`. What
changed in the move is only where the per-state expression is decided: `widthK`,
`bounceY`, `happy`, `brows` and `browTilt` are this face's private vocabulary
now. The lid expression stayed outside, because "how open are the eyes" is a
question the composition answers for every face.

**`bear`** (Milkbun) is a creature rather than a pair of eyes: one big rounded
body wider than tall, no neck, tiny dot eyes set far apart, a mouth smaller than
one eye, permanent blush, stubby nub limbs, no outlines. It earns its place next
to `rounded` by using almost none of the same channels. `open` flattens a dot
into a bar instead of closing a rectangle. The accent colour never touches the
body: the fur is cream and the eyes are ink, and `color` only paints the blush
and the little state sparks. The wide-eyed `waiting` read is a hop with a
landing squash rather than a lid change, riding `pulse` so it leaves the ground
on the ring's beat. It is the one face that cannot grow the full 1.33x between
modes, because the other two grow their eyes inside the box and this creature
*is* the box, with the label line as the ceiling.

**`arc`** (Nimbus) is two curved strokes that swell at the belly and taper into
round caps, with a slow bob, a highlight that drifts along the stroke and one
sparkle orbiting off the right eye. It is the third implementation and the one
that proves the interface is not shaped around the other two: no rectangles, no
lids, no fixed eye box, and it reads `open` as **curvature** rather than as
height. A shut eye is a flat stroke with a slight downward bow that fattens as
it flattens, so a blink never thins into a hairline. There is no polygon
primitive here, so the ribbon is walked and a filled disc of the local half
width is stamped at each sample: the union is the stroke, with round caps and
joins for free and no seam anywhere.

Both `bear` and `arc` were ported from approved browser sketches that were never
committed here (`eyes-bear.js`, `eyes-arc.js`, out of tree), by scale rather
than by copy: every length is the JavaScript's own number times a factor derived
from the box.

## 6. Measured cost

All twenty-one scenes, on the real board, with a `-DFPS_DEBUG` build, each cell
the worst frame in milliseconds across several 3-second windows.

| face | sleep | idle | busy | waiting | escalated | done | ambient |
|---|---|---|---|---|---|---|---|
| `rounded` | 14.52 | 15.40 | 15.43 | 17.04 | **17.26** | 15.39 | 14.96 |
| `bear` | 16.48 | 16.38 | 16.77 | 16.53 | 16.53 | 16.52 | 15.39 |
| `arc` | 17.68 | 17.06 | 17.64 | 17.96 | **17.97** | 17.47 | 17.00 |

**Every one of the twenty-one holds a locked 30.3 fps**, which is the `FRAME_MS`
cap and not a limit of the renderer. The worst frame anywhere is 17.97ms, 54% of
the 33ms budget, and it is the number every other document in this repo quotes.

The dominant cost in every scene is the fixed ~13ms of `fillScreen` plus
`pushSprite` on a 320x170 16-bit sprite. The most expensive face costs about 3ms
on top of that, which is why there was room for three of them.
