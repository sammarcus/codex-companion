// Face: "rounded" -- the default, and the one Sam picked over the alternatives.
//
// Two soft rectangles with a wet highlight, an upward bow for a smile and two
// eyebrows that exist for exactly one state. Nothing is a bitmap: the whole
// face is a rounded rect, an arc and a line, so it scales between ambient and
// live by changing four numbers and recolours by changing one.
//
// This is a MOVE, not a rewrite. Every constant, ratio and easing below came
// out of the old inline renderer in main.cpp unchanged, and the geometry notes
// in firmware/EYES.md parts 2 and 5 still describe it line for line. The only
// thing that changed is where the per-state expression is decided: it used to
// be set on a struct inside renderLive, and it is now derived here from the
// state, the escalation flag and the ring's own pulse.

#include <math.h>

#include "Face.hpp"

namespace {

// RGB565 white, the same C_TEXT the rest of the firmware uses. The highlight
// is 55% of the way from the eye colour toward it.
constexpr uint16_t kWhite = 0xFFFF;

// One eye. A rounded rect with a corner radius of 0.42 * min(w, h): at 0.5 it
// is a stadium and reads as a pill, below about 0.3 it reads as a screen
// bezel, and 0.42 is a soft rectangle.
void drawOneEye(FaceCanvas& c, float ex, float ey, float w, float h,
                uint16_t color, bool gloss) {
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
  c.g->fillSmoothRoundRect(x0, y0, iw, ih, r, color);

  // One highlight near the top left of each eye. It is what turns two rounded
  // rects into something that looks wet, and it costs one more fill. Dropped
  // once the lid is low enough that it would collide with the eye's own edge.
  if (gloss && ih > 18 && iw > 16) {
    int gw = (int)(iw * 0.26f);
    int gh = (int)(ih * 0.20f);
    if (gw >= 3 && gh >= 3) {
      c.g->fillSmoothRoundRect(x0 + (int)(iw * 0.17f), y0 + (int)(ih * 0.16f),
                               gw, gh, gh / 2,
                               mixColor(color, kWhite, 0.55f));
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
void drawHappyEye(FaceCanvas& c, float ex, float ey, float w, uint16_t color) {
  float rr = w / 1.854f;
  if (rr < 5.0f) rr = 5.0f;
  float t = rr * 0.30f;
  if (t < 3.0f) t = 3.0f;
  float ay = ey + rr * 0.6875f;
  c.g->fillArc((int)lroundf(ex), (int)lroundf(ay), (int)lroundf(rr - t),
               (int)lroundf(rr), 202.0f, 338.0f, color);
}

// One eyebrow. `side` is -1 for the left eye and +1 for the right, so the
// tilted end is always the one nearest the nose: level reads as alert, dropped
// inward reads as "I am still waiting". Brows sit off the nominal eye height,
// not the animated one, so a blink does not drag them down onto the eye.
void drawBrow(FaceCanvas& c, float ex, float ey, float w, float nomH, int side,
              float tilt, uint16_t color) {
  float halfW = w * 0.46f;
  float base  = ey - nomH * 0.86f;
  float inner = base + 12.0f * faceClampf(tilt, 0.0f, 1.0f);
  float y0 = base, y1 = base;
  if (side < 0) y1 = inner;
  else          y0 = inner;
  c.g->drawWideLine((int)lroundf(ex - halfW), (int)lroundf(y0),
                    (int)lroundf(ex + halfW), (int)lroundf(y1), 2.6f, color);
}

class RoundedFace : public Face {
 public:
  const char* name() const override { return "rounded"; }
  const char* description() const override {
    return "soft rectangles with a wet highlight; the original";
  }

  void draw(FaceCanvas& c, const FaceFrame& f) override {
    // Everything this face does is one of five channels: how open the lids
    // are, how wide the eye is, where the pair is looking, whether the whole
    // head has hopped, and what colour it is. `open`, the gaze and the colour
    // arrive from outside; the other two are the expression, decided here.
    float lid      = f.open;
    float widthK   = 1.0f;
    float gazeX    = f.gazeX;
    float gazeY    = f.gazeY;
    float bounceY  = 0.0f;
    bool  happy    = false;
    bool  brows    = false;
    float browTilt = 0.0f;

    if (f.ambient) {
      // A 5% height swell on the same 6.5s period as the ambient brightness
      // breathe. Two eyes holding exactly one shape between blinks look
      // painted on; this is invisible as motion and is the whole difference
      // between resting and inert. It also keeps smearing the eyes' top and
      // bottom edges, which is the half of the burn-in defence the drift does
      // not cover for a big solid shape.
      lid *= 0.97f + 0.05f * f.pulse;
    } else {
      switch (f.state) {
        case ST_SLEEP:
          // Shut, and breathing. The lid line rides 3px up and down on the
          // same 4s sine as the ring and the eye narrows slightly at the
          // bottom of it. That is the whole animation, and it is meant to be:
          // a sleeping thing should be almost still, but a genuinely frozen
          // panel is indistinguishable from a crashed one.
          widthK = 0.90f + 0.06f * f.pulse;
          gazeY += -1.6f + 3.2f * f.pulse;
          break;

        case ST_IDLE:
          break;                                  // open, level, resting

        case ST_BUSY:
          // Concentrating: the gaze tracks back and forth across something
          // only it can see. The two scan periods do not divide into each
          // other, so the eyes never retrace one fixed path and the motion
          // does not read as a mechanism. The squint itself is in `open`.
          gazeX += 6.5f * sinf((float)(f.nowMs % 1700u) / 1700.0f * 2.0f *
                               (float)M_PI);
          gazeY += 2.0f * sinf((float)(f.nowMs % 2600u) / 2600.0f * 2.0f *
                               (float)M_PI);
          break;

        case ST_WAITING:
          if (f.acked) break;   // wide, but level-browed and steady
          // The hero state, aimed at one job: reading as "hey, you" from
          // across a room. The head hops on every pulse peak, which is the
          // part that catches peripheral vision, since motion does and a
          // brightness change on a 40px object mostly does not. The brow drop
          // is the escalation gesture and the cheapest emotion on a face.
          widthK   = f.escalated ? 1.06f : 1.00f;
          bounceY  = -(f.escalated ? 5.0f : 3.0f) * f.pulse;
          brows    = true;
          browTilt = f.escalated ? 1.0f : 0.0f;
          break;

        case ST_DONE:
          // A happy squint held well past the 600ms ring flash, because a
          // smile that lasts exactly as long as a flash does not register as
          // a smile. It hops once on arrival.
          if (f.stateAgeMs < DONE_HAPPY_MS) {
            happy = true;
            if (f.stateAgeMs < 300) {
              bounceY = -4.0f * (1.0f - (float)f.stateAgeMs / 300.0f);
            }
          }
          break;
      }
    }

    const float w  = (float)f.geom.eyeW * widthK;
    const float h  = (float)f.geom.eyeH * lid;
    // Half the distance between the two eye centres. Deliberately off the
    // NOMINAL width, so a widthK stretch thickens each eye without also
    // pushing the pair apart.
    const float dx = (float)(f.geom.eyeW + f.geom.gap) * 0.5f;

    const float headX = (float)f.geom.cx;
    const float headY = (float)f.geom.cy + bounceY;
    const float eyeX  = headX + gazeX;
    const float eyeY  = headY + gazeY;

    uint16_t col = c.tint(f.color);

    for (int i = 0; i < 2; ++i) {
      float side = (i == 0) ? -1.0f : 1.0f;
      if (happy) drawHappyEye(c, eyeX + side * dx, eyeY, w, col);
      else       drawOneEye(c, eyeX + side * dx, eyeY, w, h, col, lid > 0.55f);
      if (brows) {
        drawBrow(c, headX + side * dx, headY, w, (float)f.geom.eyeH, (int)side,
                 browTilt, col);
      }
    }
  }
};

RoundedFace gRounded;

}  // namespace

Face* faceRounded() { return &gRounded; }
