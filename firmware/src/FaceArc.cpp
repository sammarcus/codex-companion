// Face: "arc" -- Nimbus, the single best design in scratchpad/eyes-arc.js.
//
// Two curved strokes that swell at the belly and taper into round caps, with a
// slow bob, a drifting highlight and one sparkle orbiting off the right eye.
// Serene, unhurried, quietly awake: a thing that has never been startled by
// anything, which is exactly what design law 4 asks of something sitting on an
// open-plan desk all day.
//
// The reference draws each stroke as one filled polygon: a cubic bezier
// centreline with a half width that varies along a bell curve, plus a little
// deterministic grain so the edge is not machine-smooth. That polygon has no
// primitive here, so the ribbon is walked as a short polyline and each segment
// is drawn with drawWideLine, LovyanGFX's anti-aliased wedge. Round caps and
// overlapping joins come free, the taper quantises to the segment count rather
// than being lost, and it costs the same primitive the eyebrows already use.
//
// This is the third implementation of the interface and the one that proves it
// is not shaped around either of the other two: it has no rectangles, no lids,
// no fixed eye box, and it reads `open` as curvature rather than as height.

#include <math.h>

#include "Face.hpp"

namespace {

constexpr uint16_t kWhite = 0xFFFF;

// The reference geometry, for scale. Eye centres 106px apart, each stroke 92px
// wide at rest, so the resting pair spans 198px. Everything below is that
// number scaled into whatever pair width the composition asked for.
constexpr float REF_SPAN = 198.0f;
constexpr float REF_SEP  = 106.0f;
constexpr float REF_HW   = 46.0f;

float sinPeriod(uint32_t nowMs, uint32_t periodMs) {
  if (periodMs == 0) return 0.0f;
  return sinf((float)(nowMs % periodMs) / (float)periodMs * 2.0f *
              (float)M_PI);
}

// Seconds since boot, wrapped every 1024s so the float stays small enough for
// sinf to keep its precision after a week on a desk.
float secs(uint32_t nowMs) { return (float)(nowMs % 1024000u) * 0.001f; }

float lerpf(float a, float b, float u) { return a + (b - a) * u; }

struct Pt { float x, y; };

Pt bez(const Pt& p0, const Pt& p1, const Pt& p2, const Pt& p3, float u) {
  float m = 1.0f - u;
  float a = m * m * m, b = 3.0f * m * m * u, c = 3.0f * m * u * u,
        d = u * u * u;
  Pt r = { a * p0.x + b * p1.x + c * p2.x + d * p3.x,
           a * p0.y + b * p1.y + c * p2.y + d * p3.y };
  return r;
}

Pt bezD(const Pt& p0, const Pt& p1, const Pt& p2, const Pt& p3, float u) {
  float m = 1.0f - u;
  float a = 3.0f * m * m, b = 6.0f * m * u, c = 3.0f * u * u;
  Pt r = { a * (p1.x - p0.x) + b * (p2.x - p1.x) + c * (p3.x - p2.x),
           a * (p1.y - p0.y) + b * (p2.y - p1.y) + c * (p3.y - p2.y) };
  return r;
}

// One stroke.
struct Eye {
  float cx, cy;       // where the chord's midpoint sits on the panel
  float hw;           // half the chord: the stroke is 2*hw wide
  float rise;         // how far the belly bulges above the chord
  float wMid, wTip;   // half width at the belly and at the caps
  float pw;           // bell exponent: higher is a sharper peak
  float tilt;         // radians
  float skew;         // pushes the belly toward one end
  float seed;         // grain phase, so the two eyes are not identical
  float glossAt;      // 0..1 along the stroke
  float gloss;        // 0 for none
};

class ArcFace : public Face {
 public:
  const char* name() const override { return "arc"; }
  const char* description() const override {
    return "tapering hand-drawn strokes; calm and unhurried";
  }

  void draw(FaceCanvas& c, const FaceFrame& f) override {
    const StateId state = f.ambient ? ST_IDLE : f.state;
    const float   T     = secs(f.nowMs);

    // Per-state parameters, straight out of the reference's table. `acked`
    // borrows waiting's shape but idle's pacing: still wide open, no longer
    // hurrying.
    float hw, rise, w, bobA, rate, sp, swayA;
    switch (state) {
      case ST_BUSY:
        hw = 40; rise = 10; w = 12; bobA = 0.9f; rate = 1.55f; sp = 0.20f;
        swayA = 0.3f; break;
      case ST_WAITING:
        hw = 53; rise = 25; w = 13;
        if (f.acked) { bobA = 1.2f; rate = 0.60f; sp = 0.45f; swayA = 0.8f; }
        else         { bobA = 4.4f; rate = 2.55f; sp = 1.00f; swayA = 3.4f; }
        break;
      case ST_DONE:
        hw = 49; rise = 23; w = 12; bobA = 1.3f; rate = 0.72f; sp = 0.80f;
        swayA = 0.6f; break;
      case ST_SLEEP:
        hw = 43; rise = 4;  w = 13; bobA = 0.8f; rate = 0.26f; sp = 0.06f;
        swayA = 0.2f; break;
      default:
        hw = 46; rise = 16; w = 11; bobA = 1.7f; rate = 0.55f; sp = 0.35f;
        swayA = 0.8f; break;
    }

    // Fit the reference pair into the pair width this composition wants, so
    // the strokes occupy exactly the footprint the default face would.
    const float span = (float)(2 * f.geom.eyeW + f.geom.gap);
    const float s    = span / REF_SPAN;
    const float sep  = REF_SEP * s;

    hw   *= s;
    rise *= s;
    w    *= s;

    const float bob  = sinf(T * rate) * bobA * s;
    const float sway = sinf(T * rate * 0.61f + 0.7f) * swayA * s;

    // `open` is curvature here, not height: a shut eye is a flat stroke with a
    // slight downward bow, and a wide one arches further. It also fattens as
    // it flattens, which is what keeps a blink from thinning into a hairline.
    const float openR = lerpf(-rise * 0.20f, rise, f.open);
    const float wOpen = w * (1.0f + 0.36f * (1.0f - f.open));

    // Sit the chord low enough that the bow straddles the same centre line the
    // default face's eyes use, so swapping faces does not move the face.
    const float baseY = (float)f.geom.cy + openR * 0.45f + bob +
                        (1.0f - f.open) * 3.0f * s;

    // The highlight wanders along the stroke rather than sitting at one spot,
    // which is most of what sells the hand-drawn read.
    const float drift = 0.30f + 0.16f * sinf(T * 0.43f);

    const uint16_t col   = c.tint(f.color);
    const uint16_t glowC = c.tint(scaleColor(f.color, 0.22f));
    const uint16_t glossC = c.tint(mixColor(f.color, kWhite, 0.55f));

    Eye left = { (float)f.geom.cx - sep * 0.5f + sway * 0.6f, baseY,
                 hw, openR, wOpen, wOpen * 0.16f, 1.50f, -0.022f, 0.06f, 1.2f,
                 drift, 1.0f };
    Eye right = { (float)f.geom.cx + sep * 0.5f + sway, baseY - 1.6f * s,
                  hw * 1.05f, openR * 0.93f, wOpen * 0.97f, wOpen * 0.15f,
                  1.68f, 0.03f, -0.05f, 4.7f, drift + 0.10f, 1.0f };

    drawEye(c, left,  col, glowC, glossC, s);
    drawEye(c, right, col, glowC, glossC, s);

    // One sparkle, orbiting off the outer edge of the right eye. It speeds up
    // and brightens for `waiting` and all but disappears for `sleep`.
    const float ang   = T * (state == ST_WAITING && !f.acked ? 1.9f : 0.42f);
    const float pulse = 0.65f + 0.35f * sinf(
        T * (state == ST_WAITING && !f.acked ? 7.0f : 1.3f));
    const float sr = (3.4f + 1.9f * sp * pulse) * s * (0.35f + 0.65f * sp);
    if (sr > 1.2f) {
      float sx = right.cx + 32.0f * s + cosf(ang) * 9.0f * s;
      float sy = baseY - 46.0f * s + sinf(ang) * 6.0f * s;
      uint16_t sc = c.tint(scaleColor(f.color, 0.45f + 0.55f * sp * pulse));
      c.g->fillEllipse((int)lroundf(sx), (int)lroundf(sy),
                       (int)lroundf(sr * 0.34f), (int)lroundf(sr), sc);
      c.g->fillEllipse((int)lroundf(sx), (int)lroundf(sy),
                       (int)lroundf(sr), (int)lroundf(sr * 0.34f), sc);
    }
  }

 private:
  // Half width along the stroke: a bell curve between the tip and belly
  // widths, skewed toward one end, times a fixed two-tone grain so the edge
  // wobbles the way a drawn line does instead of being machine-smooth.
  static float widthAt(const Eye& e, float u) {
    float sk = faceClampf(u + e.skew * sinf((float)M_PI * u), 0.0f, 1.0f);
    // sinf(M_PI) is -8.7e-8, not 0, and powf() of a negative base with a
    // fractional exponent is NaN. The ribbon samples u = 1 exactly, so without
    // this floor the last disc of every stroke gets a NaN half width, which
    // survives every "< min" comparison, reaches lroundf as garbage and hands
    // fillEllipse a radius that hangs the render loop. It cost an hour: the
    // board greeted and then went permanently silent.
    float base = sinf((float)M_PI * sk);
    if (base < 0.0f) base = 0.0f;
    float bell = powf(base, e.pw);
    float grain = 1.0f + 0.055f * sinf(u * 7.3f + e.seed) +
                  0.028f * sinf(u * 13.1f - e.seed);
    float v = (e.wTip + (e.wMid - e.wTip) * bell) * grain;
    if (!(v == v)) return 0.5f;                       // belt and braces
    return v < 0.5f ? 0.5f : v;
  }

  // Walk the centreline and stamp a filled disc of the local half width at
  // each sample. The union of the discs IS the variable width stroke, with
  // round caps and round joins for free and no seam anywhere, and the samples
  // are close enough together that the scalloping between them is well under a
  // pixel.
  //
  // This started as one drawWideLine per segment, which is the obvious
  // primitive and looks slightly better because it is anti-aliased. It cost
  // 24ms a frame: 88 wedges, each of them a per-pixel float job over its own
  // bounding box, against a 33ms budget that already spends 14ms on
  // fillScreen and pushSprite. Discs are a scanline fill and land in 3.
  //
  // `fade` tapers the width to nothing at both ends of the [u0, u1] window,
  // which is how the highlight melts into the stroke instead of stopping dead.
  static void ribbon(FaceCanvas& c, const Eye& e, uint16_t col, int n,
                     float u0, float u1, float widen, float normalOff,
                     float widthK, bool fade = false) {
    const Pt p0 = { -e.hw, 0.0f };
    const Pt p3 = {  e.hw, 0.0f };
    const Pt p1 = { -e.hw * 0.42f, -e.rise * 1.35f };
    const Pt p2 = {  e.hw * 0.42f, -e.rise * 1.35f };

    const float ct = cosf(e.tilt), st = sinf(e.tilt);

    for (int i = 0; i <= n; ++i) {
      float k = (float)i / (float)n;
      float u = u0 + (u1 - u0) * k;
      Pt p = bez(p0, p1, p2, p3, u);
      if (normalOff != 0.0f) {
        Pt d = bezD(p0, p1, p2, p3, u);
        float len = sqrtf(d.x * d.x + d.y * d.y);
        if (len < 0.0001f) len = 1.0f;
        float off = normalOff * widthAt(e, u);
        p.x += (-d.y / len) * off;
        p.y += ( d.x / len) * off;
      }
      float hwid = widthAt(e, u) * widthK;
      if (fade) hwid *= sinf((float)M_PI * k);
      hwid += widen;
      // The floor is what keeps the taper continuous rather than dotted: the
      // reference's own 0.5 clamp would leave the last few discs too small to
      // touch each other at this sample spacing.
      // A hard ceiling as well as a floor. Nothing here should ever produce a
      // radius near it, and if a future edit does, the frame gets ugly rather
      // than never returning.
      hwid = faceClampf(hwid, 1.4f, 40.0f);

      // Rotate into place, then translate.
      int px = (int)lroundf(p.x * ct - p.y * st + e.cx);
      int py = (int)lroundf(p.x * st + p.y * ct + e.cy);
      int r  = (int)lroundf(hwid);
      if (r < 1) r = 1;
      c.g->fillEllipse(px, py, r, r, col);
    }
  }

  static void drawEye(FaceCanvas& c, const Eye& e, uint16_t col,
                      uint16_t glowC, uint16_t glossC, float s) {
    // A dim halo first. On a black panel this is what the reference's 13%
    // alpha pass buys, and it is what keeps a thin stroke from looking
    // scratched onto the glass. It is deliberately undersampled: a halo can
    // scallop and nobody sees it, and this pass draws the widest discs.
    ribbon(c, e, glowC, 16, 0.0f, 1.0f, 2.6f * s, 0.0f, 1.0f);
    // The stroke itself. 34 discs over an arc of roughly 80px is a 2.4px
    // sample spacing against a 5px minimum radius, so the scallop between
    // consecutive discs is a third of a pixel at the thinnest point.
    ribbon(c, e, col,   34, 0.0f, 1.0f, 0.0f,     0.0f, 1.0f);

    // The wandering highlight: a short thin ribbon riding the upper edge,
    // fading in and out across its own span.
    if (e.gloss > 0.0f) {
      float span = 0.24f;
      float a = e.glossAt - span, b = e.glossAt + span;
      if (a < 0.02f) a = 0.02f;
      if (b > 0.98f) b = 0.98f;
      if (b - a > 0.06f) {
        ribbon(c, e, glossC, 10, a, b, 0.0f, -0.44f, 0.26f, true);
      }
    }
  }
};

ArcFace gArc;

}  // namespace

Face* faceArc() { return &gArc; }
