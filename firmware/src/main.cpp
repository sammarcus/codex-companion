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
//   "hello tdisplay-s3 v1" once on boot, "ok" per accepted line, plus the
//   unsolicited "firstrun:" / "reset:" / "err:" notices described below.
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
// The thing on the panel is a face. WHICH face is a runtime choice: the
// drawing lives behind the small interface in src/Face.hpp, three designs
// implement it, and the active one is persisted in NVS next to the owner name
// and selected with the protocol's "face" field or by holding button 2. This
// file owns the blink and glance drivers, the state machine, the compositions
// and the text; it does not know how an eye is drawn. See firmware/FACES.md,
// and firmware/EYES.md for the default face's geometry.
//
// A long turn leaves the owner with nothing to do, so the two buttons carry a
// game: hold button 1 for two seconds and the active face runs along a line
// hopping over blocks, with the personal best kept in NVS. It ends the instant
// a prompt arrives. See firmware/README.md, "Something to do".
//
// The very first time an owner powers a board it plays an out-of-the-box
// sequence instead of settling straight into ambient: dark, a light comes up
// on a pair of shut eyes, they open, they find you, they blink, a hand comes
// up and waves "Hi OpenAI", and then it says whose it is. It plays exactly
// once, the fact that it played is persisted in
// NVS beside the name and the face, and re-flashing the same image does not
// bring it back. Two optional protocol fields go with it, "reset" and
// "firstrun", plus two button gestures. See firmware/FIRSTRUN.md.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <math.h>

// Brings in LGFX_TDisplayS3.hpp, StateId, the RGB565 colour helpers and the
// face registry.
#include "Face.hpp"

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
// The active face, stored by wire name alongside the owner name in the same
// namespace. A name rather than an index, so reordering the registry cannot
// silently repoint a board that has already been set.
static constexpr const char* NVS_KEY_FACE  = "face";
// Whether the out-of-the-box sequence has already played for this owner. One
// byte, in the same namespace as the name and the face, so a factory reset
// clears all three together.
//
// Absent or 0 means armed: the next power-on plays it. 1 means spent.
// ABSENCE IS THE ARMED STATE ON PURPOSE. A freshly flashed board has no
// namespace at all, so it is armed without anything ever having been written,
// and re-arming is a key removal rather than a value.
//
// This is what makes a reflash safe. `pio run -t upload` writes the
// bootloader (0x0), the partition table (0x8000), boot_app0 (0xe000) and the
// app (0x10000); the nvs partition is at 0x9000 and is never in that list, so
// re-flashing the identical image leaves a spent flag spent. Sam can power and
// re-flash a board as often as he likes without burning the one moment the
// recipient is supposed to get.
static constexpr const char* NVS_KEY_FIRSTRUN = "firstrun";

static char    gOwnerName[OWNER_NAME_CAP] = "";
static uint8_t gFaceIdx = 0;
static bool    gFirstRunArmed = true;

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
// The factory-reset warning, which blacks out the band below CHORD_HINT_TOP
// and owns it for as long as the fingers are down. Font0 is 8px tall, so the
// words sit at 130..138 and the bar at 150..156, both clear of each other.
//
// The top is LABEL_Y minus the ambient drift's vertical amplitude, not LABEL_Y
// itself. The warning does not drift (it is on the panel for three and a half
// seconds and it is a warning, not furniture) but the owner headline it covers
// does: at 124 a negative drift left the top rows of the recipient's own name
// hanging in fragments above a red FACTORY RESET bar for about a third of the
// 61 second drift cycle. The static_assert next to AMB_DRIFT_AY keeps the two
// numbers married.
static constexpr int CHORD_HINT_TOP = 117;
// The single-button holds, drawn along the very top edge: the one band no
// composition here occupies. The timer and stats headers start at y=8 and the
// ring's top is y=14.
//
// The third number here used to read "the face's box tops out at y=25 at the
// drift extreme", which was the `rounded` face's EYES, not the box every face
// is handed. The box is FACE_BOX_Y + gOy, and a face that draws its whole
// creature inside it (bear) reaches most of the way up: at FACE_BOX_Y = 2 the
// bear's ear tops landed on row 0 at the negative drift extreme with the body
// swell near its peak, x 99..137 and 182..223, squarely inside this bar's
// 70..250. FACE_BOX_Y is 6 now for that reason, which keeps the ear top at
// y=4.3 in the worst case and leaves this band genuinely free.
static constexpr int HOLD_HINT_X = 70;
static constexpr int HOLD_HINT_Y = 2;
static constexpr int HOLD_HINT_H = 2;
static constexpr int CHORD_HINT_Y   = 130;   // datum: top-centre
static constexpr int CHORD_BAR_X    = 70;
static constexpr int CHORD_BAR_Y    = 150;
static constexpr int CHORD_BAR_H    = 6;
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

// The rectangle handed to the face as `geom.box*`, for a face that is a whole
// creature rather than a pair of eyes (see Face.hpp's geometry contract).
// Nothing else is drawn inside it, so a face may use every pixel of it.
//
// Vertically it ends at 118: LABEL_Y is 122 with a top-centre datum, so that
// leaves 4px of clear panel under it.
//
// The top used to be 2, which put a face that draws a whole creature inside
// the box (bear) into the hold hint's band whenever the ambient drift was near
// its negative extreme. Numbers: at FACE_BOX_H = 116 the bear's scale is
// 116 * BODY_FRAC_AMB / REF_H = 0.851, its body swells to 104.9px on the
// breathe, its ears reach 9 * 0.851 = 7.7px above that, and at gOy = -5 the
// ear top edge landed on row 0 against a hint bar that owns rows 2 and 3. The
// box starts at 6 and is 4px shorter for that reason: the bottom does not
// move, the bear is 3.4% smaller in ambient, and the worst-case ear top is
// y=4.3. Nothing else uses the box (rounded and arc size their eyes from
// FACE_CY/EYE_H, and the toy hands its own rectangle), so this moves one face.
static constexpr int FACE_BOX_Y      = 6;
static constexpr int FACE_BOX_H      = 112;
static constexpr int FACE_BOX_X_AMB  = 0;
static constexpr int FACE_BOX_W_AMB  = SCREEN_W;
// Live: the ring's left edge is RING_CX - RING_R_OUT = 198, so the face owns
// everything left of it with a 6px gutter.
static constexpr int FACE_BOX_X_LIVE = 0;
static constexpr int FACE_BOX_W_LIVE = 192;

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
// Do-not-disturb violet. Like C_AMB_TEAL this is a new value rather than one
// borrowed from design-spec 2.3, and for a stronger reason: the sign must not
// be mistakable for a state at four metres, so it has to be a hue that no
// state owns. It is far from C_BUSY/C_WAIT amber and C_WAIT_HOT red, which are
// the two colours on this device that mean "look at me now", and it is the
// conventional away/busy colour in the chat clients these fourteen people
// stare at all day. Nominal #A56BFF; RGB565 rounds it to #A068F8.
static constexpr uint16_t C_DND      = 0xA35F;  // #A56BFF violet
// The focus timer, and deliberately not a new hue. A pomodoro is the one
// thing on this panel that is about the PERSON rather than about the agent,
// so it borrows the ambient teal, which is already the device's own colour
// and is owned by no state. Violet is spoken for by the sign, amber and red
// mean "answer me", and green means a turn finished: none of those may be
// spent on a countdown.
static constexpr uint16_t C_FOCUS    = C_AMB_TEAL;

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
// The factory-reset warning blacks out from a fixed row and the headline it
// covers rides this amplitude. Declared here because AMB_DRIFT_AY is not in
// scope up there.
static_assert(CHORD_HINT_TOP <= LABEL_Y - (int)AMB_DRIFT_AY,
              "chord hint must cover the owner headline at every drift phase");
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
// Switch bounce, and nothing else. This used to be 180ms, which is not a
// bounce figure: it is longer than the gap in a brisk double tap, and because
// it is measured from the last ACCEPTED press rather than from the last edge
// it swallowed the second tap of "1 twice" (the focus timer, on the card in
// the box) in complete silence. Every gesture on this device resolves on a
// release or on a threshold 800ms away or more, so nothing here needs a long
// floor: 40ms is a real switch-bounce number and buys back the double tap.
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
// How long a single-button hold has to be down before the panel starts saying
// so. Long enough that an ordinary tap never draws anything.
static constexpr uint32_t HOLD_HINT_MS       = 400;
// Hold button 2 this long to swap to the next face. Comfortably longer than a
// deliberate tap and comfortably shorter than the patience of someone holding
// a button waiting for something to happen.
static constexpr uint32_t FACE_HOLD_MS       = 800;
// Button 1's two second hold used to replay the first run. It opens the toy
// now, and the replay kept its two other doors: {"firstrun":"play"} over the
// wire, and the factory reset, which replays the sequence to confirm itself.
// The threshold did not move, so the device did not gain one: see
// PLAY_HOLD_MS below and firmware/README.md, "Something to do".
// Hold BOTH buttons this long, in ambient mode only, to wipe the board. Five
// seconds on two switches at opposite ends of the board is not something a
// hand does by accident, and it is the one gesture that destroys stored state.
static constexpr uint32_t FACTORY_HOLD_MS    = 5000;
// ...and the point at which the panel starts saying so. Every other gesture
// here is short enough to be discovered by doing it; this one destroys the
// owner's name, the face, the records and the hop best, and with no host
// software installed (design law 1) the recipient has no way to put the name
// back. A hold that shows nothing at 1s, nothing at 4s and wipes the board at
// 5s is the one place where the natural response to a gesture that seems not
// to have worked (hold it longer) lands on the irreversible action. So from
// 1500ms the panel draws a bar filling toward the wipe with the words under
// it, and letting go is the obvious way out.
static constexpr uint32_t CHORD_HINT_MS      = 1500;

// ---------------------------------------------------------------------------
// SAY: a short message pushed to the panel, meant to be read across a room.
//
// "brb", "in a meeting", "shipping". It is a sign, so it takes the whole
// screen rather than squeezing into the two text lines: the face and the ring
// step aside for as long as it is up and come back on their own.
//
// Two things it must never do, and both are enforced below rather than
// documented and hoped for:
//   - hide a pending question. While the effective state is `waiting` the card
//     yields the screen back, and takes it again when the prompt is answered.
//     Its clock keeps running while it is yielding, so it can never queue up
//     behind a long prompt and surprise somebody ten minutes later.
//   - get stuck. Every card is dismissable from the board itself, with no
//     host, by pressing either button.
// ---------------------------------------------------------------------------
// Characters. 48 fits "back in ten minutes, ping me if it is urgent" whole,
// and the layout below shrinks the type rather than refusing anything shorter.
static constexpr size_t   SAY_MAX        = 48;
static constexpr size_t   SAY_CAP        = SAY_MAX + 1;
// How long a card holds, in seconds, when the host does not say. Long enough
// to walk away from the desk and see it land, short enough that a forgotten
// message clears itself.
static constexpr uint32_t SAY_DEFAULT_S  = 30;
static constexpr uint32_t SAY_MAX_S      = 3600;   // an hour, then it clamps
static constexpr uint32_t SAY_IN_MS      = 220;    // fade and settle in
static constexpr uint32_t SAY_OUT_MS     = 260;    // fade out
// The card. A rounded frame inset from the panel edge, the message centred
// inside it, and a thin bar along the bottom that drains over the hold.
static constexpr int SAY_FRAME_X  = 12;
static constexpr int SAY_FRAME_Y  = 10;
static constexpr int SAY_FRAME_W  = 296;
static constexpr int SAY_FRAME_H  = 150;
static constexpr int SAY_FRAME_R  = 16;
// The text block: 14px of padding inside the frame, and an optical centre a
// little above the frame's own, because the timer bar owns the bottom.
static constexpr int SAY_TEXT_W   = SAY_FRAME_W - 28;   // 268
static constexpr int SAY_TEXT_H   = 118;
static constexpr int SAY_TEXT_CY  = 78;
static constexpr int SAY_BAR_Y    = 150;
static constexpr int SAY_BAR_H    = 4;
static constexpr int SAY_BAR_W    = 260;
// Lines the message may occupy, and the working room the wrapper gets before
// the ladder gives up and truncates.
static constexpr int SAY_MAX_LINES  = 3;
static constexpr int SAY_WRAP_MAX   = 8;

// ---------------------------------------------------------------------------
// DO NOT DISTURB: a sign for the room, not a status for Codex.
//
// This is the one screen on the device aimed at a person walking past rather
// than at the owner or at a session. It is deliberately not a state: nothing
// about the protocol, the five states, the staleness clocks or the face
// changes while it is up. The device carries on knowing exactly what it knew;
// it just puts a sign in front of itself.
//
// Three rules it has to obey, all enforced below rather than hoped for:
//   - it must not read as `waiting`. So: no amber, no red, no pulse fast
//     enough to catch an eye, no face at all. A violet frame and two words.
//   - it must never hide a pending question. It yields the screen to
//     `waiting` exactly as the message card does, and takes it back after.
//   - it must never trap anybody. Entry costs a two-button chord, exit costs
//     any single press. That asymmetry is the whole safety argument.
//
// It is RAM only and does NOT survive a power cycle, which is a decision and
// not an omission: see firmware/README.md, "Do not disturb".
// ---------------------------------------------------------------------------
// The words. Fixed, not a protocol string: this sign has exactly one meaning
// and a host that wants to say something else already has `say`.
static const char* DND_LINE1 = "DO NOT";
static const char* DND_LINE2 = "DISTURB";
// The frame. Inset far enough that the burn-in drift (+/-9, +/-5) can never
// push it off the panel, and thick enough to be the signal in its own right:
// the type on a 44mm-wide panel gives up before four metres, the coloured
// rectangle does not.
static constexpr int DND_FRAME_X = 12;
static constexpr int DND_FRAME_Y = 8;
static constexpr int DND_FRAME_W = 296;
static constexpr int DND_FRAME_H = 154;
static constexpr int DND_FRAME_R = 16;
static constexpr int DND_FRAME_T = 8;     // stroke thickness
// Working width for the type, inside the stroke with 6px of air either side.
static constexpr int DND_TEXT_W  = DND_FRAME_W - 2 * (DND_FRAME_T + 6);   // 268
// Optical centre of the two-line block, with and without a name under it.
static constexpr int DND_TEXT_CY_NAMED = 68;
static constexpr int DND_TEXT_CY_BARE  = 84;
static constexpr int DND_NAME_Y        = 132;
static constexpr uint32_t DND_IN_MS  = 260;
static constexpr uint32_t DND_OUT_MS = 260;
// The chord. Both buttons down together for this long, released before the
// panel starts warning about the factory reset, toggles the sign.
//
// This was 250ms, which is not a tap: docs/card.md prints the gesture as
// "both" and the site as "Tap both", and everything under a quarter second was
// a completely silent no-op, on the wire as well as on the panel, because
// chordRelease marks both presses held before it returns. An eighth of a
// second of overlap is still a deliberate squeeze rather than a brush past two
// switches at opposite ends of the board: a hand sweeping across presses one
// and releases it reaching the other, and never holds both. A chord that comes
// in under it now says so on the cable instead of vanishing.
static constexpr uint32_t CHORD_MIN_MS = 120;

// ---------------------------------------------------------------------------
// THE TOY: "hop"
//
// A turn can run for ten minutes and the person in front of it has nothing to
// do, so the two buttons get a game. It is the only screen on this device that
// is played rather than read, and everything about it is bent around one rule:
// it must never be the reason somebody missed a question.
//
// The game is the Chrome dinosaur, and it is that on purpose. Nobody has to be
// told what a creature running at a block means, which is the whole test: five
// seconds, no instructions, no host, no menu.
//
// The runner IS the active face, drawn through the ordinary Face interface at
// a smaller geometry. The toy knows nothing about eyes, bears or strokes: it
// hands a box and a colour to whichever face the board is set to, exactly as
// the ambient and live compositions do, so all three play and a fourth would
// play the day it is written.
//
// Three rules, enforced below rather than hoped for:
//   - a pending question ends the game, instantly, on the frame it arrives.
//     Not covered and restored the way the message card is: after answering a
//     prompt somebody is back at their keyboard, and a game reappearing over
//     the answer would be the same failure in a different costume.
//   - it pauses whenever it is not the thing on the panel, so a message card
//     arriving mid-run cannot cost a score nobody was watching.
//   - it closes itself. Thirty seconds with nobody pressing anything and the
//     board goes back to being an object on a desk, because design law 1 says
//     ambient is what most owners see most of the time.
// ---------------------------------------------------------------------------
// The personal best, in NVS beside the owner name and the face. This is the
// whole reason to come back to it, so it has to outlive the power cycle. It is
// written ONLY when a run beats it, which is at most once per game and usually
// far less: nothing here writes flash on a timer.
static constexpr const char* NVS_KEY_HOPBEST = "hopbest";

// The ground line, and the runner standing on it. The line sits above the
// legend band so the two never touch.
static constexpr int   TOY_GROUND_Y = 128;
static constexpr int   TOY_RUN_X    = 66;    // runner centre, x
static constexpr int   TOY_BOX_W    = 46;    // the box the face is handed
static constexpr int   TOY_BOX_H    = 40;
// The eye-pair half of the geometry contract, sized so 2*eyeW + gap is exactly
// TOY_BOX_W: a face built from eyes fills the same footprint as one built from
// a box, and neither has to know which the other chose.
static constexpr int   TOY_EYE_W    = 18;
static constexpr int   TOY_EYE_H    = 22;
static constexpr int   TOY_EYE_GAP  = 10;
// Where the eye pair's centre line sits. Low enough that a short obstacle
// overlaps the eyes rather than sliding under them, which is what keeps the
// hit test and the picture agreeing for a face that does not touch the floor.
static constexpr int   TOY_EYE_CY   = TOY_GROUND_Y - 17;

// Physics, in pixels and seconds. Apex is v*v/2g = 67px and the airtime is
// 2v/g = 0.60s.
//
// THESE ARE NOT FREE NUMBERS, and the first set chosen here made the game
// unwinnable on the bench, which is the whole reason a bot played it over the
// cable before anybody was asked to. An obstacle is inside the hit box for
// (2*TOY_HIT_HALF + w) / speed seconds, and the jump is above it for the time
// the parabola spends over h + TOY_HIT_CLR. THE SECOND HAS TO BEAT THE FIRST
// AT THE SLOWEST SPEED, because a slower obstacle sits in the hit box longer:
// getting faster makes this game easier to clear and harder to react to, which
// is exactly the classic's difficulty curve and the opposite of the intuition.
//
//   worst tall block  h 21, w 12: 0.482s of clearance vs 32px / 115 = 0.278s
//   worst wide block  h 14, w 17: 0.519s of clearance vs 37px / 115 = 0.322s
//
// so the margin is about 1.6x at the start and grows from there. The first set
// had a 30px hit box against a 0.41s clearance and lost that race at every
// speed: a bot that hopped on the exact frame still died on the first block.
static constexpr float TOY_GRAVITY  = 1500.0f;
static constexpr float TOY_JUMP_V   = 450.0f;
static constexpr float TOY_SPEED0   = 115.0f;   // px/s at the first obstacle
static constexpr float TOY_SPEED_UP = 4.5f;     // px/s added per point
static constexpr float TOY_SPEED_MAX = 200.0f;  // px/s, and it stays playable
// At the cap, 200px of gap is 1.0s and the jump owns 0.6s of that, which
// leaves four tenths of a second to see the next one coming. Any tighter and
// the game stops being a thing you can pick up and put down.
static constexpr float TOY_GAP_MIN  = 200.0f;
static constexpr float TOY_GAP_MAX  = 330.0f;
static constexpr float TOY_FIRST_GAP = 80.0f;   // grace before the first one
static constexpr int   TOY_OBS_MAX  = 3;        // live obstacles at once
// The hit box. Less than half the width of the drawn creature on purpose: a
// game that feels unfair in the first thirty seconds does not get a second
// play, and this is the number the clearance arithmetic above is most
// sensitive to.
static constexpr int   TOY_HIT_HALF = 10;       // px either side of TOY_RUN_X
static constexpr int   TOY_HIT_CLR  = 3;        // px of daylight that counts
// Nobody pressing anything for this long and the board goes back to ambient.
static constexpr uint32_t TOY_IDLE_MS = 30000;
// Long enough to see what happened before a press can restart it, short enough
// that it never feels like the board is thinking about it.
static constexpr uint32_t TOY_OVER_MS = 450;
// Hold button 1 this long to open the toy. It is deliberately the SAME 2000ms
// that used to replay the first run, so the device gains a mode and does not
// gain a threshold. See firmware/README.md, "Something to do".
static constexpr uint32_t PLAY_HOLD_MS = 2000;

// ---------------------------------------------------------------------------
// STATS: the numbers, and MILESTONES: the two moments worth marking
//
// The device already watches every turn go past. Nothing was counting them,
// which is the one thing an engineer would actually want from an object that
// sits on the desk all day watching them work.
//
// WHAT IS COUNTED HERE, AND WHAT IS TOLD
//   turns and the length of each one   counted here, from the state machine
//   the current session's length       counted here, from host contact
//   tokens                             TOLD, by the optional "tokens" field
//   what day it is                     TOLD, by the optional "time" field
//
// Turns and elapsed time are the device's own arithmetic because it already
// sees every edge it needs. Tokens are not: the wire carries `tps` and two
// strings, and integrating a rate sampled at hook events would be a number
// nobody could reconcile with what Codex tells them. So the host sends the
// session total and the device banks the differences.
//
// TWO PROTOCOL FIELDS, BOTH OPTIONAL, AND AN OLDER HOST STILL WORKS
//   "time"    local wall clock, seconds. Without it there is no midnight, so
//             the daily figures simply run from boot and the screen says so.
//   "tokens"  the current Codex session's cumulative total. Without it the
//             token figures stay at zero and everything else still counts.
//
// WHAT IS PERSISTED, AND WHAT DELIBERATELY IS NOT
//   Records only: the longest single turn ever, and the most tokens in one
//   day. Those are the two numbers somebody would be annoyed to lose, they
//   change a handful of times a week, and the flash write happens only when
//   one of them actually moves.
//
//   The DAILY figures are RAM. A reboot is not a midnight, and the device
//   cannot know whether the counters it loaded belong to today or to a
//   Tuesday three weeks ago: showing yesterday's number under the word TODAY
//   is worse than showing a zero. It also keeps this feature's flash traffic
//   to record-breaking events, which is the same discipline the toy's best
//   score already follows.
// ---------------------------------------------------------------------------
// Records, in NVS beside the owner name, the face and the hop best. Both are
// uint32: no plausible turn is 136 years long and no plausible day is 4.29
// billion tokens.
static constexpr const char* NVS_KEY_BESTTURN = "bestturn";   // seconds
static constexpr const char* NVS_KEY_BESTDAY  = "bestday";    // tokens

// A "time" value below this is not a wall clock, it is a host bug (a
// milliseconds-instead-of-seconds mistake lands far above it, a zero or a
// small counter far below). 2020-01-01. Anything under it is ignored exactly
// as a wrong-typed field is, without making the line malformed.
static constexpr uint32_t CLOCK_MIN_EPOCH = 1577836800u;
static constexpr uint32_t DAY_SECONDS     = 86400u;
// ...and the other end of the same argument, which the floor above did not
// cover: a value far in the FUTURE passes CLOCK_MIN_EPOCH easily and is the
// destructive direction, because a forward day step is what zeroes the day.
// A board keeping its own clock between host lines rolls over one day at a
// time, so a step of more than this is a bad value rather than a midnight and
// is adopted rather than acted on. Two days, not one, so a host that corrects
// a clock that had been wrong by a day cannot lose a second one.
static constexpr int32_t  CLOCK_FORWARD_DAY_MAX = 2;

// The stats screen closes itself. Twenty seconds is long enough to read six
// numbers twice and short enough that a board left showing them is back to
// being an object on a desk before anyone walks past. Same reasoning as the
// toy's thirty, and shorter because there is nothing here to play with.
static constexpr uint32_t STATS_IDLE_MS = 20000;

// Layout, landscape 320x170. Two big figures over four small ones, with a
// hairline under each band, and the legend on the same datum every other
// composition puts its bottom line on.
static constexpr int STATS_MARGIN    = 14;
static constexpr int STATS_RULE1_Y   = 24;
static constexpr int STATS_BIG_LAB_Y = 32;
static constexpr int STATS_BIG_VAL_Y = 46;
static constexpr int STATS_RULE2_Y   = 86;
static constexpr int STATS_SM_LAB_Y  = 94;
static constexpr int STATS_SM_VAL_Y  = 106;
static constexpr int STATS_BIG_CX[2] = { 88, 232 };
static constexpr int STATS_SM_CX[4]  = { 40, 120, 200, 280 };

// ---------------------------------------------------------------------------
// FOCUS: a pomodoro timer, driven entirely from the two buttons
//
// Design law 1 governs the whole feature: it works with no host software ever
// installed, and the protocol fields below are a second door to it rather than
// the way in.
//
// WHAT WINS. A timer and a pending question can want the panel at the same
// moment, and the answer is the one this firmware has already given five
// times: THE QUESTION WINS, every time, everywhere. The countdown yields the
// screen exactly as the message card does -- it does not stop, it does not
// reset, it simply is not the thing being drawn -- and it takes the screen
// back the instant the prompt is answered. A timer that covered an approval
// prompt would break the only rule this device has never broken.
//
// The one place the timer outranks the milestone flourish is the FINISH. A
// milestone that happens behind a modal screen is missed on purpose, because
// nobody wants yesterday's confetti; the end of a timer is not news about the
// past, it is the single thing the owner asked this device to tell them, and
// nothing else will ever tell them. So the finish waits rather than being
// dropped: its own thirty seconds do not start until it can actually be seen.
// It cannot ambush anybody much later either, because the only thing that can
// hold it back is a prompt, and answering that prompt is the moment the owner
// is looking straight at the panel.
//
// The countdown is a CLOCK, not a screen. It runs whether or not its screen is
// on the panel, which is the whole reason it can be trusted, and while it is
// out of sight a two pixel hairline along the bottom edge of the ambient and
// live compositions drains with it. That line is the smallest honest thing
// this device can draw: legible to somebody who knows what it is, invisible to
// everybody else, and design law 4 is the reason it is not a badge.
// ---------------------------------------------------------------------------
// Beside the owner name, the face, the hop best and the two records.
static constexpr const char* NVS_KEY_FOCUSMIN = "focusmin";

// The ladder button 2 steps through. Four rungs, because a fifth is one more
// hold-and-wait than anybody will sit through: a short break, a half pomodoro,
// the classic pomodoro, and a deep block.
static const uint16_t FOCUS_LENGTHS[] = { 5, 15, 25, 45 };
static constexpr uint8_t FOCUS_LENGTH_COUNT =
    sizeof(FOCUS_LENGTHS) / sizeof(FOCUS_LENGTHS[0]);
static constexpr uint16_t FOCUS_DEFAULT_MIN = 25;
// The host may ask for anything in this range; the button ladder is a subset.
static constexpr uint16_t FOCUS_MIN_MIN = 1;
static constexpr uint16_t FOCUS_MAX_MIN = 180;

// The finish. Thirty seconds is long enough to notice from across a room and
// short enough that a board nobody came back to is an object on a desk again
// before the next meeting. It fades, it breathes on the same 2400ms the idle
// and busy states already use, and it never strobes: there is no buzzer on
// this board and design law 4 would forbid one anyway.
static constexpr uint32_t FOCUS_END_MS     = 30000;
static constexpr uint32_t FOCUS_IN_MS      = 400;
static constexpr uint32_t FOCUS_OUT_MS     = 400;
static constexpr uint32_t FOCUS_BREATHE_MS = 2400;

// The idle close, and the reason this screen needs one at all. Every other
// modal screen here puts itself away (TOY_IDLE_MS 30s, STATS_IDLE_MS 20s) and
// this one did not, so two taps of button 1 from ambient (stats, then the
// timer) parked a unit on a stopped 25:00 until somebody pressed button 2.
// Design law 1 says ambient is what most owners see most of the time, and a
// clock that is not counting is not ambient.
//
// A minute, not twenty seconds: this is a screen somebody may be reading
// rather than glancing at, and any press on it stamps the clock afresh. It
// applies ONLY to a timer at rest. A running countdown keeps its screen, and
// so does a paused one, because both are something a person deliberately put
// there and neither is a screen nobody asked to keep.
static constexpr uint32_t FOCUS_IDLE_MS    = 60000;

// Layout, landscape 320x170. The clock is Font7, the built-in 48px seven
// segment face, which is the largest digit set in the library and the only
// one that reads as a timer rather than as text. Its glyph table covers
// 0-9, ':', '.' and '-' and nothing else, which is exactly the alphabet a
// countdown needs.
static constexpr int FOCUS_MARGIN     = 14;
static constexpr int FOCUS_HEAD_Y     = 8;     // datum: top
static constexpr int FOCUS_TIME_Y     = 72;    // datum: middle
static constexpr int FOCUS_BAR_X      = 30;
static constexpr int FOCUS_BAR_W      = SCREEN_W - 2 * 30;
static constexpr int FOCUS_BAR_Y      = 118;
static constexpr int FOCUS_BAR_H      = 6;
static constexpr int FOCUS_BAR_R      = 3;
static constexpr int FOCUS_DONE_Y     = 72;    // datum: middle
static constexpr int FOCUS_DONE_SUB_Y = 116;   // datum: middle

// The hairline the base compositions carry while a run is out of sight. Below
// SUB_Y's Font2 block (146..162) and clamped so the ambient drift cannot push
// it off the bottom of the panel.
static constexpr int FOCUS_TICK_X = 14;
static constexpr int FOCUS_TICK_Y = 165;
static constexpr int FOCUS_TICK_H = 2;

// ---------------------------------------------------------------------------
// MILESTONES
//
// Design law 4 governs this entire feature: it must never be irritating to sit
// next to. So a milestone is a pill of text that fades in, sits perfectly
// still, and fades out, in two and a bit seconds. No strobe, no sound (there
// is no buzzer), no takeover, and never in amber or red, which are the two
// colours on this device that mean "look at me now".
//
// THE THRESHOLDS, and why these numbers.
//
//   A turn of TURN_LONG_S or more, on the frame it completes. Five minutes is
//   the point at which somebody has stopped watching and gone to do something
//   else, which is exactly when a quiet "that one took a while" is welcome and
//   an alarm would not be.
//
//   A turn that beats the stored longest, provided it is at least
//   TURN_RECORD_MIN_S. The floor exists so a virgin board does not celebrate
//   its first four-second turn as a personal best; a minute is past anything
//   trivial. A record supersedes the long-turn flourish, so one completed turn
//   can never fire two.
//
//   Tokens today crossing one of TOKEN_STEPS. The ladder roughly doubles
//   rather than stepping on a fixed grid, because a Codex day is measured in
//   millions and a 100k grid would fire dozens of times before lunch. Six
//   rungs is at most six flourishes in a very heavy day, hours apart, and the
//   ladder deliberately ends: past ten million the device says nothing more.
static constexpr uint32_t TURN_LONG_S       = 300;   // 5 minutes
static constexpr uint32_t TURN_RECORD_MIN_S = 60;
static const uint32_t TOKEN_STEPS[] = {
  250000u, 500000u, 1000000u, 2500000u, 5000000u, 10000000u
};
static constexpr uint8_t TOKEN_STEP_COUNT =
    sizeof(TOKEN_STEPS) / sizeof(TOKEN_STEPS[0]);

// Total, entrance and exit. 2200ms is about as long as a thing can sit on a
// screen you are not looking at without becoming furniture.
static constexpr uint32_t MILE_MS     = 2200;
static constexpr uint32_t MILE_IN_MS  = 250;
static constexpr uint32_t MILE_OUT_MS = 300;
// The pill. Height is fixed; width is solved once, when the milestone fires,
// from the two strings it holds.
static constexpr int MILE_Y     = 110;
static constexpr int MILE_H     = 48;
static constexpr int MILE_R     = 12;
static constexpr int MILE_PAD   = 22;    // either side of the wider string
static constexpr int MILE_W_MIN = 120;
static constexpr int MILE_W_MAX = 296;
static constexpr int MILE_VAL_Y = 126;   // middle datum
static constexpr int MILE_TAG_Y = 148;   // middle datum

// ---------------------------------------------------------------------------
// The first run
//
// What a board does the very first time its owner powers it on, and never
// again unless asked. Marks are cumulative milliseconds from the instant the
// boot hold ends, which is also the instant the greeting goes out. See
// firmware/FIRSTRUN.md for the beat-by-beat reasoning.
//
// The whole thing is 9.25 seconds on top of the 2 second boot hold, so the
// object has done something worth watching inside the first dozen seconds
// out of the box and is in its resting look before anyone gets bored.
// ---------------------------------------------------------------------------
static constexpr uint32_t FR_T_CRACK  = 1400;   // lights up, then lids part
static constexpr uint32_t FR_T_SQUINT = 1700;   // cracked to a squint
static constexpr uint32_t FR_T_OPEN   = 1950;   // squint held, now opening
static constexpr uint32_t FR_T_WIDE   = 2300;   // fully open
static constexpr uint32_t FR_T_LOOK   = 2700;   // has looked away
static constexpr uint32_t FR_T_FIND   = 2950;   // snapped back to centre: you
static constexpr uint32_t FR_T_BLINK  = 3400;   // the first blink
static constexpr uint32_t FR_T_GREET  = 3650;   // a hand comes up and waves
static constexpr uint32_t FR_T_NOD    = 5450;   // a small nod, name fades in
static constexpr uint32_t FR_T_HELLO  = 5650;   // "hello" fades in under it
static constexpr uint32_t FR_T_SWAP   = 7650;   // "hello" -> the ambient line
static constexpr uint32_t FR_T_END    = 9250;   // settle
static constexpr uint32_t FR_FADE_MS  = 500;    // text fade in
static constexpr uint32_t FR_SWAP_MS  = 350;    // half of the line-2 dissolve
static constexpr uint32_t FR_NOD_MS   = 350;
static constexpr float    FR_LOOK_AX  = -7.0f;  // px, where it looks first
static constexpr float    FR_LOOK_AY  = 2.0f;

// The greeting beat. The one thing on this device addressed to the room
// rather than to its owner, and the reason it sits HERE: the sequence has
// just made eye contact (FR_T_FIND) and blinked, and then had 1200ms of
// nothing to do before it started talking. A wave is what that gap was
// always shaped like. You do not wave at someone before you have seen them,
// and you do not say whose desk you are on before you have said hello, so
// the order is find you, wave at the room, then name the owner.
//
// It does NOT go at the end. The last second of the sequence is already the
// ambient composition on purpose, so the handoff at FR_T_END is invisible;
// a new thing appearing there is a new thing appearing after the object has
// finished becoming its resting self.
//
// It also does not touch the face. There is no arm anchor in the Face
// interface and adding one would mean editing all three faces, so the hand
// rises into the empty two-line text band instead, which is composition
// property. That is what makes this beat identical on rounded, arc and bear:
// none of them can tell it happened.
static constexpr uint32_t FR_GREET_MS  = 1600;  // rise, three sweeps, drop
static constexpr uint32_t FR_GREET_IN  = 300;   // hand up and text in
static constexpr uint32_t FR_GREET_OUT = 300;   // hand down and text out
static constexpr float    FR_WAVE_DEG  = 23.0f; // tilt either side of upright
static constexpr float    FR_WAVE_N    = 3.0f;  // full sweeps in the middle
static constexpr int      FR_GREET_Y   = 138;   // middle datum, group centre
static constexpr int      FR_GREET_GAP = 9;     // px between text and hand
static constexpr int      FR_GREET_RISE = 46;   // px the hand travels up
static constexpr const char* FR_GREET_TXT = "Hi OpenAI";

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

// How long "done" holds its happy expression before the eyes open again lives
// in Face.hpp as DONE_HAPPY_MS: the blink driver here suppresses blinking over
// exactly the window the face is smiling for, so both ends read one constant.

// Backlight tiers (design-spec 2.8), 0-255 PWM duty.
//
// The last rung is new and it is zero. Without it the dimmest a recipient
// could reach with no host was duty 20, and the cycle wrapped from there back
// to full: on a video call, in a shared room, or at a desk at night, the only
// gesture that changed the whole panel standalone was DO NOT DISTURB, which is
// a full-screen violet sign. A glowing face or a glowing billboard is not a
// choice, and design law 4 is the reason this device exists in the shape it
// does. `sleep` stays protocol-only, so this is the standalone owner's way to
// say "not now".
//
// It is safe to reach because it is not a mode: it is one more rung on the
// ladder button 2 already cycles, and the FIRST press of either button from
// the off rung restores full and does nothing else, exactly the way a press
// already wakes a panel the auto ladder dimmed. So a board that looks dead
// because somebody stepped one too far is one press from being alive, on
// either switch, with nothing to learn.
static const uint8_t BRIGHT_LEVELS[] = { 255, 160, 70, 20, 0 };
static constexpr uint8_t BRIGHT_LEVEL_COUNT =
    sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0]);
static constexpr uint8_t BRIGHT_FULL_IDX  = 0;
static constexpr uint8_t BRIGHT_DIM_IDX   = 2;   // 70
static constexpr uint8_t BRIGHT_SLEEP_IDX = 3;   // 20
static constexpr uint8_t BRIGHT_OFF_IDX   = 4;   // 0, only ever chosen by hand

// ---------------------------------------------------------------------------
// State
//
// enum StateId lives in Face.hpp: a face is allowed to look different per
// state, so the protocol's five names have to be visible from both sides of
// the interface.
// ---------------------------------------------------------------------------
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
// Button 2 doubles as the face swap, so it has to tell a tap from a hold.
static uint32_t button2DownMs   = 0;      // 0 when nothing is being held
static bool     button2Held     = false;  // this hold already swapped the face
static uint32_t button1DownMs   = 0;      // 0 when nothing is being held
static bool     button1Held     = false;  // this hold already fired a gesture
// Which screen each press landed on, sampled at the press edge. A hold is
// delivered only if the screen it started on is still the screen on the panel:
// the timer can arrive DURING a hold all by itself (a running clock reaching
// zero makes it visible), and without this a two second reach for the toy
// silently became "reset the timer" and retired the finish nobody saw.
static bool     button1OnFocus  = false;
static bool     button2OnFocus  = false;

// The first run. `firstRunActive` takes the screen away from both ambient and
// live for its duration; `firstRunSpend` is false for a replay, which is what
// keeps a rehearsal from consuming a board that is still armed.
static bool     firstRunActive  = false;
static uint32_t firstRunStartMs = 0;
static bool     firstRunSpend   = false;
static uint16_t firstRunAccent  = C_BOOT; // what it hands the cross-fade

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

// Do not disturb. `dndOn` is the mode; `dndOutMs` is non-zero only while the
// sign is fading back off the panel, which mirrors the message card exactly so
// the two read as one idiom rather than two.
static bool     dndOn       = false;
// When the sign went up. Two jobs, one clock: the 260ms entrance fade and the
// 60 second backlight settle both measure from here, and dndEnter returns
// early when the sign is already up, so a host repeating {"dnd":true} in a
// heartbeat can restart neither of them.
static uint32_t dndStartMs  = 0;
static uint32_t dndOutMs    = 0;   // when the fade out began, 0 = not fading

// The two-button chord that toggles it. Armed the moment the second button
// lands and resolved on the FIRST release, because that is what separates a
// chord tap from the five second chord hold that wipes the board: firing at a
// threshold instead would put the sign up on the way to a factory reset.
static bool     chordArmed  = false;
static uint32_t chordDownMs = 0;

// Taken down without a fade and without ceremony. Declared here because a
// factory reset and the out-of-the-box sequence both have to clear the sign,
// and both are defined well above where it lives.
static void dndClear();

// The toy, for the same two callers and the same reason. `toyExit` banks the
// run and hands the screen back; `toyForget` is the factory reset's half, which
// also drops the personal best, because a wiped board is an unplayed board.
static bool toyExit(uint32_t nowMs, const char* why);
static void toyForget();

// The stats screen's half of the same two callers, and one more: the records
// are in the namespace a factory reset clears, so the RAM mirrors have to come
// back to what a virgin board would load without a reboot.
static bool statsExit(uint32_t nowMs, const char* why);
static void statsForget();

// The message card's half of the same job. A card is pure RAM and outranks
// every other screen here, so a wipe that left one up would put somebody
// else's "in a meeting" over the out-of-the-box sequence.
static void sayForget();

// The focus timer, for the same two callers again. `focusHide` closes the
// screen and leaves the clock running, because a first-run replay borrowing
// the panel is not a reason to break somebody's pomodoro; `focusForget` is the
// factory reset's half and stops the clock, since a wiped board is a board
// nobody has asked anything of.
static bool focusHide(uint32_t nowMs, const char* why);
static void focusForget();

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
// clampf, scaleColor and mixColor are defined once, in Face.hpp, because the
// faces need them too. This is the local spelling of the first.
static inline float clampf(float v, float lo, float hi) {
  return faceClampf(v, lo, hi);
}

// Apply the mode cross-fade to a colour: at gXfade == 0 it is the dimmed
// accent the previous mode ended on, at 1 it is the colour as authored.
//
// Faces reach this through FaceCanvas::tint rather than by name, which is what
// lets a face live in its own translation unit with no globals in view.
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

// The message card's sanitiser, declared here because the owner name is put
// through it too: printable ASCII is the alphabet of every font on this board,
// for a name exactly as much as for a message. Defined with the rest of the
// card, further down.
static size_t saySanitize(const char* src, char* dst, size_t cap);


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
  // Through the sanitiser on the way in as well as on the way out, so a board
  // named by an older image that stored bytes the fonts cannot draw shows a
  // clean name rather than mojibake. Nothing is written back here: the next
  // {"name":...} that actually differs is what rewrites the key.
  saySanitize(buf, gOwnerName, sizeof(gOwnerName));
}

// Store a name, clamped to OWNER_NAME_MAX characters. An empty string removes
// the key so the compile-time default (or the unnamed fallback) comes back.
//
// The name goes through saySanitize first, which is the same filter the
// message card uses and for the same reason: the fonts on this board cover
// 0x20..0x7E and there is nothing honest to draw outside it. Before that,
// OWNER_NAME_MAX was a promise about characters enforced by a BYTE loop, so
// "Jose" or "Lukasz" written with their real accents stored at half the
// documented length, and a name that hit the cap mid-sequence stored a
// dangling UTF-8 lead byte that the panel and the boot greeting both carried.
// Now bytes and characters are the same number and the cap is true as written.
// Accented names are a font decision, not a truncation accident, and they are
// dropped visibly rather than mangled silently.
//
// Idempotent on purpose: NVS lives in the same flash the firmware does and a
// host that resends its whole frame every 2000ms would otherwise rewrite the
// key 43,200 times a day. The RAM mirror is exactly what is on flash, so
// comparing against it is the same test as reading the key back.
//
// Returns true if anything changed.
static bool setOwnerName(const char* v) {
  char next[OWNER_NAME_CAP];
  saySanitize(v == nullptr ? "" : v, next, sizeof(next));
  // A name that had something in it and sanitises to nothing is refused, not
  // obeyed. "" is the documented way to clear the stored name and a host that
  // sent a name plainly meant to set one, so turning an unrenderable name into
  // a clear would be the wrong half of the sentence. The card takes the other
  // rule (a message that sanitises to nothing IS a clear) because a message is
  // transient and a name outlives the frame.
  if (v != nullptr && v[0] != '\0' && next[0] == '\0') {
    Serial.println("err: name refused, nothing printable in it");
    return false;
  }
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

// ---------------------------------------------------------------------------
// Which face is active
//
// Same shape as the owner name, and stored beside it: a fresh board with an
// empty namespace comes up on the compile-time default, and anything set
// afterwards over the wire or with the button outlives the power cycle.
// ---------------------------------------------------------------------------

static void loadFaceChoice() {
  gFaceIdx = defaultFaceIndex();
  Preferences prefs;
  // A read-only open fails on a board that has never stored anything, which is
  // every freshly flashed unit. That is the default path, not an error.
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  char buf[FACE_NAME_MAX + 1];
  buf[0] = '\0';
  prefs.getString(NVS_KEY_FACE, buf, sizeof(buf));
  prefs.end();
  int i = faceIndexByName(buf);
  // An unknown stored name (a face that was removed from a later build) falls
  // back to the default rather than leaving the board with nothing to draw.
  if (i >= 0) gFaceIdx = (uint8_t)i;
}

// Switch faces, and remember it. Idempotent for the same reason setOwnerName
// is: a host that puts "face" in its 2000ms heartbeat must not rewrite the key
// 43,000 times a day. Returns true if anything changed.
static bool setFaceIndex(uint8_t idx) {
  if (idx >= faceCount()) return false;
  if (idx == gFaceIdx)    return false;      // no write, no wear
  gFaceIdx = idx;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    // Live for this power cycle but not stored. Say so rather than pretending.
    Serial.println("err: nvs open failed, face not persisted");
    return true;
  }
  prefs.putString(NVS_KEY_FACE, faceAt(idx)->name());
  prefs.end();
  return true;
}

static void cycleFace() {
  uint8_t n = faceCount();
  if (n > 0) setFaceIndex((uint8_t)((gFaceIdx + 1u) % n));
}

// ---------------------------------------------------------------------------
// The first-run flag
//
// Read once in setup(), before the first pixel, because whether this board is
// armed decides what the boot screen is allowed to be.
// ---------------------------------------------------------------------------

static void loadFirstRunFlag() {
  gFirstRunArmed = true;                 // no namespace, no key: armed
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  uint8_t v = prefs.getUChar(NVS_KEY_FIRSTRUN, 0);
  prefs.end();
  gFirstRunArmed = (v == 0);
}

// Mark the sequence spent. Called once, at the END of a first run that was
// playing for real, so a board unplugged halfway through is still armed and
// the recipient still gets the whole thing.
static void spendFirstRun() {
  if (!gFirstRunArmed) return;           // already spent, no write, no wear
  gFirstRunArmed = false;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, first run not marked spent");
    return;
  }
  prefs.putUChar(NVS_KEY_FIRSTRUN, 1);
  prefs.end();
  Serial.println("firstrun: spent");
}

// Re-arm without touching the name or the face. This is the one Sam's flashing
// run wants: name a board, check it, arm it, box it. Removing the key rather
// than storing 0 keeps "armed" and "never written" the same state.
//
// The flag a sequence ALREADY IN FLIGHT is holding has to be dropped too, or
// this call is silently undone a few seconds later. tools/flash-all.sh arms
// the instant verify_hello reads the greeting, which is t=0 of the 9.25 second
// sequence that verify_hello's own DTR/RTS reset just started, and the end of
// that sequence still runs `if (firstRunSpend) spendFirstRun()`. Measured on
// the board: `reset: firstrun ok` at t+90ms, `firstrun: spent` at t+8.47s, and
// the next boot greets with no `firstrun: playing (first)` behind it. Every
// unit in a run would ship spent. Clearing the in-flight spend makes
// `reset: firstrun ok` true at any moment rather than only when the panel
// happens to be idle, which is what the reply already claims.
static bool armFirstRun() {
  gFirstRunArmed = true;
  if (firstRunActive) firstRunSpend = false;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, first run not re-armed");
    return false;
  }
  prefs.remove(NVS_KEY_FIRSTRUN);
  prefs.end();
  return true;
}

// Everything this device has ever stored, gone: the owner name, the face and
// the first-run flag, plus anything a later firmware adds to the namespace.
// clear() rather than three removes on purpose, so this cannot silently stop
// being a full reset the next time somebody adds a key.
//
// The RAM mirrors are reset to exactly what loadOwnerName / loadFaceChoice /
// loadFirstRunFlag would produce on a virgin board, so the board is in its
// out-of-the-box state without a reboot.
static bool factoryReset() {
  Preferences prefs;
  bool ok = true;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, factory reset not persisted");
    ok = false;
  } else {
    prefs.clear();
    prefs.end();
  }
  gOwnerName[0]  = '\0';
  gFaceIdx       = defaultFaceIndex();
  gFirstRunArmed = true;
  // Same hole as armFirstRun's, on the other door: a wipe that lands during a
  // real first run was undone at the 9250ms mark by the spend the sequence was
  // still carrying. The button chord escapes this only by accident, because it
  // calls startFirstRun(nowMs, false) afterwards and that overwrites the flag.
  if (firstRunActive) firstRunSpend = false;
  // A wiped board is an out-of-the-box board, and one of those is not holding
  // a sign up or half way through a game. The sign is pure RAM; the best score
  // went with the clear() above and this resets its mirror to match.
  dndClear();
  // And the message card, for exactly the reason the sign goes: it is the one
  // other thing that can be on the panel with nothing behind it, it outranks
  // every composition here, and a board that has just been wiped is not still
  // holding up the last owner's note.
  sayForget();
  toyForget();
  // Same again for the records and the day's counters: an out-of-the-box board
  // has never seen a turn, so it must not still be claiming a twelve minute
  // one. The two stored records went with the clear() above.
  statsForget();
  // And the timer. The stored length went with the clear(); this puts the RAM
  // mirror back to the twenty-five minute default and stops a countdown that
  // belonged to the previous owner.
  focusForget();
  return ok;
}

// The rendered state, after the staleness rules. Defined with the renderer;
// declared here because the gesture and protocol guards below have to know
// whether something important is on the panel before they will interrupt it.
static StateId effectiveState(uint32_t nowMs);

// Refuse to interrupt a turn that is actually running or a prompt that is
// actually pending. Ambient always qualifies as "nothing important": it means
// no host has spoken for five minutes, or ever.
static bool safeToInterrupt(uint32_t nowMs) {
  if (ambientMode) return true;
  StateId eff = effectiveState(nowMs);
  return eff != ST_BUSY && eff != ST_WAITING;
}

// Start the sequence. `spend` is true only for the real out-of-the-box play;
// a replay leaves the stored flag exactly as it found it, so rehearsing on an
// armed board does not consume it.
static void startFirstRun(uint32_t nowMs, bool spend) {
  // Announced on the wire, because otherwise the single most important state
  // transition this device has is invisible to everything except an eye on the
  // panel. It also makes the whole lifecycle provable over serial: an armed
  // boot says "playing (first)" and then "spent", a spent boot says neither,
  // and a replay says "playing (replay)" and never spends.
  Serial.println(spend ? "firstrun: playing (first)"
                       : "firstrun: playing (replay)");
  firstRunActive  = true;
  firstRunStartMs = nowMs;
  firstRunSpend   = spend;
  firstRunAccent  = C_BOOT;

  // The sequence ends in ambient, and a sign silently reappearing over the
  // face two seconds after "hello" would be a surprise nobody asked for. A
  // game underneath it is the same problem with a score attached, so it is
  // banked and closed rather than left running.
  dndClear();
  sayForget();
  toyExit(nowMs, "firstrun");
  statsExit(nowMs, "firstrun");
  // The timer's SCREEN steps aside; its clock does not. A replay is nine and
  // a quarter seconds of theatre and a pomodoro is a commitment somebody made,
  // and the second of those outranks the first.
  focusHide(nowMs, "firstrun");

  // Dark, and shut. The sequence opens on an unlit panel and brings the light
  // up itself, which is what makes it a wake rather than a power-on. On a
  // replay this means the screen drops to black for a beat first, which reads
  // as "watch this" and is exactly the right introduction.
  lcd.setBrightness(0);
  blinkPhase   = BLINK_REST;
  blinkLeft    = 0;
  blinkNextMs  = nowMs + FR_T_BLINK;
  glanceX = glanceY = glanceTX = glanceTY = 0.0f;
  glanceReturnMs = 0;
  glanceNextMs   = nowMs + FR_T_END + randRange(GLANCE_GAP_MIN, GLANCE_GAP_MAX);
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
  // Out of range clamps to the dimmest LIT rung, not to the last element of
  // the array: the last element is duty 0 now, and a bad index must never
  // black the panel out. Every caller here passes a named constant or a
  // modulo, so this is belt and braces rather than a live path.
  if (idx >= BRIGHT_LEVEL_COUNT) idx = BRIGHT_SLEEP_IDX;
  brightIdx = idx;
  lcd.setBrightness(BRIGHT_LEVELS[idx]);
}

// Is the panel dark because somebody stepped the ladder all the way round?
// The first press of either button in that state buys the light back and is
// spent doing it: a gesture whose result nobody can see is not a gesture.
static inline bool brightIsOff() { return brightIdx == BRIGHT_OFF_IDX; }

static void brightWake() {
  setBrightness(BRIGHT_FULL_IDX);
  brightManual = true;
  dimmedByAuto = false;
}

// ---------------------------------------------------------------------------
// SAY: the message card
//
// Everything except the drawing. The drawing is renderSay, further down with
// the other compositions, because it needs the ambient palette.
//
// The layout is solved ONCE, when the message is set, not per frame: it is
// pure text metrics and the answer cannot change while the string sits still.
// ---------------------------------------------------------------------------
static char     saySrc[SAY_CAP] = "";   // sanitised message, "" = no card
static uint32_t sayStartMs = 0;         // when the card went up
static uint32_t sayHoldMs  = 0;         // settled duration, 0 = until cleared
static uint32_t sayOutMs   = 0;         // when the fade-out began, 0 = not
static bool     sayCut     = false;     // the cap threw words away: ellipsis

// The solved layout. `font` is whichever rung of the ladder fitted.
struct SayLayout {
  const lgfx::IFont* font;
  uint8_t     size;
  uint8_t     lines;
  int16_t     lineH;      // font height at that size, px
  int16_t     step;       // baseline-to-baseline for a multi-line block
  const char* tag;        // for the serial notice, e.g. "24x2"
  char        line[SAY_MAX_LINES][SAY_CAP];
};
static SayLayout sayLay = { nullptr, 1, 0, 0, 0, "", {{0}} };

// The type ladder, largest first.
//
// FreeSansBold rather than the built-in Font2/Font4: those are 16px and 26px
// bitmap faces and the only way to make them bigger is an integer pixel
// double, which at the size this needs looks like a screenshot zoomed in. The
// GFX faces are drawn at four real sizes, and only the rung that is actually
// used is linked in.
//
// `maxLines` is a taste limit, not a fitting one. One 24pt line is a sign; two
// are a message; three of anything smaller is a note. Four would be a
// paragraph on a desk toy, so the last rung truncates instead.
struct SayCandidate {
  const lgfx::IFont* font;
  uint8_t     size;
  uint8_t     maxLines;
  const char* tag;
};
static const SayCandidate SAY_LADDER[] = {
  { &fonts::FreeSansBold24pt7b, 2, 1, "24x2" },   // 112px line: "brb"
  { &fonts::FreeSansBold24pt7b, 1, 2, "24"   },   //  56px line
  { &fonts::FreeSansBold18pt7b, 1, 3, "18"   },   //  42px line
  { &fonts::FreeSansBold12pt7b, 1, 3, "12"   },   //  29px line, and it forces
};
static constexpr size_t SAY_LADDER_N =
    sizeof(SAY_LADDER) / sizeof(SAY_LADDER[0]);

// Printable ASCII only, whitespace collapsed, ends trimmed.
//
// Everything else is DROPPED rather than rejected: a message is text typed by
// a person, and refusing the whole line because it carried a tab would be a
// worse answer than showing the words. Bytes >= 0x80 go too, which means a
// UTF-8 accent or an emoji vanishes rather than rendering as mojibake: the
// fonts here are 0x20..0x7E and there is nothing honest to draw. A string that
// sanitises to nothing is a clear, which is what "" already means.
static size_t saySanitize(const char* src, char* dst, size_t cap) {
  size_t n = 0;
  bool   gap = false;   // a space is owed, once something follows it
  if (cap == 0) return 0;
  for (const unsigned char* p = (const unsigned char*)src; *p != '\0'; ++p) {
    unsigned char c = *p;
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (c < 0x20 || c > 0x7E) continue;
    if (c == ' ') { if (n > 0) gap = true; continue; }
    if (gap && n + 1 < cap) dst[n++] = ' ';
    gap = false;
    if (n + 1 >= cap) break;
    dst[n++] = (char)c;
  }
  // The cap can land mid-gap, which is the one way a trailing space survives
  // the loop above.
  while (n > 0 && dst[n - 1] == ' ') --n;
  dst[n] = '\0';
  return n;
}

// Greedy word wrap at the font currently set on gfx, measuring every candidate
// line with textWidth rather than counting characters: the faces are
// proportional, so "WWW" and "iii" are three characters and nothing like the
// same width.
//
// Returns the line count, or 0 when a single word is wider than a line and
// `hardBreak` is false, which is how a ladder rung says "too big, try the next
// one down". With `hardBreak` it never fails: an unbreakable word is split at
// the last character that fits.
static int sayWrapAll(const char* text, int maxW, bool hardBreak,
                      char out[SAY_WRAP_MAX][SAY_CAP]) {
  int    n = 0;
  char   cur[SAY_CAP];
  char   cand[SAY_CAP * 2];
  size_t i = 0;
  cur[0] = '\0';

  while (true) {
    while (text[i] == ' ') ++i;
    if (text[i] == '\0') break;

    size_t ws = i;
    while (text[i] != '\0' && text[i] != ' ') ++i;
    size_t wl = i - ws;
    if (wl > SAY_MAX) wl = SAY_MAX;
    char word[SAY_CAP];
    memcpy(word, text + ws, wl);
    word[wl] = '\0';

    if (cur[0] == '\0') copyClamped(cand, sizeof(cand), word);
    else                snprintf(cand, sizeof(cand), "%s %s", cur, word);
    if ((int)gfx->textWidth(cand) <= maxW) {
      copyClamped(cur, sizeof(cur), cand);
      continue;
    }

    // Does not fit beside what is already on the line: flush and retry alone.
    if (cur[0] != '\0') {
      if (n >= SAY_WRAP_MAX) return n;
      copyClamped(out[n++], SAY_CAP, cur);
      cur[0] = '\0';
      if ((int)gfx->textWidth(word) <= maxW) {
        copyClamped(cur, sizeof(cur), word);
        continue;
      }
    }

    // The word is wider than a whole line at this size.
    if (!hardBreak) return 0;
    size_t k = 0;
    while (word[k] != '\0') {
      char   piece[SAY_CAP];
      size_t pl = 0;
      piece[0] = '\0';
      while (word[k + pl] != '\0') {
        piece[pl]     = word[k + pl];
        piece[pl + 1] = '\0';
        if ((int)gfx->textWidth(piece) > maxW) { piece[pl] = '\0'; break; }
        ++pl;
      }
      // One character always goes, even if it is wider than the line, because
      // a zero-length piece here would never terminate.
      if (pl == 0) { piece[0] = word[k]; piece[1] = '\0'; pl = 1; }
      k += pl;
      if (word[k] == '\0') { copyClamped(cur, sizeof(cur), piece); break; }
      if (n >= SAY_WRAP_MAX) return n;
      copyClamped(out[n++], SAY_CAP, piece);
    }
  }

  if (cur[0] != '\0' && n < SAY_WRAP_MAX) copyClamped(out[n++], SAY_CAP, cur);
  return n;
}

// Trim a line until it plus an ellipsis fits, measured. Used on the last line
// of the last rung, which is the only place text is ever thrown away.
static void sayEllipsize(char* line, int maxW) {
  char buf[SAY_CAP + 4];
  snprintf(buf, sizeof(buf), "%s...", line);
  size_t len = strlen(buf);
  while (len > 3 && (int)gfx->textWidth(buf) > maxW) {
    memmove(buf + len - 4, buf + len - 3, 4);   // drop the char before the dots
    --len;
  }
  copyClamped(line, SAY_CAP, buf);
}

// Walk the ladder and keep the first rung that fits. The last rung takes
// whatever is left, so this always commits something.
static void sayLayoutText() {
  char lines[SAY_WRAP_MAX][SAY_CAP];

  for (size_t c = 0; c < SAY_LADDER_N; ++c) {
    const bool last = (c + 1 == SAY_LADDER_N);
    gfx->setFont(SAY_LADDER[c].font);
    gfx->setTextSize(SAY_LADDER[c].size);

    int lineH = (int)gfx->fontHeight();
    if (lineH < 1) lineH = 1;
    // A GFX face's advance carries generous leading, which reads as a gap at
    // this size, so a multi-line block is set 12% tighter. The fit test below
    // still budgets a full advance for the last line, so this is conservative.
    int step = (lineH * 7) / 8;

    int n = sayWrapAll(saySrc, SAY_TEXT_W, last, lines);
    if (n <= 0) continue;                       // a word too wide: go smaller

    int allow = SAY_LADDER[c].maxLines;
    if (allow > SAY_MAX_LINES) allow = SAY_MAX_LINES;
    while (allow > 1 && (allow - 1) * step + lineH > SAY_TEXT_H) --allow;
    // Words are thrown away in exactly two places: the 48-character cap and
    // this line limit. Either one earns an ellipsis, and the flag is carried
    // to one call rather than two so a message that hit both cannot end in
    // six dots.
    bool cut = sayCut;
    if (n > allow) {
      if (!last) continue;
      n   = allow;
      cut = true;
    }
    if (cut) sayEllipsize(lines[n - 1], SAY_TEXT_W);

    sayLay.font  = SAY_LADDER[c].font;
    sayLay.size  = SAY_LADDER[c].size;
    sayLay.lines = (uint8_t)n;
    sayLay.lineH = (int16_t)lineH;
    sayLay.step  = (int16_t)step;
    sayLay.tag   = SAY_LADDER[c].tag;
    for (int k = 0; k < n; ++k) copyClamped(sayLay.line[k], SAY_CAP, lines[k]);
    break;
  }

  gfx->setTextSize(1);   // leave the shared text state as every renderer wants
}

// Is there a card at all, fading in, holding or fading out.
static inline bool sayCardUp() { return saySrc[0] != '\0'; }

// Start the fade-out. Idempotent, so a button press during the fade and the
// expiry that follows cannot both announce themselves.
// The factory reset's half, named to match toyForget, statsForget and
// focusForget. Taken down with no fade and no ceremony, exactly like the sign:
// the frame after a wipe has to be an out-of-the-box board and not the tail of
// somebody else's message fading out of it.
static void sayForget() {
  saySrc[0]    = '\0';
  sayOutMs     = 0;
  sayHoldMs    = 0;
  sayLay.lines = 0;
}

static void sayDismiss(uint32_t nowMs, const char* why) {
  if (!sayCardUp() || sayOutMs != 0) return;
  sayOutMs = nowMs;
  Serial.printf("say: %s\n", why);
}

// Re-time the card that is up, from now. Defined below with the rest of the
// card's clock; declared here because setSay's idempotence guard hands a
// repeat of the message already on the panel straight to it.
static bool retimeSay(uint32_t secs);

// Set (or replace) the message. `secs` of 0 holds it until something clears
// it: a protocol line, a button, or a power cycle.
static void setSay(const char* raw, uint32_t secs) {
  // Sanitise into a buffer wider than the cap, so a message that overruns can
  // be TOLD it overran rather than just ending mid-sentence. A protocol line
  // is 512 bytes, and everything past this scratch is words nobody was ever
  // going to read off a 320px panel.
  char scratch[192];
  saySanitize(raw, scratch, sizeof(scratch));
  if (scratch[0] == '\0') {                     // "" and unprintable both clear
    sayDismiss(millis(), "cleared");
    return;
  }

  char clean[SAY_CAP];
  sayCut = (strlen(scratch) > SAY_MAX);
  copyClamped(clean, sizeof(clean), scratch);
  // The cap can land on a space, and a line ending in a space then an ellipsis
  // reads as a typo.
  for (size_t k = strlen(clean); k > 0 && clean[k - 1] == ' '; --k) {
    clean[k - 1] = '\0';
  }

  // Idempotent, for exactly the reason setOwnerName, setFaceIndex, saveFocusMins
  // and dndEnter are: a host that puts "say" in its 2000ms heartbeat must not
  // restart the 220ms entrance fade and re-measure the four rung type ladder
  // every two seconds forever. This is the composition the anti-freeze breathe
  // in renderSay exists for, so replaying its entrance on a loop is the one
  // irritation design law 4 most has to avoid. A repeat re-times the card that
  // is up instead of replaying it.
  if (sayOutMs == 0 && strcmp(clean, saySrc) == 0) {
    retimeSay(secs);
    return;
  }

  copyClamped(saySrc, sizeof(saySrc), clean);
  sayStartMs = millis();
  sayOutMs   = 0;
  sayHoldMs  = secs * 1000u;
  sayLayoutText();

  // A message arriving is an interaction, exactly like acknowledging a prompt,
  // so it wakes a panel the auto ladder had dimmed. It does not override a
  // level somebody chose by hand.
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);
    dimmedByAuto = false;
    brightManual = true;
  }

  // Echo what is actually on the panel, line by line, separated by " | ".
  // Not the string the host sent: the string the host sent has been through a
  // 48-character cap, a sanitiser and a wrapper, and the only way anybody can
  // check that work without standing in front of the board is to read the
  // result back. It proves the cap, the collapsed whitespace, the line breaks,
  // the ellipsis and which rung of the type ladder it landed on.
  //
  // Informational. Nothing parses it, and no other program has to care.
  char shown[SAY_CAP * SAY_MAX_LINES + 8];
  size_t at = 0;
  shown[0] = '\0';
  for (int i = 0; i < (int)sayLay.lines && at + 1 < sizeof(shown); ++i) {
    int w = snprintf(shown + at, sizeof(shown) - at, "%s%s",
                     (i == 0) ? "" : " | ", sayLay.line[i]);
    if (w < 0) break;
    at += (size_t)w;
    if (at >= sizeof(shown)) { at = sizeof(shown) - 1; break; }
  }
  if (secs == 0) {
    Serial.printf("say: showing \"%s\" hold lines=%u font=%s\n",
                  shown, (unsigned)sayLay.lines, sayLay.tag);
  } else {
    Serial.printf("say: showing \"%s\" %us lines=%u font=%s\n",
                  shown, (unsigned)secs, (unsigned)sayLay.lines, sayLay.tag);
  }
}

// Re-time the card that is up, from now. This is what a bare "saysecs" does.
static bool retimeSay(uint32_t secs) {
  if (!sayCardUp() || sayOutMs != 0) return false;
  sayHoldMs  = secs * 1000u;
  // Back-date the start past the entrance so the hold runs from now and the
  // card does not fade in a second time.
  sayStartMs = millis() - SAY_IN_MS;
  if (secs == 0) Serial.println("say: holding");
  else           Serial.printf("say: %us\n", (unsigned)secs);
  return true;
}

// Retire an expired or faded-out card. Called once per frame.
static void updateSay(uint32_t nowMs) {
  if (!sayCardUp()) return;
  if (sayOutMs != 0) {
    if (nowMs - sayOutMs >= SAY_OUT_MS) {
      saySrc[0]    = '\0';
      sayOutMs     = 0;
      sayLay.lines = 0;
      // Hand the screen back through the same cross-fade the device uses
      // between ambient and live, so the face returns over 700ms instead of
      // cutting in. modeChangeMs only ever gates that fade; updateMode
      // compares modes, not timestamps, so re-arming it here cannot confuse
      // which mode the device thinks it is in.
      modeChangeMs = nowMs;
      xfadeFrom    = lastAccent;
    }
    return;
  }
  if (sayHoldMs != 0 && (nowMs - sayStartMs) >= sayHoldMs + SAY_IN_MS) {
    sayDismiss(nowMs, "expired");
  }
}

// Does the card own the screen this frame.
//
// It yields to the out-of-the-box sequence and to a pending prompt, and it
// takes the screen back when either finishes if its clock has not run out. A
// message must never be the reason somebody missed the question.
static bool saySuppressed(uint32_t nowMs) {
  if (firstRunActive) return true;
  return !ambientMode && effectiveState(nowMs) == ST_WAITING;
}

static bool sayVisible(uint32_t nowMs) {
  return sayCardUp() && !saySuppressed(nowMs);
}

// 0 while off screen, 1 when settled.
static float sayAlpha(uint32_t nowMs) {
  if (sayOutMs != 0) {
    return 1.0f - clampf((float)(nowMs - sayOutMs) / (float)SAY_OUT_MS,
                         0.0f, 1.0f);
  }
  return clampf((float)(nowMs - sayStartMs) / (float)SAY_IN_MS, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// DO NOT DISTURB: everything except the drawing
//
// The drawing is renderDnd, further down with the other compositions, because
// it needs the burn-in drift.
//
// The layout is solved ONCE, when the sign goes up, exactly as the message
// card's is: the two words never change, so their widths cannot either, and
// solving them per frame would be measuring the same two strings thirty times
// a second forever.
// ---------------------------------------------------------------------------
struct DndLayout {
  const lgfx::IFont* font;
  int16_t     lineH;   // font height, px
  int16_t     step;    // baseline to baseline for the two-line block
  int16_t     w1, w2;  // measured widths, reported on the wire
  const char* tag;     // for the serial notice, "24" or "18"
};
static DndLayout dndLay = { nullptr, 0, 0, 0, 0, "" };

// Two rungs, largest first, and the first one both words fit on wins. This is
// the same idea as the say ladder and deliberately not the same code: that
// ladder wraps arbitrary host text, this one measures two compile-time
// constants, and the honest version of "does DISTURB fit" is a textWidth call
// rather than a shared wrapper carrying eight cases neither string can hit.
static void dndSolveLayout() {
  static const struct { const lgfx::IFont* font; const char* tag; } RUNGS[] = {
    { &fonts::FreeSansBold24pt7b, "24" },
    { &fonts::FreeSansBold18pt7b, "18" },
  };
  for (size_t i = 0; i < sizeof(RUNGS) / sizeof(RUNGS[0]); ++i) {
    gfx->setFont(RUNGS[i].font);
    gfx->setTextSize(1);
    int w1 = (int)gfx->textWidth(DND_LINE1);
    int w2 = (int)gfx->textWidth(DND_LINE2);
    int lineH = (int)gfx->fontHeight();
    if (lineH < 1) lineH = 1;
    const bool last = (i + 1 == sizeof(RUNGS) / sizeof(RUNGS[0]));
    if (!last && (w1 > DND_TEXT_W || w2 > DND_TEXT_W)) continue;
    dndLay.font  = RUNGS[i].font;
    dndLay.lineH = (int16_t)lineH;
    // The same 12% tightening the say card uses: a GFX face's own advance
    // carries leading that reads as a gap between two stacked words.
    dndLay.step  = (int16_t)((lineH * 7) / 8);
    dndLay.w1    = (int16_t)w1;
    dndLay.w2    = (int16_t)w2;
    dndLay.tag   = RUNGS[i].tag;
    break;
  }
  gfx->setTextSize(1);   // leave the shared text state as every renderer wants
}

// Put the sign up. Idempotent, and silent when it is already up: a host that
// puts "dnd" in its 2000ms heartbeat must not restart the fade in thirty times
// a minute or fill the wire with notices.
//
// The notice carries the solved layout, because nobody can read a panel over
// USB and the measured widths are the only proof the words fit the frame.
static bool dndEnter(uint32_t nowMs, const char* why) {
  if (dndOn && dndOutMs == 0) return false;
  // Refused while a prompt is pending, exactly like the toy, the stats screen
  // and the timer. This was the one modal screen that accepted and then hid
  // itself: dndSuppressed covers the sign while a question is on the panel, so
  // the chord (or a host line) latched a mode with nothing on screen, and both
  // buttons are gated on dndVisible so neither could take it down. It came up
  // on its own whenever the prompt resolved. Proven on hardware before the
  // fix: {"state":"waiting"} then {"dnd":true} was accepted while stats, play
  // and focus were all refusing with "prompt pending".
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    Serial.println("err: dnd refused, prompt pending");
    return false;
  }
  dndOn      = true;
  dndOutMs   = 0;
  dndStartMs = nowMs;
  dndSolveLayout();
  Serial.printf("dnd: on (%s) font=%s w=%d,%d max=%d\n", why, dndLay.tag,
                (int)dndLay.w1, (int)dndLay.w2, DND_TEXT_W);
  return true;
}

// Start the fade out. Idempotent for the same reason sayDismiss is: a button
// press during the fade and a host line behind it must not both announce.
static bool dndExit(uint32_t nowMs, const char* why) {
  if (!dndOn || dndOutMs != 0) return false;
  dndOutMs = nowMs;
  Serial.printf("dnd: off (%s)\n", why);
  return true;
}

// Gone now, no fade. Only a factory reset and the out-of-the-box sequence use
// this: both are already taking the whole screen for their own reasons, and a
// sign dissolving underneath either of them is not a transition anybody meant.
static void dndClear() {
  if (!dndOn) return;
  dndOn    = false;
  dndOutMs = 0;
  Serial.println("dnd: off (cleared)");
}

static void dndToggle(uint32_t nowMs, const char* why) {
  if (dndOn && dndOutMs == 0) dndExit(nowMs, why);
  else                        dndEnter(nowMs, why);
}

// Retire a finished fade out. Called once per frame, before the frame decides
// what to draw, so the frame that hands the screen back is the same one that
// re-arms the mode cross-fade and the face returns over 700ms instead of
// cutting in. Same handoff the message card uses.
static void updateDnd(uint32_t nowMs) {
  if (!dndOn || dndOutMs == 0) return;
  if (nowMs - dndOutMs < DND_OUT_MS) return;
  dndOn        = false;
  dndOutMs     = 0;
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
}

// What the sign yields to.
//
// A pending prompt, always. This is the same rule as the message card and it
// is the same rule for the same reason: nothing on this device may be the
// reason somebody missed the question. The sign is not cancelled by yielding,
// it is covered, and it comes back the moment the prompt is answered.
//
// The card itself wins too. A card is transient and self-clearing while the
// sign is indefinite, so a card can never permanently hide the sign, and the
// one rule a person has to learn stays "a press takes down whatever is on
// top".
static bool dndSuppressed(uint32_t nowMs) {
  if (firstRunActive)     return true;
  if (sayVisible(nowMs))  return true;
  return !ambientMode && effectiveState(nowMs) == ST_WAITING;
}

static bool dndVisible(uint32_t nowMs) {
  return dndOn && !dndSuppressed(nowMs);
}

// 0 while off screen, 1 when settled.
static float dndAlpha(uint32_t nowMs) {
  if (dndOutMs != 0) {
    return 1.0f - clampf((float)(nowMs - dndOutMs) / (float)DND_OUT_MS,
                         0.0f, 1.0f);
  }
  return clampf((float)(nowMs - dndStartMs) / (float)DND_IN_MS, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// THE TOY: everything except the drawing
//
// The drawing is renderToy, further down with the other compositions, because
// it needs the ambient palette and the face registry.
// ---------------------------------------------------------------------------
struct ToyObstacle {
  float x;        // left edge, px. Only meaningful while `live`
  int   w, h;
  bool  live;
  bool  scored;   // already counted as it went past
};

static bool     toyOn      = false;
static bool     toyDead    = false;
static uint32_t toyDeadMs  = 0;   // when the run ended
static uint32_t toyInputMs = 0;   // last press, for the walk-away timeout
static float    toyY       = 0.0f;   // px above the ground, 0 = standing
static float    toyVy      = 0.0f;   // px/s, negative is upward
static float    toySpeed   = TOY_SPEED0;
static float    toySpawnIn = TOY_FIRST_GAP;   // px of travel until the next
static float    toyRunPhase = 0.0f;           // the running bob, 0..1
static float    toyScroll  = 0.0f;            // ground ticks, 0..59
static uint16_t toyScore   = 0;
static uint16_t toyBest    = 0;
static bool     toyNewBest = false;
static bool     toyEverHopped = false;        // hides the opening prompt
static bool     toyWasVisible = false;        // for the resume grace, below
static ToyObstacle toyObs[TOY_OBS_MAX];

// The stored personal best. Read once at boot, like the name and the face.
static void loadHopBest() {
  toyBest = 0;
  Preferences prefs;
  // A board that has never played has no namespace and no key. That is the
  // normal path on a fresh unit, not an error.
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  toyBest = prefs.getUShort(NVS_KEY_HOPBEST, 0);
  prefs.end();
}

// Written only when a run actually beats the stored value, so a board that is
// played with all afternoon touches flash once per record and not once per
// game. Everything else here is RAM.
static void saveHopBest() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, best not persisted");
    return;
  }
  prefs.putUShort(NVS_KEY_HOPBEST, toyBest);
  prefs.end();
}

// Bank the run that just ended. Called from the crash and from every exit, so
// walking away mid-run cannot cost a record either.
static void toyBank() {
  if (toyScore > toyBest) {
    toyBest    = toyScore;
    toyNewBest = true;
    saveHopBest();
  }
}

static void toyResetRun(uint32_t nowMs) {
  toyDead     = false;
  toyDeadMs   = 0;
  toyY        = 0.0f;
  toyVy       = 0.0f;
  toySpeed    = TOY_SPEED0;
  toySpawnIn  = TOY_FIRST_GAP;
  toyRunPhase = 0.0f;
  toyScroll   = 0.0f;
  toyScore    = 0;
  toyNewBest  = false;
  toyInputMs  = nowMs;
  for (int i = 0; i < TOY_OBS_MAX; ++i) toyObs[i].live = false;
}

// Is the toy the thing on the panel this frame. It is below the message card
// and the sign in exactly the order they are below each other, and it pauses
// rather than playing on underneath: see toyStep.
static bool toyVisible(uint32_t nowMs) {
  if (!toyOn) return false;
  if (firstRunActive)    return false;
  if (sayVisible(nowMs)) return false;
  if (dndVisible(nowMs)) return false;
  // A pending question, tested here as well as in toyTick. toyTick is what
  // actually ends the run, but it runs inside the frame and the buttons are
  // pumped before it, so without this line there is one 33ms window in which a
  // press would jump instead of acknowledging. The yield has to be true of the
  // whole device and not just of the renderer.
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) return false;
  return true;
}

// Leaving. Every exit routes through here so the score is banked exactly once
// and the screen is handed back through the same 700ms cross-fade the device
// uses between ambient and live, rather than cutting.
static bool toyExit(uint32_t nowMs, const char* why) {
  if (!toyOn) return false;
  toyBank();
  toyOn = false;
  Serial.printf("play: end (%s) score=%u best=%u\n", why,
                (unsigned)toyScore, (unsigned)toyBest);
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  return true;
}

// Opening. Refused while a prompt is pending, loudly, for the same reason the
// first-run replay is: a screen that covers a question is the one thing this
// device must not do, and the host asking for it does not make it safe.
static bool toyEnter(uint32_t nowMs, const char* why) {
  if (toyOn) return false;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    Serial.println("err: play refused, prompt pending");
    return false;
  }
  toyOn = true;
  toyEverHopped = false;
  toyWasVisible = true;
  toyResetRun(nowMs);
  Serial.printf("play: hop (%s) best=%u\n", why, (unsigned)toyBest);
  // Starting a game is an interaction, exactly like acknowledging a prompt, so
  // it wakes a panel the auto ladder had dimmed.
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);
    dimmedByAuto = false;
    brightManual = true;
  }
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  return true;
}

// The factory reset's half. prefs.clear() has already taken the stored best
// with everything else in the namespace, so this only has to bring the RAM
// mirror back to what a virgin board would load, and put the game away.
static void toyForget() {
  toyOn      = false;
  toyBest    = 0;
  toyScore   = 0;
  toyNewBest = false;
}

// The only control the game has. Grounded jumps only: a double jump would make
// the one rule ("press when a block is close") stop being the whole rule.
static void toyHop(uint32_t nowMs, bool fromButton) {
  if (!toyOn) return;
  // Only a finger counts as input. TOY_IDLE_MS is what stops the game sitting
  // lit on an empty desk, and stamping this from the wire handed any host a
  // way to defeat it: a loop of {"play":"hop"} held the screen open forever,
  // measured over the cable. A host may still play the game; it just cannot
  // keep the screen from closing on a desk nobody is at.
  const uint32_t keepInput = toyInputMs;
  if (fromButton) toyInputMs = nowMs;
  if (toyDead) {
    // A crash has to be readable before a press can undo it, or a player still
    // pressing at the moment of impact restarts without seeing what happened.
    if (nowMs - toyDeadMs < TOY_OVER_MS) return;
    toyResetRun(nowMs);
    // toyResetRun stamps the idle clock, which is right for a person starting
    // the next run and wrong for a host: measured over the cable, a loop of
    // {"play":"hop"} kept restarting after each crash and held the screen open
    // past 45 seconds. The stamp goes back to what it was.
    if (!fromButton) toyInputMs = keepInput;
    return;
  }
  if (toyY > 0.0f) return;              // already in the air
  toyVy = -TOY_JUMP_V;
  toyEverHopped = true;
}

// Checked every frame, whether or not the toy is the thing being drawn, so the
// two ways it has to end cannot be delayed by a message card sitting on top of
// it. The physics are in toyStep and only run on frames the toy is visible.
static void toyTick(uint32_t nowMs) {
  if (!toyOn) return;
  // A pending question ends it. This is the whole safety argument for putting
  // a game on the device at all, so it is the first thing tested and it does
  // not wait for the frame the toy happens to be drawing.
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    toyExit(nowMs, "prompt");
    return;
  }
  if (nowMs - toyInputMs >= TOY_IDLE_MS) {
    toyExit(nowMs, "idle");
    return;
  }

  // Coming back out from under a message card or a sign. The pause froze the
  // whole field, so without this the runner reappears with a block already
  // touching it and dies for something it could not have seen: measured on the
  // bench, a card held over a live game killed the run on the frame it lifted.
  // The stretch is cleared and the score is kept, which is what somebody who
  // just dismissed a message would expect and is the only reading that cannot
  // feel like a cheat in either direction.
  const bool vis = toyVisible(nowMs);
  if (vis && !toyWasVisible) {
    for (int i = 0; i < TOY_OBS_MAX; ++i) toyObs[i].live = false;
    toySpawnIn = TOY_FIRST_GAP;
  }
  toyWasVisible = vis;
}

// One frame of game. Called from renderToy alone, which is what makes the
// pause free: a frame the toy is not drawn on is a frame it does not move.
static void toyStep(uint32_t nowMs, uint32_t dtMs) {
  (void)nowMs;
  if (toyDead) return;
  // Clamped harder than the renderer's own 500ms, so a stall cannot teleport
  // the runner through an obstacle it never had a chance to jump.
  if (dtMs > 100) dtMs = 100;
  const float dt = (float)dtMs / 1000.0f;

  // The jump. Integrated rather than played back from a curve, because the
  // arc has to stay the same shape whatever the frame rate is doing.
  if (toyY > 0.0f || toyVy < 0.0f) {
    toyVy += TOY_GRAVITY * dt;
    toyY  -= toyVy * dt;
    if (toyY <= 0.0f) { toyY = 0.0f; toyVy = 0.0f; }
  }

  const float travel = toySpeed * dt;

  // The run cadence, one cycle per 26px of ground. The bob it drives is
  // applied by the composition and not by the face, exactly like the first
  // run's nod: no face has to know this screen exists.
  toyRunPhase += travel / 26.0f;
  while (toyRunPhase >= 1.0f) toyRunPhase -= 1.0f;

  // Ground ticks, one every 60px. They are the only thing on screen that says
  // how fast the ground is moving, since the runner itself never moves in x.
  toyScroll += travel;
  while (toyScroll >= 60.0f) toyScroll -= 60.0f;

  // Spawning is measured in pixels of ground rather than in milliseconds, so
  // the gap between two blocks is the same distance at every speed and getting
  // faster never turns into getting unfair.
  toySpawnIn -= travel;
  if (toySpawnIn <= 0.0f) {
    for (int i = 0; i < TOY_OBS_MAX; ++i) {
      if (toyObs[i].live) continue;
      // Two shapes, so the run is not one block repeated: a narrow tall one
      // that has to be jumped and a wide short one that has to be jumped
      // earlier. Both are clearable from a standing start.
      const bool tall = (esp_random() & 1u) != 0;
      toyObs[i].w      = tall ? (int)randRange(9, 12) : (int)randRange(14, 17);
      toyObs[i].h      = tall ? (int)randRange(17, 21) : (int)randRange(11, 14);
      toyObs[i].x      = (float)SCREEN_W + 8.0f;
      toyObs[i].live   = true;
      toyObs[i].scored = false;
      break;
    }
    toySpawnIn = TOY_GAP_MIN +
                 (float)randRange(0, (uint32_t)(TOY_GAP_MAX - TOY_GAP_MIN));
  }

  const float hitL = (float)(TOY_RUN_X - TOY_HIT_HALF);
  const float hitR = (float)(TOY_RUN_X + TOY_HIT_HALF);

  for (int i = 0; i < TOY_OBS_MAX; ++i) {
    if (!toyObs[i].live) continue;
    toyObs[i].x -= travel;
    if (toyObs[i].x + (float)toyObs[i].w < -4.0f) {
      toyObs[i].live = false;
      continue;
    }

    const bool overlap = (toyObs[i].x < hitR) &&
                         (toyObs[i].x + (float)toyObs[i].w > hitL);

    // The whole hit test: you are past it if you are higher than it. The
    // vertical half is a single comparison rather than a box intersection
    // because every obstacle stands on the same ground the runner does, and
    // one comparison is one thing to explain to somebody who just lost.
    if (overlap && toyY < (float)(toyObs[i].h + TOY_HIT_CLR)) {
      toyDead   = true;
      toyDeadMs = millis();
      toyBank();
      Serial.printf("play: over score=%u best=%u%s\n", (unsigned)toyScore,
                    (unsigned)toyBest, toyNewBest ? " (new best)" : "");
      return;
    }

    if (!toyObs[i].scored && toyObs[i].x + (float)toyObs[i].w < hitL) {
      toyObs[i].scored = true;
      if (toyScore < 9999) ++toyScore;
      toySpeed += TOY_SPEED_UP;
      if (toySpeed > TOY_SPEED_MAX) toySpeed = TOY_SPEED_MAX;
    }
  }
}

// ---------------------------------------------------------------------------
// STATS and MILESTONES: everything except the drawing
//
// The drawing is renderStats and renderMilestone, further down with the other
// compositions, because both need the palette and the cross-fade.
// ---------------------------------------------------------------------------

// The clock, as told by the host. Stored as the value plus the millis() it
// arrived at, and extrapolated from there, so a host that stops talking does
// not stop the day: a board that has been told the time once at 09:00 still
// knows when midnight is.
static bool     gClockKnown = false;
static uint32_t gClockLocal = 0;    // local unix seconds, at gClockAtMs
static uint32_t gClockAtMs  = 0;
// Local day index (local seconds / 86400). -1 until a clock arrives, which is
// what the screen reads to decide between TODAY and SINCE BOOT.
static int32_t  gDayIdx     = -1;

// Today. RAM only, see the header comment: a reboot zeroes these deliberately.
static uint32_t stTurns     = 0;   // completed turns
static uint32_t stTokens    = 0;   // tokens banked from the "tokens" field
static uint32_t stLongest   = 0;   // seconds, longest completed turn today

// The records. Mirrored from NVS at boot and written back only when they move.
static uint32_t stBestTurn  = 0;   // seconds
static uint32_t stBestDay   = 0;   // tokens
static uint32_t stSavedTurn = 0;   // what is actually on flash
static uint32_t stSavedDay  = 0;

// The ceiling on a figure that is allowed to become a RECORD. The daily
// counter saturates at 0xFFFFFFFF rather than wrapping, and a saturated (or
// merely absurd) total that is promoted goes to NVS on the next completed turn
// and stays there: no protocol verb clears the records, so the only way out is
// a factory reset, which also destroys the owner name. Proven on the bench:
// one {"tokens":1e300} produced best_day=4294967295 and it survived the power
// cycle. A hundred million tokens in one day is already twenty times the
// highest rung on the milestone ladder, so nothing above it is a day somebody
// had, and refusing to promote it costs a real user nothing.
static constexpr uint32_t STATS_DAY_RECORD_MAX = 100000000u;

// The one place today's total becomes the record. Every promotion goes through
// here so the guard cannot be forgotten at one of the three edges that move it.
static inline void statsPromoteDay() {
  if (stTokens > stBestDay && stTokens <= STATS_DAY_RECORD_MAX) {
    stBestDay = stTokens;
  }
}

// The current session: the run of host contact this device is inside. It opens
// on the first accepted line after a silence and closes when the device gives
// up and goes back to ambient, which is the same five minutes every other
// staleness rule here uses.
static bool     stSessionOpen = false;
static uint32_t stSessionMs   = 0;

// The turn being timed, if any. A turn runs from the first `busy` (or the
// `waiting` that can precede one) to the `done` that completes it.
static bool     stTurnOpen  = false;
static uint32_t stTurnMs    = 0;

// Where the "tokens" field is booked. The device banks differences rather than
// the value itself, because the value restarts at zero every time a session
// begins and a board that stored the total would count one session twice.
//
// One running value is not enough, because more than one Codex session can be
// talking to one cable: section 2.12 of the protocol admits it and the shipped
// helper makes it routine, one hook process per session, each sending the
// cumulative total of ITS OWN session. Two sessions alternating against a
// single tracker measure every high against the other session's low and bank
// the whole of it. Measured on the bench: 19,000 real tokens counted as
// 10,210,000, and the inflated figure written to the best-day record.
//
// So the device keeps a small set of lanes, one per session it has heard from,
// and books a value against the lane it fits: the one giving the smallest
// non-negative step. A value that fits no lane (a session heard from for the
// first time, or a jump too big to be one event) takes a lane as a baseline
// and contributes nothing, exactly as the first value ever seen does.
static constexpr uint8_t  TOKEN_LANES    = 4;
// The largest step this device will believe from a single protocol line.
// Above it the value is not growth, it is a different session, and banking it
// is how a counter runs away. A turn that genuinely adds more than half a
// million tokens loses the excess, which costs a desk tile nothing.
static constexpr uint32_t TOKEN_LINE_MAX = 500000;
static uint32_t stTokLane[TOKEN_LANES]     = {0};
static uint32_t stTokLaneAt[TOKEN_LANES]   = {0};   // millis of its last hit
static bool     stTokLaneUsed[TOKEN_LANES] = {false};

// The stats screen.
static bool     statsOn      = false;
static uint32_t statsInputMs = 0;

// The milestone in flight, if any.
enum MileKind : uint8_t {
  MILE_NONE = 0,
  MILE_TURN_LONG,
  MILE_TURN_RECORD,
  MILE_TOKENS,
};
static MileKind mileKind    = MILE_NONE;
static uint32_t mileStartMs = 0;
static char     mileVal[16] = "";   // the big string
static char     mileTag[20] = "";   // the small one under it
static int      mileW       = MILE_W_MIN;
// Which rung of TOKEN_STEPS is next. Reset at midnight with the counters.
static uint8_t  mileTokStep = 0;

// --- formatting ------------------------------------------------------------

// "8:12", or "1:02:03" past an hour. The same shape the helper's own
// formatElapsed produces, so the sub line and this screen agree.
static void fmtDur(char* out, size_t cap, uint32_t secs) {
  uint32_t h = secs / 3600u, m = (secs % 3600u) / 60u, s = secs % 60u;
  if (h > 0) snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)h,
                      (unsigned long)m, (unsigned long)s);
  else       snprintf(out, cap, "%lu:%02lu", (unsigned long)m,
                      (unsigned long)s);
}

// "912", "12.3k", "1.24M". Three or four significant figures, because the
// whole point of the big tile is that it is readable at desk distance and
// "1243907" is not.
static void fmtTokens(char* out, size_t cap, uint32_t t) {
  if      (t < 1000u)     snprintf(out, cap, "%lu", (unsigned long)t);
  else if (t < 1000000u)  snprintf(out, cap, "%.1fk", (double)t / 1000.0);
  else                    snprintf(out, cap, "%.2fM", (double)t / 1000000.0);
}

// The same, rounded to the rung: a milestone says "1M", not "1.00M".
static void fmtTokenStep(char* out, size_t cap, uint32_t t) {
  if (t < 1000000u) snprintf(out, cap, "%luk", (unsigned long)(t / 1000u));
  else if (t % 1000000u == 0u)
    snprintf(out, cap, "%luM", (unsigned long)(t / 1000000u));
  else snprintf(out, cap, "%.1fM", (double)t / 1000000.0);
}

// --- the records in NVS ----------------------------------------------------

static void loadStatsRecords() {
  stBestTurn = stBestDay = 0;
  Preferences prefs;
  // A board that has never completed a turn has no keys, which is the normal
  // path on a fresh unit and not an error. Same as the hop best.
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  stBestTurn = prefs.getUInt(NVS_KEY_BESTTURN, 0);
  stBestDay  = prefs.getUInt(NVS_KEY_BESTDAY, 0);
  prefs.end();
  stSavedTurn = stBestTurn;
  stSavedDay  = stBestDay;
}

// Written only when a record actually moves, and only from the edges that can
// move one: a completed turn, and midnight. Never on a timer and never per
// protocol line, which is what keeps a host heartbeating every 2000ms from
// rewriting flash 43,000 times a day.
static void saveStatsRecords() {
  if (stBestTurn == stSavedTurn && stBestDay == stSavedDay) return;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, records not persisted");
    return;
  }
  if (stBestTurn != stSavedTurn) {
    prefs.putUInt(NVS_KEY_BESTTURN, stBestTurn);
    stSavedTurn = stBestTurn;
  }
  if (stBestDay != stSavedDay) {
    prefs.putUInt(NVS_KEY_BESTDAY, stBestDay);
    stSavedDay = stBestDay;
  }
  prefs.end();
}

// The factory reset's half. prefs.clear() has already taken both records with
// everything else in the namespace, so this brings the RAM mirrors back to
// what a virgin board would load and drops the day with them.
static void statsForget() {
  statsOn    = false;
  stTurns    = 0;
  stTokens   = 0;
  stLongest  = 0;
  stBestTurn = stBestDay = 0;
  stSavedTurn = stSavedDay = 0;
  stTurnOpen = false;
  for (uint8_t i = 0; i < TOKEN_LANES; ++i) {
    stTokLaneUsed[i] = false;
    stTokLane[i]     = 0;
    stTokLaneAt[i]   = 0;
  }
  mileKind    = MILE_NONE;
  mileTokStep = 0;
}

// --- milestones ------------------------------------------------------------

// Solve the pill's width once, here, rather than measuring two strings thirty
// times a second for the two seconds it is up. Same rule as the message card.
static void mileFire(uint32_t nowMs, MileKind kind, const char* tag,
                     const char* val) {
  // Never over a pending question. A flourish is the least important thing
  // this device can draw and a prompt is the most important, and the one rule
  // that has to hold for every screen added here is that nothing covers a
  // question. This one does not even start.
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) return;
  if (firstRunActive) return;

  mileKind    = kind;
  mileStartMs = nowMs;
  copyClamped(mileVal, sizeof(mileVal), val);
  copyClamped(mileTag, sizeof(mileTag), tag);

  gfx->setTextSize(1);
  gfx->setFont(&fonts::Font4);
  int w = gfx->textWidth(mileVal);
  gfx->setFont(&fonts::Font0);
  int wt = gfx->textWidth(mileTag);
  if (wt > w) w = wt;
  w += 2 * MILE_PAD;
  if (w < MILE_W_MIN) w = MILE_W_MIN;
  if (w > MILE_W_MAX) w = MILE_W_MAX;
  mileW = w;

  Serial.printf("milestone: %s %s\n", tag, val);
}

// Is a milestone the thing to draw over this frame's composition. It is an
// overlay on live and ambient only: every modal screen here outranks it, and
// its clock keeps running underneath one, so a flourish that happened while a
// message card was up is simply missed rather than queued up to surprise
// somebody later. Nobody wants yesterday's confetti.
static bool mileVisible(uint32_t nowMs) {
  if (mileKind == MILE_NONE) return false;
  // Retired the moment its window closes, rather than left to age out.
  //
  // The signed compare is what tolerates a mileStartMs stamped a millisecond
  // ahead of the nowMs a renderer was handed. On its own it is also a trap:
  // once the unsigned gap since the last milestone passes 2^31ms (24.85 days)
  // the cast goes negative and the test starts saying yes again, so the
  // flourish would be "drawn" on every frame for the next 24.85 days. Today
  // that is invisible only because renderMilestone clamps a negative age to
  // zero and paints black on black. Clearing the state closes the window for
  // good, and the unsigned test underneath catches a board that came back out
  // from under a modal screen long after the flourish was missed.
  {
    const int32_t signedAge = (int32_t)(nowMs - mileStartMs);
    if (signedAge >= 0 || signedAge < -1000) {
      if ((nowMs - mileStartMs) >= MILE_MS) {
        mileKind = MILE_NONE;
        return false;
      }
    }
  }
  if (firstRunActive) return false;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) return false;
  return true;
}

// --- the day ---------------------------------------------------------------

// The local wall clock right now, extrapolated from the last thing the host
// said. Only meaningful when gClockKnown.
static uint32_t localNow(uint32_t nowMs) {
  // Signed, and clamped at zero. gClockAtMs is stamped inside the parser with
  // its own millis(), so it can be a millisecond ahead of the nowMs a renderer
  // was handed; unsigned that wraps to 4.29 billion milliseconds and would
  // throw the clock forward by 49 days, which is a spurious midnight.
  int32_t since = (int32_t)(nowMs - gClockAtMs);
  if (since < 0) since = 0;
  return gClockLocal + (uint32_t)since / 1000u;
}

// Zero the daily figures. Called at local midnight and nowhere else: a reboot
// zeroes them by simply not loading any.
static void statsNewDay(int32_t day) {
  // The day that is ending may have been a record one. Banking it here as well
  // as incrementally is belt and braces: incrementally is what makes the
  // screen show a record the moment it happens, this is what makes it survive
  // a day that ends while nobody is looking.
  statsPromoteDay();
  stTurns   = 0;
  stTokens  = 0;
  stLongest = 0;
  mileTokStep = 0;
  gDayIdx   = day;
  saveStatsRecords();
  Serial.printf("stats: new day best_turn=%lus best_day=%lu\n",
                (unsigned long)stBestTurn, (unsigned long)stBestDay);
}

// Called every frame. Cheap: two integer divisions and a compare.
static void statsClockTick(uint32_t nowMs) {
  if (!gClockKnown) return;
  int32_t day = (int32_t)(localNow(nowMs) / DAY_SECONDS);
  if (gDayIdx < 0) {
    // The first clock this board has ever been told. Whatever it has counted
    // so far was counted today by definition, so the day is adopted rather
    // than starting a new one: a host that connects at lunchtime must not
    // throw away the morning it already watched.
    gDayIdx = day;
    return;
  }
  // Only a FORWARD step is a midnight. A backward one is a clock correction,
  // a timezone change, a laptop carried west across a date line, or the two
  // hosts on one cable that section 2.12 of the protocol already admits to,
  // and none of those are a reason to throw away the figures for a day
  // somebody is still in the middle of. A day index that went backwards is
  // adopted exactly the way the first one this board ever saw is.
  //
  // ...and a forward step gets the same plausibility test, because the forward
  // direction is the DESTRUCTIVE one and it had none at all. statsSetClock
  // accepts anything from 2020 to the year 2106, and a single implausible
  // value took the whole day with it: {"time":4294967294} on a board with
  // today's counters running answered `stats: new day` and zeroed turns,
  // tokens and longest. One bad line from a host bug (an uninitialised value,
  // a millisecond timestamp passed as seconds, a sign error) is not a reason
  // to throw away the day the owner has been watching accumulate. A real
  // midnight on a board whose clock is being kept by a host is a step of one:
  // localNow keeps ticking through every midnight on its own, so even a host
  // that disconnects for a week comes back to a day index that already
  // advanced. Anything past a couple of days is a bad value, so it is adopted
  // the way a backward one is rather than rolled over.
  if      (day > gDayIdx + CLOCK_FORWARD_DAY_MAX) gDayIdx = day;
  else if (day > gDayIdx) statsNewDay(day);
  else if (day < gDayIdx) gDayIdx = day;
}

// The "time" field. Ignored, without making the line malformed, for anything
// that is not a plausible wall clock.
static void statsSetClock(double localSeconds, uint32_t nowMs) {
  if (!(localSeconds >= (double)CLOCK_MIN_EPOCH)) return;   // NaN-safe
  if (localSeconds > 4294967295.0) return;
  gClockLocal = (uint32_t)localSeconds;
  gClockAtMs  = nowMs;
  gClockKnown = true;
  statsClockTick(nowMs);
}

// --- tokens ----------------------------------------------------------------

// The "tokens" field: the cumulative total of ONE Codex session, from a host
// that may not be the only host on this cable. See stTokLane above for why
// that matters and how a value is booked.
static void statsAddTokens(double v, uint32_t nowMs) {
  if (!(v >= 0.0)) return;                       // NaN-safe
  if (v > 4294967295.0) v = 4294967295.0;
  uint32_t cur = (uint32_t)v;

  // The lane this value belongs to: the one it is a plausible step above.
  int      best      = -1;
  uint32_t bestDelta = 0;
  for (uint8_t i = 0; i < TOKEN_LANES; ++i) {
    if (!stTokLaneUsed[i] || cur < stTokLane[i]) continue;
    uint32_t d = cur - stTokLane[i];
    if (d > TOKEN_LINE_MAX) continue;
    if (best < 0 || d < bestDelta) { best = (int)i; bestDelta = d; }
  }

  if (best < 0) {
    // No lane fits. A session this board has not heard from yet, or one that
    // has moved further than a single line can honestly account for. Either
    // way it is a baseline and contributes nothing: a board plugged in halfway
    // through a session did not watch those tokens happen and must not claim
    // them. A free lane first, then the least recently used one, because the
    // sessions still alive are the ones still sending.
    uint8_t slot = 0;
    for (uint8_t i = 0; i < TOKEN_LANES; ++i) {
      if (!stTokLaneUsed[i]) { slot = i; break; }
      if (stTokLaneUsed[slot] &&
          (int32_t)(stTokLaneAt[i] - stTokLaneAt[slot]) < 0) slot = i;
    }
    stTokLaneUsed[slot] = true;
    stTokLane[slot]     = cur;
    stTokLaneAt[slot]   = nowMs;
    return;
  }

  stTokLane[best]   = cur;
  stTokLaneAt[best] = nowMs;
  uint32_t delta    = bestDelta;
  if (delta == 0) return;

  // Saturating: 4.29 billion tokens in one day is not reachable, but a host
  // bug that sends a nonsense total must not wrap the counter to zero.
  if (stTokens > 0xFFFFFFFFu - delta) stTokens = 0xFFFFFFFFu;
  else                                stTokens += delta;

  statsPromoteDay();   // RAM now, flash at a turn

  // The ladder. A single line can cross more than one rung (a board that was
  // unplugged for an hour comes back to a big jump), and only the highest one
  // is worth marking: three pills in three seconds is the open-plan failure
  // this whole feature is bent around avoiding.
  uint8_t crossed = 0;
  bool any = false;
  while (mileTokStep < TOKEN_STEP_COUNT && stTokens >= TOKEN_STEPS[mileTokStep]) {
    crossed = mileTokStep;
    any = true;
    ++mileTokStep;
  }
  if (any) {
    char big[16];
    fmtTokenStep(big, sizeof(big), TOKEN_STEPS[crossed]);
    mileFire(nowMs, MILE_TOKENS, "TOKENS TODAY", big);
  }
}

// --- turns -----------------------------------------------------------------

// Called from the parser on every state CHANGE, before the change is applied,
// so the transition itself is what opens and closes a turn.
static void statsOnState(StateId ns, uint32_t nowMs) {
  if (ns == ST_BUSY || ns == ST_WAITING) {
    // A turn spans the whole of busy -> waiting -> busy, because that is one
    // thing the person asked for and one thing they waited on. Only the first
    // edge starts the clock.
    if (!stTurnOpen) { stTurnOpen = true; stTurnMs = nowMs; }
    return;
  }

  if (ns == ST_DONE) {
    // A `done` with no turn open is a keepalive or a host that started
    // mid-turn. Nothing to count, and counting it would invent a zero-second
    // turn on every repeat.
    if (!stTurnOpen) return;
    stTurnOpen = false;
    uint32_t secs = (nowMs - stTurnMs) / 1000u;
    ++stTurns;
    if (secs > stLongest) stLongest = secs;

    const bool record = (secs > stBestTurn && secs >= TURN_RECORD_MIN_S);
    if (record) stBestTurn = secs;
    statsPromoteDay();
    saveStatsRecords();     // the one place a turn can touch flash, and only
                            // when a record actually moved

    char dur[16];
    fmtDur(dur, sizeof(dur), secs);
    Serial.printf("stats: turn %s turns=%lu tokens=%lu longest=%lus best=%lus\n",
                  dur, (unsigned long)stTurns, (unsigned long)stTokens,
                  (unsigned long)stLongest, (unsigned long)stBestTurn);

    // One flourish per turn, and the better of the two wins. A record IS a
    // long turn as far as the person watching is concerned, so firing both
    // would be the same news twice.
    if      (record)              mileFire(nowMs, MILE_TURN_RECORD, "LONGEST YET", dur);
    else if (secs >= TURN_LONG_S) mileFire(nowMs, MILE_TURN_LONG, "LONG TURN", dur);
    return;
  }

  // idle or sleep. A turn that ends anywhere but `done` was abandoned (the
  // helper maps Interrupt and SessionEnd to idle), and an abandoned turn is
  // not a turn: counting it would reward walking away from a long one.
  stTurnOpen = false;
}

// The session clock. Opened by the first accepted line after a silence, closed
// when the device gives up on the host and goes back to ambient.
static void statsHostSpoke(uint32_t nowMs) {
  if (stSessionOpen) return;
  stSessionOpen = true;
  stSessionMs   = nowMs;
}

static void statsHostGone() {
  stSessionOpen = false;
  stTurnOpen    = false;
}

// --- the screen ------------------------------------------------------------

// Is the stats screen the thing on the panel this frame. It sits below every
// modal screen in exactly the order they sit below each other, and it is the
// one thing here that a pending question closes rather than covers: see
// statsTick.
static bool statsVisible(uint32_t nowMs) {
  if (!statsOn) return false;
  if (firstRunActive)    return false;
  if (sayVisible(nowMs)) return false;
  if (dndVisible(nowMs)) return false;
  if (toyVisible(nowMs)) return false;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) return false;
  return true;
}

static bool statsExit(uint32_t nowMs, const char* why) {
  if (!statsOn) return false;
  statsOn = false;
  saveStatsRecords();
  Serial.printf("stats: close (%s) turns=%lu tokens=%lu longest=%lus "
                "best_turn=%lus best_day=%lu\n",
                why, (unsigned long)stTurns, (unsigned long)stTokens,
                (unsigned long)stLongest, (unsigned long)stBestTurn,
                (unsigned long)stBestDay);
  // Handed back through the same 700ms cross-fade the device uses between
  // ambient and live, rather than cutting. Same as the toy.
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  return true;
}

// Opening. Refused while a prompt is pending, for the same reason the toy and
// the first-run replay are: a screen that covers a question is the one thing
// this device must not do. The refusal is what lets the button fall back to
// its old brightness step rather than doing nothing at all.
static bool statsEnter(uint32_t nowMs, const char* why) {
  if (statsOn) return false;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    Serial.println("err: stats refused, prompt pending");
    return false;
  }
  statsOn      = true;
  statsInputMs = nowMs;
  // Everything the panel is about to show, plus the two flags that explain a
  // figure that looks wrong: whether a turn is being timed right now, and
  // whether the session clock is running at all. Nobody can read a panel over
  // USB, and "why does it say zero turns" is the question this notice exists
  // to answer without one.
  Serial.printf("stats: open (%s) turns=%lu tokens=%lu longest=%lus "
                "session=%lus best_turn=%lus best_day=%lu day=%s turn=%s "
                "link=%s\n",
                why, (unsigned long)stTurns, (unsigned long)stTokens,
                (unsigned long)stLongest,
                (unsigned long)(stSessionOpen ? (nowMs - stSessionMs) / 1000u : 0u),
                (unsigned long)stBestTurn, (unsigned long)stBestDay,
                gClockKnown ? "today" : "since-boot",
                stTurnOpen ? "running" : "none",
                stSessionOpen ? "up" : "down");
  // Asking to see the numbers is an interaction, exactly like acknowledging a
  // prompt or starting a game, so it wakes a panel the auto ladder had dimmed.
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);
    dimmedByAuto = false;
    brightManual = true;
  }
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  return true;
}

// Checked every frame, whether or not the screen is the thing being drawn, so
// neither of the two ways it has to end can be delayed by a message card
// sitting on top of it. Exactly the shape of toyTick, and for the same reason.
static void statsTick(uint32_t nowMs) {
  if (!statsOn) return;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    statsExit(nowMs, "prompt");
    return;
  }
  if ((int32_t)(nowMs - statsInputMs) >= (int32_t)STATS_IDLE_MS) {
    statsExit(nowMs, "idle");
  }
}

// ---------------------------------------------------------------------------
// FOCUS: everything except the drawing
//
// The drawing is renderFocus and renderFocusTick, further down with the other
// compositions, because both need the palette and the cross-fade.
// ---------------------------------------------------------------------------
static uint16_t focusMins   = FOCUS_DEFAULT_MIN;
static uint16_t focusSaved  = FOCUS_DEFAULT_MIN;  // what is actually on flash
static bool     focusOn     = false;   // the screen is open
static bool     focusRun    = false;   // the clock is running
static uint32_t focusEndMs  = 0;       // the deadline, valid while focusRun
static uint32_t focusLeftMs = 0;       // remaining ms, valid while !focusRun
static uint32_t focusOpenMs = 0;       // when the screen was opened
// The last time a person touched this screen. Separate from focusOpenMs on
// purpose: that one drives the brightness ladder and must not be pushed
// forward by every press, this one drives the idle close and must be.
static uint32_t focusInputMs = 0;
static bool     focusFin    = false;   // it ended and the finish is still owed
static uint32_t focusFinMs  = 0;       // when the finish first became VISIBLE

static inline uint32_t focusFullMs() { return (uint32_t)focusMins * 60000u; }

// Signed, like every other subtraction of a stamp in this file: focusEndMs is
// set from a millis() taken inside the parser or a button pump, so it can be a
// millisecond ahead of the nowMs a renderer was handed, and unsigned that
// wraps to 4.29 billion.
static uint32_t focusRemainMs(uint32_t nowMs) {
  if (!focusRun) return focusLeftMs;
  int32_t d = (int32_t)(focusEndMs - nowMs);
  return d <= 0 ? 0u : (uint32_t)d;
}

// Stopped with nothing spent: the state a fresh board and a just-reset board
// are both in.
static inline bool focusAtRest() {
  return !focusRun && focusLeftMs >= focusFullMs();
}

// A run somebody started and has neither finished nor thrown away. This is
// what the hairline on the base compositions is about: it is drawn for a
// paused pomodoro too, dimmer, because a timer you forgot you paused is
// exactly the thing worth a two pixel reminder.
static inline bool focusLive() {
  return focusRun || (focusLeftMs > 0 && !focusAtRest());
}

// "25:00", and "180:00" for the longest a host may ask for. Rounded UP, so a
// freshly started twenty-five minute timer reads 25:00 rather than 24:59, and
// 0:00 appears only at the instant it is actually over.
static void fmtClock(char* out, size_t cap, uint32_t ms) {
  uint32_t s = (ms + 999u) / 1000u;
  snprintf(out, cap, "%lu:%02lu", (unsigned long)(s / 60u),
           (unsigned long)(s % 60u));
}

// --- the chosen length in NVS ----------------------------------------------

static void loadFocusPrefs() {
  focusMins = FOCUS_DEFAULT_MIN;
  Preferences prefs;
  // A board nobody has changed the length on has no key, which is the normal
  // path on a fresh unit and not an error. Same as the hop best.
  if (prefs.begin(NVS_NAMESPACE, true)) {
    uint16_t v = prefs.getUShort(NVS_KEY_FOCUSMIN, FOCUS_DEFAULT_MIN);
    prefs.end();
    if (v >= FOCUS_MIN_MIN && v <= FOCUS_MAX_MIN) focusMins = v;
  }
  focusSaved  = focusMins;
  focusLeftMs = focusFullMs();
}

// Idempotent, exactly like setOwnerName and setFaceIndex: flash is touched
// only when the value actually differs from what is stored, so a host that put
// "focusmins" in a 2000ms heartbeat would not write to it 43,000 times a day.
static void saveFocusMins() {
  if (focusMins == focusSaved) return;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("err: nvs open failed, focus length not persisted");
    return;
  }
  prefs.putUShort(NVS_KEY_FOCUSMIN, focusMins);
  prefs.end();
  focusSaved = focusMins;
}

// --- the screen ------------------------------------------------------------

// Everything that outranks the timer's screen, in exactly the order those
// screens outrank each other, plus the one rule that cuts across all of them.
// The last line is the answer to "what wins when a timer is running and the
// agent starts waiting on the human": the question does, and it is not close.
static bool focusBlocked(uint32_t nowMs) {
  if (firstRunActive)      return true;
  if (sayVisible(nowMs))   return true;
  if (dndVisible(nowMs))   return true;
  if (toyVisible(nowMs))   return true;
  if (statsVisible(nowMs)) return true;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) return true;
  return false;
}

// The finish forces itself onto the panel even when nobody opened the screen,
// which is the one thing here that is not "a screen you asked for". That is
// the point of a timer.
static bool focusVisible(uint32_t nowMs) {
  if (!focusOn && !focusFin) return false;
  return !focusBlocked(nowMs);
}

// Any press on the timer screen restarts its idle close, so a person reading a
// stopped clock is never cut off mid-read. Deliberately NOT called from the
// host paths: a heartbeat carrying "focus" must not be able to hold a screen
// open on somebody's desk forever, which is the same rule the toy's hop now
// follows.
static inline void focusTouch(uint32_t nowMs) { focusInputMs = nowMs; }

// Asking for the timer is an interaction, exactly like acknowledging a prompt
// or opening the stats screen, so it wakes a panel the auto ladder had dimmed.
static void focusWake() {
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);
    dimmedByAuto = false;
    brightManual = true;
  }
}

// Handed over through the same 700ms cross-fade the device uses between
// ambient and live, rather than cutting. Same as the toy and the stats screen.
static void focusHandBack(uint32_t nowMs) {
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
}

// Opening. Refused while a prompt is pending, for the same reason the toy, the
// stats screen and the first-run replay are. Note what is NOT refused: the
// clock. A question owns the panel, not the passage of time.
static bool focusShow(uint32_t nowMs, const char* why) {
  if (focusOn) return false;
  if (!ambientMode && effectiveState(nowMs) == ST_WAITING) {
    Serial.println("err: focus refused, prompt pending");
    return false;
  }
  focusOn      = true;
  focusOpenMs  = nowMs;
  focusInputMs = nowMs;
  char left[16];
  fmtClock(left, sizeof(left), focusRemainMs(nowMs));
  Serial.printf("focus: show (%s) len=%um left=%s %s\n", why,
                (unsigned)focusMins, left, focusRun ? "running" : "stopped");
  focusWake();
  focusHandBack(nowMs);
  return true;
}

static bool focusHide(uint32_t nowMs, const char* why) {
  if (!focusOn) return false;
  focusOn = false;
  char left[16];
  fmtClock(left, sizeof(left), focusRemainMs(nowMs));
  Serial.printf("focus: hide (%s) left=%s %s\n", why, left,
                focusRun ? "running" : "stopped");
  focusHandBack(nowMs);
  return true;
}

// --- the clock -------------------------------------------------------------

static void focusStart(uint32_t nowMs, const char* why) {
  // Starting retires an unread finish. A person who starts the next pomodoro
  // has plainly seen the end of the last one.
  focusFin   = false;
  focusFinMs = 0;
  if (focusRun) return;
  if (focusLeftMs == 0) focusLeftMs = focusFullMs();
  focusEndMs = nowMs + focusLeftMs;
  focusRun   = true;
  char left[16];
  fmtClock(left, sizeof(left), focusLeftMs);
  Serial.printf("focus: start (%s) len=%um left=%s\n", why,
                (unsigned)focusMins, left);
}

static void focusPause(uint32_t nowMs, const char* why) {
  if (!focusRun) return;
  focusLeftMs = focusRemainMs(nowMs);
  focusRun    = false;
  char left[16];
  fmtClock(left, sizeof(left), focusLeftMs);
  Serial.printf("focus: pause (%s) left=%s\n", why, left);
}

static void focusReset(uint32_t nowMs, const char* why) {
  focusRun    = false;
  focusLeftMs = focusFullMs();
  focusFin    = false;
  focusFinMs  = 0;
  (void)nowMs;
  Serial.printf("focus: reset (%s) len=%um\n", why, (unsigned)focusMins);
}

static void focusToggle(uint32_t nowMs, const char* why) {
  if (focusRun) focusPause(nowMs, why);
  else          focusStart(nowMs, why);
}

// The length. `resetNow` is the button's half: a hold on button 2 is a
// deliberate act on a stopped timer, so it starts the new length whole. The
// host's half does not, so a "focusmins" in a heartbeat cannot quietly rewind
// a pomodoro somebody paused to take a call.
static bool focusSetMins(uint16_t m, bool resetNow, const char* why) {
  if (m < FOCUS_MIN_MIN) m = FOCUS_MIN_MIN;
  if (m > FOCUS_MAX_MIN) m = FOCUS_MAX_MIN;
  const bool wasAtRest = focusAtRest();
  const bool changed   = (m != focusMins);
  focusMins = m;
  saveFocusMins();
  const bool applied = !focusRun && (resetNow || wasAtRest);
  if (applied) focusLeftMs = focusFullMs();
  if (changed) {
    Serial.printf("focus: length (%s) %um%s\n", why, (unsigned)focusMins,
                  applied ? "" : ", from the next reset");
  }
  return changed;
}

// Button 2's hold on the timer screen: the next rung, wrapping. Refused while
// the clock is RUNNING, so the gesture can never cost somebody a pomodoro; a
// refusal still consumes the hold, which is what stops the release that
// follows it from also closing the screen.
//
// A length the host set off the ladder (say 7) matches no rung, and the cycle
// lands on the first one rather than nowhere: that is how a button gets a
// board back onto the ladder with no host in the room.
static bool focusCycleLength(uint32_t nowMs, const char* why) {
  if (focusRun) {
    Serial.println("err: focus length locked, timer is running");
    return false;
  }
  if (focusFin) {
    // A hold that was in flight when the countdown reached zero. It lands here
    // because a finished run is not at rest either (focusLeftMs is 0), and it
    // used to be answered with the sentence below, which says "run is paused"
    // about a run that has finished. Same window as the button 1 hold that
    // used to retire an unseen finish; the swallowed release still closes the
    // screen, exactly as 2 CLOSE promises.
    Serial.println("err: focus length locked, the run just finished");
    return false;
  }
  if (!focusAtRest()) {
    // Paused part way through. The legend on that screen reads
    // "1 RESUME  2 CLOSE  HOLD 1 RESET": there is no room for a fourth item,
    // so this hold is not advertised there, and an unadvertised gesture must
    // not quietly change a stored setting whose effect would appear at some
    // later reset with nothing on the panel to show for it now. Refused, and
    // the swallowed release closes the screen exactly as 2 CLOSE promises.
    Serial.println("err: focus length locked, run is paused");
    return false;
  }
  uint8_t i = 0;
  while (i < FOCUS_LENGTH_COUNT && FOCUS_LENGTHS[i] != focusMins) ++i;
  const uint16_t next = (i >= FOCUS_LENGTH_COUNT)
                          ? FOCUS_LENGTHS[0]
                          : FOCUS_LENGTHS[(i + 1) % FOCUS_LENGTH_COUNT];
  // resetNow only when the timer is genuinely at rest. Paused is not at rest:
  // focusRun is false there, so passing `true` unconditionally applied the new
  // length immediately and threw away the elapsed time of a run somebody had
  // deliberately paused. focusSetMins already knows how to hold a change back
  // ("from the next reset"); this is where that case gets selected.
  focusSetMins(next, focusAtRest(), why);
  (void)nowMs;
  return true;
}

// The finish is over: put the timer back at the top of its length, ready for
// the next one, and hand the screen back through the cross-fade. The screen
// stays open if it was open, so somebody watching the countdown end is looking
// at a fresh 25:00 rather than being thrown back to the face.
static void focusFinishClear(uint32_t nowMs, const char* why) {
  if (!focusFin) return;
  focusFin    = false;
  focusFinMs  = 0;
  focusLeftMs = focusFullMs();
  Serial.printf("focus: end (%s)\n", why);
  focusHandBack(nowMs);
}

// Checked every frame, whether or not the timer is the thing being drawn,
// exactly like toyTick and statsTick and for a stronger reason: this is the
// clock itself, and a clock that only runs while somebody is looking at it is
// not a clock.
static void focusTick(uint32_t nowMs) {
  if (focusRun && focusRemainMs(nowMs) == 0) {
    focusRun    = false;
    focusLeftMs = 0;
    focusFin    = true;
    focusFinMs  = 0;
    Serial.printf("focus: done %um\n", (unsigned)focusMins);
  }
  if (!focusFin) {
    // The screen nobody asked to keep. Only ever an at-rest timer: focusAtRest
    // is false for a run that is going and for one that is paused part way
    // through, so neither can be closed out from under the person who started
    // it. The clock is not touched either way; only the screen is.
    if (focusOn && !focusRun && focusAtRest() &&
        (int32_t)(nowMs - focusInputMs) >= (int32_t)FOCUS_IDLE_MS) {
      focusHide(nowMs, "idle");
    }
    return;
  }
  // The finish's own thirty seconds do not start until it can actually be
  // SEEN. See the header comment: this is the one thing on the device that
  // waits rather than being missed.
  if (focusBlocked(nowMs)) return;
  if (focusFinMs == 0) {
    focusFinMs = nowMs;
    focusWake();
    focusHandBack(nowMs);
    return;
  }
  if ((int32_t)(nowMs - focusFinMs) >= (int32_t)FOCUS_END_MS) {
    focusFinishClear(nowMs, "elapsed");
  }
}

// The factory reset's half. prefs.clear() has already taken the stored length
// with everything else in the namespace, so this brings the RAM mirror back to
// what a virgin board would load and stops whatever was counting.
static void focusForget() {
  focusOn     = false;
  focusRun    = false;
  focusFin    = false;
  focusFinMs  = 0;
  focusMins   = FOCUS_DEFAULT_MIN;
  focusSaved  = FOCUS_DEFAULT_MIN;
  focusLeftMs = focusFullMs();
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

  // Two fields carry enums, and either can name something that does not exist.
  // Both are validated here, BEFORE anything is written, so a line with a host
  // typo in either is dropped whole rather than half applied. Everything past
  // this block is infallible.
  JsonVariant vState = doc["state"];
  const bool haveState = !vState.isNull() && vState.is<const char*>();
  StateId ns = st.state;
  if (haveState) {
    bool nameOk = false;
    ns = parseStateName(vState.as<const char*>(), &nameOk);
    // An unrecognised name is a host bug, not a keepalive: drop the whole line
    // silently rather than acking it while showing the previous state.
    if (!nameOk) return false;
  }

  JsonVariant vFace = doc["face"];
  const bool haveFace = !vFace.isNull() && vFace.is<const char*>();
  int faceIdx = -1;
  if (haveFace) {
    faceIdx = faceIndexByName(vFace.as<const char*>());
    if (faceIdx < 0) return false;           // same rule as an unknown state
  }

  // "reset" wipes stored state, "firstrun" replays the out-of-the-box
  // sequence. Both are optional, both are enums, and both follow the same rule
  // as "state" and "face": a value outside the list makes the WHOLE line
  // malformed. A host that means "factory" and writes "Factory" must not have
  // its typo silently acked as though something happened.
  JsonVariant vReset = doc["reset"];
  const bool haveReset = !vReset.isNull() && vReset.is<const char*>();
  bool resetFactory = false;
  if (haveReset) {
    const char* r = vReset.as<const char*>();
    if      (!strcmp(r, "factory"))  resetFactory = true;
    else if (!strcmp(r, "firstrun")) resetFactory = false;
    else return false;
  }

  JsonVariant vFirst = doc["firstrun"];
  const bool haveFirst = !vFirst.isNull() && vFirst.is<const char*>();
  if (haveFirst && strcmp(vFirst.as<const char*>(), "play") != 0) return false;

  // The toy. Three values, so it is a string enum rather than the boolean
  // "dnd" is, and it follows the same rule every other enum on this wire
  // follows: a value outside the list makes the WHOLE line malformed.
  //
  // "hop" is the only field here that exists partly for the bench. There is no
  // finger inside a test harness, so it is the only way the game itself can be
  // exercised over the cable; a host that wants to play from a keyboard gets
  // that for free. The shipped helper sends none of the three.
  JsonVariant vPlay = doc["play"];
  const bool havePlay = !vPlay.isNull() && vPlay.is<const char*>();
  uint8_t playOp = 0;   // 1 on, 2 off, 3 hop
  if (havePlay) {
    const char* p = vPlay.as<const char*>();
    if      (!strcmp(p, "on"))  playOp = 1;
    else if (!strcmp(p, "off")) playOp = 2;
    else if (!strcmp(p, "hop")) playOp = 3;
    else return false;
  }

  // The stats screen. The second door to a feature whose first door is a
  // button tap, exactly like "dnd" and "play", and validated up here with the
  // other enums so a typo drops the whole line rather than being acked.
  JsonVariant vStats = doc["stats"];
  const bool haveStats = !vStats.isNull() && vStats.is<const char*>();
  uint8_t statsOp = 0;   // 1 on, 2 off
  if (haveStats) {
    const char* p = vStats.as<const char*>();
    if      (!strcmp(p, "on"))  statsOp = 1;
    else if (!strcmp(p, "off")) statsOp = 2;
    else return false;
  }

  // The focus timer. Five verbs, so a string enum like "play" and "stats", and
  // the same rule every enum on this wire follows: a value outside the list
  // makes the WHOLE line malformed rather than being acked while nothing
  // happens.
  JsonVariant vFocus = doc["focus"];
  const bool haveFocus = !vFocus.isNull() && vFocus.is<const char*>();
  uint8_t focusOp = 0;   // 1 show, 2 hide, 3 start, 4 pause, 5 reset
  if (haveFocus) {
    const char* p = vFocus.as<const char*>();
    if      (!strcmp(p, "show"))  focusOp = 1;
    else if (!strcmp(p, "hide"))  focusOp = 2;
    else if (!strcmp(p, "start")) focusOp = 3;
    else if (!strcmp(p, "pause")) focusOp = 4;
    else if (!strcmp(p, "reset")) focusOp = 5;
    else return false;
  }

  JsonVariant v;

  // Applied FIRST, so one line can wipe a board and then rename it:
  //   {"reset":"factory","name":"Alex Rivera"}
  // ends up blank-then-named rather than named-then-blank. The reply line is
  // for scripts; the "ok" below still goes out exactly as it does for any
  // other accepted line.
  if (haveReset) {
    if (resetFactory) {
      Serial.println(factoryReset() ? "reset: factory ok" : "reset: factory ram-only");
    } else {
      Serial.println(armFirstRun() ? "reset: firstrun ok" : "reset: firstrun ram-only");
    }
    touched = true;
  }

  if (haveState) {
    if (ns != st.state) {
      // Turns are counted off the state machine's own edges, before the change
      // lands, because the transition IS the event: busy opens a turn and done
      // closes one. Nothing about the state machine changes to make this work.
      statsOnState(ns, millis());
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

  // Which creature is on the panel. Applied immediately and persisted to NVS,
  // so like "name" this one outlives the frame. Unlike "name" it takes effect
  // on the very next rendered frame, mid-animation, with no reboot.
  if (haveFace) {
    setFaceIndex((uint8_t)faceIdx);
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

  // The wall clock, in LOCAL seconds: unix time plus the host's own UTC
  // offset. Local rather than UTC because the only question this device asks
  // of it is "has midnight happened where the owner is sitting", and a device
  // with no timezone database cannot answer that from UTC. A host that resends
  // it every frame keeps daylight saving correct for free.
  //
  // Read as a double, not a float: 2026 in unix seconds is 1.79e9 and a
  // 24-bit float mantissa quantises that to steps of 128 seconds, which is
  // fine for a day index and not fine for anything else that might want it.
  // Optional, and it can never make a line malformed: a value that is not a
  // plausible wall clock is ignored exactly as a wrong-typed "ring" is.
  v = doc["time"];
  if (!v.isNull() && v.is<double>()) {
    statsSetClock(v.as<double>(), millis());
    touched = true;
  }

  // The host session's cumulative token total. Optional; without it every
  // other figure on the stats screen still counts and the token ones stay at
  // zero. Differences are banked, never the value itself: see statsAddTokens.
  // Applied after "time" so a line that crosses midnight resets the day before
  // this adds to it, rather than adding to the day that just ended.
  v = doc["tokens"];
  if (!v.isNull() && v.is<double>()) {
    statsAddTokens(v.as<double>(), millis());
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

  // The message card. "say" is the text, "saysecs" how long it holds.
  //
  // Neither can make a line malformed. The text is sanitised rather than
  // rejected (see saySanitize) and the duration is clamped like every other
  // number on the wire, so the worst a host typo can do here is show fewer
  // words or hold them for a different length of time.
  //
  // "saysecs" on its own re-times the card that is already up, which is how a
  // host extends "in a meeting" without making it fade in and out again. With
  // no card up it is accepted and does nothing, exactly like a "ring" value
  // that happens to match the current one.
  JsonVariant vSay  = doc["say"];
  JsonVariant vSecs = doc["saysecs"];
  const bool haveSay  = !vSay.isNull()  && vSay.is<const char*>();
  const bool haveSecs = !vSecs.isNull() && vSecs.is<float>();
  uint32_t saySecs = SAY_DEFAULT_S;
  if (haveSecs) {
    float s = vSecs.as<float>();
    // Zero is the sentinel for "hold until something clears it", so a NEGATIVE
    // duration must not clamp into it: that would turn a host arithmetic bug
    // into a card that never leaves. Below zero is nonsense, and nonsense is
    // treated as though the field had not been sent at all.
    saySecs = (s < 0.0f) ? SAY_DEFAULT_S
                         : (uint32_t)clampf(s, 0.0f, (float)SAY_MAX_S);
  }
  if (haveSay) {
    setSay(vSay.as<const char*>(), saySecs);
    touched = true;
  } else if (haveSecs) {
    retimeSay(saySecs);
    touched = true;
  }

  // The do-not-disturb sign. Optional, boolean, and it can never make a line
  // malformed: unlike "state" and "face" a boolean has no typo space, so a
  // wrong-typed value is ignored exactly as a wrong-typed "ring" is and the
  // rest of the line still applies.
  //
  // The feature is a button gesture first and this is the second door to it,
  // the same way "face" and "firstrun" mirror their holds. An older host that
  // never sends it is unaffected, and the shipped helper never sends it.
  JsonVariant vDnd = doc["dnd"];
  if (!vDnd.isNull() && vDnd.is<bool>()) {
    if (vDnd.as<bool>()) dndEnter(millis(), "host");
    else                 dndExit(millis(), "host");
    touched = true;
  }

  // The toy. Like "dnd" this is the second door to a feature whose first door
  // is a button, and like "dnd" it changes no state, no clock and no face:
  // it takes the screen and hands it straight back. "on" is refused while a
  // prompt is pending, inside toyEnter, and a "hop" with no game up is
  // accepted and does nothing, exactly like a "saysecs" with no card up.
  if (havePlay) {
    if      (playOp == 1) toyEnter(millis(), "host");
    else if (playOp == 2) toyExit(millis(), "host");
    else                  toyHop(millis(), /*fromButton=*/false);
    touched = true;
  }

  // The stats screen, same shape as the toy: it takes the screen and hands it
  // straight back, changes no state and no clock, and "on" is refused while a
  // prompt is pending inside statsEnter, so the gesture and the field cannot
  // disagree about when it is safe.
  if (haveStats) {
    if (statsOp == 1) statsEnter(millis(), "host");
    else              statsExit(millis(), "host");
    touched = true;
  }

  // The focus timer's length, in minutes. A NUMBER, so it takes the "ring"
  // rule and not the enum rule: a wrong type is ignored and the rest of the
  // line still applies, and a value outside 1..180 is clamped rather than
  // making the line malformed.
  //
  // Applied BEFORE the verb below, so {"focusmins":45,"focus":"start"} starts
  // a forty-five minute timer rather than starting the old length and then
  // changing the number underneath it. It never rewinds a run in flight: see
  // focusSetMins.
  v = doc["focusmins"];
  if (!v.isNull() && v.is<float>()) {
    float m = clampf(v.as<float>(), (float)FOCUS_MIN_MIN, (float)FOCUS_MAX_MIN);
    focusSetMins((uint16_t)lroundf(m), false, "host");
    touched = true;
  }

  // The timer itself. The second door to a feature whose first door is the two
  // buttons, exactly like "dnd", "play" and "stats".
  //
  // "start" also opens the screen, because that is what the button does and a
  // host should not have to send two lines to get one gesture. The two halves
  // are refused separately and deliberately: THE CLOCK IS NEVER REFUSED, the
  // SCREEN is, so a "start" during a pending prompt begins the countdown and
  // leaves the question on the panel.
  if (haveFocus) {
    switch (focusOp) {
      case 1: focusShow(millis(), "host"); break;
      case 2: focusHide(millis(), "host"); break;
      case 3: focusStart(millis(), "host"); focusShow(millis(), "host"); break;
      case 4: focusPause(millis(), "host"); break;
      default: focusReset(millis(), "host"); break;
    }
    touched = true;
  }

  // Applied LAST, because it takes the screen for 9.25 seconds and everything
  // above should already be in place when it settles out of the sequence.
  //
  // A replay never touches the stored flag, so a script can rehearse on an
  // armed board without spending it. It is refused, loudly, while a turn is
  // running or a prompt is pending: a host that asks for a demo in the middle
  // of its own approval prompt is asking for the one thing this device must
  // not do, which is hide a question from the person who has to answer it.
  if (haveFirst) {
    if (!safeToInterrupt(millis())) {
      Serial.println("err: firstrun refused, session is active");
    } else {
      startFirstRun(millis(), false);
    }
    touched = true;
  }

  // An object with no recognised field is still well-formed JSON: accept it
  // as a keepalive so the host can hold the link open with "{}".
  (void)touched;
  return true;
}

// How long one pumpSerial call may spend before it has to give the frame back.
//
// The drain used to be `while (Serial.available() > 0)` with no ceiling on
// lines, bytes or time, and a host writing faster than the device parses never
// gave control back at all. Measured on the board with a -DFPS_DEBUG build:
// twelve seconds of continuous ~55 byte lines took the panel from a locked
// 30.3fps to 1.0fps, twelve rendered frames where 364 were due, and NOT ONE
// fps line for the whole flood. Everything that ticks inside renderFrame
// stalled with it: a {"say":...,"saysecs":2} card set at t=0 did not report
// `say: expired` until 10.09s, 0.09s after a 10 second flood stopped. The same
// stall reaches the toy's walk-away close, the stats screen's 20s close, the
// midnight roll and the pomodoro countdown.
//
// The shipped helper cannot produce this (one line per hook plus a 2000ms
// heartbeat) and the device self-heals the instant the flood stops, so nothing
// is corrupted. But a buggy host must not be able to freeze the panel, so the
// budget is a ceiling and not a target: at 115200 baud four milliseconds is
// about 46 bytes, and loop() drains twice per iteration and again after every
// frame, which outruns any legitimate host by orders of magnitude.
static constexpr uint32_t SERIAL_PUMP_MS = 4;

static void pumpSerial() {
  const uint32_t pumpStart = millis();
  while (Serial.available() > 0) {
    // Tested per byte rather than per line, so a host that never sends a
    // newline at all cannot hold the loop either. A line already half read
    // keeps its bytes in lineBuf and resumes on the next call.
    if (millis() - pumpStart >= SERIAL_PUMP_MS) break;
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (lineLen > 0 && !lineOver) {
        lineBuf[lineLen] = '\0';
        if (applyJsonLine(lineBuf, lineLen)) {
          lastDataMs    = millis();
          hostEverSpoke = true;
          // The session clock starts on the first accepted line after a
          // silence, which is the same edge that takes the device out of
          // ambient. It is measured here rather than told, because "how long
          // have I been at this" is a question about the desk and not about
          // any one Codex session.
          statsHostSpoke(millis());
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
// The drawing moved out. What is left here is everything that is NOT a face:
// the blink driver, the glance driver, and the two geometry descriptions that
// get handed to whichever face is active. The face itself is one virtual call
// at the end of each renderer.
//
// The default "rounded" design, its geometry constants and its per-state
// expressions all moved to src/FaceRounded.cpp verbatim. src/Face.hpp is the
// interface, src/Faces.cpp the registry, and firmware/FACES.md the guide to
// writing another one.
// ---------------------------------------------------------------------------

// The colour transform every face runs its own palette through. Passing it as
// a function pointer rather than letting faces call xf() directly is what
// keeps a face free of globals.
static FaceCanvas faceCanvas() {
  FaceCanvas c;
  c.g    = gfx;
  c.tint = &xf;
  return c;
}

// Ambient: a big face centred on the panel, owning everything above the two
// text lines. The burn-in drift is baked in here, so no face ever applies it.
static FaceGeometry ambientGeometry() {
  FaceGeometry g;
  g.cx   = FACE_CX_AMB + gOx;
  g.cy   = FACE_CY_AMB + gOy;
  g.eyeW = EYE_W_AMB;
  g.eyeH = EYE_H_AMB;
  g.gap  = EYE_GAP_AMB;
  g.boxX = FACE_BOX_X_AMB + gOx;
  g.boxY = FACE_BOX_Y + gOy;
  g.boxW = FACE_BOX_W_AMB;
  g.boxH = FACE_BOX_H;
  return g;
}

// Live: a smaller face on the left, the ring on the right. gOx/gOy are zero
// once a live frame has settled and non-zero only while the cross-fade out of
// ambient is unwinding the drift, which is why they are added here too.
static FaceGeometry liveGeometry() {
  FaceGeometry g;
  g.cx   = FACE_CX_LIVE + gOx;
  g.cy   = FACE_CY_LIVE + gOy;
  g.eyeW = EYE_W_LIVE;
  g.eyeH = EYE_H_LIVE;
  g.gap  = EYE_GAP_LIVE;
  g.boxX = FACE_BOX_X_LIVE + gOx;
  g.boxY = FACE_BOX_Y + gOy;
  g.boxW = FACE_BOX_W_LIVE;
  g.boxH = FACE_BOX_H;
  return g;
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
  // Signed, for the reason spelled out over ambientActive: lastDataMs can be a
  // millisecond ahead of the nowMs this was called with, and unsigned that
  // wraps to 4.29 billion and reads as a dead host. Here it cost one frame of
  // busy or waiting rendered as idle, which is invisible at 30fps and wrong
  // anyway.
  int32_t sinceData = (int32_t)(nowMs - lastDataMs);
  // Past the 30s dim mark the host is gone but not yet declared dead. Holding
  // the escalated red "waiting" pulse there would keep demanding attention for
  // an approval prompt that no longer exists, so the two active states fall
  // back to the idle look. The stored st.state is untouched: one fresh line
  // restores the real look immediately.
  if (sinceData >= (int32_t)STALE_DIM_MS &&
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

  // What the face is handed, filled in by the state below. Defaults are the
  // idle look. Note that these are INPUTS to a face, not a description of one:
  // nothing here says rectangle, arc or creature.
  uint16_t faceColor = C_IDLE;   // accent for the face, levelled per state
  float    pulse     = 0.0f;     // the ring's own 0..1 breathe this frame
  bool     escalated = false;    // waiting only
  uint32_t stateAge  = nowMs - stateEnterMs;

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
      pulse    = b;
      // C_SLEEP is #26314A, which at the sleep backlight tier is very close to
      // invisible. The lid is lifted toward the text colour so a closed eye
      // still reads as a closed eye rather than as a blank screen.
      faceColor = scaleColor(mixColor(C_SLEEP, C_TEXT, 0.42f),
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
      pulse     = b;
      faceColor = scaleColor(C_IDLE, 0.80f + 0.20f * b);
      glanceOK  = true;
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
      // Concentrating: lids down to a squint. Whatever a scanning gaze looks
      // like is the face's business; all this decides is how shut the lids are
      // and that the blink slows down.
      lidScale  = 0.60f;
      pulse     = b;
      faceColor = scaleColor(C_BUSY, 0.70f + 0.30f * b);
      gapLo     = BLINK_GAP_BUSY_MIN;
      gapHi     = BLINK_GAP_BUSY_MAX;
      break;
    }
    case ST_WAITING: {
      // How long the prompt has been up, not how long st.state has been set:
      // the escalation and the face both key off the waiting edge.
      stateAge = nowMs - waitEnterMs;
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
        // Still wide, because a prompt is still pending. Everything else the
        // ack changes is the face's own reading of `acked`.
        lidScale  = 1.10f;
        pulse     = 0.0f;
        faceColor = scaleColor(C_WAIT, 0.78f);
        break;
      }
      // Amber pulse; faster with tps, escalates to red after 10s.
      uint32_t waited = stateAge;
      escalated       = waited >= WAIT_ESCALATE_MS;
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
      lidScale  = escalated ? 1.28f : 1.18f;
      pulse     = b;
      faceColor = scaleColor(accent, 0.45f + 0.55f * b);
      gapLo     = BLINK_GAP_WAIT_MIN;
      gapHi     = BLINK_GAP_WAIT_MAX;
      break;
    }
    case ST_DONE: {
      // Measured from the "done" edge, which is what arms the flash, rather
      // than from the last state change of any kind.
      stateAge = nowMs - doneEnterMs;
      uint32_t since = stateAge;
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
        // The blink is suppressed for exactly as long as the face is smiling,
        // which is why DONE_HAPPY_MS is shared with it rather than private.
        blinkOK       = false;
        faceColor     = C_DONE;
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

  FaceFrame ff;
  ff.nowMs      = nowMs;
  ff.dtMs       = dtMs;
  ff.stateAgeMs = stateAge;
  ff.state      = eff;
  ff.ambient    = false;
  ff.escalated  = escalated;
  ff.acked      = waitAcked;
  ff.open       = lidScale * blinkLid;
  ff.pulse      = pulse;
  ff.gazeX      = glanceX;
  ff.gazeY      = glanceY;
  ff.color      = faceColor;
  ff.geom       = liveGeometry();

  FaceCanvas fc = faceCanvas();
  faceAt(gFaceIdx)->draw(fc, ff);

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
  // One breathe, used twice: it dims the whole face and it is handed to the
  // face as `pulse` so anything the face animates lands on the same beat.
  float    breath = breathe(nowMs, AMB_BREATHE_MS);
  float    level  = 0.55f + 0.40f * breath;

  // The comet ring used to live here. It is gone from ambient: the face is
  // 148px wide and the ring was 104px across in the same place, and shrinking
  // either one to fit made both worse. The ring is not lost, it just belongs
  // to live mode now, where it carries real numbers. Everything the comet was
  // here to do (never a still frame, no error message, no dead-looking object)
  // the face does better, because the motion means something.
  float blinkLid = updateBlink(nowMs, BLINK_GAP_MIN, BLINK_GAP_MAX, true);
  updateGlance(nowMs, dtMs, true);

  // Ambient has no protocol state of its own, so the face is handed the
  // resting one and told it is in ambient. What a face does with that (the
  // default one adds a slow height swell on the breathe) is its business.
  FaceFrame ff;
  ff.nowMs      = nowMs;
  ff.dtMs       = dtMs;
  ff.stateAgeMs = nowMs - ambientEnterMs;
  ff.state      = ST_IDLE;
  ff.ambient    = true;
  ff.escalated  = false;
  ff.acked      = false;
  ff.open       = blinkLid;
  ff.pulse      = breath;
  ff.gazeX      = glanceX;
  ff.gazeY      = glanceY;
  ff.color      = scaleColor(accent, level);
  ff.geom       = ambientGeometry();

  FaceCanvas fc = faceCanvas();
  faceAt(gFaceIdx)->draw(fc, ff);

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

// Declared here because the sequence below picks the mode to settle into.
static bool ambientActive(uint32_t nowMs);

// ---------------------------------------------------------------------------
// The first run
//
// The out-of-the-box moment. Someone opens a box, plugs this into any USB
// port, and before they have read a word the object has to do something.
//
// The shape of it is a waking creature, not a boot sequence. It is dark, a
// light comes up on a pair of shut eyes, they crack open against the light,
// they look around, they find the person in front of them, and they blink.
// Then, and only then, it says who it belongs to. Nothing here needs a host,
// nothing here is a status, and nothing here is a wall of text.
//
// The last two seconds are already the ambient composition (same face, same
// position, same two text lines), so the handoff at FR_T_END has nothing left
// to change and the transition into normal life is invisible.
//
// Every value a face is given comes from this function, so the sequence works
// with whichever face the board is set to: it is handed ST_IDLE and
// `ambient = true` throughout and drives the lids entirely through `open`,
// which is the one channel every face honours literally.
// ---------------------------------------------------------------------------

// 0..1 progress across [a, b], clamped outside it.
static float frSeg(uint32_t t, uint32_t a, uint32_t b) {
  if (b <= a || t <= a) return 0.0f;
  if (t >= b) return 1.0f;
  return (float)(t - a) / (float)(b - a);
}

// ---------------------------------------------------------------------------
// The hand.
//
// The device fonts are ASCII, so there is no waving-hand glyph to reach for
// and there is no image loader to reach for either. It is six capsules and a
// rotation: `drawWideLine` is LovyanGFX's anti-aliased wedge with round caps,
// which is the same primitive FaceArc walks its ribbons with and FaceBear
// draws its mouth with, so the edges match everything else on the panel and
// it costs six calls a frame.
//
// Local coordinates are millimetres of nothing in particular: x right, y UP,
// origin at the wrist, which is also the pivot. Rotating about the wrist is
// what makes it a wave rather than a slide, because that is where a real hand
// hinges. Resting extents are about 21px wide by 26 tall; the sweep takes it
// to roughly 29px wide, which is the width the layout reserves.
struct FrHandSeg { float x0, y0, x1, y1, w; };
static const FrHandSeg FR_HAND[] = {
  {  0.0f,  2.0f,   0.0f,  9.0f, 13.0f },  // palm, one fat capsule
  { -5.0f,  4.0f, -11.5f,  9.0f,  4.5f },  // thumb, out to the left
  { -4.5f, 10.0f,  -5.5f, 20.0f,  4.0f },  // index
  { -1.5f, 11.0f,  -1.8f, 22.0f,  4.0f },  // middle, the tallest
  {  1.5f, 11.0f,   1.8f, 21.0f,  4.0f },  // ring
  {  4.5f, 10.0f,   5.5f, 18.5f,  4.0f },  // little
};
static constexpr int   FR_HAND_N    = (int)(sizeof(FR_HAND) / sizeof(FR_HAND[0]));
// Width the layout reserves, and how far the pivot sits inside it from the
// left. The hand is not symmetric about the wrist (the thumb only goes one
// way) and the sweep is, so the slot is the union of both.
static constexpr int   FR_HAND_W    = 30;
static constexpr int   FR_HAND_PIVX = 17;
// Rest extents above and below the wrist, used to centre the group on
// FR_GREET_Y rather than eyeballing it.
static constexpr float FR_HAND_UP   = 24.0f;
static constexpr float FR_HAND_DOWN = 4.5f;

// Draw the hand with its wrist at (px, py), rotated `deg` clockwise about
// that wrist, in `color`. Nothing is cached: at six segments the trig is
// cheaper than the state to avoid it.
static void drawFrHand(float px, float py, float deg, uint16_t color) {
  const float a = deg * (float)M_PI / 180.0f;
  const float s = sinf(a), c = cosf(a);
  for (int i = 0; i < FR_HAND_N; ++i) {
    const FrHandSeg& g = FR_HAND[i];
    // y is up in local space and down on the panel, hence the negated y.
    float ax = px + (g.x0 * c + g.y0 * s);
    float ay = py - (g.y0 * c - g.x0 * s);
    float bx = px + (g.x1 * c + g.y1 * s);
    float by = py - (g.y1 * c - g.x1 * s);
    // drawWideLine's float argument is a RADIUS, and the table is in widths
    // because that is how a finger is measured.
    gfx->drawWideLine((int)lroundf(ax), (int)lroundf(ay),
                      (int)lroundf(bx), (int)lroundf(by),
                      g.w * 0.5f, color);
  }
}

// The greeting beat: "Hi OpenAI" with a hand waving beside it.
//
// `t` is sequence time and `accent` is the face's colour this frame, which
// the hand is painted in deliberately: it is the creature's own hand reaching
// up from just off the panel, not a piece of chrome. The text stays white
// like every other word this device says.
//
// The whole group is centred as one unit, so a different string or a
// different font would still land centred without anything here being
// retuned.
static void renderFirstRunGreeting(uint32_t t, uint16_t accent) {
  if (t < FR_T_GREET || t >= FR_T_GREET + FR_GREET_MS) return;
  const uint32_t u = t - FR_T_GREET;

  // Rise, wave, drop. The rise decelerates and the drop accelerates, so the
  // hand arrives softly and leaves like it is being lowered rather than
  // switched off. Alpha rides the same two ramps.
  float lift = 1.0f, alpha = 1.0f;
  if (u < FR_GREET_IN) {
    float k = frSeg(u, 0, FR_GREET_IN);
    lift  = sinf(k * (float)M_PI * 0.5f);
    alpha = k;
  } else if (u >= FR_GREET_MS - FR_GREET_OUT) {
    float k = frSeg(u, FR_GREET_MS - FR_GREET_OUT, FR_GREET_MS);
    lift  = 1.0f - k * k;
    alpha = 1.0f - k;
  }
  if (alpha <= 0.0f) return;

  // The sweep runs only in the middle, and starts and ends at exactly
  // upright: sin is zero at both ends of a whole number of cycles, so the
  // hand never snaps back to vertical when the beat hands over.
  float deg = 0.0f;
  if (u >= FR_GREET_IN && u < FR_GREET_MS - FR_GREET_OUT) {
    float k = frSeg(u, FR_GREET_IN, FR_GREET_MS - FR_GREET_OUT);
    deg = FR_WAVE_DEG * sinf(k * FR_WAVE_N * 2.0f * (float)M_PI);
  }

  gfx->setFont(&fonts::Font2);
  gfx->setTextSize(1);
  const int textW = gfx->textWidth(FR_GREET_TXT);
  const int groupW = textW + FR_GREET_GAP + FR_HAND_W;
  const int left   = SCREEN_W / 2 - groupW / 2;

  gfx->setTextDatum(textdatum_t::middle_left);
  gfx->setTextColor(scaleColor(C_TEXT, alpha * 0.88f), C_BG);
  gfx->drawString(FR_GREET_TXT, left, FR_GREET_Y);

  // Wrist y so the resting hand is centred on the same row as the text.
  const float restY = (float)FR_GREET_Y + (FR_HAND_UP - FR_HAND_DOWN) * 0.5f;
  drawFrHand((float)(left + textW + FR_GREET_GAP + FR_HAND_PIVX),
             restY + (1.0f - lift) * (float)FR_GREET_RISE,
             deg, scaleColor(accent, alpha));

  // Font0 is what the caller draws its second line in, and setFont is
  // sticky, so hand the state back the way it was found.
  gfx->setTextDatum(textdatum_t::top_center);

#ifdef FPS_DEBUG
  // Nobody can see this screen from a serial port, so the debug build says
  // where it put the group. Once per beat, on the frame the hand is upright
  // and fully up, which is the frame the numbers describe. The extents are
  // the SWEPT ones, not the resting ones: the check that matters is that the
  // hand at full tilt still clears the panel edge and the face box above.
  static bool said = false;
  if (u < FR_GREET_IN) said = false;      // re-arms for the next replay
  if (u >= FR_GREET_IN && !said) {
    said = true;
    Serial.printf("greet: textw=%d group=%d left=%d wrist=(%d,%d) "
                  "sweep_x=%d..%d y=%d..%d\n",
                  textW, groupW, left,
                  left + textW + FR_GREET_GAP + FR_HAND_PIVX,
                  (int)lroundf(restY),
                  left + textW + FR_GREET_GAP + FR_HAND_PIVX - 17,
                  left + textW + FR_GREET_GAP + FR_HAND_PIVX + 13,
                  (int)lroundf(restY - FR_HAND_UP),
                  (int)lroundf(restY + FR_HAND_DOWN));
  }
#endif
}

static void renderFirstRun(uint32_t nowMs, uint32_t dtMs) {
  (void)dtMs;
  const uint32_t t = nowMs - firstRunStartMs;

  // --- the light comes up -------------------------------------------------
  // Squared, so it arrives rather than ramps: a linear fade from black reads
  // as a dissolve, and this has to read as something switching on.
  static uint8_t lastDuty = 255;
  float lightK = frSeg(t, 0, FR_T_CRACK);
  uint8_t duty = (uint8_t)lroundf(255.0f * lightK * lightK);
  if (duty != lastDuty) { lcd.setBrightness(duty); lastDuty = duty; }

  // --- the lids -----------------------------------------------------------
  // Shut, then cracked to a squint against the light, then held there, then
  // the rest of the way open. The hold is what separates waking up from a
  // shutter opening: a real eye opens in two moves, not one.
  float lid;
  if      (t < FR_T_CRACK)  lid = 0.0f;
  else if (t < FR_T_SQUINT) lid = 0.35f * frSeg(t, FR_T_CRACK, FR_T_SQUINT);
  else if (t < FR_T_OPEN)   lid = 0.35f;
  else {
    float k = frSeg(t, FR_T_OPEN, FR_T_WIDE);
    lid = 0.35f + 0.65f * sinf(k * (float)M_PI * 0.5f);   // decelerating
  }

  // The scripted first blink. The blink machine was armed in startFirstRun to
  // fire at exactly FR_T_BLINK and is left to run itself from there, so every
  // blink after the first one is the ordinary randomised rhythm rather than
  // more choreography.
  lid *= updateBlink(nowMs, BLINK_GAP_MIN, BLINK_GAP_MAX, t >= FR_T_BLINK);

  // --- where it is looking ------------------------------------------------
  // It wakes up looking somewhere else, drifts a little further, and then
  // snaps to centre. The snap is the beat the whole sequence is built around:
  // slow enough to see, fast enough to read as noticing rather than scanning.
  float gx = 0.0f, gy = 0.0f;
  if (t >= FR_T_WIDE && t < FR_T_LOOK) {
    float k = frSeg(t, FR_T_WIDE, FR_T_LOOK);
    gx = FR_LOOK_AX * k;
    gy = FR_LOOK_AY * k;
  } else if (t >= FR_T_LOOK && t < FR_T_FIND) {
    float k = frSeg(t, FR_T_LOOK, FR_T_FIND);
    float e = sinf(k * (float)M_PI * 0.5f);               // decelerating
    gx = FR_LOOK_AX * (1.0f - e);
    gy = FR_LOOK_AY * (1.0f - e);
  }
  // After FR_T_FIND the gaze is locked dead centre. It is looking at you, and
  // a face that keeps glancing away while introducing itself is not.

  // --- colour -------------------------------------------------------------
  // Sleep blue while shut, the resting blue once open, then eased into the
  // live ambient accent over the last four seconds so the settle at FR_T_END
  // has no colour step left to make.
  const uint16_t shutCol = mixColor(C_SLEEP, C_TEXT, 0.42f);
  uint16_t col;
  float    breath = 0.0f;
  if (t < FR_T_CRACK) {
    col = scaleColor(shutCol, 0.35f + 0.65f * lightK);
  } else if (t < FR_T_WIDE) {
    col = mixColor(shutCol, C_IDLE, frSeg(t, FR_T_CRACK, FR_T_WIDE));
  } else if (t < FR_T_NOD) {
    col = C_IDLE;
  } else {
    breath = breathe(nowMs, AMB_BREATHE_MS);
    uint16_t amb = scaleColor(ambientAccent(nowMs), 0.55f + 0.40f * breath);
    col = mixColor(C_IDLE, amb, frSeg(t, FR_T_NOD, FR_T_END));
  }
  firstRunAccent = (t < FR_T_NOD) ? C_IDLE : ambientAccent(nowMs);

  // --- the nod ------------------------------------------------------------
  // One small lift of the whole head as the name appears. The composition owns
  // where the face sits, so this is a geometry offset and not something the
  // face is asked to do: no face has to know the sequence exists.
  FaceGeometry geom = ambientGeometry();
  if (t >= FR_T_NOD && t < FR_T_NOD + FR_NOD_MS) {
    float k = 1.0f - frSeg(t, FR_T_NOD, FR_T_NOD + FR_NOD_MS);
    int   dy = (int)lroundf(-3.0f * k);
    geom.cy   += dy;
    geom.boxY += dy;
  }

  FaceFrame ff;
  ff.nowMs      = nowMs;
  ff.dtMs       = dtMs;
  ff.stateAgeMs = t;
  ff.state      = ST_IDLE;
  ff.ambient    = true;
  ff.escalated  = false;
  ff.acked      = false;
  ff.open       = lid;
  ff.pulse      = breath;
  ff.gazeX      = gx;
  ff.gazeY      = gy;
  ff.color      = col;
  ff.geom       = geom;

  FaceCanvas fc = faceCanvas();
  faceAt(gFaceIdx)->draw(fc, ff);

  // --- the greeting -------------------------------------------------------
  // Drawn over the face and under nothing, in the text band the two lines
  // below have not reached yet: the greeting is finished at FR_T_GREET +
  // FR_GREET_MS = 5250 and the name does not begin fading in until
  // FR_T_NOD = 5450, so the two never share the panel and neither has to
  // know about the other.
  renderFirstRunGreeting(t, col);

  // --- the two lines ------------------------------------------------------
  // Line 1 is whatever ambient's line 1 will be, so it never moves or changes.
  // Line 2 says "hello" and then dissolves into whatever ambient's line 2 will
  // be. By FR_T_SWAP + FR_SWAP_MS*2 the screen IS the ambient screen.
  char unit[24];
  unitIdString(unit, sizeof(unit));
  const char* owner = activeName();
  const char* line1 = owner ? owner : PRODUCT_NAME;
  const char* line2Rest = owner ? PRODUCT_NAME : unit;

  gfx->setTextDatum(textdatum_t::top_center);
  gfx->setTextSize(1);

  float a1 = frSeg(t, FR_T_NOD, FR_T_NOD + FR_FADE_MS);
  if (a1 > 0.0f) {
    gfx->setTextColor(scaleColor(C_TEXT, a1 * 0.88f), C_BG);
    gfx->setFont(&fonts::Font2);
    gfx->drawString(line1, SCREEN_W / 2, LABEL_Y);
  }

  // "hello" fades in, holds, fades out; the ambient line fades in behind it.
  // They never overlap: the swap is a dip through black at FR_T_SWAP +
  // FR_SWAP_MS, which is cheaper to read than a cross-dissolve of two
  // different strings in the same place.
  const char* line2 = nullptr;
  float a2 = 0.0f;
  if (t < FR_T_SWAP + FR_SWAP_MS) {
    line2 = "hello";
    a2 = frSeg(t, FR_T_HELLO, FR_T_HELLO + FR_FADE_MS) *
         (1.0f - frSeg(t, FR_T_SWAP, FR_T_SWAP + FR_SWAP_MS));
  } else {
    line2 = line2Rest;
    a2 = frSeg(t, FR_T_SWAP + FR_SWAP_MS, FR_T_SWAP + 2 * FR_SWAP_MS);
  }
  if (a2 > 0.0f && line2 != nullptr && line2[0] != '\0') {
    gfx->setTextColor(scaleColor(C_TEXT_DIM, a2), C_BG);
    gfx->setFont(&fonts::Font0);
    gfx->drawString(line2, SCREEN_W / 2, SUB_Y);
  }

  lastAccent = firstRunAccent;

  // --- settle -------------------------------------------------------------
  // Hand the screen back. The panel already looks exactly like ambient, so the
  // 700ms cross-fade this arms has almost nothing to do, which is the point.
  if (t >= FR_T_END) {
    firstRunActive = false;
    lastDuty       = 255;
    if (firstRunSpend) spendFirstRun();
    firstRunSpend  = false;
    setBrightness(BRIGHT_FULL_IDX);
    brightManual   = false;
    dimmedByAuto   = false;
    // Whichever mode is correct NOW: a board that was nudged by a host during
    // the sequence has hostEverSpoke latched and belongs in live.
    ambientMode    = ambientActive(nowMs);
    ambientEnterMs = nowMs;
    modeChangeMs   = nowMs;
    xfadeFrom      = firstRunAccent;
    lastTier       = 0;
    // Give the blink and glance machines an ordinary schedule to resume on.
    blinkNextMs    = nowMs + randRange(BLINK_GAP_MIN, BLINK_GAP_MAX);
    glanceNextMs   = nowMs + randRange(GLANCE_GAP_MIN, GLANCE_GAP_MAX);
  }
}

// True whenever the live view has nothing honest to show.
//
// SIGNED compare, and the sign is the whole point. loop() reads millis() once
// at the top and hands that value to everything; pumpSerial stamps lastDataMs
// with a FRESH millis() later in the same iteration, so on any line that lands
// across a millisecond boundary lastDataMs is one or two ms AHEAD of the nowMs
// this is called with. Unsigned, `nowMs - lastDataMs` is then 0xFFFFFFFE,
// which sails past a 300000 threshold and declares the host gone.
//
// The symptom was a single-frame flip into ambient and straight back on a live
// board that was being talked to normally, roughly once per line. It armed a
// 700ms cross-fade every time it happened, and it was found the way this class
// of bug always is: by hanging state off the transition (the stats session
// clock) and watching that state get cleared by a host that had never stopped
// talking. Pre-existing, and the same reasoning applies to the two staleness
// tests below.
static bool ambientActive(uint32_t nowMs) {
  if (!hostEverSpoke) return true;
  return (int32_t)(nowMs - lastDataMs) >= (int32_t)AMBIENT_AFTER_MS;
}

// Flip the mode and arm the cross-fade. Called from loop() before both the
// brightness ladder and the renderer, so the two can never disagree about
// which mode this tick is in.
static void updateMode(uint32_t nowMs) {
  // The first run owns the screen and picks its own mode to settle into when
  // it ends. Flipping underneath it would arm a second cross-fade mid-beat.
  if (firstRunActive) return;
  bool amb = ambientActive(nowMs);
  if (amb == ambientMode) return;
  ambientMode  = amb;
  modeChangeMs = nowMs;
  xfadeFrom    = lastAccent;
  if (amb) {
    ambientEnterMs = nowMs;
    // Five minutes with no host is the device's own definition of "that
    // session is over", so it is the definition the session clock uses too. An
    // unfinished turn goes with it: a turn nobody ever said `done` to did not
    // complete, and holding its clock open would make the next `done` claim a
    // turn that lasted all afternoon.
    statsHostGone();
  }
}

// The two anti burn-in sine phases, -1..1. Shared so the compositions and the
// message card wander on exactly the same slow schedule instead of each
// rolling its own.
static void driftPhase(uint32_t nowMs, float* px, float* py) {
  *px = sinf((float)(nowMs % AMB_DRIFT_X_MS) / (float)AMB_DRIFT_X_MS *
             2.0f * (float)M_PI);
  *py = sinf((float)(nowMs % AMB_DRIFT_Y_MS) / (float)AMB_DRIFT_Y_MS *
             2.0f * (float)M_PI);
}

// ---------------------------------------------------------------------------
// SAY: the card itself
//
// A framed sign, the message as large as it will go, and a bar along the
// bottom that drains over the hold so the thing visibly has an end. Nothing
// moves after the entrance except that bar and the burn-in drift, because this
// is the one composition that can legitimately sit on a desk for an hour and
// design law 4 says it must not be irritating to sit next to.
// ---------------------------------------------------------------------------

// The card borrows the colour of whatever it is covering, so a message during
// a busy turn is amber and one on an idle desk is the drifting ambient blue.
static uint16_t sayAccent(uint32_t nowMs) {
  if (ambientMode) return ambientAccent(nowMs);
  switch (effectiveState(nowMs)) {
    case ST_SLEEP:   return C_SLEEP;
    case ST_BUSY:    return C_BUSY;
    case ST_DONE:    return C_DONE;
    case ST_WAITING: return C_WAIT;   // unreachable: the card yields to it
    default:         return C_IDLE;
  }
}

static void renderSay(uint32_t nowMs) {
  // Smoothstep, so the entrance decelerates into place rather than arriving
  // at full speed.
  float a = sayAlpha(nowMs);
  float e = a * a * (3.0f - 2.0f * a);

  // Full drift amplitude in both modes. Live mode normally holds the
  // composition pixel-exact, but a card sent with "saysecs":0 can hold the
  // same white pixels for hours, which is the one case in this firmware that
  // actually wants the wander.
  float px, py;
  driftPhase(nowMs, &px, &py);
  int ox = (int)lroundf(AMB_DRIFT_AX * px);
  int oy = (int)lroundf(AMB_DRIFT_AY * py) + (int)lroundf((1.0f - e) * 9.0f);

  uint16_t accent = sayAccent(nowMs);
  // A card sent with "saysecs":0 draws no hold bar, and once the entrance has
  // settled its only moving part is the drift, which holds one (ox, oy) pair
  // for a median of 1.45s and up to 7.6s near the sine peaks. That is a
  // byte-identical frame for multi-second stretches on a screen that can be up
  // for an hour, which is exactly what the sign's breathe exists to prevent:
  // a frozen panel and a crashed one look the same from a desk away. Same
  // breathe, same period, on the stroke alone. A card that IS counting down
  // has its bar and needs none of this.
  //
  // The gate is the hold LENGTH and not the sentinel. A card that IS counting
  // down was assumed to have a visibly moving bar, which is only true while
  // the bar actually steps: it is SAY_BAR_W pixels wide, so at the one hour
  // cap one pixel is 13.8 seconds, far slower than the drift's own worst hold,
  // and {"say":"in a meeting","saysecs":3600} is exactly as frozen as the card
  // this breathe was written for.
  float frameK = 1.0f;
  if (sayHoldMs == 0 || sayHoldMs > (uint32_t)SAY_BAR_W * 4000u) {
    frameK = 0.90f + 0.10f * breathe(nowMs, AMB_BREATHE_MS);
  }
  // Through the cross-fade like every other composition here. Without it a
  // long-lived card sitting across the live-to-ambient boundary popped its
  // accent in a single frame while the 700ms cross-fade ran unseen underneath.
  uint16_t edge   = xf(scaleColor(accent, 0.40f * e * frameK));
  gfx->drawRoundRect(SAY_FRAME_X + ox, SAY_FRAME_Y + oy,
                     SAY_FRAME_W, SAY_FRAME_H, SAY_FRAME_R, edge);
  gfx->drawRoundRect(SAY_FRAME_X + 1 + ox, SAY_FRAME_Y + 1 + oy,
                     SAY_FRAME_W - 2, SAY_FRAME_H - 2, SAY_FRAME_R - 1, edge);

  if (sayLay.lines > 0 && sayLay.font != nullptr) {
    gfx->setFont(sayLay.font);
    gfx->setTextSize(sayLay.size);
    gfx->setTextDatum(textdatum_t::middle_center);
    // Transparent background, like the centre text: the buffer is already
    // cleared and an opaque box would punch a rectangle through the frame.
    gfx->setTextColor(xf(scaleColor(C_TEXT, e)));
    int span = (sayLay.lines - 1) * sayLay.step;
    int y0   = SAY_TEXT_CY + oy - span / 2;
    for (int i = 0; i < sayLay.lines; ++i) {
      gfx->drawString(sayLay.line[i], SCREEN_W / 2 + ox, y0 + i * sayLay.step);
    }
    gfx->setTextSize(1);
  }

  // The hold, drawn as a bar that drains from both ends toward the centre. A
  // card with no expiry deliberately has no bar: nothing is counting down.
  if (sayHoldMs != 0) {
    float    left = 1.0f;
    uint32_t age  = nowMs - sayStartMs;
    if (age > SAY_IN_MS) {
      uint32_t run = age - SAY_IN_MS;
      left = (run >= sayHoldMs) ? 0.0f
                                : 1.0f - (float)run / (float)sayHoldMs;
    }
    int w = (int)lroundf((float)SAY_BAR_W * left * e);
    if (w > 0) {
      gfx->fillRect(SCREEN_W / 2 + ox - w / 2, SAY_BAR_Y + oy, w, SAY_BAR_H,
                    xf(scaleColor(accent, 0.55f * e)));
    }
  }
}

// ---------------------------------------------------------------------------
// DO NOT DISTURB: the sign
//
// A thick violet frame, two words, and the owner's name under them. No face,
// no ring, no metric, no countdown. The face is this device's whole vocabulary
// for "alive and working on something", so the surest way to say "not now" is
// for the creature to be absent rather than for it to pull a face about it.
//
// The frame is the part that survives distance. The panel is 44mm wide, so at
// 24pt the cap height is about 4.6mm and the words give out somewhere past two
// metres; a 296x154 violet rectangle is still a violet rectangle across an
// open-plan floor, and violet is a hue no state on this device owns.
//
// Nothing moves except a slow breathe on the frame and the burn-in drift,
// which this composition applies at full amplitude in BOTH modes for the same
// reason the message card does: it is the other screen here that can
// legitimately hold the same pixels until somebody comes back from lunch. The
// words themselves are rock steady, because type that breathes is type that
// nags, and design law 4 says this thing must be sittable-next-to.
// ---------------------------------------------------------------------------
static void renderDnd(uint32_t nowMs) {
  float a = dndAlpha(nowMs);
  float e = a * a * (3.0f - 2.0f * a);      // smoothstep, decelerating entrance

  float px, py;
  driftPhase(nowMs, &px, &py);
  int ox = (int)lroundf(AMB_DRIFT_AX * px);
  int oy = (int)lroundf(AMB_DRIFT_AY * py);

  // A very slow, shallow breathe on the frame alone. Six and a half seconds is
  // the ambient breathe, which is slow enough to be invisible unless you look
  // for it; the only job it has is to keep a sign that may be up for an hour
  // from being a byte-identical frame, since a frozen panel and a crashed one
  // look exactly the same from a desk away.
  float b = breathe(nowMs, AMB_BREATHE_MS);
  uint16_t frame = xf(scaleColor(C_DND, (0.72f + 0.28f * b) * e));

  // Concentric rounded rects rather than one filled rect minus another: the
  // stroke is 6,944 pixels drawn this way and 84,224 drawn as two fills, and
  // the frame is on screen for as long as somebody is at lunch.
  for (int i = 0; i < DND_FRAME_T; ++i) {
    gfx->drawRoundRect(DND_FRAME_X + i + ox, DND_FRAME_Y + i + oy,
                       DND_FRAME_W - 2 * i, DND_FRAME_H - 2 * i,
                       DND_FRAME_R - i, frame);
  }

  const char* owner = activeName();

  if (dndLay.font != nullptr) {
    gfx->setFont(dndLay.font);
    gfx->setTextSize(1);
    gfx->setTextDatum(textdatum_t::middle_center);
    // White, not violet: the frame carries the colour and the words carry the
    // contrast. Transparent background, like every other string here, because
    // the buffer is cleared every frame anyway.
    gfx->setTextColor(xf(scaleColor(C_TEXT, e)));
    int cy = (owner ? DND_TEXT_CY_NAMED : DND_TEXT_CY_BARE) + oy;
    gfx->drawString(DND_LINE1, SCREEN_W / 2 + ox, cy - dndLay.step / 2);
    gfx->drawString(DND_LINE2, SCREEN_W / 2 + ox, cy + dndLay.step / 2);
  }

  // Whose desk this is. Only when there is a name: "codex companion" under a
  // DO NOT DISTURB sign is an advertisement, not information.
  if (owner != nullptr) {
    gfx->setFont(&fonts::Font2);
    gfx->setTextSize(1);
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setTextColor(xf(scaleColor(C_TEXT, 0.55f * e)));
    gfx->drawString(owner, SCREEN_W / 2 + ox, DND_NAME_Y + oy);
  }
}

// ---------------------------------------------------------------------------
// THE TOY: the game itself
//
// A ground line, a creature standing on it, blocks arriving from the right,
// and two numbers. Nothing else, because the entire brief for this screen is
// that somebody understands it in five seconds without being told anything.
//
// The runner is the ACTIVE FACE, drawn through the ordinary interface at a
// smaller geometry. That is why this composition is short: it does not know
// how an eye is drawn, it hands over a box and a colour like every other
// composition here, and the bear runs as a bear while the arc runs as two
// strokes. It is also why the jump and the run bob are applied to the geometry
// rather than asked of the face: no face has to know this screen exists, which
// is the same rule the first run's nod already follows.
//
// The palette is the ambient blue, deliberately. Amber and red are the two
// colours on this device that mean "look at me now", and a game must never
// borrow either: at four metres this has to read as the object being off duty,
// not as a state nobody recognises.
// ---------------------------------------------------------------------------
static void renderToy(uint32_t nowMs, uint32_t dtMs) {
  toyStep(nowMs, dtMs);

  const uint16_t accent = ambientAccent(nowMs);
  const uint16_t live   = xf(scaleColor(accent, 0.92f));
  const uint16_t dim    = xf(scaleColor(C_TEXT_DIM, 0.85f));

  // The crash flash. The ground goes bright for as long as the restart is
  // locked out, which makes the lockout visible instead of making the board
  // feel briefly unresponsive.
  float flash = 0.0f;
  if (toyDead) {
    flash = 1.0f - clampf((float)(nowMs - toyDeadMs) / (float)TOY_OVER_MS,
                          0.0f, 1.0f);
  }
  // ...and, once the flash has decayed, one thing that is still moving.
  //
  // Everything on this screen stops when toyDead: toyStep returns before any
  // travel, so the scroll, the run phase and every obstacle x are frozen; the
  // run bob is zeroed; the blink is disabled and `open` is pinned below; the
  // flash is gone at TOY_OVER_MS (450ms); the score, BEST and legend strings
  // are constant; and gOx/gOy are exactly zero in settled live mode. On the
  // default face, whose non-ambient ST_IDLE branch reads nothing from nowMs,
  // that left a byte-identical frame from 450ms after a crash until
  // TOY_IDLE_MS (30s) closed the screen. A frozen panel and a crashed one look
  // the same from a desk away, which is why renderSay, renderDnd and
  // renderFocus each carry a breathe of their own; this is that breathe, on
  // the same AMB_BREATHE_MS period, on the one element the dead state still
  // owns. Bear and arc escaped this only by accident, because their idle
  // motion is free-running.
  float still = 1.0f;
  if (toyDead) {
    const float ph = (float)(nowMs % AMB_BREATHE_MS) / (float)AMB_BREATHE_MS;
    still = 0.62f + 0.38f * (0.5f - 0.5f * cosf(ph * 2.0f * (float)M_PI));
  }
  const uint16_t ground = xf(mixColor(scaleColor(C_TEXT_DIM, 0.8f * still),
                                      C_TEXT, flash));

  // The burn-in drift, applied to the whole scene exactly as the ambient and
  // live compositions apply it. This was the one composition here drawn at
  // absolute coordinates, and its ground line is a full-width bar on identical
  // pixels for as long as the screen is up. The physics are untouched: the hit
  // test in toyStep stays in world coordinates and everything visible moves by
  // the same offset, so drifting the drawing cannot change who hits what.
  //
  // gOx and gOy are zero in settled live mode, which is correct here too: a
  // game opened over a live session is up for at most thirty seconds.
  const int toyDx = gOx;
  const int toyDy = gOy;

  // Overdrawn past both edges so the drift can never expose a gap at one end.
  gfx->fillRect(toyDx - 12, TOY_GROUND_Y + toyDy, SCREEN_W + 24, 2, ground);
  // Ticks under the line. The runner never moves in x, so these are the only
  // thing that says the ground is moving at all, and they stop when it does.
  const uint16_t tick = xf(scaleColor(C_TEXT_DIM, 0.45f));
  for (int k = -1; k < 6; ++k) {
    int x = (int)lroundf((float)(k * 60) - toyScroll);
    if (x < -14 || x > SCREEN_W) continue;
    gfx->fillRect(x + toyDx, TOY_GROUND_Y + 5 + toyDy, 14, 2, tick);
  }

  for (int i = 0; i < TOY_OBS_MAX; ++i) {
    if (!toyObs[i].live) continue;
    gfx->fillSmoothRoundRect((int)lroundf(toyObs[i].x) + toyDx,
                             TOY_GROUND_Y - toyObs[i].h + toyDy,
                             toyObs[i].w, toyObs[i].h, 3, dim);
  }

  // The runner. `bob` is the gait and `jump` is the physics; both move the
  // whole creature, so they are added to the geometry rather than handed to
  // the face as some new channel it would have to honour.
  const int jump = (int)lroundf(toyY);
  const float cadence = 0.5f - 0.5f * cosf(toyRunPhase * 2.0f * (float)M_PI);
  const int   bob     = (toyDead || jump > 0) ? 0 : (int)lroundf(2.0f * cadence);
  const int   lift    = jump + bob;

  FaceGeometry g;
  g.cx   = TOY_RUN_X + toyDx;
  g.cy   = TOY_EYE_CY - lift + toyDy;
  g.eyeW = TOY_EYE_W;
  g.eyeH = TOY_EYE_H;
  g.gap  = TOY_EYE_GAP;
  g.boxX = TOY_RUN_X - TOY_BOX_W / 2 + toyDx;
  g.boxY = TOY_GROUND_Y - TOY_BOX_H - lift + toyDy;
  g.boxW = TOY_BOX_W;
  g.boxH = TOY_BOX_H;

  // It still blinks while it runs, on the same driver and the same randomised
  // schedule as every other screen, so the creature in the game is recognisably
  // the creature on the desk. A crash shuts the eyes instead.
  float open = updateBlink(nowMs, BLINK_GAP_MIN, BLINK_GAP_MAX, !toyDead);
  if (toyDead) open = 0.12f;

  FaceFrame ff;
  ff.nowMs      = nowMs;
  ff.dtMs       = dtMs;
  ff.stateAgeMs = nowMs - toyInputMs;
  ff.state      = ST_IDLE;
  ff.ambient    = false;
  ff.escalated  = false;
  ff.acked      = false;
  ff.open       = open;
  ff.pulse      = cadence;
  // Looking where it is going, and up a little while it is off the ground.
  ff.gazeX      = 3.0f;
  ff.gazeY      = (jump > 0) ? -1.5f : 0.0f;
  ff.color      = toyDead ? scaleColor(accent, 0.45f) : accent;
  ff.geom       = g;

  FaceCanvas fc = faceCanvas();
  faceAt(gFaceIdx)->draw(fc, ff);

  // The two numbers, top right, out of the runner's way and out of the way of
  // anything arriving from the right.
  char buf[16];
  gfx->setTextDatum(textdatum_t::top_right);
  gfx->setTextSize(1);
  gfx->setFont(&fonts::Font4);
  gfx->setTextColor(live);
  snprintf(buf, sizeof(buf), "%u", (unsigned)toyScore);
  gfx->drawString(buf, SCREEN_W - 8 + toyDx, 6 + toyDy);

  gfx->setFont(&fonts::Font0);
  gfx->setTextColor(dim);
  snprintf(buf, sizeof(buf), "BEST %u", (unsigned)toyBest);
  gfx->drawString(buf, SCREEN_W - 8 + toyDx, 36 + toyDy);

  // One celebration, and only for the thing worth celebrating.
  if (toyDead && toyNewBest) {
    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setFont(&fonts::Font4);
    gfx->setTextColor(live);
    gfx->drawString("NEW BEST", SCREEN_W / 2 + toyDx, 52 + toyDy);
  }

  // The legend. It is the reason no instruction is needed anywhere else: two
  // buttons, two words, and it says what they do right now. It goes quiet once
  // the first hop proves the point, and comes back when the run ends.
  const char* legend = nullptr;
  if      (toyDead)         legend = "1 AGAIN     2 EXIT";
  else if (!toyEverHopped)  legend = "1 HOP     2 EXIT";
  // The way out stays on the panel for the whole run. Only the half that has
  // been proved goes quiet: a player who wants to stop mid-run is the common
  // case on a shared desk, and the alternative was crashing on purpose.
  else                      legend = "2 EXIT";
  if (legend != nullptr) {
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->setFont(&fonts::Font0);
    gfx->setTextColor(dim);
    gfx->drawString(legend, SCREEN_W / 2 + toyDx, SUB_Y + toyDy);
  }

  lastAccent = accent;
}

// ---------------------------------------------------------------------------
// STATS: the screen
//
// Six numbers, laid out as two big ones over four small ones, on the object
// that has been quietly watching all of them happen. It is a READ screen, so
// it obeys the device's one rule about modal screens without needing a gesture
// of its own: any press takes it down.
//
// It borrows the colour of whatever it is covering, exactly as the message
// card does, so asking for the numbers in the middle of a busy turn does not
// change what colour the desk is.
//
// The two words in the top right are the honest part. A board that has never
// been told the time cannot have a "today", so it says SINCE BOOT and means
// it, rather than printing TODAY over a figure that has been accumulating for
// a week.
// ---------------------------------------------------------------------------
// The screen borrows the colour of whatever it is covering, like the message
// card, with one exception. ST_SLEEP's #26314A is a colour for a dark ring on
// an idle desk, not for six numbers somebody has just asked to read: a
// sleeping board that is asked for its stats borrows the idle blue rather than
// printing them in near-black. The focus timer's screen shares it, which is
// why it is not called statsAccent any more: both screens borrow the colour of
// what they are covering, and there is one answer to that question.
static uint16_t coverAccent(uint32_t nowMs) {
  if (ambientMode) return ambientAccent(nowMs);
  switch (effectiveState(nowMs)) {
    case ST_BUSY: return C_BUSY;
    case ST_DONE: return C_DONE;
    default:      return C_IDLE;      // idle, sleep, and the unreachable wait
  }
}

static void renderStats(uint32_t nowMs) {
  const uint16_t accent = coverAccent(nowMs);
  const uint16_t val    = xf(accent);
  const uint16_t dim    = xf(scaleColor(C_TEXT_DIM, 0.95f));
  const uint16_t rule   = xf(scaleColor(C_TEXT_DIM, 0.35f));
  const uint16_t text   = xf(C_TEXT);

  // gOx/gOy are the composition's own drift: zero in live, wandering in
  // ambient. This screen closes itself after twenty seconds, so unlike the
  // message card and the sign it has no business forcing full amplitude in
  // live mode. Twenty seconds cannot burn anything in.
  const int ox = gOx, oy = gOy;

  gfx->setTextSize(1);

  // Header. The left word says what this is; the right one says what "today"
  // is worth.
  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(textdatum_t::top_left);
  gfx->setTextColor(dim);
  gfx->drawString("STATS", STATS_MARGIN + ox, 8 + oy);
  gfx->setTextDatum(textdatum_t::top_right);
  gfx->drawString(gClockKnown ? "TODAY" : "SINCE BOOT",
                  SCREEN_W - STATS_MARGIN + ox, 8 + oy);

  gfx->fillRect(STATS_MARGIN + ox, STATS_RULE1_Y + oy,
                SCREEN_W - 2 * STATS_MARGIN, 1, rule);
  gfx->fillRect(STATS_MARGIN + ox, STATS_RULE2_Y + oy,
                SCREEN_W - 2 * STATS_MARGIN, 1, rule);

  char buf[16];

  // The two figures somebody actually wants: how much did I do today.
  static const char* BIG_LAB[2] = { "TURNS", "TOKENS" };
  for (int i = 0; i < 2; ++i) {
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->setFont(&fonts::Font0);
    gfx->setTextColor(dim);
    gfx->drawString(BIG_LAB[i], STATS_BIG_CX[i] + ox, STATS_BIG_LAB_Y + oy);

    if (i == 0) snprintf(buf, sizeof(buf), "%lu", (unsigned long)stTurns);
    else        fmtTokens(buf, sizeof(buf), stTokens);
    gfx->setFont(&fonts::Font4);
    gfx->setTextColor(val);
    gfx->drawString(buf, STATS_BIG_CX[i] + ox, STATS_BIG_VAL_Y + oy);
  }

  // The four that give them a shape: today's longest, how long this sitting
  // has been going, and the two records the board has been keeping.
  static const char* SM_LAB[4] = { "LONGEST", "SESSION", "BEST TURN",
                                   "BEST DAY" };
  for (int i = 0; i < 4; ++i) {
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->setFont(&fonts::Font0);
    gfx->setTextColor(dim);
    gfx->drawString(SM_LAB[i], STATS_SM_CX[i] + ox, STATS_SM_LAB_Y + oy);

    switch (i) {
      case 0: fmtDur(buf, sizeof(buf), stLongest); break;
      case 1:
        // A dash, not a zero. Nobody is in a session right now and pretending
        // otherwise with 0:00 would read as a stopwatch that is broken.
        if (stSessionOpen) fmtDur(buf, sizeof(buf), (nowMs - stSessionMs) / 1000u);
        else               copyClamped(buf, sizeof(buf), "--");
        break;
      case 2: fmtDur(buf, sizeof(buf), stBestTurn); break;
      default: fmtTokens(buf, sizeof(buf), stBestDay); break;
    }
    gfx->setFont(&fonts::Font2);
    gfx->setTextColor(text);
    gfx->drawString(buf, STATS_SM_CX[i] + ox, STATS_SM_VAL_Y + oy);
  }

  // The legend, on the same datum every other bottom line here uses, and now
  // the one place the two-screen fork is written down where somebody with no
  // manual can find it: button 1 goes on to the timer, button 2 leaves. It
  // still says the only thing there is to know, which is that this screen is a
  // visit and not a mode you can get stuck in.
  gfx->setTextDatum(textdatum_t::top_center);
  gfx->setFont(&fonts::Font0);
  gfx->setTextColor(xf(scaleColor(C_TEXT_DIM, 0.7f)));
  gfx->drawString("1 FOCUS    2 CLOSE", SCREEN_W / 2 + ox, SUB_Y + oy);

  lastAccent = accent;
}

// ---------------------------------------------------------------------------
// FOCUS: the screen, and the hairline
//
// A header that says what this is on the left and WHAT CODEX IS DOING on the
// right, the remaining time in the largest digits the library has, a bar that
// drains, and a legend that spells out both buttons. The state word and the
// accent are the coexistence: the screen belongs to the timer and the colour
// still belongs to the agent, so one glance answers both questions. The one
// state that never appears up there is `waiting`, because a pending question
// takes the whole panel and this screen is not drawn at all.
//
// Nothing moves except the digits, the bar and the burn-in drift, which this
// composition applies at FULL amplitude in both modes for exactly the reason
// the message card and the sign do: a forty-five minute countdown holds the
// same pixels far longer than any live composition ever does.
// ---------------------------------------------------------------------------

// What the agent is doing, in a word, for the top right of the timer screen.
static const char* focusStateWord(uint32_t nowMs) {
  if (ambientMode) return "AMBIENT";
  switch (effectiveState(nowMs)) {
    case ST_SLEEP:   return "SLEEP";
    case ST_BUSY:    return "BUSY";
    case ST_DONE:    return "DONE";
    case ST_WAITING: return "WAITING";   // unreachable: the screen yields to it
    default:         return "IDLE";
  }
}

static void renderFocus(uint32_t nowMs) {
  float px, py;
  driftPhase(nowMs, &px, &py);
  const int ox = (int)lroundf(AMB_DRIFT_AX * px);
  const int oy = (int)lroundf(AMB_DRIFT_AY * py);

  const uint16_t accent = coverAccent(nowMs);
  gfx->setTextSize(1);

  // The finish. It fades in, breathes on the same 2400ms the idle and busy
  // states already use, and fades out. No strobe and no siren: there is no
  // buzzer on this board and design law 4 would forbid one anyway. Green,
  // because the milestone pill already established green as the one colour
  // here that celebrates without meaning "answer me now".
  if (focusFin) {
    int32_t signedAge = (focusFinMs == 0) ? 0 : (int32_t)(nowMs - focusFinMs);
    if (signedAge < 0) signedAge = 0;
    const uint32_t age = (uint32_t)signedAge;
    float a;
    if      (age < FOCUS_IN_MS)                 a = (float)age / (float)FOCUS_IN_MS;
    else if (age > FOCUS_END_MS - FOCUS_OUT_MS) a = (float)(FOCUS_END_MS - age) /
                                                    (float)FOCUS_OUT_MS;
    else                                        a = 1.0f;
    a = clampf(a, 0.0f, 1.0f);
    const float e = a * a * (3.0f - 2.0f * a);          // smoothstep
    const float b = 0.80f + 0.20f * breathe(nowMs, FOCUS_BREATHE_MS);

    gfx->setTextDatum(textdatum_t::middle_center);
    gfx->setFont(&fonts::FreeSansBold24pt7b);
    gfx->setTextColor(xf(scaleColor(C_DONE, e * b)));
    // "FOCUS" and not "DONE": `done` is a Codex state on this device and the
    // end of a pomodoro is not it.
    gfx->drawString("FOCUS", SCREEN_W / 2 + ox, FOCUS_DONE_Y + oy);

    char sub[28];
    snprintf(sub, sizeof(sub), "%u MINUTE%s DONE", (unsigned)focusMins,
             focusMins == 1 ? "" : "S");
    gfx->setFont(&fonts::Font2);
    gfx->setTextColor(xf(scaleColor(C_TEXT, 0.85f * e)));
    gfx->drawString(sub, SCREEN_W / 2 + ox, FOCUS_DONE_SUB_Y + oy);

    gfx->setFont(&fonts::Font0);
    gfx->setTextDatum(textdatum_t::top_center);
    gfx->setTextColor(xf(scaleColor(C_TEXT_DIM, 0.7f * e)));
    gfx->drawString("PRESS TO CLOSE", SCREEN_W / 2 + ox, SUB_Y + oy);
    lastAccent = accent;
    return;
  }

  // Header.
  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(textdatum_t::top_left);
  gfx->setTextColor(xf(scaleColor(C_TEXT_DIM, 0.95f)));
  gfx->drawString("FOCUS", FOCUS_MARGIN + ox, FOCUS_HEAD_Y + oy);
  gfx->setTextDatum(textdatum_t::top_right);
  gfx->setTextColor(xf(accent));
  gfx->drawString(focusStateWord(nowMs), SCREEN_W - FOCUS_MARGIN + ox,
                  FOCUS_HEAD_Y + oy);

  // The clock. Dimmed while it is stopped, which is the whole "is this thing
  // running" question answered without anything blinking.
  const uint32_t full = focusFullMs();
  const uint32_t left = focusRemainMs(nowMs);
  char t[16];
  fmtClock(t, sizeof(t), left);
  gfx->setFont(&fonts::Font7);
  gfx->setTextDatum(textdatum_t::middle_center);
  gfx->setTextColor(xf(focusRun ? C_TEXT : scaleColor(C_TEXT, 0.50f)));
  gfx->drawString(t, SCREEN_W / 2 + ox, FOCUS_TIME_Y + oy);

  // The bar, draining, exactly like the message card's hold bar.
  //
  // The anti-freeze breathe is on the TRACK, which is always drawn, and not
  // only on the fill. Stopped, the whole composition is constant: a fixed
  // header, fixed Font7 digits and a fixed legend, with only the burn-in drift
  // moving, and the drift holds one offset for up to 7.6 seconds. A screen
  // that can sit on a desk for an hour has to be visibly alive, and the fill
  // cannot carry that alone: below 2.31% remaining it is narrower than the bar
  // is tall and used to disappear, taking the only moving element with it.
  const float stillK = focusRun
                         ? 1.0f
                         : (0.70f + 0.30f * breathe(nowMs, AMB_BREATHE_MS));
  gfx->fillRoundRect(FOCUS_BAR_X + ox, FOCUS_BAR_Y + oy, FOCUS_BAR_W,
                     FOCUS_BAR_H, FOCUS_BAR_R,
                     xf(scaleColor(C_TEXT_DIM, 0.30f * stillK)));
  if (full > 0) {
    const float frac = clampf((float)left / (float)full, 0.0f, 1.0f);
    int w = (int)lroundf((float)FOCUS_BAR_W * frac);
    // A run with anything left on it draws something. The rounded fill cannot
    // be narrower than it is tall, so the last 2.31% is drawn as a stub rather
    // than as nothing at all: "a few seconds left" and "no timer" must not be
    // the same picture.
    if (w > 0 && w < FOCUS_BAR_H) w = FOCUS_BAR_H;
    if (w > 0) {
      const float k = focusRun
                        ? 1.0f
                        : (0.50f + 0.10f * breathe(nowMs, AMB_BREATHE_MS));
      gfx->fillRoundRect(FOCUS_BAR_X + ox, FOCUS_BAR_Y + oy, w, FOCUS_BAR_H,
                         FOCUS_BAR_R, xf(scaleColor(C_FOCUS, k)));
    }
  }

  // The legend. Both buttons, always, because this is the one screen here with
  // four things to do on it and nobody is getting a manual with the box.
  const char* legend;
  if      (focusRun)      legend = "1 PAUSE    2 CLOSE";
  else if (focusAtRest()) legend = "1 START    2 CLOSE    HOLD 2 LENGTH";
  else                    legend = "1 RESUME    2 CLOSE    HOLD 1 RESET";
  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(textdatum_t::top_center);
  gfx->setTextColor(xf(scaleColor(C_TEXT_DIM, 0.7f)));
  gfx->drawString(legend, SCREEN_W / 2 + ox, SUB_Y + oy);

  lastAccent = accent;
}

// The hairline the ambient and live compositions carry while a run is out of
// sight. Two pixels, dim, draining with the clock, riding the composition's
// own drift and clamped so the drift cannot push it off the panel. It is the
// smallest honest thing this device can draw, and it is not a badge on purpose.
static void renderFocusTick(uint32_t nowMs) {
  if (!focusLive()) return;
  const uint32_t full = focusFullMs();
  if (full == 0) return;

  int y = FOCUS_TICK_Y + gOy;
  if (y > SCREEN_H - FOCUS_TICK_H) y = SCREEN_H - FOCUS_TICK_H;
  if (y < 0) y = 0;
  const int x = FOCUS_TICK_X + gOx;
  const int w = SCREEN_W - 2 * FOCUS_TICK_X;

  gfx->fillRect(x, y, w, FOCUS_TICK_H, xf(scaleColor(C_TEXT_DIM, 0.22f)));
  const float frac = clampf((float)focusRemainMs(nowMs) / (float)full,
                            0.0f, 1.0f);
  const int fw = (int)lroundf((float)w * frac);
  if (fw > 0) {
    gfx->fillRect(x, y, fw, FOCUS_TICK_H,
                  xf(scaleColor(C_FOCUS, focusRun ? 0.75f : 0.35f)));
  }
}

// ---------------------------------------------------------------------------
// MILESTONES: the flourish
//
// A pill that fades in over a quarter of a second, rising seven pixels as it
// arrives, then sits ABSOLUTELY STILL for a second and a half and fades out.
// The stillness is the design: this is an open-plan office and a thing that
// moves in the corner of somebody else's eye for two seconds is a thing they
// will ask to have unplugged. The entrance is the celebration; everything
// after it is just legible.
//
// It is an overlay, not a screen. The composition underneath keeps running
// and the pill lands over the two text lines, which is the one band of the
// panel whose content is the least urgent thing on it.
//
// The colours are the two on this device that do not mean "look at me now":
// the `done` green for a turn, and the ambient teal for tokens. Amber and red
// are reserved for a question, and a celebration must never borrow either.
// ---------------------------------------------------------------------------
static void renderMilestone(uint32_t nowMs) {
  int32_t signedAge = (int32_t)(nowMs - mileStartMs);
  if (signedAge < 0) signedAge = 0;      // see the note over loop()
  const uint32_t age = (uint32_t)signedAge;
  float a;
  if      (age < MILE_IN_MS)             a = (float)age / (float)MILE_IN_MS;
  else if (age > MILE_MS - MILE_OUT_MS)  a = (float)(MILE_MS - age) /
                                             (float)MILE_OUT_MS;
  else                                   a = 1.0f;
  a = clampf(a, 0.0f, 1.0f);
  const float e = a * a * (3.0f - 2.0f * a);      // smoothstep

  const uint16_t base = (mileKind == MILE_TOKENS) ? C_AMB_TEAL : C_DONE;
  const int ox = gOx;
  const int oy = gOy + (int)lroundf((1.0f - e) * 7.0f);
  const int x  = SCREEN_W / 2 - mileW / 2 + ox;

  // A filled plate rather than a stroke, because this one has to lift off the
  // composition underneath it instead of sitting in it. Dark enough that it
  // never becomes the brightest thing in the room.
  gfx->fillRoundRect(x, MILE_Y + oy, mileW, MILE_H, MILE_R,
                     xf(scaleColor(base, 0.16f * e)));
  gfx->drawRoundRect(x, MILE_Y + oy, mileW, MILE_H, MILE_R,
                     xf(scaleColor(base, 0.60f * e)));

  gfx->setTextSize(1);
  gfx->setTextDatum(textdatum_t::middle_center);
  gfx->setFont(&fonts::Font4);
  gfx->setTextColor(xf(scaleColor(base, e)));
  gfx->drawString(mileVal, SCREEN_W / 2 + ox, MILE_VAL_Y + oy);

  gfx->setFont(&fonts::Font0);
  gfx->setTextColor(xf(scaleColor(C_TEXT_DIM, 0.95f * e)));
  gfx->drawString(mileTag, SCREEN_W / 2 + ox, MILE_TAG_Y + oy);
}

// How far along the factory-reset hold the fingers currently are, 0 to 1, or
// -1 when that gesture is not in flight. Everything it tests is the same
// condition pumpButton fires on, so the bar cannot promise a wipe that will
// not happen: not while the first run owns the screen, not outside ambient
// mode, not once either press has already been spent on a gesture of its own.
static float chordHoldProgress(uint32_t nowMs) {
  if (firstRunActive) return -1.0f;
  if (!ambientMode)   return -1.0f;
  // The timer screen refuses the wipe, so it must refuse the bar as well. It
  // is the one modal screen the gesture could otherwise still reach, and a
  // paused run can hold that screen indefinitely (it never idle-closes), so
  // this is not a corner: the same test guards the wipe itself in pumpButton.
  if (focusVisible(nowMs)) return -1.0f;
  if (button1DownMs == 0 || button2DownMs == 0) return -1.0f;
  if (button1Held || button2Held)               return -1.0f;
  const uint32_t held = nowMs - button1DownMs;
  if (held < CHORD_HINT_MS)     return -1.0f;
  if (held >= FACTORY_HOLD_MS)  return 1.0f;
  return (float)(held - CHORD_HINT_MS) /
         (float)(FACTORY_HOLD_MS - CHORD_HINT_MS);
}

// The bar. Red, because red on this device means "look at me now" and this is
// the one moment that earns it. It clears the two text lines underneath rather
// than sharing them: this must not read as part of whatever composition it
// landed on. No drift and no breathe, because it is on the panel for three and
// a half seconds at most and it is a warning, not furniture.
static void renderChordHint(float p) {
  gfx->fillRect(0, CHORD_HINT_TOP, SCREEN_W, SCREEN_H - CHORD_HINT_TOP, C_BG);

  gfx->setTextSize(1);
  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(textdatum_t::top_center);
  gfx->setTextColor(C_WAIT_HOT);
  gfx->drawString("FACTORY RESET", SCREEN_W / 2, CHORD_HINT_Y);

  const int w = SCREEN_W - 2 * CHORD_BAR_X;
  gfx->fillRect(CHORD_BAR_X, CHORD_BAR_Y, w, CHORD_BAR_H,
                scaleColor(C_TEXT_DIM, 0.30f));
  const int fw = (int)lroundf((float)w * clampf(p, 0.0f, 1.0f));
  if (fw > 0) {
    gfx->fillRect(CHORD_BAR_X, CHORD_BAR_Y, fw, CHORD_BAR_H, C_WAIT_HOT);
  }
}

// The other four holds. The factory chord was the only gesture on this device
// that said what it was doing while it was doing it, and it is also the only
// one that cannot be undone. The other four (face swap, toy, timer reset,
// timer length) are 800ms or 2000ms of a finger on a switch with nothing on
// the panel to say how much longer, and letting go early does not do nothing:
// the release runs the TAP instead, so a 1.9 second reach for the toy becomes
// the stats screen and a 700ms reach for a face becomes a brightness step.
// Both are recoverable and neither is learnable, and the two closest
// thresholds are 1200ms apart on the same finger.
//
// Everything here is a condition the pumps actually fire on, so the bar never
// promises a gesture that will not arrive: not while a modal screen is up
// (those refuse to arm a hold at all), not when the other button is also down
// (that is the chord, which has its own bar), and not when the screen has
// changed since the press edge, which is the same test the pumps re-gate on.
static float holdHintProgress(uint32_t nowMs) {
  if (firstRunActive) return -1.0f;
  if (sayVisible(nowMs) || dndVisible(nowMs) || toyVisible(nowMs) ||
      statsVisible(nowMs)) return -1.0f;

  const bool d1 = digitalRead(PIN_BUTTON_1) == 0;
  const bool d2 = digitalRead(PIN_BUTTON_2) == 0;
  if (d1 && d2) return -1.0f;

  const bool onFocus = focusVisible(nowMs);
  uint32_t   held, thresh;
  if (d1 && button1DownMs != 0 && !button1Held) {
    if (onFocus != button1OnFocus) return -1.0f;
    // ...and the fifth refusal, which belongs to this button alone. While the
    // state is `waiting` and the prompt has already been acknowledged, every
    // modal screen above is hidden and focusVisible is false via focusBlocked,
    // so the bar armed at PLAY_HOLD_MS and filled all the way. But toyEnter
    // refuses while a prompt is pending (verified on hardware: {"play":"on"}
    // in `waiting` answers `err: play refused, prompt pending`), so the
    // gesture the full bar promised never arrives and the brightness steps
    // instead. Button 2 is deliberately not covered by this: its face cycle
    // does fire in `waiting`, so its bar is telling the truth there.
    if (!ambientMode && effectiveState(nowMs) == ST_WAITING && !onFocus) {
      return -1.0f;
    }
    held   = nowMs - button1DownMs;
    // The timer screen's own hold sits at FACE_HOLD_MS on this button; every
    // other screen this can be reached on opens the toy at PLAY_HOLD_MS.
    thresh = onFocus ? FACE_HOLD_MS : PLAY_HOLD_MS;
  } else if (d2 && button2DownMs != 0 && !button2Held) {
    if (onFocus != button2OnFocus) return -1.0f;
    held   = nowMs - button2DownMs;
    thresh = FACE_HOLD_MS;
  } else {
    return -1.0f;
  }

  if (held < HOLD_HINT_MS) return -1.0f;
  if (held >= thresh)      return 1.0f;
  return (float)(held - HOLD_HINT_MS) / (float)(thresh - HOLD_HINT_MS);
}

// Two pixels along the very top edge, filling toward the threshold. Dim, and
// deliberately not red: this is an affordance, not a warning, and the only
// warning colour on this device belongs to the wipe.
static void renderHoldHint(float p) {
  const int w = SCREEN_W - 2 * HOLD_HINT_X;
  gfx->fillRect(HOLD_HINT_X, HOLD_HINT_Y, w, HOLD_HINT_H,
                scaleColor(C_TEXT_DIM, 0.22f));
  const int fw = (int)lroundf((float)w * clampf(p, 0.0f, 1.0f));
  if (fw > 0) {
    gfx->fillRect(HOLD_HINT_X, HOLD_HINT_Y, fw, HOLD_HINT_H,
                  scaleColor(C_TEXT, 0.55f));
  }
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
  float px, py;
  driftPhase(nowMs, &px, &py);
  gOx = (int)lroundf(AMB_DRIFT_AX * px * driftK);
  gOy = (int)lroundf(AMB_DRIFT_AY * py * driftK);

  // The first run is 9.25 seconds and lands on the authored geometry, so it
  // opts out of the burn-in drift entirely: 9.25 seconds cannot burn anything
  // in, and a composition that wanders while it is introducing itself looks
  // like it is sliding off the panel. The cross-fade is likewise held settled,
  // because the sequence does its own fades.
  if (firstRunActive) {
    gOx = 0;
    gOy = 0;
    gXfade = 1.0f;
  }

#ifdef FPS_DEBUG
  uint32_t t0 = micros();
#endif

  // Retire an expired card before deciding what to draw, so the frame that
  // hands the screen back is the same one that re-arms the cross-fade.
  updateSay(nowMs);
  updateDnd(nowMs);
  // The two ways the game has to end are checked here rather than inside its
  // renderer, so a message card sitting on top of it cannot delay either one.
  toyTick(nowMs);
  // Same for the stats screen, and one more: midnight has to arrive whether or
  // not anybody is looking at the numbers when it does.
  statsTick(nowMs);
  statsClockTick(nowMs);
  // And the timer, for the same reason and one more: this one is the clock
  // itself, so it has to run on the frames where its screen is not drawn.
  focusTick(nowMs);

  gfx->fillScreen(C_BG);
  if (firstRunActive)           renderFirstRun(nowMs, dtMs);
  else if (sayVisible(nowMs))   renderSay(nowMs);
  else if (dndVisible(nowMs))   renderDnd(nowMs);
  else if (toyVisible(nowMs))   renderToy(nowMs, dtMs);
  else if (statsVisible(nowMs)) renderStats(nowMs);
  else if (focusVisible(nowMs)) renderFocus(nowMs);
  else if (ambientMode) {
    renderAmbient(nowMs, dtMs);
    // A run that is out of sight, drawn as two pixels along the bottom edge.
    // Under the flourish in the same order it is under every modal screen.
    renderFocusTick(nowMs);
    // The flourish is an overlay on the two base compositions and on nothing
    // else. Every modal screen outranks it, and its clock runs underneath one,
    // so a milestone that happened behind a message card is missed rather than
    // saved up.
    if (mileVisible(nowMs)) renderMilestone(nowMs);
  } else {
    renderLive(nowMs, dtMs);
    renderFocusTick(nowMs);
    if (mileVisible(nowMs)) renderMilestone(nowMs);
  }
  // Last of all, over everything including the modal screens: the only hold on
  // this device that cannot be undone is the only one that says what it is
  // doing while it is doing it.
  {
    const float hold = holdHintProgress(nowMs);
    if (hold >= 0.0f) renderHoldHint(hold);
    const float chord = chordHoldProgress(nowMs);
    if (chord >= 0.0f) renderChordHint(chord);
  }

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
                  firstRunActive        ? "firstrun"
                  : sayVisible(nowMs)   ? "say"
                  : dndVisible(nowMs)   ? "dnd"
                  : toyVisible(nowMs)   ? "play"
                  : statsVisible(nowMs) ? "stats"
                  : focusVisible(nowMs) ? (focusFin ? "focus-done" : "focus")
                  : mileVisible(nowMs)  ? (ambientMode ? "ambient+mile"
                                                       : "live+mile")
                  : ambientMode         ? "ambient"
                                        : "live",
                  (unsigned long)fpsFrames,
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
// Button 1 (GPIO 0, active low). What it actually does, in order:
//
//   press    dismiss whatever modal screen is up, else acknowledge a pending
//            prompt (the one action that cannot wait for the release)
//   release  the run control on the timer screen, else open the stats screen,
//            and the brightness ladder only when the stats screen refuses,
//            which is only ever while a prompt is pending
//   800ms    on the timer screen: back to the top of the length, refused while
//            the clock is running
//   2s       the toy
//   5s+b2    the factory reset, ambient only
//
// The brightness ladder is NOT a general fallback on this button any more.
// An older comment here claimed it was kept so a unit whose GPIO 14 switch is
// unreachable in its case still had a brightness control; that is exactly what
// the stats screen spent, and firmware/README.md ("The button model, again")
// is the accounting. On such a unit button 1 reaches the stats screen and the
// timer, and brightness is button 2 alone.
// ---------------------------------------------------------------------------
// The brightness half of button 1, unchanged in behaviour and lifted out only
// so the tap path has one home.
//
// The wake case is tracked with an explicit flag rather than inferred from
// brightIdx. Inferring it made the "cycle" a two-level toggle: from 255 the
// else-branch gave 160, and from 160 the if-branch went straight back to 255,
// so 70 and 20 were unreachable by any sequence of presses.
static void button1Brightness() {
  if (dimmedByAuto) {
    setBrightness(BRIGHT_FULL_IDX);              // first press wakes
    dimmedByAuto = false;
  } else {
    setBrightness((uint8_t)((brightIdx + 1) % BRIGHT_LEVEL_COUNT));
  }
  brightManual = true;
}

// The two-button chord, resolved on the FIRST release.
//
// Why release and not a threshold: every other gesture here fires the moment
// its threshold is crossed, because that feels like a button rather than like
// a timeout. This one cannot, because the same two switches held five seconds
// longer are the factory reset, and putting a DO NOT DISTURB sign up on the
// way to wiping a board is exactly the "surprise the gesture never asked for"
// that already keeps button 2 from swapping the face on that journey.
//
// Either way both releases are swallowed, so a chord never also steps the
// brightness twice on the way back up, and a chord that turned into the
// factory reset has already done its work.
static bool chordRelease(uint32_t nowMs) {
  if (!chordArmed) return false;
  chordArmed = false;
  // A minimum AND a maximum. Without the ceiling a chord held past the five
  // second wipe still toggled the sign on the way back up, and in live mode
  // (where the wipe is refused and neither press is ever marked held) it did
  // so with no upper bound at all: somebody who read "hold both, forget
  // everything" on the card and tried it on a running session got a DO NOT
  // DISTURB sign across the panel instead of the nothing they should have got.
  //
  // The ceiling is CHORD_HINT_MS and not FACTORY_HOLD_MS, because the ceiling
  // has to be the moment the panel starts making a promise. From 1500ms
  // renderChordHint blacks out the bottom half and prints FACTORY RESET in red,
  // and CHORD_HINT_MS's own comment says the bar is there so that "letting go
  // is the obvious way out". With the ceiling at 5000ms every release in the
  // 1500..5000ms window, which is exactly the window in which the panel is
  // shouting FACTORY RESET, raised a full-screen DO NOT DISTURB sign instead.
  // That is the path a first-time user takes: the card says hold both, they
  // hold, they see red, they let go, and they get a violet sign they never
  // asked for. Now letting go while the warning is up does what the warning
  // implies, which is nothing.
  const uint32_t held = nowMs - chordDownMs;
  const bool tap = !button1Held && !button2Held &&
                   held >= CHORD_MIN_MS && held < CHORD_HINT_MS;
  // A squeeze too short to count used to be silent in every direction: no
  // sign, nothing on the panel, and nothing on the wire, because both presses
  // are marked held below and neither release runs its own tap either. Say so,
  // so the gesture is at least provable over the cable.
  const bool tooShort = !button1Held && !button2Held && held < CHORD_MIN_MS;
  button1Held = true;
  button2Held = true;
  if (tap)           dndToggle(nowMs, "buttons");
  else if (tooShort) Serial.printf("err: chord too short, %lums\n",
                                   (unsigned long)held);
  return true;
}

static void pumpButton(uint32_t nowMs) {
  // The first run owns the device for its 9.25 seconds. Both gestures on this
  // button start or restart it, and the brightness ladder is suspended anyway,
  // so there is nothing here that could do anything but interfere.
  if (firstRunActive) return;

  bool level = digitalRead(PIN_BUTTON_1) != 0;   // true == released (pull-up)
  bool b2Down = digitalRead(PIN_BUTTON_2) == 0;  // is the other one held too

  if (level != lastButtonLevel) {
    if (!level) {                                // pressed
      // The debounce is tested BEFORE the tracker is committed. Committing
      // first left lastButtonLevel saying "pressed" for a press that had been
      // thrown away, so the real edge behind it was never seen at all: the
      // release found button1DownMs at 0 and did nothing, and both holds on
      // this button were unreachable for the whole window. Leaving the level
      // uncommitted means the press is simply seen again once it settles.
      if (nowMs - lastButtonMs < BUTTON_DEBOUNCE_MS) return;
      lastButtonLevel = level;
      lastButtonMs  = nowMs;
      button1DownMs = nowMs;
      button1Held   = false;
      // Which screen this press landed on. A hold is delivered only while that
      // is still the screen on the panel: see the re-gate below.
      button1OnFocus = focusVisible(nowMs);

      // The panel is off because somebody chose it. Buy the light back and
      // spend the press doing it, ahead of every screen test below: a stats
      // screen opened on a dark panel is a press that did nothing anybody can
      // see. Marked held, so the release does not also step the ladder.
      if (brightIsOff()) {
        brightWake();
        button1Held = true;
        return;
      }

      // A message card takes both buttons for as long as it is up: the press
      // dismisses it and does nothing else. That is the whole interaction, on
      // either button, with no host and nothing to learn, and it is the reason
      // a card with no expiry is safe to offer at all. Marking the press held
      // swallows the release, so dismissing never also steps the brightness,
      // and the hold gestures start clean from the NEXT press.
      if (sayVisible(nowMs)) {
        sayDismiss(nowMs, "cleared (button)");
        button1Held = true;
        return;
      }

      // The do-not-disturb sign is modal in exactly the same way, and for a
      // stronger reason: it is the only screen here that stays up until
      // somebody takes it down, so ANY single press has to take it down. That
      // is the whole reason a chord is what puts it up. Prior art in this repo
      // is unambiguous about which half is dangerous: a mode with no way out
      // (docs/prior-art-status-light.md on the hooks port latching amber with
      // no PermissionResolved event) is a bug, while a mode that is one press
      // to leave costs a stray press nothing at all.
      if (dndVisible(nowMs)) {
        dndExit(nowMs, "button");
        button1Held = true;
        return;
      }

      // The toy is modal in exactly the same way, and it is the one modal
      // screen here that is played rather than read, so the two buttons split
      // instead of both doing the same thing: this one is the whole game.
      // On the press edge, like the acknowledgement, because a jump that waits
      // for the release is a jump that arrives after the block.
      if (toyVisible(nowMs)) {
        toyHop(nowMs, /*fromButton=*/true);
        button1Held = true;
        return;
      }

      // The stats screen. Button 1 is "tell me something" and there are now two
      // things it can tell you, so on this screen it goes ON to the timer
      // rather than closing. Button 2 still closes, which is the rule that
      // cuts across every modal screen here. The legend on the panel says both.
      if (statsVisible(nowMs)) {
        statsExit(nowMs, "button");
        focusShow(nowMs, "button");
        button1Held = true;
        return;
      }

      // The timer. Both of its buttons resolve on the RELEASE, because both
      // also carry a hold on this screen and the press edge cannot yet tell
      // which one it is; a tap is still a tap and nobody can feel the
      // difference. The finish is the exception, because it is a sign and not
      // a control: any press takes it down, exactly as it does for the message
      // card and the do-not-disturb sign.
      if (focusVisible(nowMs)) {
        focusTouch(nowMs);
        if (focusFin) {
          focusFinishClear(nowMs, "button");
          button1Held = true;
        }
        return;
      }

      // Acknowledge takes priority over everything, and it is the one action
      // that still fires on the PRESS edge: an ack has to feel instant, and
      // waiting for the release to tell a tap from a hold would put a visible
      // delay on the single most time-critical thing this button does.
      // Marking the press as "held" swallows both the release and the hold, so
      // an ack can never also step the brightness or start a replay.
      if (!ambientMode && !waitAcked && effectiveState(nowMs) == ST_WAITING) {
        waitAcked   = true;
        button1Held = true;
        if (dimmedByAuto) {                      // an ack is an interaction too
          setBrightness(BRIGHT_FULL_IDX);
          dimmedByAuto = false;
          brightManual = true;
        }
      }

      // Both buttons down at once: arm the chord. Not if this press was
      // already spent on an acknowledgement, and not if the other button's
      // press was spent on something of its own, because a gesture that has
      // already fired is not half of a new one.
      if (!button1Held && button2DownMs != 0 && !button2Held) {
        chordArmed  = true;
        chordDownMs = nowMs;
      }
      return;
    }

    lastButtonLevel = level;                     // released
    // Stamped on THIS edge too, not just on the press. The debounce test above
    // is `nowMs - lastButtonMs < BUTTON_DEBOUNCE_MS`, and with the stamp on the
    // press alone that window was measured from the original press rather than
    // from the release: for any press held longer than 40ms, which is every
    // real tap, the test was already false and a contact bounce on the way up
    // was admitted as a genuine new press. The two edges do different things
    // here, so that is not cosmetic: this release opens the stats screen, and
    // a bounce press immediately behind it would find statsVisible true, take
    // the stats branch above and land the owner on the focus timer, having
    // passed through stats in a frame. A debounce that only guards one edge is
    // not a debounce.
    lastButtonMs = nowMs;
    // Released. A hold that already fired swallows its own release, which is
    // what stops a replay or a factory reset from also stepping the
    // brightness on the way back up. A chord resolves here for the same
    // reason, and takes both releases with it.
    if (chordRelease(nowMs)) { button1DownMs = 0; return; }
    // A plain tap now asks for the numbers. The brightness step it used to be
    // is what paid for the stats screen, and it is still exactly what happens
    // whenever the numbers cannot be shown -- which is only ever while a
    // prompt is pending -- so this button is never a dead press. The whole
    // argument is in firmware/README.md, "The button model, again".
    if (!button1Held && button1DownMs != 0) {
      // On the timer screen this button is the run control and nothing else.
      if (focusVisible(nowMs)) {
        focusTouch(nowMs);
        focusToggle(nowMs, "button");
      } else if (!statsEnter(nowMs, "button")) {
        button1Brightness();
      }
    }
    button1DownMs = 0;
    return;
  }

  if (level || button1Held || button1DownMs == 0) return;

  // Re-gate. The press path above refuses to arm anything while a modal screen
  // is up, and a hold in flight has to answer the same question every pump: a
  // host line landing inside the two second window ({"say":...} or {"dnd":true}
  // during a hold) used to leave the toy opening underneath the new card and
  // appearing the moment somebody dismissed it, which from the owner's side is
  // a game materialising unbidden. A hold whose screen changed under it is
  // abandoned, not delivered late.
  // The timer is in this list by way of a comparison rather than a name,
  // because it is the one screen here that can arrive on its own: a running
  // clock reaching zero makes it visible with no host and no press. Without
  // that comparison a hold started on the face was delivered to whichever
  // branch matched at the threshold, so a two second reach for the toy became
  // "reset the timer" and silently retired a finish nobody had seen yet.
  if (sayVisible(nowMs) || dndVisible(nowMs) || toyVisible(nowMs) ||
      statsVisible(nowMs) || focusVisible(nowMs) != button1OnFocus) {
    button1Held = true;
    return;
  }

  uint32_t held = nowMs - button1DownMs;

  // Both buttons, five seconds, ambient only: wipe the board. Two switches at
  // opposite ends of the board held together for five seconds is not something
  // a hand does by accident, and ambient-only means no host has spoken for
  // five minutes, so there is no session and no name in use to destroy.
  // button2Held is set as well, so button 2 swallows its release too and does
  // not step the brightness when the fingers come off.
  // Two more tests than this used to carry, and the warning bar draws on
  // exactly the same set (chordHoldProgress):
  //   !button2Held      button 2's press may already have been spent on a face
  //                     swap, which the bar refuses to draw for. It has to
  //                     refuse the wipe too, or the one gesture that cannot be
  //                     undone fires with nothing on screen to announce it.
  //   !focusVisible     every other modal screen is refused here by the
  //                     re-gate above; the timer was the one that was not, and
  //                     a paused run can hold it on the panel indefinitely.
  if (b2Down && !button2Held && !focusVisible(nowMs) &&
      ambientMode && held >= FACTORY_HOLD_MS) {
    button1Held = true;
    button2Held = true;
    Serial.println(factoryReset() ? "reset: factory ok (buttons)"
                                  : "reset: factory ram-only (buttons)");
    // Straight into the sequence, so the gesture confirms itself by showing
    // exactly what the next owner will see. It is a replay, not a spend: the
    // flag this just re-armed has to survive for the recipient.
    startFirstRun(nowMs, false);
    return;
  }

  // The timer screen's hold: back to the top of the chosen length. 800ms and
  // not the toy's two seconds, because 800ms is the hold threshold this device
  // already has and adding a fifth number to the four it already carries is
  // exactly the pile-on firmware/README.md warns about. It is only ever
  // reachable while the timer screen is the thing on the panel.
  // `!focusFin` because a hold that was already in flight when the countdown
  // reached zero cannot be caught by the re-gate above: focusVisible is true
  // on both sides of the expiry, so the button1OnFocus comparison passes, and
  // the else-branch below then called focusReset, which clears focusFin and
  // focusFinMs and retires a "FOCUS / n MINUTES DONE" panel nobody ever saw.
  // The window is the 800ms of a press that begins just before zero, which is
  // exactly when somebody reaches for a nearly finished timer. The press edge
  // already handles the finish correctly (focusFinishClear); this is the same
  // rule for the hold path, which never got it.
  if (!b2Down && focusVisible(nowMs) && !focusFin && held >= FACE_HOLD_MS) {
    button1Held = true;
    focusTouch(nowMs);
    if (focusAtRest()) {
      // The legend on this screen says "1 START" while the timer is at rest,
      // and a slow press used to deliver nothing at all: focusReset on an
      // at-rest timer is a no-op (focusRun already false, focusLeftMs already
      // focusFullMs(), focusFin already false), and setting button1Held then
      // swallowed the release so focusToggle never ran either. Verified over
      // the wire: {"focus":"reset"} on an at-rest timer answers
      // `focus: reset (host) len=1m` and changes nothing. FACE_HOLD_MS is only
      // 800ms, which a deliberate press reaches easily, and the hold hint bar
      // filled to 100% on the way to confirm that something had happened. So
      // 1 START means START whether the finger is fast or slow. Nothing is
      // lost, because reset at rest had nothing to do.
      focusStart(nowMs, "button");
    } else if (focusRun) {
      // Refused while the clock is running, exactly like button 2's length
      // hold on this same screen. The asymmetry was the bug: the guarded hold
      // was the harmless one and the unguarded hold silently discarded a 25
      // minute pomodoro, on the button whose on-screen label says PAUSE. So a
      // slow finger gets the thing the legend promises instead of nothing and
      // instead of a wipe.
      Serial.println("err: focus reset locked, timer is running");
      focusPause(nowMs, "button");
    } else {
      focusReset(nowMs, "button");
    }
    return;
  }

  // Button 1 alone, two seconds: open the toy. Deliberately on button 1,
  // because button 2's hold is already the face cycle at 800ms and two holds
  // on one button would be a guessing game.
  //
  // This is the threshold that used to replay the first run, taken rather than
  // added: the device has a fifth screen and still has four thresholds. The
  // refusal while a prompt is pending lives inside toyEnter, so the gesture and
  // the protocol field cannot disagree about when it is safe.
  if (!b2Down && !focusVisible(nowMs) && held >= PLAY_HOLD_MS) {
    button1Held = true;
    // A refusal is not a reason for the gesture to do nothing at all. toyEnter
    // refuses only while a prompt is pending, which is the one situation in
    // which this button's ordinary release action (the stats screen) is also
    // refused and falls to the brightness ladder, so that is what a refused
    // hold does too. The rule stays "this button is never a dead press", and
    // the refusal is still on the wire for anyone reading it.
    if (!toyEnter(nowMs, "button")) button1Brightness();
  }
}

// ---------------------------------------------------------------------------
// Button 2 (GPIO 14, active low, same INPUT_PULLUP wiring as button 1 in every
// vendor example that reads it).
//
// Tap: one brightness step, exactly as before. Unlike button 1 it never
// wakes-then-cycles, which is what makes it predictable as the brightness
// control.
//
// Hold FACE_HOLD_MS: swap to the next face, and remember it. That is on this
// button because comparing faces is a bench activity done with the board in
// your hand, and because it cannot be reached any other way without a host.
//
// The one behavioural change: a tap now acts on RELEASE rather than on press,
// because the press edge is no longer enough to tell a tap from a hold. The
// hold fires the moment the threshold is crossed rather than on release, so it
// feels like a button and not like a timeout, and the release that follows is
// swallowed instead of also stepping the brightness.
// ---------------------------------------------------------------------------
static void pumpButton2(uint32_t nowMs) {
  if (firstRunActive) return;                    // see pumpButton
  bool level = digitalRead(PIN_BUTTON_2) != 0;   // true == released (pull-up)

  if (level != lastButton2Level) {
    if (!level) {                                // pressed
      // Tested before the tracker is committed, for the reason spelled out in
      // pumpButton: a press thrown away by the debounce must leave the tracker
      // saying "released", or the real edge behind it is lost entirely.
      if (nowMs - lastButton2Ms < BUTTON_DEBOUNCE_MS) return;
      lastButton2Level = level;
      lastButton2Ms = nowMs;
      button2DownMs = nowMs;
      button2Held   = false;
      button2OnFocus = focusVisible(nowMs);
      // Same wake rule as button 1, and on this button it is the other half of
      // the same ladder: one more press after the off rung brings the light
      // back rather than wrapping blind to full through a screen nobody saw.
      if (brightIsOff()) {
        brightWake();
        button2Held = true;
        return;
      }
      // Same rule as button 1: while a card is up this button only dismisses
      // it, and the release it swallows does not step the brightness.
      if (sayVisible(nowMs)) {
        sayDismiss(nowMs, "cleared (button)");
        button2Held = true;
        return;
      }
      // Same rule again for the sign: either button, one press, it is gone.
      if (dndVisible(nowMs)) {
        dndExit(nowMs, "button");
        button2Held = true;
        return;
      }
      // The toy's other half. Button 1 plays it and this one leaves it, on the
      // press edge and with no hold to learn: abandoning a game has to be
      // cheaper than starting one, and the score is banked on the way out so a
      // stray press can never cost a record.
      if (toyVisible(nowMs)) {
        toyExit(nowMs, "button");
        button2Held = true;
        return;
      }
      // The stats screen takes the plain modal rule on this button too: one
      // press, either button, and it is gone. See pumpButton.
      if (statsVisible(nowMs)) {
        statsExit(nowMs, "button");
        button2Held = true;
        return;
      }
      // The timer. Leaving resolves on the RELEASE here, unlike every other
      // modal screen, because this button also carries the length hold on this
      // screen. The finish is a sign, so it goes on the press like the others.
      if (focusVisible(nowMs)) {
        focusTouch(nowMs);
        if (focusFin) {
          focusFinishClear(nowMs, "button");
          button2Held = true;
        }
        return;
      }
      // The other half of the chord. See pumpButton.
      if (button1DownMs != 0 && !button1Held) {
        chordArmed  = true;
        chordDownMs = nowMs;
      }
      return;
    }
    lastButton2Level = level;                    // released
    // Stamped on this edge too, for the reason spelled out in pumpButton: the
    // debounce window on the next press has to be measured from the release
    // and not from a press that may have been held for seconds. On this button
    // an admitted release bounce steps the brightness twice and skips a tier.
    lastButton2Ms = nowMs;
    // Released. A press swallowed by the debounce never reached this path at
    // all: it left the tracker on "released", so there is no transition here
    // to answer for.
    if (chordRelease(nowMs)) { button2DownMs = 0; return; }
    if (!button2Held && button2DownMs != 0) {
      if (focusVisible(nowMs)) {
        // Leaving does not stop the clock. The hairline along the bottom of
        // the composition underneath is what says so.
        focusTouch(nowMs);
        focusHide(nowMs, "button");
      } else {
        setBrightness((uint8_t)((brightIdx + 1) % BRIGHT_LEVEL_COUNT));
        brightManual = true;
        dimmedByAuto = false;
      }
    }
    button2DownMs = 0;
    return;
  }

  // The face cycle is suppressed while button 1 is also down, because that
  // combination is on its way to the factory reset and swapping the face at
  // 800ms on the way through would be a surprise the gesture never asked for.
  if (!level && !button2Held && button2DownMs != 0 &&
      digitalRead(PIN_BUTTON_1) != 0 &&
      (nowMs - button2DownMs) >= FACE_HOLD_MS) {
    // Re-gated for the same reason button 1's hold is: a card or a sign that
    // arrived after the press must not find this hold swapping the face
    // underneath it.
    // Same comparison as pumpButton's re-gate, and for the same reason: a
    // timer that arrived during the hold turned a face swap into a length
    // change on a screen the finger never landed on.
    if (sayVisible(nowMs) || dndVisible(nowMs) || toyVisible(nowMs) ||
        statsVisible(nowMs) || focusVisible(nowMs) != button2OnFocus) {
      button2Held = true;
      return;
    }
    button2Held = true;
    // One gesture, one threshold, one meaning: hold button 2 to step a stored
    // choice. On the base compositions that choice is the face; on the timer
    // screen it is the length. Both persist to the same NVS namespace, and
    // both are refused in exactly one case (see focusCycleLength) with the
    // release swallowed either way, so a refusal cannot also close the screen.
    if (focusVisible(nowMs)) {
      focusTouch(nowMs);
      // A refusal is not a reason for the gesture to do nothing at all. The
      // length hold is refused while the clock is running, and the swallowed
      // release meant somebody holding this button to be sure they were
      // closing the timer got no response of any kind. The legend on that
      // screen says 2 CLOSE, so a refused hold closes it.
      if (!focusCycleLength(nowMs, "button")) focusHide(nowMs, "button");
    } else {
      cycleFace();
    }
  }
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
  // The first run drives the backlight itself, from 0 upward: the light coming
  // on IS the first beat. The ladder resumes when it settles.
  if (firstRunActive) return;
  uint8_t tier = 0;

  if (toyVisible(nowMs)) {
    // Somebody is pressing buttons at it, and it closes itself after thirty
    // seconds of nobody doing that, so it never sits lit on an empty desk.
    tier = 0;
  } else if (statsVisible(nowMs)) {
    // Same argument, and a shorter timeout: somebody just asked to read six
    // small numbers, and the screen is gone twenty seconds later whatever
    // happens. There is no version of this that sits lit on an empty desk.
    tier = 0;
  } else if (focusVisible(nowMs)) {
    // The finish has to be noticed from across a room, so it runs at full for
    // its half minute. The countdown itself can legitimately sit on a desk for
    // forty-five minutes, so it takes the ambient ladder instead: properly lit
    // for a minute, then a tier down for as long as it lasts. Same argument as
    // the sign, and design law 4 is the reason for both.
    tier = focusFin ? 0
                    : ((nowMs - focusOpenMs) >= AMBIENT_BRIGHT_MS ? 1 : 0);
  } else if (dndVisible(nowMs)) {
    // The sign follows the ambient ladder rather than the live one: it is a
    // composition that can legitimately be up for an hour, so it stays
    // properly lit for a minute and then settles a tier down for as long as it
    // lasts. Full backlight on a violet frame all evening is the open-plan
    // failure design law 4 exists to prevent, and the sleep tier would make a
    // sign nobody can read from the doorway.
    tier = (nowMs - dndStartMs) >= AMBIENT_BRIGHT_MS ? 1 : 0;
  } else if (ambientMode) {
    // Ambient is the resting look, not a fault, so it stays properly lit for a
    // minute (a freshly plugged-in unit should look like it is showing you
    // something) and then settles one tier down for the rest of its life. It
    // never reaches the sleep tier: the whole point is that it stays readable.
    tier = (nowMs - ambientEnterMs) >= AMBIENT_BRIGHT_MS ? 1 : 0;
  } else {
    // Signed, same reason as ambientActive: an unsigned subtract of a
    // lastDataMs that is a millisecond in the future wraps and dimmed the
    // backlight for a frame on a board nothing was wrong with.
    if ((int32_t)(nowMs - lastDataMs) >= (int32_t)STALE_DIM_MS) tier = 1;

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

  // A hand-picked "off" outranks the auto ladder, which otherwise lit a panel
  // somebody had deliberately turned off the moment any tier changed: ambient's
  // sixty second settle alone would have undone it. The tier tracker is kept
  // current rather than frozen, so the wake press lands on full and stays
  // there instead of being corrected a frame later by a tier that moved in the
  // dark.
  //
  // One state is not allowed to be hidden this way, and it is the same one
  // that overrides everything else on this device: an unanswered prompt. A
  // person who turned the screen off has not thereby agreed to miss the
  // question their session is blocked on, so `waiting` buys the light back. It
  // is also the only state that can arrive with nobody touching the board.
  if (brightIsOff()) {
    if (!ambientMode && eff == ST_WAITING && !waitAcked) {
      brightWake();                    // an unanswered prompt takes the panel
    } else {
      lastTier     = tier;
      dimmedByAuto = false;
      return;
    }
  }

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
  //
  // 16 KB and not 1 KB, because a burst is delivered at USB speed and not at
  // the nominal 115200: a host's single write() lands in this ring long before
  // the next drain, and whatever does not fit is dropped by the driver's ISR
  // with no flow control and no notice. Measured on the board at 1024, with a
  // rendered frame holding the CPU for 15 to 18ms: one write() of 500 lines
  // (14000 bytes) arrived as its first 36 lines and nothing else, and 200
  // lines lost the same way, in one contiguous run. 16 KB holds every burst a
  // host can produce between two drains and costs 16 KB of the 300 KB this
  // firmware never uses. It is not unbounded and nothing here can make it
  // unbounded, which is why loop() also drains twice per frame.
  Serial.setRxBufferSize(16384);
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

  // Every registered face gets its init, not just the active one, so a swap
  // mid-animation is a pointer change and nothing else. Then the stored choice
  // wins over the compile-time default.
  for (uint8_t i = 0; i < faceCount(); ++i) faceAt(i)->init();
  loadFaceChoice();

  // Whether this board is armed decides what the boot screen is allowed to be,
  // so the flag has to be in RAM before the first pixel.
  loadFirstRunFlag();

  // The personal best. Nothing shows it until somebody opens the toy, but it
  // is read here with the rest of the stored state so there is one place that
  // knows what a virgin board looks like.
  loadHopBest();

  // The two records, read with everything else so there is one place that
  // knows what a virgin board looks like. The DAILY figures are deliberately
  // not loaded, because there is nothing to load: they are RAM, and a board
  // that has just booted has not seen today yet.
  loadStatsRecords();

  // The focus timer's chosen length, read here with everything else so there
  // is one place that knows what a virgin board looks like. It also arms the
  // remaining-time counter, which is why it cannot wait until somebody opens
  // the screen.
  loadFocusPrefs();

  if (gFirstRunArmed) {
    // An armed board shows NOTHING for the boot hold, on an unlit panel. The
    // wordmark is an assembly-day convenience and it is the wrong first frame
    // for a recipient: the sequence below opens on darkness and brings the
    // light up itself, and a product name flashing first would give that away.
    // The greeting still goes out at exactly BOOT_HOLD_MS, so
    // tools/flash-all.sh's verify_hello is unaffected.
    lcd.setBrightness(0);
    gfx->fillScreen(C_BG);
    if (fbReady) fb.pushSprite(0, 0);
  } else {
    drawBootScreen();
  }
  uint32_t bootStart = millis();
  while (millis() - bootStart < BOOT_HOLD_MS) {
    // Drain the RX queue during the hold. The helper is already streaming, and
    // bytes left unread here would overflow the queue and corrupt the first
    // real line.
    //
    // Only the serial pump runs here, which is what the sentence above is
    // about. The two button pumps used to run as well, and on an ARMED board
    // that is two seconds of live gestures on a panel that is deliberately
    // black with nothing drawn on it (see the branch above and FIRSTRUN.md
    // section 1): firstRunActive is still false at this point, every modal
    // screen test in both pumps is false, and pumpButton2's face cycle needs
    // only 800ms of button 2 held with button 1 up. A thumb resting on button 2
    // while the USB-C plug goes in permanently changed the stored face with no
    // light on the panel and no way for the recipient to know. Nothing is lost
    // by not pumping: lastButtonLevel and lastButton2Level are both re-read
    // from the pins immediately after this loop, so no edge is missed.
    pumpSerial();
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

  // An armed board plays the out-of-the-box sequence instead of fading into
  // whichever mode is correct. It picks the mode itself when it ends, so a
  // board plugged into a running host still lands in live.
  //
  // Spending is deliberately the LAST thing the sequence does, not the first:
  // a board unplugged halfway through is still armed and its owner still gets
  // the whole thing the next time.
  if (gFirstRunArmed) startFirstRun(millis(), true);
}

void loop() {
  static uint32_t lastDraw = 0;

  // millis() is read AFTER the serial pump, not before it, and that ordering
  // is load bearing. Everything stamped inside pumpSerial (lastDataMs, the
  // session clock, a turn's start, a milestone's start) uses its own fresh
  // millis(), so a `now` captured first can be a millisecond BEHIND those
  // stamps. Every `now - stamp` in this file is unsigned, and one millisecond
  // of negative wraps to 4.29 billion, which sails past every threshold here:
  // it flipped the device into ambient for a frame, and it would have closed a
  // freshly opened stats screen for being twenty seconds idle. One line fixes
  // the whole class. The signed compares in ambientActive, effectiveState,
  // statsTick and mileVisible are belt and braces on top of it.
  uint32_t now = millis();

  pumpSerial();
  now = millis();
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
    // Again, immediately. A frame is the only thing in this loop that holds
    // the CPU long enough to matter (15 to 18ms measured), and the RX ring
    // fills from a host that has no idea a frame is happening. Draining on
    // the far side of the draw halves the window in which a stream can
    // outrun the reader, and costs one call on the frames where nothing
    // arrived. See the buffer note in setup() for what this does not fix.
    pumpSerial();
  }
  delay(1);
}
