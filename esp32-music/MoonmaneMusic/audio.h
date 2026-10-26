// Sound for MoonmaneMusic: the horse's beeps plus an MP3/WAV player reading /Music on the SD card.
// One background task owns the speaker. Beeps are mixed over the music, which ducks while they play.
#pragma once
#include <stdint.h>
#include "game.h"

namespace audio {

bool init(int volume);         // I2S, ES8311 codec, speaker amp, audio task
void beep(sm::Sfx s);
bool busy();                   // true while anything is audible (keeps the board out of light sleep)

// Music library. scan() mounts the card and lists /Music (and one level of subfolders).
enum ScanResult { SCAN_OK, SCAN_NO_CARD, SCAN_NO_FOLDER, SCAN_EMPTY };
ScanResult scan();
ScanResult lastScan();
int trackCount();
const char *trackName(int i);  // relative path without extension, e.g. "Album/Song"

void setStart(int i);          // which track PLAY starts from when nothing is loaded (call once at boot)
void play(int i);              // start track i
void toggle();                 // pause / resume (starts the last track if nothing is loaded)
void next();
void prev();
void setVolume(int v);         // 0-100, codec volume for music and beeps

struct Status {
  bool loaded, paused;
  int index, rate;
  uint32_t pos, size;          // bytes read / file size, for the progress bar
  bool mp3;
  const char *error;           // last problem, "" if none
};
Status status();

}  // namespace audio
