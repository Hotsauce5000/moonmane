// MoonmaneMusic board code: setup() and loop() live here.
// Kept in a .cpp file so the Arduino IDE does not rewrite it (see MoonmaneMusic.ino).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_random.h>
#include "audio.h"
#include "game.h"
#include "sprites.h"

// ---------------------------------------------------------------- settings
#define SCREEN_TIMEOUT_MS 30000   // screen off after this long without input
#define CALL_SCREEN_MS 15000      // how long a call keeps the screen on
#define BRIGHTNESS 200            // 0-255
#define VOLUME 70                 // 0-100, starting volume (the player's VOL buttons change it)
#define USE_LIGHT_SLEEP 1         // sleep between ticks while the screen is off (big battery saving)
#define MAX_CATCHUP_S (2 * 3600)  // time that passes while powered off; anything longer is a pause
#define BOOT_TEST 0               // 1 = flash red, green, blue at startup and log each step to Serial
#define PWR_SLEEP_MS 3000         // hold PWR this long to turn the screen off
#define PWR_OFF_MS 5000           // hold PWR this long to save and power off

// ---------------------------------------------------------------- pins (Waveshare V2 pin_config.h)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_W 368
#define LCD_H 448
#define I2C_SDA 15
#define I2C_SCL 14
#define TP_INT 21
#define BOOT_PIN 0
#define PA_EN 46  // speaker amp (audio.cpp drives it; powerOff() turns it off)

#define ADDR_EXPANDER 0x20  // TCA9554: P0 LCD reset, P1 display power, P2 touch reset, P4 PWR key, P7 SD CS
#define ADDR_TOUCH 0x15
#define ADDR_RTC 0x51       // PCF85063
#define ADDR_PMU 0x34       // AXP2101

using namespace sm;

// ---------------------------------------------------------------- display
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H, 16, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_W, LCD_H, panel, 0, 0, 0);

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static const uint16_t C_COAT = rgb(0x3b, 0x1a, 0x0e);   // bay chestnut shell
static const uint16_t C_COAT_HI = rgb(0x5a, 0x2a, 0x15);
static const uint16_t C_BEZEL = rgb(0x22, 0x17, 0x11);
static const uint16_t C_BRASS = rgb(0xd2, 0xa8, 0x45);
static const uint16_t C_LCD = rgb(0xb6, 0xc2, 0x9d);
static const uint16_t C_INK = rgb(0x1e, 0x26, 0x17);
static const uint16_t C_GHOST = rgb(0xab, 0xb7, 0x94);  // unlit dot: ink at 7%
static const uint16_t C_ICON_OFF = rgb(0x9f, 0xab, 0x89);  // unlit icon: ink at 15%
static const uint16_t C_CREAM = rgb(0xf3, 0xe9, 0xd4);
static const uint16_t C_BTN_TEXT = rgb(0x7d, 0x67, 0x45);

// layout (all in screen pixels). A 52 px top bar holds the name and the horseshoe music button.
static const int TOP_BAR = 52;
static const int SHOE_X = 320, SHOE_Y = 28, SHOE_R = 21;
static const int PANEL_Y = 48;  // brass rim top
static const int DOT_X0 = 24, DOT_Y0 = 112, DOT_PITCH = 10, DOT_SIZE = 8;
static const int ICON_PX = 4, ICON_TOP_CY = 84, ICON_BOT_CY = 338;
static const int BTN_Y = 410, BTN_R = 28;
static const int BTN_X[3] = {94, 184, 274};
static const int TOUCH_BTN_TOP = 378;

// ---------------------------------------------------------------- globals
Game game;
Preferences prefs;
uint8_t fb[W * H];
bool screenOn = true, muted = false;
uint32_t lastInputMs = 0, screenHoldUntil = 0, lastSaveMs = 0, lastRtcMs = 0, toastUntil = 0;
const char *toast = nullptr;
enum View : uint8_t { V_GAME, V_PLAYER, V_LIST };
View view = V_GAME;
int volume = VOLUME, listTop = 0, savedTrack = -1;
uint32_t lastUiDraw = 0;
int8_t pressedBtn = -1;
uint32_t pressedUntil = 0;
volatile bool touchIrq = false;

// ---------------------------------------------------------------- I2C helpers
static bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// ---------------------------------------------------------------- IO expander
static bool expanderInit() {
  bool ok = i2cWrite8(ADDR_EXPANDER, 0x03, 0x78);  // P0,P1,P2,P7 outputs; the rest inputs
  ok &= i2cWrite8(ADDR_EXPANDER, 0x01, 0x80);      // hold display and touch in reset
  delay(20);
  ok &= i2cWrite8(ADDR_EXPANDER, 0x01, 0x87);      // release resets, display power on
  delay(120);
  return ok;
}
static bool pwrKeyDown() {
  uint8_t v = 0;
  if (!i2cRead(ADDR_EXPANDER, 0x00, &v, 1)) return false;
  return v & 0x10;  // P4 reads high while PWR is held
}
static bool usbPowered() {
  uint8_t v = 0;
  if (!i2cRead(ADDR_PMU, 0x00, &v, 1)) return false;
  return v & 0x20;  // AXP2101 status 1, bit 5: VBUS good
}

// ---------------------------------------------------------------- RTC (local time, no time zones)
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
static void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}
static uint8_t bcd2(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }
static uint8_t tobcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static bool rtcRead(int64_t &t) {
  uint8_t r[7];
  if (!i2cRead(ADDR_RTC, 0x04, r, 7)) return false;
  if (r[0] & 0x80) return false;  // oscillator stopped: time is not trustworthy
  int sec = bcd2(r[0] & 0x7f), mn = bcd2(r[1] & 0x7f), hr = bcd2(r[2] & 0x3f);
  int day = bcd2(r[3] & 0x3f), mon = bcd2(r[5] & 0x1f), yr = 2000 + bcd2(r[6]);
  if (yr < 2025 || mon < 1 || mon > 12 || day < 1) return false;
  t = daysFromCivil(yr, mon, day) * 86400 + hr * 3600 + mn * 60 + sec;
  return true;
}
static void rtcWrite(int64_t t) {
  int y; unsigned m, d;
  int64_t days = t / 86400, s = t % 86400;
  civilFromDays(days, y, m, d);
  uint8_t wd = (uint8_t)((days + 4) % 7);  // 1970-01-01 was a Thursday
  Wire.beginTransmission(ADDR_RTC);
  Wire.write(0x04);
  Wire.write(tobcd(s % 60));  // also clears the oscillator-stop flag
  Wire.write(tobcd((s / 60) % 60));
  Wire.write(tobcd(s / 3600));
  Wire.write(tobcd(d));
  Wire.write(wd);
  Wire.write(tobcd(m));
  Wire.write(tobcd(y - 2000));
  Wire.endTransmission();
}
static int64_t compileTime() {
  static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int d, y, hh, mm, ss;
  sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  int m = (int)((strstr(MONTHS, mon) - MONTHS) / 3) + 1;
  return daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}
static int64_t nowLocal() {
  int64_t t;
  if (rtcRead(t)) return t;
  t = compileTime();
  rtcWrite(t);
  return t;
}

// ---------------------------------------------------------------- touch (CST816 family)
void IRAM_ATTR onTouchIrq() { touchIrq = true; }
static void touchInit() {
  i2cWrite8(ADDR_TOUCH, 0xFA, 0x40);  // interrupt: periodic pulses while touched (as Waveshare's demo)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(TP_INT, onTouchIrq, FALLING);
}
static bool touchRead(int &x, int &y) {
  uint8_t r[5];
  if (!i2cRead(ADDR_TOUCH, 0x02, r, 5)) return false;
  if ((r[0] & 0x0f) == 0) return false;
  x = ((r[1] & 0x0f) << 8) | r[2];
  y = ((r[3] & 0x0f) << 8) | r[4];
  return true;
}

// ---------------------------------------------------------------- sound
static void sound(Sfx s) {
  if (!muted && s != SFX_NONE) audio::beep(s);
}

// ---------------------------------------------------------------- drawing
static void drawText(const char *s, int x, int y, int scale, uint16_t c) {
  for (; *s; s++, x += 4 * scale) {
    const char *g = glyph15(*s);
    if (!g) continue;
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 3; i++)
        if (g[j * 3 + i] == '#') gfx->fillRect(x + i * scale, y + j * scale, scale, scale, c);
  }
}
static int textWidth(const char *s, int scale) { return ((int)strlen(s) * 4 - 1) * scale; }

// File names can hold any characters, so they use the library's built-in font (6x8 per size step).
static void drawName(const char *s, int x, int y, int size, uint16_t c, int maxChars) {
  char buf[64];
  int n = 0;
  for (; *s && n < maxChars && n < 63; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch >= 0x80) { if ((ch & 0xC0) == 0x80) continue; ch = '?'; }  // one '?' per non-ASCII character
    buf[n++] = (char)ch;
  }
  if (*s && n >= 3) { buf[n - 1] = '.'; buf[n - 2] = '.'; buf[n - 3] = '.'; }
  buf[n] = 0;
  gfx->setTextSize(size);
  gfx->setTextColor(c);
  gfx->setCursor(x, y);
  gfx->print(buf);
}
// Word-wraps into up to `lines` lines of `cols` characters.
static void drawWrapped(const char *s, int x, int y, int size, int lineH, uint16_t c, int cols, int lines) {
  int len = (int)strlen(s), start = 0;
  for (int l = 0; l < lines && start < len; l++) {
    int end = start + cols;
    if (end >= len) end = len;
    else if (l < lines - 1) {
      int sp = end;
      while (sp > start && s[sp] != ' ' && s[sp] != '/' && s[sp] != '-' && s[sp] != '_') sp--;
      if (sp > start + cols / 2) end = s[sp] == ' ' ? sp : sp + 1;  // break at the space, or just after / - _
    }
    char line[64];
    int n = end - start < 63 ? end - start : 63;
    memcpy(line, s + start, n);
    line[n] = 0;
    bool last = l == lines - 1 && end < len;
    drawName(line, x, y + l * lineH, size, c, last ? cols - 1 : cols);
    if (last) drawName("...", x + (cols - 3) * 6 * size, y + l * lineH, size, c, 3);
    start = end;
    while (start < len && s[start] == ' ') start++;
  }
}

static void drawIcon(int i, bool on) {
  int cx = DOT_X0 + 80 * (i % 4) + 40, cy = i < 4 ? ICON_TOP_CY : ICON_BOT_CY;
  int x0 = cx - ICON_W * ICON_PX / 2, y0 = cy - ICON_H * ICON_PX / 2;
  uint16_t c = on ? C_INK : C_ICON_OFF;
  for (int j = 0; j < ICON_H; j++)
    for (int k = 0; k < ICON_W; k++)
      if (ICON_R[i][j][k] == '#') gfx->fillRect(x0 + k * ICON_PX, y0 + j * ICON_PX, ICON_PX, ICON_PX, c);
}
static void drawPixelIcon(int i, int cx, int cy, int px, uint16_t c) {
  int x0 = cx - ICON_W * px / 2, y0 = cy - ICON_H * px / 2;
  for (int j = 0; j < ICON_H; j++)
    for (int k = 0; k < ICON_W; k++)
      if (ICON_R[i][j][k] == '#') gfx->fillRect(x0 + k * px, y0 + j * px, px, px, c);
}

static void drawTopBar(uint32_t now) {
  audio::Status st = audio::status();
  bool playing = st.loaded && !st.paused;
  bool calling = game.attention() && view != V_GAME;
  const char *label = (toast && now < toastUntil) ? toast : calling ? "HORSE NEEDS YOU" : "MOONMANE";
  drawText(label, 26, 22, 2, calling && !(toast && now < toastUntil) ? C_BRASS : C_CREAM);
  if (calling && (now / 500) % 2) drawPixelIcon(7, 262, SHOE_Y, 2, C_BRASS);
  // The horseshoe: opens the music player. Brass while music plays, cream while the player is open.
  uint16_t fill = view != V_GAME ? C_CREAM : playing ? C_BRASS : C_BEZEL;
  gfx->fillCircle(SHOE_X, SHOE_Y, SHOE_R + 2, C_BRASS);
  gfx->fillCircle(SHOE_X, SHOE_Y, SHOE_R, fill);
  drawPixelIcon(2, SHOE_X, SHOE_Y, 2, fill == C_BEZEL ? C_BRASS : C_BEZEL);
}

static void drawGameView(uint32_t now) {
  // screen: brass rim, dark bezel, LCD glass
  gfx->fillRoundRect(6, PANEL_Y, 356, 326, 22, C_BRASS);
  gfx->fillRoundRect(10, PANEL_Y + 4, 348, 318, 18, C_BEZEL);
  gfx->fillRoundRect(18, PANEL_Y + 12, 332, 302, 12, C_LCD);
  game.render(fb);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      gfx->fillRect(DOT_X0 + x * DOT_PITCH, DOT_Y0 + y * DOT_PITCH, DOT_SIZE, DOT_SIZE, fb[y * W + x] ? C_INK : C_GHOST);
  int sel = game.selectedIcon();
  for (int i = 0; i < 8; i++) drawIcon(i, i == sel || (i == 7 && game.attention()));
  static const char *L[3] = {"A", "B", "C"};
  for (int i = 0; i < 3; i++) {
    bool down = pressedBtn == i && now < pressedUntil;
    gfx->fillCircle(BTN_X[i], BTN_Y + 4, BTN_R, C_BEZEL);  // shadow
    gfx->fillCircle(BTN_X[i], BTN_Y + (down ? 3 : 0), BTN_R, down ? C_BRASS : C_CREAM);
    drawText(L[i], BTN_X[i] - 6, BTN_Y - 10 + (down ? 3 : 0), 4, C_BTN_TEXT);
  }
}

// player geometry, shared by drawing and touch
static const int P_ROW = 278, P_PREV_X = 96, P_PLAY_X = 184, P_NEXT_X = 272, P_SIDE_R = 28, P_PLAY_R = 36;
static const int V_ROW = 344, V_MINUS_X = 50, V_PLUS_X = 174, V_R = 22;
static const int LIST_Y0 = 100, LIST_ROW = 34, LIST_ROWS = 7, LIST_BTN_Y = 372;

static void pill(int x, int y, int w, int h, uint16_t bg, const char *label, uint16_t fg) {
  gfx->fillRoundRect(x, y, w, h, h / 2, bg);
  drawText(label, x + (w - textWidth(label, 2)) / 2, y + (h - 10) / 2, 2, fg);
}

static void drawPlayerView(uint32_t now) {
  audio::Status st = audio::status();
  int n = audio::trackCount();
  drawText("MUSIC", 26, 68, 3, C_BRASS);
  if (n > 0) {
    char c[16];
    snprintf(c, sizeof c, "%d/%d", st.index + 1, n);
    drawText(c, 344 - textWidth(c, 2), 72, 2, C_CREAM);
  }
  gfx->fillRoundRect(16, 98, 336, 110, 14, C_BEZEL);
  if (n == 0) {
    const char *head = "NO SD CARD", *hint = "Insert a FAT32 micro SD card, then tap here.";
    if (audio::lastScan() == audio::SCAN_NO_FOLDER) { head = "NO MUSIC FOLDER"; hint = "Make a folder named Music on the card, then tap here."; }
    else if (audio::lastScan() == audio::SCAN_EMPTY) { head = "NO SONGS FOUND"; hint = "Put MP3 or WAV files in the Music folder, then tap here."; }
    drawText(head, 30, 114, 2, C_BRASS);
    drawWrapped(hint, 30, 140, 2, 22, C_CREAM, 25, 3);
    return;
  }
  drawWrapped(audio::trackName(st.index), 30, 112, 2, 22, C_CREAM, 25, 3);
  char line[40];
  if (*st.error && !st.loaded) snprintf(line, sizeof line, "%s", st.error);
  else {
    const char *state = !st.loaded ? "STOPPED" : st.paused ? "PAUSED" : "PLAYING";
    if (st.loaded) snprintf(line, sizeof line, "%s  %s %d.%dKHZ", state, st.mp3 ? "MP3" : "WAV", st.rate / 1000, (st.rate % 1000) / 100);
    else snprintf(line, sizeof line, "%s", state);
  }
  drawText(line, 30, 186, 2, C_BRASS);

  // progress
  gfx->fillRoundRect(24, 222, 320, 8, 4, C_BEZEL);
  if (st.loaded && st.size) {
    int w = (int)((uint64_t)320 * st.pos / st.size);
    if (w > 8) gfx->fillRoundRect(24, 222, w, 8, 4, C_BRASS);
  }

  // transport
  gfx->fillCircle(P_PREV_X, P_ROW, P_SIDE_R, C_CREAM);
  gfx->fillRect(P_PREV_X - 12, P_ROW - 11, 4, 22, C_BEZEL);
  gfx->fillTriangle(P_PREV_X + 12, P_ROW - 11, P_PREV_X + 12, P_ROW + 11, P_PREV_X - 7, P_ROW, C_BEZEL);
  gfx->fillCircle(P_NEXT_X, P_ROW, P_SIDE_R, C_CREAM);
  gfx->fillRect(P_NEXT_X + 8, P_ROW - 11, 4, 22, C_BEZEL);
  gfx->fillTriangle(P_NEXT_X - 12, P_ROW - 11, P_NEXT_X - 12, P_ROW + 11, P_NEXT_X + 7, P_ROW, C_BEZEL);
  gfx->fillCircle(P_PLAY_X, P_ROW, P_PLAY_R, C_BRASS);
  if (st.loaded && !st.paused) {
    gfx->fillRect(P_PLAY_X - 12, P_ROW - 14, 9, 28, C_BEZEL);
    gfx->fillRect(P_PLAY_X + 3, P_ROW - 14, 9, 28, C_BEZEL);
  } else {
    gfx->fillTriangle(P_PLAY_X - 10, P_ROW - 16, P_PLAY_X - 10, P_ROW + 16, P_PLAY_X + 16, P_ROW, C_BEZEL);
  }

  // volume and list
  gfx->fillCircle(V_MINUS_X, V_ROW, V_R, C_CREAM);
  gfx->fillRect(V_MINUS_X - 9, V_ROW - 2, 18, 4, C_BEZEL);
  gfx->fillCircle(V_PLUS_X, V_ROW, V_R, C_CREAM);
  gfx->fillRect(V_PLUS_X - 9, V_ROW - 2, 18, 4, C_BEZEL);
  gfx->fillRect(V_PLUS_X - 2, V_ROW - 9, 4, 18, C_BEZEL);
  char v[12];
  snprintf(v, sizeof v, "VOL %d", volume);
  drawText(v, (V_MINUS_X + V_PLUS_X) / 2 - textWidth(v, 2) / 2, V_ROW - 5, 2, C_CREAM);
  pill(216, V_ROW - 22, 136, 44, C_CREAM, "SONGS", C_BTN_TEXT);

  const char *hint = "BOOT NEXT  PWR PLAY  HOLD BOOT BACK";
  drawText(hint, (LCD_W - textWidth(hint, 2)) / 2, 410, 2, C_BTN_TEXT);
  (void)now;
}

static void drawListView(uint32_t now) {
  audio::Status st = audio::status();
  int n = audio::trackCount();
  drawText("SONGS", 26, 68, 3, C_BRASS);
  char c[24];
  int last = listTop + LIST_ROWS < n ? listTop + LIST_ROWS : n;
  snprintf(c, sizeof c, "%d-%d/%d", n ? listTop + 1 : 0, last, n);
  drawText(c, 344 - textWidth(c, 2), 72, 2, C_CREAM);
  for (int r = 0; r < LIST_ROWS; r++) {
    int i = listTop + r;
    if (i >= n) break;
    int y = LIST_Y0 + r * LIST_ROW;
    bool cur = i == st.index && st.loaded;
    gfx->fillRoundRect(16, y, 336, LIST_ROW - 4, 8, cur ? C_BRASS : C_BEZEL);
    drawName(audio::trackName(i), 28, y + 7, 2, cur ? C_BEZEL : C_CREAM, 26);
  }
  pill(16, LIST_BTN_Y - 22, 104, 44, listTop > 0 ? C_CREAM : C_BEZEL, "UP", C_BTN_TEXT);
  pill(132, LIST_BTN_Y - 22, 104, 44, listTop + LIST_ROWS < n ? C_CREAM : C_BEZEL, "DOWN", C_BTN_TEXT);
  pill(248, LIST_BTN_Y - 22, 104, 44, C_CREAM, "BACK", C_BTN_TEXT);
  (void)now;
}

static void drawFrame() {
  uint32_t now = millis();
  gfx->fillScreen(C_COAT);
  gfx->fillCircle(70, 40, 120, C_COAT_HI);  // soft sheen across the shell
  gfx->fillCircle(70, 40, 95, C_COAT);
  if (view == V_GAME) drawGameView(now);
  else if (view == V_PLAYER) drawPlayerView(now);
  else drawListView(now);
  drawTopBar(now);
  gfx->flush();
  lastUiDraw = now;
}

// ---------------------------------------------------------------- screen power
static void screenWake() {
  lastInputMs = millis();
  if (screenOn) return;
  panel->displayOn();
  panel->setBrightness(BRIGHTNESS);
  screenOn = true;
  game.update(millis());
  drawFrame();
}
static void screenSleep() {
  if (!screenOn) return;
  panel->setBrightness(0);
  panel->displayOff();
  screenOn = false;
}

// ---------------------------------------------------------------- saving
static void saveGame() {
  prefs.putBytes("state", &game.state(), sizeof(State));
  lastSaveMs = millis();
}

// ---------------------------------------------------------------- music player controls
static void setVolume(int v) {
  volume = v < 0 ? 0 : v > 100 ? 100 : v;
  audio::setVolume(volume);
  prefs.putInt("vol", volume);
}
static void openList() {
  int n = audio::trackCount();
  int idx = audio::status().index;
  listTop = n ? (idx / LIST_ROWS) * LIST_ROWS : 0;
  view = V_LIST;
}
static void togglePlayer() {
  if (view == V_GAME) {
    if (audio::trackCount() == 0) audio::scan();
    view = V_PLAYER;
  } else {
    view = V_GAME;
  }
  sound(SFX_CLICK);
}
static bool inCircle(int x, int y, int cx, int cy, int r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

static void playerTap(int x, int y) {
  int n = audio::trackCount();
  if (view == V_PLAYER) {
    if (n == 0) { audio::scan(); sound(SFX_CLICK); return; }  // tap anywhere to look again
    if (inCircle(x, y, P_PREV_X, P_ROW, P_SIDE_R + 12)) audio::prev();
    else if (inCircle(x, y, P_PLAY_X, P_ROW, P_PLAY_R + 8)) audio::toggle();
    else if (inCircle(x, y, P_NEXT_X, P_ROW, P_SIDE_R + 12)) audio::next();
    else if (inCircle(x, y, V_MINUS_X, V_ROW, V_R + 10)) setVolume(volume - 5);
    else if (inCircle(x, y, V_PLUS_X, V_ROW, V_R + 10)) setVolume(volume + 5);
    else if ((x >= 210 && y >= V_ROW - 30 && y <= V_ROW + 30) || (y >= 98 && y <= 208)) openList();
    else return;
  } else {
    if (y >= LIST_Y0 && y < LIST_Y0 + LIST_ROWS * LIST_ROW) {
      int i = listTop + (y - LIST_Y0) / LIST_ROW;
      if (i >= n) return;
      audio::play(i);
      view = V_PLAYER;
    } else if (y >= LIST_BTN_Y - 30 && y <= LIST_BTN_Y + 30) {
      if (x < 126) { if (listTop > 0) listTop -= LIST_ROWS; }
      else if (x < 242) { if (listTop + LIST_ROWS < n) listTop += LIST_ROWS; }
      else view = V_PLAYER;
    } else return;
  }
  sound(SFX_CLICK);
}

// BOOT = A, PWR = B, BOOT held = C. In the player they skip, play/pause and go back.
static void playerButton(Btn b) {
  int n = audio::trackCount();
  if (b == BTN_C) { view = view == V_LIST ? V_PLAYER : V_GAME; sound(SFX_CLICK); return; }
  if (n == 0) { audio::scan(); return; }
  if (b == BTN_B) audio::toggle();
  else if (view == V_LIST) listTop = listTop + LIST_ROWS < n ? listTop + LIST_ROWS : 0;
  else audio::next();
}

// ---------------------------------------------------------------- input
static void buttonPressed(Btn b) {
  uint32_t now = millis();
  if (!screenOn) { screenWake(); return; }  // the first press only wakes the screen
  lastInputMs = now;
  if (view != V_GAME) { playerButton(b); lastUiDraw = 0; return; }
  pressedBtn = b; pressedUntil = now + 140;
  game.press(b, now);
}
static void showToast(const char *t) { toast = t; toastUntil = millis() + 2000; }

static bool touching = false, touchConsumed = false, touchWoke = false;
static int8_t touchBtn = -1;
static uint32_t touchStart = 0, touchLast = 0;
static void pollTouch() {
  uint32_t now = millis();
  if (touchIrq) {
    touchIrq = false;
    int x, y;
    if (touchRead(x, y)) {
      touchLast = now;
      if (!touching) {
        touching = true; touchStart = now; touchConsumed = false; touchBtn = -1;
        if (!screenOn) { screenWake(); touchWoke = true; touchConsumed = true; }
        else if (y < TOP_BAR + 4 && x >= SHOE_X - 44) { lastInputMs = now; togglePlayer(); lastUiDraw = 0; touchConsumed = true; }
        else if (view != V_GAME) { lastInputMs = now; playerTap(x, y); lastUiDraw = 0; touchConsumed = true; }
        else {
          touchBtn = y >= TOUCH_BTN_TOP ? (x < 139 ? 0 : x < 229 ? 1 : 2) : -1;
          if (touchBtn == 0 || touchBtn == 1) { buttonPressed((Btn)touchBtn); touchConsumed = true; }
          else lastInputMs = now;
        }
      }
    }
  }
  if (touching && touchBtn == 2 && !touchConsumed && now - touchStart >= 2000) {
    muted = !muted;
    prefs.putBool("muted", muted);
    showToast(muted ? "SOUND OFF" : "SOUND ON");
    if (!muted) sound(SFX_GOOD);
    touchConsumed = true;
    lastInputMs = now;
  }
  if (touching && now - touchLast > 180) {  // no more pulses: finger lifted
    if (touchBtn == 2 && !touchConsumed) buttonPressed(BTN_C);
    touching = false; touchWoke = false;
  }
}

static void powerOff() {
  saveGame();
  prefs.end();
  digitalWrite(PA_EN, LOW);
  panel->setBrightness(0);
  panel->displayOff();
  delay(50);
  uint8_t v = 0;
  if (i2cRead(ADDR_PMU, 0x10, &v, 1)) i2cWrite8(ADDR_PMU, 0x10, v | 0x01);  // AXP2101 soft power-off
  for (;;) delay(1000);
}

static bool bootDown = false, bootLongSent = false, pwrDown = false;
static uint32_t bootStart = 0, lastPwrPoll = 0, pwrStart = 0;
static uint8_t pwrStage = 0;  // 0 = short press so far, 1 = screen turned off, 2 = powering off
static void pollButtons() {
  uint32_t now = millis();
  bool b = digitalRead(BOOT_PIN) == LOW;
  if (b && !bootDown) { bootDown = true; bootStart = now; bootLongSent = false; }
  if (b && bootDown && !bootLongSent && now - bootStart >= 600) { bootLongSent = true; buttonPressed(BTN_C); }
  if (!b && bootDown) {
    bootDown = false;
    if (!bootLongSent && now - bootStart >= 30) buttonPressed(BTN_A);
  }
  if (now - lastPwrPoll >= 30) {
    lastPwrPoll = now;
    bool p = pwrKeyDown();
    if (p && !pwrDown) { pwrStart = now; pwrStage = 0; }
    if (p && pwrDown) {
      uint32_t held = now - pwrStart;
      if (pwrStage < 1 && held >= PWR_SLEEP_MS) { pwrStage = 1; screenSleep(); }
      if (pwrStage < 2 && held >= PWR_OFF_MS) { pwrStage = 2; powerOff(); }
    }
    if (!p && pwrDown && pwrStage == 0 && now - pwrStart < 1000) buttonPressed(BTN_B);  // B on release
    pwrDown = p;
  }
}

// ---------------------------------------------------------------- light sleep
static void lightSleepTick() {
#if USE_LIGHT_SLEEP
  if (audio::busy() || touching || bootDown || pwrDown) { delay(20); return; }
  if (usbPowered()) { delay(50); return; }  // stay awake on USB so uploads and serial keep working
  gpio_wakeup_enable((gpio_num_t)BOOT_PIN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(1000000);
  esp_light_sleep_start();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  gpio_wakeup_disable((gpio_num_t)BOOT_PIN);
  gpio_wakeup_disable((gpio_num_t)TP_INT);
  gpio_set_intr_type((gpio_num_t)TP_INT, GPIO_INTR_NEGEDGE);  // restore the touch interrupt
  gpio_intr_enable((gpio_num_t)TP_INT);
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    if (digitalRead(TP_INT) == LOW) touchIrq = true;  // let pollTouch see the finger and wake the screen
  }
#else
  delay(20);
#endif
}

// ---------------------------------------------------------------- setup / loop
static volatile const char *bootStep = "starting";
static void bootLog(const char *msg) {
  bootStep = msg;
#if BOOT_TEST
  Serial.printf("[moonmane %6lu ms] %s\n", (unsigned long)millis(), msg);
#endif
}
#if BOOT_TEST
// Repeats the last startup step every 2 s, so the log is visible even if the
// Serial Monitor connects late, and shows where startup stopped if it hangs.
static void heartbeatTask(void *) {
  for (;;) {
    Serial.printf("[moonmane] up %lus, last step: %s, screen %s\n", (unsigned long)(millis() / 1000),
                  (const char *)bootStep, screenOn ? "on" : "off");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
#endif

void setup() {
  Serial.begin(115200);
#if BOOT_TEST
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 2000;) delay(10);  // give Serial Monitor a moment
#endif
  bootLog("boot");
#if BOOT_TEST
  xTaskCreatePinnedToCore(heartbeatTask, "beat", 3072, nullptr, 1, nullptr, 0);
#endif
  Serial.printf("PSRAM size: %u bytes, free heap: %u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
  setCpuFrequencyMhz(160);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  static const uint8_t PROBE[] = {ADDR_EXPANDER, ADDR_TOUCH, ADDR_RTC, ADDR_PMU, 0x18 /* ES8311 */};
  for (uint8_t a : PROBE) {
    Wire.beginTransmission(a);
    Serial.printf("I2C 0x%02X: %s\n", a, Wire.endTransmission() == 0 ? "found" : "MISSING");
  }
  uint8_t pk = 0;  // AXP2101 0x27 bits 3:2 = hardware long-press power-off time; set 10 s
  if (i2cRead(ADDR_PMU, 0x27, &pk, 1)) i2cWrite8(ADDR_PMU, 0x27, (pk & 0xF3) | (3 << 2));
  bootLog(expanderInit() ? "IO expander set: display power on, resets released" : "IO expander did not answer");

  // Bring the panel up on its own first, so a framebuffer problem can still show something.
  if (!panel->begin()) {
    bootLog("display init FAILED (QSPI)");
    for (;;) delay(1000);
  }
  panel->displayOn();
  panel->setBrightness(BRIGHTNESS);
  bootLog("display init ok");
#if BOOT_TEST
  panel->fillScreen(RGB565_RED);   delay(400);
  panel->fillScreen(RGB565_GREEN); delay(400);
  panel->fillScreen(RGB565_BLUE);  delay(400);
  bootLog("color test done");
#endif
  if (!gfx->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    // Screen buffer needs PSRAM. Show solid red so the problem is visible without a computer.
    bootLog("framebuffer allocation FAILED: set Tools > PSRAM > OPI PSRAM and upload again");
    panel->fillScreen(RGB565_RED);
    for (;;) delay(1000);
  }
  bootLog("framebuffer ok");
  touchInit();
  bootLog("touch ready");
  prefs.begin("stablemate", false);  // same save as the plain version, so your horse carries over
  muted = prefs.getBool("muted", false);
  volume = prefs.getInt("vol", VOLUME);
  savedTrack = prefs.getInt("track", 0);
  bootLog(audio::init(volume) ? "sound ready" : "sound init failed; running without sound");
  audio::setStart(savedTrack);
  game.seed(esp_random());
  int64_t now = nowLocal();
  State saved;
  bool loaded = prefs.getBytes("state", &saved, sizeof saved) == sizeof saved && game.load(saved);
  if (loaded) {
    int64_t away = now - saved.t;
    if (away > MAX_CATCHUP_S) game.clockChanged(now - MAX_CATCHUP_S);  // treat the rest as a pause
    game.advanceTo(now, true);
  } else {
    game.reset(now);
    saveGame();
  }
  lastInputMs = millis();
  lastSaveMs = millis();
  bootLog("game loaded, drawing first frame");
  drawFrame();
  bootLog("running");
}

void loop() {
  uint32_t ms = millis();

  pollTouch();
  pollButtons();

  if (ms - lastRtcMs >= 250 || !screenOn) {
    lastRtcMs = ms;
    int64_t t;
    if (rtcRead(t)) game.advanceTo(t, false);
  }
  game.update(ms);

  Sfx s;
  while ((s = game.popSfx()) != SFX_NONE) sound(s);
  if (game.takeCallStarted()) { screenWake(); screenHoldUntil = millis() + CALL_SCREEN_MS; }

  int h, m;
  if (game.takeClockRequest(h, m)) {
    int64_t t = nowLocal();
    int64_t newT = (t / 86400) * 86400 + h * 3600 + m * 60;
    rtcWrite(newT);
    game.clockChanged(newT);
    saveGame();
  }
  if (game.takeSaveRequest() || millis() - lastSaveMs > 10UL * 60 * 1000) saveGame();

  audio::Status st = audio::status();
  if (st.loaded && st.index != savedTrack) { savedTrack = st.index; prefs.putInt("track", savedTrack); }

  if (screenOn) {
    bool redraw = view == V_GAME
                      ? (game.dirty() || millis() < pressedUntil + 40)
                      : (millis() - lastUiDraw >= 250);  // player: progress bar and call badge
    if (redraw || (toast && millis() < toastUntil + 40) || lastUiDraw == 0) {
      game.clean();
      drawFrame();
    }
    bool idle = millis() - lastInputMs > SCREEN_TIMEOUT_MS && millis() > screenHoldUntil && !game.busy() && !touching;
    if (idle) screenSleep();
    delay(10);
  } else {
    lightSleepTick();
  }
}
