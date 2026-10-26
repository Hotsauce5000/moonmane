#include <Arduino.h>
#include <ESP_I2S.h>
#include <FS.h>
#include <SD_MMC.h>
#include <vector>
#include <string>
#include <algorithm>
#include "audio.h"
#include "decoder.h"
#include "es8311.h"

// Waveshare ESP32-S3-Touch-AMOLED-1.8 audio and SD card pins
#define I2S_MCK 16
#define I2S_BCK 9
#define I2S_WS 45
#define I2S_DOUT 8
#define I2S_DIN 10
#define PA_EN 46
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0 3

namespace audio {
using namespace sm;

// ------------------------------------------------------------------ shared state
static I2SClass i2s;
static es8311_handle_t es = nullptr;
static QueueHandle_t q = nullptr;
static volatile bool busyFlag = false;
static int curRate = 16000;

enum CmdType : uint8_t { C_BEEP, C_PLAY, C_TOGGLE, C_NEXT, C_PREV, C_VOL };
struct Cmd { uint8_t t; int16_t a; };
static void send(uint8_t t, int a = 0) {
  if (!q) return;
  Cmd c{t, (int16_t)a};
  xQueueSend(q, &c, 0);
}

// ------------------------------------------------------------------ library
static std::vector<std::pair<std::string, std::string>> tracks;  // display name, full path
static ScanResult scanRes = SCAN_NO_CARD;
static bool mounted = false;

static bool audioExt(const std::string &n, bool &mp3) {
  size_t dot = n.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string e = n.substr(dot + 1);
  for (auto &c : e) c = (char)tolower((unsigned char)c);
  if (e == "mp3") { mp3 = true; return true; }
  if (e == "wav") { mp3 = false; return true; }
  return false;
}
static void scanDir(const char *dir, const std::string &prefix, int depth) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File e = d.openNextFile(); e && tracks.size() < 500; e = d.openNextFile()) {
    std::string nm = e.name();
    size_t slash = nm.find_last_of('/');
    if (slash != std::string::npos) nm = nm.substr(slash + 1);
    if (nm.empty() || nm[0] == '.') continue;  // hidden files, including macOS "._" copies
    if (e.isDirectory()) {
      if (depth < 1) { std::string p = e.path(); scanDir(p.c_str(), prefix + nm + "/", depth + 1); }
      continue;
    }
    bool mp3;
    if (!audioExt(nm, mp3)) continue;
    std::string shown = prefix + nm.substr(0, nm.find_last_of('.'));
    tracks.push_back({shown, std::string(e.path())});
  }
}

ScanResult scan() {
  if (status().loaded) return scanRes;  // never touch the card list while a song is reading from it
  if (!mounted) {
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    mounted = SD_MMC.begin("/sdcard", true);  // 1-bit mode
    if (!mounted) return scanRes = SCAN_NO_CARD;
  }
  tracks.clear();
  const char *dirs[] = {"/Music", "/music", "/MUSIC"};
  bool found = false;
  for (const char *d : dirs) {
    File f = SD_MMC.open(d);
    if (f && f.isDirectory()) { found = true; f.close(); scanDir(d, "", 0); break; }
  }
  if (!found) return scanRes = SCAN_NO_FOLDER;
  std::sort(tracks.begin(), tracks.end(), [](const std::pair<std::string, std::string> &a, const std::pair<std::string, std::string> &b) {
    return strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
  });
  return scanRes = tracks.empty() ? SCAN_EMPTY : SCAN_OK;
}
ScanResult lastScan() { return scanRes; }
int trackCount() { return (int)tracks.size(); }
const char *trackName(int i) { return (i >= 0 && i < (int)tracks.size()) ? tracks[i].first.c_str() : ""; }

// ------------------------------------------------------------------ codec and sample rate
static bool rateSupported(int hz) {
  static const int R[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  for (int r : R) if (r == hz) return true;
  return false;
}
static bool setRate(int hz) {
  if (hz == curRate) return true;
  if (!rateSupported(hz)) return false;
  i2s.end();
  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCK);
  if (!i2s.begin(I2S_MODE_STD, hz, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) return false;
  if (es) es8311_sample_frequency_config(es, hz * 256, hz);
  curRate = hz;
  return true;
}

// ------------------------------------------------------------------ beeps
struct Note { uint16_t hz, ms; };
static const Note N_CLICK[] = {{2200, 35}};
static const Note N_ERR[] = {{330, 80}, {0, 30}, {330, 80}};
static const Note N_CALL[] = {{1760, 70}, {0, 70}, {1760, 70}, {0, 70}, {1760, 70}};
static const Note N_GOOD[] = {{1320, 70}, {1760, 70}, {2640, 100}};
static const Note N_DIE[] = {{880, 200}, {660, 200}, {440, 400}};
static const Note N_WIN[] = {{1760, 80}};
static const Note N_LOSE[] = {{440, 120}};
static const Note *bNotes = nullptr;
static int bCount = 0, bIdx = 0, bLeft = 0, bPhase = 0;

static void startBeep(int s) {
  switch (s) {
    case SFX_CLICK: bNotes = N_CLICK; bCount = 1; break;
    case SFX_ERR: bNotes = N_ERR; bCount = 3; break;
    case SFX_CALL: bNotes = N_CALL; bCount = 5; break;
    case SFX_GOOD: bNotes = N_GOOD; bCount = 3; break;
    case SFX_DIE: bNotes = N_DIE; bCount = 3; break;
    case SFX_WIN: bNotes = N_WIN; bCount = 1; break;
    case SFX_LOSE: bNotes = N_LOSE; bCount = 1; break;
    default: return;
  }
  bIdx = 0; bPhase = 0;
  bLeft = curRate * bNotes[0].ms / 1000;
}
// Adds the beep over `frames` stereo frames. Music under it drops to a quarter while it sounds.
static void mixBeep(int16_t *buf, int frames, bool duck) {
  for (int i = 0; i < frames && bNotes; i++) {
    int hz = bNotes[bIdx].hz, v = 0;
    if (hz) { int half = curRate / (2 * hz); v = ((bPhase++ / half) & 1) ? 3500 : -3500; }
    for (int c = 0; c < 2; c++) {
      int s = (duck ? buf[2 * i + c] / 4 : buf[2 * i + c]) + v;
      buf[2 * i + c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    if (--bLeft <= 0) {
      if (++bIdx >= bCount) { bNotes = nullptr; break; }
      bLeft = curRate * bNotes[bIdx].ms / 1000;
      bPhase = 0;
    }
  }
}

// ------------------------------------------------------------------ music
struct SdReader : music::Reader {
  File f;
  size_t read(uint8_t *b, size_t n) override { return f.read(b, n); }
  bool seek(uint32_t p) override { return f.seek(p); }
  uint32_t position() override { return f.position(); }
  uint32_t size() override { return f.size(); }
};
static music::Decoder *dec = nullptr;
static SdReader rd;
static volatile bool loaded = false, paused = false, isMp3 = true;
static volatile int index = -1, lastIndex = 0;
static volatile uint32_t pos = 0, fsize = 0;
static const char *volatile err = "";
static bool firstFrame = false;
static int failStreak = 0;

static void closeTrack() {
  if (dec) dec->close();
  if (rd.f) rd.f.close();
  loaded = false;
}
static bool openTrack(int i) {
  closeTrack();
  int n = (int)tracks.size();
  if (!n) { err = "NO SONGS"; return false; }
  i = ((i % n) + n) % n;
  index = lastIndex = i;
  pos = 0; fsize = 0;
  bool mp3 = true;
  audioExt(tracks[i].second, mp3);
  rd.f = SD_MMC.open(tracks[i].second.c_str());
  if (!rd.f) { err = "CAN'T OPEN FILE"; return false; }
  if (!dec->open(&rd, mp3)) { err = dec->error(); rd.f.close(); return false; }
  if (!mp3 && !setRate(dec->rate())) { err = "SAMPLE RATE NOT SUPPORTED"; closeTrack(); return false; }
  isMp3 = mp3; fsize = rd.f.size(); err = "";
  loaded = true; paused = false; firstFrame = true;
  return true;
}
// Open track i, or the next one that works. Gives up after trying every song once.
static void playFrom(int i, int dir) {
  int n = (int)tracks.size();
  for (int k = 0; k < n; k++) {
    if (openTrack(i + k * dir)) return;  // failStreak resets once a frame actually decodes
  }
  closeTrack();
}

static void handle(const Cmd &c) {
  if (c.t != C_BEEP && c.t != C_VOL) failStreak = 0;  // a fresh request from the player
  switch (c.t) {
    case C_BEEP: startBeep(c.a); break;
    case C_PLAY: playFrom(c.a, 1); break;
    case C_TOGGLE:
      if (loaded) paused = !paused;
      else playFrom(lastIndex, 1);
      break;
    case C_NEXT: playFrom((loaded ? index : lastIndex) + 1, 1); break;
    case C_PREV: playFrom((loaded ? index : lastIndex) - 1, -1); break;
    case C_VOL: if (es) es8311_voice_volume_set(es, c.a, NULL); break;
  }
}

static void audioTask(void *) {
  static int16_t out[music::MAX_FRAMES * 2];
  for (;;) {
    bool active = (loaded && !paused) || bNotes;
    Cmd c;
    if (xQueueReceive(q, &c, active ? 0 : pdMS_TO_TICKS(1500)) == pdTRUE) {
      handle(c);
      while (xQueueReceive(q, &c, 0) == pdTRUE) handle(c);
      continue;
    }
    if (!active) { digitalWrite(PA_EN, LOW); busyFlag = false; continue; }
    busyFlag = true;
    digitalWrite(PA_EN, HIGH);

    int frames = 0;
    bool music = loaded && !paused;
    if (music) {
      frames = dec->decode(out);
      if (frames <= 0) {  // end of the song, or it couldn't be decoded
        if (firstFrame) { err = *dec->error() ? dec->error() : "CAN'T DECODE"; failStreak++; }
        if (failStreak >= (int)tracks.size()) { closeTrack(); continue; }
        int from = index + 1;
        playFrom(from, 1);
        continue;
      }
      if (firstFrame || dec->rate() != curRate) {
        if (!setRate(dec->rate())) { err = "SAMPLE RATE NOT SUPPORTED"; failStreak++; playFrom(index + 1, 1); continue; }
      }
      firstFrame = false; failStreak = 0;
      pos = rd.f.position();
    } else {
      frames = 256;
      memset(out, 0, frames * 4);
    }
    if (bNotes) mixBeep(out, frames, music);
    i2s.write((uint8_t *)out, frames * 4);
  }
}

// ------------------------------------------------------------------ public API
bool init(int volume) {
  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, LOW);
  i2s.setPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCK);
  if (!i2s.begin(I2S_MODE_STD, curRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) return false;
  es = es8311_create(0, ES8311_ADDRRES_0);
  if (!es) return false;
  const es8311_clock_config_t clk = {false, false, true, curRate * 256, curRate};
  if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
  es8311_voice_volume_set(es, volume, NULL);
  dec = new music::Decoder();
  q = xQueueCreate(16, sizeof(Cmd));
  // MP3 decoding keeps about 16 KB of scratch on the stack.
  xTaskCreatePinnedToCore(audioTask, "audio", 32768, nullptr, 3, nullptr, 0);
  return true;
}
void beep(Sfx s) { send(C_BEEP, s); }
bool busy() { return busyFlag; }
void setStart(int i) { if (!loaded) lastIndex = i < 0 ? 0 : i; }
void play(int i) { send(C_PLAY, i); }
void toggle() { send(C_TOGGLE); }
void next() { send(C_NEXT); }
void prev() { send(C_PREV); }
void setVolume(int v) { send(C_VOL, v < 0 ? 0 : v > 100 ? 100 : v); }
Status status() {
  Status s;
  s.loaded = loaded; s.paused = paused; s.index = loaded ? index : lastIndex; s.rate = curRate;
  s.pos = pos; s.size = fsize; s.mp3 = isMp3; s.error = err;
  return s;
}

}  // namespace audio
