#include "decoder.h"
#include <string.h>
#include <stdlib.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

namespace music {

static const int IN_SIZE = 16 * 1024;
static const int REFILL_AT = 8 * 1024;

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

Decoder::Decoder() {
  in_ = (uint8_t *)malloc(IN_SIZE);
  mp3_state_ = malloc(sizeof(mp3dec_t));
  pcm_ = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
}
Decoder::~Decoder() {
  free(in_);
  free(mp3_state_);
  free(pcm_);
}

bool Decoder::open(Reader *r, bool isMp3) {
  r_ = r; mp3_ = isMp3; rate_ = 0; err_ = "";
  inLen_ = inPos_ = 0; eof_ = false;
  if (!in_ || !mp3_state_ || !pcm_) { err_ = "OUT OF MEMORY"; r_ = nullptr; return false; }
  bool ok = isMp3 ? openMp3() : openWav();
  if (!ok) r_ = nullptr;
  return ok;
}

bool Decoder::openMp3() {
  uint8_t h[10];
  uint32_t start = 0;
  if (r_->read(h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
    // Skip the ID3v2 tag (often large because of embedded cover art).
    start = 10 + (((h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f));
    if (h[5] & 0x10) start += 10;  // footer present
  }
  if (!r_->seek(start)) { err_ = "CAN'T READ FILE"; return false; }
  mp3dec_init((mp3dec_t *)mp3_state_);
  return true;
}

bool Decoder::openWav() {
  uint8_t h[16];
  if (r_->read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { err_ = "NOT A WAV FILE"; return false; }
  bool haveFmt = false;
  for (;;) {
    if (r_->read(h, 8) != 8) { err_ = "NO AUDIO DATA"; return false; }
    uint32_t sz = le32(h + 4);
    uint32_t next = r_->position() + sz + (sz & 1);
    if (!memcmp(h, "fmt ", 4)) {
      uint8_t f[16];
      if (sz < 16 || r_->read(f, 16) != 16) { err_ = "BAD WAV HEADER"; return false; }
      uint16_t fmt = le16(f);
      channels_ = le16(f + 2);
      rate_ = (int)le32(f + 4);
      bits_ = le16(f + 14);
      if (fmt != 1 && fmt != 0xFFFE) { err_ = "WAV NOT PCM"; return false; }
      if ((bits_ != 16 && bits_ != 8) || channels_ < 1 || channels_ > 8) { err_ = "WAV FORMAT UNSUPPORTED"; return false; }
      haveFmt = true;
    } else if (!memcmp(h, "data", 4)) {
      if (!haveFmt) { err_ = "BAD WAV HEADER"; return false; }
      dataLeft_ = sz;
      return true;
    }
    if (!r_->seek(next)) { err_ = "NO AUDIO DATA"; return false; }
  }
}

int Decoder::decode(int16_t *out) {
  if (!r_) return 0;
  return mp3_ ? decodeMp3(out) : decodeWav(out);
}

int Decoder::decodeWav(int16_t *out) {
  int bytesPerFrame = channels_ * (bits_ / 8);
  uint32_t want = (uint32_t)MAX_FRAMES * bytesPerFrame;
  if (want > (uint32_t)IN_SIZE) want = (IN_SIZE / bytesPerFrame) * bytesPerFrame;
  if (want > dataLeft_) want = dataLeft_ - dataLeft_ % bytesPerFrame;
  if (want == 0) return 0;
  size_t got = r_->read(in_, want);
  got -= got % bytesPerFrame;
  dataLeft_ -= got;
  int frames = (int)(got / bytesPerFrame);
  for (int i = 0; i < frames; i++) {
    const uint8_t *p = in_ + i * bytesPerFrame;
    int16_t l, rr;
    if (bits_ == 16) { l = (int16_t)le16(p); rr = channels_ > 1 ? (int16_t)le16(p + 2) : l; }
    else { l = (int16_t)((p[0] - 128) << 8); rr = channels_ > 1 ? (int16_t)((p[1] - 128) << 8) : l; }
    out[2 * i] = l;
    out[2 * i + 1] = rr;
  }
  return frames;
}

int Decoder::decodeMp3(int16_t *out) {
  mp3dec_t *dec = (mp3dec_t *)mp3_state_;
  for (int guard = 0; guard < 64; guard++) {
    if (!eof_ && inLen_ - inPos_ < REFILL_AT) {
      memmove(in_, in_ + inPos_, inLen_ - inPos_);
      inLen_ -= inPos_;
      inPos_ = 0;
      size_t n = r_->read(in_ + inLen_, IN_SIZE - inLen_);
      if (n == 0) eof_ = true;
      inLen_ += (int)n;
    }
    if (inLen_ - inPos_ <= 0) return 0;
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(dec, in_ + inPos_, inLen_ - inPos_, pcm_, &info);
    if (info.frame_bytes == 0) {  // no frame in what's buffered
      if (eof_) return 0;
      inPos_ = inLen_;            // drop it and read on
      continue;
    }
    inPos_ += info.frame_bytes;
    if (samples == 0) continue;   // skipped tag or junk
    rate_ = info.hz;
    if (info.channels == 1) {
      for (int i = 0; i < samples; i++) { out[2 * i] = pcm_[i]; out[2 * i + 1] = pcm_[i]; }
    } else {
      memcpy(out, pcm_, samples * 2 * sizeof(int16_t));
    }
    return samples;
  }
  err_ = "NOT AN MP3";
  return 0;
}

}  // namespace music
