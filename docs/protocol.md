# Codex Desk Companion: serial protocol spec

Formal spec for the newline-delimited JSON protocol between the host helper
and the T-Display-S3 firmware, over native USB CDC serial at **115200 baud**.

Both ends are small enough to read in full, and both were read for this
document:

| End | File | Notes |
|---|---|---|
| device | `firmware/src/main.cpp` | the parser is `applyJsonLine`, fed by `pumpSerial` |
| host | `helper/codex-companion.js` | **one file, zero dependencies.** There is no `helper/src/` directory |

**Citation note, and a correction.** An earlier revision of this document
cited `helper/src/frame.js` and `helper/src/device.js`. Neither file exists,
and neither is coming back: the helper was rebuilt as a single dependency-free
file, so the frame builder (`frameToLine`, `metricFields`, `frameForEvent`),
the port picker (`choosePort`) and the transport (`class Link`) are all
symbols inside `helper/codex-companion.js`. Every host-side citation below
names one of those symbols.

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
  constructed with `read: true`, which only `doctor` does. This is deliberate:
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
| `name` | string | **Owner name. The only field whose effect outlives the frame.** Stored in NVS and used from that instant, reloaded on every boot. `""` clears the stored name. See 2.6. | 24 chars (`OWNER_NAME_MAX`) | unset: no stored name |

Any field not present in a line is left untouched, see 2.3. Any key that is
not one of the seven above is ignored rather than treated as malformed, which
is inherent to how `ArduinoJson`'s `doc["field"]` lookups work: a key the
parser never asks for is simply never read.

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

**Those defaults are almost never what you see, and this is a change from
earlier revisions of this document.** The firmware now has an ambient mode,
and the live view (the five states, the ring, the centre text) renders only
while a host is actually talking to the board. `ambientActive` returns true
whenever `hostEverSpoke` is false, so a board that has never received a single
accepted line is in ambient mode from the end of the boot screen onward, and
stays there indefinitely.

Concretely, on a board plugged into a charger with no software anywhere:

| Time since boot | What is on the panel |
|---|---|
| 0 to 2s (`BOOT_HOLD_MS`) | boot screen: product name, owner name if one is stored, unit id |
| 2s onward | **ambient**: drifting comet on a faint ring track, colour drifting through two blues and a teal, plus the name and product lines |
| 60s (`AMBIENT_BRIGHT_MS`) | ambient continues, backlight settles from full to the `70` tier |
| forever | ambient. It never reaches the `20` sleep tier and it never stops moving |

The stored `idle` / `CODEX` default is what the board falls back to the moment
a host speaks and then says nothing further, not what an unattached board
shows. `firmware/AMBIENT.md` is the full description of that mode.

**An earlier revision of this document said the opposite**, that a unit with
no host dims at 30s and reaches the 20/255 sleep look at 5 minutes. That was
true of the pre-ambient firmware and is now wrong in both halves: an
unattached board never enters the live view at all, and never reaches the
sleep tier. `ST_SLEEP` is now reachable only when a host explicitly sends
`{"state":"sleep"}`.

### 2.5 Staleness, once a host has spoken

These timers are measured from `lastDataMs`, which `setup()` seeds at boot and
`pumpSerial` refreshes on every accepted line. They only govern the live view,
which requires `hostEverSpoke`.

| Elapsed since last accepted line | Behaviour | Symbols |
|---|---|---|
| under 30s (`STALE_DIM_MS`) | Render per the current struct. Backlight full unless the state tier says otherwise. | `updateBrightness` |
| 30s to 5min | **Dim** to `70`/255 (`BRIGHT_DIM_IDX`, index 2 of `BRIGHT_LEVELS = {255,160,70,20}`). One content change rides along: `effectiveState` renders a stored `busy` or `waiting` as the **idle look**, so a dead host cannot leave a board pulsing red for an approval prompt that no longer exists. `sleep`, `idle` and `done` render unchanged, and `st.state` is untouched either way. | `effectiveState`, `updateBrightness` |
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
Button 1 (GPIO 0) wakes-then-cycles; button 2 (GPIO 14) is a plain one-press,
one-step brightness control.

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
printf '{"name":"Alex Rivera"}\n' > /dev/cu.usbmodem101   # set
printf '{"name":""}\n'            > /dev/cu.usbmodem101   # clear
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

**The shipped helper never sends this field.** `frameToLine` emits only
`state`, `ring`, `center`, `label`, `sub` and `tps`; grep
`helper/codex-companion.js` for `name` and you will find no protocol writer.
Naming is an operator or owner action done with `printf`, and the greeting
(3.1) is how you read back what it did.

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

### 3.2 `ok`

One `ok` per successfully parsed and applied host line, written immediately
after the struct merge completes (`pumpSerial`). Nothing at all is sent for a
malformed line.

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
  in as many words. `writeFrame` never waits for a reply. `doctor` is the one
  reader, and it reports `acked our keepalive` rather than asserting anything
  per line.
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
```

Six of those lines come verbatim from `DEMO_FRAMES` in
`helper/codex-companion.js`, which is what `codex-companion demo` actually
puts on the wire. Notes:

- **`label` is only ever `CTX`, `QUOTA` or `TIME`.** `metricFields` picks in
  that order: a fresh rate-limit window at 80% or more takes the ring and the
  `QUOTA` label, otherwise a known context fill gives `CTX`, otherwise the
  ring parks at 0.15 and the label reads `TIME` with an elapsed clock or `--`
  in the centre. An earlier revision of this document used an `"APPROVE"`
  label in its `waiting` example and flagged it as aspirational. It is now
  removed rather than flagged: the helper has no tool-name awareness and no
  code path that could produce it. `firmware/tools/sim.py` does send `APPROVE`
  and `DONE` as labels, which is fine for a simulator but means the simulator
  does not show you exactly what the real helper shows.
- **`waiting` always carries `sub: "your turn"`**, unconditionally, set in
  `frameForEvent` after the metric fields are merged. There is no tool name,
  no command, no file path. The screen says that you are the one holding
  things up; your terminal says what for.
- **`waiting` comes from a hook event, not from a heuristic.** It is Codex's
  own `PermissionRequest` event, mapped in `EVENT_STATE`. An earlier revision
  of this document described a 45-second stall heuristic in a
  `helper/src/codex-watcher.js` with a `--no-heuristic` flag. None of that
  exists any more: there is no watcher module, no stall timer, and no such
  flag. State is first-party hook data now, and the rollout file is read only
  for numbers.
- **`done` produces exactly one flash.** The firmware runs a 600ms flash
  (`DONE_FLASH_MS`): a 120ms ramp (`DONE_RISE_MS`), a decay, then a 150ms
  cross-fade into the idle look (`DONE_XFADE_MS`), after which it draws the
  idle look while `st.state` stays `ST_DONE`. The flash is armed only on the
  state edge, inside `applyJsonLine`'s `if (ns != st.state)` branch, so a
  repeated `done` line is a keepalive that replays nothing.
- The last two lines are partial updates. `{"tps":22.4}` changes one number
  and leaves everything else, including `state`, exactly as it was.
  `{"name":"Alex Rivera"}` changes nothing on the current frame's ring or text
  and writes to NVS.

---

## 5. Still not pinned down

- CDC framing details (parity, stop bits) beyond the assumed 8N1 default
  (section 1). Neither end sets them, and native USB CDC ignores them, so this
  is unlikely ever to matter.
- The `name` round trip has been exercised over the wire, but a 24-character
  name has never been looked at on the panel. That is a visual item, see
  `docs/NEEDS-EYES.md`.

See `docs/open-questions.md` for the full consolidated list.
