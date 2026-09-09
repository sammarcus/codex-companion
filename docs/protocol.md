# Codex Desk Companion: serial protocol spec

Formal spec for the newline-delimited JSON protocol between the host helper
and the T-Display-S3 firmware, over native USB CDC serial at **115200 baud**.

Both ends are small enough to read in full, and both were read for this
document:

| End | File | Notes |
|---|---|---|
| device | `firmware/src/main.cpp` | the parser is `applyJsonLine`, fed by `pumpSerial` |
| host | `helper/codex-companion.js` | **one file, zero dependencies.** There is no `helper/src/` directory |

The frame builder (`frameToLine`, `metricFields`, `frameForEvent`), the port
picker (`choosePort`) and the transport (`class Link`) are all symbols inside
that one file.

**Citations name symbols, not line numbers**, on both ends. Function,
constant and field names survive edits that line numbers do not, so
`grep -n applyJsonLine firmware/src/main.cpp` and
`grep -n metricFields helper/codex-companion.js` are the intended way to
follow a citation here.

---

## 1. Transport

- USB CDC-ACM, native (not a UART bridge chip). Board USB ids: VID `0x303A`
  (Espressif), PID `0x1001`. The firmware side is the board's own descriptor;
  the host side is `ESP_VENDOR_ID` / `ESP_PRODUCT_ID` in
  `helper/codex-companion.js`, matched out of `ioreg` by `parseIoreg` and
  `choosePort`. `firmware/tools/flash-all.sh` matches the same pair as the
  string `303A:1001` against `pio device list --json-output`.
- Baud rate **115200**, agreed on both ends: `Serial.begin(115200)` in
  `setup()` and `monitor_speed = 115200` in `firmware/platformio.ini`
  (`upload_speed = 460800` is a flashing-only speed), `BAUD = '115200'` in the
  helper.
- **There is no serial library on the host.** A USB CDC port on macOS is an
  ordinary tty, so `Link.open()` does `fs.openSync(port, O_WRONLY | O_NOCTTY |
  O_NONBLOCK)` and configures the line with
  `/bin/stty -f <port> 115200 raw -echo`. Nothing native is compiled and no
  npm package is installed. Data bits, parity and stop bits are never set
  explicitly by either end, so they stay at the tty's defaults (8N1 on macOS)
  and are in any case ignored by native USB CDC, which is what the comment on
  `Link.open()`'s `stty` failure path says.
- The link is **write-only by default**. `Link` opens `O_RDWR` only when
  constructed with `read: true`, which only `doctor` and `selftest` do. This is
  deliberate:
  see the transmit quirk in section 3.3.
- Host to device carries state updates, device to host carries a greeting plus
  per-line acks. Both directions are newline-delimited (`\n`) UTF-8 text.
  There is no binary framing, no length prefix, no checksum.
- **Lines longer than 512 bytes are discarded whole, not truncated**
  (`LINE_CAP = 512` and the discard branch in `pumpSerial`). A line that
  overflows the buffer sets a discard flag and everything up to the next `\n`
  is dropped silently, so a partial write can never be parsed as a valid
  frame. The host knows this: `MAX_LINE_BYTES = 512` in
  `helper/codex-companion.js`, and `frameToLine` drops the `sub` field and
  re-serializes rather than emit a line the board would throw away.

---

## 2. Host to device: state line

Exactly one JSON object per line, terminated by `\n`. The device parses each
line independently with `ArduinoJson` (`applyJsonLine`); there is no
multi-line message.

### 2.1 Field table

| Field | Type | Range / allowed values | Firmware clamp | Default if never sent |
|---|---|---|---|---|
| `state` | string enum | `"sleep"`, `"idle"`, `"busy"`, `"waiting"`, `"done"`. Anything else makes the **whole line** malformed, see 3.4. | n/a | `idle` (see 2.4) |
| `ring` | number | `0.0` to `1.0`, a fraction and not a percentage (`0.62` means 62% fill, not `62`). Out-of-range values are clamped, not rejected (`clampf`, called from `applyJsonLine`). The host clamps first, in `clamp01`. | 0..1 | `0.0` |
| `center` | string | Big text inside the ring. **Firmware buffer is 16 bytes, so 15 usable characters** (`char center[16]`, truncated by `copyClamped`). What will not fit the ring's inner hole steps down a font size and is then truncated, so plan on roughly 4 characters at the large face and about 9 at the small one. Host clamps to the same 15 (`CENTER_MAX`). | 15 chars | `""` |
| `label` | string | Small text above the sub line, e.g. `"CTX"`. **Buffer 24 bytes, 23 usable characters** (`char label[24]`; host `LABEL_MAX = 23`). | 23 chars | `""`, but `setup()` seeds it to `"CODEX"`, see 2.4 |
| `sub` | string | Smallest line at the bottom, e.g. `"12:34 elapsed"`. **Buffer 40 bytes, 39 usable characters** (`char sub[40]`; host `SUB_MAX = 39`). Truncated, never wrapped. | 39 chars | `""` |
| `tps` | number | Tokens per second, `0` to `10000`, clamped (`clampf(v.as<float>(), 0.0f, 10000.0f)`). Shortens the `waiting` pulse period. The host emits it only when it is finite and greater than zero (`frameToLine`). | 0..10000 | `0.0` |
| `say` | string | **A short message, shown large on a card that takes the whole screen.** Sanitised rather than validated: printable ASCII survives, whitespace collapses, everything else is dropped, and the result is capped at **48 characters** (`SAY_MAX`). A value that sanitises to nothing, `""` included, clears the card. It can never make a line malformed. See 2.9. | 48 chars | unset: no card |
| `saysecs` | number | How long the card holds, in seconds, before it fades out on its own. `0` holds it until something clears it. Clamped to `0..3600`; a **negative** value is treated as though the field were absent, so a host arithmetic bug cannot clamp into the never-expires sentinel. On a line with no `say` it re-times the card that is already up. See 2.9. | 0..3600 | `30` (`SAY_DEFAULT_S`) |
| `dnd` | boolean | **The DO NOT DISTURB sign.** `true` puts it up, `false` takes it down. Optional, and the second door to a feature whose first door is a two-button chord on the board, exactly as `face` and `firstrun` mirror their holds. It can never make a line malformed: a boolean has no typo space, so a wrong-typed value is ignored like a wrong-typed `ring`. Not persisted, and not a protocol state: nothing about `state`, the staleness clocks or the face changes while it is up. See 2.10. | n/a | unset: no sign |
| `name` | string | **Owner name. Its effect outlives the frame.** Stored in NVS and used from that instant, reloaded on every boot. `""` clears the stored name. See 2.6. | 24 chars (`OWNER_NAME_MAX`) | unset: no stored name |
| `face` | string | **Which face is drawn. Its effect outlives the frame.** One of the registered wire names, `"rounded"`, `"bear"` or `"arc"`, matched case insensitively (`faceIndexByName`). Applied on the very next rendered frame, mid-animation, with no reboot, and stored in NVS beside the owner name. Anything not registered makes the **whole line** malformed, exactly like `state`, see 3.4. See 2.8. | n/a | unset: the compile-time `DEFAULT_FACE`, which is `rounded` |
| `reset` | string enum | `"factory"` clears the entire NVS namespace (owner name, face, first-run flag). `"firstrun"` re-arms the first run only and leaves the name and face alone. Anything else makes the **whole line** malformed, like `state`. **Applied before every other field on the line.** See 2.7. | n/a | never sent: nothing is reset |
| `firstrun` | string enum | `"play"` plays the out-of-the-box sequence now. Never writes the stored flag, so a rehearsal cannot consume an armed board. Refused while the effective state is `busy` or `waiting`. Anything else makes the whole line malformed. **Applied after every other field.** See 2.7. | n/a | never sent: nothing plays |
| `play` | string enum | **The toy.** `"on"` opens it, `"off"` closes it, `"hop"` jumps. Anything else makes the **whole line** malformed, like `state`. `"on"` is refused while a prompt is pending; `"off"` with nothing open and `"hop"` with no game up are both accepted and do nothing, exactly like a `saysecs` with no card up. Not persisted (the personal best is, but that is not a protocol value), and not a protocol state: nothing about `state`, the staleness clocks or the face changes while it is up. See 2.11. | n/a | never sent: no game |
| `time` | number | **The host's local wall clock, in seconds.** Unix time plus the host's own UTC offset, not UTC: the only question the device asks of it is whether midnight has happened where the owner is sitting, and a device with no timezone database cannot answer that from UTC. Optional, and it can never make a line malformed: anything below 2020-01-01 (`CLOCK_MIN_EPOCH`) or above the 32-bit range is ignored exactly as a wrong-typed `ring` is. Without it there is no midnight, so the daily figures run from boot and the stats screen says `SINCE BOOT` instead of `TODAY`. See 2.12. | must be >= 1577836800 | never sent: the device has no clock |
| `tokens` | number | **The current Codex session's cumulative token total.** The device banks the DIFFERENCES, never the value, so a value lower than the last one is read as a new session and counted whole, and the first value a board ever sees is a baseline that contributes nothing. Optional, never malformed, and negatives and NaN are ignored. Without it every other figure still counts and the token figures stay at zero. See 2.12. | 0..2^32-1, saturating | never sent: token figures stay at 0 |
| `stats` | string enum | **The stats screen.** `"on"` opens it, `"off"` closes it. Anything else makes the **whole line** malformed, like `state`. `"on"` is refused while a prompt is pending; `"off"` with nothing open is accepted and does nothing. The second door to a feature whose first door is a tap on button 1. Not persisted, and not a protocol state. See 2.12. | n/a | never sent: no screen |
| `focus` | string enum | **The focus timer.** `"show"` and `"hide"` move its screen, `"start"`, `"pause"` and `"reset"` drive its clock. Anything else makes the **whole line** malformed, like `state`. The clock is never refused; only the screen is, and only while a prompt is pending. `"start"` also opens the screen, because that is what the button does. The second door to a feature whose first door is the two buttons. Not a protocol state: nothing about `state`, the staleness clocks or the face changes while it is up. See 2.13. | n/a | never sent: no timer |
| `focusmins` | number | **The timer's length in minutes. Its effect outlives the frame.** Stored in NVS beside the owner name and the face, and written only when it actually changes. A **number**, so it takes the `ring` rule and not the enum rule: a wrong type is ignored and the rest of the line still applies, and a value outside the range is clamped rather than rejected. It never rewinds a run in flight; a new length applies from the next reset. See 2.13. | 1..180, clamped | unset: the stored length, `25` on a fresh board |

Any field not present in a line is left untouched, see 2.3. Any key that is
not one of the nineteen above is ignored rather than treated as malformed, which
is inherent to how `ArduinoJson`'s `doc["field"]` lookups work: a key the
parser never asks for is simply never read.

Nineteen is checkable: `grep -c 'doc\["' firmware/src/main.cpp` returns 19,
one lookup per field. They fall into two groups, and the split is the whole
privacy argument in one line:

- **Eight are emitted by the shipped helper's hook path**, and only these
  eight: `state`, `ring`, `center`, `label`, `sub`, `tps`, `time` and
  `tokens`. Numbers, timestamps and one of five fixed state words. A running
  Codex session can never reach any of the other eleven.
- **Eleven are driven by a person**, by hand or through one of the helper's
  direct-to-board subcommands: `name` and `face` (persisted to NVS), `say` and
  `saysecs` (the message card), `dnd` (the sign), `play` (the toy), `stats`,
  `focus` and `focusmins` (the timer), `reset` and `firstrun`.

Each of the eleven repeats that fact in its own section below, because a field
table is read one row at a time.

### 2.2 Example line

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
```

The same shape appears in `firmware/README.md`'s "Feeding it data by hand"
section and in `firmware/tools/sim.py`.

### 2.3 Keep-last-value semantics

The device holds one persistent struct (`struct State`, instantiated as
`static State st;`). On receiving a syntactically valid JSON object line:

1. For each known key, if the key is present **and not JSON `null`** and is
   the expected type, overwrite the struct field. Every field write is gated
   by `!v.isNull() && v.is<T>()`.
2. If the key is absent, or `null`, or the wrong type (`"ring":"high"`), the
   field is left untouched. `null` and absent are identical by construction.
3. This is a per-field merge, not a per-line replace. A line containing only
   `{"tps":22.1}` updates `tps` and leaves everything else exactly as it was.
4. A JSON object with **no** recognized keys at all (`{}`, or an object of
   unknown keys) is still well-formed: it gets an `ok` and changes nothing.
   The comment in `applyJsonLine` calls this out as the intended keepalive,
   and `firmware/tools/flash-all.sh` uses exactly `{}` to nudge the greeting
   out of the board (section 3.3).
5. The merge happens once per accepted line, synchronously, immediately before
   the `ok` goes out (section 3).

`name` follows the same gating but has an extra effect, see 2.6.

### 2.4 Boot, and what a board with no host actually shows

`struct State`'s in-code initializers are the defaults:

```cpp
struct State {
  StateId state = ST_IDLE;
  float   ring  = 0.0f;
  float   tps   = 0.0f;
  char    center[16];   // "" in setup()
  char    label[24];    // "CODEX" in setup()
  char    sub[40];      // "" in setup()
};
```

**Those defaults are almost never what you see.** The device has an ambient
mode, and the live view (the five states, the ring, the centre text) renders
only while a host is actually talking to the board. `ambientActive` returns true
whenever `hostEverSpoke` is false, so a board that has never received a single
accepted line is in ambient mode from the end of the boot screen onward, and
stays there indefinitely.

Concretely, on a board plugged into a charger with no software anywhere:

| Time since boot | What is on the panel |
|---|---|
| 0 to 2s (`BOOT_HOLD_MS`) | boot screen: product name, owner name if one is stored, unit id |
| 2s onward | **ambient**: the face. Two eyes centred at (160, 62), each a 56 x 64 rounded rect with a 36px gap, blinking on a randomised interval and glancing around, riding a 6500ms brightness breathe, colour drifting over 45s through two blues and a teal, plus the name and product lines under it |
| 60s (`AMBIENT_BRIGHT_MS`) | ambient continues, backlight settles from full to the `70` tier |
| forever | ambient. It never reaches the `20` sleep tier and it never stops moving |

The stored `idle` / `CODEX` default is what the board falls back to the moment
a host speaks and then says nothing further, not what an unattached board
shows. `firmware/AMBIENT.md` is the full description of that mode.

**An unattached board never enters the live view at all, and never reaches the
sleep tier.** `ST_SLEEP` is reachable only when a host explicitly sends
`{"state":"sleep"}`.

### 2.5 Staleness, once a host has spoken

These timers are measured from `lastDataMs`, which `setup()` seeds at boot and
`pumpSerial` refreshes on every accepted line. They only govern the live view,
which requires `hostEverSpoke`.

| Elapsed since last accepted line | Behaviour | Symbols |
|---|---|---|
| under 30s (`STALE_DIM_MS`) | Render per the current struct. Backlight full unless the state tier says otherwise. | `updateBrightness` |
| 30s to 5min | **Dim** to `70`/255 (`BRIGHT_DIM_IDX`, index 2 of `BRIGHT_LEVELS = {255,160,70,20,0}`; the `0` rung is only ever
reached by hand, from button 2). One content change rides along: `effectiveState` renders a stored `busy` or `waiting` as the **idle look**, so a dead host cannot leave a board pulsing red for an approval prompt that no longer exists. `sleep`, `idle` and `done` render unchanged, and `st.state` is untouched either way. | `effectiveState`, `updateBrightness` |
| 5min or more (`AMBIENT_AFTER_MS`, 300000) | **Back to ambient mode**, cross-faded over 700ms (`MODE_XFADE_MS`), then the ambient brightness rule takes over (full for 60s, then `70`). | `ambientActive`, `updateMode` |
| a line arrives after either | The staleness tier clears on the next check and the live view resumes immediately. Coming back from ambient is a 700ms cross-fade that unwinds the burn-in drift to exactly zero, so the live layout lands on its authored geometry. | `updateMode`, `xf()` |

**Two brightness inputs, dimmer wins.** In live mode `updateBrightness`
computes a staleness tier and a state tier and takes the higher:

| Effective state | State tier | Floor it imposes |
|---|---|---|
| `sleep` | 2 | `20`/255 immediately |
| `idle` held `IDLE_DIM_MS` (15000ms) or longer | 1 | `70`/255 |
| anything else | 0 | full |

In ambient mode that whole ladder is bypassed: the tier is 0 for the first
`AMBIENT_BRIGHT_MS` and 1 thereafter, and tier 2 is unreachable.

A manual brightness press pins the level (`brightManual`) until the combined
auto tier itself changes, at which point `updateBrightness` clears the pin.
Button 2 (GPIO 14) is a plain one-press, one-step brightness control. Button 1
(GPIO 0) is no longer a second copy of it: its release tries `statsEnter` first
and only falls back to the old wake-then-cycle when the stats screen is refused,
which happens in exactly one case, a prompt already pending. That trade is the
subject of `firmware/README.md`, "The button model, again".

**The host keeps a live board out of the staleness tiers on purpose.**
`cmdRun` polls once a second and writes a line either when the serialized
frame changed or when 2000ms have passed since the last write, whichever comes
first. That 2-second floor is well inside the 30s dim window. The hook path
writes one line per Codex event instead, which is bursty, so between two quiet
events a board can and will dim; that is the timers working.

### 2.6 The `name` field

The recipients' names are not known at flash time, so the name is runtime
state on the device rather than a build flag. Every unit in the fleet is
flashed with the byte-identical image and named afterwards over the wire.

```sh
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodemXXXX   # set
printf '{"name":""}\n'            > /dev/cu.usbmodemXXXX   # clear
```

Behaviour, from `setOwnerName` and `loadOwnerName`:

- Stored in NVS via Arduino `Preferences`, namespace `codexbuddy`, key
  `owner`. `loadOwnerName()` runs in `setup()` before the boot screen draws,
  so the first frame after power-on already carries the right name.
- **Capped at 24 characters** (`OWNER_NAME_MAX`), silently truncated past
  that. Non-ASCII does not render; the built-in LovyanGFX bitmap fonts have no
  glyphs for it.
- **An empty string clears it**, removing the key rather than storing a blank.
- **Writes are idempotent.** `setOwnerName` compares against the RAM mirror
  and returns early when they match, so nothing touches flash unless the value
  actually changed. A host that repeated `"name"` in a 2-second heartbeat
  would otherwise rewrite the key about 43,000 times a day.
- If NVS cannot be opened for writing, the name still applies for this power
  cycle and the board prints `err: nvs open failed, name not persisted`
  instead of pretending it stuck.
- Precedence, highest first: the NVS name, then a `-DUNIT_NAME` build-time
  default if the image was built with one, then no name (the screens show the
  product name and the unit id). `activeName()` is the single resolver.
- The line is acked with `ok` like any other, and a malformed or non-string
  `name` is ignored exactly like a non-string `ring`.

**The shipped helper's hook path never sends this field.** That path emits
only `state`, `ring`, `center`, `label`, `sub`, `tps`, `time` and `tokens`, so
a running session can never rename a board. The helper does have a dedicated
subcommand for it, `codex-companion name "Alex Rivera"` (add `--dry-run` to see
the exact line), which is the same one-field write the `printf` below performs.
Naming is an operator or owner action either way, and the greeting (3.1) is how
you read back what it did.

### 2.7 `reset` and `firstrun`

The very first time an owner powers a board it plays an out-of-the-box
sequence instead of settling straight into ambient, once, and then never
again. These two optional fields are how a script drives that, and they exist
because Sam powers every unit several times while flashing and boxing it: the
last thing done to a board has to be able to re-arm it.

Full description of the sequence, the button gestures and the flashing
procedure is in `firmware/FIRSTRUN.md`. What matters on the wire:

```sh
printf '{"reset":"factory"}\n'  > /dev/cu.usbmodemXXXX   # wipe everything
printf '{"reset":"firstrun"}\n' > /dev/cu.usbmodemXXXX   # re-arm, keep the name
printf '{"firstrun":"play"}\n'  > /dev/cu.usbmodemXXXX   # play it now
```

Behaviour, from `applyJsonLine`, `factoryReset`, `armFirstRun` and
`startFirstRun`:

- **Both are validated before anything is written**, in the same pre-validation
  block as `state` and `face`, so a line carrying a typo in any of the four is
  dropped whole rather than half applied. `"Factory"` is not `"factory"`.
- **`reset` is applied first**, before `state`, `face`, the text fields and
  `name`. One line can therefore wipe a board and then rename it:
  `{"reset":"factory","name":"Alex Rivera"}` ends up blank-then-named.
- **`"factory"` calls `Preferences::clear()`** on the whole `codexbuddy`
  namespace rather than removing three known keys, so it cannot silently stop
  being a full reset when a later firmware adds a key. The RAM mirrors are
  reset to exactly what a virgin board would load, with no reboot.
- **`"firstrun"` removes only the `firstrun` key.** Absence is the armed state,
  so re-arming is a removal rather than a value.
- **`firstrun` is applied last**, because it takes the screen for 8.6 seconds
  and everything else on the line should already be in place when it settles.
- **A play never writes the stored flag**, in either direction. Only a real
  out-of-the-box run marks itself spent, and it does so at the *end* of the
  sequence, so a board unplugged halfway through is still armed.
- **A play is refused** while the effective state is `busy` or `waiting`. The
  board replies `err: firstrun refused, session is active` and nothing happens.
  Hiding a running turn or a pending approval prompt behind an animation is the
  one thing this device must not do.

**The first-run flag survives a reflash**, which is the whole point of storing
it. `pio run -t upload` writes the bootloader (`0x0`), the partition table
(`0x8000`), `boot_app0` (`0xe000`) and the app (`0x10000`); the `nvs` partition
is at `0x9000` and is in none of them. Verified on hardware, not assumed.

**The shipped helper's hook path never sends either field**, exactly like
`name`. `reset` is the one field in this spec that **no** code path in the
helper ever emits, on purpose: it is destructive, and `help` prints the
`printf` line rather than offering a command for it. `firstrun` does have a
subcommand, `codex-companion firstrun`, which emits `{"firstrun":"play"}`.
These are operator actions either way, done with `printf`, that subcommand, or
`firmware/tools/flash-all.sh`.

### 2.8 The `face` field

The face is the whole product, and it has been redrawn several times. It is
therefore not compiled in: the drawing lives behind the interface in
`firmware/src/Face.hpp`, three designs implement it, and which one a board
draws is runtime state stored beside the owner name. Swapping one costs a
protocol line, not a reflash.

```sh
printf '{"face":"bear"}\n'    > /dev/cu.usbmodemXXXX
printf '{"face":"rounded"}\n' > /dev/cu.usbmodemXXXX   # back to the default
```

Behaviour, from `applyJsonLine`, `faceIndexByName` and `setFaceIndex`:

- **The registered names are `rounded`, `bear` and `arc`**, matched case
  insensitively, so `{"face":"BeAr"}` is accepted. `faceCount()` and
  `faceAt()` in `firmware/src/Faces.cpp` are the only list.
- **An unregistered name makes the whole line malformed.** It is validated in
  the same pre-validation block as `state`, before anything on the line is
  written, so `{"state":"done","label":"X","face":"nope"}` changes nothing at
  all rather than applying the state and then giving up. Dropped silently, no
  `ok`. A non-string value is ignored like any other wrong-typed field and the
  rest of the line still applies.
- **It takes effect on the very next rendered frame**, mid-animation, with no
  reboot and no visible seam: the blink and glance drivers, the state machine
  and the geometry all live outside the face, so a swap is a pointer change.
- **Stored in NVS**, namespace `codexbuddy`, key `face`, as the wire NAME
  rather than an index, so the registry can be reordered without repointing
  boards that are already set. A stored name that a later firmware no longer
  registers falls back to the compile-time default.
- **Writes are idempotent**, for the same reason `name`'s are: `setFaceIndex`
  returns early when the value is already active, so a host that put `"face"`
  in a 2000ms heartbeat would never touch flash.
- Precedence at boot: the stored name, then `DEFAULT_FACE` (a compile-time
  default, `"rounded"`, not set in `platformio.ini`), then the first
  registered face so a misspelled default cannot leave a board blank.

**Nothing about the device to host protocol changed.** The greeting is still
byte-for-byte `hello tdisplay-s3 v1 name="<stored name>"` and there is still
exactly one `ok` per accepted line. The active face is deliberately not on the
wire: two other programs match on that greeting and neither should have to
learn anything new. It is also reachable without a host at all, by holding
button 2 for 800ms.

Full description of the interface, the geometry contract and how to write a
new face is in `firmware/FACES.md`.

**The shipped helper's hook path never sends this field**, exactly like
`name`, `reset` and `firstrun`. `codex-companion face rounded|bear|arc` is the
subcommand that does.

---

### 2.9 `say` and `saysecs`: the message card

A short message, pushed to the panel and readable across a room. "brb", "in a
meeting", "shipping". It is a sign rather than a status line, so it takes the
whole screen for as long as it is up: the face and the ring step aside and
come back on their own.

```sh
printf '{"say":"brb"}\n'                            > /dev/cu.usbmodemXXXX
printf '{"say":"in a meeting","saysecs":0}\n'       > /dev/cu.usbmodemXXXX
printf '{"saysecs":300}\n'                          > /dev/cu.usbmodemXXXX
printf '{"say":""}\n'                               > /dev/cu.usbmodemXXXX
```

#### What reaches the panel

The string is sanitised, capped, then typeset. All three steps are reported
back on the wire (see 3.2a), because none of them are guessable from the host
side and nobody can read a panel over USB.

1. **Sanitise.** Printable ASCII, `0x20` to `0x7E`, survives. Tabs and
   newlines become spaces, runs of whitespace collapse to one, and the ends
   are trimmed. Control bytes and **every byte `>= 0x80`** are dropped, which
   means an accented letter or an emoji disappears rather than rendering as
   mojibake: the fonts on the device are ASCII and there is nothing honest to
   draw. `"café crème 🚀"` arrives as `caf crme`.
2. **Cap.** 48 characters (`SAY_MAX`). Anything past that is thrown away and
   the card is marked as cut.
3. **Typeset.** A four rung ladder of FreeSansBold, largest first, and the
   first rung on which the message fits wins:

   | Rung | Face | Line height | Lines allowed |
   |---|---|---|---|
   | 1 | `FreeSansBold24pt7b` at text size 2 | 112px | 1 |
   | 2 | `FreeSansBold24pt7b` | 56px | 2 |
   | 3 | `FreeSansBold18pt7b` | 42px | 3 |
   | 4 | `FreeSansBold12pt7b` | 29px | 3, and it takes whatever is left |

   Wrapping is greedy and **measured, never counted**: every candidate line is
   put through `textWidth` at the rung's own font, because these faces are
   proportional and `WWWWWWWW` is not eight of anything `iiiiiiii` is. A word
   too wide for a whole line sends the layout down a rung; a word too wide at
   the last rung is hard-broken at the last character that fits.

Text is thrown away in exactly two places, the 48-character cap and the line
limit of the last rung, and either one earns a **measured ellipsis** on the
last line: characters come off until the line plus `...` fits. A message that
hits both still gets exactly one ellipsis.

Measured on the board, `lines=` and `font=` as the device itself reported
them:

| Sent | On the panel | Rung |
|---|---|---|
| `brb` | `brb` | `24x2` |
| `shipping` | `shipping` | `24` |
| `in a meeting` | `in a` / `meeting` | `24` |
| `deploying to prod, do not unplug` | `deploying to` / `prod, do not` / `unplug` | `18` |
| `supercalifragilisticexpialidocious` | `supercalifragilisticexpi` / `alidocious` | `12` |
| 84 characters of prose | `this message is` / `far longer than` / `the forty eight...` | `18` |

#### How long it stays

`saysecs` is the settled hold in seconds, default 30, clamped to an hour. The
entrance and exit fades sit outside it, so a 3 second card is on the panel for
about 3.5 seconds in total.

`saysecs: 0` means **hold until something clears it**. That something can be a
protocol line, or a button, and it does not have to be a host: pressing either
button dismisses the card and does nothing else, which is what makes an
open-ended card safe to offer at all.

A **negative** `saysecs` is treated as though the field had not been sent, and
falls back to the 30 second default. It deliberately does not clamp to zero:
zero is the never-expires sentinel, and a host that computed `-1` by accident
should not get a card that never leaves.

`saysecs` on a line with no `say` re-times the card that is already up, from
that moment, without making it fade in again. That is how a host extends "in a
meeting" without a visible flicker. With no card up it is accepted and does
nothing.

#### What it yields to, and what it never covers

The card **yields the screen to a pending prompt.** While the effective state
is `waiting`, the normal live view is drawn and the card waits; when the
prompt is answered the card takes the screen back for whatever is left of its
hold. It yields to the out-of-the-box sequence the same way.

**Its clock runs while it is yielding.** A 30 second message sent during a
five minute approval prompt is gone before the prompt is, rather than
appearing, stale, at the moment the human looks away from the screen. Verified
on hardware: a 2 second card sent during `waiting` was accepted, announced,
and expired 2.5 seconds later without ever being drawn.

The card is RAM only. It is not persisted, it survives no power cycle, and it
has nothing to do with NVS.

#### Neither field can break a line

`say` is a message typed by a person, so it is sanitised rather than
validated: dropping a tab is a better answer than dropping the whole line.
`saysecs` is a number, so it is clamped like `ring` and `tps`. Neither can
make a line malformed, and a wrong-typed value in either is ignored while the
rest of the line still applies (`{"say":123,"label":"CTX"}` sets the label).
A malformed enum elsewhere on the line still drops everything, `say` included,
because the enums are all validated before anything is written:
`{"say":"hi","state":"nope"}` shows nothing and is not acked.

#### On the panel

A rounded frame inset from the edge, the message centred in it, and a thin bar
along the bottom that drains over the hold so the card visibly has an end. A
card with no expiry has no bar, because nothing is counting down. The frame
and the bar borrow the colour of whatever the card is covering: the drifting
ambient blue on an idle desk, amber during a busy turn.

Nothing moves after the 220ms entrance except that bar and the anti burn-in
drift, which the card applies at **full amplitude in both modes**, unlike the
live composition that holds still. `saysecs: 0` is the one thing in this
firmware that can legitimately hold the same white pixels for an hour.

**The shipped helper's hook path never sends either field**, exactly like
`name`, `face`, `reset` and `firstrun`. `codex-companion say "back in five"
[--secs 300]` and `codex-companion say --clear` are the subcommands that do.

### 2.10 `dnd`: the do-not-disturb sign

A sign for the room. It is the only screen on this device aimed at somebody
walking past rather than at the owner or at a session, and it is the only one
that works as a complete feature with no host in the building: the field
documented here is the second door to it, and the first is a chord on the two
buttons (`firmware/README.md`, "Do not disturb").

```sh
printf '{"dnd":true}\n'   > /dev/cu.usbmodemXXXX
printf '{"dnd":false}\n'  > /dev/cu.usbmodemXXXX
```

#### It is not a state

`state` is what Codex is doing. `dnd` is what the human wants the room to
know, and the two are orthogonal: putting the sign up does not touch
`st.state`, `waitEnterMs`, the staleness clocks, the ack, the face, the stored
name or NVS. The device carries on knowing exactly what it knew and puts a
sign in front of itself. Take the sign down and the live view is where it was.

#### On the panel

A violet frame 8px thick, the words `DO NOT` and `DISTURB` in white
FreeSansBold, and the owner's name under them when the board has one. No face,
no ring, no metric, no countdown bar.

The face is this device's entire vocabulary for "alive and working on
something", so the surest way to say *not now* is for the creature to be
absent rather than for it to pull a face about it. The frame is what survives
distance: the panel is 44mm wide, so at the 24pt rung the cap height is about
4.6mm and the words give out somewhere past two metres, while a 296x154 violet
rectangle is still a violet rectangle across an open-plan floor. Violet
(`C_DND`, `0xA35F`) is a hue no state owns, and it is deliberately far from the
amber and red that mean "look at me now" here.

The type is solved once, when the sign goes up, and the measured widths are
reported on the wire because nobody can read a panel over USB:

```
{"dnd":true}
dnd: on (host) font=24 w=186,213 max=268
ok
```

Two rungs, and the second exists only so a future edit to the words cannot
silently overflow the frame:

| Rung | Face | Line height | Fits |
|---|---|---|---|
| 1 | `FreeSansBold24pt7b` | 56px | both words under 268px |
| 2 | `FreeSansBold18pt7b` | 42px | whatever is left |

Measured on the board: `DO NOT` is 186px and `DISTURB` is 213px against a
268px working width, so the shipped words land on rung 1 with 55px to spare.

Nothing moves except a 6500ms breathe on the frame and the anti burn-in drift,
which this composition applies at **full amplitude in both modes** for the
same reason the message card does: it is the other screen here that can
legitimately hold the same pixels until somebody comes back from lunch. The
words themselves are rock steady. The backlight follows the ambient ladder
rather than the live one, full for 60 seconds and then a tier down for as long
as the sign lasts.

#### What it yields to

The same rule as the message card, for the same reason: **nothing on this
device may be the reason somebody missed the question.**

| On screen | Wins |
|---|---|
| the out-of-the-box sequence | the sequence |
| a pending prompt (effective state `waiting`) | the prompt |
| a message card | the card |
| anything else | the sign |

Yielding does not cancel the sign, it covers it, and it comes back on its own.
Verified on hardware: with the sign up, `{"state":"waiting",...}` put the
frame counter into `mode=live`, and `{"state":"done"}` put it back into
`mode=dnd` with no further line. A card sent over the sign showed `mode=say`
for its hold and `mode=dnd` again when it expired.

A card and a sign together therefore take two presses to clear, which is the
same single rule twice: **a press takes down whatever is on top.**

**The shipped helper's hook path never sends this field**, exactly like
`name`, `face`, `reset`, `firstrun` and `say`. That path emits only `state`,
`ring`, `center`, `label`, `sub`, `tps`, `time` and `tokens`, so a host written
against an older build of this spec is unaffected by the field existing.
`codex-companion dnd on|off` is the subcommand that sends it.

#### It is not persisted

The sign is RAM only. A power cycle clears it, exactly like the message card
and unlike `name`, `face` and the first-run flag.

That is a decision, not an omission. Unplugging a desk companion and plugging
it back in is a thing a person does *at the desk*, which is the one moment a
DO NOT DISTURB sign is provably stale. Persisting it would also mean a board
that arrives in its box, or comes back from a colleague's desk, can come up
holding a sign nobody in the room set, with no explanation and no host. The
mode is one press to leave in any case, so persistence would buy an edge case
and cost a wrong first frame.

#### Entering it needs two buttons; leaving it needs one

Full reasoning in `firmware/README.md`. The short version, because it is a
protocol-visible behaviour too: a stray press can never put the sign up (it
takes both switches, at opposite ends of the board, held together for 250ms
and released before the five second factory reset), and any single press takes
it down. The asymmetry is the entire safety argument, and it is the answer to
the failure this repo already documented in
`docs/prior-art.md`: a latched mode with no event that clears it.

### 2.11 `play`: the toy

A Codex turn can run for ten minutes, and for those ten minutes the person in
front of it has nothing to do. The two buttons carry a game: the active face
runs along a line and hops over blocks, and the best score is kept in NVS.

Its first door is a button hold (`firmware/README.md`, "Something to do"). This
field is the second, exactly as `dnd`, `face` and `firstrun` mirror their own
gestures.

```sh
printf '{"play":"on"}\n'  > /dev/cu.usbmodemXXXX
printf '{"play":"hop"}\n' > /dev/cu.usbmodemXXXX
printf '{"play":"off"}\n' > /dev/cu.usbmodemXXXX
```

**The shipped helper's hook path sends none of the three**, and an older host
that has never heard of the field is unaffected in every direction.
`codex-companion play on|off|hop` is the subcommand that sends them.

#### Why `"hop"` is on the wire at all

Because nothing in a test harness has a finger. `"hop"` is how the game itself
was exercised on real hardware: a bot on the far end of the cable played it for
ninety seconds and scored 52, which is what proved the collision, the scoring,
the speed ramp and the stored best. A host that wants to play from a keyboard
gets that for free. It cannot approve or deny anything, and it cannot change
what the device believes about the session.

#### It is not a state

Nothing about `state`, `ring`, `tps`, the staleness clocks, the escalation
timer or the face changes while the game is up. The device carries on knowing
exactly what it knew; it just puts a different picture in front of itself, the
same way the message card and the sign do.

#### What it yields to, and what ends it

| Event | What happens |
|---|---|
| `waiting` becomes the effective state | **the game ends**, on that frame. The score is banked and the live view is back |
| `{"play":"on"}` while `waiting` is pending | refused, with `err: play refused, prompt pending` |
| a message card or the sign goes up | the game **pauses** under it, frame for frame, and resumes on a cleared stretch with the score intact |
| the out-of-the-box sequence starts | the game is banked and closed |
| thirty seconds with no press | the game closes itself and the board goes back to ambient |

The `waiting` rule is deliberately harsher than the message card's. A card is
covered and restored; the game is ended. After answering a prompt somebody is
back at their keyboard, and a game reappearing over their answer would be the
same failure in a different costume.

#### The personal best

Kept in NVS beside the owner name and the face, in the same `codexbuddy`
namespace, so `{"reset":"factory"}` takes it with everything else and
`{"reset":"firstrun"}` leaves it alone. It is written **only when a run beats
it**, which is at most once per game and usually far less: nothing here writes
flash on a timer. It is not readable over the protocol, but every `play:`
notice carries it.

### 2.12 `time`, `tokens` and `stats`: the numbers

The device has been watching every turn go past since the first firmware.
Nothing was counting them. This is the counting, the screen that shows it, and
the two flourishes that mark the moments worth marking.

#### What the device works out, and what it has to be told

| Figure | Where it comes from |
|---|---|
| turns today | counted here, off the state machine's own edges |
| longest turn today, and the longest ever | timed here, from those same edges |
| the current session's length | timed here, from host contact |
| tokens today, and the best token day | **told**, by `tokens` |
| what day it is | **told**, by `time` |

Turns and elapsed time are the device's own arithmetic because it already sees
every edge it needs: a turn opens on the first `busy` (or on a `waiting` that
precedes one) and closes on the `done` that completes it. A turn that ends
anywhere else was abandoned, and an abandoned turn is not counted.

Tokens are not derived, deliberately. The wire carries `tps` and two display
strings, and integrating a rate sampled at hook events would produce a number
nobody could reconcile with what Codex tells them. So the host sends the
session total and the device banks the differences.

#### `time`

Local seconds, not UTC. The host sends `Date.now()` shifted by its own
`getTimezoneOffset()`, so a laptop carried across a timezone or through a
daylight-saving change corrects itself on the next hook event with nothing
stored anywhere.

The device stores the value with the `millis()` it arrived at and extrapolates,
so a host that stops talking does not stop the day: a board told the time once
at 09:00 still knows when midnight is.

**A board that is never told the time still works.** There is simply no
midnight: the daily figures run from boot, and the screen says `SINCE BOOT`
where it would otherwise say `TODAY`. The first clock a board is ever told is
**adopted** rather than treated as a new day, so a host that connects at
lunchtime does not throw away the morning the board already watched.

#### `tokens`

The current Codex session's cumulative total, which is exactly what the helper
already reads out of the rollout file (`total_token_usage`). Three rules, all
in `statsAddTokens`:

- the **first** value a board ever sees is a baseline and contributes nothing.
  A board plugged in halfway through a session did not watch those tokens
  happen and must not claim them.
- a value **higher** than the last adds the difference.
- a value **lower** than the last is a new session, and the whole of it is new.

Nothing about this needs the host to remember anything between hook
invocations, which is what keeps the helper a stateless one-shot program.

Two sessions driving one board at the same time will interleave and inflate the
count. That is a known and accepted limit: the device has one cable and no way
to tell two hosts apart.

#### What is persisted

**Records only**: the longest single turn ever, and the most tokens in one day.
They live in NVS beside the owner name, the face and the hop best, in the same
`codexbuddy` namespace, so `{"reset":"factory"}` takes them and
`{"reset":"firstrun"}` leaves them alone. Flash is touched only when a record
actually moves, from a completed turn or from midnight, and never on a timer or
per line.

**The daily figures are RAM and do not survive a reboot.** That is a decision.
A reboot is not a midnight, and a board cannot know whether counters it loaded
belong to today or to a Tuesday three weeks ago; showing yesterday's number
under the word TODAY is worse than showing a zero.

#### The milestones

| Threshold | Fires | Reads |
|---|---|---|
| a completed turn of 5 minutes or more | once, on the frame it completes | `LONG TURN 5:12` |
| a completed turn that beats the stored longest, and is at least 60 seconds | once | `LONGEST YET 8:40` |
| tokens today crossing 250k, 500k, 1M, 2.5M, 5M or 10M | once per rung per day | `TOKENS TODAY 1M` |

The 60-second floor stops a virgin board celebrating its first four-second turn
as a personal best. A record supersedes the long-turn flourish, so one
completed turn can never fire two, and a single line that crosses several token
rungs fires only the highest one. The ladder deliberately ends: past ten
million the device says nothing more.

A flourish is a pill of text that fades in over 250ms, rising seven pixels as
it arrives, then sits **absolutely still** for a second and a half and fades
out. 2200ms end to end. No strobe, no takeover, and never amber or red, which
are the two colours on this device that mean "look at me now": a turn is
`done` green and tokens are the ambient teal.

It is an overlay on the live and ambient compositions and on nothing else.
Every modal screen outranks it, it does not draw at all while a prompt is
pending, and its clock runs underneath a message card rather than queueing, so
a flourish nobody could see is missed rather than saved up for later.

#### `stats`: the screen

Six figures: turns today and tokens today large, then today's longest turn, the
current session's length, the longest turn ever and the best token day. The top
right says `TODAY` or `SINCE BOOT`, and the bottom says `1 FOCUS    2 CLOSE`
(`renderStats`). `PRESS TO CLOSE` is a different screen's legend: it belongs to
the focus timer's finish, in `renderFocus`'s `focusFin` branch.

It takes the device's rule for modal screens, which is **button 2 leaves,
button 1 is that screen's own action**: button 2 closes it, and button 1 goes
*on* to the focus timer rather than closing (`statsExit` then `focusShow` in
`pumpButton`). "Any press takes it down" is wrong and would send somebody
pressing button 1 to leave into a 25 minute pomodoro. It closes itself after
twenty seconds, and a pending question closes
it the way a question ends the toy rather than covering it the way one covers a
message card.

| Event | What happens |
|---|---|
| button 2, one press | closed |
| button 1, one press | closed, and the focus timer opens |
| `waiting` becomes the effective state | closed, on that frame |
| `{"stats":"on"}` while `waiting` is pending | refused, with `err: stats refused, prompt pending` |
| a message card or the sign goes up | the screen yields under it and comes back |
| twenty seconds with nothing pressed | closed |
| the out-of-the-box sequence starts | closed |

On the board its first door is a **tap on button 1**, which is what that tap
does whenever there is no prompt to acknowledge. `firmware/README.md`, "The
button model, again", is the argument for that trade.

### 2.13 `focus` and `focusmins`: the focus timer

A pomodoro that is driven entirely from the two buttons and needs no host at
all. These two fields are the second door to it, exactly as `dnd`, `play` and
`stats` are second doors to features whose first door is a gesture.

#### The clock and the screen are two different things

This is the distinction the whole feature turns on, and it is why `focus` has
five verbs rather than an on/off pair.

- **The clock** runs whether or not anything about it is on the panel. It
  survives the screen closing, a message card, the sign, the game, the stats
  screen, a first-run replay, and a pending question. It is never refused.
- **The screen** is a modal screen like all the others, and it is refused
  while a prompt is pending, with `err: focus refused, prompt pending`, for
  exactly the reason `play` and `stats` are.

So `{"focus":"start"}` during a `waiting` prompt starts a real countdown and
leaves the question on the panel, and reports the refusal of the screen half
only.

While the clock is running with its screen out of sight, the ambient and live
compositions carry a **two pixel hairline along the bottom edge** that drains
with it. It is dimmer for a paused run than a running one. It is the smallest
honest thing the device can draw, and it is deliberately not a badge.

#### What wins when a timer is running and the agent starts waiting

**The question wins. Always.** The timer's screen yields the panel back to the
live view exactly as the message card does, the countdown keeps running
underneath, and the screen returns the instant the prompt is answered. A timer
that covered an approval prompt would break the only rule this device has
never broken.

The **finish** is the one place the timer outranks something. A milestone
flourish that happens behind a modal screen is missed on purpose, because
nobody wants yesterday's confetti. The end of a timer is not news about the
past: it is the single thing the owner asked this device to tell them, and
nothing else ever will. So it **waits** rather than being dropped, and its own
thirty seconds do not begin until it can actually be seen. It cannot ambush
anybody much later either, because the only thing that can hold it back is a
prompt, and answering that prompt is the moment the owner is looking straight
at the panel.

| Event | What happens |
|---|---|
| `waiting` becomes the effective state | the screen yields, the clock keeps running, the hairline stays |
| the prompt is answered | the screen comes back, mid-countdown |
| `{"focus":"show"}` while `waiting` is pending | refused, with `err: focus refused, prompt pending` |
| `{"focus":"start"}` while `waiting` is pending | the clock starts; only the screen half is refused |
| the timer ends while a prompt is pending | `focus: done <n>m` is emitted at once, the finish waits |
| a message card, the sign, the game or the stats screen goes up | the screen yields under it and comes back |
| the out-of-the-box sequence starts or replays | the screen closes, **the clock keeps running** |
| sixty seconds with nothing pressed, and only while the clock is at rest | closed (`FOCUS_IDLE_MS`, `focusHide(nowMs, "idle")` in `focusTick`). A running countdown is never timed out |
| a factory reset | the clock stops and the length goes back to 25 |

#### The five verbs

| Value | Effect |
|---|---|
| `"show"` | open the screen. Refused while a prompt is pending. Opening it wakes a panel the auto-dim ladder had turned down, exactly as opening the stats screen does |
| `"hide"` | close the screen. **The clock is untouched** |
| `"start"` | start, or resume from a pause. Retires an unread finish, since somebody starting the next pomodoro has plainly seen the end of the last one. Also opens the screen, subject to the refusal above |
| `"pause"` | stop the clock where it is. The remaining time is kept |
| `"reset"` | stop and go back to the top of the chosen length. Also clears a pending finish |

Every one of them is idempotent in the way that matters: a `"pause"` with
nothing running, a `"hide"` with nothing open and a `"start"` on a timer that
is already running are all accepted and do nothing, exactly like a `saysecs`
with no card up.

#### `focusmins`

1 to 180, clamped. Stored in NVS (namespace `codexbuddy`, key `focusmin`) and
written only when the value actually differs from what is stored, so a host
that put it in a 2000ms heartbeat would never touch flash.

It is applied **before** the `focus` verb on the same line, so
`{"focusmins":45,"focus":"start"}` starts a forty-five minute timer rather
than starting the old length and then changing the number underneath it.

A new length lands on the displayed time immediately only when the timer is at
rest, meaning stopped with nothing spent. A run in flight keeps the length it
was started with, and the new one applies from the next reset; the notice says
which happened. That rule exists so a `focusmins` in a heartbeat cannot quietly
rewind a pomodoro somebody paused to take a call.

The button ladder on the board is `5, 15, 25, 45`, a subset of what a host may
ask for. A host-set length that is not on the ladder is honoured, and the next
press of the ladder gesture lands on the first rung, which is how a board gets
back onto the ladder with no host in the room.

#### On the panel

The screen carries `FOCUS` on the left of its header and **what Codex is
doing** on the right, in that state's own colour: `AMBIENT`, `SLEEP`, `IDLE`,
`BUSY` or `DONE`. That is the coexistence in one line: the screen belongs to
the timer and the colour still belongs to the agent, so a single glance answers
both questions. `WAITING` never appears there, because a pending question takes
the whole panel and this screen is not drawn at all.

Under it, the remaining time in the largest digit face the library has (the
48px seven-segment `Font7`), a bar that drains, and a legend that names both
buttons. The digits are dimmed while the clock is stopped, which answers "is
this thing running" without anything blinking.

The finish is the word `FOCUS` in the `done` green over `25 MINUTES DONE`, held
for thirty seconds. It fades in, breathes on the same 2400ms period the idle
and busy states already use, and fades out. No strobe and no siren: there is no
buzzer on this board, and design law 4 would forbid one if there were.

---

## 3. Device to host

Newline-delimited UTF-8 text, but not JSON. Plain lines.

### 3.1 The greeting

```
hello tdisplay-s3 v1 name="Alex Rivera"
```

Sent once, after the 2000ms boot hold (`BOOT_HOLD_MS`; the hold loop and the
`Serial.printf` are both at the end of `setup()`).

- The `hello tdisplay-s3 v1` prefix is fixed and is what every consumer
  matches on: `/hello\s+tdisplay-s3/i` in the helper's `doctor`, and the
  substring `hello tdisplay-s3` in `firmware/tools/flash-all.sh`. Both are
  prefix or substring tests, so the suffix is invisible to them.
- `name="..."` is the name the device is actually running with, resolved
  through `activeName()`. It is **always quoted**, so an unnamed board is an
  unambiguous `name=""`.
- It exists so a host can read back what a `"name"` line did without a second
  command, and so persistence across a power cycle is provable over the wire
  rather than by eye.

Captured on a board on `/dev/cu.usbmodem1101`, reading the greeting before and
after setting a name, with a hardware reset (DTR and RTS pulsed) in between:

```
hello tdisplay-s3 v1 name=""
                                     <- {"name":"Alex Rivera"} then reset
hello tdisplay-s3 v1 name="Alex Rivera"
                                     <- {"name":""} then reset
hello tdisplay-s3 v1 name=""
```

The name survived a power cycle and then cleared cleanly, which is exactly what
it has to do for a fleet whose recipients are not known at flash time.

### 3.2 `ok`

One `ok` per successfully parsed and applied host line, written immediately
after the struct merge completes (`pumpSerial`). Nothing at all is sent for a
malformed line.

### 3.2a Unsolicited notices

Plain lines the device emits on its own, in addition to (never instead of) the
`ok` for the line that caused them. They exist so the first-run lifecycle is
provable over the wire rather than by eye.

| Line | When |
|---|---|
| `firstrun: playing (first)` | the real out-of-the-box play has started, right after the greeting on an armed board |
| `firstrun: playing (replay)` | a replay has started, from `{"firstrun":"play"}` or from a factory reset confirming itself |
| `firstrun: spent` | the flag has just been written, at the end of a real first run |
| `reset: factory ok` / `reset: firstrun ok` | a `reset` was applied |
| `reset: factory ram-only` / `reset: firstrun ram-only` | applied in RAM, but NVS could not be opened |
| `reset: factory ok (buttons)` | the both-buttons gesture fired |
| `err: firstrun refused, session is active` | a play was asked for during `busy` or `waiting` |
| `err: nvs open failed, ...` | a name, face or flag write could not be persisted |
| `say: showing "<lines>" <n>s lines=<n> font=<rung>` | a message card went up. See below |
| `say: showing "<lines>" hold lines=<n> font=<rung>` | the same, for `saysecs: 0` |
| `say: <n>s` / `say: holding` | a bare `saysecs` re-timed the card that was up |
| `say: cleared` | cleared by `{"say":""}`, or by a value that sanitised to nothing |
| `say: cleared (button)` | dismissed with either button on the board |
| `say: expired` | the hold ran out and the card is fading |
| `dnd: on (host)` / `dnd: on (buttons)` | the sign went up, with the solved layout: `font=<rung> w=<line1>,<line2> max=<working width>` |
| `dnd: off (host)` / `dnd: off (button)` | the sign is fading out |
| `dnd: off (cleared)` | a factory reset or the start of a first run took it down without a fade |
| `play: hop (button)` / `play: hop (host)` | the toy opened, carrying the stored personal best: `best=<n>` |
| `play: over score=<n> best=<n>` | a run ended in a crash. ` (new best)` is appended when it beat the stored value, and that is the only moment the best is written to flash |
| `play: end (<why>) score=<n> best=<n>` | the toy closed. `why` is `button`, `host`, `prompt` (a question arrived), `idle` (thirty seconds with no press) or `firstrun` |
| `err: play refused, prompt pending` | `{"play":"on"}` arrived while the effective state was `waiting` |
| `stats: open (<why>) ...` | the stats screen opened, carrying every figure it is about to show plus `turn=running\|none` and `link=up\|down`. `why` is `button` or `host` |
| `stats: close (<why>) ...` | it closed. `why` is `button`, `host`, `prompt` (a question arrived), `idle` (twenty seconds) or `firstrun` |
| `err: stats refused, prompt pending` | `{"stats":"on"}` arrived while the effective state was `waiting` |
| `stats: turn <m:ss> turns=<n> tokens=<n> longest=<n>s best=<n>s` | a turn completed and was counted |
| `stats: new day best_turn=<n>s best_day=<n>` | local midnight passed and the daily figures were zeroed |
| `milestone: <TAG> <value>` | a flourish fired: `LONG TURN`, `LONGEST YET` or `TOKENS TODAY` |
| `err: nvs open failed, records not persisted` | a record moved but could not be written |
| `focus: show (<why>) len=<n>m left=<m:ss> running\|stopped` | the timer screen opened. `why` is `button` or `host` |
| `focus: hide (<why>) left=<m:ss> running\|stopped` | it closed. `why` is `button`, `host`, `idle` (sixty seconds with nothing pressed, and only while the clock is at rest) or `firstrun`. The clock is untouched |
| `focus: start (<why>) len=<n>m left=<m:ss>` | the clock started or resumed |
| `focus: pause (<why>) left=<m:ss>` | the clock stopped where it was |
| `focus: reset (<why>) len=<n>m` | back to the top of the chosen length |
| `focus: length (<why>) <n>m` | the stored length changed. `, from the next reset` is appended when the current run keeps its own length |
| `focus: done <n>m` | the countdown reached zero. Emitted at once, even when the finish itself has to wait for a prompt to clear |
| `focus: end (<why>)` | the finish is over. `why` is `elapsed` (its thirty seconds ran out) or `button` |
| `err: focus refused, prompt pending` | `{"focus":"show"}`, or the screen half of a `"start"`, arrived while the effective state was `waiting` |
| `err: focus length locked, timer is running` | the length gesture was used while the clock was running |
| `err: focus reset locked, timer is running` | button 1's hold was used on a running timer; the timer pauses instead |
| `err: nvs open failed, focus length not persisted` | the length changed but could not be written |

The `say: showing` line echoes **what is on the panel, not what was sent**:
the sanitised, capped, wrapped result, one entry per rendered line, joined
with ` | `. It is the only way to check the cap, the collapsed whitespace, the
line breaks, the ellipsis and the chosen type size without standing in front
of the board, which is exactly what `docs/NEEDS-EYES.md` cannot do for you.

```
{"say":"deploying to prod, do not unplug"}
say: showing "deploying to | prod, do not | unplug" 30s lines=3 font=18
ok
```

It is informational. Nothing parses it, the separator is not escaped, and a
message containing a literal ` | ` will look like two lines in the notice and
still be one on the panel.

All of these are additive. A consumer that matches the greeting on a prefix or
substring, counts `ok`s, or ignores anything it does not recognise, is
unaffected; that covers the helper's `doctor`, `firmware/tools/flash-all.sh`'s
`verify_hello`, and `Link`, which does not read at all by default.

An armed board's boot therefore looks like this on the wire:

```
hello tdisplay-s3 v1 name="Alex Rivera"
firstrun: playing (first)
                                    ... 8.6 seconds ...
firstrun: spent
```

and every boot after it emits the greeting alone.

### 3.3 Known quirk: the transmit path runs one message behind

**This is the single most important thing to know before writing a consumer.**

The board's USB CDC transmit path runs exactly one message behind. The `ok`
for line N does not reach the host until the host writes line N+1. With a
3-second wait and nothing further sent, it never arrives at all.

- It is not this project's code. It reproduces on both the current firmware
  and the pre-ambient firmware on the same board, so it is in the Arduino
  `HWCDC` layer.
- It is pre-existing, not introduced by any recent work.
- **Nothing is lost over a stream.** The ack count always reconciles with the
  number of accepted lines. It is a delay, not a drop.

Consequences, all of them load-bearing:

- **No consumer may build a blocking per-line handshake on `ok`.** A host that
  treats a missing `ok` as a failure will mis-report the last line of every
  burst, every time.
- Treat acks as a running count, not a per-line confirmation.
- The shipped helper sidesteps it entirely: `Link` is **write-only** unless
  explicitly opened with `read: true`, and the comment above the class says so
  in as many words. `writeFrame` never waits for a reply. `doctor` and
  `selftest` are the only readers: `doctor` reports `acked our keepalive`
  rather than asserting anything per line, and `selftest` reads an answer back
  for each of its checks.
- `firmware/tools/flash-all.sh`'s `verify_hello` works around it by writing a
  bare `{}` about once a second until the greeting appears. `{}` is the
  no-op keepalive from 2.3, so the nudge cannot change any state on the board
  it is verifying.

### 3.4 What counts as malformed

Malformed means silently dropped: no `ok`, and **no partial application** of
the line's other fields.

- Not valid JSON at all (`deserializeJson` returns an error).
- Valid JSON that is not an object (`42`, `"busy"`, `[1,2,3]`).
- The raw line exceeds 512 bytes before a `\n` (section 1).
- A `state` key holding a string outside the five-name enum. `parseStateName`
  takes a `bool* ok` out-parameter and `applyJsonLine` acts on it before
  touching anything else:

  ```c
  bool nameOk = false;
  StateId ns = parseStateName(v.as<const char*>(), &nameOk);
  // An unrecognised name is a host bug, not a keepalive: drop the whole line
  // silently rather than acking it while showing the previous state.
  if (!nameOk) return false;
  ```

Everything else, including fields of the wrong type (skipped by the
`!v.isNull() && v.is<T>()` gate) and objects with zero recognized keys, is
accepted and acked.

`say`, `saysecs` and `dnd` are the fields that can hold garbage without
consequence: the first is sanitised (2.9), the second is clamped, and the
third is a boolean with no typo space, so none of them ever reaches this
list. An enum elsewhere on the line still drops the
whole thing, `say` included, because all of the enums are validated before any
field is written.

---

## 4. Worked examples

```json
{"state":"sleep","ring":0.0,"center":"--","label":"CODEX","sub":""}
{"state":"idle","ring":0.08,"center":"8%","label":"CTX","sub":"idle"}
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"3:07 elapsed","tps":31.2}
{"state":"waiting","ring":0.62,"center":"62%","label":"CTX","sub":"your turn"}
{"state":"done","ring":0.64,"center":"64%","label":"CTX","sub":"3:21 elapsed"}
{"state":"idle","ring":0.91,"center":"91%","label":"QUOTA","sub":"weekly window"}
{"tps":22.4}
{"name":"Alex Rivera"}
{"say":"brb"}
{"say":"in a meeting","saysecs":0}
{"saysecs":600}
{"say":""}
```

Six of those lines come verbatim from `DEMO_FRAMES` in
`helper/codex-companion.js`, which is what `codex-companion demo` actually
puts on the wire. Notes:

- **`label` is only ever `CTX`, `QUOTA` or `TIME`.** `metricFields` picks in
  that order: a fresh rate-limit window at 80% or more takes the ring and the
  `QUOTA` label, otherwise a known context fill gives `CTX`, otherwise the
  ring parks at 0.15 and the label reads `TIME` with an elapsed clock or `--`
  in the centre. There is no `APPROVE` label: the helper has no tool-name
  awareness and no code path that could produce one. `firmware/tools/sim.py`
  does send `APPROVE` and `DONE` as labels, which is fine for a simulator but
  means the simulator does not show you what the real helper shows.
- **`waiting` always carries `sub: "your turn"`**, unconditionally, set in
  `frameForEvent` after the metric fields are merged. There is no tool name,
  no command, no file path. The screen says that you are the one holding
  things up; your terminal says what for.
- **`waiting` comes from a hook event, not from a heuristic.** It is Codex's
  own `PermissionRequest` event, mapped in `EVENT_STATE`. There is no stall
  timer and no watcher module: state is first-party hook data, and the rollout
  file is read only for numbers. `docs/architecture.md` section 8 records the
  45-second heuristic this replaced.
- **`done` produces exactly one flash.** The firmware runs a 600ms flash
  (`DONE_FLASH_MS`): a 120ms ramp (`DONE_RISE_MS`), a decay, then a 150ms
  cross-fade into the idle look (`DONE_XFADE_MS`), after which it draws the
  idle look while `st.state` stays `ST_DONE`. The flash is armed only on the
  state edge, inside `applyJsonLine`'s `if (ns != st.state)` branch, so a
  repeated `done` line is a keepalive that replays nothing.
- `{"tps":22.4}` is a partial update: it changes one number and leaves
  everything else, including `state`, exactly as it was.
  `{"name":"Alex Rivera"}` changes nothing on the current frame's ring or text
  and writes to NVS.
- The last four lines are the message card, and none of them come from the
  helper. `{"say":"brb"}` puts a card up for 30 seconds; adding
  `"saysecs":0` holds it until it is cleared; a bare `{"saysecs":600}`
  re-times whatever card is up; `{"say":""}` takes it down. Nothing on those
  lines is persisted and none of them can hide a pending prompt: see 2.9.

---

## 5. Still not pinned down

- CDC framing details (parity, stop bits) beyond the assumed 8N1 default
  (section 1). Neither end sets them, and native USB CDC ignores them, so this
  is unlikely ever to matter.
- The `name` round trip has been exercised over the wire, but a 24-character
  name has never been looked at on the panel. That is a visual item, see
  `docs/NEEDS-EYES.md`.
- The **do-not-disturb chord has not been exercised on hardware**, for the
  same reason and with the same standing: every `dnd` path in 2.10 was driven
  over the wire and read back, but nothing in this project can press a
  physical button, so "both buttons together put the sign up and any single
  press takes it down" is a claim about the code, not a measurement. It is in
  `docs/NEEDS-EYES.md`.
- The message card's **button dismissal has not been exercised on hardware**.
  Every other path in 2.9 was driven over the wire and read back, but nothing
  in this project can press a physical button, so "either button dismisses the
  card and does nothing else" is a claim about the code and not a measurement.
  It is in `docs/NEEDS-EYES.md`.
- The toy's **button gestures have not been exercised on hardware**, and for
  once that gap covers the feature's main door: every path in 2.11 was driven
  over the wire and read back, including a bot playing a real game, but "hold
  button 1 for two seconds to open it, 1 jumps, 2 leaves" is a claim about the
  code. It is in `docs/NEEDS-EYES.md`.

See `docs/open-questions.md` for the full consolidated list.
