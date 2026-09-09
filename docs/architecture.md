# Architecture

How the whole thing fits together, and why each piece is shaped the way it is.

Every claim below was checked against the code it describes. Citations name
symbols rather than line numbers, because `grep -n applyJsonLine
firmware/src/main.cpp` still works after an edit.

---

## 1. What the object is

A LILYGO T-Display-S3 on the end of a USB cable, sitting on a desk, showing
what its owner's OpenAI Codex session is doing. ESP32-S3, 16MB flash, 8MB
PSRAM, a 320x170 colour LCD in landscape, two buttons, native USB CDC. No WiFi,
no accounts, no pairing, no radio of any kind.

Fourteen of them, as a gift.

That last fact is the root of most of the architecture. These are not products
sold to people who wanted one. They are objects handed to colleagues who did
not ask, who may work at a company that will not let them run a stranger's
program on a work laptop, and who will judge the thing in the first ten seconds
on a desk they already share with a monitor, a keyboard and a plant.

## 2. The four laws

Everything below is downstream of these. They are the project brief, not
guidance.

1. **It must be delightful on USB power alone, with no host software ever
   installed.** The host program is an optional bonus. Ambient mode is what
   most owners will see most of the time.
2. **The host program is one zero-dependency Node file.** It reads state from
   Codex's first-party hook events. It can never approve or deny anything. It
   installs nothing by default. No dependencies, no persistence, no network.
3. **Every unit flashes the byte-identical image.** One binary, one hash.
   Nothing is compiled per unit. Units identify themselves from the chip MAC;
   the owner name is runtime state set over the protocol.
4. **It must never be irritating to sit next to in an open-plan office.**

Law 1 is why the device is autonomous and the host is optional. Law 2 is why
the host is a single file you can read in one sitting. Law 3 is why nothing is
personalised at build time. Law 4 has killed more good ideas here than the
other three combined, and it is why the milestone pill sits absolutely still
after its entrance, why the do-not-disturb sign only breathes, why the game
borrows the ambient blue rather than amber, and why there is no buzzer.

## 3. The system

```
  Codex CLI
      |
      |  (a) first-party hook events. Codex spawns the helper once per
      |      event with a JSON payload on stdin. THIS IS THE STATE PATH.
      |
      |  (b) ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl, which Codex
      |      writes anyway. Read, never written. NUMBERS ONLY.
      v
  +--------------------------------------------------+
  |  helper/codex-companion.js                        |
  |  one file, four Node built-ins, no dependencies    |
  |                                                    |
  |  hook payload -> one of five state words           |
  |  rollout tail  -> ring fraction, centre text,       |
  |                   label, sub line, tps, tokens      |
  |  runs, exits. Remembers nothing between events.     |
  +----------------------+-----------------------------+
                         |
                         |  USB CDC, 115200 baud
                         |  one JSON object per line
                         v
  +--------------------------------------------------+
  |  firmware/src/main.cpp                            |
  |                                                    |
  |  parser + state machine  -> which of five states    |
  |  blink driver, glance driver, cross-fade, drift     |
  |  compositions: ambient, live, and six modal screens |
  |  ring, text, backlight ladder                       |
  |                    |                                |
  |                    v  Face.hpp interface            |
  |            FaceRounded / FaceBear / FaceArc         |
  +----------------------+-----------------------------+
                         |
                         v
              320x170 ST7789, 8-bit parallel
```

The arrow from Codex to the helper only ever points one way, and the arrow from
the helper to the device only ever points one way in the sense that matters:
the device replies `ok`, and nothing else. There is no path by which the device
can influence Codex, and that is a load-bearing property, not an accident of
scope. See section 6.

**The device is complete without the top two boxes.** Unplug the helper, or
never install it, and the firmware runs ambient mode forever. That is law 1,
and it is the single most important structural decision in the project.

---

## 4. The device

### The two base compositions

| | when | what is on the panel |
|---|---|---|
| **ambient** | no host has ever spoken, or the last accepted line was 5 minutes ago | a face, the owner's name, the product name. No ring |
| **live** | a line was accepted less than 5 minutes ago | a face on the left, the ring on the right, two text lines centred under both |

`hostEverSpoke` latches on the first accepted line and never clears, so the two
ambient paths (never had a host, had one and lost it) land on the same screen.
One 700ms cross-fade mechanism serves boot-to-ambient, ambient-to-live and
live-to-ambient, and the burn-in drift winds up on the way into ambient and
unwinds to exactly zero on the way into live, so the live layout always lands
on pixel-exact geometry.

### Five states

`sleep`, `idle`, `busy`, `waiting`, `done`. `waiting` is the hero state: the
human is being asked something. Everything about it is aimed at reading as "hey,
you" from across a room, where the text is illegible and the ring is a small
amber dot: eyes wide, brows up, the whole head hopping on every pulse peak,
blinking twice as often, and the pulse period shortening as `tps` rises before
flipping to red at ten seconds.

Motion carries that, not brightness. A brightness change on a 40px object is
mostly invisible in peripheral vision; movement is not.

### Staleness, which is the device's only defence against a dead host

- **30 seconds** with no data: backlight drops a tier, and `busy` and `waiting`
  fall back to the idle look. That second half matters: a dead host must not be
  able to leave a board pulsing red for an approval prompt that no longer
  exists. The stored state is untouched, so one fresh line restores it.
- **5 minutes** with no data: back to ambient.

### Six modal screens

The message card (`say`), the do-not-disturb sign (`dnd`), the game (`play`),
the stats screen (`stats`), the focus timer (`focus`), and the out-of-the-box
first run. Each is documented in `firmware/README.md`.

One rule cuts across every one of them, and it is the whole precedence model:

| on screen | wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| **a pending prompt (`waiting`)** | **the prompt** |
| a message card | the card |
| the sign | the sign |
| the toy | the toy |
| the stats screen | the stats screen |
| anything else | the timer |

**A pending question outranks everything except the first run.** That is the
single failure this device is built not to have. A countdown, a sign or a
milestone is something the owner can reconstruct from memory or from a phone;
an approval prompt is a thing only this panel is showing. Every screen here
either yields the panel back and returns afterwards (the card, the sign, the
timer) or closes outright and hands the screen back (the toy, the stats
screen), and the ones that close do so because after answering a prompt
somebody is back at their keyboard and a game reappearing over their answer is
the same failure in a different costume.

### Persistence

Arduino `Preferences`, one NVS namespace `codexbuddy`. It holds the owner name
(`owner`), the active face (`face`), the first-run-spent flag (`firstrun`), the
game's best score (`hopbest`), the two stats records (`bestturn`, longest turn
ever, and `bestday`, best token day) and the focus timer's chosen length
(`focusmin`). Seven keys, and the list that cannot drift is
`grep -n NVS_KEY_ firmware/src/main.cpp`. Each of them is written **only when
the value actually changes**, never on a timer and never per line, because NVS lives in the same
flash the firmware does and a host repeating `"name"` in a 2000ms heartbeat
would otherwise rewrite the key about 43,000 times a day.

Three things are deliberately **not** persisted. The do-not-disturb sign,
because unplugging and replugging a desk companion is something a person does
at the desk, which is the one moment a sign is provably stale, and a board that
arrives in its box must not come up holding a sign nobody set. The message
card, same reasoning. And the **daily** stats figures, because a reboot is not
a midnight and showing yesterday's number under the word TODAY is worse than
showing a zero.

### Rendering

One full-screen 320x170 16-bit `LGFX_Sprite` in PSRAM (108,800 bytes), pushed
to the panel once per frame. If PSRAM refuses it falls back to internal RAM, and
if both fail it prints `err: sprite alloc failed, drawing direct` and renders
straight to the panel at 5fps, so a unit still shows its state rather than a
black screen.

`FRAME_MS` is 33, a 30fps cap. The dominant cost in every scene is the fixed
~13ms of `fillScreen` plus `pushSprite`; the most expensive face costs about
3ms on top of that. The worst frame measured anywhere in the project is 17.97ms
against the 33ms budget, which is why there was room for three faces, a game
and six modal screens.

---

## 5. The protocol

Newline-delimited JSON at 115200 over native USB CDC, host to device. The full
spec is `docs/protocol.md`; this is the shape and the reasoning.

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3,"name":"..."}
```

Four properties do most of the work:

**Any field may be omitted and the device keeps its previous value.** So a
delta line is legal, and two programs can drive the same board without fighting:
the hook sends `state` plus numbers, and `run` sends only numbers and never
`state`. They compose rather than conflict. `{}` is a valid no-op keepalive.

**A bad `state`, `face`, `reset`, `firstrun`, `play` or `stats` value makes the
whole line malformed**, and a malformed line is dropped silently with no `ok`
and **no partial application**. Validation happens before anything is written,
so `{"state":"done","label":"X","face":"nope"}` changes nothing at all rather
than applying the state and then giving up halfway. Fields that can hold
arbitrary text (`say`, `sub`, `label`) can never make a line malformed; they are
sanitised and clamped instead.

**Lines over 512 bytes are discarded whole**, never truncated, so a partial
write can never be parsed as a valid frame.

**Device to host is three things and nothing else**: `hello tdisplay-s3 v1
name="<stored name>"` once after the boot screen, `ok` per accepted line, and a
set of unsolicited notices prefixed `say:`, `dnd:`, `play:`, `stats:`,
`milestone:`, `firstrun:`, `reset:` and `err:`. The notices exist because
nobody can read a panel over USB: they report the solved layout, the measured
text widths, the figures on the stats screen, so that what actually reached the
panel is provable from a terminal.

The `hello tdisplay-s3 v1` prefix is frozen. Two other programs match on it
(`/hello\s+tdisplay-s3/i` in the helper's `cmdDoctor`, and a substring test in
`verify_hello` inside `firmware/tools/flash-all.sh`), so the `name="..."` suffix
was added as a suffix precisely so both remain substring matches. The active
face is deliberately **not** on the wire for the same reason.

### The one quirk

The device's USB CDC transmit path runs exactly one message behind: the `ok`
for line N does not reach the host until the host writes line N+1. It was
verified on the current firmware and on the pre-ambient firmware on the same
board, so it is in `HWCDC` and not in this code. Over a whole stream nothing is
lost and the ack count always reconciles. The consequence for anyone writing a
host: treat acks as a running count, not a per-line handshake, and write an
extra `{}` to flush anything you are waiting to read.

---

## 6. The hook system, and why the helper cannot approve anything

Codex has a first-party hook system. `install-hook` registers this same file as
a `command` handler for eight of the twelve events Codex offers, in
`$CODEX_HOME/hooks.json`. Each event sets one word:

| Codex event | state |
|---|---|
| `SessionStart` | idle |
| `UserPromptSubmit` | busy |
| `PreToolUse` | busy |
| `PermissionRequest` | **waiting** |
| `PostToolUse` | busy |
| `Stop` | done |
| `Interrupt` | idle |
| `SessionEnd` | idle |

**There is no `PermissionResolved` event in Codex.** So the amber pulse is
cleared by whatever happens next: `PreToolUse`, `PostToolUse` or `Stop`. If the
owner walks away instead, nothing at all happens next, and the device's own
staleness timers are the only thing that recovers it. That is why those timers
exist in the form they do, and it is a lesson taken directly from prior art:
`docs/prior-art.md` records a hooks port that latches amber with
no event to clear it, so a resolved-then-quiet session stays amber forever.

### It can never approve or deny. Two independent reasons.

1. The `PermissionRequest` handler **writes nothing to stdout and exits 0**,
   which is Codex's own documented way for a handler to decline to decide and
   let the normal approval flow continue
   (`hooks/src/events/permission_request.rs`, the `trimmed_stdout.is_empty()`
   branch of `parse_completed`).
2. It is registered `"async": true`, and Codex only applies an allow or a deny
   from a handler whose execution mode is `Sync`
   (`hooks/src/engine/mod.rs`, `can_apply_control_effects`). So even a hook that
   did print a verdict would be ignored.

Either one alone is sufficient. That redundancy is deliberate: it is the claim
the printed card makes with the word "ever", and a claim on fourteen physical
cards handed to colleagues at the company that makes Codex had better be true
twice over.

### It can never block a session

Every handler except `SessionEnd` is registered async, which Codex schedules
and does not wait on (`hooks/src/engine/dispatcher.rs`). `SessionEnd` is the
one event Codex always runs synchronously, with a hard 3 second cap, so it is
registered with a 2 second timeout rather than pretending otherwise. On top of
that, **every failure path in the program is "do nothing and exit 0"**: no
device, a busy port, a malformed payload, an unreadable session file, an
outright bug. A test spawns the real hook for every event with the port pointed
at a device that does not exist and asserts exit 0 each time.

A broken desk toy must never be able to break somebody's work.

### What it reads, and the shape of the privacy argument

From the hook payload it reads exactly two fields: `hook_event_name` and
`transcript_path`. It never touches `tool_input`, `tool_response`, `prompt`,
`last_assistant_message`, `cwd`, `model` or `session_id`.

Hook payloads carry no numbers, so the ring, the percentage and the stopwatch
come from the session rollout file Codex is already writing. That file is read,
never written, and the defence is structural rather than a promise: a line is
only parsed at all if it contains one of eight substrings in `METRIC_HINTS`
(`"token_count"`, `"token_usage_record"`, `"compacted"`, `"task_started"`,
`"turn_started"`, `"task_complete"`, `"turn_complete"`, `"turn_aborted"`).
Every other line, which is to say every line carrying prompts, replies, command
output or file contents, is skipped **before `JSON.parse` ever sees it**. A test
walks the entire metrics object built from a real captured session and fails if
any value is a string.

The whole argument reduces to one sentence: everything that survives into a
frame is a number, a timestamp, or one of five fixed words.

### Numbers, in two places where the obvious answer is wrong

**Context fill is not `used / window`.** It uses Codex's own baseline-adjusted
formula (`contextFill`, from `protocol.rs`
`percent_of_context_window_remaining`, inverted) so the percentage on the ring
agrees with what the Codex TUI shows. A ring that disagrees with the terminal
next to it is worse than no ring.

**Tokens are sent, not derived.** The wire carries `tps` and two display
strings; integrating a rate sampled at hook events would produce a number
nobody could reconcile with what Codex reports. So the host sends the session
total and **the device banks the differences**: the first value a board ever
sees is a baseline contributing nothing (a board plugged in mid-session did not
watch those tokens happen), a higher value adds the difference, a lower one is
a new session and counts whole. That design is what lets the helper stay a
stateless one-shot program: it never has to remember anything between hook
invocations.

**`time` is local, not UTC**, for the same class of reason. The only question
the device asks of the clock is whether midnight has happened where the owner
is sitting, and a device with no timezone database cannot answer that from UTC.
The helper sends `Date.now()` shifted by its own `getTimezoneOffset()`, fresh
every frame, so a laptop carried across a timezone corrects itself with nothing
stored anywhere.

### Port discovery

`ioreg` is parsed as text and matched on Espressif's USB vendor id `0x303a` and
the product id `0x1001`. Not a `/dev/cu.usbmodem*` glob, which would happily
pick a USB power meter, and not a native module, which would be a dependency.
**If two boards match it refuses to guess and asks for `--port`.** That refusal
is deliberate: writing frames to the wrong device is a worse failure than
printing an error.

---

## 7. The face interface

The face is the product, and it has been redrawn several times. So it is not
part of the renderer.

```cpp
class Face {
  virtual const char* name() const = 0;
  virtual const char* description() const = 0;
  virtual void init() {}
  virtual void draw(FaceCanvas& c, const FaceFrame& f) = 0;
};
```

`draw` is called once per rendered frame for the active face only, after the
ring and before the text. That is the entire contract. A face has no update
call, no state that has to survive a swap, and no way to reach into
`main.cpp`, so swapping faces mid-animation is a pointer change.

Three designs implement it: `rounded` (the default, soft rectangles with a wet
highlight), `bear` (a cream creature whose mood is in its mouth), and `arc`
(two tapering hand-drawn strokes). Which one a board draws is runtime state in
NVS beside the owner name, set with `{"face":"bear"}` or an 800ms hold of
button 2. A new face costs one file and three lines in `firmware/src/Faces.cpp`.

### The two channels that explain the whole seam

**`open` is the entire blink channel, and a face never computes it.** It
arrives already multiplied: the blink driver's factor times the state's own lid
expression, so a `busy` squint arrives as 0.60 and a blink through that squint
arrives as 0.60 falling to 0. A face that decides when to blink is the specific
bug this interface exists to prevent, and the failure mode is concrete:
fourteen units on one table blinking in lockstep, or in three different rhythms
depending on which face is loaded. (Relatedly, the blink interval uses
`esp_random()` rather than `random()`, because unseeded `random()` is identical
on every boot of every board, which would produce exactly that lockstep.)

**`pulse` is the only motion channel not derivable from `nowMs`.** The
`waiting` pulse period moves with `tps` and its phase is accumulated outside,
so anything a face animates on the beat has to ride `pulse` to land on the same
frame the ring brightens.

Everything else stays outside, permanently: the parser and state machine, the
staleness timeouts, the blink and glance drivers, the cross-fade, the burn-in
drift, both compositions, the ring, the backlight ladder and all text layout.

The geometry contract describes the same space twice, as an eye pair and as a
box, so a face built from two eyes and a face that is a whole creature can each
use the half that suits it. The burn-in drift is already baked into the
coordinates a face is handed, which is what lets ambient wander nine pixels
while live lands on pixel-exact geometry.

**The interface was validated by the third implementation, not the second.**
`arc` has no rectangles, no lids and no fixed eye box, and reads `open` as
*curvature* rather than as height. That it fit without changing the interface is
the evidence that the seam is not shaped around the first two faces.

The game reuses the same interface at a smaller geometry: it hands over a box,
a colour and an `open` value exactly as the compositions do, and knows nothing
about eyes, bears or strokes. So the bear runs as a bear, a fourth face would
run the day it is written, and the creature in the game blinks on the same
shared driver as the creature on the desk.

---

## 8. Decisions that were reversed

This project changed its own architecture twice in one day and then rebuilt its
interaction model three times. If you are reading this months later and
wondering why something obvious is not here, the answer is usually that it was
here and was taken out.

| Cut | What it was | Why it went | Cost to restore |
|---|---|---|---|
| **The installed npm package** | a multi-file helper with a `serialport` dependency, 19 transitive packages and a native binding, shipped as a `.tgz`, with an `install` subcommand writing a launchd agent or a systemd unit | recipients are employees who may not be allowed to run a stranger's program on a work laptop. Every line of that install story was a reason to say no, and none of it bought anything the owner wanted | high, and it should not come back. "Unplug the board" is a complete uninstall now |
| **The 45 second stall heuristic for `waiting`** | inferred a pending prompt from an open turn going quiet, with a `--no-heuristic` off switch | the off switch was the tell: a signal you ship an off switch for is a signal you do not believe. Codex has a first-party `PermissionRequest` hook event | nothing to restore. The first-party event is strictly better |
| **Per-unit builds** | `tools/units.txt`, `-DUNIT_ID` and `-DUNIT_NAME` baked in, a flasher that force-removed `main.cpp.o` between units | fourteen images means fourteen hashes and a class of mistake (Alex's board with Jordan's name on it) that is invisible until somebody opens a box | the flags still work. `UNIT_BUILD_FLAGS='-DUNIT_ID=3' ./tools/flash-all.sh` compiles per unit, and that unit gets its own hash. Law 3 is the reason not to |
| **Five minutes of silence forcing `sleep`** | the device went to backlight duty 20 and a near-black screen | law 1 arriving late. On a desk with no host software, which is most of these units, the object was permanently dark, and a tray of flashed boards looked like a tray of dead ones | `{"state":"sleep"}` still reaches that look on demand. Only the automatic path is gone |
| **The ambient comet ring** | a faint full-ring track plus five arc segments totalling 76 degrees, lapping every 24 seconds | a small glowing ring on a desk that already has a monitor and a plant is furniture. Eye contact is the one image a human reads across a room without deciding to, and the two could not coexist at 148px and 104px in the same place | the code is gone. Ambient got 1.2ms faster losing it |
| **`error` and `celebrate` as protocol states** | `docs/design-spec.md` part 2 specifies seven states | the shipped protocol has five. `attention` became `waiting`, `done` got a 600ms flash instead of a celebrate sequence, and `error` was never implemented | `error` is the one genuinely open re-addition. It needs a hue no state owns, and violet and teal are both spoken for |
| **Button 1's brightness cycle** | brightness had two controls, button 1 with a wake-first case and button 2 without | the stats screen spent it. Brightness is button 2 alone now, at most two presses from anywhere, and opening the stats screen still wakes a dimmed panel | `docs/NEEDS-EYES.md` asks whether losing it is missed. Live with it for an afternoon before deciding |
| **Button 1's hold replaying the first run** | a two second hold replayed the out-of-the-box sequence | the game took it. That was the weakest binding on the board: every owner sees the first run once automatically, and it kept two other doors | both doors are still open: `{"firstrun":"play"}`, and the factory reset, which replays it to confirm itself |
| **The cross-cutting modal rule** | "any press takes down whatever is on top, except the toy" | two exceptions would not be a rule. It was replaced rather than exempted from: **button 2 leaves, button 1 is that screen's own action, and where a screen has no action it leaves too** | not a restoration question. The new rule is one sentence and covers all six screens |
| **The game's first physics constants** | a 30px hit box against 0.41s of jump clearance | it lost the clearance race at every speed. A bot playing over the cable, jumping on the exact frame, died on the first block every time. The shipped numbers give about 1.6x margin at the starting speed | the arithmetic is in `firmware/README.md`: an obstacle sits in the hit box for `(2*hitHalf + w)/speed`, and the jump must beat that **at the slowest speed**, because a slower obstacle sits there longer |
| **A per-unit npm install line on the printed card** | `npm install -g ./codex-companion-1.0.0.tgz`, and a QR encoding an install command | a QR that pastes a command into a coworker's terminal is a worse idea than one that opens a page they can read first | the card encodes a URL now, and `make-qr.sh` refuses to render while that URL is a placeholder |
| **The `arc` face drawn the way its reference draws it** | one filled polygon per stroke, walked as 88 anti-aliased wedges | 24ms a frame against a 33ms budget that already spends 14ms on `fillScreen` and `pushSprite`. Stamping filled discs along the ribbon costs 3ms for the same picture | the general form is worth keeping: on this hardware anti-aliased primitives are per-pixel float work over their own bounding box, and the cheap-looking call is often the expensive one |

**What would be worth adding next**, and neither needs a gesture: an `error`
state, which is the one thing design-spec specified and this device does not
have; and a `tokens`-driven weekly figure on the stats screen, since the device
already banks differences and already has a midnight. The button model is full,
so anything that wants a gesture has to displace something and write down what.

---

## 9. What is not proven

Honest gaps, because a document that only lists what works is not useful.

- **Nothing on the panel has been verified by an automated check.** Every
  visual claim in this repo is arithmetic. The serial port proves the protocol
  and says nothing about whether the thing looks like a gift.
  `docs/NEEDS-EYES.md` is the checklist and it is the gate before flashing
  fourteen units.
- **The full chain from a real Codex `PermissionRequest` to an amber board has
  not run end to end.** All eight event frames have been accepted and acked by
  real hardware, but driven by the test harness rather than by Codex. This is
  item N6 in `docs/open-questions.md` and it is the hero behaviour.
- **The context ring has never been shown a real number.** The one captured
  session on disk died on expired auth before any token accounting was written,
  so `token_count` and `token_usage_record` line shapes have never been
  observed in the wild. The parser is written against Codex's own structs and
  tested against a synthetic fixture, which proves it handles data shaped the
  way we believe real data is shaped, not that real data is shaped that way.
- **macOS only, in practice.** Port discovery parses `ioreg`. Nothing else in
  the helper is platform specific, but nothing else has been tried.

## 10. Where to read next

| Question | File |
|---|---|
| the wire format, field by field | `docs/protocol.md` |
| what the firmware does, all of it | `firmware/README.md` |
| the face interface, and writing one | `firmware/FACES.md` |
| the default face's geometry and blink model | `firmware/EYES.md` |
| ambient mode, the name, the buttons | `firmware/AMBIENT.md` |
| the out-of-the-box sequence | `firmware/FIRSTRUN.md` |
| what Codex writes to `~/.codex/` | `docs/codex-state-format.md` |
| the design the palette came from | `docs/design-spec.md` |
| what other people built, and what they got wrong | `docs/prior-art.md` |
| what is still unchecked | `docs/open-questions.md`, `docs/NEEDS-EYES.md` |
| flash day | `docs/wednesday-runbook.md` |
| third-party licences | `docs/licences.md` |
| building, flashing, testing, adding a face | `CONTRIBUTING.md` |
