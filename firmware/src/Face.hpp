// The face interface.
//
// The thing on the panel is a creature, and which creature it is has changed
// several times and will change again. This header is the seam that makes that
// cheap: a face is one object with one draw call, it owns nothing outside
// itself, and swapping one for another is a runtime choice, not a rebuild.
//
// WHAT IS INSIDE A FACE
//   Only drawing. Given "the eyes are 62% open, the accent is this amber, your
//   box is here, it is 4100ms into `waiting` and it has escalated", a face
//   renders one frame. Everything a face decides is a shape, a colour or a
//   local offset.
//
// WHAT IS OUTSIDE A FACE, and always stays outside
//   the protocol parser and the state machine, the staleness timeouts, the
//   blink driver, the glance driver, the mode cross-fade, the anti burn-in
//   drift, the ambient and live compositions, the ring, the backlight ladder
//   and all text layout. A face never reads a global, never touches millis()
//   for scheduling, and above all never decides when to blink: it is told how
//   open the eyes are and draws that.
//
// See firmware/FACES.md for how to write one.

#pragma once

#include <stdint.h>

#include "LGFX_TDisplayS3.hpp"

// ---------------------------------------------------------------------------
// The five protocol states. This lives here rather than in main.cpp because a
// face is allowed to look different per state, so both ends need one enum.
// ---------------------------------------------------------------------------
enum StateId : uint8_t {
  ST_SLEEP = 0,
  ST_IDLE,
  ST_BUSY,
  ST_WAITING,
  ST_DONE,
};

// How long "done" holds its happy expression. Shared rather than private to a
// face because the blink driver suppresses blinking over exactly this window,
// so the two would drift apart if each owned its own copy.
static constexpr uint32_t DONE_HAPPY_MS = 1600;

// ---------------------------------------------------------------------------
// Colour helpers. Faces need these and so does the composition, so they live
// here as the single definition rather than being copied into each face.
// ---------------------------------------------------------------------------
static inline float faceClampf(float v, float lo, float hi) {
  if (!(v == v)) return lo;                       // NaN guard
  return v < lo ? lo : (v > hi ? hi : v);
}

// Scale an RGB565 colour toward black by f (0..1), per channel.
static inline uint16_t scaleColor(uint16_t c, float f) {
  f = faceClampf(f, 0.0f, 1.0f);
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
static inline uint16_t mixColor(uint16_t a, uint16_t b, float f) {
  f = faceClampf(f, 0.0f, 1.0f);
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r  = (int)((float)ar + (float)(br - ar) * f + 0.5f);
  int g  = (int)((float)ag + (float)(bg - ag) * f + 0.5f);
  int bl = (int)((float)ab + (float)(bb - ab) * f + 0.5f);
  if (r  < 0) r  = 0; if (r  > 0x1F) r  = 0x1F;
  if (g  < 0) g  = 0; if (g  > 0x3F) g  = 0x3F;
  if (bl < 0) bl = 0; if (bl > 0x1F) bl = 0x1F;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ---------------------------------------------------------------------------
// The geometry contract.
//
// Two independent descriptions of the same space, and a face uses whichever
// suits it:
//
//   cx/cy/eyeW/eyeH/gap  the eye-pair convention. The pair straddles cx with
//                        `gap` px of black between two eyeW x eyeH boxes, so
//                        it is (2*eyeW + gap) wide and eyeH tall at full open.
//                        A face built from two eyes should use these and
//                        inherit the ambient/live size difference for free.
//
//   boxX/Y/W/H           the whole rectangle this face owns, for a face that
//                        is a creature rather than a pair of eyes. Nothing
//                        else is ever drawn inside it. It is the region ABOVE
//                        the two text lines, so drawing to its edges is safe
//                        and drawing past them is not.
//
// The burn-in drift is ALREADY BAKED INTO BOTH. A face must never add it, and
// must never read the drift globals: that is the composition's job, and it is
// what lets live mode land on pixel-exact geometry while ambient wanders.
// ---------------------------------------------------------------------------
struct FaceGeometry {
  int cx, cy;              // centre of the face
  int eyeW, eyeH;          // one eye at full open
  int gap;                 // black between the two eyes
  int boxX, boxY;          // top left of the rectangle the face owns
  int boxW, boxH;          // its size
};

// ---------------------------------------------------------------------------
// One frame of input. Everything a face is allowed to know.
// ---------------------------------------------------------------------------
struct FaceFrame {
  uint32_t nowMs;          // millis(), for a face's own free-running motion
  uint32_t dtMs;           // ms since the previous frame, clamped to 500
  uint32_t stateAgeMs;     // how long `state` has been the rendered state.
                           // Measured from the edge that matters per state:
                           // the "done" arrival for ST_DONE, the "waiting"
                           // arrival for ST_WAITING, the last state change
                           // otherwise.

  StateId  state;          // the EFFECTIVE state, after the staleness rules
  bool     ambient;        // true in ambient mode: no host, bigger box, no
                           // ring. Advisory. The geometry already differs.
  bool     escalated;      // ST_WAITING only: past the 10s threshold
  bool     acked;          // ST_WAITING only: button 1 said "I have seen it"

  // How open the eyes are. 0 is shut, 1 is this state's resting open, and
  // above 1 is wide. It is the blink driver's factor multiplied by the state's
  // own lid expression, so a squinting busy face still blinks from its squint.
  // A FACE NEVER DECIDES THIS. It is told.
  float    open;

  // The 0..1 breathe or pulse the ring is riding this frame, so anything a
  // face animates lands on the same beat as the ring rather than near it. It
  // is the only motion channel that is not derivable from nowMs: the waiting
  // pulse period moves with `tps`, and its phase is accumulated outside.
  float    pulse;

  // Where the shared glance driver is looking, in px. Add it to the eyes and
  // not to anything that reads as part of the head.
  float    gazeX, gazeY;

  // The accent for this frame, already brightened or dimmed by the state's own
  // breathe. A face may paint everything with it (Arc), or use it only as a
  // highlight on its own palette (Bear).
  uint16_t color;

  FaceGeometry geom;
};

// ---------------------------------------------------------------------------
// Where a face draws.
//
// `g` is the back buffer, or the panel itself if both sprite allocations
// failed. `tint` is the ambient/live cross-fade: run every colour through it,
// including a face's own private palette, or the face will not fade with the
// rest of the screen. It is the identity function once a fade has settled.
// ---------------------------------------------------------------------------
struct FaceCanvas {
  LovyanGFX* g;
  uint16_t (*tint)(uint16_t);
};

// ---------------------------------------------------------------------------
// A face.
//
// One statically allocated instance per design, registered in Faces.cpp. No
// heap, no per-frame allocation, no state that has to survive a face swap:
// switching faces mid-frame is legal and costs nothing.
// ---------------------------------------------------------------------------
class Face {
 public:
  virtual ~Face() {}

  // Stable wire name, lowercase ASCII, <= 15 characters. This is what the
  // protocol's "face" field carries and what is persisted to NVS, so renaming
  // one silently retires every board already set to it.
  virtual const char* name() const = 0;

  // One line for a human, shown nowhere on the device. Documentation that
  // cannot drift away from the code.
  virtual const char* description() const = 0;

  // Called once, from setup(), for every registered face whether or not it is
  // the active one. Precompute here; there is nothing to tear down.
  virtual void init() {}

  // Draw one frame. Called exactly once per rendered frame for the active face
  // only, after the ring and before the text.
  virtual void draw(FaceCanvas& c, const FaceFrame& f) = 0;
};

// ---------------------------------------------------------------------------
// The registry, defined in Faces.cpp. Order is the button-cycle order.
// ---------------------------------------------------------------------------
uint8_t     faceCount();
Face*       faceAt(uint8_t index);
// Case-insensitive lookup by wire name. -1 when no face has that name.
int         faceIndexByName(const char* name);
// Index of the compile-time default (DEFAULT_FACE), or 0 if that name is not
// registered, so a fresh board always has something to draw.
uint8_t     defaultFaceIndex();

// Maximum wire-name length, and therefore the NVS value size.
static constexpr size_t FACE_NAME_MAX = 15;
