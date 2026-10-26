// Moonmane game logic. Hardware-free: the sketch feeds it wall-clock time and button
// presses, and reads back a 32x20 dot framebuffer, icon state and sound requests.
#pragma once
#include <stdint.h>

namespace sm {

constexpr int W = 32, H = 20;
constexpr uint16_t STATE_VERSION = 3;

enum Btn : uint8_t { BTN_A, BTN_B, BTN_C };
enum Sfx : uint8_t { SFX_NONE, SFX_CLICK, SFX_ERR, SFX_CALL, SFX_GOOD, SFX_DIE, SFX_WIN, SFX_LOSE };
enum Form : uint8_t { F_EGG, F_FOAL, F_YEARLING, F_ARAB, F_PONY, F_MULE };
enum Need : uint8_t { N_HUNGER, N_HAPPY, N_LIGHTS, N_TRAIN, N_COUNT };

// Everything that must survive a reboot. Plain data so it can be stored as one NVS blob.
struct State {
  uint16_t ver;
  uint8_t form, dead;
  int64_t t;          // last simulated local time (seconds since 1970, local clock)
  uint32_t st;        // seconds in the current stage
  uint8_t hunger, happy, disc;  // hearts 0-4, discipline 0-100
  int16_t wOff;       // weight offset from the stage's base, lb
  uint32_t hT, pT;    // awake seconds since a heart was lost
  uint8_t poops;
  int32_t poopT;      // awake seconds until next manure
  uint8_t sick, doses;
  uint32_t sickT;
  uint8_t asleep, lights;
  uint8_t falseCall;
  int32_t discT;      // awake seconds until the next attention-seeking call (0 = none planned)
  uint16_t mistakes, stageMist;
  uint32_t needT[N_COUNT];
  uint32_t starveT;
  uint8_t snacks;
  uint16_t age;       // horse years = mornings woken up
  uint8_t deadForm;
  uint16_t deadAge;
  uint8_t callOn;
};

class Game {
 public:
  void seed(uint32_t s) { rng_ = s ? s : 0x9e3779b9u; }
  void reset(int64_t now);
  bool load(const State &s);  // false if the blob is from another version
  const State &state() const { return S; }

  // Simulate up to local time `now`. Silent catch-up never kills, counts no mistakes,
  // plays no sounds and starts no animations.
  void advanceTo(int64_t now, bool silent);
  // The clock was changed by the player: jump without simulating the gap.
  void clockChanged(int64_t now) { S.t = now; }

  void update(uint32_t ms);           // animation and walking, call often
  void press(Btn b, uint32_t ms);
  void render(uint8_t *fb) const;     // W*H bytes, 1 = dot on

  int8_t selectedIcon() const { return sel_; }
  bool attention() const { return S.callOn && !S.dead; }
  bool busy() const { return anim_.active; }
  Sfx popSfx();
  bool takeCallStarted() { bool c = callStarted_; callStarted_ = false; return c; }
  bool takeSaveRequest() { bool s = saveReq_; saveReq_ = false; return s; }
  // Clock-setting UI result: true once with the chosen hour and minute.
  bool takeClockRequest(int &h, int &m) {
    if (!clockReq_) return false;
    clockReq_ = false; h = setH_; m = setM_; return true;
  }
  bool dirty() const { return dirty_; }
  void clean() { dirty_ = false; }

 private:
  enum Mode : uint8_t { M_IDLE, M_MENU, M_STATUS, M_GAME, M_CLOCK, M_SET_H, M_SET_M };
  enum AnimType : uint8_t { A_EAT, A_REFUSE, A_HAPPY, A_VET, A_MUCK, A_TRAIN, A_HATCH, A_EVOLVE };
  struct Anim {
    bool active;
    AnimType type;
    uint8_t f, n, a, b;  // frame, frame count, params
    uint16_t step;
    uint32_t last;
  };
  struct GallopGame { uint8_t round, wins, phase, f; bool win; int8_t turn; };

  State S{};
  uint32_t rng_ = 1;
  Mode mode_ = M_IDLE;
  int8_t sel_ = -1;
  uint8_t menu_ = 0, menuIdx_ = 0, page_ = 0;  // menu_: 0 feed, 1 lights
  GallopGame g_{};
  Anim anim_{};
  Anim queue_[4]{};
  uint8_t qn_ = 0;
  uint32_t beat_ = 0, lastBeat_ = 0, nowMs_ = 0;
  int petX_ = 10, petDir_ = 1;
  Sfx sfx_[8]{};
  uint8_t sfxHead_ = 0, sfxN_ = 0;
  bool callStarted_ = false, saveReq_ = false, clockReq_ = false, dirty_ = true;
  int setH_ = 0, setM_ = 0;

  // simulation
  void tick(bool silent);
  void hatch(bool silent);
  void evolve(uint8_t to, bool silent);
  void die();
  bool shouldSleep() const;
  uint16_t lifespan() const;
  int32_t nextPoop();
  int32_t nextDisc();
  void addMistake() { S.mistakes++; S.stageMist++; }
  void addWeight(float f);
  int weight() const;
  bool refuses();
  uint32_t rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
  int rand(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }
  float frand() { return (rnd() & 0xffffff) / 16777216.0f; }
  void sfx(Sfx s);

  // ui
  void play(AnimType t, uint8_t n, uint16_t step, uint8_t a = 0, uint8_t b = 0);
  void animDone(const Anim &a);
  void activate(int i);
  void feed(int k);
  void gameInput(Btn b);
  void stepGame();
  void walk();
  int maxX() const;

  // drawing
  void drawIdle(uint8_t *fb) const;
  void drawDead(uint8_t *fb) const;
  void drawMenu(uint8_t *fb) const;
  void drawStatus(uint8_t *fb) const;
  void drawClock(uint8_t *fb) const;
  void drawGame(uint8_t *fb) const;
  void drawAnim(uint8_t *fb) const;
  void ageText(char *out, uint8_t form, uint16_t age) const;
};

}  // namespace sm
