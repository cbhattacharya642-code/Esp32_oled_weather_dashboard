#ifndef GFXCOMPAT_H
#define GFXCOMPAT_H

/*
 ============================================================
   GfxCompat.h  -  lets EyesIntro.h / FlappyBird.h (written for
   U8g2) draw straight onto the dashboard's shared
   Adafruit_SSD1306 `display` object.

   * Exposes a global `u8g2` object with the subset of the U8g2
     API those headers use, so NO second I2C driver is created
     and there are no bus conflicts.
   * U8g2 fonts map to the built-in 5x7 font (small sizes) or
     FreeSansBold9pt7b (the big "ncenB10 / helvB10" sizes).
   * Declares the single-button (GPIO 0) hooks that the main
     sketch implements.
 ============================================================
*/

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold9pt7b.h>

// Defined in esp32_oled_weather_dashboard.ino
extern Adafruit_SSD1306 display;

// ── Single-button (GPIO 0) + platform hooks, implemented in the .ino ──
extern bool     btnPressed();              // true ONCE per new press (debounced edge)
extern bool     btnReleased();             // true ONCE per release   (debounced edge)
extern bool     btnHeldFor(uint32_t ms);   // button currently down for >= ms
extern uint32_t btnPressStart();           // millis() of the latest press
extern void     beep(uint16_t freq, uint16_t ms);  // no-op unless a buzzer pin is set
extern void     waitRelease();
extern void     backgroundTick();          // keeps Wi-Fi / NTP / polling alive in blocking loops

// ── Fake "font data": first byte selects the real font ──────────────
static const uint8_t u8g2_font_4x6_tr[]     = {0};
static const uint8_t u8g2_font_5x7_tr[]     = {0};
static const uint8_t u8g2_font_6x10_tr[]    = {0};
static const uint8_t u8g2_font_6x13B_tr[]   = {0};
static const uint8_t u8g2_font_ncenB10_tr[] = {1};
static const uint8_t u8g2_font_helvB10_tr[] = {1};

class U8g2Shim : public Adafruit_GFX {
 public:
  U8g2Shim() : Adafruit_GFX(128, 64) {
    setTextWrap(false);
    setTextColor(1);   // single-arg => transparent background
    setTextSize(1);
  }

  // ---- state ----
  void setDrawColor(uint8_t c) { _col = c; }
  void setFontMode(uint8_t) {}
  void setFont(const uint8_t *f) { _font = f ? f[0] : 0; }
  void setClipWindow(int x0, int y0, int x1, int y1) {   // x1,y1 exclusive
    _cx0 = max(0, x0); _cy0 = max(0, y0);
    _cx1 = min(128, x1); _cy1 = min(64, y1);
  }
  void setMaxClipWindow() { _cx0 = 0; _cy0 = 0; _cx1 = 128; _cy1 = 64; }

  // ---- buffer ----
  void clearBuffer() { display.clearDisplay(); }
  void sendBuffer()  { display.display(); }
  uint8_t *getBufferPtr() { return display.getBuffer(); }
  uint8_t getBufferTileWidth()  const { return 16; }
  uint8_t getBufferTileHeight() const { return 8; }

  // ---- every Adafruit_GFX primitive ends up here (clipped, colour-aware) ----
  void drawPixel(int16_t x, int16_t y, uint16_t) override { plotPx(x, y); }
  void drawPixel(int x, int y) { plotPx(x, y); }

  // ---- primitives (U8g2 names) ----
  void drawBox(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, h, 1);
  }
  void drawHLine(int x, int y, int w) { if (w > 0) drawFastHLine(x, y, w, 1); }
  void drawLine(int x0, int y0, int x1, int y1) { Adafruit_GFX::drawLine(x0, y0, x1, y1, 1); }
  void drawDisc(int x, int y, int r) { fillCircle(x, y, r, 1); }
  void drawCircle(int x, int y, int r) { Adafruit_GFX::drawCircle(x, y, r, 1); }
  void drawRBox(int x, int y, int w, int h, int r) {
    if (w <= 0 || h <= 0) return;
    fillRoundRect(x, y, w, h, r, 1);
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2) {   // filled, like U8g2
    fillTriangle(x0, y0, x1, y1, x2, y2, 1);
  }
  void drawFilledEllipse(int cx, int cy, int rx, int ry) {
    if (rx < 1 || ry < 1) return;
    for (int dy = -ry; dy <= ry; dy++) {
      float k = 1.0f - (float)(dy * dy) / (float)(ry * ry);
      if (k < 0) k = 0;
      int dx = (int)(rx * sqrtf(k) + 0.5f);
      drawHLine(cx - dx, cy + dy, 2 * dx + 1);
    }
  }

  // ---- text (y = baseline, like U8g2) ----
  void drawStr(int x, int y, const char *s) {
    applyFont();
    setCursor(x, _font ? y : y - 6);
    print(s);
  }
  int drawGlyph(int x, int y, char c) {
    applyFont();
    setCursor(x, _font ? y : y - 6);
    write((uint8_t)c);
    return advance(c);
  }
  int getStrWidth(const char *s) {
    int w = 0;
    for (; *s; ++s) w += advance(*s);
    return w;
  }
  int getAscent()  const { return _font ? 12 : 7; }
  int getDescent() const { return _font ? -4 : -1; }

 private:
  uint8_t _col = 1, _font = 0;
  int _cx0 = 0, _cy0 = 0, _cx1 = 128, _cy1 = 64;

  void applyFont() {
    Adafruit_GFX::setFont(_font ? &FreeSansBold9pt7b : nullptr);
    setTextSize(1);
  }
  int advance(char c) const {
    if (!_font) return 6;
    uint8_t first = FreeSansBold9pt7b.first;     // ESP32: flash is directly addressable
    uint8_t last  = FreeSansBold9pt7b.last;
    if ((uint8_t)c < first || (uint8_t)c > last) return 0;
    return FreeSansBold9pt7b.glyph[(uint8_t)c - first].xAdvance;
  }
  void plotPx(int x, int y) {
    if (x < _cx0 || y < _cy0 || x >= _cx1 || y >= _cy1) return;
    uint8_t *b = display.getBuffer();
    uint16_t i = x + (y >> 3) * 128;
    uint8_t m = 1 << (y & 7);
    if (_col == 1) b[i] |= m;
    else if (_col == 0) b[i] &= ~m;
    else b[i] ^= m;
  }
};

static U8g2Shim u8g2;

// Horizontally centred string, baseline y
static inline void centreStr(const char *s, uint8_t y) {
  u8g2.drawStr((128 - u8g2.getStrWidth(s)) / 2, y, s);
}

#endif