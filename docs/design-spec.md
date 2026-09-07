# codex-buddy animation spec

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

## Part 2 - codex-buddy design (new; T-Display-S3, 170x320 ST7789)

### 2.0 Orientation: landscape, 320x170

**Recommendation: landscape (320px wide x 170px tall), not portrait.**

Reasoning:
- Of the T-Display-S3 example sketches in the vendor SDK that call
 `setRotation()` at all, the majority use `setRotation(1)` (landscape) - 
 `grep -rn setRotation examples/*/*.ino` in
 `vendor/T-Display-S3/examples/` turns up `setRotation(1)` in
 `CapacitiveTouch.ino`, `Arduino_GFXDemo.ino`, `factory.ino`,
 `ImageScroll.ino`, `PokerS3.ino`, `PCBClock.ino` - landscape is the
 board's own conventional default, not an arbitrary choice.
- claude-desktop-buddy's own closest analog to an ambient always-on status
 display - its landscape "clock face" mode, shown whenever nothing urgent
 is happening - deliberately switches to a **wide** layout for that
 purpose (`main.cpp:403-477`): big centered digits plus a small pet, side
 by side, direct-to-LCD. A wide frame reads better at a glance than a tall
 one for exactly this kind of ambient status glance, which is what a desk
 stand is for.
 - A portrait 170-wide column is also just a worse fit for a horizontal
 label + sub-line under a ring (see 2.2) - landscape's extra width goes
 directly to legible text instead of empty margin.
- A desk stand cradles the case in a wide, low, stable base more naturally
 than balancing a tall narrow slab - this is a physical-stability
 judgment call, not something verifiable from the repos, flagged as such.

All geometry below assumes **320 (W) x 170 (H)**, origin top-left, which on
this panel is `setRotation(1)` (or `3`, mirrored) relative to the native
170x320 portrait framebuffer.

### 2.1 Ring geometry

Full-screen `LGFX_Sprite` back buffer, `320x170`, pushed with one
`sprite.pushSprite(&lcd, 0, 0)` per frame - same pattern as 1.6.

- **Center**: `(160, 68)` - horizontally centered, vertically above
 mid-height to leave room for two text lines below.
- **Outer radius**: `52px`.
- **Stroke width**: `10px` (so inner radius `= 42px`).
- **Ring drawn as**: an arc via `sprite.fillArc(cx, cy, r_in, r_out,
 start_deg, end_deg, color)` (LovyanGFX has `fillArc`/`drawArc` natively - 
 no manual trig needed for the sweep animations in 2.3).
- **Center content box**: `40x40px` centered at `(160, 68)`, for a glyph or
 short digit readout (see 2.4 for size).
- **Label line**: baseline at `y=130`, horizontally centered, width budget
 up to `280px` (20px margin each side).
- **Sub line**: baseline at `y=150`, horizontally centered, same width
 budget, smaller font. Truncate (not wrap) - this is a status glance, not
 a transcript; codex-buddy's own equivalent of the reference's scrolling
 transcript HUD is out of scope for the ring screen and belongs on a
 separate detail screen if one gets built later.
- Bottom margin to the panel edge: `170 - 160 = 10px`, tight but the sub
 line's descenders are the only thing that reaches it - acceptable at
 font size 1 (see 2.5).

### 2.2 State model (adapted from Part 1, ring-appropriate)

codex-buddy has no pose/expression channel, so - unlike claude-desktop-buddy
 - color is a first-class per-state signal here, not just an accent. Seven
states, keeping the reference's names where the mapping is direct:

| State | Trigger | Ring behavior | Color |
|---|---|---|---|
| `boot` | device just powered on | see 2.6 | boot accent |
| `sleep` | no heartbeat received in `SLEEP_TIMEOUT` (see 2.4) | slow full-ring breathing | sleep |
| `idle` | connected, `running==0`, `waiting==0` | gentle partial-ring breathing | idle |
| `busy` | `running>0` | ring segment sweeps continuously | busy |
| `attention` | `waiting>0` (a prompt is pending) | ring pulses at fixed rate; color escalates once at threshold | attention → attention_escalated |
| `celebrate` | one-shot: a run just completed cleanly | ring flashes/rotates through accent colors, fixed duration | celebrate |
| `error` | one-shot: last action was denied/failed | ring flashes once, short | error |

Unlike the reference's `derive()`, disconnection maps explicitly to
`sleep`, not `idle` - Part 1.1 flagged that mapping as a likely bug in the
source; codex-buddy does not inherit it.

### 2.3 Per-state colors (RGB565-friendly hex)

Computed with the standard `((R>>3)<<11)|((G>>2)<<5)|(B>>3)` RGB565 packing
(verified by running the conversion, not by hand):

| State | 24-bit design hex | RGB565 (`0xRRRRGGGGGGBBBBB` packed) |
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

`attention_escalated` intentionally lands close to claude-desktop-buddy's
`HOT` (`0xFA20` / `#FF4500`, Part 1.5) - same "this needs you" red-orange
family - while every other state uses a fresh palette rather than reusing
the reference's colors verbatim, since this is a different product with a
different (ring, not character) visual language.

### 2.4 Per-state animation

All periods below are new for codex-buddy - informed by, but not copied
from, Part 1.3/1.4 (that repo's ASCII pets pulse at 200-600ms fixed rates;
this device's motion needs to read clearly on a bare geometric ring, which
wants slightly slower, smoother periods so it doesn't look jittery):

| State | Period | Easing | What varies |
|---|---|---|---|
| `boot` | one-shot, ~1800ms | ease-out | wordmark fades/scales in, then holds (see 2.6) |
| `sleep` | 4000ms | sine (`0.5 - 0.5*cos`) | ring opacity/brightness sweeps ~15%→45%→15% of full stroke alpha; sweep angle stays fixed at full 360° (a dim, breathing full ring, not a partial arc - reads as "resting", distinct from idle's partial arc) |
| `idle` | 2400ms | sine | arc sweep length breathes 200°→260°→200° of the 360° ring, centered/rotating slowly (one full slow rotation per ~12s) so it doesn't look static |
| `busy` | 1000ms per revolution | linear | a fixed 90°-long bright segment sweeps the full ring continuously (classic spinner) - linear, not eased, since this is "work in progress," not a mood |
| `attention` (pre-threshold) | 900ms | ease-in-out pulse | ring stroke width pulses 8px→12px→8px at constant color; **no color change yet** |
| `attention_escalated` (post-threshold) | 500ms | ease-in-out pulse | same stroke pulse, faster, plus color swaps to `attention_escalated`; this is codex-buddy's one deliberate departure from Part 1.4's "no escalation past the single color flip" - a faster pulse on top of the color flip, since a static ring has less going on than an animated character and can afford a second signal without reading as noisy |
| `celebrate` | one-shot, 2500ms total | ease-out then hold | ring does 1.5 fast rotations cycling through `celebrate`→white→`celebrate`, then settles to a solid `celebrate` ring for the remaining duration before returning to `baseState` |
| `error` | one-shot, 600ms total | ease-out | ring flashes `error` → background → `error` once (two-beat "no"), then returns to `baseState` |

Threshold for `attention` → `attention_escalated`: **10 seconds**,
deliberately kept identical to the reference's `waited >= 10` flip
(`main.cpp:735`, Part 1.4) - no reason to invent a different number for the
same product decision ("how long is a prompt allowed to sit before this
gets urgent-looking").

### 2.5 Transition rules

- **State changes redraw immediately**, bypassing whatever point the
 current animation cycle is at - same principle as the reference's
 `buddyInvalidate()` forcing an immediate redraw on state change
 (`buddy.cpp:147,183-188`), so a new state is never delayed behind the
 tail of the old animation.
- **One-shots (`boot`, `celebrate`, `error`) are exclusive**: while one is
 playing, incoming state changes are queued as "what to show when this
 ends," not interrupted - the reference has no such queue (Part 1.1, a
 second one-shot just overwrites `oneShotUntil`/`activeState`), but
 `celebrate`/`error` here carry actual meaning (success/failure) that
 should not be clipped by, say, `busy` starting up again mid-animation.
 `attention` is the one exception allowed to interrupt a one-shot
 immediately - a pending approval always wins, since it is blocking on
 the user.
- **No cross-fade between states.** Match the reference: an instant cut
 keeps the "this changed" signal legible at a glance, which is the whole
 point of an ambient status device. A ring redraw is cheap enough
 (`fillArc` over a small area) that this is a stylistic choice, not a
 performance one.
- **`attention`'s wait timer resets to 0 on state entry**, i.e. every time
 `waiting` transitions from 0 to >0 for a *new* prompt id (mirrors
 `promptArrivedMs` reset in `main.cpp:1028` keyed off `promptId` changing,
 Part 1.4) - not merely because `waiting` stayed >0.

### 2.6 Boot screen

One-shot, ~1800ms, full black background:

1. `t=0-300ms`: background fades from black to `background` (already
 black - effectively a no-op hold, kept as an explicit beat so the boot
 sequence has a clean start frame to test against).
2. `t=300-1000ms`: wordmark text ("codex-buddy" or whatever short product
 name is decided - not sourced, placeholder) fades/scales in at label
 size (2.7), centered at `(160, 70)`, in `boot` accent color.
3. `t=1000-1400ms`: unit number fades in below the wordmark, centered at
 `(160, 100)`, sub-line size, dim text color: `UNIT ####`, where `####`
 is derived from the device's own MAC/chip-id last two bytes - same
 scheme as the reference's `Claude-XXXX` BLE name
 (`main.cpp:10-19`, `snprintf(btName, ..., "Claude-%02X%02X", mac[4],
 mac[5])`) - reusing that exact pattern (last two MAC bytes,
 zero-padded hex) rather than inventing a new numbering scheme, since it
 already solves "make each unit distinguishable" and this device has the
 same multi-unit-in-one-room problem the reference called out
 (`REFERENCE.md:31-33`).
4. `t=1400-1800ms`: ring fades in at full size/position (2.1) already
 idling in `sleep` color, then boot hands off to whatever state the
 first heartbeat (or its absence) actually derives.

### 2.7 Font sizes

LovyanGFX font-size multiples of its 6x8 base glyph (matching the
reference's own convention, `buddyPrintSprite`/`setTextSize` throughout
`buddy.cpp`/`main.cpp`), applied to a bitmap font (e.g. built-in `Font2` /
`Font4`, or a loaded GFXFont - TBD by whoever picks the actual face):

| Role | Size multiple | Approx glyph box | Where |
|---|---|---|---|
| Center readout (ring interior) | 4x | 24x32px per glyph | inside the 40x40 center box, 2.1 - big enough for a 1-2 digit count or a single glyph |
| Boot wordmark | 3x | 18x24px per glyph | 2.6 step 2 |
| Label line (state name / short status) | 2x | 12x16px per glyph | 2.1 label line - ~23 chars fit the 280px budget |
| Sub line / unit number | 1x | 6x8px per glyph | 2.1 sub line, 2.6 step 3 - ~46 chars fit the 280px budget, plenty for "approve: Bash" style hints (reference caps these at 20-44 chars, `data.h:20-21`) |

### 2.8 Dim / sleep brightness levels

T-Display-S3's backlight is a direct PWM-driven GPIO (`PIN_LCD_BL = 38`,
confirmed in `vendor/T-Display-S3/examples/tft/pin_config.h:5`) via
`ledcWrite` or LovyanGFX's own `setBrightness()`, **not** an AXP192 PMIC
call like the M5StickC Plus reference uses (`M5.Axp.ScreenBreath()`,
Part 1.3) - this board has no AXP192, so that API doesn't apply here; the
reference's 0-100 `ScreenBreath` scale is analogous in spirit but the
actual call on this hardware is an 8-bit PWM duty cycle (0-255).

| Level | Duty (0-255) | Entered when | Analogous reference constant |
|---|---|---|---|
| Full | `255` | any state change, button/interaction, or `attention`/`busy`/`celebrate`/`error` active | brightness level 4 = 100 (`main.cpp:46,97`) |
| Dim (idle) | `70` | `idle` or `sleep` state held for `IDLE_DIM_TIMEOUT = 15000ms` with no state change | no direct equivalent - the reference has no intermediate dim tier, only full brightness or full nap-dim (`ScreenBreath(8)`, `main.cpp:1246`); this is a new two-stage design for codex-buddy since an always-on desk display benefits from a "still alive, just quiet" tier that a battery-and-pocket device (M5StickC Plus) doesn't need |
| Sleep-dim | `20` | `sleep` state held for `SLEEP_TIMEOUT = 60000ms` total with no heartbeat at all | roughly analogous to the reference's nap dim (`8`/100 ≈ 3%; `20`/255 ≈ 8%, same "barely visible" intent) |

`IDLE_DIM_TIMEOUT` (15s) and `SLEEP_TIMEOUT` (60s, measured from the same
30000ms no-heartbeat threshold that flips the state to `sleep` per 2.2,
plus a further 30s of staying there) are both new numbers for this device,
not sourced from the reference - chosen to be longer than the reference's
single 30s screen-off (Part 1.3) since this is a desk display meant to be
glanceable, not a handheld meant to save battery; going fully dark at 30s
would defeat the point. No full screen-off state is specified here at all
 - unlike the reference (`SCREEN_OFF_MS`, Part 1.3), which needs to save a
LiPo. If the deployed unit turns out to want one anyway (e.g. for a dark
room at night), that's an easy `Off` tier to add below Sleep-dim later; it
just isn't justified by anything in this recon.

### 2.9 LovyanGFX implementation notes

- Back buffer: one `LGFX_Sprite spr(&lcd); spr.createSprite(320, 170);
 spr.setColorDepth(16);` (RGB565, matching every color value in 2.3) - 
 same full-screen-sprite-then-push pattern as the reference (Part 1.6).
- Ring draws: `spr.fillArc(160, 68, 42, 52, startAngle, endAngle, color)`
 for the sweep/pulse states; a full 360° call for `sleep`'s breathing
 ring.
- Gate redraws on a dirty flag (state changed) OR a fixed tick interval,
 not unconditionally every loop - same rationale as the reference's
 `buddyTick()` gating (Part 1.6): a ring redraw is cheap, but there's no
 reason to `pushSprite` faster than the eye can see a difference. A
 100-150ms tick keeps every animation above smooth (10 steps/sec is
 visually indistinguishable from a genuine 60fps arc sweep at these
 speeds) while cutting SPI/parallel bus traffic roughly proportionally.
- Real T-Display-S3 pin set (8-bit parallel bus, not SPI - confirmed from
 `vendor/T-Display-S3/examples/tft/pin_config.h`, read in full):
 `LCD_D0-D7 = 39,40,41,42,45,46,47,48`, `LCD_CS=6, DC=7, WR=8, RD=9,
 RES=5, BL=38`, `POWER_ON=15`, buttons `BUTTON_1=0, BUTTON_2=14`. Use
 these for the `LGFX` panel/bus config (`Bus_Parallel8` +
 `Panel_ST7789`) rather than re-deriving them - they're read directly off
 the vendor SDK's own example, not guessed.

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
