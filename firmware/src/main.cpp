// Codex Desk Companion - LILYGO T-Display-S3 firmware
//
// Shows a live OpenAI Codex CLI session as a colour ring + centre text.
// A Node host helper tails ~/.codex/ and streams newline-delimited JSON over
// USB CDC at 115200. Zero WiFi, zero accounts.
//
// Protocol, host -> device, one JSON object per line. Any field may be
// omitted and the device keeps the previous value:
//   {"state":"busy","ring":0.62,"center":"62%","label":"CTX",
//    "sub":"12:34 elapsed","tps":17.3,"name":"Alex Rivera"}
// "name" is the one field with a side effect beyond the current frame: it is
// persisted to NVS and survives a power cycle. "" clears it.
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
//
// The thing on the panel is a face: a pair of drawn eyes that blink, glance
// around and change expression per state. See firmware/EYES.md for the
// geometry, the blink timing model and the composition in both modes.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <math.h>

#include "LGFX_TDisplayS3.hpp"

#ifndef UNIT_ID
#define UNIT_ID 0
#endif

// Owner name. There are three sources, in this precedence order:
//
//   1. the name stored in NVS, set at runtime by a protocol line carrying
//      "name" (see setOwnerName below). This is how the fleet is
//      personalized: all 14 boards flash from one identical image and are
//      named afterwards, over the wire.
//   2. -DUNIT_NAME, a build-time DEFAULT for anyone who does want one baked
//      in. It is never written to NVS, so a stored name always wins and
//      clearing the stored name falls back to it rather than to nothing.
//   3. no name at all: the screens show the product name and the unit id.
//
//   PLATFORMIO_BUILD_FLAGS='-DUNIT_ID=3 -DUNIT_NAME="\"Alex\""' pio run
//
// The quotes are part of the macro body and PlatformIO shlex-splits the
// environment variable, which is why the inner pair is backslash-escaped.
// Unset (or set to an empty string) is the normal configuration and not an
// error: tools/flash-all.sh does not set it at all.
#ifndef UNIT_NAME
#define UNIT_NAME ""
#endif

static const char kUnitName[] = UNIT_NAME;

// Persisted owner name, mirrored in RAM. Empty means "nothing stored", which
// is not the same as "stored as an empty string": clearing removes the key.
//
// 24 characters, measured rather than guessed. On this board, with this
// firmware, LovyanGFX Font2 at textSize 1 reports fontHeight 16 and a widest
// printable-ASCII advance of 10px (the glyph is 'M'; every ASCII code 32..126
// was measured with textWidth on the real panel, not looked up). The ambient
// headline is centred and rides the burn-in drift, whose horizontal amplitude
// is AMB_DRIFT_AX = 9px, so the width that can never touch an edge is
// 320 - 2*9 = 302px, i.e. 30 characters of the worst-case glyph. 24 is that
// ceiling minus a deliberate margin: 24 x 10 = 240px worst case, which leaves
// 31px of clear panel on each side at the drift extreme. For reference, the
// same probe measured "Alexandra Rivera-Smith" (22 chars) at 143px.
static constexpr size_t OWNER_NAME_MAX = 24;          // characters
static constexpr size_t OWNER_NAME_CAP = OWNER_NAME_MAX + 1;
static constexpr const char* NVS_NAMESPACE = "codexbuddy";
static constexpr const char* NVS_KEY_OWNER = "owner";

static char gOwnerName[OWNER_NAME_CAP] = "";

// Stored name, then compile-time default, then nothing.
static const char* activeName() {
  if (gOwnerName[0] != '\0') return gOwnerName;
  if (kUnitName[0]  != '\0') return kUnitName;
  return nullptr;
}
static inline bool haveUnitName() { return activeName() != nullptr; }

static constexpr const char* PRODUCT_NAME = "codex companion";

// ---------------------------------------------------------------------------
// Geometry, landscape 320x170
//
// design-spec 2.1 put a single 52px-radius ring in the middle of the panel.
// The face takes that spot now, so live mode splits the top band in two: the
// eyes on the left, a smaller ring on the right carrying exactly the same
// metrics it always did. The two text lines keep their y datums and stay
// centred on the whole panel, under both. Ambient mode has no ring at all and
// centres a larger face instead. See firmware/EYES.md part 4.
// ---------------------------------------------------------------------------
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 170;
// Ring, live mode only. Was (160, 68) r 42..52; moved right and shrunk by 6px
// of radius to clear the face. Every renderLive stroke-width expression is
// relative to RING_R_OUT, so they all followed the shrink unchanged.
static constexpr int RING_CX  = 244;
static constexpr int RING_CY  = 60;
static constexpr int RING_R_OUT = 46;
static constexpr int RING_R_IN  = 37;
static constexpr int LABEL_Y  = 122;   // datum: top-centre
static constexpr int SUB_Y    = 146;   // datum: top-centre

// Face. One eye is a rounded rect EYE_W x EYE_H; the pair straddles FACE_CX
// with EYE_GAP of black between them, so the pair is 2*EYE_W + EYE_GAP wide.
//
// Ambient: 2*56 + 36 = 148px wide, x 86..234, y 30..94 at rest. The label line
// starts at y=122, so a fully open eye clears it by 28px and the widest
// expression (waiting, 1.25x height) still clears it by 20px.
static constexpr int FACE_CX_AMB  = 160;
static constexpr int FACE_CY_AMB  = 62;
static constexpr int EYE_W_AMB    = 56;
static constexpr int EYE_H_AMB    = 64;
static constexpr int EYE_GAP_AMB  = 36;
// Live: 2*40 + 26 = 106px wide, x 43..149. The ring's left edge is at
// 244 - 46 = 198, so there are 49px of black between the face and the ring.
static constexpr int FACE_CX_LIVE = 96;
static constexpr int FACE_CY_LIVE = 60;
static constexpr int EYE_W_LIVE   = 40;
static constexpr int EYE_H_LIVE   = 48;
static constexpr int EYE_GAP_LIVE = 26;

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
static constexpr uint32_t AMB_BREATHE_MS    = 6500;    // slow brightness breathe
static constexpr uint32_t AMB_HUE_MS        = 45000;   // colour drift, 3 stops
// Burn-in drift. Two mutually prime-ish periods so the composition never
// retraces the same path, and an amplitude big enough to smear the text edges
// across several pixels over a few minutes.
static constexpr uint32_t AMB_DRIFT_X_MS    = 97000;
static constexpr uint32_t AMB_DRIFT_Y_MS    = 61000;
static constexpr float    AMB_DRIFT_AX      = 9.0f;
static constexpr float    AMB_DRIFT_AY      = 5.0f;
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

// Blink. A human blink is asymmetric: the lid drops far faster than it lifts.
// Equal ramps read as a mechanical shutter, so the close is 70ms and the open
// 150ms, with a short shut hold between them. At FRAME_MS = 33 the close gets
// only ~2 frames, which is the point: it should be almost too fast to see.
static constexpr uint32_t BLINK_CLOSE_MS  = 70;
static constexpr uint32_t BLINK_SHUT_MS   = 34;
static constexpr uint32_t BLINK_OPEN_MS   = 150;
// Gap between the two halves of a double blink, measured from the moment the
// first one finishes opening.
static constexpr uint32_t BLINK_REPEAT_MS = 110;
// Chance in 100 that a blink is a double, and (within that) that it is a
// triple. A metronome blink is the thing that reads as broken, so the interval
// below is randomised per blink and the count is randomised too.
static constexpr uint32_t BLINK_DOUBLE_PCT = 24;
static constexpr uint32_t BLINK_TRIPLE_PCT = 7;
// Per-state interval windows, milliseconds between bursts. Resting is 2.6-6.4s
// (a relaxed human is 3-5s); busy blinks less because it is concentrating;
// waiting blinks more because it is agitated.
static constexpr uint32_t BLINK_GAP_MIN      = 2600;
static constexpr uint32_t BLINK_GAP_MAX      = 6400;
static constexpr uint32_t BLINK_GAP_BUSY_MIN = 4200;
static constexpr uint32_t BLINK_GAP_BUSY_MAX = 9000;
static constexpr uint32_t BLINK_GAP_WAIT_MIN = 1300;
static constexpr uint32_t BLINK_GAP_WAIT_MAX = 2800;

// Glance: an occasional look away and back, ambient and idle only.
static constexpr uint32_t GLANCE_GAP_MIN  = 4500;
static constexpr uint32_t GLANCE_GAP_MAX  = 13000;
static constexpr uint32_t GLANCE_HOLD_MIN = 700;
static constexpr uint32_t GLANCE_HOLD_MAX = 1600;
static constexpr float    GLANCE_AX       = 7.0f;    // px, horizontal
static constexpr float    GLANCE_AY       = 3.5f;    // px, vertical
static constexpr float    GLANCE_EASE_MS  = 110.0f;  // smoothing time constant

// How long "done" holds its happy squint before the eyes open again.
static constexpr uint32_t DONE_HAPPY_MS   = 1600;

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

// Face. The blink runs as an explicit phase machine rather than a function of
// the wall clock, because every interval is randomised: a modulo would give
// exactly the metronome this is trying to avoid.
enum BlinkPhase : uint8_t {
  BLINK_REST = 0,   // eyes open, waiting for blinkNextMs
  BLINK_CLOSING,
  BLINK_SHUT,
  BLINK_OPENING,
};
static BlinkPhase blinkPhase   = BLINK_REST;
static uint32_t   blinkPhaseMs = 0;   // when the current phase started
static uint32_t   blinkNextMs  = 0;   // when the next burst fires
static uint8_t    blinkLeft    = 0;   // blinks still owed in this burst

// Glance: current offset in px and where it is easing toward.
static float      glanceX = 0.0f, glanceY = 0.0f;
static float      glanceTX = 0.0f, glanceTY = 0.0f;
static uint32_t   glanceNextMs   = 0;  // when to look away
static uint32_t   glanceReturnMs = 0;  // when to look back, 0 = not looking
static bool       doneHappyOver  = false;  // "done" squint has already opened

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

// Inclusive uniform integer in [lo, hi]. esp_random() is the hardware RNG; it
// needs no seeding and, unlike random(), gives every board a different blink
// rhythm from the first frame. Fourteen units on one table blinking in lockstep
// would be the single most robot-like thing this device could do.
static uint32_t randRange(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return lo;
  return lo + (uint32_t)(esp_random() % (hi - lo + 1u));
}

static void copyClamped(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  if (src == nullptr) { dst[0] = '\0'; return; }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

// ---------------------------------------------------------------------------
// Owner name in NVS
// ---------------------------------------------------------------------------

// Read the stored name at boot. A read-only open fails when the namespace has
// never been written, which is the state of every freshly flashed board: that
// is the normal no-name path, not an error.
static void loadOwnerName() {
  gOwnerName[0] = '\0';
  // A board that has never been named has no "codexbuddy" namespace and no
  // "owner" key, and the Arduino Preferences wrapper reports both misses with
  // log_e. On this target that log shares the USB CDC the protocol uses, so an
  // unnamed board would otherwise greet its host with two stray
  // "[E][Preferences.cpp:...] nvs_... NOT_FOUND" lines every boot. Those calls
  // are compiled out by -DCORE_DEBUG_LEVEL=0 in platformio.ini; note that
  // esp_log_level_set() does NOT reach them, because at this log level the
  // Arduino HAL macros call log_printf directly and never go through an
  // esp_log tag (cores/esp32/esp32-hal-log.h:159).
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  char buf[OWNER_NAME_CAP];
  buf[0] = '\0';
  prefs.getString(NVS_KEY_OWNER, buf, sizeof(buf));
  prefs.end();
  copyClamped(gOwnerName, sizeof(gOwnerName), buf);
}

// Store a name, clamped to OWNER_NAME_MAX characters. An empty string removes
// the key so the compile-time default (or the unnamed fallback) comes back.
//
// Idempotent on purpose: NVS lives in the same flash the firmware does and a
// host that resends its whole frame every 2000ms would otherwise rewrite the
// key 43,200 times a day. The RAM mirror is exactly what is on flash, so
// comparing against it is the same test as reading the key back.
//
// Returns true if anything changed.
static bool setOwnerName(const char* v) {
  char next[OWNER_NAME_CAP];
  copyClamped(next, sizeof(next), v);
  if (strcmp(next, gOwnerName) == 0) return false;   // no write, no wear

  copyClamped(gOwnerName, sizeof(gOwnerName), next);

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    // The name is live on screen for this power cycle but will not survive a
    // reboot. Say so rather than pretending it stuck.
    Serial.println("err: nvs open failed, name not persisted");
    return true;
  }
  if (next[0] != '\0') prefs.putString(NVS_KEY_OWNER, next);
  else                 prefs.remove(NVS_KEY_OWNER);
  prefs.end();
  return true;
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

// "UNIT 03", or "UNIT AB12" when UNIT_ID was never injected.
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
  snprintf(out, cap, "UNIT %02X%02X", (unsigned)mac[4], (unsigned)mac[5]);
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

  // "name" outlives the frame: it is written to NVS and reloaded on the next
  // boot. "" clears the stored name and falls back to UNIT_NAME, then to the
  // unnamed screens. setOwnerName only touches flash when the value actually
  // differs from what is already stored.
  v = doc["name"];
  if (!v.isNull() && v.is<const char*>()) {
    setOwnerName(v.as<const char*>());
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

// ---------------------------------------------------------------------------
// The face
//
// Two eyes, drawn entirely from primitives: a rounded rect per eye, one
// smaller rounded rect for the highlight, an arc for the happy squint, a wide
// line per eyebrow. Nothing here is a bitmap, so the whole face scales between
// the two modes by changing four numbers and recolours by changing one.
//
// Everything the face does is one of five channels: how open the lids are
// (blink and expression), how wide the eye is, where the pair is looking,
// whether the whole face has hopped, and what colour it is. Per-state
// expressions are just different settings of those five, resolved in
// renderLive and renderAmbient. See firmware/EYES.md.
// ---------------------------------------------------------------------------
struct Face {
  int      cx, cy;         // centre of the pair, before the burn-in drift
  int      eyeW, eyeH;     // one eye at full open
  int      gap;            // black between the two eyes
  float    lid;            // 0 shut, 1 normal, >1 wide
  float    widthK;         // horizontal squash or stretch
  float    gazeX, gazeY;   // px, the eyes move, the brows do not
  float    bounceY;        // px, the whole head, negative is up
  uint16_t color;
  bool     happy;          // upward arcs instead of rounded rects
  bool     brows;
  float    browTilt;       // 0 level and raised, 1 dropped toward the nose
};

static void drawOneEye(float ex, float ey, float w, float h, uint16_t color,
                       bool gloss) {
  int iw = (int)lroundf(w);
  int ih = (int)lroundf(h);
  if (iw < 3) iw = 3;
  // A shut eye is a 3px lid line, never nothing. A vanished eye reads as a
  // rendering fault; a line reads as a blink.
  if (ih < 3) ih = 3;
  int r = (int)(((iw < ih) ? iw : ih) * 0.42f);
  if (r < 1) r = 1;
  int x0 = (int)lroundf(ex) - iw / 2;
  int y0 = (int)lroundf(ey) - ih / 2;
  gfx->fillSmoothRoundRect(x0, y0, iw, ih, r, color);

  // One highlight near the top left of each eye. It is what turns two rounded
  // rects into something that looks wet, and it costs one more fill. Dropped
  // once the lid is low enough that it would collide with the eye's own edge.
  if (gloss && ih > 18 && iw > 16) {
    int gw = (int)(iw * 0.26f);
    int gh = (int)(ih * 0.20f);
    if (gw >= 3 && gh >= 3) {
      gfx->fillSmoothRoundRect(x0 + (int)(iw * 0.17f), y0 + (int)(ih * 0.16f),
                               gw, gh, gh / 2, mixColor(color, C_TEXT, 0.55f));
    }
  }
}

// The happy squint. fillArc angles are degrees with 0 at 3 o'clock increasing
// clockwise, so 202..338 is the top of a circle: put that circle's centre
// below the eye and the visible piece is an upward bow.
//
// The chord across +-68 degrees of vertical is 2*r*sin(68) = 1.854*r, so
// r = w/1.854 makes the bow exactly as wide as the open eye it replaces, and
// offsetting the centre by 0.6875*r puts the bow's own vertical middle back on
// the eye's centre line instead of hanging below it.
static void drawHappyEye(float ex, float ey, float w, uint16_t color) {
  float rr = w / 1.854f;
  if (rr < 5.0f) rr = 5.0f;
  float t = rr * 0.30f;
  if (t < 3.0f) t = 3.0f;
  float ay = ey + rr * 0.6875f;
  gfx->fillArc((int)lroundf(ex), (int)lroundf(ay), (int)lroundf(rr - t),
               (int)lroundf(rr), 202.0f, 338.0f, color);
}

// One eyebrow. `side` is -1 for the left eye and +1 for the right, so the
// tilted end is always the one nearest the nose: level reads as alert, dropped
// inward reads as "I am still waiting". Brows sit off the nominal eye height,
// not the animated one, so a blink does not drag them down onto the eye.
static void drawBrow(float ex, float ey, float w, float nomH, int side,
                     float tilt, uint16_t color) {
  float halfW = w * 0.46f;
  float base  = ey - nomH * 0.86f;
  float inner = base + 12.0f * clampf(tilt, 0.0f, 1.0f);
  float y0 = base, y1 = base;
  if (side < 0) y1 = inner;
  else          y0 = inner;
  gfx->drawWideLine((int)lroundf(ex - halfW), (int)lroundf(y0),
                    (int)lroundf(ex + halfW), (int)lroundf(y1), 2.6f, color);
}

static void drawFace(const Face& f) {
  float w = (float)f.eyeW * f.widthK;
  float h = (float)f.eyeH * f.lid;
  float dx = (float)(f.eyeW + f.gap) * 0.5f;

  float headX = (float)(f.cx + gOx);
  float headY = (float)(f.cy + gOy) + f.bounceY;
  float eyeX  = headX + f.gazeX;
  float eyeY  = headY + f.gazeY;

  uint16_t col = xf(f.color);

  for (int i = 0; i < 2; ++i) {
    float side = (i == 0) ? -1.0f : 1.0f;
    if (f.happy) drawHappyEye(eyeX + side * dx, eyeY, w, col);
    else drawOneEye(eyeX + side * dx, eyeY, w, h, col, f.lid > 0.55f);
    if (f.brows) {
      drawBrow(headX + side * dx, headY, w, (float)f.eyeH, (int)side,
               f.browTilt, col);
    }
  }
}

// Advance the blink machine and return the lid factor, 0 shut to 1 open.
// Called exactly once per frame. `gapLo`/`gapHi` are the current state's
// interval window; passing `allowed` false parks the machine open and rearms
// it, which is how sleep (already shut) and done (squinting) opt out.
static float updateBlink(uint32_t nowMs, uint32_t gapLo, uint32_t gapHi,
                         bool allowed) {
  if (!allowed) {
    blinkPhase  = BLINK_REST;
    blinkLeft   = 0;
    blinkNextMs = nowMs + randRange(gapLo, gapHi);
    return 1.0f;
  }

  switch (blinkPhase) {
    case BLINK_REST:
      if ((int32_t)(nowMs - blinkNextMs) >= 0) {
        // Only roll a new burst length when the previous one is spent, or a
        // double blink would re-roll itself into an unbounded flutter.
        if (blinkLeft == 0) {
          uint32_t roll = esp_random() % 100u;
          blinkLeft = (roll < BLINK_TRIPLE_PCT)   ? 3
                      : (roll < BLINK_DOUBLE_PCT) ? 2
                                                  : 1;
        }
        blinkPhase   = BLINK_CLOSING;
        blinkPhaseMs = nowMs;
      }
      return 1.0f;

    case BLINK_CLOSING: {
      float k = (float)(nowMs - blinkPhaseMs) / (float)BLINK_CLOSE_MS;
      if (k >= 1.0f) {
        blinkPhase   = BLINK_SHUT;
        blinkPhaseMs = nowMs;
        return 0.0f;
      }
      return 1.0f - k * k;          // accelerating: the lid drops
    }

    case BLINK_SHUT:
      if (nowMs - blinkPhaseMs >= BLINK_SHUT_MS) {
        blinkPhase   = BLINK_OPENING;
        blinkPhaseMs = nowMs;
      }
      return 0.0f;

    case BLINK_OPENING: {
      float k = (float)(nowMs - blinkPhaseMs) / (float)BLINK_OPEN_MS;
      if (k >= 1.0f) {
        if (blinkLeft > 0) --blinkLeft;
        blinkPhase  = BLINK_REST;
        blinkNextMs = (blinkLeft > 0) ? (nowMs + BLINK_REPEAT_MS)
                                      : (nowMs + randRange(gapLo, gapHi));
        return 1.0f;
      }
      return sinf(k * (float)M_PI * 0.5f);   // decelerating: the lid lifts
    }
  }
  return 1.0f;
}

// Look away, hold, look back. Ambient and idle only: a face that glances
// around while it is meant to be concentrating or demanding an answer reads as
// distracted rather than alive.
static void updateGlance(uint32_t nowMs, uint32_t dtMs, bool allowed) {
  if (!allowed) {
    glanceTX = glanceTY = 0.0f;
    glanceReturnMs = 0;
    glanceNextMs   = nowMs + randRange(GLANCE_GAP_MIN, GLANCE_GAP_MAX);
  } else if (glanceReturnMs != 0) {
    if ((int32_t)(nowMs - glanceReturnMs) >= 0) {
      glanceTX = glanceTY = 0.0f;
      glanceReturnMs = 0;
      glanceNextMs   = nowMs + randRange(GLANCE_GAP_MIN, GLANCE_GAP_MAX);
    }
  } else if ((int32_t)(nowMs - glanceNextMs) >= 0) {
    // Never a zero move: the direction is drawn first and the magnitude never
    // reaches zero, so a scheduled glance always visibly goes somewhere.
    float dir = (esp_random() & 1u) ? 1.0f : -1.0f;
    float mag = 0.55f + (float)(esp_random() % 46u) / 100.0f;   // 0.55..1.00
    glanceTX = GLANCE_AX * mag * dir;
    glanceTY = GLANCE_AY * ((float)(esp_random() % 3u) - 1.0f) * 0.7f;
    glanceReturnMs = nowMs + randRange(GLANCE_HOLD_MIN, GLANCE_HOLD_MAX);
  }

  // Frame-rate independent smoothing, so the eyes slide rather than snap.
  float k = (float)dtMs / GLANCE_EASE_MS;
  if (k > 1.0f) k = 1.0f;
  glanceX += (glanceTX - glanceX) * k;
  glanceY += (glanceTY - glanceY) * k;
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
    // Centred on the panel, not on the ring: the ring moved right to make room
    // for the face, and the two text lines still belong to the whole screen.
    gfx->drawString(st.label, SCREEN_W / 2 + gOx, LABEL_Y + gOy);
  }
  if (st.sub[0] != '\0') {
    gfx->setTextColor(scaleColor(C_TEXT_DIM, gXfade), C_BG);
    gfx->setFont(&fonts::Font0);
    gfx->setTextSize(1);
    gfx->drawString(st.sub, SCREEN_W / 2 + gOx, SUB_Y + gOy);
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

  // The face, filled in by the state below and drawn once at the end. Defaults
  // are the idle look: open, level, looking straight ahead.
  Face f;
  f.cx      = FACE_CX_LIVE;
  f.cy      = FACE_CY_LIVE;
  f.eyeW    = EYE_W_LIVE;
  f.eyeH    = EYE_H_LIVE;
  f.gap     = EYE_GAP_LIVE;
  f.lid     = 1.0f;
  f.widthK  = 1.0f;
  f.gazeX   = 0.0f;
  f.gazeY   = 0.0f;
  f.bounceY = 0.0f;
  f.color   = C_IDLE;
  f.happy   = false;
  f.brows   = false;
  f.browTilt = 0.0f;

  uint32_t gapLo = BLINK_GAP_MIN, gapHi = BLINK_GAP_MAX;
  bool blinkOK  = true;
  bool glanceOK = false;
  float lidScale = 1.0f;      // expression, multiplied by the blink factor

  switch (eff) {
    case ST_SLEEP: {
      // Dim, slow full-ring breathe: 15% -> 45% -> 15%.
      float b = breathe(nowMs, SLEEP_PERIOD_MS);
      level  = 0.15f + 0.30f * b;
      accent = C_SLEEP;
      drawRing(1.0f, scaleColor(accent, level), RING_R_IN, RING_R_OUT,
               scaleColor(C_TRACK, 0.4f));
      // Shut, and breathing. The lid line rides 3px up and down on the same 4s
      // sine as the ring and the eye narrows slightly at the bottom of it.
      // That is the whole animation: a sleeping thing should be almost still,
      // but a genuinely frozen panel is indistinguishable from a crashed one.
      blinkOK  = false;
      lidScale = 0.0f;
      f.widthK = 0.90f + 0.06f * b;
      f.gazeY  = -1.6f + 3.2f * b;
      // C_SLEEP is #26314A, which at the sleep backlight tier is very close to
      // invisible. The lid is lifted toward the text colour so a closed eye
      // still reads as a closed eye rather than as a blank screen.
      f.color  = scaleColor(mixColor(C_SLEEP, C_TEXT, 0.42f),
                            0.45f + 0.35f * b);
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
      // Open, blinking at the resting rate, and looking around now and then.
      f.color  = scaleColor(C_IDLE, 0.80f + 0.20f * b);
      glanceOK = true;
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
      // Concentrating: lids down to a squint, and the gaze tracks back and
      // forth across something only it can see. The two scan periods do not
      // divide into each other, so the eyes never retrace one fixed path and
      // the motion does not read as a mechanism.
      lidScale = 0.60f;
      f.gazeX  = 6.5f * sinf((float)(nowMs % 1700u) / 1700.0f * 2.0f *
                             (float)M_PI);
      f.gazeY  = 2.0f * sinf((float)(nowMs % 2600u) / 2600.0f * 2.0f *
                             (float)M_PI);
      f.color  = scaleColor(C_BUSY, 0.70f + 0.30f * b);
      gapLo    = BLINK_GAP_BUSY_MIN;
      gapHi    = BLINK_GAP_BUSY_MAX;
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
        // Still wide, because a prompt is still pending, but level-browed and
        // steady. This is the face of something that has been told you know.
        lidScale = 1.10f;
        f.color  = scaleColor(C_WAIT, 0.78f);
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

      // The hero state. Everything here is aimed at one job: reading as
      // "hey, you" from across a room, where the text is illegible and the
      // ring is a small amber dot.
      //   - eyes wide, wider still once escalated
      //   - eyebrows up, and dropped toward the nose once escalated
      //   - the whole head hops a few px on every pulse peak, which is the
      //     part that catches peripheral vision, since motion does and a
      //     brightness change on a 40px object mostly does not
      //   - blinking twice as often as at rest, which reads as agitation
      // The gaze stays locked forward: a face that looks away while demanding
      // an answer stops demanding it.
      lidScale   = escalated ? 1.28f : 1.18f;
      f.widthK   = escalated ? 1.06f : 1.00f;
      f.bounceY  = -(escalated ? 5.0f : 3.0f) * b;
      f.color    = scaleColor(accent, 0.45f + 0.55f * b);
      f.brows    = true;
      f.browTilt = escalated ? 1.0f : 0.0f;
      gapLo      = BLINK_GAP_WAIT_MIN;
      gapHi      = BLINK_GAP_WAIT_MAX;
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

      // The face holds a happy squint for a full DONE_HAPPY_MS, well past the
      // 600ms ring flash, because a smile that lasts exactly as long as a
      // flash does not register as a smile. It hops once on arrival.
      if (since < DONE_HAPPY_MS) {
        blinkOK = false;
        f.happy = true;
        f.color = C_DONE;
        if (since < 300) {
          f.bounceY = -4.0f * (1.0f - (float)since / 300.0f);
        }
        doneHappyOver = false;
      } else if (!doneHappyOver) {
        // Coming out of the squint. Handing the machine a half-finished open
        // makes the eyes lift out of the smile instead of cutting to open,
        // which is exactly what a face does after it stops grinning.
        doneHappyOver = true;
        blinkPhase    = BLINK_OPENING;
        blinkPhaseMs  = nowMs;
        blinkLeft     = 0;
      }
      break;
    }
  }

  // One blink update and one glance update per frame, after the state has
  // chosen its windows. The expression's own lid factor multiplies the blink's
  // rather than replacing it, so a squinting busy face still blinks.
  float blinkLid = updateBlink(nowMs, gapLo, gapHi, blinkOK);
  updateGlance(nowMs, dtMs, glanceOK);
  f.lid    = lidScale * blinkLid;
  f.gazeX += glanceX;
  f.gazeY += glanceY;
  drawFace(f);

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

static void renderAmbient(uint32_t nowMs, uint32_t dtMs) {
  uint16_t accent = ambientAccent(nowMs);
  float    level  = 0.55f + 0.40f * breathe(nowMs, AMB_BREATHE_MS);

  // The comet ring used to live here. It is gone from ambient: the face is
  // 148px wide and the ring was 104px across in the same place, and shrinking
  // either one to fit made both worse. The ring is not lost, it just belongs
  // to live mode now, where it carries real numbers. Everything the comet was
  // here to do (never a still frame, no error message, no dead-looking object)
  // the face does better, because the motion means something.
  Face f;
  f.cx      = FACE_CX_AMB;
  f.cy      = FACE_CY_AMB;
  f.eyeW    = EYE_W_AMB;
  f.eyeH    = EYE_H_AMB;
  f.gap     = EYE_GAP_AMB;
  f.widthK  = 1.0f;
  f.bounceY = 0.0f;
  f.happy   = false;
  f.brows   = false;
  f.browTilt = 0.0f;
  f.color   = scaleColor(accent, level);

  float blinkLid = updateBlink(nowMs, BLINK_GAP_MIN, BLINK_GAP_MAX, true);
  updateGlance(nowMs, dtMs, true);
  // A 5% height swell on the same 6.5s period as the brightness breathe. Two
  // eyes holding exactly one shape between blinks look painted on; this is
  // invisible as motion and is the whole difference between resting and inert.
  // It also keeps smearing the eyes' top and bottom edges, which is the half
  // of the burn-in defence the drift does not cover for a big solid shape.
  f.lid   = blinkLid * (0.97f + 0.05f * breathe(nowMs, AMB_BREATHE_MS));
  f.gazeX = glanceX;
  f.gazeY = glanceY;
  drawFace(f);

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
  const char* owner = activeName();
  gfx->drawString(owner ? owner : PRODUCT_NAME,
                  SCREEN_W / 2 + gOx, LABEL_Y + gOy);

  gfx->setTextColor(scaleColor(C_TEXT_DIM, gXfade), C_BG);
  gfx->setFont(&fonts::Font0);
  gfx->drawString(owner ? PRODUCT_NAME : unit,
                  SCREEN_W / 2 + gOx, SUB_Y + gOy);

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

#ifdef FPS_DEBUG
  uint32_t t0 = micros();
#endif

  gfx->fillScreen(C_BG);
  if (ambientMode) renderAmbient(nowMs, dtMs);
  else             renderLive(nowMs, dtMs);
  if (fbReady) fb.pushSprite(0, 0);

#ifdef FPS_DEBUG
  // Build with PLATFORMIO_BUILD_FLAGS=-DFPS_DEBUG to get a frame-cost line on
  // the same USB CDC the protocol uses. Never enabled on a fleet image: the
  // protocol channel has to stay clean. See firmware/EYES.md part 6.
  static uint32_t fpsWindowMs = 0, fpsFrames = 0, fpsSumUs = 0, fpsMaxUs = 0;
  uint32_t dus = micros() - t0;
  ++fpsFrames;
  fpsSumUs += dus;
  if (dus > fpsMaxUs) fpsMaxUs = dus;
  if (fpsWindowMs == 0) fpsWindowMs = nowMs;
  if (nowMs - fpsWindowMs >= 3000) {
    Serial.printf("fps: mode=%s frames=%lu fps=%.1f avg_ms=%.2f max_ms=%.2f\n",
                  ambientMode ? "ambient" : "live", (unsigned long)fpsFrames,
                  1000.0f * (float)fpsFrames / (float)(nowMs - fpsWindowMs),
                  (float)fpsSumUs / (float)fpsFrames / 1000.0f,
                  (float)fpsMaxUs / 1000.0f);
    fpsWindowMs = nowMs;
    fpsFrames = 0; fpsSumUs = 0; fpsMaxUs = 0;
  }
#endif
}

static void drawBootScreen() {
  gfx->fillScreen(C_BG);
  gfx->setTextDatum(textdatum_t::middle_center);
  gfx->setTextColor(C_BOOT, C_BG);
  gfx->setFont(&fonts::Font4);
  gfx->setTextSize(1);
  gfx->drawString(PRODUCT_NAME, SCREEN_W / 2, 70);

  // UNIT_ID is never injected on the plain `pio run -t upload` path, nor by
  // tools/flash-all.sh, which flashes every unit from the byte-identical
  // image. unitIdString() therefore renders "UNIT <MAC>" rather than a
  // "UNIT 00" that would read like a real serial number. See its comment for
  // why the suffix has to come from the tail of the MAC.
  char unit[24];
  unitIdString(unit, sizeof(unit));

  if (haveUnitName()) {
    // Named build: the owner's name is the second line and the unit id drops
    // to a third, smaller line. Both are still shown, so assembly-day
    // identification does not depend on remembering who got which name.
    gfx->setTextColor(C_TEXT, C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->drawString(activeName(), SCREEN_W / 2, 106);
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

  // Before the first pixel: the boot screen shows the owner name, so the
  // stored one has to be in RAM by now.
  loadOwnerName();

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

  // The greeting keeps its exact "hello tdisplay-s3 v1" prefix (the helper
  // matches /hello\s+tdisplay-s3/i and tools/flash-all.sh globs on the same
  // substring) and appends the name the device is actually running with, so a
  // host can read back what a "name" line did without a new command. Always
  // quoted, so no name is an unambiguous name="".
  const char* bootName = activeName();
  Serial.printf("hello tdisplay-s3 v1 name=\"%s\"\n", bootName ? bootName : "");

  lastDataMs      = millis();
  waitEnterMs     = millis();
  doneEnterMs     = millis();
  stateEnterMs    = millis();
  lastFrameMs     = millis();
  lastButtonLevel  = digitalRead(PIN_BUTTON_1) != 0;
  lastButton2Level = digitalRead(PIN_BUTTON_2) != 0;

  // Arm the face. Both schedules are randomised from the first frame, so two
  // units side by side on the same desk do not blink together.
  blinkPhase     = BLINK_REST;
  blinkLeft      = 0;
  blinkNextMs    = millis() + randRange(BLINK_GAP_MIN, BLINK_GAP_MAX);
  glanceNextMs   = millis() + randRange(GLANCE_GAP_MIN, GLANCE_GAP_MAX);
  glanceReturnMs = 0;

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
