// Turns an MP3 or WAV file into 16-bit stereo PCM, one chunk at a time.
// Hardware-free: it reads through a small Reader interface, so it runs on a desktop for testing.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace music {

struct Reader {
  virtual ~Reader() {}
  virtual size_t read(uint8_t *buf, size_t n) = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual uint32_t position() = 0;
  virtual uint32_t size() = 0;
};

constexpr int MAX_FRAMES = 1152;  // most stereo frames decode() ever returns

class Decoder {
 public:
  Decoder();
  ~Decoder();
  // isMp3 picks the format; false means WAV. Returns false with error() set if the file can't be played.
  bool open(Reader *r, bool isMp3);
  void close() { r_ = nullptr; }
  // Fills `out` with interleaved stereo samples. Returns frames written; 0 at the end of the track.
  int decode(int16_t *out);
  int rate() const { return rate_; }  // sample rate; known after open() for WAV, after the first decode() for MP3
  const char *error() const { return err_; }

 private:
  Reader *r_ = nullptr;
  bool mp3_ = true;
  int rate_ = 0, channels_ = 2, bits_ = 16;
  uint32_t dataLeft_ = 0;
  const char *err_ = "";
  uint8_t *in_ = nullptr;  // MP3 input buffer
  int inLen_ = 0, inPos_ = 0;
  bool eof_ = false;
  void *mp3_state_ = nullptr;
  int16_t *pcm_ = nullptr;
  bool openWav();
  bool openMp3();
  int decodeWav(int16_t *out);
  int decodeMp3(int16_t *out);
};

}  // namespace music
