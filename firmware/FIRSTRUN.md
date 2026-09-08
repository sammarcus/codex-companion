# The first run

The out-of-the-box moment. Someone opens a box, plugs this into any USB port,
and before they have read a single word the object has to do something.

A brand new board used to boot straight into the calm ambient face. That is
exactly right for hour two and completely wasted on second one: the ambient
screen is designed to be ignorable, and the one moment it is guaranteed to be
watched is the one moment it should not be.

So an armed board plays a sequence once, and then never again.

Everything here lives in `src/main.cpp`, in `renderFirstRun` and the handful of
functions above it. It draws through the same `Face` interface as ambient and
live (`src/Face.hpp`), so it works with whichever face the board is set to.

---

## 1. The sequence, beat by beat

It is a creature waking up, not a device starting. Nothing in it is a status,
nothing needs a host, and the only words on screen are the owner's name and
`hello`.

Times are milliseconds from the end of the 2000ms boot hold, which is also the
instant the greeting goes out on serial. The whole thing is 8.6 seconds, so the
object has done its trick inside the first eleven seconds out of the box.

| From | To | Beat | What is on the panel |
|---|---|---|---|
| | 0 | **Dark** | The boot hold. Backlight at duty 0, nothing drawn. From the recipient's side the thing simply has not woken up yet. An armed board deliberately does **not** show the wordmark boot screen. |
| 0 | 1400 | **A light comes on** | The backlight ramps 0 to 255 on a squared curve, so it arrives rather than dissolves. Under it, two **shut** eyes: the 3px lid line, in the sleep blue lifted 42% toward white. Something was asleep and the lights just came up on it. |
| 1400 | 1700 | **It cracks an eye** | Lids part to 35%. |
| 1700 | 1950 | **Squint** | Held at 35%. This hold is the whole trick: a real eye opens in two moves, against the light. One smooth ramp reads as a shutter. |
| 1950 | 2300 | **Open** | 35% to fully open on a decelerating curve. Colour has finished lerping from sleep blue to `C_IDLE`. |
| 2300 | 2700 | **Looking around** | The gaze drifts off to the left and slightly down (7px, 2px). It is awake but has not found anyone yet. |
| 2700 | 2950 | **It finds you** | The gaze snaps back to dead centre over 250ms, decelerating. Slow enough to see, fast enough to read as *noticing* rather than scanning. This is the beat the whole sequence exists for. |
| 2950 | 3400 | **Eye contact** | Locked centre. No glancing away from here on. |
| 3400 | | **The first blink** | One blink, then the ordinary randomised blink rhythm takes over for the rest of the sequence. Only the first one is choreographed. |
| 4600 | | **A small nod** | The whole head lifts 3px and settles over 350ms. |
| 4600 | 5100 | **Whose it is** | Line 1 fades in: the owner's name, or the product name when there is none. Font2, exactly where ambient's line 1 lives. |
| 4800 | 5300 | **hello** | Line 2 fades in: `hello`. Font0, exactly where ambient's line 2 lives. |
| 7000 | 7700 | **The swap** | `hello` fades out and the ambient line 2 fades in behind it, dipping through black between them. From 7700 the screen **is** the ambient screen. |
| 8600 | | **Settle** | Hand back to ambient (or to live, if a host spoke during the sequence). |

### Why the settle is invisible

The last second is already the ambient composition: same face, same size, same
position, same two text lines, and the accent colour has been easing into
`ambientAccent(nowMs)` since the nod at 4600. The 700ms `MODE_XFADE_MS`
cross-fade that fires at 8600 therefore has almost nothing left to change.

The sequence also opts out of the anti burn-in drift entirely (`gOx`/`gOy`
forced to 0) and holds `gXfade` at 1.0, because 8.6 seconds cannot burn
anything in and a composition that wanders while it is introducing itself looks
like it is sliding off the panel. Ambient's drift winds up from zero afterwards
exactly as it does coming out of the boot screen.

### What it is not

No progress bar, no version string, no "waiting for host", no unit id, no
instructions, no logo animation. The unit id is on the *normal* boot screen for
assembly-day identification, and an armed board is the one case where that
screen is the wrong thing to show, because a product name flashing first gives
away that the darkness was a boot and not a sleep.

---

## 2. Persistence: how it plays exactly once

One byte in NVS, in the same `codexbuddy` namespace as the owner name and the
face, under key `firstrun`.

| Stored value | Meaning |
|---|---|
| key absent, or `0` | **armed**: the next power-on plays the sequence |
| `1` | **spent** |

**Absence is the armed state on purpose.** A freshly flashed board has no
namespace at all, so it is armed without anything ever having been written, and
re-arming is a key *removal* rather than a value. There is no state where a
board is ambiguous.

### Why a reflash does not bring it back

`pio run -t upload` writes four regions: the bootloader at `0x0`, the partition
table at `0x8000`, `boot_app0` at `0xe000`, and the app at `0x10000`. The `nvs`
partition is at `0x9000` (size `0x5000`, from `default_16MB.csv`) and is in none
of them.

So a spent flag stays spent across any number of reflashes of any image. Sam can
power and re-flash a board as often as he likes during the build without burning
the one moment the recipient is supposed to get. This is proven on hardware in
section 6, not assumed from the partition table.

### Why it is spent at the END of the sequence

`spendFirstRun()` runs at the 8600ms mark, not at the start. A board unplugged
halfway through is therefore still armed, and its owner still gets the whole
thing next time. The cost of that choice is that a board which is reset
repeatedly mid-sequence will keep replaying, which is the correct failure
direction: replaying is a much smaller problem than a recipient opening a box
and getting nothing.

A replay never writes the flag at all, in either direction.

---

## 3. Replaying it

```sh
printf '{"firstrun":"play"}\n' > /dev/cu.usbmodem1101
```

**This used to be a button gesture as well, and is not any more.** Button 1's
two second hold is the toy now (`firmware/README.md`, "Something to do"). The
button model was full, the next feature had to replace something in it rather
than add a fifth threshold, and this was the weakest binding on the board: the
replay is a party trick that every owner is shown once automatically, while the
toy is a thing they can reach for every time a turn runs long.

Two doors are left, and both are host-free or nearly so:

- `{"firstrun":"play"}` over the cable, which is what the flashing run and any
  demo script use anyway.
- the factory reset, which replays the sequence to confirm itself. That one
  costs the stored name, so it is a wipe-and-show rather than a show.

**If that trade turns out to be wrong it is one line to reverse**: put
`REPLAY_HOLD_MS` back and gate the hold on `safeToInterrupt()`, and give the toy
a different door. The rest of this document is unaffected either way.

### When it is refused

`{"firstrun":"play"}` does nothing at all while the effective state is `busy` or
`waiting`, which is `safeToInterrupt()`. A turn that is actually running, and a
prompt that is actually pending, are the two things this device exists to show;
hiding either behind an 8.6 second animation would be the one thing it must
never do. Ambient always qualifies as safe, because it means no host has spoken
for five minutes or ever.

On refusal the board replies `err: firstrun refused, session is active` rather
than silently doing nothing.

---

## 4. The factory reset

**Hold BOTH buttons together for 5 seconds, in ambient mode only.**

On completion the board wipes its stored state and immediately plays the
sequence as a **replay**, so the gesture confirms itself by showing exactly what
the next owner will see, without spending the flag it just re-armed.

### Why this gesture

Two switches at opposite ends of the board, held together for five seconds, is
not something a hand does by accident, and this is the one gesture that destroys
stored state. Ambient-only means no host has spoken for five minutes, so there
is no live session and no name in active use to destroy.

While button 1 is down, button 2's 800ms face cycle is suppressed, so travelling
to 5 seconds does not swap the face on the way through. **Press them together.**

Sloppy ordering does not merely delay the wipe, it refuses it for that press.
If button 2 goes down first and is held past 800ms on its own, the face cycles
and `pumpButton2` sets `button2Held`, which is the press being spent. Both the
wipe (`b2Down && !button2Held && ...` in `pumpButton`) and the red warning bar
(`if (button1Held || button2Held) return -1.0f` in `chordHoldProgress`) test
that flag, so what follows is a persisted face swap and then nothing at all: no
bar, no wipe, no line on the wire, however long the two are then held. The
fingers have to come off and go back down together. The end state is **not** the
same as a clean chord, and the silence is the whole reason this paragraph exists.

### What it clears

`Preferences::clear()` on the whole `codexbuddy` namespace, which is:

- the owner name (`owner`)
- the active face (`face`)
- the first-run flag (`firstrun`)
- the game's best score (`hopbest`)
- the two stats records (`bestturn`, `bestday`)
- the focus timer's chosen length (`focusmin`)
- anything a later firmware adds to that namespace

That is the whole of `grep -n NVS_KEY_ firmware/src/main.cpp` as of this
writing, and that grep is the list to re-run rather than trusting this one.

`clear()` rather than three individual removes, deliberately, so this cannot
silently stop being a full reset the next time somebody adds a key.

The RAM mirrors are reset to exactly what a virgin board would load, so the
board is in its out-of-the-box state without a reboot.

### Over the protocol

```sh
printf '{"reset":"factory"}\n'  > /dev/cu.usbmodem1101   # wipe everything
printf '{"reset":"firstrun"}\n' > /dev/cu.usbmodem1101   # re-arm ONLY
```

`reset` is applied **before** every other field on the same line, so one line can
wipe a board and then rename it:

```sh
printf '{"reset":"factory","name":"Alex Rivera"}\n' > /dev/cu.usbmodem1101
```

The board replies `reset: factory ok` or `reset: firstrun ok` (and
`reset: ... ram-only` if NVS could not be opened, rather than pretending it
stuck), in addition to the normal `ok`.

---

## 5. The flashing run

This is the operationally important part, and it is why `reset` has two values
rather than one.

Sam powers every unit several times while flashing and boxing it, and the first
of those power-ups spends the flag. The fourteenth recipient must still get the
version he designed, so the last thing done to each board has to re-arm it.

**Recommended, per unit:**

```sh
cd firmware && pio run -t upload --upload-port /dev/cu.usbmodemNNNN
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemNNNN   # name it
printf '{"reset":"firstrun"}\n'   > /dev/cu.usbmodemNNNN   # arm it, keep the name
# unplug, box
```

That leaves a **named** board with the sequence armed. The recipient plugs it in
and sees their own name at the 4600ms beat.

`{"reset":"factory"}` is the other tool: it returns a board to genuinely blank,
name and all. Use it when a board needs to go back to zero, not as the last step
before boxing, unless the boards are meant to ship unnamed.

**The trap worth knowing:** if you power a board after arming it, to check it,
the sequence plays and spends itself. Arm it again before boxing. The greeting
line is how you check without guessing:

```
hello tdisplay-s3 v1 name="Alex Rivera"
```

and an armed board follows it with `firstrun: playing (first)`.

---

## 6. Protocol additions

Both fields are optional, both are enums, and both follow the same rule as
`state` and `face`: a value outside the accepted list makes the **whole line**
malformed, so it is dropped silently with no `ok`. A host that means `factory`
and writes `Factory` must not have its typo acked as though something happened.

| Field | Values | Effect |
|---|---|---|
| `reset` | `"factory"` | Clears the whole NVS namespace: name, face, first-run flag. Applied before every other field on the line. |
| `reset` | `"firstrun"` | Re-arms the first run only. Name and face untouched. |
| `firstrun` | `"play"` | Plays the sequence now. Never writes the flag. Refused while `busy` or `waiting`. Applied last. |

New device-to-host lines, all unsolicited notices rather than replies:

| Line | When |
|---|---|
| `firstrun: playing (first)` | the real out-of-the-box play has started |
| `firstrun: playing (replay)` | a replay has started, from the wire or from a factory reset |
| `firstrun: spent` | the flag has just been written, at the end of a real first run |
| `reset: factory ok` / `reset: firstrun ok` | a reset was applied |
| `reset: ... ram-only` | applied in RAM, but NVS could not be opened |
| `err: firstrun refused, session is active` | a replay was asked for at a bad moment |

These exist so the entire lifecycle is provable over the wire instead of by eye,
which is what section 8 does. They are additive: the greeting prefix, the `ok`
per accepted line and the silent-drop rule for malformed lines are all
unchanged, and every existing consumer matches on prefixes or substrings that
these lines do not collide with.

Full field table in `docs/protocol.md` section 2.7.

---

## 7. What did not change

- the greeting, `hello tdisplay-s3 v1 name="..."`, byte for byte, still sent at
  exactly `BOOT_HOLD_MS`. `tools/flash-all.sh`'s `verify_hello` and the helper's
  `doctor` are unaffected, including on an armed board.
- the five states, the ring, the ambient and live compositions, the staleness
  timeouts, the anti burn-in drift, the face interface.
- button 2: tap is still one brightness step, hold is still the face cycle.
- button 1 in live mode while a prompt is pending: still acknowledges on the
  **press** edge, instantly. An ack marks the press as handled, so it can never
  also step the brightness or open the toy.

One deliberate behavioural change, the same one button 2 already took when the
face cycle landed: **outside of an ack, button 1's brightness step now happens
on release rather than on press**, because the press edge is no longer enough to
tell a tap from a hold. Imperceptible for a tap.

---

## 8. Verified on hardware

Board `AC:A7:04:F6:51:64` on `/dev/cu.usbmodem1101`, image sha256
`e2363fe2...b096d10aa`. Every line below was read off the wire:

| Proven | How it showed up |
|---|---|
| factory reset over the protocol | `reset: factory ok` |
| power cycle: the sequence plays and spends itself | `hello ... name=""`, then `firstrun: playing (first)`, then `firstrun: spent` |
| another power cycle: it does not play | greeting alone, no first-run lines |
| **a reflash of the same image does not bring it back** | flashed 386,320 bytes at `0x00010000`, power cycled, greeting alone. The stored name survived too |
| a deliberate replay does not spend | `firstrun: playing (replay)`, and no `firstrun: spent` |
| a replay is refused mid-prompt | `err: firstrun refused, session is active` |
| a bad enum value drops the whole line | `{"reset":"Factory"}` and `{"firstrun":"replay"}` both did nothing and were not acked |
| `reset:"firstrun"` arms without clearing the name | `reset: firstrun ok`, then a boot reading `hello ... name="Alexandra Rivera"` followed by `firstrun: playing (first)` |

The board is currently left factory reset on the clean image, so unplugging and
replugging it plays the sequence exactly as a recipient will see it.

**A note on the test conditions.** A sibling agent was driving the same board
throughout, and two processes on one USB CDC port reset the chip at random and
corrupt readings. Contention can only ever break a step, never fake a passing
one, so the run above was repeated until one completed clean end to end (it took
sixteen attempts) and only that run is reported.

---

## 9. Frame rate

`FRAME_MS` is 33, a 30fps cap. The sequence draws strictly less than live mode:
the same face through the same interface, no ring, and at most two text lines.

Measured on the real board with a `-DFPS_DEBUG` build, each row averaged over
3 second windows, several windows per scene:

| Scene | avg ms | worst ms | fps |
|---|---|---|---|
| **first run** | **12.69 to 14.77** | **14.96** | 30.3 |
| ambient | 14.74 to 14.98 | 15.82 | 30.3 |
| live `sleep` | 14.03 to 14.35 | 15.40 | 30.3 |
| live `idle` | 14.92 to 15.02 | 15.19 | 30.3 |
| live `busy` | 14.94 to 15.18 | 15.41 | 30.3 |
| live `done` | 14.95 to 16.19 | 16.98 | 30.3 |
| live `waiting` | 16.24 to 16.70 | 16.96 | 30.3 |

Every scene holds a locked 30.3fps, which is the `FRAME_MS` cap and not a limit
of the renderer. The worst frame anywhere is 16.98ms, 51% of the 33ms budget.

**The first run is the cheapest scene on the device**, measured both on a real
armed boot and on a replay driven from the wire. That is not luck: it draws the
same face through the same interface as ambient, and it draws no ring at all.
It cannot regress the frame budget because nothing else can be removed from it.

The dominant cost in every scene is the fixed ~13ms of `fillScreen` plus
`pushSprite` on a 320x170 16-bit sprite, which the sequence pays exactly like
every other scene. The 12.69ms low is the first window of the sequence, where
the eyes are still shut and there is no text yet.

---

## 10. What a human still has to look at

The serial port proved the persistence, the gestures' effects and the protocol.
It cannot prove a single thing about whether the sequence is any good. Every
item below needs an eye on the panel.

**The sequence itself**

- [ ] **The dark boot does not read as broken.** Two seconds of an unlit panel
      before anything happens is the riskiest beat in here. If it reads as a
      dead board rather than a sleeping one, the fix is to shorten the dark or
      to bring a faint lid line up earlier.
- [ ] **The light coming up reads as a wake, not as a fade-in.** The ramp is
      squared for exactly this reason.
- [ ] **The two-stage eye open reads as waking against the light**, not as a
      stutter or a dropped frame. The 250ms squint hold is the whole gesture.
- [ ] **The snap to centre reads as being noticed.** This is the moment the
      whole thing is built around. 250ms may be too fast to register or too slow
      to feel like a reaction.
- [ ] **The first blink lands as punctuation**, right after eye contact.
- [ ] **The nod is visible but not comic.** 3px over 350ms.
- [ ] **`hello` is the right word**, and the right amount of text. It is the
      only thing the device ever says that is not a name, a metric or a status.
- [ ] **The name is legible and correctly placed** at the 4600ms beat, and a
      24 character name still fits.
- [ ] **The swap from `hello` to the product line reads as a settle**, not as a
      glitch. It dips through black rather than cross-dissolving.
- [ ] **The hand-off at 8.6s is invisible.** Watch for any jump in colour,
      position or brightness at the end. If anything twitches, the sequence and
      ambient disagree about something and that is a bug, not a taste call.
- [ ] **The whole thing is worth watching twice**, and does not outstay 8.6
      seconds.
- [ ] **It works on all three faces**, not just `rounded`. Set `bear` and `arc`
      and replay.

**The gestures (I cannot press buttons)**

- [ ] `{"firstrun":"play"}` replays the sequence. The button no longer does:
      holding button 1 for 2 seconds opens the toy instead.
- [ ] A short tap of button 1 in ambient opens the stats screen, and does not
      feel laggy now that it acts on release. (It no longer steps the
      brightness: see `AMBIENT.md`, "Superseded by the stats screen".)
- [ ] Button 1 in `waiting` still acknowledges instantly on the press.
- [ ] `{"firstrun":"play"}` during `busy` or `waiting` is refused, out loud.
- [ ] Holding both buttons for 5 seconds in ambient wipes the board and
      immediately plays the sequence.
- [ ] Travelling to 5 seconds does not swap the face on the way.
- [ ] Neither gesture can be triggered by the case.

**Out of the box**

- [ ] Factory reset a board, unplug it, wait, plug it into a plain USB charger
      with no computer anywhere, and watch the whole thing. This is the only
      test that matters, and it is the exact thing fourteen people will do.
