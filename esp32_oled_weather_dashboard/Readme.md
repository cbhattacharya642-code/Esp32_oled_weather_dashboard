# ESP32 OLED Weather Dashboard

A clock, weather and air-quality dashboard for an ESP32 and a 0.96" SSD1306 OLED, with an animated eyes boot intro and a playable Flappy Bird. Everything is controlled with one button: the on-board BOOT button (GPIO 0).

- **Clock:** NTP time (IST by default) with a seconds bar
- **Weather:** temperature, humidity, wind and conditions from [Open-Meteo](https://open-meteo.com) (no API key)
- **Air quality:** US AQI, PM2.5 and PM10 from Open-Meteo
- **Arcade:** Flappy Bird, with levels, bonus stars and a best score saved to flash
- **Boot intro:** animated eyes that play while Wi-Fi connects

## Hardware

| Part | Connection |
| --- | --- |
| ESP32 DevKit | USB |
| SSD1306 128x64 I2C OLED (0x3C) | VCC to 3V3, GND to GND, SDA to GPIO 21, SCL to GPIO 22 |
| Button | The on-board BOOT button (GPIO 0, active LOW). No extra hardware needed |

> **GPIO 0 is a strapping pin.** If it is held LOW while the chip resets or powers on, the ESP32 enters bootloader mode and the sketch won't run. Don't hold the button while plugging in power or pressing EN/RST.

## Software setup

1. Install the Arduino IDE and add the **esp32 by Espressif Systems** board package. Select **ESP32 Dev Module**.
2. Install these libraries from the Library Manager:
   - **Adafruit SSD1306** (also installs Adafruit BusIO)
   - **Adafruit GFX Library**
   - **ArduinoJson**, version **7.x** (version 6 will not compile)
3. Put all of these files in one folder named `esp32_oled_weather_dashboard`:
   - `esp32_oled_weather_dashboard.ino`
   - `EyesIntro.h`
   - `FlappyBird.h`
   - `GfxCompat.h`
4. Open the `.ino` and edit the **USER CONFIGURATION** section:

   ```cpp
   static const char*  WIFI_SSID      = "Your_wifi_ssid";
   static const char*  WIFI_PASS      = "Your_wifi_password";
   static const char*  LOCATION_NAME  = "KHARAGPUR";
   static const float  LOCATION_LAT   = 22.3460f;
   static const float  LOCATION_LON   = 87.2320f;
   static const long   GMT_OFFSET_SEC = 19800;   // IST = UTC+5:30
   static const bool   USE_24H_CLOCK  = true;
   ```

5. Upload. Don't commit your real Wi-Fi name and password if you publish your fork.

The header file names are case-sensitive on Linux and macOS. Keep them exactly as listed above.

## Controls (GPIO 0 / BOOT)

**Boot**

| Action | Result |
| --- | --- |
| Press at any time | Skip the eyes intro and go to the clock |

**Dashboard**

| Action | Result |
| --- | --- |
| Single click | Next screen (fires 400 ms after release) |
| Double-click (within 400 ms) | Launch Flappy Bird from any screen |
| Hold 2 seconds | Force a weather and air-quality refresh |
| Press while the screen is off | Wake the display, staying on the same screen |
| No presses for 60 seconds | OLED turns off (Wi-Fi, clock and polling keep running) |

**Flappy Bird**

| Action | Result |
| --- | --- |
| Click on the Start or Game Over screen | Play or retry |
| Press during play | Flap |
| Hold 2 seconds (anywhere in the game) | Quit back to the dashboard |
| Idle 20 seconds on Start or Game Over | Back to the dashboard |

While you play, the 60-second sleep timer and screen cycling are paused. Wi-Fi, NTP and weather polling keep running in the background.

## How it works

- `loop()` is non-blocking. Everything runs on `millis()` timers.
- HTTPS requests run in a FreeRTOS task on core 0, so the clock and animations never freeze.
- `EyesIntro.h` and `FlappyBird.h` were written for the U8g2 library. `GfxCompat.h` provides a small stand-in `u8g2` object that draws onto the sketch's shared Adafruit `display`, so there is only one display driver and no I2C conflict.
- If Wi-Fi or the API fails, the clock keeps running, a warning icon blinks, the last good data stays on screen, and the sketch retries every 60 seconds.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| `ArduinoJson.h: No such file or directory` | Install ArduinoJson 7.x from the Library Manager |
| Compile errors mentioning `JsonDocument` | You have ArduinoJson 6. Install 7.x |
| Header file not found | Make sure all four files are in the same folder, with names matching the `#include` lines |
| Sketch won't start after reset | You held the BOOT button during reset. Release it and reset again |
| Display stays blank | Check wiring and that the OLED address is 0x3C |
| Time shows `NO TIME SYNC` | Check Wi-Fi details. NTP needs internet access |

## Credits

Weather and air-quality data by [Open-Meteo](https://open-meteo.com).