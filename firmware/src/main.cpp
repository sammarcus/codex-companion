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
// The palette is docs/design-spec.md part 2 verbatim. Animation timing is
// derived from it but diverges for "busy" and "idle"; see firmware/README.md.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <math.h>

#include "LGFX_TDisplayS3.hpp"

#ifndef UNIT_ID
#define UNIT_ID 0
#endif

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

// ---------------------------------------------------------------------------
// Timing (design-spec 2.4 / 2.8 and the protocol brief)
// ---------------------------------------------------------------------------
static constexpr uint32_t BOOT_HOLD_MS      = 2000;
static constexpr uint32_t FRAME_MS          = 33;      // ~30 fps, sprite path
static constexpr uint32_t FRAME_MS_NOBUF    = 200;     // ~5 fps, direct-to-LCD
static constexpr uint32_t STALE_DIM_MS      = 30000;   // 30s no data -> dim
static constexpr uint32_t STALE_SLEEP_MS    = 300000;  // 5 min no data -> sleep
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
          lastDataMs = millis();
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
  gfx->fillArc(RING_CX, RING_CY, rIn, rOut, 0.0f, 360.0f, trackColor);
  float f = clampf(fraction, 0.0f, 1.0f);
  if (f <= 0.0f) return;
  if (f >= 1.0f) {
    gfx->fillArc(RING_CX, RING_CY, rIn, rOut, 0.0f, 360.0f, color);
    return;
  }
  gfx->fillArc(RING_CX, RING_CY, rIn, rOut, ARC_START, ARC_START + 360.0f * f,
               color);
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
    gfx->setTextColor(C_TEXT, C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->setTextSize(1);
    gfx->drawString(st.label, RING_CX, LABEL_Y);
  }
  if (st.sub[0] != '\0') {
    gfx->setTextColor(C_TEXT_DIM, C_BG);
    gfx->setFont(&fonts::Font0);
    gfx->setTextSize(1);
    gfx->drawString(st.sub, RING_CX, SUB_Y);
  }
  if (showCenter && st.center[0] != '\0') {
    gfx->setTextDatum(textdatum_t::middle_center);
    // No opaque background here: renderFrame clears the whole buffer every
    // frame, so an opaque box buys nothing and a long string would punch it
    // straight through the ring stroke at mid-height.
    gfx->setTextColor(accent);
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

    if (buf[0] != '\0') gfx->drawString(buf, RING_CX, RING_CY);
  }
}

// The state actually rendered this frame, after the staleness overrides.
// Shared by renderFrame and updateBrightness so the panel's content and its
// backlight can never disagree about what the device is doing.
//
// lastDataMs is seeded at boot, so a board sitting on a desk with no host dims
// and then sleeps on the same schedule as one whose host went away.
static StateId effectiveState(uint32_t nowMs) {
  uint32_t sinceData = nowMs - lastDataMs;
  // 5 minutes with no host data forces sleep regardless of last reported state.
  if (sinceData >= STALE_SLEEP_MS) return ST_SLEEP;
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

static void renderFrame(uint32_t nowMs) {
  gfx->fillScreen(C_BG);

  uint32_t dtMs = nowMs - lastFrameMs;
  if (dtMs > 500) dtMs = 500;          // clamp after a stall, no phase jump
  lastFrameMs = nowMs;

  uint32_t sinceData  = nowMs - lastDataMs;
  bool     staleSleep = sinceData >= STALE_SLEEP_MS;
  StateId  eff        = effectiveState(nowMs);

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

  // A stale-sleep frame carries whatever percentage was last received, which
  // may be minutes old, so the number is dropped rather than shown as current.
  drawText(nowMs, accent, !staleSleep);
  if (fbReady) fb.pushSprite(0, 0);
}

static void drawBootScreen() {
  gfx->fillScreen(C_BG);
  gfx->setTextDatum(textdatum_t::middle_center);
  gfx->setTextColor(C_BOOT, C_BG);
  gfx->setFont(&fonts::Font4);
  gfx->setTextSize(1);
  gfx->drawString("codex companion", SCREEN_W / 2, 70);

  char unit[24];
#if UNIT_ID == 0
  // UNIT_ID was never injected. tools/flash-all.sh passes -DUNIT_ID=<n> for
  // units 1..14, so reaching this means the plain `pio run -t upload` path was
  // used. "UNIT 00" would look like a real serial number; show an obviously
  // unset marker plus the efuse MAC suffix so 14 identical boards are still
  // tellable apart.
  //
  // The suffix must come from the LAST two MAC bytes (mac[4], mac[5]), which
  // are the per-device half. The first three are the Espressif OUI and are
  // identical on every board in the batch, so a suffix taken from that end
  // would print the same four hex digits on all 14 units. ESP.getEfuseMac()
  // returns a uint64_t that IDF filled by writing 6 bytes through a uint8_t*
  // (Esp.cpp: esp_efuse_mac_get_default((uint8_t*)&_chipmacid)), so on this
  // little-endian target its low 16 bits are mac[0]/mac[1], i.e. the OUI.
  // Reading the byte buffer directly sidesteps that trap entirely and matches
  // docs/design-spec.md 2.6 ("%02X%02X", mac[4], mac[5]).
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  snprintf(unit, sizeof(unit), "UNIT -- %02X%02X",
           (unsigned)mac[4], (unsigned)mac[5]);
#else
  snprintf(unit, sizeof(unit), "UNIT %02d", (int)UNIT_ID);
#endif
  gfx->setTextColor(C_TEXT_DIM, C_BG);
  gfx->setFont(&fonts::Font2);
  gfx->drawString(unit, SCREEN_W / 2, 108);
  if (fbReady) fb.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Button 1 (GPIO 0, active low): wake to full brightness, then cycle tiers.
// ---------------------------------------------------------------------------
static void pumpButton(uint32_t nowMs) {
  bool level = digitalRead(PIN_BUTTON_1) != 0;   // true == released (pull-up)
  if (level == lastButtonLevel) return;
  lastButtonLevel = level;
  if (level) return;                             // only act on press
  if (nowMs - lastButtonMs < BUTTON_DEBOUNCE_MS) return;
  lastButtonMs = nowMs;

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
  uint32_t since = nowMs - lastDataMs;
  uint8_t tier = 0;
  if (since >= STALE_SLEEP_MS)     tier = 2;
  else if (since >= STALE_DIM_MS)  tier = 1;

  uint8_t stateTier = 0;
  if (eff == ST_SLEEP) {
    stateTier = 2;
  } else if (eff == ST_IDLE && (nowMs - stateEnterMs) >= IDLE_DIM_MS) {
    stateTier = 1;
  }
  if (stateTier > tier) tier = stateTier;

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
    delay(10);
  }

  Serial.println("hello tdisplay-s3 v1");

  lastDataMs      = millis();
  waitEnterMs     = millis();
  doneEnterMs     = millis();
  stateEnterMs    = millis();
  lastFrameMs     = millis();
  lastButtonLevel = digitalRead(PIN_BUTTON_1) != 0;
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();

  pumpSerial();
  pumpButton(now);
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
