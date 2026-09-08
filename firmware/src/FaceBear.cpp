// Face: "bear" -- Milkbun, the single best design in scratchpad/eyes-bear.js.
//
// A warm rounded creature rather than a pair of eyes. The whole idiom, taken
// from the JavaScript unchanged: the creature IS the frame, one big rounded
// body wider than tall with no neck, eyes are tiny solid dots set far apart,
// the mouth is smaller than one eye, blush is always on, limbs are stubby
// nubs, no outlines anywhere. Expression lives in the MOUTH and in whole-body
// squash. The eyes barely move.
//
// Which is why this one earns its place next to "rounded": it uses almost none
// of the same channels. `open` collapses a dot into a bar rather than closing
// a rectangle, the accent colour never touches the body and only paints the
// blush and the little state sparks, and the wide-eyed `waiting` read is
// carried by a hop with a landing squash instead of by the lids. If the
// interface can serve both of these it can serve the next one.
//
// Ported from the canvas reference by scale rather than by copy. Every length
// below is the JavaScript's own number multiplied by `s`, where `s` is derived
// from the box the composition handed over and from which mode it is, so the
// creature fits the wide ambient box and the narrower live box from one set of
// constants and is genuinely larger on the first of them.

#include <math.h>

#include "Face.hpp"

namespace {

// Warm flat palette, RGB565 conversions of the reference's hex. The body
// colour is never the state colour: that is the point of the design.
constexpr uint16_t CREAM   = 0xF77B;   // #f6ecdc
constexpr uint16_t CREAM_D = 0xE697;   // #e2d3bd
constexpr uint16_t INK     = 0x2903;   // #2c211a

// The reference creature, for scale. Everything is expressed against these.
constexpr float REF_W = 146.0f;
constexpr float REF_H = 120.0f;

// Fraction of the owned box the body occupies at rest. The rest is headroom
// for the ears and the waiting hop.
//
// Two of them, because one was a bug the arithmetic hid. FACE_BOX_H is 116 in
// both modes, so a single fraction made the height term identical in both and
// the width term (320 * 0.95 / 146 = 2.08 ambient, 192 * 0.95 / 146 = 1.25
// live) is never the smaller one, which meant this face rendered at exactly
// the same size on both screens while `rounded` grew 40x48 to 56x64 and `arc`
// grew a 106px eye span to 148px. Choosing the bear silently gave up ambient's
// larger presence.
//
// It cannot take the 1.33x the other two take: they grow their eyes inside the
// box and this creature IS the box, with a label line at y=122 and the hold
// hint's band along the very top putting a hard ceiling of 112px on it. 0.88
// is what fits. At s = 112 * 0.88 / 120 = 0.821 the body is 98.5px, the
// resting swell (2.8%) takes it to 101, and the ears reach 9 * s = 7.4px above
// that, landing at y=9.3 inside a box that starts at y=6. That is 13% more
// creature in ambient with nothing clipped.
//
// The box moved down 4px (FACE_BOX_Y 2 -> 6, FACE_BOX_H 116 -> 112) after this
// arithmetic was found to be one term short: it is right at rest, and the
// ambient drift then rides the whole composition 5px up (AMB_DRIFT_AY), which
// put the ear tops on row 0 under the hold hint. The 4px is that amplitude
// less the 1px the old numbers had spare.
constexpr float BODY_FRAC     = 0.78f;
constexpr float BODY_FRAC_AMB = 0.88f;

// sin() over an exact integer period, so the phase wraps cleanly however long
// the board has been up. sinf((float)millis()/k) loses a whole radian of
// precision after a few hours; this never does.
float sinPeriod(uint32_t nowMs, uint32_t periodMs) {
  if (periodMs == 0) return 0.0f;
  return sinf((float)(nowMs % periodMs) / (float)periodMs * 2.0f *
              (float)M_PI);
}

void ell(FaceCanvas& c, float cx, float cy, float rx, float ry,
         uint16_t col) {
  int irx = (int)lroundf(rx), iry = (int)lroundf(ry);
  if (irx < 1) irx = 1;
  if (iry < 1) iry = 1;
  c.g->fillEllipse((int)lroundf(cx), (int)lroundf(cy), irx, iry, col);
}

// The reference's stroked arc. Canvas radians with 0 at 3 o'clock increasing
// clockwise are LovyanGFX degrees with the same convention, so the only
// conversion is the scale factor and turning a line width into two radii.
void arcStroke(FaceCanvas& c, float cx, float cy, float r, float a0Rad,
               float a1Rad, uint16_t col, float lw) {
  float half = lw * 0.5f;
  int r0 = (int)lroundf(r - half);
  int r1 = (int)lroundf(r + half);
  if (r0 < 0) r0 = 0;
  if (r1 <= r0) r1 = r0 + 1;
  c.g->fillArc((int)lroundf(cx), (int)lroundf(cy), r0, r1,
               a0Rad * 180.0f / (float)M_PI, a1Rad * 180.0f / (float)M_PI,
               col);
}

// A four point star, standing in for the reference's eight point polygon. At
// the 5-7px radius it is drawn at, two crossed spindles read as the same
// sparkle for a fraction of the fill cost.
void spark(FaceCanvas& c, float cx, float cy, float r, uint16_t col) {
  if (r < 1.5f) return;
  ell(c, cx, cy, r * 0.30f, r, col);
  ell(c, cx, cy, r, r * 0.30f, col);
}

// Per-state body motion: breathe, sway, hop, and the squash that lands it.
struct Motion {
  float sx, sy, hop, sway;
};

Motion motionFor(const FaceFrame& f) {
  Motion m = { 1.0f, 1.0f, 0.0f, 0.0f };
  const uint32_t t = f.nowMs;

  if (!f.ambient && f.state == ST_WAITING && !f.acked) {
    // The state the whole object exists for: a real hop with a landing squash.
    // The reference free-ran this on its own clock; here it rides the ring's
    // pulse instead, so the creature leaves the ground on the same beat the
    // ring brightens. `up` is the top half of the pulse and `down` the bottom.
    float up   = f.pulse * 2.0f - 1.0f;
    float down = 1.0f - f.pulse * 2.0f;
    if (up   < 0.0f) up   = 0.0f;
    if (down < 0.0f) down = 0.0f;
    m.sx   = 1.0f - up * 0.05f + down * 0.13f;
    m.sy   = 1.0f + up * 0.06f - down * 0.13f;
    m.hop  = up * 13.0f * (f.escalated ? 1.35f : 1.0f);
    m.sway = sinPeriod(t, 3770) * 5.0f;
    return m;
  }

  uint32_t perMs, swayMs;
  float amp, swayA, hopA, base, wide = 1.0f;
  if (!f.ambient && f.state == ST_BUSY) {
    perMs = 3896;  amp = 0.022f; swayMs = 2073;  swayA = 3.5f; hopA = 0.0f;
    base = 0.98f;
  } else if (!f.ambient && f.state == ST_DONE) {
    perMs = 7226;  amp = 0.038f; swayMs = 5529;  swayA = 4.5f; hopA = 3.0f;
    base = 1.00f;
  } else if (!f.ambient && f.state == ST_SLEEP) {
    perMs = 21363; amp = 0.052f; swayMs = 21363; swayA = 2.0f; hopA = 0.0f;
    base = 0.94f; wide = 1.06f;
  } else {
    perMs = 16336; amp = 0.028f; swayMs = 11938; swayA = 2.2f; hopA = 0.0f;
    base = 1.00f;
  }

  float br = sinPeriod(t, perMs);
  m.sx   = (1.0f + amp * br) * wide;
  m.sy   = (1.0f - amp * br) * base;
  m.sway = sinPeriod(t, swayMs) * swayA;
  if (hopA > 0.0f) {
    float h = sinPeriod(t, swayMs);
    m.hop = (h > 0.0f ? h : 0.0f) * hopA;
  }
  return m;
}

class BearFace : public Face {
 public:
  const char* name() const override { return "bear"; }
  const char* description() const override {
    return "warm cream creature; the mouth carries the mood";
  }

  void draw(FaceCanvas& c, const FaceFrame& f) override {
    // Ambient has no state of its own, so it borrows the resting look. The
    // reference's own busy narrowing is dropped: `open` already carries the
    // squint, and applying both would shut the eyes.
    const StateId state = f.ambient ? ST_IDLE : f.state;
    const float   o     = faceClampf(f.open, 0.0f, 1.0f);

    const uint16_t fur  = c.tint(CREAM);
    const uint16_t fur2 = c.tint(CREAM_D);
    const uint16_t ink  = c.tint(INK);
    const uint16_t col  = c.tint(f.color);

    // Fit the reference creature to the box. Height is what binds on this
    // panel (170px tall with two text lines under the face), so the width
    // limit below only ever matters if the live box gets narrower. The two
    // fractions are what make ambient a bigger creature than live, since the
    // box itself is the same height in both: see BODY_FRAC_AMB.
    //
    // The toy hands `ambient = false` with a 46x40 box, so the runner keeps
    // the smaller fraction and its own geometry, unchanged.
    float s = (float)f.geom.boxH * (f.ambient ? BODY_FRAC_AMB : BODY_FRAC) /
              REF_H;
    float sw = (float)f.geom.boxW * 0.95f / REF_W;
    if (sw < s) s = sw;
    if (s < 0.2f) s = 0.2f;

    const Motion m = motionFor(f);

    const float w = REF_W * s * m.sx;
    const float h = REF_H * s * m.sy;
    const float cx = (float)f.geom.cx + m.sway * s;

    // The creature always sits on the bottom of its box. Clamp the hop so the
    // ears cannot leave the top of it: the box is the whole contract, and a
    // face that draws past it lands on the label line.
    const float floorY = (float)(f.geom.boxY + f.geom.boxH);
    float hop = m.hop * s;
    float headroom = (floorY - h) - (9.0f * s) - (float)f.geom.boxY - 2.0f;
    if (headroom < 0.0f) headroom = 0.0f;
    if (hop > headroom) hop = headroom;

    const float bottom = floorY - hop;
    const float top    = bottom - h;

    arms(c, cx, top, bottom, w, h, s, state, f.nowMs, fur);

    // Ears, behind the body so they read as one silhouette with it.
    ell(c, cx - w * 0.30f, top + 7.0f * s, 17.0f * s, 16.0f * s, fur);
    ell(c, cx + w * 0.30f, top + 7.0f * s, 17.0f * s, 16.0f * s, fur);

    // The body. One rounded rect, wider than tall, no neck.
    {
      int bw = (int)lroundf(w), bh = (int)lroundf(h);
      if (bw < 4) bw = 4;
      if (bh < 4) bh = 4;
      int r = (int)(((bw < bh) ? bw : bh) * 0.46f);
      if (r < 1) r = 1;
      c.g->fillSmoothRoundRect((int)lroundf(cx) - bw / 2, (int)lroundf(top),
                               bw, bh, r, fur);
    }

    // Inner ears, in front.
    ell(c, cx - w * 0.30f, top + 3.0f * s, 8.5f * s, 7.5f * s, fur2);
    ell(c, cx + w * 0.30f, top + 3.0f * s, 8.5f * s, 7.5f * s, fur2);

    // Feet.
    ell(c, cx - w * 0.23f, bottom - 8.0f * s, w * 0.145f, 8.0f * s, fur2);
    ell(c, cx + w * 0.23f, bottom - 8.0f * s, w * 0.145f, 8.0f * s, fur2);

    const float drop = (state == ST_BUSY) ? 3.0f * s : 0.0f;
    const float ey   = top + h * 0.40f + drop;

    blush(c, cx, top + h * 0.53f + drop, w * 0.305f, w * 0.075f, h * 0.055f,
          state, f, col);
    eyes(c, cx, ey, w * 0.20f, 5.0f * s, o, state, f.nowMs, ink);
    mouth(c, cx, top + h * 0.555f + drop, s, state, f, ink);
    accent(c, cx, top, w, s, state, f, col);
  }

 private:
  // Tiny dot eyes, far apart. A blink flattens the dot into a short bar, which
  // is exactly how the reference blinks: it widens as it shortens.
  static void eyes(FaceCanvas& c, float cx, float ey, float gap, float r,
                   float o, StateId state, uint32_t nowMs, uint16_t ink) {
    for (int i = -1; i <= 1; i += 2) {
      float x = cx + (float)i * gap;
      if (state == ST_DONE) {
        // The one state where the eye shape changes: a happy upward arc,
        // flattening toward a bar as `open` drops.
        float amp = 0.16f + 0.20f * o;
        arcStroke(c, x, ey + r * 1.5f, r * 1.55f,
                  (float)M_PI * (1.5f - amp), (float)M_PI * (1.5f + amp),
                  ink, 3.2f * (r / 5.0f));
      } else if (state == ST_SLEEP) {
        float rad = r * 3.0f + sinPeriod(nowMs, 15080) * r * 0.8f;
        arcStroke(c, x, ey + rad * 0.92f, rad, (float)M_PI * 1.36f,
                  (float)M_PI * 1.64f, ink, 3.2f * (r / 5.0f));
      } else {
        float ry = r * o;
        if (ry < 1.5f) ry = 1.5f;
        ell(c, x, ey, r * (1.0f + (1.0f - o) * 0.28f), ry, ink);
      }
    }
  }

  // Always smaller than one eye, except the waiting "o" at full pulse. This is
  // where the whole expression lives.
  static void mouth(FaceCanvas& c, float cx, float my, float s, StateId state,
                    const FaceFrame& f, uint16_t ink) {
    if (state == ST_WAITING && !f.acked) {
      float r = (4.4f + (f.pulse * 2.0f - 1.0f) * 1.7f) * s;
      ell(c, cx, my, r * 0.88f, r, ink);
    } else if (state == ST_WAITING) {
      ell(c, cx, my, 3.9f * s, 4.4f * s, ink);       // acked: a steady o
    } else if (state == ST_DONE) {
      arcStroke(c, cx, my - 3.0f * s, 8.0f * s, (float)M_PI * 0.16f,
                (float)M_PI * 0.84f, ink, 3.0f * s);
    } else if (state == ST_BUSY) {
      c.g->drawWideLine((int)lroundf(cx - 5.0f * s), (int)lroundf(my),
                        (int)lroundf(cx + 5.0f * s), (int)lroundf(my),
                        1.5f * s, ink);
    } else if (state == ST_SLEEP) {
      ell(c, cx, my, 3.1f * s,
          (2.5f + sinPeriod(f.nowMs, 15080) * 1.1f) * s, ink);
    } else {
      // The little w.
      arcStroke(c, cx - 2.7f * s, my - 1.0f * s, 2.9f * s,
                (float)M_PI * 0.06f, (float)M_PI * 0.94f, ink, 2.5f * s);
      arcStroke(c, cx + 2.7f * s, my - 1.0f * s, 2.9f * s,
                (float)M_PI * 0.06f, (float)M_PI * 0.94f, ink, 2.5f * s);
    }
  }

  static void blush(FaceCanvas& c, float cx, float by, float gap, float rx,
                    float ry, StateId state, const FaceFrame& f,
                    uint16_t col) {
    float k = 1.0f;
    if (state == ST_WAITING && !f.acked) k = 1.0f + fabsf(f.pulse) * 0.22f;
    else if (state == ST_DONE)           k = 1.12f;
    else if (state == ST_SLEEP)          k = 0.90f;
    ell(c, cx - gap, by, rx * k, ry * k, col);
    ell(c, cx + gap, by, rx * k, ry * k, col);
  }

  // Stubby nubs, barely articulated, tucked behind the body so they read as
  // one shape with it.
  static void arms(FaceCanvas& c, float cx, float top, float bottom, float w,
                   float h, float s, StateId state, uint32_t nowMs,
                   uint16_t fur) {
    (void)bottom;
    float lift  = (state == ST_WAITING) ? -32.0f
                : (state == ST_DONE)    ? -17.0f
                : (state == ST_SLEEP)   ?   9.0f
                : (state == ST_BUSY)    ?  -3.0f
                                        :   2.0f;
    float swing = (state == ST_WAITING) ? 8.0f
                : (state == ST_BUSY)    ? 5.0f
                : (state == ST_DONE)    ? 3.0f
                : (state == ST_SLEEP)   ? 0.8f
                                        : 1.6f;
    uint32_t per = (state == ST_WAITING) ? 1005u
                 : (state == ST_BUSY)    ? 1571u
                 : (state == ST_DONE)    ? 3267u
                 : (state == ST_SLEEP)   ? 18850u
                                         : 12566u;
    for (int i = -1; i <= 1; i += 2) {
      // Half a period out of phase between the two arms.
      uint32_t t = nowMs + ((i > 0) ? per / 2u : 0u);
      float ph = sinPeriod(t, per);
      ell(c, cx + (float)i * (w * 0.5f - 3.0f * s),
          top + h * 0.60f + lift * s + ph * swing * s,
          w * 0.105f, h * 0.105f, fur);
    }
  }

  // The only place the state colour goes besides the blush.
  static void accent(FaceCanvas& c, float cx, float top, float w, float s,
                     StateId state, const FaceFrame& f, uint16_t col) {
    const uint32_t t = f.nowMs;
    if (state == ST_WAITING) {
      if (f.acked) return;                     // told you know: stop shouting
      for (int i = 0; i < 3; ++i) {
        float p = fabsf(sinPeriod(t + (uint32_t)(i * 140), 1257));
        float y = top - (11.0f + p * 4.0f) * s;
        float lo = (float)f.geom.boxY + 7.0f * s;
        if (y < lo) y = lo;
        ell(c, cx + (float)(i - 1) * 17.0f * s, y,
            (2.2f + p * 2.8f) * s, (2.2f + p * 2.8f) * s, col);
      }
    } else if (state == ST_DONE) {
      spark(c, cx - w * 0.5f - 15.0f * s, top + 26.0f * s,
            (7.0f + sinPeriod(t, 4398) * 1.5f) * s, col);
      spark(c, cx + w * 0.5f + 15.0f * s, top + 46.0f * s,
            (5.0f + sinPeriod(t, 5655) * 1.2f) * s, col);
    } else if (state == ST_BUSY) {
      for (int k = 0; k < 3; ++k) {
        float q = (sinPeriod(t - (uint32_t)(k * 208), 1634) + 1.0f) * 0.5f;
        ell(c, cx + w * 0.5f + (10.0f + (float)k * 11.0f) * s,
            top + 22.0f * s, (1.8f + q * 2.4f) * s, (1.8f + q * 2.4f) * s,
            col);
      }
    } else if (state == ST_SLEEP) {
      for (int z = 0; z < 2; ++z) {
        float fr = (float)((t + (uint32_t)z * 1800u) % 3600u) / 3600.0f;
        float y = top + (30.0f - fr * 30.0f) * s;
        float lo = (float)f.geom.boxY + 8.0f * s;
        if (y < lo) y = lo;
        ell(c, cx + w * 0.5f + (8.0f + (float)z * 15.0f) * s, y,
            (2.4f + fr * 3.0f) * s, (2.4f + fr * 3.0f) * s, col);
      }
    }
  }
};

BearFace gBear;

}  // namespace

Face* faceBear() { return &gBear; }
