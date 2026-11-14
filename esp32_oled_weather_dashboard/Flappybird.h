#ifndef FLAPPYBIRD_H
#define FLAPPYBIRD_H

/*
 ============================================================
   FLAPPY BIRD  -  ESP32 Game Console edition
   Ported from the standalone Adafruit_SSD1306 sketch to the
   console's U8g2 + shared-helper API.

   Controls (single button: GPIO 0 / BOOT, active LOW)
     In game ........ every press flaps
     Start / Game over: click      = play / retry
                        hold 2 s   = back to the dashboard
     In game ........ hold 2 s     = quit to the dashboard
     Idle 20 s on Start / Game over also returns to the dashboard.

   Extras kept from the original: levels, bonus stars (+2),
   milestone jingles, non-blocking sound queue, and a best
   score that is saved to flash (ESP32 Preferences / NVS).
 ============================================================
*/

#include <Arduino.h>
#include <Preferences.h>

// U8g2-style drawing on the shared Adafruit display + GPIO 0 hooks
// (btnPressed, btnReleased, btnHeldFor, beep, waitRelease, backgroundTick, centreStr)
#include "GfxCompat.h"

namespace FlappyBird {

// ── Branding (change freely) ──────────────────────────────
static const char *const BRAND = "Chirag";

// ── Tuning ────────────────────────────────────────────────
constexpr int CEILING = 11;            // play-field top (HUD above it)
constexpr int BIRD_X = 25;
constexpr int BIRD_R = 3;
constexpr int PIPE_W = 13;
constexpr int GAP_H = 34;
constexpr float GRAVITY = 0.13f;
constexpr float FLAP_POWER = -1.45f;
constexpr uint32_t STEP_MS = 30;       // fixed physics step (~33 Hz).
                                       // Lower to 25 for a faster feel.
constexpr uint32_t LOCKOUT_MS = 700;   // ignore buttons after dying
constexpr uint32_t HOLD_EXIT_MS = 2000; // hold GPIO 0 this long to quit
constexpr uint32_t IDLE_EXIT_MS = 20000; // idle on Start / Game over -> dashboard

// ── State ─────────────────────────────────────────────────
struct State {
  float birdY, birdVel;
  int pipeX, gapY;
  int score, level, pipeSpeed, nextMilestone;
  bool pipeScored, flapQueued;
  bool starActive;
  int starX, starY;
};

State g;
int bestScore = 0;
Preferences prefs;

// ═════════════════════════════════════════════════════════
//  NON-BLOCKING SOUND QUEUE (built on the console's beep())
// ═════════════════════════════════════════════════════════
struct Note {
  uint16_t freq;  // 0 = rest
  uint16_t ms;
};

constexpr uint8_t SND_Q = 24;
Note sndQ[SND_Q];
uint8_t sndR = 0, sndW = 0;
uint32_t sndEnd = 0;

void soundClear() {
  sndR = sndW = 0;
  sndEnd = 0;
}

void addSound(uint16_t freq, uint16_t ms) {
  uint8_t next = (sndW + 1) % SND_Q;
  if (next == sndR) return;  // queue full: drop note, never block
  sndQ[sndW] = {freq, ms};
  sndW = next;
}

// Call as often as possible (every loop iteration).
void updateSound() {
  backgroundTick();                         // keep Wi-Fi / NTP / polling alive
  uint32_t now = millis();
  if ((int32_t)(now - sndEnd) < 0) return;  // current note still playing
  if (sndR == sndW) return;                 // nothing queued
  Note n = sndQ[sndR];
  sndR = (sndR + 1) % SND_Q;
  sndEnd = now + n.ms;
  if (n.freq) beep(n.freq, n.ms);
}

void flapSound()        { addSound(1000, 25); }
void bonusSound()       { addSound(1200, 45);  addSound(1600, 65); }
void levelSound()       { addSound(700, 60);   addSound(1000, 60);  addSound(1400, 100); }
void startSound()       { addSound(660, 80);   addSound(880, 80);   addSound(1320, 140); }
void achievementSound() { addSound(784, 80);   addSound(988, 80);   addSound(1175, 80); addSound(1568, 180); }
void gameOverSound()    { addSound(880, 100);  addSound(660, 130);  addSound(440, 180);  addSound(250, 260); }

// ═════════════════════════════════════════════════════════
//  INPUT  (single button on GPIO 0)
// ═════════════════════════════════════════════════════════
enum Choice : uint8_t { CHOICE_PLAY, CHOICE_EXIT };

// Start / Game-over screens:
//   click (press + release < 2 s) -> PLAY      hold 2 s -> EXIT      idle 20 s -> EXIT
// Acting on RELEASE is what lets a long hold mean "exit" instead of
// retrying the instant the button goes down.
Choice waitChoice(uint32_t lockoutMs) {
  uint32_t lockEnd = millis() + lockoutMs;
  while ((int32_t)(millis() - lockEnd) < 0) {
    updateSound();
    btnPressed();     // swallow presses / releases during the lockout
    btnReleased();
    delay(5);
  }
  uint32_t idleSince = millis();
  while (true) {
    updateSound();
    uint32_t now = millis();

    bool pressed = btnPressed();
    if (pressed) idleSince = now;

    if (btnHeldFor(HOLD_EXIT_MS) && btnPressStart() > lockEnd) return CHOICE_EXIT;

    if (btnReleased()) {
      idleSince = now;
      // only a press that STARTED after the lockout counts
      if (btnPressStart() > lockEnd && (now - btnPressStart()) < HOLD_EXIT_MS) return CHOICE_PLAY;
    }
    if ((now - idleSince) >= IDLE_EXIT_MS) return CHOICE_EXIT;
    delay(5);
  }
}

// ═════════════════════════════════════════════════════════
//  DRAWING HELPERS
// ═════════════════════════════════════════════════════════
// U8g2 coordinates are unsigned by default, so a negative x
// wraps around and the shape vanishes. Clip in int first.
void box(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  int x2 = x + w, y2 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x2 > SCREEN_W) x2 = SCREEN_W;
  if (y2 > SCREEN_H) y2 = SCREEN_H;
  if (x2 <= x || y2 <= y) return;
  u8g2.drawBox(x, y, x2 - x, y2 - y);
}

void drawHeader(const char *txt) {
  u8g2.setFont(u8g2_font_6x10_tr);
  centreStr(txt, 9);
  u8g2.drawHLine(0, 11, SCREEN_W);
}

void drawBackground() {
  static const uint8_t stars[][2] = {
      {8, 17},  {49, 14}, {75, 23}, {111, 17}, {58, 35}, {95, 43},
      {8, 46},  {115, 51}, {45, 56}, {81, 17},  {102, 29}};

  for (uint8_t i = 0; i < 11; i++) {
    int x = stars[i][0], y = stars[i][1];
    if (abs(x - BIRD_X) < 10 && abs(y - (int)g.birdY) < 8) continue;  // clear of bird
    if (x >= g.pipeX - 2 && x <= g.pipeX + PIPE_W + 2) continue;      // clear of pipe
    if (g.starActive && abs(x - g.starX) < 6 && abs(y - g.starY) < 6) continue;
    u8g2.drawPixel(x, y);
  }

  // Mountains + ground line
  u8g2.drawLine(0, 62, 12, 58);
  u8g2.drawLine(12, 58, 21, 62);
  u8g2.drawLine(91, 62, 104, 57);
  u8g2.drawLine(104, 57, 119, 62);
  u8g2.drawHLine(0, 63, SCREEN_W);
}

void drawPipes() {
  int bottomY = g.gapY + GAP_H;
  box(g.pipeX, CEILING, PIPE_W, g.gapY - CEILING);       // top pipe
  box(g.pipeX, bottomY, PIPE_W, SCREEN_H - bottomY);     // bottom pipe
  box(g.pipeX - 2, g.gapY - 3, PIPE_W + 4, 3);           // top cap
  box(g.pipeX - 2, bottomY, PIPE_W + 4, 3);              // bottom cap
}

void drawStar(int x, int y) {
  u8g2.drawLine(x, y - 4, x, y + 4);
  u8g2.drawLine(x - 4, y, x + 4, y);
  u8g2.drawLine(x - 3, y - 3, x + 3, y + 3);
  u8g2.drawLine(x + 3, y - 3, x - 3, y + 3);
}

void drawBird() {
  int by = (int)g.birdY;
  u8g2.drawDisc(BIRD_X, by, BIRD_R);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(BIRD_X + 1, by - 1);  // eye
  u8g2.setDrawColor(1);
  u8g2.drawLine(BIRD_X + 3, by, BIRD_X + 6, by - 1);  // beak
}

void drawHUD() {
  char buf[12];
  u8g2.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof(buf), "S:%d", g.score);
  u8g2.drawStr(0, 9, buf);
  snprintf(buf, sizeof(buf), "LV:%d", g.level);
  u8g2.drawStr(48, 9, buf);
  snprintf(buf, sizeof(buf), "B:%d", bestScore);
  u8g2.drawStr(91, 9, buf);
  u8g2.drawHLine(0, 10, SCREEN_W);
}

void render() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  drawBackground();
  drawPipes();
  if (g.starActive) drawStar(g.starX, g.starY);
  drawBird();
  drawHUD();
  u8g2.sendBuffer();
}

// ═════════════════════════════════════════════════════════
//  GAME LOGIC
// ═════════════════════════════════════════════════════════
void resetGame() {
  g.birdY = 30;
  g.birdVel = FLAP_POWER;  // small hop on start so the first tap isn't a free fall
  g.pipeX = SCREEN_W;
  g.gapY = random(15, 25);
  g.score = 0;
  g.level = 1;
  g.pipeSpeed = 1;
  g.nextMilestone = 10;
  g.pipeScored = false;
  g.flapQueued = false;
  g.starActive = false;
  g.starX = 0;
  g.starY = 0;
}

void updateLevel() {
  int newLevel = 1 + g.score / 10;
  if (newLevel > 5) newLevel = 5;
  if (newLevel > g.level) {
    g.level = newLevel;
    g.pipeSpeed = 1 + (g.level - 1) / 2;  // 1,1,2,2,3
    if (g.pipeSpeed > 3) g.pipeSpeed = 3;
    levelSound();
  }
}

void checkMilestone() {
  while (g.score >= g.nextMilestone) {
    achievementSound();
    g.nextMilestone += 10;
  }
}

bool collides() {
  if (g.birdY - BIRD_R <= CEILING || g.birdY + BIRD_R >= SCREEN_H) return true;

  bool overlapsPipe = (BIRD_X + BIRD_R >= g.pipeX) &&
                      (BIRD_X - BIRD_R <= g.pipeX + PIPE_W);
  if (overlapsPipe) {
    if (g.birdY - BIRD_R < g.gapY || g.birdY + BIRD_R > g.gapY + GAP_H) return true;
  }
  return false;
}

// One fixed-timestep update. Returns false if the bird died.
bool step() {
  if (g.flapQueued) {
    g.birdVel = FLAP_POWER;
    g.flapQueued = false;
    flapSound();
  }

  g.birdVel += GRAVITY;
  g.birdY += g.birdVel;
  g.pipeX -= g.pipeSpeed;

  // Bonus star
  if (g.starActive) {
    g.starX -= g.pipeSpeed;
    int dx = g.starX - BIRD_X;
    int dy = g.starY - (int)g.birdY;
    if (dx * dx + dy * dy <= 64) {
      g.starActive = false;
      g.score += 2;
      bonusSound();
      updateLevel();
      checkMilestone();
    } else if (g.starX < 5) {  // off-screen (also keeps x-4 >= 0 for drawing)
      g.starActive = false;
    }
  }

  // Pipe passed
  if (!g.pipeScored && g.pipeX + PIPE_W < BIRD_X) {
    g.score++;
    g.pipeScored = true;
    updateLevel();
    checkMilestone();
  }

  // Next pipe (+ 40% chance of a bonus star)
  if (g.pipeX + PIPE_W < 0) {
    g.pipeX = SCREEN_W;
    g.gapY = random(15, 25);
    g.pipeScored = false;
    if (!g.starActive && random(0, 100) < 40) {
      g.starActive = true;
      g.starX = 108;
      g.starY = random(g.gapY + 7, g.gapY + GAP_H - 6);
    }
  }

  return !collides();
}

enum RoundResult : uint8_t { ROUND_DIED, ROUND_NEW_BEST, ROUND_QUIT };

// Plays one round.
RoundResult playRound() {
  resetGame();
  startSound();
  btnPressed();   // flush the edge that started the round

  uint32_t last = millis();
  uint32_t acc = 0;

  while (true) {
    updateSound();

    if (btnPressed()) g.flapQueued = true;        // every GPIO 0 press flaps
    if (btnHeldFor(HOLD_EXIT_MS)) {               // hold 2 s -> quit to dashboard
      soundClear();
      return ROUND_QUIT;
    }

    uint32_t now = millis();
    acc += now - last;
    last = now;
    if (acc > STEP_MS * 4) acc = STEP_MS * 4;  // don't "catch up" after a stall

    if (acc < STEP_MS) {
      delay(1);
      continue;
    }

    bool alive = true;
    while (acc >= STEP_MS) {
      acc -= STEP_MS;
      if (!step()) {
        alive = false;
        break;
      }
    }
    if (!alive) break;

    render();
  }

  bool newBest = g.score > bestScore;
  if (newBest) {
    bestScore = g.score;
    prefs.putInt("best", bestScore);
  }
  return newBest ? ROUND_NEW_BEST : ROUND_DIED;
}

// ═════════════════════════════════════════════════════════
//  SCREENS
// ═════════════════════════════════════════════════════════
Choice showStartScreen() {
  char buf[16];
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  drawHeader(BRAND);
  u8g2.setFont(u8g2_font_ncenB10_tr);
  centreStr("FLAPPY BIRD", 30);
  u8g2.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof(buf), "BEST: %d", bestScore);
  centreStr(buf, 43);
  centreStr("CLICK: PLAY", 54);
  centreStr("HOLD 2s: EXIT", 63);
  u8g2.sendBuffer();
  return waitChoice(0);
}

Choice showGameOverScreen(bool newBest) {
  char buf[28];
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  drawHeader(newBest ? "NEW BEST!" : BRAND);
  u8g2.setFont(u8g2_font_ncenB10_tr);
  centreStr("GAME OVER", 30);
  u8g2.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof(buf), "SCORE:%d  BEST:%d", g.score, bestScore);
  centreStr(buf, 43);
  centreStr("CLICK: RETRY", 54);
  centreStr("HOLD 2s: EXIT", 63);
  u8g2.sendBuffer();
  gameOverSound();
  return waitChoice(LOCKOUT_MS);
}

}  // namespace FlappyBird

// ═════════════════════════════════════════════════════════
//  ENTRY POINT (called from the dashboard on a GPIO 0 double-click)
// ═════════════════════════════════════════════════════════
// Returns to the caller (the dashboard) when the player quits.
void game_flappy() {
  using namespace FlappyBird;

  prefs.begin("flappy", false);
  bestScore = prefs.getInt("best", 0);
  soundClear();

  Choice c = showStartScreen();
  while (c == CHOICE_PLAY) {
    RoundResult r = playRound();
    if (r == ROUND_QUIT) break;
    c = showGameOverScreen(r == ROUND_NEW_BEST);
  }

  prefs.end();
  soundClear();
  u8g2.setDrawColor(1);
  waitRelease();   // don't let a still-held button leak into the dashboard
}

#endif