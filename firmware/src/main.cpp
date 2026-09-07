// Codex Desk Companion - LILYGO T-Display-S3 firmware
//
// Shows a live OpenAI Codex CLI session as a colour ring + centre text.
// A Node host helper tails ~/.codex/ and streams newline-delimited JSON over
// USB CDC at 115200. Zero WiFi, zero accounts.
//
// Protocol, host -> device, one JSON object per line. Any field may be
// omitted and the device keeps the previous value:
//   {"state":"busy","ring":0.62,"center":"62%","label":"CTX",
//    "sub":"12:34 elapsed","tps":17.3}
// Device -> host:
//   "hello tdisplay-s3 v1" once on boot, "ok" per accepted line.
//   Malformed lines are ignored silently. A "state" value outside the five
//   names above counts as malformed: no "ok", no partial application.
//
// The palette is docs/design-spec.md part 2 verbatim, plus one ambient-only
// colour that part 2 has no state for. Animation timing is derived from it but
// diverges for "busy" and "idle"; see firmware/README.md.
//
// With no host attached the device runs an ambient screen instead of looking
// idle or asleep. See firmware/AMBIENT.md.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <math.h>

#include "LGFX_TDisplayS3.hpp"

#ifndef UNIT_ID
#define UNIT_ID 0
#endif

// Optional owner name, baked in at build time next to UNIT_ID:
//
//   PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex\""' pio run
//
// tools/flash-all.sh reads tools/units.txt and does that quoting for you.
// Unset (or set to an empty string) is a supported configuration, not an
// error: the boot and ambient screens fall back to the UNIT_ID line, so a
// board flashed with a plain `pio run -t upload` still reads correctly.
#ifndef UNIT_NAME
#define UNIT_NAME ""
#endif

static const char kUnitName[] = UNIT_NAME;
static inline bool haveUnitName() { return kUnitName[0] != '\0'; }

static constexpr const char* PRODUCT_NAME = "codex companion";

// ---------------------------------------------------------------------------
// Geometry (design-spec 2.1), landscape 320x170
// ---------------------------------------------------------------------------
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 170;
static constexpr int RING_CX  = 160;
static constexpr int RING_CY  = 68;
static constexpr int RING_R_OUT = 52;
static constexpr int RING_R_IN  = 42;
static constexpr int LABEL_Y  = 122;   // datum: top-centre
static constexpr int SUB_Y    = 146;   // datum: top-centre

// ---------------------------------------------------------------------------
// Palette (design-spec 2.3), RGB565
// ---------------------------------------------------------------------------
static constexpr uint16_t C_SLEEP     = 0x2189;  // #26314A
static constexpr uint16_t C_IDLE      = 0x3D1F;  // #3AA0FF
static constexpr uint16_t C_BUSY      = 0xFD84;  // #FFB020
static constexpr uint16_t C_WAIT      = 0xFE27;  // #FFC53D  amber
static constexpr uint16_t C_WAIT_HOT  = 0xF9C6;  // #FF3B30  escalated
static constexpr uint16_t C_DONE      = 0x362B;  // #34C759  green
static constexpr uint16_t C_BOOT      = 0x5C7F;  // #5B8CFF
static constexpr uint16_t C_BG        = 0x0000;
static constexpr uint16_t C_TEXT      = 0xFFFF;
static constexpr uint16_t C_TEXT_DIM  = 0x8410;
static constexpr uint16_t C_TRACK     = 0x18E3;  // dark grey ring track
// Ambient-only third colour stop. design-spec 2.3 has no "no host attached"
// state, so its table has nothing to borrow here; this is a new value, picked
// to sit between the two spec blues without reading as any live state.
static constexpr uint16_t C_AMB_TEAL  = 0x2E98;  // #2ED3C6

// ---------------------------------------------------------------------------
// Timing (design-spec 2.4 / 2.8 and the protocol brief)
// ---------------------------------------------------------------------------
static constexpr uint32_t BOOT_HOLD_MS      = 2000;
static constexpr uint32_t FRAME_MS          = 33;      // ~30 fps, sprite path
static constexpr uint32_t FRAME_MS_NOBUF    = 200;     // ~5 fps, direct-to-LCD
static constexpr uint32_t STALE_DIM_MS      = 30000;   // 30s no data -> dim
// 5 minutes with no host puts the device back into ambient mode. It used to
// force ST_SLEEP, which on a desk with no host software at all left a dark,
// near-dead-looking object; ambient is the resting look now, sleep is only
// ever entered because a host explicitly asked for it.
static constexpr uint32_t AMBIENT_AFTER_MS  = 300000;
static constexpr uint32_t AMBIENT_BRIGHT_MS = 60000;   // then settle a tier down
static constexpr uint32_t MODE_XFADE_MS     = 700;     // ambient <-> live handoff
static constexpr uint32_t AMB_SWEEP_MS      = 24000;   // one comet lap
static constexpr uint32_t AMB_BREATHE_MS    = 6500;    // slow brightness breathe
static constexpr uint32_t AMB_HUE_MS        = 45000;   // colour drift, 3 stops
// Burn-in drift. Two mutually prime-ish periods so the composition never
// retraces the same path, and an amplitude big enough to smear the text edges
// across several pixels over a few minutes.
static constexpr uint32_t AMB_DRIFT_X_MS    = 97000;
static constexpr uint32_t AMB_DRIFT_Y_MS    = 61000;
static constexpr float    AMB_DRIFT_AX      = 9.0f;
static constexpr float    AMB_DRIFT_AY      = 5.0f;
static constexpr float    AMB_ARC_DEG       = 76.0f;   // comet head + tail
static constexpr int      AMB_TAIL_SEGS     = 5;
static constexpr uint32_t IDLE_DIM_MS       = 15000;   // idle held -> dim tier
static constexpr uint32_t BUSY_PERIOD_MS    = 2400;    // slow breathe
static constexpr uint32_t IDLE_PERIOD_MS    = 2400;    // low-amplitude breathe
static constexpr uint32_t SLEEP_PERIOD_MS   = 4000;
static constexpr uint32_t WAIT_PERIOD_MAX   = 900;     // amber pulse, tps=0
// Fastest un-escalated pulse. Escalation halves it, so the shortest period the
// renderer ever sees is 350ms: at FRAME_MS = 33 that is ~10.6 frames per sine
// cycle, enough samples to read as a pulse instead of a strobe. Lowering this
// without also lowering FRAME_MS reintroduces the strobe.
static constexpr uint32_t WAIT_PERIOD_MIN   = 700;     // amber pulse, fast tps
static constexpr uint32_t WAIT_ESCALATE_MS  = 10000;   // -> red, halved period
static constexpr uint32_t DONE_FLASH_MS     = 600;
static constexpr uint32_t DONE_RISE_MS      = 120;     // flash ramps up first
static constexpr uint32_t DONE_XFADE_MS     = 150;     // then melts into idle
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 180;

// Backlight tiers (design-spec 2.8), 0-255 PWM duty.
static const uint8_t BRIGHT_LEVELS[] = { 255, 160, 70, 20 };
static constexpr uint8_t BRIGHT_LEVEL_COUNT =
    sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0]);
static constexpr uint8_t BRIGHT_FULL_IDX  = 0;
static constexpr uint8_t BRIGHT_DIM_IDX   = 2;   // 70
static constexpr uint8_t BRIGHT_SLEEP_IDX = 3;   // 20

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum StateId : uint8_t {
  ST_SLEEP = 0,
  ST_IDLE,
  ST_BUSY,
  ST_WAITING,
  ST_DONE,
};

struct State {
  StateId state = ST_IDLE;
  float   ring  = 0.0f;   // 0..1
  float   tps   = 0.0f;   // tokens/sec, >= 0
  char    center[16];
  char    label[24];
  char    sub[40];
};

static State st;

static LGFX_TDisplayS3 lcd;
static LGFX_Sprite     fb(&lcd);
static bool            fbReady = false;

// Draw target. Normally the off-screen sprite; if both sprite allocations fail
// this points straight at the panel so the unit still shows something (with
// visible tearing) instead of a permanently black screen. Both LGFX_Sprite and
// LGFX_Device derive from LovyanGFX, so the drawing calls are identical.
static LovyanGFX* gfx = nullptr;

static uint32_t lastDataMs      = 0;
static uint32_t waitEnterMs     = 0;   // when ST_WAITING was entered
static uint32_t doneEnterMs     = 0;   // when ST_DONE was entered
static uint32_t stateEnterMs    = 0;   // when the current st.state was entered
static uint32_t lastFrameMs     = 0;   // for frame-delta phase accumulation
static float    waitPhase       = 0.0f;  // 0..1, advanced by dt/period
static uint8_t  brightIdx       = BRIGHT_FULL_IDX;
static bool     brightManual    = false;  // button override beats auto-dim
static bool     dimmedByAuto    = false;  // auto tier currently holds us down
static uint8_t  lastTier        = 0;      // 0 fresh, 1 dim, 2 sleep-dim
static uint32_t lastButtonMs    = 0;
static bool     lastButtonLevel = true;   // pull-up: true == released
static uint32_t lastButton2Ms   = 0;
static bool     lastButton2Level = true;

// Ambient mode. `hostEverSpoke` latches on the first accepted protocol line
// and never clears: a unit that has been driven once still falls back to
// ambient when the host goes away, and a unit that never saw a host is in
// ambient from the first frame after the boot screen.
static bool     hostEverSpoke   = false;
static bool     ambientMode     = true;
static uint32_t ambientEnterMs  = 0;
static uint32_t modeChangeMs    = 0;
static uint16_t xfadeFrom       = C_BOOT;  // accent the last mode ended on
static uint16_t lastAccent      = C_BOOT;

// Cross-fade state read by every draw helper, so ambient and live frames
// share one handoff instead of each rolling their own.
static float    gXfade     = 1.0f;   // 0 = old mode's colour, 1 = settled
static uint16_t gXfadeFrom = C_BOOT;

// Whole-composition offset, in pixels. Non-zero only in ambient mode.
static int      gOx = 0;
static int      gOy = 0;

// Button-1 acknowledgement of a "waiting" prompt: quiets the pulse without
// changing the state the host reported. Cleared on the next state change.
static bool     waitAcked  = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline float clampf(float v, float lo, float hi) {
  if (!(v == v)) return lo;             // NaN guard
  return v < lo ? lo : (v > hi ? hi : v);
}

// Scale an RGB565 colour toward black by f (0..1), per channel.
static uint16_t scaleColor(uint16_t c, float f) {
  f = clampf(f, 0.0f, 1.0f);
  uint32_t r = (c >> 11) & 0x1F;
  uint32_t g = (c >> 5)  & 0x3F;
  uint32_t b =  c        & 0x1F;
  r = (uint32_t)(r * f + 0.5f);
  g = (uint32_t)(g * f + 0.5f);
  b = (uint32_t)(b * f + 0.5f);
  if (r > 0x1F) r = 0x1F;
  if (g > 0x3F) g = 0x3F;
  if (b > 0x1F) b = 0x1F;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Linear blend from RGB565 `a` to RGB565 `b` by f (0..1), per channel.
static uint16_t mixColor(uint16_t a, uint16_t b, float f) {
  f = clampf(f, 0.0f, 1.0f);
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = (int)((float)ar + (float)(br - ar) * f + 0.5f);
  int g = (int)((float)ag + (float)(bg - ag) * f + 0.5f);
  int bl = (int)((float)ab + (float)(bb - ab) * f + 0.5f);
  if (r < 0) r = 0; if (r > 0x1F) r = 0x1F;
  if (g < 0) g = 0; if (g > 0x3F) g = 0x3F;
  if (bl < 0) bl = 0; if (bl > 0x1F) bl = 0x1F;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Apply the mode cross-fade to a colour: at gXfade == 0 it is the dimmed
// accent the previous mode ended on, at 1 it is the colour as authored.
static uint16_t xf(uint16_t c) {
  return mixColor(scaleColor(gXfadeFrom, 0.30f), c, gXfade);
}

// 0..1 sine breathe from a 0..1 phase.
static float breatheFromPhase(float phase) {
  return 0.5f - 0.5f * cosf(clampf(phase, 0.0f, 1.0f) * 2.0f * (float)M_PI);
}

// 0..1 sine breathe over `period` ms, keyed off the wall clock. Fine for the
// fixed-period states; the waiting pulse uses the phase accumulator instead
// because its period moves with tps and a modulo would jump on every change.
static float breathe(uint32_t nowMs, uint32_t period) {
  if (period == 0) return 1.0f;
  return breatheFromPhase((float)(nowMs % period) / (float)period);
}

static void copyClamped(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  if (src == nullptr) { dst[0] = '\0'; return; }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

// Resolve a protocol state name. `*ok` is set false for anything not in the
// five-name enum, so the caller can reject the whole line rather than acking a
// host typo with "ok" while silently keeping the old state.
static StateId parseStateName(const char* s, bool* ok) {
  if (ok) *ok = true;
  if (s != nullptr) {
    if (!strcmp(s, "sleep"))   return ST_SLEEP;
    if (!strcmp(s, "idle"))    return ST_IDLE;
    if (!strcmp(s, "busy"))    return ST_BUSY;
    if (!strcmp(s, "waiting")) return ST_WAITING;
    if (!strcmp(s, "done"))    return ST_DONE;
  }
  if (ok) *ok = false;
  return st.state;
}

// "UNIT 03", or "UNIT -- AB12" when UNIT_ID was never injected.
//
// The MAC suffix must come from the LAST two bytes (mac[4], mac[5]), which are
// the per-device half. The first three are the Espressif OUI and are identical
// on every board in the batch, so a suffix taken from that end would print the
// same four hex digits on all 14 units. ESP.getEfuseMac() returns a uint64_t
// that IDF filled by writing 6 bytes through a uint8_t*, so on this
// little-endian target its low 16 bits are mac[0]/mac[1], i.e. the OUI.
// Reading the byte buffer directly sidesteps that trap and matches
// docs/design-spec.md 2.6 ("%02X%02X", mac[4], mac[5]).
static void unitIdString(char* out, size_t cap) {
#if UNIT_ID == 0
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  snprintf(out, cap, "UNIT -- %02X%02X", (unsigned)mac[4], (unsigned)mac[5]);
#else
  snprintf(out, cap, "UNIT %02d", (int)UNIT_ID);
#endif
}

static void setBrightness(uint8_t idx) {
  if (idx >= BRIGHT_LEVEL_COUNT) idx = BRIGHT_LEVEL_COUNT - 1;
  brightIdx = idx;
  lcd.setBrightness(BRIGHT_LEVELS[idx]);
}

// ---------------------------------------------------------------------------
// Serial line reader: non-blocking, 512-byte cap, discard on overflow.
// ---------------------------------------------------------------------------
static constexpr size_t LINE_CAP = 512;
static char   lineBuf[LINE_CAP];
static size_t lineLen  = 0;
static bool   lineOver = false;

static bool applyJsonLine(const char* line, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line, len);
  if (err) return false;
  if (!doc.is<JsonObject>()) return false;

  bool touched = false;

  JsonVariant v = doc["state"];
  if (!v.isNull() && v.is<const char*>()) {
    bool nameOk = false;
    StateId ns = parseStateName(v.as<const char*>(), &nameOk);
    // An unrecognised name is a host bug, not a keepalive: drop the whole line
    // silently rather than acking it while showing the previous state.
    if (!nameOk) return false;
    if (ns != st.state) {
      if (ns == ST_WAITING) { waitEnterMs = millis(); waitPhase = 0.0f; }
      // Arm the done flash on the 0->1 edge only. The host resends an
      // unchanged frame every heartbeatMs (2000ms) while a turn's numbers sit
      // still, so re-arming on a repeat would play the flash three times per
      // completed turn; a repeated "done" line is a keepalive, nothing more.
      if (ns == ST_DONE)    doneEnterMs = millis();
      // The acknowledgement is scoped to one prompt, so any state change
      // retires it. The next "waiting" starts impatient again.
      waitAcked    = false;
      st.state     = ns;
      stateEnterMs = millis();
    }
    touched = true;
  }

  v = doc["ring"];
  if (!v.isNull() && v.is<float>()) {
    st.ring = clampf(v.as<float>(), 0.0f, 1.0f);
    touched = true;
  }

  v = doc["tps"];
  if (!v.isNull() && v.is<float>()) {
    st.tps = clampf(v.as<float>(), 0.0f, 10000.0f);
    touched = true;
  }

  v = doc["center"];
  if (!v.isNull() && v.is<const char*>()) {
    copyClamped(st.center, sizeof(st.center), v.as<const char*>());
    touched = true;
  }

  v = doc["label"];
  if (!v.isNull() && v.is<const char*>()) {
    copyClamped(st.label, sizeof(st.label), v.as<const char*>());
    touched = true;
  }

  v = doc["sub"];
  if (!v.isNull() && v.is<const char*>()) {
    copyClamped(st.sub, sizeof(st.sub), v.as<const char*>());
    touched = true;
  }

  // An object with no recognised field is still well-formed JSON: accept it
  // as a keepalive so the host can hold the link open with "{}".
  (void)touched;
  return true;
}

static void pumpSerial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (lineLen > 0 && !lineOver) {
        lineBuf[lineLen] = '\0';
        if (applyJsonLine(lineBuf, lineLen)) {
          lastDataMs    = millis();
          hostEverSpoke = true;
          Serial.println("ok");
        }
        // malformed: ignored silently, no reply
      }
      lineLen  = 0;
      lineOver = false;
      continue;
    }
    if (lineOver) continue;                 // still draining an over-long line
    if (lineLen + 1 >= LINE_CAP) {          // leave room for the NUL
      lineOver = true;                      // discard the whole line
      lineLen  = 0;
      continue;
    }
    lineBuf[lineLen++] = (char)c;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// LovyanGFX arc angles: degrees, 0 = +X (3 o'clock), increasing clockwise.
// Ring value starts at 12 o'clock.
static constexpr float ARC_START = -90.0f;

static void drawRing(float fraction, uint16_t color, int rIn, int rOut,
                     uint16_t trackColor) {
  if (rIn < 0) rIn = 0;
  if (rOut <= rIn) rOut = rIn + 1;
  const int cx = RING_CX + gOx;
  const int cy = RING_CY + gOy;
  color      = xf(color);
  trackColor = xf(trackColor);
  gfx->fillArc(cx, cy, rIn, rOut, 0.0f, 360.0f, trackColor);
  float f = clampf(fraction, 0.0f, 1.0f);
  if (f <= 0.0f) return;
  if (f >= 1.0f) {
    gfx->fillArc(cx, cy, rIn, rOut, 0.0f, 360.0f, color);
    return;
  }
  gfx->fillArc(cx, cy, rIn, rOut, ARC_START, ARC_START + 360.0f * f, color);
}

// Amber pulse period: shortens as tokens/sec rises, then halves once the
// wait passes the 10s escalation threshold (design-spec 2.4).
static uint32_t waitPeriodMs(float tps, bool escalated) {
  float t = clampf(tps, 0.0f, 60.0f);
  float span = (float)(WAIT_PERIOD_MAX - WAIT_PERIOD_MIN);
  uint32_t p = (uint32_t)((float)WAIT_PERIOD_MAX - span * (t / 60.0f));
  if (escalated) p = p / 2;
  // Defensive floor: keeps the pulse above ~9 render frames per cycle if the
  // WAIT_PERIOD_* constants above are ever lowered. Unreachable at the current
  // constants (the smallest value this can compute is 700/2 = 350).
  if (p < 300) p = 300;
  return p;
}

// Widest the centre string may be before it starts landing on the ring stroke.
// The ring hole is 2 * RING_R_IN across; leave a 4px margin either side.
static constexpr int CENTER_MAX_W = 2 * RING_R_IN - 8;

static void drawText(uint32_t nowMs, uint16_t accent, bool showCenter) {
  (void)nowMs;
  gfx->setTextDatum(textdatum_t::top_center);

  if (st.label[0] != '\0') {
    // Primary tier: the label is the word the recipient reads at desk
    // distance, so it gets C_TEXT and the sub line keeps C_TEXT_DIM.
    gfx->setTextColor(scaleColor(C_TEXT, gXfade), C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->setTextSize(1);
    gfx->drawString(st.label, RING_CX + gOx, LABEL_Y + gOy);
  }
  if (st.sub[0] != '\0') {
    gfx->setTextColor(scaleColor(C_TEXT_DIM, gXfade), C_BG);
    gfx->setFont(&fonts::Font0);
    gfx->setTextSize(1);
    gfx->drawString(st.sub, RING_CX + gOx, SUB_Y + gOy);
  }
  if (showCenter && st.center[0] != '\0') {
    gfx->setTextDatum(textdatum_t::middle_center);
    // No opaque background here: renderFrame clears the whole buffer every
    // frame, so an opaque box buys nothing and a long string would punch it
    // straight through the ring stroke at mid-height.
    gfx->setTextColor(xf(accent));
    gfx->setTextSize(1);

    char buf[sizeof(st.center)];
    copyClamped(buf, sizeof(buf), st.center);

    // 4 characters or fewer get the big face; anything longer steps down, and
    // whatever is still too wide for the ring hole is truncated rather than
    // drawn over the stroke. The protocol admits 15 characters; only ~9 of
    // them fit inside the hole at Font2.
    if (strlen(buf) <= 4) gfx->setFont(&fonts::Font4);
    else                  gfx->setFont(&fonts::Font2);
    if (gfx->textWidth(buf) > CENTER_MAX_W) gfx->setFont(&fonts::Font0);
    for (size_t n = strlen(buf); n > 0 && gfx->textWidth(buf) > CENTER_MAX_W;
         --n) {
      buf[n - 1] = '\0';
    }

    if (buf[0] != '\0') gfx->drawString(buf, RING_CX + gOx, RING_CY + gOy);
  }
}

// The state actually rendered this frame, after the staleness overrides.
// Shared by renderFrame and updateBrightness so the panel's content and its
// backlight can never disagree about what the device is doing.
//
// Past AMBIENT_AFTER_MS this value stops being rendered at all: ambient mode
// takes the screen. ST_SLEEP is therefore only ever reached because a host
// asked for it, never because a host went missing.
static StateId effectiveState(uint32_t nowMs) {
  uint32_t sinceData = nowMs - lastDataMs;
  // Past the 30s dim mark the host is gone but not yet declared dead. Holding
  // the escalated red "waiting" pulse there would keep demanding attention for
  // an approval prompt that no longer exists, so the two active states fall
  // back to the idle look. The stored st.state is untouched: one fresh line
  // restores the real look immediately.
  if (sinceData >= STALE_DIM_MS &&
      (st.state == ST_WAITING || st.state == ST_BUSY)) {
    return ST_IDLE;
  }
  return st.state;
}

static void renderLive(uint32_t nowMs, uint32_t dtMs) {
  StateId  eff = effectiveState(nowMs);

  // The ack belongs to one prompt. A state change clears it in the parser;
  // this clears it when the 30s staleness fallback pulls the rendered state
  // off "waiting" without any line arriving to do it.
  if (eff != ST_WAITING) waitAcked = false;

  uint16_t accent = C_IDLE;
  float    level  = 1.0f;

  switch (eff) {
    case ST_SLEEP: {
      // Dim, slow full-ring breathe: 15% -> 45% -> 15%.
      float b = breathe(nowMs, SLEEP_PERIOD_MS);
      level  = 0.15f + 0.30f * b;
      accent = C_SLEEP;
      drawRing(1.0f, scaleColor(accent, level), RING_R_IN, RING_R_OUT,
               scaleColor(C_TRACK, 0.4f));
      break;
    }
    case ST_IDLE: {
      // Ring shows the reported fill. A low-amplitude brightness breathe keeps
      // the frame from being byte-identical every tick: this is the state the
      // device spends most of its life in, and a genuinely frozen panel is
      // indistinguishable from a crashed one. The centre text stays at full
      // accent so the number itself does not flicker.
      float b = breathe(nowMs, IDLE_PERIOD_MS);
      accent = C_IDLE;
      drawRing(st.ring, scaleColor(C_IDLE, 0.75f + 0.25f * b), RING_R_IN,
               RING_R_OUT, C_TRACK);
      break;
    }
    case ST_BUSY: {
      // Slow breathing ring: brightness and stroke both breathe.
      float b = breathe(nowMs, BUSY_PERIOD_MS);
      level  = 0.55f + 0.45f * b;
      accent = C_BUSY;
      int rIn = RING_R_OUT - (int)(10.0f + 3.0f * b);
      if (rIn < 0) rIn = 0;
      drawRing(st.ring, scaleColor(accent, level), rIn, RING_R_OUT, C_TRACK);
      break;
    }
    case ST_WAITING: {
      if (waitAcked) {
        // Button 1 was pressed while this prompt was up: "I have seen it."
        // Hold a calm, steady amber ring instead. No pulse, no escalation to
        // red, no stroke movement. The state itself is untouched, so the
        // colour still says a prompt is pending, it just stops nagging.
        // Reset the phase so a later un-acked wait starts from the trough.
        waitPhase = 0.0f;
        accent    = C_WAIT;
        level     = 0.62f;
        drawRing(st.ring, scaleColor(accent, level), RING_R_IN, RING_R_OUT,
                 scaleColor(C_TRACK, 0.5f));
        break;
      }
      // Amber pulse; faster with tps, escalates to red after 10s.
      uint32_t waited = nowMs - waitEnterMs;
      bool escalated  = waited >= WAIT_ESCALATE_MS;
      uint32_t period = waitPeriodMs(st.tps, escalated);
      // Advance an explicit phase so a tps-driven period change speeds the
      // pulse up smoothly instead of snapping it to a new modulo position.
      waitPhase += (float)dtMs / (float)period;
      while (waitPhase >= 1.0f) waitPhase -= 1.0f;
      float b = breatheFromPhase(waitPhase);
      level  = 0.45f + 0.55f * b;
      accent = escalated ? C_WAIT_HOT : C_WAIT;
      int rIn = RING_R_OUT - (int)(8.0f + 4.0f * b);
      if (rIn < 0) rIn = 0;
      // Draw the reported fill, not a hardcoded 1.0: the centre text shows the
      // same metric, so a full ring next to "71%" put two contradictory
      // numbers on screen. Colour, brightness and stroke width already carry
      // the waiting signal on their own.
      drawRing(st.ring, scaleColor(accent, level), rIn, RING_R_OUT,
               scaleColor(C_TRACK, 0.5f));
      break;
    }
    case ST_DONE: {
      uint32_t since = nowMs - doneEnterMs;
      if (since < DONE_FLASH_MS) {
        // A real flash: ramp up over DONE_RISE_MS, then decay. A monotonic
        // fade-out has no rise and does not read as a flash at all.
        float k;
        if (since < DONE_RISE_MS) {
          k = (float)since / (float)DONE_RISE_MS;
        } else {
          k = 1.0f - (float)(since - DONE_RISE_MS) /
                     (float)(DONE_FLASH_MS - DONE_RISE_MS);
        }
        level = 0.4f + 0.6f * clampf(k, 0.0f, 1.0f);

        // Cross-fade the last DONE_XFADE_MS into the idle look: colour, ring
        // fraction and inner radius all land exactly on the idle values at the
        // 600ms mark, so the handoff is continuous rather than a one-frame cut
        // from 40% green to full-brightness blue.
        uint32_t xStart = DONE_FLASH_MS - DONE_XFADE_MS;
        float x = (since >= xStart)
                    ? (float)(since - xStart) / (float)DONE_XFADE_MS
                    : 0.0f;
        x = clampf(x, 0.0f, 1.0f);

        uint16_t ringCol = mixColor(scaleColor(C_DONE, level), C_IDLE, x);
        float frac = st.ring + (1.0f - st.ring) * (1.0f - x);
        int   rIn  = RING_R_IN - (int)(2.0f * (1.0f - x));
        drawRing(frac, ringCol, rIn, RING_R_OUT, C_TRACK);
        accent = mixColor(C_DONE, C_IDLE, x);
      } else {
        // Flash finished: behave as idle until the host says otherwise.
        accent = C_IDLE;
        drawRing(st.ring, accent, RING_R_IN, RING_R_OUT, C_TRACK);
      }
      break;
    }
  }

  drawText(nowMs, accent, true);
  lastAccent = accent;
}

// ---------------------------------------------------------------------------
// Ambient mode
//
// What the device does when no host has ever spoken, and what it falls back to
// AMBIENT_AFTER_MS after one goes away. The whole point of this mode is that a
// recipient who never installs the host software still gets an object worth
// leaving switched on, so it deliberately shows no error, no "waiting for
// host", and no still frame: the comet is always moving, even at the bottom of
// the breathe.
// ---------------------------------------------------------------------------

// Three-stop colour loop. Two of the stops are design-spec 2.3 blues; the
// third is the ambient-only teal, which is what stops the drift from reading
// as one flat blue for 45 seconds.
static uint16_t ambientAccent(uint32_t nowMs) {
  static const uint16_t stops[3] = { C_IDLE, C_AMB_TEAL, C_BOOT };
  float p   = (float)(nowMs % AMB_HUE_MS) / (float)AMB_HUE_MS * 3.0f;
  int   seg = (int)p;
  if (seg > 2) seg = 2;
  return mixColor(stops[seg], stops[(seg + 1) % 3], p - (float)seg);
}

static void renderAmbient(uint32_t nowMs) {
  const int cx = RING_CX + gOx;
  const int cy = RING_CY + gOy;

  uint16_t accent = ambientAccent(nowMs);
  float    level  = 0.45f + 0.45f * breathe(nowMs, AMB_BREATHE_MS);

  // Faint full track so the ring reads as a ring, not a lone floating arc.
  gfx->fillArc(cx, cy, RING_R_IN, RING_R_OUT, 0.0f, 360.0f,
               xf(scaleColor(C_TRACK, 0.7f)));

  // Comet: a head plus a tail of dimmer segments, one lap per AMB_SWEEP_MS.
  // At 24s a lap that is about 15 degrees per second, slow enough to read as
  // drifting rather than spinning.
  float head = ARC_START +
               360.0f * ((float)(nowMs % AMB_SWEEP_MS) / (float)AMB_SWEEP_MS);
  float seg  = AMB_ARC_DEG / (float)AMB_TAIL_SEGS;
  for (int i = 0; i < AMB_TAIL_SEGS; ++i) {
    float k = level * (1.0f - 0.19f * (float)i);
    // Half a degree of overlap: adjacent fillArc calls otherwise leave a
    // visible hairline of track colour between segments.
    gfx->fillArc(cx, cy, RING_R_IN, RING_R_OUT,
                 head - seg * (float)(i + 1) - 0.5f, head - seg * (float)i,
                 xf(scaleColor(accent, k)));
  }

  // Text. The owner name is the headline when there is one; otherwise the
  // product name takes the top line and the unit id sits below it. Nothing
  // here ever says "no host", because from the recipient's side there is
  // nothing wrong.
  char unit[24];
  unitIdString(unit, sizeof(unit));

  gfx->setTextDatum(textdatum_t::top_center);
  gfx->setTextSize(1);

  gfx->setTextColor(scaleColor(C_TEXT, gXfade * 0.88f), C_BG);
  gfx->setFont(&fonts::Font2);
  gfx->drawString(haveUnitName() ? kUnitName : PRODUCT_NAME,
                  RING_CX + gOx, LABEL_Y + gOy);

  gfx->setTextColor(scaleColor(C_TEXT_DIM, gXfade), C_BG);
  gfx->setFont(&fonts::Font0);
  gfx->drawString(haveUnitName() ? PRODUCT_NAME : unit,
                  RING_CX + gOx, SUB_Y + gOy);

  lastAccent = accent;
}

// True whenever the live view has nothing honest to show.
static bool ambientActive(uint32_t nowMs) {
  if (!hostEverSpoke) return true;
  return (nowMs - lastDataMs) >= AMBIENT_AFTER_MS;
}

// Flip the mode and arm the cross-fade. Called from loop() before both the
// brightness ladder and the renderer, so the two can never disagree about
// which mode this tick is in.
static void updateMode(uint32_t nowMs) {
  bool amb = ambientActive(nowMs);
  if (amb == ambientMode) return;
  ambientMode  = amb;
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  if (amb) ambientEnterMs = nowMs;
}

static void renderFrame(uint32_t nowMs) {
  uint32_t dtMs = nowMs - lastFrameMs;
  if (dtMs > 500) dtMs = 500;          // clamp after a stall, no phase jump
  lastFrameMs = nowMs;

  gXfade     = (MODE_XFADE_MS == 0)
                 ? 1.0f
                 : clampf((float)(nowMs - modeChangeMs) / (float)MODE_XFADE_MS,
                          0.0f, 1.0f);
  gXfadeFrom = xfadeFrom;

  // Burn-in drift, applied to the whole composition. It winds up as ambient
  // takes over and unwinds to exactly zero as live does, so the live layout
  // still lands on the pixel-exact geometry of design-spec 2.1.
  float driftK = ambientMode ? gXfade : (1.0f - gXfade);
  float px = sinf((float)(nowMs % AMB_DRIFT_X_MS) / (float)AMB_DRIFT_X_MS *
                  2.0f * (float)M_PI);
  float py = sinf((float)(nowMs % AMB_DRIFT_Y_MS) / (float)AMB_DRIFT_Y_MS *
                  2.0f * (float)M_PI);
  gOx = (int)lroundf(AMB_DRIFT_AX * px * driftK);
  gOy = (int)lroundf(AMB_DRIFT_AY * py * driftK);

  gfx->fillScreen(C_BG);
  if (ambientMode) renderAmbient(nowMs);
  else             renderLive(nowMs, dtMs);
  if (fbReady) fb.pushSprite(0, 0);
}

static void drawBootScreen() {
  gfx->fillScreen(C_BG);
  gfx->setTextDatum(textdatum_t::middle_center);
  gfx->setTextColor(C_BOOT, C_BG);
  gfx->setFont(&fonts::Font4);
  gfx->setTextSize(1);
  gfx->drawString(PRODUCT_NAME, SCREEN_W / 2, 70);

  // UNIT_ID is never injected on the plain `pio run -t upload` path, so
  // unitIdString() renders "UNIT -- <MAC>" there rather than a "UNIT 00" that
  // would read like a real serial number. See its comment for why the suffix
  // has to come from the tail of the MAC.
  char unit[24];
  unitIdString(unit, sizeof(unit));

  if (haveUnitName()) {
    // Named build: the owner's name is the second line and the unit id drops
    // to a third, smaller line. Both are still shown, so assembly-day
    // identification does not depend on remembering who got which name.
    gfx->setTextColor(C_TEXT, C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->drawString(kUnitName, SCREEN_W / 2, 106);
    gfx->setTextColor(C_TEXT_DIM, C_BG);
    gfx->setFont(&fonts::Font0);
    gfx->drawString(unit, SCREEN_W / 2, 132);
  } else {
    gfx->setTextColor(C_TEXT_DIM, C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->drawString(unit, SCREEN_W / 2, 108);
  }
  if (fbReady) fb.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Button 1 (GPIO 0, active low): acknowledge a pending prompt if there is one,
// otherwise the pre-existing behaviour -- wake to full brightness, then cycle
// brightness tiers. Brightness now also has a dedicated button (button 2), but
// button 1 keeps its cycling so a unit whose GPIO 14 switch is unreachable in
// its case is not left without one.
// ---------------------------------------------------------------------------
static void pumpButton(uint32_t nowMs) {
  bool level = digitalRead(PIN_BUTTON_1) != 0;   // true == released (pull-up)
  if (level == lastButtonLevel) return;
  lastButtonLevel = level;
  if (level) return;                             // only act on press
  if (nowMs - lastButtonMs < BUTTON_DEBOUNCE_MS) return;
  lastButtonMs = nowMs;

  // Acknowledge takes priority over everything else while a prompt is up. One
  // press quiets the pulse; a second press falls through to the brightness
  // behaviour below, so the button is never a dead key.
  if (!ambientMode && !waitAcked && effectiveState(nowMs) == ST_WAITING) {
    waitAcked = true;
    if (dimmedByAuto) {                          // an ack is also an interaction
      setBrightness(BRIGHT_FULL_IDX);
      dimmedByAuto = false;
      brightManual = true;
    }
    return;
  }

  // The wake case is tracked with an explicit flag rather than inferred from
  // brightIdx. Inferring it made the "cycle" a two-level toggle: from 255 the
  // else-branch gave 160, and from 160 the if-branch went straight back to
  // 255, so 70 and 20 were unreachable by any sequence of presses.
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);              // first press wakes
    dimmedByAuto = false;
  } else {
    setBrightness((uint8_t)((brightIdx + 1) % BRIGHT_LEVEL_COUNT));
  }
  brightManual = true;
}

// ---------------------------------------------------------------------------
// Button 2 (GPIO 14, active low, same INPUT_PULLUP wiring as button 1 in every
// vendor example that reads it): a dedicated brightness cycle. It was
// previously unused. Unlike button 1 it never wakes-then-cycles: one press is
// always exactly one step, which is what makes it predictable as the
// brightness control.
// ---------------------------------------------------------------------------
static void pumpButton2(uint32_t nowMs) {
  bool level = digitalRead(PIN_BUTTON_2) != 0;   // true == released (pull-up)
  if (level == lastButton2Level) return;
  lastButton2Level = level;
  if (level) return;                             // only act on press
  if (nowMs - lastButton2Ms < BUTTON_DEBOUNCE_MS) return;
  lastButton2Ms = nowMs;

  setBrightness((uint8_t)((brightIdx + 1) % BRIGHT_LEVEL_COUNT));
  brightManual = true;
  dimmedByAuto = false;
}

// Auto brightness. Two inputs, and the dimmer of the two wins:
//   staleness  - full while fresh, dim after 30s, sleep-dim after 5 min
//   state      - sleep dims immediately, idle dims once it has been held
//                IDLE_DIM_MS, everything else runs at full
// Without the state input the tiers were unreachable in practice: the host
// helper sends a keepalive every 2000ms, so `since` never approached 30s while
// it was running and a host-declared "sleep" sat at full backlight all night.
//
// A button press pins the level; the pin is released only when the tier
// actually changes, so a hand-picked level survives a busy data stream.
static void updateBrightness(uint32_t nowMs, StateId eff) {
  uint8_t tier = 0;

  if (ambientMode) {
    // Ambient is the resting look, not a fault, so it stays properly lit for a
    // minute (a freshly plugged-in unit should look like it is showing you
    // something) and then settles one tier down for the rest of its life. It
    // never reaches the sleep tier: the whole point is that it stays readable.
    tier = (nowMs - ambientEnterMs) >= AMBIENT_BRIGHT_MS ? 1 : 0;
  } else {
    if ((nowMs - lastDataMs) >= STALE_DIM_MS) tier = 1;

    uint8_t stateTier = 0;
    if (eff == ST_SLEEP) {
      stateTier = 2;
    } else if (eff == ST_IDLE && (nowMs - stateEnterMs) >= IDLE_DIM_MS) {
      stateTier = 1;
    }
    if (stateTier > tier) tier = stateTier;
  }

  static const uint8_t TIER_IDX[3] = {
    BRIGHT_FULL_IDX, BRIGHT_DIM_IDX, BRIGHT_SLEEP_IDX
  };

  if (tier != lastTier) {
    lastTier     = tier;
    brightManual = false;
    dimmedByAuto = (tier != 0);
    setBrightness(TIER_IDX[tier]);
    return;
  }
  if (brightManual) return;
  dimmedByAuto = (tier != 0);
  if (brightIdx != TIER_IDX[tier]) setBrightness(TIER_IDX[tier]);
}

// ---------------------------------------------------------------------------
void setup() {
  // Board power rail must come up before anything else touches the panel.
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  pinMode(PIN_BUTTON_1, INPUT_PULLUP);
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);

  // HWCDC's RX queue defaults to 256 bytes. The helper starts streaming the
  // moment the port opens, without waiting for the hello line, so several
  // ~90-byte frames can arrive during the boot hold below; 256 bytes would
  // overflow mid-line and hand the parser a fragment.
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);

  st.center[0] = '\0';
  st.label[0]  = '\0';
  st.sub[0]    = '\0';
  copyClamped(st.label, sizeof(st.label), "CODEX");

  lcd.init();
  // Rotation 2, not 1. cfg.offset_rotation is 1 (LGFX_TDisplayS3.hpp), and
  // Panel_LCD::setRotation adds the two before deciding whether to swap the
  // panel's 170x320 dimensions: rotations 0 and 2 are the landscape 320x170
  // pair, 1 and 3 are portrait. setRotation(1) gave a 170-wide panel and
  // clipped the whole UI. 2 is what every LovyanGFX config in the vendor SDK
  // ships with (e.g. examples/T-Display-S3-Queue/ST7789_Handler.cpp:17).
  lcd.setRotation(2);          // landscape, 320x170
  lcd.setBrightness(BRIGHT_LEVELS[BRIGHT_FULL_IDX]);
  lcd.fillScreen(C_BG);

  fb.setColorDepth(16);
  fb.setPsram(true);           // full-screen back buffer in PSRAM
  fbReady = (fb.createSprite(SCREEN_W, SCREEN_H) != nullptr);
  if (!fbReady) {              // PSRAM refused: retry from internal RAM
    fb.setPsram(false);
    fbReady = (fb.createSprite(SCREEN_W, SCREEN_H) != nullptr);
  }
  // Both allocations failed. Draw straight to the panel instead of returning
  // early from every draw call, which left a powered-but-black unit with no
  // way to tell it apart from a panel wiring fault. Say so on serial too.
  gfx = fbReady ? (LovyanGFX*)&fb : (LovyanGFX*)&lcd;
  if (!fbReady) Serial.println("err: sprite alloc failed, drawing direct");

  drawBootScreen();
  uint32_t bootStart = millis();
  while (millis() - bootStart < BOOT_HOLD_MS) {
    // Drain the RX queue during the hold. The helper is already streaming, and
    // bytes left unread here would overflow the queue and corrupt the first
    // real line.
    pumpSerial();
    pumpButton(millis());
    pumpButton2(millis());
    delay(10);
  }

  Serial.println("hello tdisplay-s3 v1");

  lastDataMs      = millis();
  waitEnterMs     = millis();
  doneEnterMs     = millis();
  stateEnterMs    = millis();
  lastFrameMs     = millis();
  lastButtonLevel  = digitalRead(PIN_BUTTON_1) != 0;
  lastButton2Level = digitalRead(PIN_BUTTON_2) != 0;

  // Hand the boot screen over to ambient with the same cross-fade the device
  // uses between modes, so the first thing after the wordmark is a fade rather
  // than a cut. A line that arrived during the boot hold has already latched
  // hostEverSpoke, so a board plugged into a running host goes straight to
  // live and the fade runs in that direction instead.
  ambientMode    = ambientActive(millis());
  ambientEnterMs = millis();
  modeChangeMs   = millis();
  xfadeFrom      = C_BOOT;
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();

  pumpSerial();
  pumpButton(now);
  pumpButton2(now);
  updateMode(now);
  updateBrightness(now, effectiveState(now));

  // Direct-to-panel fallback has no back buffer, so it redraws far more slowly
  // to keep the tearing from turning into a strobe.
  uint32_t frameMs = fbReady ? FRAME_MS : FRAME_MS_NOBUF;
  if (now - lastDraw >= frameMs) {
    lastDraw = now;
    renderFrame(now);
  }
  delay(1);
}
