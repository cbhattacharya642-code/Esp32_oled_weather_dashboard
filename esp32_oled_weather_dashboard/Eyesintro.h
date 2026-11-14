#ifndef EYESINTRO_H
#define EYESINTRO_H

/*
 ============================================================
   EYES INTRO  -  boot screen for the ESP32 Game Console
   Animated OLED eyes (U8g2). GPIO 0 press = skip. When boot work is done the eyes
   give a happy squint + chirp, flatten into a glowing line,
   which opens to reveal "Gaming never stops" (letters glide
   in, a light sweeps across, a star twinkles), then the line
   closes and control returns to the caller (the main menu).

   Script (every 2.8 s, ZZZ holds for 2 ticks):
     blink -> look left -> look right -> look up ->
     HAPPY -> SURPRISED -> SQUARE -> DETERMINED -> TARGET ->
     SCAN -> GLITCH -> PIXEL -> LOADER -> HAPPY (done ping) ->
     LOVE -> WINK -> SPARKLE -> SHY -> ROLL -> CONFUSED ->
     DIZZY -> EQ -> SHADES -> SAD -> SLEEPY -> ZZZ -> reset
   plus random automatic blinking and a "wake up" at boot.

   Usage (in the console sketch):
     #include "EyesIntro.h"
     ...
     showEyesIntro(bootFinished);   // runs until bootFinished() is true,
                                    // or until GPIO 0 is pressed (skip)
 ============================================================
*/

#include <Arduino.h>
#include <math.h>
#include <string.h>

// Draws onto the dashboard's shared Adafruit_SSD1306 `display` through the
// U8g2-style shim (also declares btnPressed(), beep(), backgroundTick() ...).
#include "GfxCompat.h"

namespace EyesIntro {

enum Emotion : uint8_t {
  NORMAL = 0, HAPPY, SURPRISED, SLEEPY,
  SCAN, GLITCH, LOADER, ZZZ, DETERMINED,
  LOVE, DIZZY, SPARKLE, SAD, SHY, ROLL,
  SQUARE, CONFUSED, WINK,
  PIXEL, TARGET, SHADES, EQ,
  NUM_EMOTIONS
};

// ── Layout / tuning ───────────────────────────────────────
constexpr int EYE_W = 36;             // base eye size
constexpr int EYE_H = 38;
constexpr int EYE_R = 9;              // corner radius
constexpr int EYE_CY = 32;            // vertical centre
constexpr int EYE_LX = 34;            // left / right eye centres
constexpr int EYE_RX = 94;
constexpr float GAZE_X = 13.0f;       // max eye travel in pixels
constexpr float GAZE_Y = 8.0f;
constexpr uint32_t STEP_MS = 2200;    // time between scripted actions (faster)
constexpr uint32_t LEAVE_MS = 550;    // happy squint before leaving
constexpr uint8_t NUM_STEPS = 27;

// Emotion presets:
//   h, w    size multipliers
//   lid     fraction hidden from the top (sleepy)
//   arch    happy cheek cut-out
//   pupil   black pupil size (disc)
//   slant   brow cut: +1 = angry "V" (inner corner down),
//                     -1 = sad "/\" (outer corner down)
//   rad     corner radius multiplier (0 = hard square, 4 = circle)
//   dy      vertical offset in pixels
struct Look {
  float h, w, lid, arch, pupil, slant, rad, dy;
};
static const Look LOOKS[NUM_EMOTIONS] = {
    /* NORMAL     */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* HAPPY      */ {0.90f, 1.05f, 0.00f, 1.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* SURPRISED  */ {1.20f, 0.88f, 0.00f, 0.0f, 1.0f,  0.0f, 1.0f,  0.0f},
    /* SLEEPY     */ {0.85f, 1.00f, 0.42f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* SCAN       */ {0.40f, 1.05f, 0.00f, 0.0f, 0.0f,  0.0f, 0.6f,  0.0f},
    /* GLITCH     */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 0.4f,  0.0f},
    /* LOADER     */ {0.95f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 4.0f,  0.0f},
    /* ZZZ        */ {0.80f, 1.00f, 0.60f, 0.0f, 0.0f,  0.0f, 1.0f,  4.0f},
    /* DETERMINED */ {0.80f, 1.00f, 0.00f, 0.0f, 0.0f,  1.0f, 0.7f,  2.0f},
    /* LOVE       */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* DIZZY      */ {0.95f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 4.0f,  0.0f},
    /* SPARKLE    */ {1.10f, 1.00f, 0.00f, 0.0f, 3.2f,  0.0f, 2.0f,  0.0f},
    /* SAD        */ {0.80f, 1.00f, 0.00f, 0.0f, 0.0f, -1.0f, 1.0f, -3.0f},
    /* SHY        */ {0.70f, 0.95f, 0.20f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* ROLL       */ {0.90f, 1.00f, 0.35f, 0.0f, 0.0f,  0.0f, 0.8f,  0.0f},
    /* SQUARE     */ {0.85f, 0.90f, 0.00f, 0.0f, 1.3f,  0.0f, 0.0f,  0.0f},
    /* CONFUSED   */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 0.9f,  0.0f},
    /* WINK       */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* PIXEL      */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 0.6f,  0.0f},
    /* TARGET     */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* SHADES     */ {0.55f, 0.90f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
    /* EQ         */ {1.00f, 1.00f, 0.00f, 0.0f, 0.0f,  0.0f, 1.0f,  0.0f},
};

// Ticks (of STEP_MS) to hold after each scripted step
static const uint8_t STEP_HOLD[NUM_STEPS] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                             1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                             1, 1, 1, 1, 1, 2, 1};

// Bounding box of a drawn eye (used by the effect overlays)
struct Geo {
  int x, y, w, h;
};

static inline int ei_min(int a, int b) { return a < b ? a : b; }
static inline float ei_clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// ── Drawing helpers ───────────────────────────────────────
// U8g2 coordinates are unsigned: clip in int first.
static void rect(int x, int y, int w, int h, uint8_t col) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0) return;
  u8g2.setDrawColor(col);
  u8g2.drawBox(x, y, w, h);
}

static inline void plot(int x, int y) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  u8g2.drawPixel(x, y);
}

// 4-point sparkle star
static void drawStar(int cx, int cy, int s) {
  if (s < 1) { plot(cx, cy); return; }
  for (int i = -s; i <= s; i++) {
    plot(cx + i, cy);
    plot(cx, cy + i);
  }
}

// Line that is skipped if either end is off-screen (U8g2 coords are unsigned)
static void lineSafe(int x0, int y0, int x1, int y1) {
  if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0) return;
  if (x0 >= SCREEN_W || x1 >= SCREEN_W || y0 >= SCREEN_H || y1 >= SCREEN_H) return;
  u8g2.drawLine(x0, y0, x1, y1);
}

// Filled heart centred on (cx, cy), r = lobe radius (total width ~4r)
static void drawHeart(int cx, int cy, int r) {
  if (r < 1) return;
  if (cx - 2 * r < 0 || cy - 2 * r < 0) return;
  u8g2.setDrawColor(1);
  u8g2.drawDisc(cx - r, cy - r / 2, r);
  u8g2.drawDisc(cx + r, cy - r / 2, r);
  rect(cx - r, cy - r / 2, 2 * r, r / 2 + r / 8 + 1, 1);
  int tx = (r * 7) / 4;
  u8g2.setDrawColor(1);
  u8g2.drawTriangle(cx - tx, cy + r / 8, cx + tx, cy + r / 8, cx,
                    cy + 2 * r + r / 8);
}

static Geo drawEye(int cx, int cy, float w, float h, const Look &L, float arch,
                   bool rightEye, float gx, float gy) {
  int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
  if (iw < 4) iw = 4;
  if (ih < 3) ih = 3;
  int x = cx - iw / 2, y = cy - ih / 2;
  if (y < 0) y = 0;
  if (y + ih > SCREEN_H) y = SCREEN_H - ih;
  if (x < 0) x = 0;
  if (x + iw > SCREEN_W) x = SCREEN_W - iw;

  int r = (int)(EYE_R * L.rad + 0.5f);
  int m = (ih - 2) / 2;
  if (m < r) r = m;
  m = (iw - 2) / 2;
  if (m < r) r = m;

  u8g2.setDrawColor(1);
  if (r < 1) u8g2.drawBox(x, y, iw, ih);
  else u8g2.drawRBox(x, y, iw, ih, r);

  // Sleepy: upper lid
  int lh = (int)(ih * L.lid);
  if (lh > 0) rect(x - 1, y - 1, iw + 2, lh + 1, 0);

  // Happy: cut the lower part away -> "^" shaped eye
  if (arch > 0.03f) {
    int ry = (int)(ih * 0.68f * arch);
    int ex = x + iw / 2;
    int rx = iw / 2 + 3;
    if (rx > ex) rx = ex;
    if (ry > 0) {
      u8g2.setDrawColor(0);
      u8g2.drawFilledEllipse(ex, y + ih, rx, ry);
    }
  }

  // Brow cut with a black triangle
  int y0 = y > 0 ? y - 1 : 0;
  int xs = x > 0 ? x - 1 : 0;
  int xe = x + iw + 1;
  if (L.slant > 0.03f) {
    // Determined: upper INNER corner removed -> "V" brow
    int cut = (int)(ih * 0.55f * L.slant);
    if (cut > 0) {
      u8g2.setDrawColor(0);
      if (!rightEye) u8g2.drawTriangle(xs, y0, xe, y0, xe, y + cut);
      else u8g2.drawTriangle(xe, y0, xs, y0, xs, y + cut);
    }
  } else if (L.slant < -0.03f) {
    // Sad: upper OUTER corner removed -> droopy brow
    int cut = (int)(ih * 0.50f * (-L.slant));
    if (cut > 0) {
      u8g2.setDrawColor(0);
      if (!rightEye) u8g2.drawTriangle(xs, y0, xe, y0, xs, y + cut);
      else u8g2.drawTriangle(xe, y0, xs, y0, xe, y + cut);
    }
  }

  // Pupil: round dot (surprised / sparkle) or square with a shine (square)
  int pr = (int)(3.0f * L.pupil + 0.5f);
  if (pr >= 1 && ih > 8) {
    int px = x + iw / 2 + (int)(gx * 3);
    int py = y + ih / 2 + (int)(gy * 3);
    u8g2.setDrawColor(0);
    if (L.rad < 0.35f) {
      u8g2.drawBox(px - pr, py - pr, pr * 2, pr * 2);
      u8g2.setDrawColor(1);
      u8g2.drawBox(px - pr + 1, py - pr + 1, 2, 2);  // white shine
    } else {
      u8g2.drawDisc(px, py, pr);
    }
  }
  u8g2.setDrawColor(1);

  Geo g = {x, y, iw, ih};
  return g;
}

// Shift one 8-pixel tile row of the frame buffer sideways (glitch band)
static void shiftRow(uint8_t *row, int n, int dx) {
  if (dx > 0) {
    memmove(row + dx, row, n - dx);
    memset(row, 0, dx);
  } else if (dx < 0) {
    dx = -dx;
    memmove(row, row + dx, n - dx);
    memset(row + n - dx, 0, dx);
  }
}

// ── Eye animation ─────────────────────────────────────────
struct Eyes {
  float gx = 0, gy = 0, tgx = 0, tgy = 0;  // current / target gaze (-1..1)
  Look cur = LOOKS[NORMAL];
  Look tgt = LOOKS[NORMAL];
  Emotion emo = NORMAL;
  float wake = 0.04f;                      // eyelids opening at boot
  bool blinking = false;
  float blinkT = 0, blinkDur = 0.16f;
  uint32_t nextBlink = 0;

  // effect state
  float fxT = 0;                           // seconds since emotion started
  float nodT = 0;                          // ZZZ head-nod timer
  float wArch = 0, wArchT = 0;             // right-eye wink (glitch)
  int shake = 0;                           // x jitter
  int curOx = 0, curOy = 0;                // current eye offset (for overlays)
  uint32_t frame = 0;
  Geo gL = {0, 0, 0, 0}, gR = {0, 0, 0, 0};
  float eqH[10] = {0}, eqP[10] = {0};      // equalizer bar heights / peaks

  void begin(uint32_t now) { nextBlink = now + 1600; }

  void lookAt(float x, float y) { tgx = x; tgy = y; }

  void setEmotion(Emotion e) {
    emo = e;
    tgt = LOOKS[e];
    tgx = tgy = 0;
    fxT = 0;
    nodT = 0;
    wArch = wArchT = 0;
    if (e == SQUARE) {   // startled pop: hard corners, size overshoot, hop up
      cur.rad = 0.0f;
      cur.h = 1.30f;
      cur.w = 1.20f;
      cur.dy = -5.0f;
    }
  }

  void reset() { setEmotion(NORMAL); }

  bool noBlink() const {
    return emo == LOADER || emo == LOVE || emo == DIZZY || emo == SPARKLE ||
           emo == TARGET || emo == EQ || emo == SHADES;
  }

  void blink() {
    if (blinking || noBlink()) return;
    blinking = true;
    blinkT = 0;
    if (emo == SLEEPY || emo == ZZZ) blinkDur = 0.45f;
    else if (emo == DETERMINED || emo == ROLL) blinkDur = 0.30f;
    else blinkDur = 0.16f;
  }

  void update(float dt, uint32_t now) {
    float aG = 1.0f - expf(-13.0f * dt);
    float aE = 1.0f - expf(-14.0f * dt);
    float aW = 1.0f - expf(-7.0f * dt);
    gx += (tgx - gx) * aG;
    gy += (tgy - gy) * aG;
    cur.h += (tgt.h - cur.h) * aE;
    cur.w += (tgt.w - cur.w) * aE;
    cur.lid += (tgt.lid - cur.lid) * aE;
    cur.arch += (tgt.arch - cur.arch) * aE;
    cur.pupil += (tgt.pupil - cur.pupil) * aE;
    cur.slant += (tgt.slant - cur.slant) * aE;
    cur.rad += (tgt.rad - cur.rad) * aE;
    cur.dy += (tgt.dy - cur.dy) * aE;
    wake += (1.0f - wake) * aW;
    wArch += (wArchT - wArch) * aE;

    fxT += dt;
    shake = 0;
    switch (emo) {
    case SCAN:        // smooth left-right sweep
      tgx = sinf(fxT * 3.0f);
      tgy = 0;
      break;
    case GLITCH:      // glitch -> wink -> reopen
      wArchT = (fxT >= 0.25f && fxT < 0.70f) ? 1.0f : 0.0f;
      break;
    case ZZZ:         // slow drift down, then jerk back up
      nodT += dt;
      if (nodT >= 1.5f) {
        nodT = 0;
        cur.dy = 3.0f;
      }
      tgt.dy = 4.0f + 5.0f * (nodT / 1.5f);
      break;
    case DETERMINED:  // tiny shake when the brow lands
      if (fxT > 0.12f && fxT < 0.26f) shake = (((int)(fxT * 60.0f)) & 1) ? 1 : -1;
      break;
    case DIZZY:       // wobbling gaze circle
      tgx = 0.25f * cosf(fxT * 5.0f);
      tgy = 0.25f * sinf(fxT * 5.0f);
      break;
    case SPARKLE:     // eager gaze drifting side to side
      tgx = 0.5f * sinf(fxT * 2.2f);
      tgy = -0.1f;
      break;
    case SAD:         // look down, little tremble
      tgy = 0.3f;
      if (fxT > 0.4f && ((((int)(fxT * 12.0f)) & 3) == 0)) shake = 1;
      break;
    case SHY:         // glance away, down
      tgx = -0.8f;
      tgy = 0.6f;
      break;
    case ROLL: {      // full clockwise eye roll, then a "meh" look
      if (fxT < 0.3f) {
        tgx = tgy = 0;
      } else if (fxT < 1.7f) {
        float u = (fxT - 0.3f) / 1.4f;
        float a = PI + u * TWO_PI;
        tgx = cosf(a);
        tgy = sinf(a);
      } else {
        tgx = 0;
        tgy = 0.4f;
      }
    } break;
    case CONFUSED:    // slow suspicious gaze sweep
      tgx = 0.6f * sinf(fxT * 1.8f);
      tgy = -0.15f;
      break;
    case WINK:        // right eye winks after a beat, then reopens
      wArchT = (fxT >= 0.3f && fxT < 1.9f) ? 1.0f : 0.0f;
      break;
    case PIXEL: {     // retro sprite gaze: hops left / centre / right
      static const float hop[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
      tgx = hop[((int)(fxT * 2.0f)) & 3];
      tgy = 0;
    } break;
    case TARGET:      // reticles hunt around, then lock on centre
      if (fxT < 0.6f) {
        tgx = 0.8f * sinf(fxT * 9.0f);
        tgy = 0.4f * cosf(fxT * 7.0f);
      } else {
        tgx = tgy = 0;
      }
      break;
    case EQ: {        // bouncing equalizer bars
      float amp = ei_clamp01(fxT / 0.3f);
      for (int i = 0; i < 10; i++) {
        float hgt = 4.0f + 32.0f * amp *
                    fabsf(sinf(fxT * (5.0f + (i % 5) * 1.7f) + i * 1.3f)) *
                    (0.65f + 0.35f * sinf(fxT * 2.3f + i * 0.9f));
        eqH[i] = hgt;
        if (hgt * 0.5f > eqP[i]) eqP[i] = hgt * 0.5f;
        else eqP[i] -= dt * 14.0f;
        if (eqP[i] < 2.0f) eqP[i] = 2.0f;
      }
    } break;
    default: break;
    }

    if (blinking) {
      blinkT += dt / blinkDur;
      if (blinkT >= 1.0f) {
        blinking = false;
        blinkT = 0;
        bool slow = (emo == SLEEPY || emo == ZZZ);
        nextBlink = now + (slow ? random(1400, 2800) : random(2200, 5200));
      }
    } else if ((int32_t)(now - nextBlink) >= 0) {
      blink();
    }
  }

  // ── Effect overlays ────────────────────────────────────
  void fxScan() {
    // scanner bar travelling inside each slit
    const Geo *gs[2] = {&gL, &gR};
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int bx = g.x + (int)((0.5f + 0.5f * sinf(fxT * 6.0f)) * (g.w - 2));
      rect(bx, g.y, 2, g.h, 0);
    }
    // expanding arcs from the leading edge (a new ring every 300 ms)
    int dir = (cosf(fxT * 3.0f) >= 0) ? 1 : -1;
    u8g2.setDrawColor(1);
    for (int k = 0; k < 2; k++) {
      float age = fmodf(fxT, 0.3f) + k * 0.3f;  // 0..0.6 s
      float r = 3.0f + (age / 0.6f) * 11.0f;    // 3..14 px
      if (k == 1 && (frame & 1)) continue;      // older ring flickers out
      for (int i = 0; i < 2; i++) {
        const Geo &g = *gs[i];
        int cx = g.x + (dir > 0 ? g.w : 0);
        int cy = g.y + g.h / 2;
        for (int a = -50; a <= 50; a += 5) {
          float rad = a * 0.0174533f;
          plot(cx + (int)(dir * r * cosf(rad)), cy + (int)(r * sinf(rad)));
        }
      }
    }
  }

  void fxLoader() {
    float ramp = ei_clamp01((fxT - 0.35f) / 0.30f);  // circle -> ring
    const Geo *gs[2] = {&gL, &gR};
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int cx = g.x + g.w / 2, cy = g.y + g.h / 2;
      int base = ei_min(g.w, g.h) / 2;
      int ri = (int)((base - 3) * ramp);
      if (ri > 0) {
        u8g2.setDrawColor(0);
        u8g2.drawDisc(cx, cy, ri);
      }
      if (ramp > 0.5f) {
        float dirS = i ? -1.0f : 1.0f;  // rings spin in opposite directions
        float head = fxT * TWO_PI * 1.5f * dirS;
        int R = base + 5;
        u8g2.setDrawColor(1);
        for (int k = 0; k < 8; k++) {
          float a = head - dirS * k * 0.55f;
          int dr = 3 - k / 3;           // fading tail 3 -> 1
          int px = cx + (int)(R * cosf(a));
          int py = cy + (int)(R * sinf(a));
          if (px < dr || py < dr || px >= SCREEN_W - dr || py >= SCREEN_H - dr) continue;
          u8g2.drawDisc(px, py, dr);
        }
      }
    }
  }

  void fxZzz() {
    static const uint8_t *fonts[3] = {u8g2_font_4x6_tr, u8g2_font_5x7_tr,
                                      u8g2_font_6x10_tr};
    static const char *glyph[3] = {"z", "Z", "Z"};
    u8g2.setFontMode(1);
    u8g2.setDrawColor(1);
    for (int i = 0; i < 3; i++) {
      float t = fxT - i * 0.5f;
      if (t < 0) continue;
      float age = fmodf(t, 2.5f);
      float p = age / 2.0f;
      if (p > 1.0f) continue;
      int x = 100 + (int)(p * 14.0f + 3.0f * sinf(age * 6.0f));
      int y = 18 - (int)(p * 16.0f);
      u8g2.setFont(fonts[i]);
      u8g2.drawStr(x, y, glyph[i]);
    }
    u8g2.setFontMode(0);
  }

  // LOVE: pulsing heart eyes + floating mini hearts
  void fxLove() {
    float ramp = ei_clamp01(fxT / 0.25f);
    if (ramp >= 1.0f) {
      float pulse = 7.5f + 1.5f * sinf(fxT * 9.0f);
      int r = (int)(pulse + 0.5f);
      drawHeart(EYE_LX + curOx, EYE_CY + curOy + 2, r);
      drawHeart(EYE_RX + curOx, EYE_CY + curOy + 2, r);
    } else {
      int r = (int)(8.0f * ramp);
      drawHeart(EYE_LX + curOx, EYE_CY + curOy + 2, r);
      drawHeart(EYE_RX + curOx, EYE_CY + curOy + 2, r);
    }
    if (fxT > 0.4f) {
      for (int i = 0; i < 2; i++) {
        float age = fmodf(fxT + i * 0.8f, 1.6f);
        float p = age / 1.6f;
        int x = 64 + (int)(5.0f * sinf(age * 5.0f + i * 3.0f));
        int y = 40 - (int)(p * 34.0f);
        if (y > 6) drawHeart(x, y, p > 0.6f ? 1 : 2);
      }
    }
  }

  // DIZZY: black spirals spinning inside round eyes + orbiting stars
  void fxDizzy() {
    const Geo *gs[2] = {&gL, &gR};
    u8g2.setDrawColor(0);
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int cx = g.x + g.w / 2, cy = g.y + g.h / 2;
      int R = ei_min(g.w, g.h) / 2 - 3;
      if (R < 5) continue;
      float s = i ? -1.0f : 1.0f;       // spin in opposite directions
      float spin = fxT * 9.0f;
      for (float t = 0.3f; t < 15.0f; t += 0.13f) {
        float rr = R * t / 15.0f;
        float a = s * (t + spin);
        int px = cx + (int)(rr * cosf(a));
        int py = cy + (int)(rr * sinf(a));
        plot(px, py);
        plot(px + 1, py);
      }
    }
    // little stars circling above the head
    u8g2.setDrawColor(1);
    for (int i = 0; i < 3; i++) {
      float a = fxT * 4.0f + i * 2.094f;
      int x = 64 + (int)(30.0f * cosf(a));
      int y = 7 + (int)(3.0f * sinf(a));
      drawStar(x, y, 1);
    }
  }

  // SPARKLE: big shiny pupils + twinkling stars
  void fxSparkle() {
    const Geo *gs[2] = {&gL, &gR};
    u8g2.setDrawColor(1);
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int px = g.x + g.w / 2 + (int)(gx * 3);
      int py = g.y + g.h / 2 + (int)(gy * 3);
      u8g2.drawDisc(px - 4, py - 4, 3);   // big shine
      u8g2.drawDisc(px + 4, py + 3, 1);   // small shine
    }
    static const int8_t sx[5] = {64, 6, 122, 6, 122};
    static const int8_t sy[5] = {32, 8, 8, 57, 57};
    for (int i = 0; i < 5; i++) {
      float v = sinf(fxT * 5.0f + i * 1.7f);
      int s = v > 0 ? (int)(v * 5.0f) : 0;
      if (s > 0) drawStar(sx[i], sy[i], s);
    }
  }

  // SAD: tears falling from the outer bottom of each eye
  void fxSad() {
    if (fxT < 0.4f) return;
    const Geo *gs[2] = {&gL, &gR};
    const float per = 1.1f;
    u8g2.setDrawColor(1);
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int tx = (i == 0) ? g.x + 7 : g.x + g.w - 7;
      int top = g.y + g.h - 3;
      float age = fmodf(fxT + i * 0.5f, per);
      int ty = top + (int)((age / per) * (SCREEN_H - top));
      if (ty + 2 >= SCREEN_H) continue;
      u8g2.drawDisc(tx, ty, 2);
      u8g2.drawTriangle(tx, ty - 5, tx - 2, ty - 1, tx + 2, ty - 1);
    }
  }

  // SHY: blush marks "///" under each eye
  void fxShy() {
    if (fxT < 0.3f) return;
    const Geo *gs[2] = {&gL, &gR};
    u8g2.setDrawColor(1);
    for (int i = 0; i < 2; i++) {
      const Geo &g = *gs[i];
      int cx = g.x + g.w / 2;
      int yb = g.y + g.h + 4;
      if (yb > 58) yb = 58;
      for (int k = 0; k < 3; k++) {
        int x0 = cx - 9 + k * 7;
        u8g2.drawLine(x0, yb + 4, x0 + 3, yb);
      }
    }
  }

  // SQUARE: little "!" popping up between the eyes
  void fxSquare() {
    if (fxT < 0.05f || fxT > 1.6f) return;
    int hop = (fxT < 0.25f) ? (int)((0.25f - fxT) * 24.0f) : 0;  // pop-in bounce
    u8g2.setDrawColor(1);
    u8g2.drawBox(63, 24 - hop, 3, 8);
    u8g2.drawBox(63, 35 - hop, 3, 3);
  }

  // CONFUSED: a bobbing "?" between the eyes
  void fxConfused() {
    if (fxT < 0.3f) return;
    int bob = (int)(2.0f * sinf(fxT * 6.0f));
    u8g2.setFontMode(1);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_helvB10_tr);
    u8g2.drawStr(61, 37 + bob, "?");
    u8g2.setFontMode(0);
  }

  // WINK: beating heart in the gap + twinkling star by the winking eye
  void fxWink() {
    if (fxT < 0.5f) return;
    u8g2.setDrawColor(1);
    int r = (sinf(fxT * 9.0f) > 0.4f) ? 3 : 2;
    drawHeart(64, 34, r);
    float v = sinf(fxT * 7.0f);
    int s = v > 0 ? 1 + (int)(v * 4.0f) : 1;
    drawStar(120, 9, s);
  }

  // TARGET: crosshair reticles spin in, hunt, then lock on with a flash
  void fxTarget() {
    float k = ei_clamp01(fxT / 0.6f);
    float e = 1.0f - (1.0f - k) * (1.0f - k);          // ease-out
    int R = (int)(22.0f - 10.0f * e + 0.5f);           // 22 -> 12
    float spin = (1.0f - e) * TWO_PI;                  // one turn, then settles
    bool locked = fxT >= 0.6f;
    u8g2.setDrawColor(1);
    for (int i = 0; i < 2; i++) {
      int cx = (i ? EYE_RX : EYE_LX) + curOx;
      int cy = EYE_CY + curOy;
      u8g2.drawCircle(cx, cy, R);
      u8g2.drawCircle(cx, cy, R - 1);
      for (int q = 0; q < 4; q++) {
        float a = spin + q * (PI / 2.0f);
        float ca = cosf(a), sa = sinf(a);
        lineSafe(cx + (int)((R - 7) * ca), cy + (int)((R - 7) * sa),
                 cx + (int)((R + 4) * ca), cy + (int)((R + 4) * sa));
      }
      if (locked) {
        if (fxT < 0.72f) u8g2.drawDisc(cx, cy, R - 2);  // lock flash
        else u8g2.drawDisc(cx, cy, 4);
      } else {
        u8g2.drawDisc(cx, cy, 1);
      }
    }
  }

  // SHADES: "deal with it" sunglasses drop in, bounce, glint and twinkle
  void fxShades() {
    const int lw = 42, lh = 26, ly0 = EYE_CY - lh / 2 + 2;
    float p = ei_clamp01(fxT / 0.35f);
    int ly = ly0 - (int)(ly0 * (1.0f - p) * (1.0f - p));
    float w = fxT - 0.35f;
    if (w > 0) ly += (int)(4.0f * expf(-9.0f * w) * sinf(28.0f * w));
    if (ly < 0) ly = 0;
    int lx[2] = {EYE_LX - lw / 2, EYE_RX - lw / 2};

    u8g2.setDrawColor(1);
    rect(0, ly + 5, lx[0], 2, 1);                       // arms
    rect(lx[1] + lw, ly + 5, SCREEN_W - lx[1] - lw, 2, 1);
    rect(lx[0] + lw - 2, ly + 6, lx[1] - lx[0] - lw + 4, 3, 1);  // bridge
    for (int i = 0; i < 2; i++) {
      u8g2.setDrawColor(1);
      u8g2.drawRBox(lx[i], ly, lw, lh, 9);              // frame
      u8g2.setDrawColor(0);
      u8g2.drawRBox(lx[i] + 2, ly + 2, lw - 4, lh - 4, 7);  // dark lens
    }
    u8g2.setDrawColor(1);

    float gp = (fxT - 0.5f) / 0.5f;                     // glint sweep
    if (gp > 0 && gp < 1.0f) {
      int sx = (int)(easeGlint(gp) * (lw + 20));
      for (int i = 0; i < 2; i++) {
        u8g2.setClipWindow(lx[i] + 3, ly + 3, lx[i] + lw - 3, ly + lh - 3);
        int xs = lx[i] - 10 + sx;
        lineSafe(xs, ly + lh, xs + 10, ly);
        lineSafe(xs + 3, ly + lh, xs + 13, ly);
        u8g2.setMaxClipWindow();
      }
    } else if (gp >= 1.0f) {                            // twinkle
      float v = sinf(fxT * 8.0f);
      int s = v > 0 ? 1 + (int)(v * 3.0f) : 1;
      drawStar(lx[0] + 9, ly + 7, s);
      drawStar(lx[1] + 9, ly + 7, s > 1 ? s - 1 : 1);
    }
  }
  static float easeGlint(float t) { return t * t * (3.0f - 2.0f * t); }

  // EQ: eyes become mirrored equalizer bars with falling peak caps
  void fxEq() {
    const int bw = 5, gap = 2, total = 5 * bw + 4 * gap;   // 33 px wide
    int cy = EYE_CY + curOy;
    u8g2.setDrawColor(1);
    for (int e = 0; e < 2; e++) {
      int x0 = (e ? EYE_RX : EYE_LX) + curOx - total / 2;
      for (int i = 0; i < 5; i++) {
        int idx = e * 5 + i;
        int h = (int)eqH[idx];
        if (h < 3) h = 3;
        int x = x0 + i * (bw + gap);
        rect(x, cy - h / 2, bw, h, 1);
        int pk = (int)eqP[idx];
        rect(x, cy - pk - 3, bw, 1, 1);                    // peak caps
        rect(x, cy + pk + 2, bw, 1, 1);
      }
    }
  }

  // PIXEL: re-render the frame as a chunky 3x3-dot LED matrix (retro sprite)
  void pixelate() {
    uint8_t *buf = u8g2.getBufferPtr();
    int tw = u8g2.getBufferTileWidth() * 8;
    const int BW = SCREEN_W / 4, BH = SCREEN_H / 4;        // 32 x 16 blocks
    static uint8_t grid[16][32];
    for (int by = 0; by < BH; by++) {
      for (int bx = 0; bx < BW; bx++) {
        int n = 0;
        for (int sy = 1; sy <= 2; sy++) {
          for (int sx = 1; sx <= 2; sx++) {
            int px = bx * 4 + sx, py = by * 4 + sy;
            n += (buf[(py >> 3) * tw + px] >> (py & 7)) & 1;
          }
        }
        grid[by][bx] = (n >= 2);
      }
    }
    float dis = (fxT < 0.5f) ? 0.5f * (1.0f - fxT / 0.5f) : 0.0f;  // dissolve in
    int skip = (int)((dis + 0.02f) * 1000.0f);
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    for (int by = 0; by < BH; by++) {
      for (int bx = 0; bx < BW; bx++) {
        if (!grid[by][bx]) continue;
        if (random(1000) < skip) continue;
        u8g2.drawBox(bx * 4, by * 4, 3, 3);
      }
    }
  }

  // Glitch: shove random 8-px tile rows sideways and cut scanlines
  void glitchBuffer() {
    uint8_t *buf = u8g2.getBufferPtr();
    int tw = u8g2.getBufferTileWidth() * 8;
    int th = u8g2.getBufferTileHeight();
    for (int t = 0; t < th; t++) {
      if (random(100) < 55) {
        int dx = random(1, 5);
        if (random(2)) dx = -dx;
        shiftRow(buf + t * tw, tw, dx);
      }
    }
    u8g2.setDrawColor(0);
    for (int i = 0; i < 3; i++) u8g2.drawHLine(0, random(SCREEN_H), SCREEN_W);
    u8g2.setDrawColor(1);
  }

  void draw() {
    frame++;
    float open = wake;
    if (blinking) open *= 1.0f - 0.92f * sinf(PI * blinkT);
    float w = EYE_W * cur.w;
    float h = EYE_H * cur.h * open;
    int ox = (int)(gx * GAZE_X) + shake;
    int oy = (int)(gy * GAZE_Y) + (int)(cur.dy + (cur.dy >= 0 ? 0.5f : -0.5f));
    curOx = ox;
    curOy = oy;

    bool glitchOn = (emo == GLITCH) &&
                    (fxT < 0.25f || (fxT > 0.70f && fxT < 0.78f));
    float hl = h, hr = h;
    float archR = cur.arch;
    if (emo == GLITCH) {
      if (glitchOn && (frame & 1)) { hl *= 0.7f; hr *= 0.7f; }  // flicker
      if (fxT >= 0.25f && fxT < 0.70f) hl *= 0.95f;              // happy squash
      archR = fmaxf(cur.arch, wArch);                            // right wink "^"
    }

    if (emo == CONFUSED) { hl *= 0.70f; hr *= 1.15f; }   // uneven eyes
    if (emo == WINK) archR = fmaxf(cur.arch, wArch);     // right eye "^" wink

    // LOVE: eyes shrink away while the hearts grow in
    bool heartsOnly = false;
    if (emo == LOVE) {
      float ramp = ei_clamp01(fxT / 0.25f);
      hl *= (1.0f - ramp);
      hr *= (1.0f - ramp);
      heartsOnly = (ramp >= 1.0f);
    }

    u8g2.clearBuffer();
    if (!heartsOnly && emo != TARGET && emo != EQ) {
      gL = drawEye(EYE_LX + ox, EYE_CY + oy, w, hl, cur, cur.arch, false, gx, gy);
      gR = drawEye(EYE_RX + ox, EYE_CY + oy, w, hr, cur, archR, true, gx, gy);
    }

    switch (emo) {
    case SCAN:    fxScan();    break;
    case LOADER:  fxLoader();  break;
    case ZZZ:     fxZzz();     break;
    case LOVE:    fxLove();    break;
    case DIZZY:   fxDizzy();   break;
    case SPARKLE: fxSparkle(); break;
    case SAD:     fxSad();     break;
    case SHY:     fxShy();     break;
    case SQUARE:   fxSquare();   break;
    case CONFUSED: fxConfused(); break;
    case WINK:     fxWink();     break;
    case TARGET:   fxTarget();   break;
    case SHADES:   fxShades();   break;
    case EQ:       fxEq();       break;
    default: break;
    }
    u8g2.setDrawColor(1);

    if (emo == PIXEL) pixelate();
    if (glitchOn) glitchBuffer();
    u8g2.sendBuffer();
  }
};

// Scripted demo actions
static void runStep(Eyes &e, uint8_t step) {
  switch (step) {
  case 0:  e.blink(); break;
  case 1:  e.lookAt(-1.0f, 0.0f); break;
  case 2:  e.lookAt(1.0f, 0.0f); break;
  case 3:  e.lookAt(0.0f, -1.0f); break;
  case 4:  e.setEmotion(HAPPY); break;
  case 5:  e.setEmotion(SURPRISED); break;
  case 6:  e.setEmotion(SQUARE); break;
  case 7:  e.setEmotion(DETERMINED); break;
  case 8:  e.setEmotion(TARGET); break;
  case 9:  e.setEmotion(SCAN); break;
  case 10: e.setEmotion(GLITCH); break;
  case 11: e.setEmotion(PIXEL); break;
  case 12: e.setEmotion(LOADER); break;
  case 13: e.setEmotion(HAPPY); beep(1800, 40); break;  // "done" ping
  case 14: e.setEmotion(LOVE); break;
  case 15: e.setEmotion(WINK); break;
  case 16: e.setEmotion(SPARKLE); break;
  case 17: e.setEmotion(SHY); break;
  case 18: e.setEmotion(ROLL); break;
  case 19: e.setEmotion(CONFUSED); break;
  case 20: e.setEmotion(DIZZY); break;
  case 21: e.setEmotion(EQ); break;
  case 22: e.setEmotion(SHADES); break;
  case 23: e.setEmotion(SAD); break;
  case 24: e.setEmotion(SLEEPY); break;
  case 25: e.setEmotion(ZZZ); break;
  case 26: e.reset(); break;
  }
}

// ═════════════════════════════════════════════════════════
//  EXIT SEQUENCE  (plays after the happy squint)
//    eyes flatten into bars -> bars slide together into one line
//    -> line opens into a band while "Gaming never stops"
//    spreads in from the centre -> a light sweeps across the text
//    -> band closes -> line shrinks to nothing -> main menu
// ═════════════════════════════════════════════════════════
// End time (seconds) of each outro phase:
constexpr float OUT_B1 = 0.25f;   // eyes flatten to bars
constexpr float OUT_B2 = 0.45f;   // bars merge into one line
constexpr float OUT_C  = 0.90f;   // line opens into the text band
constexpr float OUT_D  = 2.00f;   // text holds (shine sweep + star)
constexpr float OUT_E1 = 2.30f;   // band closes
constexpr float OUT_E2 = 2.45f;   // line shrinks away
static const char OUT_MSG[] = "Gaming never stops";

static inline float easeIO(float t) {
  t = ei_clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}
static inline int lerpi(int a, int b, float k) {
  return a + (int)((b - a) * k + ((b >= a) ? 0.5f : -0.5f));
}

static void outro(Eyes &eyes) {
  const int n = (int)sizeof(OUT_MSG) - 1;
  int gw[sizeof(OUT_MSG)];
  int xs[sizeof(OUT_MSG)];
  int tw = 0;

  // Pick the biggest font that fits the screen width
  const uint8_t *fonts[2] = {u8g2_font_helvB10_tr, u8g2_font_6x13B_tr};
  u8g2.setFontMode(1);
  for (int f = 0; f < 2; f++) {
    u8g2.setFont(fonts[f]);
    tw = 0;
    // drawGlyph() returns the glyph's advance width; clip to a 1x1 window
    // so this measuring pass draws (almost) nothing, then wipe the buffer.
    u8g2.setClipWindow(0, 0, 1, 1);
    u8g2.setDrawColor(1);
    for (int i = 0; i < n; i++) {
      gw[i] = (int)u8g2.drawGlyph(0, 20, OUT_MSG[i]);
      xs[i] = tw;
      tw += gw[i];
    }
    u8g2.setMaxClipWindow();
    u8g2.clearBuffer();
    if (tw <= SCREEN_W - 6) break;
  }
  const int tx = (SCREEN_W - tw) / 2;
  for (int i = 0; i < n; i++) xs[i] += tx;
  const float cxT = tx + tw * 0.5f;
  const int asc = u8g2.getAscent(), desc = u8g2.getDescent();
  const int base = EYE_CY + (asc + desc) / 2;   // text baseline (centred)
  const int band = (asc - desc) + 8;            // full height of the text band
  const int bandY0 = EYE_CY - band / 2;

  // Draw the band at openness k (0 = thin line, 1 = fully open).
  // The letters are spread out from the centre while it is opening/closing.
  auto drawBand = [&](float k) {
    int wh = 3 + (int)((band - 3) * k + 0.5f);
    int y0 = EYE_CY - wh / 2;
    int y1 = y0 + wh;
    u8g2.setDrawColor(1);
    if (wh <= 4) {
      rect(0, y0, SCREEN_W, wh, 1);
      return;
    }
    u8g2.setClipWindow(0, y0 + 2, SCREEN_W, y1 - 2);
    float sp = 0.25f * (1.0f - k);
    for (int i = 0; i < n; i++) {
      int gx0 = xs[i] + (int)((xs[i] + gw[i] * 0.5f - cxT) * sp);
      if (gx0 < 0 || gx0 + gw[i] > SCREEN_W) continue;
      u8g2.drawGlyph(gx0, base, OUT_MSG[i]);
    }
    u8g2.setMaxClipWindow();
    u8g2.drawHLine(0, y0, SCREEN_W);
    u8g2.drawHLine(0, y1 - 1, SCREEN_W);
  };

  // Where the happy eyes were
  const float ew = EYE_W * eyes.cur.w;
  const float eh = EYE_H * eyes.cur.h;
  const int iw = (int)(ew + 0.5f);
  const int cxL = EYE_LX + eyes.curOx, cxR = EYE_RX + eyes.curOx;
  const int cy0 = EYE_CY + eyes.curOy;
  const int xL = cxL - iw / 2, xR = cxR - iw / 2;

  uint32_t t0 = millis();
  bool ping1 = false, ping2 = false;

  while (true) {
    float t = (millis() - t0) / 1000.0f;

    backgroundTick();                 // Wi-Fi / NTP keep running during the outro
    if (btnPressed()) return;         // GPIO 0 skips the outro instantly

    if (t >= OUT_E2) break;
    if (!ping1 && t >= OUT_B2) { beep(1800, 40); ping1 = true; }
    if (!ping2 && t >= OUT_C)  { beep(2400, 50); ping2 = true; }

    u8g2.clearBuffer();
    u8g2.setDrawColor(1);

    if (t < OUT_B1) {
      // 1) eyes flatten (still happy-shaped) down to thin bars
      float k = easeIO(t / OUT_B1);
      drawEye(cxL, cy0, ew, eh * (1.0f - k), eyes.cur, eyes.cur.arch, false,
              eyes.gx, eyes.gy);
      drawEye(cxR, cy0, ew, eh * (1.0f - k), eyes.cur, eyes.cur.arch, true,
              eyes.gx, eyes.gy);
    } else if (t < OUT_B2) {
      // 2) bars slide together and stretch into one full-width line
      float k = easeIO((t - OUT_B1) / (OUT_B2 - OUT_B1));
      int cy = lerpi(cy0, EYE_CY, k);
      int l0 = lerpi(xL, 0, k), l1 = lerpi(xL + iw, SCREEN_W / 2, k);
      int r0 = lerpi(xR, SCREEN_W / 2, k), r1 = lerpi(xR + iw, SCREEN_W, k);
      rect(l0, cy - 1, l1 - l0, 3, 1);
      rect(r0, cy - 1, r1 - r0, 3, 1);
    } else if (t < OUT_C) {
      // 3) line opens into the band, letters glide into place
      drawBand(easeIO((t - OUT_B2) / (OUT_C - OUT_B2)));
    } else if (t < OUT_D) {
      // 4) hold: light sweep across the text, then a twinkling star
      drawBand(1.0f);
      float p = (t - OUT_C) / 0.7f;
      if (p < 1.0f) {
        int sx = tx - 8 + (int)(easeIO(p) * (tw + 16));
        int xa = sx < tx ? tx : sx;
        int xb = sx + 6;
        if (xb > tx + tw) xb = tx + tw;
        if (xb > xa) {
          u8g2.setDrawColor(2);               // XOR -> inverted strip
          u8g2.drawBox(xa, bandY0 + 2, xb - xa, band - 4);
          u8g2.setDrawColor(1);
        }
      } else {
        int sxs = tx + tw + 5;
        if (sxs > SCREEN_W - 4) sxs = SCREEN_W - 4;
        int s = 1 + (int)(2.0f * (0.5f + 0.5f * sinf(t * 12.0f)));
        drawStar(sxs, base - asc + 1, s);
      }
    } else if (t < OUT_E1) {
      // 5) band closes, letters draw back together
      drawBand(1.0f - easeIO((t - OUT_D) / (OUT_E1 - OUT_D)));
    } else {
      // 6) the line shrinks to the centre and vanishes
      float k = easeIO((t - OUT_E1) / (OUT_E2 - OUT_E1));
      int half = (int)((SCREEN_W / 2) * (1.0f - k));
      if (half > 0) rect(SCREEN_W / 2 - half, EYE_CY - 1, half * 2, 3, 1);
    }

    u8g2.sendBuffer();
    delay(1);
  }

  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFontMode(0);
}

// Runs the intro.
//   isDone : polled every frame; when it returns true the eyes do their
//            happy squint + "Gaming never stops" outro and the intro ends.
//   returns true if the user SKIPPED it with GPIO 0 (exits immediately, no outro).
static bool run(bool (*isDone)()) {
  Eyes eyes;
  uint32_t now = millis(), last = now;
  uint32_t stepT = now - STEP_MS + 1800;  // first action ~1.8 s after boot
  uint8_t step = 1;                        // (the wake-up replaces the first blink)
  uint8_t hold = 1;                        // ticks to wait before next step
  bool leaving = false, chirp2 = false;
  uint32_t leaveT = 0;
  eyes.begin(now);
  btnPressed();                            // flush any stale edge

  while (true) {
    now = millis();
    float dt = (now - last) / 1000.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;

    backgroundTick();                      // Wi-Fi connect / NTP / weather fetch

    // GPIO 0 at ANY time -> skip straight out of the intro
    if (btnPressed()) {
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      u8g2.setDrawColor(1);
      u8g2.setFontMode(0);
      return true;
    }

    if (!leaving && isDone && isDone()) {
      leaving = true;
      leaveT = now;
      eyes.setEmotion(HAPPY);
      beep(1000, 70);
    }

    if (leaving) {
      if (!chirp2 && now - leaveT > 110) {
        beep(1500, 90);
        chirp2 = true;
      }
      if (now - leaveT > LEAVE_MS) break;
    } else if (now - stepT >= STEP_MS * (uint32_t)hold) {
      stepT = now;
      runStep(eyes, step);
      hold = STEP_HOLD[step];
      step = (step + 1) % NUM_STEPS;
    }

    eyes.update(dt, now);
    eyes.draw();
    delay(1);
  }

  outro(eyes);   // "Gaming never stops" exit animation (GPIO 0 can still skip it)
  u8g2.setDrawColor(1);
  u8g2.setFontMode(0);
  return false;
}

}  // namespace EyesIntro

// ═════════════════════════════════════════════════════════
//  ENTRY POINT - call from setup() after the OLED is initialised
// ═════════════════════════════════════════════════════════
// Returns true if the user skipped the intro with GPIO 0.
bool showEyesIntro(bool (*isDone)()) {
  return EyesIntro::run(isDone);
}

#endif