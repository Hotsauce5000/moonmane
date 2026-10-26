// Decodes MP3/WAV files on a desktop and prints rate and length.
// Build: g++ -std=c++17 -O2 -I../MoonmaneMusic decoder_test.cpp ../MoonmaneMusic/decoder.cpp -o decoder_test
// Run:   ./decoder_test song.mp3 clip.wav
#include "decoder.h"
#include <cstdio>
#include <cstring>
#include <cmath>
struct FileReader : music::Reader {
  FILE *f; uint32_t sz;
  FileReader(const char *p) { f = fopen(p, "rb"); fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); }
  ~FileReader() { fclose(f); }
  size_t read(uint8_t *b, size_t n) override { return fread(b, 1, n, f); }
  bool seek(uint32_t p) override { return fseek(f, p, SEEK_SET) == 0; }
  uint32_t position() override { return ftell(f); }
  uint32_t size() override { return sz; }
};
int main(int argc, char **argv) {
  music::Decoder d;
  static int16_t out[music::MAX_FRAMES * 2];
  for (int i = 1; i < argc; i++) {
    FileReader r(argv[i]);
    bool mp3 = strstr(argv[i], ".mp3");
    if (!d.open(&r, mp3)) { printf("%s: open failed: %s\n", argv[i], d.error()); continue; }
    long frames = 0; int n, peak = 0;
    while ((n = d.decode(out)) > 0) { frames += n; for (int k = 0; k < n * 2; k++) if (abs(out[k]) > peak) peak = abs(out[k]); }
    printf("%s: %d Hz, %ld frames = %.2f s, peak %d %s\n", argv[i], d.rate(), frames, d.rate() ? (double)frames / d.rate() : 0, peak, d.error());
  }
}
