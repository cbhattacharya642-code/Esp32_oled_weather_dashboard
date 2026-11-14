/*
 ===========================================================================
  ESP32 OLED ENV-NODE  -  Clock / Weather / Air-Quality Dashboard
 ===========================================================================
  Board    : ESP32 DevKit (ESP32 Arduino core 2.x or 3.x by Espressif)
  Display  : 0.96" SSD1306 128x64 I2C OLED @ 0x3C
  Data     : Open-Meteo Weather + Air-Quality APIs (free, NO API key)
  Time     : NTP (pool.ntp.org), IST (UTC+5:30)

  ---------------------------------------------------------------------------
  WIRING
  ---------------------------------------------------------------------------
    OLED VCC  -> 3V3          OLED GND -> GND
    OLED SDA  -> GPIO 21      OLED SCL -> GPIO 22
    Button    -> between GPIO 0 and GND  (internal INPUT_PULLUP, active LOW)
                 The on-board BOOT button on a standard ESP32 DevKit IS this
                 button, so no extra hardware is required.

    !! GPIO 0 is a STRAPPING PIN !!
       If it is held LOW while the chip resets / powers on, the ESP32 enters
       ROM bootloader (download) mode and your sketch will NOT run.
       -> Do not hold the button while plugging in power or pressing EN/RST.
       -> After boot, GPIO 0 is a normal input and is safe to use.
       -> Do not add an external pull-DOWN or a load that pulls GPIO 0 low.

  ---------------------------------------------------------------------------
  ARDUINO IDE LIBRARY MANAGER DEPENDENCIES
  ---------------------------------------------------------------------------
    1. "Adafruit SSD1306"   (Adafruit)       - also installs Adafruit BusIO
    2. "Adafruit GFX Library" (Adafruit)
    3. "ArduinoJson"        (Benoit Blanchon) - version 7.x REQUIRED
    Built in with the ESP32 core: WiFi, WiFiClientSecure, HTTPClient, Wire,
    time.h, Preferences.
    Board manager: "esp32 by Espressif Systems".  Select "ESP32 Dev Module".

  Sketch folder must contain (same folder as this .ino):
    EyesIntro.h   FlappyBird.h   GfxCompat.h
    (the two headers draw through GfxCompat.h onto the shared `display`;
     U8g2 is NOT needed and there is only ONE I2C display driver.)

  ---------------------------------------------------------------------------
  BUTTON BEHAVIOUR (GPIO 0)
  ---------------------------------------------------------------------------
    Short press (screen on)  : next screen (fires 400 ms after release, so a
                               second click can be recognised as a double-click)
    DOUBLE-CLICK (<= 400 ms) : launch FLAPPY BIRD from any screen
                               (the 4th "ARCADE" screen shows this hint)
    Short press (screen off) : wake display, stay on the SAME screen
    Hold >= 2 s              : force weather + AQI refresh ("Syncing..." overlay)
    60 s without any press   : OLED turned OFF (burn-in protection).
                               Wi-Fi, clock and data polling keep running.
    Boot                     : animated eyes intro; press GPIO 0 at any time
                               to skip straight to the clock.

  FLAPPY BIRD (GPIO 0 only)
    Start / Game-over screen : click = play / retry
    In game                  : every press = flap
    Hold >= 2 s anywhere     : quit back to the dashboard (no reboot)
    Idle 20 s on Start/Over  : back to the dashboard
    While playing, auto-sleep and screen cycling are paused; Wi-Fi, NTP and
    weather polling keep running in the background.

  ---------------------------------------------------------------------------
  ARCHITECTURE NOTES
  ---------------------------------------------------------------------------
    * loop() is fully non-blocking (millis() state machines everywhere).
    * HTTPS requests run in a FreeRTOS task on core 0, so the clock, the
      animations and the button never freeze while a request is in flight.
    * Shared weather/AQI data is guarded by a mutex.
    * Offline / API failure -> clock keeps running, blinking warning icon,
      last good data stays on screen, retry every 60 s.
 ===========================================================================
*/

#define SSD1306_NO_SPLASH            // skip Adafruit logo splash (saves flash)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// ===========================================================================
//  USER CONFIGURATION  (edit these)
// ===========================================================================
static const char*  WIFI_SSID      = "Your_wifi_ssid";
static const char*  WIFI_PASS      = "Your_wifi_password";
//==========================================================
//Adjust the latitude and longitude according to your city
//==========================================================
static const char*  LOCATION_NAME  = "KHARAGPUR";   //<-- Your city name
static const float  LOCATION_LAT   = 22.3460f;     // <-- latitude
static const float  LOCATION_LON   = 87.2320f;     // <-- longitude

static const long   GMT_OFFSET_SEC = 19800;        // IST = UTC+5:30
static const int    DST_OFFSET_SEC = 0;            // India has no DST
static const char*  NTP_SERVER_1   = "pool.ntp.org";
static const char*  NTP_SERVER_2   = "time.nist.gov";
static const bool   USE_24H_CLOCK  = true;

// ---- Pins ----
static constexpr uint8_t PIN_BUTTON = 0;           // strapping pin, see notes!
static constexpr uint8_t PIN_SDA    = 21;
static constexpr uint8_t PIN_SCL    = 22;

// ---- Display ----
static constexpr int      SCREEN_W   = 128;
static constexpr int      SCREEN_H   = 64;
static constexpr uint8_t  OLED_ADDR  = 0x3C;

// ---- Timing (all in ms) ----
static constexpr uint32_t DEBOUNCE_MS          = 30;     // lock-out after an edge (0-latency press)
static constexpr uint32_t DOUBLE_CLICK_MS      = 400;    // 2nd press must follow 1st release within this
static constexpr uint32_t LONG_PRESS_MS        = 2000;
static constexpr uint32_t AUTO_CYCLE_MS        = 6000;
static constexpr uint32_t SLEEP_AFTER_MS       = 60000;
static constexpr uint32_t POLL_INTERVAL_MS     = 10UL * 60UL * 1000UL;  // 10 min
static constexpr uint32_t RETRY_INTERVAL_MS    = 60UL * 1000UL;         // after a failure
static constexpr uint32_t WIFI_RETRY_MS        = 20000;
static constexpr uint32_t HTTP_TIMEOUT_MS      = 6000;
static constexpr uint32_t INTRO_MIN_MS         = 6000;   // eyes always play at least this long
static constexpr uint32_t BOOT_WIFI_TIMEOUT_MS = 15000;  // give up waiting for Wi-Fi (offline mode)
static constexpr uint32_t BOOT_MAX_MS          = 25000;  // hard cap on the intro
static constexpr uint32_t MIN_SYNC_OVERLAY_MS  = 1000;
static constexpr uint32_t RESULT_OVERLAY_MS    = 900;

// Any epoch below this is treated as "clock not yet set" (Nov 2023)
static constexpr time_t   MIN_VALID_EPOCH      = 1700000000;

// ===========================================================================
//  GLOBALS
// ===========================================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

#define C_ON  SSD1306_WHITE
#define C_OFF SSD1306_BLACK

// Mini-game + boot animation. They draw on the shared `display` above through
// GfxCompat.h and are controlled by the single GPIO 0 button (hooks below).
#include "EyesIntro.h"
#include "FlappyBird.h"

enum Screen : uint8_t { SCREEN_CLOCK = 0, SCREEN_WEATHER = 1, SCREEN_AQI = 2, SCREEN_ARCADE = 3, SCREEN_COUNT = 4 };
enum WxIcon : uint8_t { WX_SUN, WX_MOON, WX_PARTLY, WX_CLOUD, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM };
enum OverlayState : uint8_t { OV_NONE, OV_SYNCING, OV_RESULT };

// ---- Shared environment data (written by fetch task, read by UI) ----
struct EnvData {
  bool  haveWeather = false;
  bool  haveAqi     = false;
  float tempC       = 0;
  float windKmh     = 0;
  int   humidity    = 0;
  int   wmoCode     = 0;
  bool  isDay       = true;
  int   usAqi       = 0;
  float pm25        = 0;
  float pm10        = 0;
};
static EnvData          envShared;            // protected by dataMutex
static EnvData          envView;              // UI-side snapshot
static SemaphoreHandle_t dataMutex       = nullptr;
static TaskHandle_t      fetchTaskHandle = nullptr;

static volatile bool     fetchBusy   = false;
static volatile bool     lastFetchOk = true;   // true until proven otherwise
static volatile bool     fetchedOnce = false;
static volatile uint32_t nextFetchAt = 0;

// ---- UI state ----
static uint8_t  screen            = SCREEN_CLOCK;
static uint32_t lastScreenChange  = 0;
static uint32_t screenEnterMs     = 0;
static uint32_t lastRender        = 0;
static time_t   lastRenderedSec   = 0;
static bool     displayAsleep     = false;
static uint32_t lastActivity      = 0;

// ---- Overlay ----
static OverlayState ov      = OV_NONE;
static uint32_t     ovStart = 0;

// ---- Button (single debouncer shared by dashboard, intro and game) ----
static bool     btnStable     = HIGH;
static uint32_t btnChangedAt  = 0;
static uint32_t btnPressedAt  = 0;
static bool     evPress       = false;       // debounced edges waiting to be consumed
static bool     evRelease     = false;
static bool     longFired     = false;
static bool     pressConsumed = false;       // press was used (wake / double-click / skip)
static bool     clickPending  = false;       // first click released, waiting for a 2nd
static uint32_t clickAt       = 0;

// ---- Wi-Fi ----
static bool     wasConnected   = false;
static uint32_t lastWifiTry    = 0;
static uint32_t lastRssiPoll   = 0;
static uint8_t  wifiLevel      = 0;          // 0..4

// ---- Slide transition ----
static constexpr size_t  BUF_SIZE     = SCREEN_W * SCREEN_H / 8;   // 1024
static constexpr uint8_t TRANS_STEPS  = 8;
static const uint8_t     TRANS_OFFSETS[TRANS_STEPS] = {10, 28, 52, 76, 98, 114, 124, 128}; // ease-out
static uint8_t  transOld[BUF_SIZE];
static uint8_t  transNew[BUF_SIZE];
static bool     transActive = false;
static uint8_t  transStep   = 0;
static uint32_t transLast   = 0;

// ===========================================================================
//  PIXEL ART (8x8 bitmaps, PROGMEM)
// ===========================================================================
static const unsigned char BMP_THERMO[] PROGMEM = {
  0x18, 0x24, 0x24, 0x2C, 0x2C, 0x5A, 0x5A, 0x3C
};
static const unsigned char BMP_DROP[] PROGMEM = {
  0x10, 0x10, 0x38, 0x38, 0x7C, 0x7C, 0x7C, 0x38
};

// ===========================================================================
//  SMALL HELPERS
// ===========================================================================
static bool getTimeNow(struct tm &t) {
  time_t now;
  time(&now);
  if (now < MIN_VALID_EPOCH) return false;   // not synced yet (never blocks)
  localtime_r(&now, &t);
  return true;
}

static inline bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

static void drawCentered(const char *s, int cx, int y) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(cx - (int)w / 2 - x1, y);
  display.print(s);
}

static void refreshEnvView() {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    envView = envShared;
    xSemaphoreGive(dataMutex);
  }                                           // else: keep previous snapshot
}

static bool warnActive() { return !wifiUp() || !lastFetchOk; }

// ---- WMO weather-code mapping ----
static WxIcon wmoToIcon(int c, bool day) {
  if (c == 0)                         return day ? WX_SUN : WX_MOON;
  if (c == 1 || c == 2)               return WX_PARTLY;
  if (c == 3)                         return WX_CLOUD;
  if (c == 45 || c == 48)             return WX_FOG;
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86)   return WX_SNOW;
  if (c >= 95)                        return WX_STORM;
  return WX_CLOUD;
}

static const char *wmoToText(int c) {
  switch (c) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Frzg drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Rain showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm + hail";
    default: return "Unknown";
  }
}

// ---- US AQI rating ----
static const char *aqiLabel(int aqi, bool have) {
  if (!have)     return "NO DATA";
  if (aqi <= 50)  return "GOOD";
  if (aqi <= 100) return "MODERATE";
  if (aqi <= 150) return "POOR";
  if (aqi <= 200) return "UNHEALTHY";
  if (aqi <= 300) return "SEVERE";
  return "HAZARDOUS";
}

static void fmtPM(char *out, size_t n, float v) {
  if (v < 100.0f) snprintf(out, n, "%.1f", v);
  else            snprintf(out, n, "%.0f", v);
}

// ===========================================================================
//  ICON / WIDGET DRAWING
// ===========================================================================
static void drawWifiBars(int x, int y) {
  bool conn = wifiUp();
  for (int i = 0; i < 4; i++) {
    int h  = 2 + i * 2;
    int bx = x + i * 3;
    if (conn && i < wifiLevel) display.fillRect(bx, y + 8 - h, 2, h, C_ON);
    else                       display.drawFastHLine(bx, y + 7, 2, C_ON);   // empty slot
  }
  if (!conn) {                                                               // small "x"
    display.drawLine(x + 13, y + 1, x + 17, y + 5, C_ON);
    display.drawLine(x + 17, y + 1, x + 13, y + 5, C_ON);
  }
}

static void drawWarning(int x, int y) {          // 9x9 blinking triangle with "!"
  if ((millis() / 600) & 1) return;
  display.drawTriangle(x + 4, y, x, y + 8, x + 8, y + 8, C_ON);
  display.drawFastVLine(x + 4, y + 3, 3, C_ON);
  display.drawPixel(x + 4, y + 7, C_ON);
}

static void drawWindIcon(int x, int y) {         // 8x8 procedural "wind" glyph
  display.drawFastHLine(x, y + 1, 6, C_ON);
  display.drawPixel(x + 6, y, C_ON);
  display.drawPixel(x + 7, y + 1, C_ON);
  display.drawFastHLine(x, y + 4, 8, C_ON);
  display.drawFastHLine(x + 1, y + 6, 5, C_ON);
  display.drawPixel(x + 6, y + 7, C_ON);
  display.drawPixel(x + 7, y + 6, C_ON);
}

static void drawPagination(int xLeft, int y) {   // 3 dots, active = filled
  for (uint8_t i = 0; i < SCREEN_COUNT; i++) {
    int cx = xLeft + i * 8;
    if (i == screen) display.fillCircle(cx, y, 2, C_ON);
    else             display.drawCircle(cx, y, 2, C_ON);
  }
}

static void drawHeader(const char *title) {
  display.setTextSize(1);
  display.setTextColor(C_ON);
  display.setCursor(4, 1);
  display.print(title);
  drawWifiBars(98, 1);
  if (warnActive()) drawWarning(118, 0);
  display.drawFastHLine(0, 10, SCREEN_W, C_ON);
}

// ---- 32x32 weather icon building blocks (origin ox,oy = top-left) ----
static void drawCloudShape(int ox, int oy, int y0, uint16_t col, int g) {
  display.fillCircle(ox + 10, oy + y0 + 10, 6 + g, col);
  display.fillCircle(ox + 18, oy + y0 + 7,  7 + g, col);
  display.fillCircle(ox + 24, oy + y0 + 11, 5 + g, col);
  display.fillRect(ox + 10 - g, oy + y0 + 9 - g, 15 + 2 * g, 8 + 2 * g, col);
}

static void drawSun(int cx, int cy, int r, int rayIn, int rayOut, float rot) {
  display.fillCircle(cx, cy, r, C_ON);
  for (int k = 0; k < 8; k++) {
    float a = rot + k * (PI / 4.0f);
    int x1 = cx + (int)roundf(cosf(a) * rayIn);
    int y1 = cy + (int)roundf(sinf(a) * rayIn);
    int x2 = cx + (int)roundf(cosf(a) * rayOut);
    int y2 = cy + (int)roundf(sinf(a) * rayOut);
    display.drawLine(x1, y1, x2, y2, C_ON);
  }
}

static void drawWeatherIcon(int ox, int oy, WxIcon icon, bool day, bool valid) {
  uint8_t ph = (millis() / 250) % 3;             // animation phase

  if (!valid) {                                  // no data yet: "?"
    display.drawCircle(ox + 16, oy + 16, 12, C_ON);
    display.setTextSize(2);
    display.setTextColor(C_ON);
    display.setCursor(ox + 10, oy + 9);
    display.print('?');
    return;
  }

  switch (icon) {
    case WX_SUN:
      drawSun(ox + 16, oy + 16, 7, 10, 14, ph * (PI / 24.0f));
      break;

    case WX_MOON:
      display.fillCircle(ox + 14, oy + 16, 10, C_ON);
      display.fillCircle(ox + 20, oy + 12, 9, C_OFF);
      if (ph != 1) display.drawPixel(ox + 26, oy + 7, C_ON);
      if (ph != 2) display.drawPixel(ox + 22, oy + 24, C_ON);
      display.drawPixel(ox + 27, oy + 15, C_ON);
      break;

    case WX_PARTLY:
      if (day) {
        drawSun(ox + 11, oy + 11, 5, 8, 11, ph * (PI / 24.0f));
      } else {
        display.fillCircle(ox + 11, oy + 11, 7, C_ON);
        display.fillCircle(ox + 15, oy + 8, 6, C_OFF);
      }
      drawCloudShape(ox, oy, 13, C_OFF, 2);      // black halo separates sun & cloud
      drawCloudShape(ox, oy, 13, C_ON, 0);
      break;

    case WX_CLOUD:
      drawCloudShape(ox, oy, 8, C_ON, 0);
      display.drawFastHLine(ox + 6, oy + 27, 8, C_ON);   // little detail lines
      display.drawFastHLine(ox + 18, oy + 29, 8, C_ON);
      break;

    case WX_FOG:
      drawCloudShape(ox, oy, 2, C_ON, 0);
      for (int i = 0; i < 3; i++) {
        int off = ((ph + i) % 3) * 2;
        display.drawFastHLine(ox + 3 + off, oy + 21 + i * 4, 22, C_ON);
      }
      break;

    case WX_RAIN:
      drawCloudShape(ox, oy, 2, C_ON, 0);
      for (int i = 0; i < 4; i++) {
        int x = ox + 8 + i * 6;
        int y = oy + 21 + ((ph + i) % 3) * 3;
        display.drawLine(x, y, x - 2, y + 3, C_ON);
      }
      break;

    case WX_SNOW:
      drawCloudShape(ox, oy, 2, C_ON, 0);
      for (int i = 0; i < 3; i++) {
        int cx = ox + 8 + i * 8;
        int cy = oy + 22 + ((ph + i) % 3) * 3;
        display.drawPixel(cx, cy, C_ON);
        display.drawPixel(cx - 1, cy, C_ON);
        display.drawPixel(cx + 1, cy, C_ON);
        display.drawPixel(cx, cy - 1, C_ON);
        display.drawPixel(cx, cy + 1, C_ON);
      }
      break;

    case WX_STORM:
      drawCloudShape(ox, oy, 1, C_ON, 0);
      if ((millis() / 400) % 4 != 3) {           // flickering lightning bolt
        display.fillTriangle(ox + 19, oy + 18, ox + 12, oy + 26, ox + 18, oy + 26, C_ON);
        display.fillTriangle(ox + 19, oy + 23, ox + 13, oy + 31, ox + 14, oy + 23, C_ON);
      }
      break;
  }
}

// ===========================================================================
//  SCREEN 1 : CLOCK & STATUS
// ===========================================================================
static void drawClockScreen() {
  struct tm t;
  bool synced = getTimeNow(t);
  char buf[24];

  // ---- status bar ----
  display.setTextSize(1);
  display.setTextColor(C_ON);
  drawWifiBars(2, 1);
  if (synced) {
    strftime(buf, sizeof(buf), "%a %d %b", &t);
    for (char *p = buf; *p; ++p) *p = toupper((unsigned char)*p);
  } else {
    strcpy(buf, "NO TIME SYNC");
  }
  drawCentered(buf, 64, 1);
  if (warnActive()) drawWarning(118, 0);
  display.drawFastHLine(0, 10, SCREEN_W, C_ON);

  // ---- big HH:MM (colon blinks each second) ----
  if (synced) {
    int h = t.tm_hour;
    if (!USE_24H_CLOCK) { h %= 12; if (h == 0) h = 12; }
    snprintf(buf, sizeof(buf), "%02d%c%02d", h, (t.tm_sec % 2 == 0) ? ':' : ' ', t.tm_min);
  } else {
    strcpy(buf, "--:--");
  }
  display.setTextSize(4);
  display.setCursor(4, 12);
  display.print(buf);

  // ---- seconds progress bar + digits ----
  int sec = synced ? t.tm_sec : 0;
  display.drawRoundRect(4, 45, 96, 5, 2, C_ON);
  if (synced) display.fillRect(5, 46, (94 * (sec + 1)) / 60, 3, C_ON);
  display.setTextSize(1);
  display.setCursor(104, 45);
  if (synced) snprintf(buf, sizeof(buf), "%02ds", sec);
  else        strcpy(buf, "--s");
  display.print(buf);

  // ---- bottom mini-ticker ----
  display.drawFastHLine(0, 53, SCREEN_W, C_ON);
  display.drawBitmap(2, 55, BMP_THERMO, 8, 8, C_ON);
  display.setCursor(12, 55);
  if (envView.haveWeather) display.printf("%dC", (int)lroundf(envView.tempC));
  else                     display.print("--C");
  display.setCursor(42, 55);
  if (envView.haveAqi) display.printf("AQI %d", envView.usAqi);
  else                 display.print("AQI --");
  drawPagination(96, 59);
}

// ===========================================================================
//  SCREEN 2 : WEATHER STATION
// ===========================================================================
static void drawWeatherScreen() {
  drawHeader(LOCATION_NAME);

  // icon in a rounded frame
  display.drawRoundRect(2, 13, 40, 40, 6, C_ON);
  WxIcon icon = wmoToIcon(envView.wmoCode, envView.isDay);
  drawWeatherIcon(6, 17, icon, envView.isDay, envView.haveWeather);
  drawPagination(10, 59);

  // temperature
  char tb[8];
  if (envView.haveWeather) snprintf(tb, sizeof(tb), "%d", (int)lroundf(envView.tempC));
  else                     strcpy(tb, "--");
  display.setTextColor(C_ON);
  display.setTextSize(3);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(tb, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(48, 12);
  display.print(tb);
  int ex = 48 + (int)w + 3;
  display.drawCircle(ex + 1, 14, 2, C_ON);          // degree sign
  display.setTextSize(2);
  display.setCursor(ex + 6, 12);
  display.print('C');

  // humidity
  display.setTextSize(1);
  display.drawBitmap(48, 38, BMP_DROP, 8, 8, C_ON);
  display.setCursor(59, 38);
  if (envView.haveWeather) display.printf("%d%% RH", envView.humidity);
  else                     display.print("--% RH");

  // wind
  drawWindIcon(48, 47);
  display.setCursor(59, 47);
  if (envView.haveWeather) display.printf("%.0f km/h", envView.windKmh);
  else                     display.print("-- km/h");

  // description
  display.setCursor(48, 56);
  display.print(envView.haveWeather ? wmoToText(envView.wmoCode) : "No data");
}

// ===========================================================================
//  SCREEN 3 : AIR QUALITY
// ===========================================================================
static void drawAqiScreen() {
  drawHeader("AIR QUALITY");
  display.setTextColor(C_ON);

  // big AQI number
  char nb[8];
  if (envView.haveAqi) snprintf(nb, sizeof(nb), "%d", envView.usAqi);
  else                 strcpy(nb, "--");
  display.setTextSize(3);
  display.setCursor(4, 13);
  display.print(nb);
  display.setTextSize(1);
  display.setCursor(4, 38);
  display.print("US AQI");

  // rating badge (filled when POOR or worse)
  const char *lbl = aqiLabel(envView.usAqi, envView.haveAqi);
  bool filled = envView.haveAqi && envView.usAqi > 100;
  if (filled) {
    display.fillRoundRect(64, 13, 60, 16, 4, C_ON);
    display.setTextColor(C_OFF);
  } else {
    display.drawRoundRect(64, 13, 60, 16, 4, C_ON);
    display.setTextColor(C_ON);
  }
  drawCentered(lbl, 94, 17);
  display.setTextColor(C_ON);

  // PM values
  char pv[12];
  display.setCursor(64, 32);
  if (envView.haveAqi) { fmtPM(pv, sizeof(pv), envView.pm25); display.printf("PM2.5 %s", pv); }
  else                 display.print("PM2.5 --");
  display.setCursor(64, 40);
  if (envView.haveAqi) { fmtPM(pv, sizeof(pv), envView.pm10); display.printf("PM10  %s", pv); }
  else                 display.print("PM10  --");

  // severity bar (0..300 scale, animates on entry), ticks at 50/100/150/200
  display.drawRoundRect(4, 48, 120, 7, 3, C_ON);
  if (envView.haveAqi) {
    float frac = (float)min(envView.usAqi, 300) / 300.0f;
    float t    = (float)(millis() - screenEnterMs) / 700.0f;
    if (t > 1.0f) t = 1.0f;
    float ease = 1.0f - (1.0f - t) * (1.0f - t);
    int fillW  = (int)(118.0f * frac * ease);
    if (fillW > 0) display.fillRect(5, 49, fillW, 5, C_ON);
  }
  for (int v = 50; v <= 200; v += 50) {
    display.drawFastVLine(4 + (120 * v) / 300, 55, 2, C_ON);
  }
  drawPagination(52, 60);
}

// ===========================================================================
//  SCREEN 4 : ARCADE (hint card for the Flappy Bird mini-game)
// ===========================================================================
static void drawArcadeScreen() {
  drawHeader("ARCADE");
  display.setTextColor(C_ON);

  // mini scene: bobbing bird + a pipe pair
  int bob = (int)lroundf(sinf(millis() / 280.0f) * 2.0f);
  display.fillRect(32, 12, 8, 6, C_ON);           // top pipe
  display.fillRect(30, 17, 12, 3, C_ON);          // top cap
  display.fillRect(30, 35, 12, 3, C_ON);          // bottom cap
  display.fillRect(32, 38, 8, 4, C_ON);           // bottom pipe
  display.drawFastHLine(4, 42, 38, C_ON);         // ground
  int by = 27 + bob;
  display.fillCircle(14, by, 4, C_ON);
  display.drawPixel(15, by - 1, C_OFF);           // eye
  display.drawLine(18, by, 21, by - 1, C_ON);     // beak

  display.setTextSize(2);
  display.setCursor(48, 14);
  display.print("FLAPPY");
  display.setCursor(48, 30);
  display.print("BIRD");
  display.setTextSize(1);

  display.drawRoundRect(2, 45, 124, 11, 3, C_ON);
  drawCentered("DOUBLE-CLICK: PLAY", 64, 47);
  drawPagination(52, 60);
}

// ===========================================================================
//  OVERLAY ("Syncing..." / result)
// ===========================================================================
static void drawOverlay() {
  if (ov == OV_NONE) return;
  const int w = 96, h = 28, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
  display.fillRoundRect(x, y, w, h, 6, C_OFF);
  display.drawRoundRect(x, y, w, h, 6, C_ON);
  display.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 4, C_ON);
  display.setTextSize(1);
  display.setTextColor(C_ON);

  if (ov == OV_SYNCING) {
    drawCentered("Syncing...", SCREEN_W / 2, y + 7);
    int span = w - 16 - 16;
    int px   = (millis() / 20) % (span * 2);
    if (px > span) px = span * 2 - px;                 // ping-pong
    display.drawFastHLine(x + 8, y + 19, w - 16, C_ON);
    display.fillRect(x + 8 + px, y + 17, 16, 4, C_ON);
  } else {
    const char *msg = !wifiUp() ? "No Wi-Fi" : (lastFetchOk ? "Updated!" : "Sync failed");
    drawCentered(msg, SCREEN_W / 2, y + 10);
  }
}

// ===========================================================================
//  RENDERING
// ===========================================================================
static void drawScreenToBuffer() {
  refreshEnvView();
  display.clearDisplay();
  display.setTextWrap(false);
  switch (screen) {
    case SCREEN_CLOCK:   drawClockScreen();   break;
    case SCREEN_WEATHER: drawWeatherScreen(); break;
    case SCREEN_AQI:     drawAqiScreen();     break;
    case SCREEN_ARCADE:  drawArcadeScreen();  break;
  }
}

static void renderCurrent() {
  drawScreenToBuffer();
  drawOverlay();
  display.display();
  lastRender = millis();
  time(&lastRenderedSec);
}

// ---- slide-left transition between screens (non-blocking) ----
static void startTransition(uint8_t target) {
  if (target == screen) return;
  memcpy(transOld, display.getBuffer(), BUF_SIZE);   // whatever is on screen now
  screen        = target;
  screenEnterMs = millis();
  drawScreenToBuffer();
  memcpy(transNew, display.getBuffer(), BUF_SIZE);
  transActive = true;
  transStep   = 0;
  transLast   = 0;
}

static void updateTransition(uint32_t now) {
  if (now - transLast < 15) return;
  transLast = now;
  uint8_t  off = TRANS_OFFSETS[transStep];
  uint8_t *buf = display.getBuffer();
  for (uint8_t page = 0; page < 8; page++) {
    size_t base = (size_t)page * SCREEN_W;
    if (off < SCREEN_W) memcpy(buf + base, transOld + base + off, SCREEN_W - off);
    memcpy(buf + base + (SCREEN_W - off), transNew + base, off);
  }
  display.display();
  if (++transStep >= TRANS_STEPS) {
    transActive   = false;
    screenEnterMs = now;                              // AQI bar animates from here
    lastRender    = now;
  }
}

// ===========================================================================
//  DISPLAY POWER MANAGEMENT
// ===========================================================================
static void displaySleep() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  displayAsleep = true;
  transActive   = false;
}

static void displayWake(uint32_t now) {
  renderCurrent();                                    // refresh GDDRAM first
  display.ssd1306_command(SSD1306_DISPLAYON);
  displayAsleep    = false;
  lastScreenChange = now;                             // give current screen a full slot
}

// ===========================================================================
//  BUTTON (non-blocking, 50 ms debounce)
// ===========================================================================
static bool requestFetch() {
  if (fetchBusy) return false;
  fetchBusy = true;                                   // set BEFORE notify (no race with overlay)
  xTaskNotifyGive(fetchTaskHandle);
  return true;
}

static void goNextScreen(uint32_t now) {
  startTransition((screen + 1) % SCREEN_COUNT);
  lastScreenChange = now;
}

// Low-level debouncer. Safe to call from ANY loop (dashboard, intro, game).
// Lock-out style: an edge is accepted immediately, further edges are ignored
// for DEBOUNCE_MS, so presses register with zero added latency (good for games).
static void btnService(uint32_t now) {
  bool raw = digitalRead(PIN_BUTTON);
  if (raw != btnStable && (now - btnChangedAt) >= DEBOUNCE_MS) {
    btnStable    = raw;
    btnChangedAt = now;
    if (raw == LOW) { evPress = true; btnPressedAt = now; }
    else            { evRelease = true; }
  }
}

// ---- hooks used by EyesIntro.h / FlappyBird.h (declared in GfxCompat.h) ----
bool btnPressed()  { btnService(millis()); bool e = evPress;   evPress   = false; return e; }
bool btnReleased() { btnService(millis()); bool e = evRelease; evRelease = false; return e; }
bool btnHeldFor(uint32_t ms) {
  btnService(millis());
  return btnStable == LOW && (millis() - btnPressedAt) >= ms;
}
uint32_t btnPressStart() { return btnPressedAt; }

void beep(uint16_t, uint16_t) {}      // no buzzer on this board: silent stub

void waitRelease() {                   // wait (max 5 s) for the button to go up
  uint32_t t = millis();
  while (digitalRead(PIN_BUTTON) == LOW && (millis() - t) < 5000) {
    backgroundTick();
    delay(5);
  }
}

// After any blocking section (intro / game) that used the button: swallow the
// press that is still in flight so it can't trigger a screen change or refresh.
static void btnResume() {
  btnService(millis());
  evPress = evRelease = false;
  pressConsumed = true;
  longFired     = true;
  clickPending  = false;
}

static void launchFlappy(uint32_t now) {
  Serial.println("[GAME] Flappy Bird started");
  ov          = OV_NONE;                // drop any stale overlay
  transActive = false;
  clickPending = false;

  game_flappy();                        // blocks until the player quits (GPIO 0 hold / idle)

  Serial.println("[GAME] back to dashboard");
  now = millis();
  btnResume();
  lastActivity     = now;               // full 60 s of screen-on after the game
  lastScreenChange = now;
  screenEnterMs    = now;
  displayAsleep    = false;
  renderCurrent();
}

static void onPress(uint32_t now) {
  longFired     = false;
  pressConsumed = false;
  lastActivity  = now;
  if (displayAsleep) {                                // wake WITHOUT changing screen
    displayWake(now);
    pressConsumed = true;
    clickPending  = false;
    return;
  }
  if (clickPending && (now - clickAt) <= DOUBLE_CLICK_MS) {   // 2nd click -> game
    clickPending  = false;
    pressConsumed = true;                             // its release does nothing
    launchFlappy(now);
  }
}

static void onRelease(uint32_t now) {
  lastActivity = now;
  if (longFired || pressConsumed) return;
  clickPending = true;                                // act after DOUBLE_CLICK_MS if no 2nd click
  clickAt      = now;
}

static void onLongPress(uint32_t now) {
  lastActivity = now;
  longFired    = true;
  clickPending = false;
  requestFetch();                                     // may already be busy: overlay still shows
  ov      = OV_SYNCING;
  ovStart = now;
}

static void handleButton(uint32_t now) {
  btnService(now);
  if (evPress)   { evPress   = false; onPress(now);   }
  if (evRelease) { evRelease = false; onRelease(now); }

  if (btnStable == LOW && !longFired && (now - btnPressedAt) >= LONG_PRESS_MS) {
    onLongPress(now);
  }
  if (clickPending && (now - clickAt) > DOUBLE_CLICK_MS) {      // no 2nd click: single click
    clickPending = false;
    goNextScreen(now);
  }
}

// ===========================================================================
//  HTTPS FETCH (runs in its own FreeRTOS task)
// ===========================================================================
static bool httpGetJson(const char *url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();   // Skips CA validation (data is public & non-sensitive).
                          // For pinning, replace with client.setCACert(<root CA PEM>).
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("ESP32-EnvNode/1.0");
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[HTTP] GET failed (%d)\n", code);
    http.end();
    return false;
  }
  String body = http.getString();                     // handles chunked encoding
  http.end();

  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[JSON] %s\n", err.c_str());
    return false;
  }
  return true;
}

static bool fetchWeather(EnvData &out) {
  char url[360];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
    "&wind_speed_unit=kmh&timezone=auto",
    LOCATION_LAT, LOCATION_LON);

  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  JsonObject cur = doc["current"];
  if (cur.isNull() || cur["temperature_2m"].isNull() || cur["relative_humidity_2m"].isNull() ||
      cur["weather_code"].isNull() || cur["wind_speed_10m"].isNull()) return false;

  out.tempC    = cur["temperature_2m"].as<float>();
  out.humidity = cur["relative_humidity_2m"].as<int>();
  out.wmoCode  = cur["weather_code"].as<int>();
  out.windKmh  = cur["wind_speed_10m"].as<float>();
  out.isDay    = cur["is_day"].isNull() ? true : (cur["is_day"].as<int>() != 0);
  return true;
}

static bool fetchAirQuality(EnvData &out) {
  char url[300];
  snprintf(url, sizeof(url),
    "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
    "&current=us_aqi,pm2_5,pm10&timezone=auto",
    LOCATION_LAT, LOCATION_LON);

  JsonDocument doc;
  if (!httpGetJson(url, doc)) return false;

  JsonObject cur = doc["current"];
  if (cur.isNull() || cur["us_aqi"].isNull()) return false;   // API may return null AQI

  out.usAqi = cur["us_aqi"].as<int>();
  out.pm25  = cur["pm2_5"].isNull() ? 0.0f : cur["pm2_5"].as<float>();
  out.pm10  = cur["pm10"].isNull()  ? 0.0f : cur["pm10"].as<float>();
  return true;
}

static bool doFetchAll() {
  if (!wifiUp()) return false;

  EnvData w, a;
  bool okW = fetchWeather(w);
  bool okA = fetchAirQuality(a);
  Serial.printf("[FETCH] weather=%s aqi=%s\n", okW ? "ok" : "FAIL", okA ? "ok" : "FAIL");

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    if (okW) {
      envShared.tempC = w.tempC;  envShared.humidity = w.humidity;
      envShared.wmoCode = w.wmoCode;  envShared.windKmh = w.windKmh;
      envShared.isDay = w.isDay;  envShared.haveWeather = true;
    }
    if (okA) {
      envShared.usAqi = a.usAqi;  envShared.pm25 = a.pm25;
      envShared.pm10 = a.pm10;    envShared.haveAqi = true;
    }
    xSemaphoreGive(dataMutex);
  }
  return okW && okA;
}

static void fetchTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);          // sleep until requested
    bool ok     = doFetchAll();
    lastFetchOk = ok;
    fetchedOnce = true;
    nextFetchAt = millis() + (ok ? POLL_INTERVAL_MS : RETRY_INTERVAL_MS);
    fetchBusy   = false;
  }
}

// ===========================================================================
//  BACKGROUND MANAGERS (called from loop)
// ===========================================================================
static void manageWiFi(uint32_t now) {
  bool conn = wifiUp();

  if (conn && !wasConnected) {                        // just (re)connected
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    time_t t; time(&t);
    if (t < MIN_VALID_EPOCH) configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  }
  wasConnected = conn;

  if (conn) {
    if (now - lastRssiPoll >= 2000) {
      lastRssiPoll = now;
      int r = WiFi.RSSI();
      wifiLevel = (r > -55) ? 4 : (r > -65) ? 3 : (r > -75) ? 2 : (r > -85) ? 1 : 0;
    }
  } else {
    wifiLevel = 0;
    if (now - lastWifiTry >= WIFI_RETRY_MS) {         // keep trying in background
      lastWifiTry = now;
      Serial.println("[WiFi] reconnecting...");
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}

static void managePolling(uint32_t now) {
  if (wifiUp() && !fetchBusy && (int32_t)(now - nextFetchAt) >= 0) requestFetch();
}

static void manageOverlay(uint32_t now) {
  if (ov == OV_SYNCING && !fetchBusy && (now - ovStart) >= MIN_SYNC_OVERLAY_MS) {
    ov = OV_RESULT;
    ovStart = now;
  } else if (ov == OV_RESULT && (now - ovStart) >= RESULT_OVERLAY_MS) {
    ov = OV_NONE;
  }
}

// Called from inside blocking sections (intro, game) so Wi-Fi reconnects, NTP
// and weather polling keep working while the main loop() isn't running.
void backgroundTick() {
  uint32_t now = millis();
  manageWiFi(now);
  managePolling(now);
}

// ===========================================================================
//  BOOT (eyes intro runs while Wi-Fi / NTP / first fetch happen in background)
// ===========================================================================
static uint32_t bootT0 = 0;

// Polled by the eyes intro every frame: true => play the outro and finish.
static bool bootFinished() {
  uint32_t el = millis() - bootT0;
  if (el < INTRO_MIN_MS)  return false;            // let the eyes play a bit
  if (el >= BOOT_MAX_MS)  return true;
  if (!wifiUp())          return el >= BOOT_WIFI_TIMEOUT_MS;   // offline mode
  time_t t; time(&t);
  return (t >= MIN_VALID_EPOCH) && fetchedOnce;    // clock synced + first data fetch done
}

// ===========================================================================
//  SETUP / LOOP
// ===========================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);                  // GPIO 0, active LOW
  btnStable = digitalRead(PIN_BUTTON);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] SSD1306 not found at 0x3C - check wiring!");
    for (;;) delay(1000);
  }
  display.clearDisplay();
  display.display();

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, &fetchTaskHandle, 0);

  // Start Wi-Fi in the background, then play the eyes intro. manageWiFi() /
  // managePolling() are pumped from inside the intro (backgroundTick), so NTP
  // and the first weather fetch complete while the eyes animate.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  bootT0 = millis();
  lastWifiTry = bootT0;
  wasConnected = false;

  bool skipped = showEyesIntro(bootFinished);         // GPIO 0 skips instantly
  Serial.println(skipped ? "[BOOT] intro skipped" : "[BOOT] intro finished");
  btnResume();

  uint32_t now = millis();
  lastActivity     = now;
  lastScreenChange = now;
  screenEnterMs    = now;
  screen           = SCREEN_CLOCK;
  renderCurrent();
}

void loop() {
  uint32_t now = millis();

  handleButton(now);
  manageWiFi(now);
  managePolling(now);
  manageOverlay(now);

  // ---- OLED sleep (burn-in protection) ----
  if (!displayAsleep && (now - lastActivity) >= SLEEP_AFTER_MS) displaySleep();

  if (!displayAsleep) {
    // ---- auto-cycle ----
    if (!transActive && (now - lastScreenChange) >= AUTO_CYCLE_MS) goNextScreen(now);

    // ---- draw ----
    if (transActive) {
      updateTransition(now);
    } else {
      bool animated = (ov != OV_NONE) || (screen != SCREEN_CLOCK);
      uint32_t interval = animated ? 100 : 250;
      time_t sec; time(&sec);
      if ((now - lastRender) >= interval || sec != lastRenderedSec) renderCurrent();
    }
  }

  delay(2);   // brief scheduler yield; all logic above is millis()-driven
}
