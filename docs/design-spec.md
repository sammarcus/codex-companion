# codex-buddy animation spec

> **Read the header before the document.** This was written before any code
> existed, and Part 2 is a *proposal*, not a record. What actually shipped
> diverges from it, and this file is kept for two reasons only: **Part 1 is the
> provenance record `docs/licences.md` points at** for what was taken from
> `claude-desktop-buddy`, and **section 2.3's palette is the one thing that
> survived verbatim**, round-tripping to the RGB565 constants in
> `firmware/src/main.cpp`.
>
> What Part 2 specified and the firmware does **not** do: seven states (the
> protocol has five, `attention` became `waiting`, `celebrate` became a 600ms
> flash on `done`, and `error` was never implemented at all); the ring at
> (160, 68) with radii 42 to 52 (it is at (244, 60) with radii 37 to 46, and
> ambient draws no ring at all); a 15 second idle dim and a 60 second sleep
> timeout (they are 30 seconds and 5 minutes); a 100 to 150ms redraw tick (the
> renderer runs a locked 30fps); and a device with no face on it, which is now
> the entire product. `docs/architecture.md` section 8 records why each of those
> reversed.

Target hardware: LilyGo T-Display-S3 (ESP32-S3R8, ST7789, 170x320 physical
panel), cased, sitting on a desk stand. Rendering: LovyanGFX, full-screen
sprite back buffer pushed to the panel each frame.

Design reference: `anthropics/claude-desktop-buddy` (MIT), cloned shallow
into `vendor/claude-desktop-buddy/` for this recon. That project is a
different device (M5StickC Plus, 135x240, ASCII/GIF pet characters) - it is
used here only to mine a proven state model, timing constants, and
"impatience" behavior. codex-buddy's own UI is a ring indicator, not a pet,
so Part 2 is a new design informed by Part 1, not a port of it.

Every fact in Part 1 cites the file it came from. Part 2 is this document's
own design; nothing there is a source fact.

---

## Part 1 - what claude-desktop-buddy actually does (extracted)

### 1.1 State model

Seven states, shared enum order used everywhere (`main.cpp:32`, mirrored in
`buddy.h:9` and `character.cpp:9-11`):

```
sleep(0) idle(1) busy(2) attention(3) celebrate(4) dizzy(5) heart(6)
```

Derivation (`main.cpp:479-485`, `derive()`, priority order - first match
wins):

```
!connected → idle (see note below)
sessionsWaiting > 0 → attention
recentlyCompleted → celebrate
sessionsRunning >= 3 → busy
else → idle
```

Note: the README's own state table (`README.md:140-150`) documents `sleep`
as triggered by "bridge not connected", but the `derive()` function actually
returns `idle` in that case, not `sleep`. `sleep` is reached only via the
default variable init (`PersonaState baseState = P_SLEEP;`, `main.cpp:36`)
and a 12s post-wake hold (`main.cpp:1000`: while `baseState==P_IDLE` and
inside the `wakeTransitionUntil` window, it's forced to `P_SLEEP` so the
user sees the wake animation instead of idle immediately). This is a real
discrepancy between the doc and the code in that repo - flagged, not
resolved, since it doesn't need resolving for codex-buddy's own logic.

Two more states are reached only as **one-shot overrides**, not from
`derive()`:

- `dizzy` - device shaken (accelerometer delta > 0.8g over a decaying
 baseline, `checkShake()`, `main.cpp:492-499`), one-shot 2000ms
 (`triggerOneShot(P_DIZZY, 2000)`, `main.cpp:1016`).
- `heart` - permission approved within 5s of the prompt arriving
 (`main.cpp:1087`: `if (tookS < 5) triggerOneShot(P_HEART, 2000)`), one-shot
 2000ms.
- `celebrate` is also a one-shot when triggered by a level-up
 (`triggerOneShot(P_CELEBRATE, 3000)`, `main.cpp:995`), separate from the
 `recentlyCompleted` path through `derive()`.

One-shot mechanism (`main.cpp:487-490`): `activeState` is pinned to the
override state until `oneShotUntil` elapses, then snaps back to whatever
`baseState` currently is - no cross-fade, no queueing of a second one-shot
while one is active.

Per-state meaning, from the README's own table (`README.md:142-150`):

| State | Trigger | Feel |
|---|---|---|
| sleep | bridge not connected (see discrepancy above) | eyes closed, slow breathing |
| idle | connected, nothing urgent | blinking, looking around |
| busy | sessions actively running | sweating, working |
| attention | approval pending | alert, **LED blinks** |
| celebrate | level up (every 50K tokens) | confetti, bouncing |
| dizzy | you shook the stick | spiral eyes, wobbling |
| heart | approved in under 5s | floating hearts |

### 1.2 Busy/waiting detection - where the numbers come from

The device never inspects the agent directly. It parses a JSON heartbeat
pushed over BLE/USB from the desktop app (`REFERENCE.md:40-76`,
`data.h:70-127`):

```json
{ "total": 3, "running": 1, "waiting": 1, "msg": "...", "entries": [...],
 "tokens": 184502, "tokens_today": 31200,
 "prompt": { "id": "req_abc123", "tool": "Bash", "hint": "rm -rf /tmp/foo" } }
```

- `running > 0` → at least one session generating.
- `waiting > 0` → a permission prompt is blocking (this is what drives
 `attention`).
- `prompt` object present → a specific approve/deny decision is pending;
 its `id` is echoed back in the response.
- Connection liveness is a pure timeout on receipt, not a heartbeat ack:
 `dataConnected()` = a JSON line arrived within the last 30000ms
 (`data.h:50-52`, `_lastLiveMs`). No keepalive round-trip is required from
 the device; it just has to have heard *something* recently. The desktop
 side sends a keepalive every ~10s specifically so this 30s window doesn't
 expire during quiet periods (`REFERENCE.md:42-43,78-79`).
- Separately, `dataBtActive()` uses a **15000ms** timeout on raw BLE byte
 traffic (`data.h:54-57`) purely to report "bt" vs "usb" as the transport
 label on the info screen - it is not part of state derivation.

### 1.3 Timing constants (all confirmed by grep, file:line given)

| Constant | Value | Source | What it gates |
|---|---|---|---|
| `TICK_MS` | 200ms (5fps) | `buddy.cpp:104` | ASCII-pet animation frame advance |
| loop delay | 16ms (60fps) when awake, 100ms when screen off | `main.cpp:1264` | main loop cadence |
| `SCREEN_OFF_MS` | 30000ms | `main.cpp:84` | backlight off after no button/interaction, *unless* on USB power or a prompt is pending (`main.cpp:1258-1262`) |
| wake-transition hold | 12000ms | `main.cpp:105` (`wakeTransitionUntil = millis()+12000`) | after waking from screen-off, `idle` is forced to render as `sleep` so the wake animation is visible |
| data connection timeout | 30000ms | `data.h:51` | no JSON line in this window ⇒ treated as disconnected |
| BT-traffic-active timeout | 15000ms | `data.h:56` | transport label only ("bt" vs "usb"), not state |
| desktop keepalive interval | ~10000ms | `REFERENCE.md:42-43` | desktop-side, keeps the 30s window alive |
| face-down nap enter debounce | 15 frames sustained (loop runs ~60fps while awake ⇒ ~240-960ms depending on load) | `main.cpp:1236-1246` | enters "napping" (screen dimmed to breath level 8, animation paused) |
| face-down nap exit debounce | 8 frames sustained (opposite direction) | `main.cpp:1248` | resumes |
| shake-check interval | 50ms | `main.cpp:1012` | how often the accelerometer is polled for a shake |
| dizzy one-shot | 2000ms | `main.cpp:1016` | shake reaction |
| heart one-shot | 2000ms | `main.cpp:1087` | fast-approval reaction |
| celebrate one-shot (level-up) | 3000ms | `main.cpp:995` | token-milestone reaction |
| GIF idle-variant dwell | 5000ms (`VARIANT_DWELL_MS`) | `character.cpp:59` | multi-clip idle rotation loops the same clip for at least this long before advancing to the next |
| GIF inter-variant pause | 800ms (`ANIM_PAUSE_MS`) | `character.cpp:60` | freeze-frame gap when rotating between idle variants |
| LED attention blink | 400ms half-period (`(now/400)%2`) | `main.cpp:1006` | red LED toggles on/off in `attention` state only, and only if the `led` setting is on |
| tap-twice reset arm window | 3000ms | `main.cpp:188` | destructive-action confirm ("really?") |
| brightness levels | index 0-4 → `20 + level*20` = 20/40/60/80/100 | `main.cpp:46,97` | `M5.Axp.ScreenBreath()` argument (0-100 scale on this PMIC) |
| nap dim level | `ScreenBreath(8)` | `main.cpp:1246` | much dimmer than the lowest normal brightness step |

### 1.4 The "impatient while waiting" behavior - exact mechanics

This is the closest analog to what codex-buddy needs, so it's worth being
precise about what does and doesn't escalate over time. There is **no
continuous escalation curve** (no growing pulse rate, no rising pitch, no
color ramp) - it's a small set of fixed-frequency effects plus one
threshold flip:

1. **On arrival** (prompt appears, `main.cpp:1023-1039`): a single alert
 chirp, `beep(1200, 80)` (1200Hz, 80ms), plus the display is forced to the
 approval screen regardless of what was open, and the screen wakes if it
 was off.
2. **While waiting**, three things run at fixed, non-escalating rates - 
 they don't get faster or more intense the longer the wait goes on:
 - LED blinks at a fixed 400ms half-period (`main.cpp:1005-1009`).
 - The capybara species (representative of all 18) pulses two `!` glyphs
 at fixed independent rates: one at `(t/2)&1` (~400ms half-period at
 `TICK_MS=200`), the other at `(t/3)&1` (~600ms) - see
 `buddies/capybara.cpp:111-120`. Different species vary the exact
 motif but the mechanism (fixed-rate alternation keyed off the shared
 tick counter) is the same across all 18 files.
 - The elapsed-wait counter itself updates every frame:
 `printf("approve? %lus", waited)` where `waited = (now -
 promptArrivedMs)/1000` (`main.cpp:734-736`).
3. **One threshold, at 10 seconds** (`main.cpp:735`): once `waited >= 10`,
 the "approve? Ns" counter text switches from dim gray to `HOT`
 (`0xFA20`, a red-orange) and stays that color for the rest of the wait.
 Nothing else changes at 10s - no new sound, no faster blink, no motion
 change. The escalation is a single color-state flip, not a ramp.
4. **On response**: approve → green text, `beep(2400, 60)`, and if the
 whole wait was under 5s, a 2000ms `heart` one-shot celebration
 (`main.cpp:1077-1087`). Deny → `beep(600, 60)` (low-pitched, short) and
 no celebration (`main.cpp:1112-1118`).

So: one alert sound at arrival, fixed-rate ambient motion/blink for the
whole wait, and exactly one color escalation at a fixed 10s mark. Nothing
in the reference repo scales urgency continuously with wait duration.

### 1.5 Colors (native RGB565, as declared in source)

Shared across all species (`buddy_common.h`, values in `buddy.cpp:20-29`):

| Name | RGB565 | ≈24-bit (decoded) | Used for |
|---|---|---|---|
| `BUDDY_BG` | `0x0000` | `#000000` | canvas background |
| `BUDDY_HEART` | `0xF810` | `#FF0084` | heart-state particles |
| `BUDDY_DIM` | `0x8410` | `#848284` | secondary/dim glyphs (e.g. one of the two sleep "z" particles) |
| `BUDDY_YEL` | `0xFFE0` | `#FFFF00` | attention `!` glyphs, celebrate confetti |
| `BUDDY_WHITE` | `0xFFFF` | `#FFFFFF` | primary glyphs, confetti |
| `BUDDY_CYAN` | `0x07FF` | `#00FFFF` | dizzy stars, confetti |
| `BUDDY_GREEN` | `0x07E0` | `#00FF00` | confetti |
| `BUDDY_PURPLE` | `0xA01F` | `#A500FF` | (declared, not exercised in the species files read) |
| `BUDDY_RED` | `0xF800` | `#FF0000` | (declared; distinct from `HOT`) |
| `BUDDY_BLUE` | `0x041F` | `#0082FF` | (declared) |

UI-chrome colors, declared separately in `main.cpp:29-31`:

| Name | RGB565 | ≈24-bit | Used for |
|---|---|---|---|
| `HOT` | `0xFA20` | `#FF4500` | escalated-wait timer text, deny-button label, reset-confirm border, low-energy tier |
| `PANEL` | `0x2104` | `#212021` | menu/settings overlay panel background |

Species body color is per-species, e.g. capybara `0xC2A6` (`#C55531`,
terracotta) declared in `buddies/capybara.cpp:207`. GIF character packs
declare their own 5-color palette (`body/bg/text/textDim/ink`) as 24-bit hex
in `manifest.json`, converted at load time
(`character.cpp:63-68,189-194`) - the bufo example
(`characters/bufo/manifest.json`) uses `body:#6B8E23, bg:#000000,
text:#FFFFFF, textDim:#808080, ink:#000000`.

There is no per-state color table in this repo - color is per-species (or
per-GIF-pack), constant across all 7 states; only specific overlay
particles (the `!`, confetti, hearts, stars) use the shared accent colors
above. State is communicated primarily through motion/pose and the LED, not
through recoloring the whole character. Note for codex-buddy in Part 2:
recoloring the whole ring per-state is a deliberate departure from this
pattern, chosen because a ring has no pose/expression channel to lean on - 
color is the only signal available besides motion.

### 1.6 Render pipeline (relevant to LovyanGFX port)

Everything draws into one `TFT_eSPI` sprite (`spr`, `135x240`,
`main.cpp:8,954`) and is pushed with a single `spr.pushSprite(0,0)` per
frame (`main.cpp:1229`) - i.e. a full-screen sprite back buffer, exactly the
pattern this project should replicate with a LovyanGFX `LGFX_Sprite`.
Redraws are gated on a dirty check, not unconditional: the ASCII path only
re-renders when the shared `tickCount` advances (every `TICK_MS`) or the
state/species actually changed (`buddyTick()`, `buddy.cpp:173-196`,
comment at `buddy.cpp:141-144` notes this "saves ~12x the fillRect + sprite
print work" versus redrawing every loop iteration at 60fps.

---

## Part 2 - the original design proposal

Kept in outline. The palette is verbatim and load-bearing; everything else here
is superseded and is preserved only so the reversals in
`docs/architecture.md` section 8 have something to refer back to.

### 2.3 Per-state colours (the part that shipped)

Computed with the standard `((R>>3)<<11)|((G>>2)<<5)|(B>>3)` RGB565 packing,
verified by running the conversion rather than by hand. Every row below
round-trips to a constant in `firmware/src/main.cpp`, except `error`, which was
never implemented.

| State | 24-bit design hex | RGB565 |
|---|---|---|
| `sleep` | `#26314A` | `0x2189` |
| `idle` | `#3AA0FF` | `0x3D1F` |
| `busy` | `#FFB020` | `0xFD84` |
| `attention` (< threshold) | `#FFC53D` | `0xFE27` |
| `attention_escalated` (>= threshold) | `#FF3B30` | `0xF9C6` |
| `celebrate` | `#34C759` | `0x362B` |
| `error` | `#D70015` | `0xD002` |
| `boot` accent | `#5B8CFF` | `0x5C7F` |
| background | `#000000` | `0x0000` |
| primary text | `#FFFFFF` | `0xFFFF` |
| secondary/dim text | `#808080` | `0x8410` |

`attention_escalated` intentionally lands close to claude-desktop-buddy's `HOT`
(`0xFA20` / `#FF4500`, Part 1.5), the same "this needs you" red-orange family,
while every other state uses a fresh palette rather than reusing the reference's
colours verbatim.

The shipped firmware adds one colour this table has no state for, an
ambient-only teal (`#2ED3C6`), because Part 2 never anticipated a "no host
attached" mode. `error`'s red is the one row with no consumer: re-adding an
`error` state is the one genuinely open item from this document.

### 2.2 The seven-state model, and what became of it

| Proposed | What shipped |
|---|---|
| `boot` | a 2 second non-protocol startup screen, and an armed board skips it entirely |
| `sleep` | unchanged, but only reachable when a host asks for it |
| `idle` | unchanged in name; a brightness breathe on the reported fill, not a rotating 200-260 degree arc sweep |
| `busy` | unchanged in name; a 2400ms breathe, not a 1000ms linear spinner |
| `attention` | became **`waiting`**. The 10 second escalation threshold survived verbatim, deliberately identical to the reference's own `waited >= 10` |
| `celebrate` | became a 600ms flash on **`done`**, not a 2500ms rotation through accent colours |
| `error` | **never implemented.** It would need a hue no other state owns, and violet and teal are now both spoken for |

### 2.4 to 2.9, in one paragraph each

**Animation.** The per-state periods here were proposals. Two survived (`sleep`
at 4000ms, the pre-escalation `waiting` pulse at 900ms) and the rest diverged;
`firmware/README.md` says which and why.

**Transitions.** "No cross-fade between states, an instant cut keeps the *this
changed* signal legible" was reversed for mode changes: there is now a 700ms
cross-fade between ambient and live, because a mode change is not a state
change. Instant redraw on state change survived. The one-shot queueing rule
survived in a stronger form: `docs/architecture.md` section 4 has the full
precedence table, and a pending prompt still always wins.

**Boot screen.** The `UNIT ####` scheme, last two MAC bytes zero-padded hex,
survived verbatim and is what every fleet board shows, for exactly the reason
the reference gave: several units in one room have to be distinguishable.

**Fonts.** The proposed size multiples were replaced by named LovyanGFX faces
(Font0, Font2, Font4, Font7 and FreeSansBold), with measured widths rather than
character counts, because the proportional faces make a character budget a lie.

**Brightness.** The duty values `255`, `70` and `20` survived and are three of
the five rungs in `BRIGHT_LEVELS`. Their *triggers* did not: the ladder is one
shared set of tiers rather than two independent ones, staleness tops out at 70,
and a fifth rung of `0` was added as a deliberate hand-picked "not now". The
proposed 15s and 60s timeouts became 30s and 5 minutes.

**LovyanGFX notes.** The pin set quoted here is correct and is what
`src/LGFX_TDisplayS3.hpp` uses; `docs/hardware-recon.md` is the fuller citation.
The redraw-gating advice was superseded: the renderer runs a locked 30fps into a
full-screen sprite, because the face has continuous motion that a dirty flag
cannot express.

---

## Sources

- `vendor/claude-desktop-buddy/` - shallow clone of
 `https://github.com/anthropics/claude-desktop-buddy` (MIT, Copyright 2026
 Anthropic PBC - `LICENSE` read in full). Files read in full: `README.md`,
 `REFERENCE.md`, `src/main.cpp`, `src/buddy.cpp`, `src/buddy.h`,
 `src/buddy_common.h`, `src/character.cpp`, `src/character.h`,
 `src/data.h`, `src/stats.h`, `src/buddies/capybara.cpp`,
 `characters/bufo/manifest.json`.
- `vendor/T-Display-S3/` - already present in the project's `vendor/`
 directory at task start (not cloned by this task). Used only for board
 orientation convention (`examples/*/*.ino` `setRotation()` calls) and the
 real pin map (`examples/tft/pin_config.h`), both read directly, plus the
 panel spec table in `README.md` (170x320 ST7789, `README.md:18`).
- RGB565 conversions in 1.5 and 2.3 computed with a short Python script
 (`(r>>3)<<11 | (g>>2)<<5 | b>>3` and its inverse), not hand math.
