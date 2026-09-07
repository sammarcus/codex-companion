# Codex Desk Companion: serial protocol spec

Formal spec for the newline-delimited JSON protocol between the Node host
helper and the T-Display-S3 firmware, over native USB CDC serial at
**115200 baud, 8N1**.

This document was drafted before `firmware/src/main.cpp` and
`helper/src/frame.js` existed, then corrected against the actual shipped
source once both landed (2026-09-07, same day). Every field limit, timing
value, and edge-case behavior below is now cited to a real file, not
inferred. Where this doc's original guess turned out right or wrong, that
is noted so the history isn't silently erased.

**Citations name symbols, not line numbers.** An earlier revision cited
`main.cpp:NNN` throughout; every one of those into a function body had
drifted out of date, while still carrying a promise of line accuracy.
Function, constant, and field names survive edits that line numbers do
not, so `grep -n applyJsonLine firmware/src/main.cpp` is the intended way
to follow a citation here.

`README.md` carries only a summary and links back here.

---

## 1. Transport

- USB CDC-ACM, native (not a UART bridge chip). Confirmed board USB
  `hwids`: VID `0x303A` (Espressif), PID `0x1001`
  (`vendor/T-Display-S3/boards/lilygo-t-display-s3.json:16-19`, cited in
  `docs/hardware-recon.md` section 7, and matched by the shipped helper's
  own port filter, `ESP_VENDOR_ID = '303a'` in `helper/src/device.js`).
  Whether a live unit actually enumerates with that exact PID is still
  UNCONFIRMED until a board is plugged in, see `docs/open-questions.md`
  items H4/H6.
- Baud rate: **115200**, confirmed on both ends: `Serial.begin(115200)`
  in `setup()` (`firmware/src/main.cpp`) and `monitor_speed = 115200` /
  `upload_speed = 460800` in `firmware/platformio.ini` (upload speed is
  faster, only the run-time link is 115200). Framing is otherwise whatever
  the host's `serialport` library and the device's USB CDC stack negotiate
  as defaults (8 data bits, no parity, 1 stop bit, the near-universal
  CDC-ACM default), not separately pinned in either source file.
- One direction is host to device (state updates), the other is device to
  host (handshake plus per-line acks). Both directions are
  newline-delimited (`\n`) UTF-8 text. There is no binary framing, no
  length prefix, no checksum.
- **Lines longer than 512 bytes are discarded whole, not truncated**
  (`LINE_CAP = 512` and the `lineOver` discard branch in `pumpSerial`,
  `firmware/src/main.cpp`). A line that
  overflows the buffer sets a discard flag and the entire line up to the
  next `\n` is dropped, silently, so a partial write can never be parsed
  as a (wrong) valid frame. This was not something the earlier draft of
  this spec anticipated; it's a real firmware behavior worth knowing if a
  `sub` string plus JSON overhead ever gets long.

---

## 2. Host to device: state line

Exactly one JSON object per line, terminated by `\n`. The device parses
each line independently with `ArduinoJson` (`applyJsonLine`,
`firmware/src/main.cpp`);
there is no multi-line message.

### 2.1 Field table

| Field | Type | Range / allowed values | Buffer clamp (firmware) | Default if never sent |
|---|---|---|---|---|
| `state` | string enum | `"sleep"`, `"idle"`, `"busy"`, `"waiting"`, `"done"` | n/a | See 2.4 |
| `ring` | number (float) | `0.0` to `1.0` inclusive, a fraction, not a percentage (`0.62` means 62% fill, not `62`). Firmware clamps out-of-range values rather than rejecting the line (`clampf`, called from `applyJsonLine`). | n/a | `0.0` |
| `center` | string | Short text for the 40x40px center readout. **Firmware buffer is 16 bytes, so up to 15 usable characters** (`char center[16]` in `struct State`, confirmed again in `firmware/README.md`'s own field table: "clamped to 15 chars"). Longer values are truncated at 15, not rejected (`copyClamped`). Font size auto-switches: `<= 4` characters renders large (`Font4`), longer renders smaller (`Font2`) (the `strlen(st.center)` branch in `drawText`), so the "1-4 char" budget from the original design-spec-derived draft of this doc was about the *large* rendering, not the field's actual hard limit. | Empty string |
| `label` | string | Short status word, e.g. `"CTX"`. **Buffer is 24 bytes, 23 usable characters** (`char label[24]` in `struct State`; matches `firmware/README.md`'s "clamped to 23 chars"). | Empty string, though the boot sequence pre-seeds it to `"CODEX"` (the `copyClamped` call in `setup()`) before any host line ever arrives, see 2.4. |
| `sub` | string | Freeform short status text, e.g. `"12:34 elapsed"`. **Buffer is 40 bytes, 39 usable characters** (`char sub[40]` in `struct State`; matches `firmware/README.md`'s "clamped to 39 chars"). The original draft of this doc guessed ~46 based on the design-spec's pixel-width budget; the shipped firmware's actual buffer size is smaller, 39 is the real number. Truncated, not wrapped. | Empty string |
| `tps` | number (float) | `0` to `10000` inclusive, clamped (`clampf(v.as<float>(), 0.0f, 10000.0f)` in `applyJsonLine`). Tokens/sec, a derived average over an interval on the host side (`docs/codex-state-format.md`, "Tokens/sec" under section 6; `helper/src/frame.js` only includes `tps` in the emitted frame at all when it's finite and `> 0`, see the closing `Number.isFinite(s.tps)` guard in `buildFrame`). | `0.0` |

Any field not listed above in a given line is left untouched, see the
keep-last-value rule (2.3). Any field present in the JSON that is not one
of the six above is ignored, not treated as malformed, this is inherent to
how `ArduinoJson`'s `doc["fieldname"]` lookups work: reading a key the
parser doesn't ask for simply never happens.

### 2.2 Example line (from the task spec, given verbatim)

```json
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
```

Confirmed byte-for-byte reused as the worked protocol example in both
`firmware/README.md` and `firmware/tools/README`-adjacent test snippets
(the `printf` example in `firmware/README.md`'s "Feeding it data by hand"
section uses the same shape with `state:"waiting"`).

### 2.3 Keep-last-value semantics (exact rule, confirmed against source)

The device holds one persistent struct (`struct State`, instantiated as
`static State st;`) with all six fields. On receiving a syntactically
valid JSON object line (`applyJsonLine`):

1. For each of the six known keys, if the key is present **and not
   JSON `null`** (`ArduinoJson`'s `v.isNull()` check, once per field in
   `applyJsonLine`),
   overwrite the corresponding struct field with that value.
2. If the key is absent, or its value is JSON `null`, or its value is
   present but the wrong type (e.g. `"ring":"high"`, a string where a
   float is expected, `v.is<float>()` fails), the field is left untouched.
   **This is confirmed, not inferred**: the original draft of this spec
   guessed that `null` should behave like "absent" without a source to
   check against; the shipped firmware treats them identically by
   construction (`!v.isNull() && v.is<T>()` gates every field write).
3. This is a per-field merge, not a per-line replace. A line containing
   only `{"tps":22.1}` updates `tps` and leaves `state`, `ring`, `center`,
   `label`, and `sub` exactly as they were.
4. A JSON object with **no** recognized keys at all (e.g. `{}`, or an
   object containing only unknown keys) is still accepted as well-formed:
   it gets an `ok` and updates nothing. The comment above `applyJsonLine`'s
   `return true` calls this out
   explicitly in a comment: it exists so the host can hold the serial link
   open with a bare `{}` as a keepalive without having to resend the full
   state every time.
5. The merge happens once per accepted line, immediately, synchronously
   with sending the `ok` ack (section 3).

### 2.4 Boot default / before the first line (corrected)

**The original draft of this spec guessed the boot default was
`state:"sleep"` with everything empty, reasoning from the design-spec's
description of the boot sequence. That guess was wrong.** The actual
default, confirmed directly from the struct's in-code initializer
(`struct State`, `main.cpp`):

```cpp
struct State {
  StateId state = ST_IDLE;   // NOT sleep
  float   ring  = 0.0f;
  float   tps   = 0.0f;
  char    center[16];   // set to "" in setup()
  char    label[24];    // set to "CODEX" in setup()
  char    sub[40];      // set to "" in setup()
};
```

So immediately after the boot screen (2.6), before any host line has ever
arrived, the device renders `idle`'s steady empty ring (0% fill, since
`ring` defaults to `0.0`), with the label reading `"CODEX"` and no center
or sub text.

**That idle look is not permanent: the 30s/5min no-data timeouts (2.5)
are armed from boot, not from the first accepted line.** There is no
"have we ever heard from a host" flag in the firmware, and an earlier
revision of this document asserted one (`everGotData`) that has never
existed in any shipped source. `setup()` seeds the staleness clock with
`lastDataMs = millis();`, and both consumers, `effectiveState` (which
`renderFrame` uses) and `updateBrightness`, measure against it
unconditionally. The firmware says so itself, in the comment directly
above `effectiveState`: "lastDataMs is seeded at boot, so a board sitting
on a desk with no host dims and then sleeps on the same schedule as one
whose host went away."

Concretely: **a unit that is powered on and never receives a single line
still dims to 70/255 at 30 seconds and reaches the 20/255 sleep look at
5 minutes**, exactly like a unit whose helper went away. That matters for
the flash-day smoke test (`docs/wednesday-runbook.md`): check the
full-brightness `CODEX` idle look promptly, inside the first 30 seconds
after boot. A tray of flashed boards with no helper attached will all be
dim within five minutes, and that is the timeouts working, not fourteen
dead boards. Button 1 (GPIO 0, handled by `pumpButton`) forces one back
to full brightness on demand.

### 2.5 No-data timeouts (device-side, autonomous, confirmed against source)

These fire independent of anything the host explicitly sends, and they
are armed from boot rather than from the first accepted line (2.4). The
device tracks wall-clock time since `lastDataMs`, which `setup()` seeds
at boot and `pumpSerial` refreshes on every accepted line:

| Elapsed since last accepted line | Device behavior | Source |
|---|---|---|
| under 30s (`STALE_DIM_MS`) | Render normally per the current struct and `state`; brightness held at full (`updateBrightness`). | `STALE_DIM_MS`, `updateBrightness` |
| 30s to 5min | **Dim.** Backlight duty drops to **70/255** (`BRIGHT_DIM_IDX`, index 2 of `BRIGHT_LEVELS = {255,160,70,20}`). The panel keeps animating rather than freezing, "dim last frame" is a brightness effect, not a frozen frame. One content change does ride along at this tier: `effectiveState` renders a stored `busy` or `waiting` as the `idle` look once past `STALE_DIM_MS`, so a stale unit stops demanding attention for an approval prompt that no longer exists. `sleep`, `idle`, and `done` render unchanged, and `st.state` is untouched either way. This confirms `docs/design-spec.md` section 2.8's separately-proposed "idle dim" duty of `70` was reused here, one shared value, not two distinct tiers, resolving what was an open question in the first draft of this doc. | `STALE_DIM_MS`, `BRIGHT_LEVELS`, `BRIGHT_DIM_IDX`, `updateBrightness` |
| 5min or more (`STALE_SLEEP_MS`) | Backlight drops further to **20/255** (`BRIGHT_SLEEP_IDX`, index 3), and **rendering** is forced to the `sleep` look (dim breathing full ring) regardless of the last `state` the host sent, via `effectiveState`, called fresh each frame by `renderFrame`. **Confirmed: this is a render-time override only. It does not overwrite the stored `st.state` field.** The moment a new line arrives, whatever `state` it carries (or the previously-stored `state`, if the new line omits it) renders immediately, the forced-sleep look was never actually written into the struct. This resolves what the first draft of this doc could only guess at as a design choice. | `STALE_SLEEP_MS`, `BRIGHT_SLEEP_IDX`, `effectiveState`, `updateBrightness` |
| New line arrives after either timeout | The **staleness** input returns to tier 0 on the next `since < STALE_DIM_MS` check and rendering resumes per the merged struct immediately, no fade. Brightness returns to full only if the **state** input is also at tier 0: a fresh line carrying `sleep` lands on 20/255 straight away, and a fresh `idle` comes back to full but steps down to 70/255 again once that `idle` has been held `IDLE_DIM_MS`. | `updateBrightness` |

**Staleness is only one of two brightness inputs, and the dimmer of the
two wins.** `updateBrightness(nowMs, eff)` computes a staleness tier and
a state tier independently, then takes `if (stateTier > tier) tier =
stateTier;` before mapping through `TIER_IDX = {BRIGHT_FULL_IDX,
BRIGHT_DIM_IDX, BRIGHT_SLEEP_IDX}`:

| State (as `effectiveState` resolved it) | State tier | Backlight floor it imposes |
|---|---|---|
| `sleep` | 2 | **20/255 immediately**, no waiting period, whether that `sleep` came from the host or from the 5-minute render override |
| `idle` held for `IDLE_DIM_MS` (**15000ms**) or longer, measured from `stateEnterMs` | 1 | **70/255** |
| anything else (`busy`, `waiting`, `done`, `idle` held under 15s) | 0 | full |

The source comment says why the state input exists at all: without it
the tiers were unreachable in practice, since a running helper sends a
keepalive every 2000ms, so `since` never approaches 30s and a
host-declared `sleep` would otherwise sit at full backlight all night.

A manual brightness override (pressing Button 1, GPIO 0) pins the
brightness level and suppresses this whole auto-dim ladder
(the `brightManual` flag, set by `pumpButton` and honoured by
`updateBrightness`) until the **combined auto tier** itself changes, at
which point `updateBrightness` clears the flag and releases the pin. The
release is keyed to `tier != lastTier` after the state tier has already
been folded in, so an `idle` crossing its 15-second `IDLE_DIM_MS` mark
releases a manual pin exactly the same way a 30-second staleness
crossing does.

The host-side helper is confirmed to send data often enough that this
matters in practice, and not by accident: `helper/src/device.js`'s
`DeviceLink._maybeSend` sends a new line either when
the computed frame actually changed (its `framesEqual` check,
imported from `frame.js`), rate-limited to at most one send per
`frameMinMs` (default **250ms**, in `DEFAULTS`), **or**, regardless of
whether anything changed, once at least `heartbeatMs` (default **2000ms**,
same block) has elapsed since the last send. That 2-second heartbeat is
a real, explicit keepalive, comfortably inside the device's 30s dim
window (2.5) and its own 5-minute `sleepMs` default
(`sleepMs` in `codex-watcher.js`'s `DEFAULTS`, matching the firmware's
`STALE_SLEEP_MS`), so a
running helper reliably keeps a connected device out of both no-data
tiers even during a long silent turn where the *content* of the frame
never changes.

---

## 3. Device to host: response lines

Also newline-delimited UTF-8 text, but not JSON, plain text lines.

| Line | When | Source |
|---|---|---|
| `hello tdisplay-s3 v1` | Once, after the boot screen's **2000ms hold** (`BOOT_HOLD_MS = 2000`; the hold loop and the `Serial.println` are both in `setup()`; matches `firmware/README.md`'s own "after the 2s boot screen"). The original draft of this doc estimated "~1800ms" by reading `docs/design-spec.md`'s proposed boot timeline; the shipped firmware uses a round 2000ms instead, a real, confirmed difference from that earlier design doc, not an error in either document, just a value someone picked when implementing. | `BOOT_HOLD_MS`, `setup()` |
| `ok` | Once per successfully parsed and applied host to device line (section 2), sent immediately after the struct merge completes (`pumpSerial`). | `pumpSerial` |
| (nothing) | A line that fails to parse as JSON, parses but is not a JSON object, or exceeds the 512-byte line cap (section 1). | `applyJsonLine`, `pumpSerial` |

### 3.1 What counts as "malformed" (confirmed, no longer open)

The first draft of this doc treated "does an unrecognized `state` value
make the whole line malformed" as an open question, then a later draft
answered it backwards. **The shipped answer is yes: an unrecognized
`state` string makes the whole line malformed.** `parseStateName` takes
a `bool* ok` out-parameter and sets it false for anything outside the
five-name enum, and `applyJsonLine` acts on that immediately:

```c
bool nameOk = false;
StateId ns = parseStateName(v.as<const char*>(), &nameOk);
// An unrecognised name is a host bug, not a keepalive: drop the whole line
// silently rather than acking it while showing the previous state.
if (!nameOk) return false;
```

That `return false` runs **before** the `ring`, `tps`, `center`, `label`
and `sub` blocks, so nothing else on the line is applied either: it is
not a partial update, it is a dropped line. Concretely, malformed
(silently dropped, no `ok`) means exactly:

- Not valid JSON at all (`deserializeJson` returns an error).
- Valid JSON that is not an object (e.g. `42`, `"busy"`, `[1,2,3]`,
  `doc.is<JsonObject>()` fails).
- The raw line exceeds 512 bytes before a `\n` is seen (section 1).
- A `state` key holding a string that is not one of `sleep`, `idle`,
  `busy`, `waiting`, `done` (`parseStateName` sets `*ok = false`,
  `applyJsonLine` returns false before touching any other field).

Everything else, including an object with fields of the wrong type
(skipped by the `!v.isNull() && v.is<T>()` gate) or an object with zero
recognized keys, is accepted and acked. `firmware/README.md`'s field
table and `main.cpp`'s own file header both state the same rule.

---

## 4. Six example lines (all five states plus one partial update)

```json
{"state":"sleep","ring":0.0,"center":"","label":"","sub":"no session","tps":0.0}
{"state":"idle","ring":0.08,"center":"8%","label":"CTX","sub":"waiting for next prompt","tps":0.0}
{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}
{"state":"waiting","ring":0.71,"center":"71%","label":"APPROVE","sub":"Bash: rm -rf /tmp/foo","tps":0.0}
{"state":"done","ring":0.71,"center":"done","label":"CTX","sub":"turn complete, 6.9s","tps":0.0}
{"tps":22.4}
```

Notes on this set, updated against the shipped source:

- Line 1 (`sleep`) is an explicit host-sent sleep, distinct from the
  device's own idle boot default (2.4), both render the same dim
  breathing ring, but only this one is driven by an actual host decision.
- Line 3 (`busy`) is the task spec's own example, reused verbatim, and
  matches `firmware/README.md`'s worked example exactly.
- Line 4 (`waiting`) is the hero interaction. `helper/src/codex-watcher.js`
  confirms this is a **shipped, real heuristic**, not merely a documented
  possibility: an open turn that's gone silent for `stallMs` (45 seconds,
  `stallMs` in `DEFAULTS`) while `approval_policy` is known and not
  `"never"` sets `state:'waiting'` with a `heuristic:true` flag internally
  (the `approvalHeuristic` branch of `derive`). The helper also
  opportunistically parses
  real `exec_approval_request`/`apply_patch_approval_request` lines if
  they ever do show up in a rollout (the `APPROVAL_REQUEST` set in
  `codex-watcher.js`),
  which per `docs/codex-state-format.md` they normally won't, that code
  path exists mainly for the synthetic test fixture and any future Codex
  version that changes the persistence policy. The heuristic can be
  disabled entirely with `--no-heuristic` at install/run time
  (`install.js` USAGE text). `center` and `label` in this example line are
  illustrative; the real helper's `frame.js` picks `label` from whichever
  of context-fill / quota / elapsed-time it has data for (the label ladder in `buildFrame`),
  it does not know the specific pending tool name, so an `"APPROVE"` label
  like this example is aspirational, not what the shipped v1 actually
  sends. Confirm before assuming a future version adds tool-name
  awareness, see `docs/open-questions.md`.
- Line 5 (`done`): confirmed firmware behavior is a **600ms** flash
  (`DONE_FLASH_MS`; the `ST_DONE` branch of `renderFrame`) after which rendering
  automatically falls back to looking like `idle`, **without** the stored
  `st.state` changing away from `ST_DONE`
  (the `else` branch of that block's `since < DONE_FLASH_MS`
  check just draws the idle look while `st.state` stays `done`
  internally). The shipped helper holds `state:'done'` in its
  derived snapshots for `doneMs = 5000` ms after a turn completes
  (`doneMs`; the `done` branch of `derive`, `codex-watcher.js`) before
  moving on to `idle`, but
  `DeviceLink._maybeSend` (`device.js`, see 2.5) only actually writes a new
  line when the frame changed **or** its 2-second heartbeat is due, not on
  every internal tick. If nothing else in the frame changes during the
  done hold (a plausible case: context-fill and elapsed can both be
  static once the turn has ended), the realistic result is the device
  gets `"done"` once immediately, then the identical line resent by the
  heartbeat at roughly +2s and +4s. **Those repeats replay nothing.** The
  flash is armed on the state edge only, inside the `if (ns != st.state)`
  branch of `applyJsonLine`:

  ```c
  // Arm the done flash on the 0->1 edge only. The host resends an
  // unchanged frame every heartbeatMs (2000ms) while a turn's numbers sit
  // still, so re-arming on a repeat would play the flash three times per
  // completed turn; a repeated "done" line is a keepalive, nothing more.
  if (ns == ST_DONE)    doneEnterMs = millis();
  ```

  So one completed turn produces **exactly one 600ms flash**: a 120ms
  ramp up (`DONE_RISE_MS`), a decay, and a 150ms cross-fade into the idle
  look over the tail (`DONE_XFADE_MS`), after which the `else` branch just
  draws the idle look for as long as `st.state` stays `ST_DONE`. An
  earlier draft of this doc claimed two or three flashes per turn,
  describing a re-arm the shipped firmware deliberately does not do. This
  also resolves the original open question about whether `done`
  auto-reverts on its own or waits for the host: the device's rendering
  auto-reverts on its own after 600ms, and the host's later `done` lines
  are pure keepalives that neither extend nor repeat the animation.
- Line 6 is a bare partial update (`tps` only) demonstrating the
  keep-last-value rule from 2.3, every other field, including `state`,
  stays whatever it was after line 5.

---

## 5. Things this spec still does not fully pin down

Everything that was open in the first draft is now resolved against
source, except:

- CDC serial framing details (parity/stop bits) beyond the assumed CDC-ACM
  default (section 1), still not pinned to a specific line in either
  source file.

See `docs/open-questions.md` for the full consolidated list, including the
hardware- and live-account-dependent items this doc doesn't touch at all.
