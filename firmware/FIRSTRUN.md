# The first run

The out-of-the-box moment. Someone opens a box, plugs this into any USB port,
and before they have read a single word the object has to do something.

An armed board plays a 9.25 second sequence once, and then never again. A brand
new board used to boot straight into ambient, which is exactly right for hour
two and wasted on second one: the ambient screen is designed to be ignorable,
and the one moment it is guaranteed to be watched is the one moment it should
not be.

Everything here is in `src/main.cpp`, in `renderFirstRun` and the handful of
functions above it. It draws through the same `Face` interface as ambient and
live (`src/Face.hpp`), so it works on whichever face the board is set to.

## 1. What the sequence does

It is a creature waking up, not a device starting. Nothing in it is a status,
nothing needs a host, and the only words on screen are the owner's name and
`hello`.

The board holds dark for the 2000ms boot hold (an armed board deliberately
skips the wordmark boot screen, because a product name flashing first gives
away that the darkness was a boot and not a sleep). Then the backlight ramps up
on a squared curve over a pair of shut eyes, so the light arrives rather than
dissolves. The eyes crack open, hold a squint, and then open fully: a real eye
opens in two moves against the light, and one smooth ramp reads as a shutter.
The gaze drifts off to one side, then snaps back to centre. That snap is the
beat the whole sequence exists for, the moment of being noticed. Then a first
blink. Then a hand rises into frame beside the words `Hi OpenAI`, waves three
times and drops back out. Then a small nod, the owner's name fading in, and
`hello` under it. At the end `hello` dips through black and the ambient second
line fades in behind it.

**The wave sits where it does because a greeting is a social act with an
order to it.** You do not wave at somebody before you have seen them, and you
do not say whose desk you are on before you have said hello, so it goes after
the eye contact and before the name. It also went into the only slack the
sequence had: between the first blink and the nod there used to be a second
and a bit where the creature had already noticed you and had not started
talking yet, which is precisely the shape of a wave. The beat costs 1.6
seconds and the sequence only got 0.65 seconds longer, because the `hello`
hold at the end paid the other 0.2 and the old dead air paid the rest.

**It does not touch the faces, and that is why it works on all three.** There
is no arm anchor in the `Face` interface, and inventing one would mean editing
rounded, arc and bear plus the header they share. The hand instead rises into
the two text lines' band, which is the composition's own property and is empty
at that moment: the name does not begin fading in until 200ms after the hand
has gone. No face can tell the beat happened. The hand itself is six
anti-aliased capsules (`drawWideLine`, the same primitive the arc face walks
its ribbons with) rotated about a wrist point, because a hand that hinges at
the wrist reads as waving and a hand that slides sideways reads as a windscreen
wiper.

The exact beats, easings and offsets are in the code. `renderFirstRun` is the
specification and this prose is not.

**The settle is invisible on purpose.** The last second of the sequence already
*is* the ambient composition: same face, same size, same position, same two text
lines, and the accent colour has been easing toward `ambientAccent(nowMs)` since
the nod. The 700ms `MODE_XFADE_MS` cross-fade at the end therefore has almost
nothing left to change. The sequence also forces the anti burn-in drift to zero
and holds `gXfade` at 1.0: 9.25 seconds cannot burn anything in, and a
composition that wanders while it is introducing itself looks like it is sliding
off the panel.

**What it is not:** no progress bar, no version string, no "waiting for host",
no unit id, no instructions, no logo animation.

## 2. Persistence: how it plays exactly once

One byte in NVS, in the same `codexbuddy` namespace as the owner name and the
face, under key `firstrun`. Key absent or `0` means armed; `1` means spent.

**Absence is the armed state on purpose.** A freshly flashed board has no
namespace at all, so it is armed without anything ever having been written, and
re-arming is a key *removal* rather than a value. There is no state where a
board is ambiguous.

**A reflash does not bring it back.** `pio run -t upload` writes four regions:
the bootloader at `0x0`, the partition table at `0x8000`, `boot_app0` at
`0xe000`, and the app at `0x10000`. The `nvs` partition is at `0x9000` (size
`0x5000`, from `default_16MB.csv`) and is in none of them. So a spent flag stays
spent across any number of reflashes of any image, and a board can be powered
and re-flashed as often as the build needs without burning the one moment the
recipient is supposed to get. This was proven on hardware, not inferred from the
partition table: an image was flashed over an already-spent board and the
sequence did not return, and the stored name survived too.

**It is spent at the END of the sequence.** `spendFirstRun()` runs at the
9250ms mark, not at the start, so a board unplugged halfway through is still
armed and its owner still gets the whole thing. The cost is that a board reset
repeatedly mid-sequence keeps replaying, which is the correct failure direction.
A replay never writes the flag at all, in either direction.

## 3. Replaying it

```sh
printf '{"firstrun":"play"}\n' > /dev/cu.usbmodem1101
```

**This used to be a button gesture and is not any more.** Button 1's two second
hold is the toy now (`firmware/README.md`, "The toy"). The button model was
full, so the next feature had to replace something rather than add a fifth
threshold, and this was the weakest binding on the board: the replay is a party
trick every owner is shown once automatically, while the toy is a thing they can
reach for every time a turn runs long. Reversing that call is one line: put
`REPLAY_HOLD_MS` back, gate the hold on `safeToInterrupt()`, and give the toy a
different door.

Two doors are left, both host-free or nearly so: the protocol line above, and
the factory reset, which replays the sequence to confirm itself (that one costs
the stored name, so it is a wipe-and-show rather than a show).

**When it is refused.** `{"firstrun":"play"}` does nothing while the effective
state is `busy` or `waiting`, which is `safeToInterrupt()`. A turn that is
actually running and a prompt that is actually pending are the two things this
device exists to show, and hiding either behind a 9.25 second animation is the
one thing it must never do. Ambient always qualifies as safe. On refusal the
board says `err: firstrun refused, session is active` rather than silently doing
nothing.

## 4. The factory reset

**Hold BOTH buttons together for 5 seconds, in ambient mode only.** On
completion the board wipes its stored state and immediately plays the sequence
as a **replay**, so the gesture confirms itself by showing exactly what the next
owner will see, without spending the flag it just re-armed.

Two switches at opposite ends of the board held together for five seconds is not
something a hand does by accident, and this is the one gesture that destroys
stored state. Ambient-only means no host has spoken for five minutes, so there
is no live session and no name in active use to destroy.

**Press them together.** While button 1 is down, button 2's 800ms face cycle is
suppressed, so travelling to 5 seconds does not swap the face on the way. Sloppy
ordering does not merely delay the wipe, it refuses it for that press: if button
2 goes down first and is held past 800ms on its own, the face cycles and
`pumpButton2` sets `button2Held`, which is the press being spent. Both the wipe
(`b2Down && !button2Held && ...` in `pumpButton`) and the red warning bar
(`if (button1Held || button2Held) return -1.0f` in `chordHoldProgress`) test that
flag, so what follows is a persisted face swap and then nothing at all: no bar,
no wipe, no line on the wire, however long the two are then held. The fingers
have to come off and go back down together. The silence is the whole reason this
paragraph exists.

**What it clears:** `Preferences::clear()` on the whole `codexbuddy` namespace,
which is the owner name, the face, the first-run flag, the game's best score,
the two stats records and the focus timer's chosen length, plus anything a later
firmware adds. `grep -n NVS_KEY_ firmware/src/main.cpp` is the list to trust
rather than this one. `clear()` rather than individual removes, deliberately, so
this cannot silently stop being a full reset the next time somebody adds a key.
The RAM mirrors are reset to exactly what a virgin board would load, so the
board is in its out-of-the-box state without a reboot.

```sh
printf '{"reset":"factory"}\n'  > /dev/cu.usbmodem1101   # wipe everything
printf '{"reset":"firstrun"}\n' > /dev/cu.usbmodem1101   # re-arm ONLY
```

`reset` is applied **before** every other field on the same line, so one line
can wipe a board and then rename it:
`{"reset":"factory","name":"Alex Rivera"}`. The board replies `reset: factory
ok` or `reset: firstrun ok`, or `reset: ... ram-only` if NVS could not be
opened, rather than pretending it stuck.

## 5. The flashing run

This is why `reset` has two values rather than one. Every unit is powered
several times while it is flashed and boxed, and the first of those power-ups
spends the flag. So the last thing done to each board has to re-arm it.

```sh
cd firmware && pio run -t upload --upload-port /dev/cu.usbmodemNNNN
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemNNNN   # name it
printf '{"reset":"firstrun"}\n'   > /dev/cu.usbmodemNNNN   # arm it, keep the name
# unplug, box
```

That leaves a **named** board with the sequence armed. `{"reset":"factory"}` is
the other tool: it returns a board to genuinely blank, name and all. Use it when
a board needs to go back to zero, not as the last step before boxing, unless the
boards are meant to ship unnamed.

**The trap:** powering a board after arming it, to check it, plays and spends
the sequence. Arm it again before boxing. The greeting is how you check without
guessing:

```
hello tdisplay-s3 v1 name="Alex Rivera"
```

and an armed board follows it with `firstrun: playing (first)`.
`tools/flash-all.sh` does the arm itself, as its last step per unit, and logs
`no-arm` if it does not take. See `docs/wednesday-runbook.md` for the full
flash-day procedure.

## 6. Protocol additions

Both fields follow the same rule as `state` and `face`: a value outside the
accepted list makes the **whole line** malformed, so it is dropped silently with
no `ok`. A host that means `factory` and writes `Factory` must not have its typo
acked as though something happened.

| Field | Values | Effect |
|---|---|---|
| `reset` | `"factory"` | Clears the whole NVS namespace. Applied before every other field on the line |
| `reset` | `"firstrun"` | Re-arms the first run only. Name and face untouched |
| `firstrun` | `"play"` | Plays the sequence now. Never writes the flag. Refused while `busy` or `waiting`. Applied last |

New device-to-host lines, all unsolicited notices rather than replies:
`firstrun: playing (first)`, `firstrun: playing (replay)`, `firstrun: spent`,
`reset: factory ok`, `reset: firstrun ok`, `reset: ... ram-only`, and
`err: firstrun refused, session is active`. They exist so the whole lifecycle is
provable over the wire instead of by eye. They are additive: the greeting
prefix, the `ok` per accepted line and the silent-drop rule for malformed lines
are all unchanged, and every existing consumer matches on prefixes these do not
collide with.

Full field table in `docs/protocol.md` section 2.7.

## 7. What is proven, and what is not

The whole lifecycle was driven over the serial port on board
`AC:A7:04:F6:51:64`: a factory reset, a power cycle that played and spent the
sequence, a second power cycle that did not play it, a reflash that did not
bring it back, a replay that did not spend, a replay refused mid-prompt, a bad
enum value dropping the whole line, and `reset:"firstrun"` arming without
clearing the name.

It was driven again on board `44:1B:F6:D2:41:08` after the wave was added, and
the wire proved the new length as a side effect: `firstrun: playing (first)`
and `firstrun: spent` came out 9.273 seconds apart, which is the 9250ms mark
plus the frame it lands on.

The serial port cannot prove a single thing about whether the sequence is any
good. Every visual claim about it is unticked in `docs/NEEDS-EYES.md`, which is
the gate. The one test that matters is there: factory reset a board, unplug it,
and plug it into a plain USB charger with no computer anywhere.

Frame cost is still not a concern. The first run draws the same face through
the same interface as ambient and draws no ring at all, and the wave adds six
capsules and a string to 48 of its 280 frames. Measured on hardware with
`-DFPS_DEBUG`, on all three faces, before and after: every window holds the
locked 30.3fps `FRAME_MS` cap, and the worst frame in the whole sequence went
from 17.20ms to 18.04ms against a 33ms budget. Both numbers are the arc face,
which is the expensive one. `FACES.md` part 6 has the measured table.
