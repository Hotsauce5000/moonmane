#include "game.h"
#include "sprites.h"
#include <string.h>
#include <stdio.h>

namespace sm {

// ---------------------------------------------------------------------------
// Pacing, modelled on the 1996 Tamagotchi: a 5-minute egg, a 65-minute baby
// stage with one short nap, then real days with a night's sleep by the clock.
// All values are seconds.
// ---------------------------------------------------------------------------
static const uint32_t HATCH_S = 5 * 60;
static const uint32_t FOAL_S = 65 * 60;
static const uint32_t FOAL_NAP_START = 40 * 60, FOAL_NAP_END = 45 * 60;
static const uint32_t YEARLING_S = 71 * 3600;  // adult about 3 days after hatching
static const uint32_t MISTAKE_S = 15 * 60;     // an ignored call becomes a care mistake
static const uint32_t SICK_MISTAKE_S = 3 * 3600, SICK_DIE_S = 12 * 3600;
static const uint32_t STARVE_DIE_S = 12 * 3600;

struct Rates {
  uint32_t h, p;           // awake seconds per lost hunger / happy heart
  int32_t poopMin, poopMax;
  uint8_t sleepH, wakeH;   // night sleep window, local hours
};
static const Rates RATES[6] = {
    {0, 0, 0, 0, 0, 0},                    // egg
    {180, 240, 1800, 1800, 0, 0},          // foal: hearts every 3 / 4 minutes
    {3600, 4200, 9000, 12600, 20, 9},      // yearling: 8 PM - 9 AM
    {4800, 5400, 10800, 14400, 21, 9},     // arabian: 9 PM - 9 AM
    {5400, 4800, 10800, 14400, 20, 8},     // pony: 8 PM - 8 AM
    {3600, 3600, 9000, 12600, 22, 9},      // mule: 10 PM - 9 AM
};
struct Info { const char *name, *hh; int lb; uint16_t life; };
static const Info INFO[6] = {
    {"EGG", "", 0, 0},          {"FOAL", "10.0", 120, 0},  {"YEARLING", "13.1", 550, 0},
    {"ARABIAN", "15.1", 950, 16}, {"PONY", "13.2", 650, 14}, {"MULE", "15.0", 900, 12},
};

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
static inline void px(uint8_t *fb, int x, int y, uint8_t v = 1) {
  if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = v;
}
static inline int bw(const Bitmap &b) { return (int)strlen(b.rows[0]); }
static void blit(uint8_t *fb, const Bitmap &b, int x, int y, bool flip = false, uint8_t v = 1, int cols = -1) {
  int w = bw(b), lim = cols < 0 ? w : (cols > w ? w : cols);
  for (int j = 0; j < b.h; j++)
    for (int i = 0; i < lim; i++)
      if (b.rows[j][i] == '#') px(fb, flip ? x + w - 1 - i : x + i, y + j, v);
}
static void fill(uint8_t *fb, uint8_t v) { memset(fb, v, W * H); }
static void text(uint8_t *fb, const char *s, int x, int y, uint8_t v = 1) {
  for (; *s; s++, x += 4) {
    const char *g = glyph15(*s);
    if (!g) continue;
    for (int j = 0; j < 5; j++)
      for (int i = 0; i < 3; i++)
        if (g[j * 3 + i] == '#') px(fb, x + i, y + j, v);
  }
}
static int tw(const char *s) { return (int)strlen(s) * 4 - 1; }
static void textC(uint8_t *fb, const char *s, int y, uint8_t v = 1) { text(fb, s, (W - tw(s)) / 2, y, v); }

static int horseW(uint8_t f) { return bw(HORSES[f].body); }
static int horseH(uint8_t f, bool lying) { return HORSES[f].body.h + (lying ? 0 : HORSES[f].legs[0].h); }
static int centerX(uint8_t f) { return (W - horseW(f)) / 2; }
static void horse(uint8_t *fb, uint8_t f, int x, int frame, bool flip, int yOff = 0, bool lying = false,
                  uint8_t v = 1) {
  const HorseSprite &h = HORSES[f];
  int y = H - horseH(f, lying) + yOff;
  blit(fb, h.body, x, y, flip, v);
  if (!lying) blit(fb, h.legs[frame & 1], x, y + h.body.h, flip, v);
}
static const int PILES[4][2] = {{27, 16}, {21, 16}, {27, 10}, {21, 10}};
static void drawPile(uint8_t *fb, int i, uint32_t f) {
  int x = PILES[i][0], y = PILES[i][1];
  blit(fb, PILE, x, y);
  if (f & 1) { px(fb, x + 1, y - 2); px(fb, x + 3, y - 3); }
  else { px(fb, x + 1, y - 3); px(fb, x + 3, y - 2); }
}
static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline int localHour(int64_t t) { int64_t d = t % 86400; if (d < 0) d += 86400; return (int)(d / 3600); }
static inline int localMin(int64_t t) { int64_t d = t % 3600; if (d < 0) d += 3600; return (int)(d / 60); }

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
void Game::reset(int64_t now) {
  memset(&S, 0, sizeof S);
  S.ver = STATE_VERSION;
  S.form = F_EGG;
  S.t = now;
  S.hunger = 2; S.happy = 2;
  S.lights = 1;
  S.poopT = 15 * 60;
  S.discT = 20 * 60;
  mode_ = M_IDLE; sel_ = -1; anim_.active = false; qn_ = 0;
  petX_ = 10; petDir_ = 1;
  dirty_ = true;
}

bool Game::load(const State &s) {
  if (s.ver != STATE_VERSION || s.form > F_MULE) return false;
  S = s;
  if (S.form != F_EGG && !S.dead) petX_ = clampi(centerX(S.form), 0, maxX() > 0 ? maxX() : 0);
  dirty_ = true;
  return true;
}

void Game::sfx(Sfx s) {
  if (sfxN_ >= 8) return;
  sfx_[(sfxHead_ + sfxN_) % 8] = s;
  sfxN_++;
}
Sfx Game::popSfx() {
  if (!sfxN_) return SFX_NONE;
  Sfx s = sfx_[sfxHead_];
  sfxHead_ = (sfxHead_ + 1) % 8;
  sfxN_--;
  return s;
}

bool Game::shouldSleep() const {
  if (S.form == F_EGG) return false;
  if (S.form == F_FOAL) return S.st >= FOAL_NAP_START && S.st < FOAL_NAP_END;
  const Rates &R = RATES[S.form];
  int h = localHour(S.t);
  return R.sleepH > R.wakeH ? (h >= R.sleepH || h < R.wakeH) : (h >= R.sleepH && h < R.wakeH);
}
uint16_t Game::lifespan() const {
  int l = INFO[S.form].life - S.mistakes / 4;
  return (uint16_t)(l < 4 ? 4 : l);
}
int32_t Game::nextPoop() { const Rates &R = RATES[S.form]; return rand(R.poopMin, R.poopMax); }
int32_t Game::nextDisc() {
  if (S.form == F_FOAL) return 0;  // a foal tests you once
  if (S.form == F_YEARLING) return rand(3 * 3600, 6 * 3600);
  return rand(6 * 3600, 10 * 3600);
}
void Game::addWeight(float f) {
  int b = INFO[S.form].lb;
  int v = S.wOff + (int)(b * f);
  S.wOff = (int16_t)clampi(v, -(int)(b * 0.3f), (int)(b * 0.6f));
}
int Game::weight() const { return ((INFO[S.form].lb + S.wOff + 2) / 5) * 5; }
bool Game::refuses() { return frand() < 0.3f * (1.0f - S.disc / 100.0f) * (S.form == F_FOAL ? 0.5f : 1.0f); }

void Game::advanceTo(int64_t now, bool silent) {
  // Guard against a wild clock: never grind through more than 4 days in one call.
  if (now - S.t > 4 * 86400) S.t = now - 4 * 86400;
  while (S.t < now) {
    S.t++;
    tick(silent);
  }
}

void Game::tick(bool silent) {
  if (S.dead) return;
  S.st++;
  if (S.form == F_EGG) {
    if (S.st >= HATCH_S) hatch(silent);
    return;
  }
  const Rates &R = RATES[S.form];

  bool want = shouldSleep();
  if (S.asleep && !want) {
    S.asleep = 0; S.lights = 1;
    if (S.form != F_FOAL) {
      S.age++;
      saveReq_ = true;
      if (INFO[S.form].life && !silent && S.age >= lifespan()) { die(); return; }
    }
  } else if (!S.asleep && want) {
    S.asleep = 1;
  }

  if (!S.asleep) {
    if (++S.hT >= R.h) { S.hT = 0; if (S.hunger) S.hunger--; }
    if (++S.pT >= R.p) { S.pT = 0; if (S.happy) S.happy--; }
    if (--S.poopT <= 0) { if (S.poops < 4) S.poops++; S.poopT = nextPoop(); }
    if (S.disc < 100 && !S.falseCall && S.discT > 0 && --S.discT <= 0) { S.falseCall = 1; S.discT = nextDisc(); }
    if (S.snacks && S.st % 1200 == 0) S.snacks--;
    if (S.st % 600 == 0 && !S.sick) {
      float p = 0.04f * S.poops + (S.hunger == 0 ? 0.05f : 0.0f);
      if (frand() < p) { S.sick = 1; S.doses = frand() < 0.5f ? 2 : 1; S.sickT = 0; }
    }
  }
  if (S.sick) {
    S.sickT++;
    if (!silent && S.form != F_FOAL) {
      if (S.sickT % SICK_MISTAKE_S == 0) addMistake();
      if (S.sickT >= SICK_DIE_S) { die(); return; }
    }
  }
  if (S.hunger == 0) {
    if (!S.asleep) S.starveT++;
    if (!silent && S.form != F_FOAL && S.starveT >= STARVE_DIE_S) { die(); return; }
  } else S.starveT = 0;

  bool needs[N_COUNT] = {S.hunger == 0 && !S.asleep, S.happy == 0 && !S.asleep, S.asleep && S.lights,
                         S.falseCall && !S.asleep};
  bool fresh = false;
  for (int k = 0; k < N_COUNT; k++) {
    if (needs[k]) {
      if (S.needT[k] == 0) fresh = true;
      S.needT[k]++;
      if (S.needT[k] == MISTAKE_S) {
        if (!silent) addMistake();
        if (k == N_TRAIN) S.falseCall = 0;
      }
    } else S.needT[k] = 0;
  }
  S.callOn = needs[N_HUNGER] || needs[N_HAPPY] || needs[N_LIGHTS] || (S.falseCall && !S.asleep);
  if (fresh && !silent) { sfx(SFX_CALL); callStarted_ = true; }

  if (S.form == F_FOAL && S.st >= FOAL_S) evolve(F_YEARLING, silent);
  else if (S.form == F_YEARLING && S.st >= YEARLING_S)
    evolve(S.stageMist <= 2 ? F_ARAB : S.stageMist <= 5 ? F_PONY : F_MULE, silent);
}

void Game::hatch(bool silent) {
  S.form = F_FOAL; S.st = 0; S.hT = 0; S.pT = 0;
  S.poopT = 15 * 60; S.discT = 20 * 60;
  petX_ = centerX(F_FOAL);
  saveReq_ = true;
  if (!silent) play(A_HATCH, 12, 260);
}
void Game::evolve(uint8_t to, bool silent) {
  uint8_t from = S.form;
  S.form = to; S.st = 0; S.wOff = 0; S.stageMist = 0;
  S.discT = nextDisc();
  S.poopT = nextPoop();
  petX_ = clampi(petX_, 0, maxX() > 0 ? maxX() : 0);
  saveReq_ = true;
  if (!silent) play(A_EVOLVE, 12, 220, from, to);
}
void Game::die() {
  S.deadAge = S.age; S.deadForm = S.form; S.dead = 1;
  S.asleep = 0; S.sick = 0; S.poops = 0; S.lights = 1; S.callOn = 0; S.falseCall = 0;
  mode_ = M_IDLE; sel_ = -1; anim_.active = false; qn_ = 0;
  sfx(SFX_DIE);
  saveReq_ = true; dirty_ = true;
}

// ---------------------------------------------------------------------------
// animation
// ---------------------------------------------------------------------------
void Game::play(AnimType t, uint8_t n, uint16_t step, uint8_t a, uint8_t b) {
  Anim x{true, t, 0, n, a, b, step, nowMs_};
  if (anim_.active) { if (qn_ < 4) queue_[qn_++] = x; return; }
  anim_ = x;
  dirty_ = true;
}

void Game::update(uint32_t ms) {
  nowMs_ = ms;
  if (anim_.active && ms - anim_.last >= anim_.step) {
    anim_.last = ms;
    if (++anim_.f >= anim_.n) {
      Anim done = anim_;
      anim_.active = false;
      animDone(done);
      if (!anim_.active && qn_) {
        anim_ = queue_[0];
        anim_.last = ms;
        for (int i = 1; i < qn_; i++) queue_[i - 1] = queue_[i];
        qn_--;
      }
    }
    dirty_ = true;
  }
  if (ms - lastBeat_ >= 500) {
    lastBeat_ = ms; beat_++;
    if (!anim_.active) {
      if (mode_ == M_GAME) stepGame();
      else if (mode_ == M_IDLE && !S.dead && S.form != F_EGG && !S.asleep) walk();
      else if (mode_ == M_IDLE && S.asleep) petX_ = clampi(petX_, 0, maxX() > 0 ? maxX() : 0);
    }
    dirty_ = true;
  }
}

void Game::animDone(const Anim &a) {
  switch (a.type) {
    case A_EAT:
      if (a.a == 0) { S.hunger = S.hunger < 4 ? S.hunger + 1 : 4; addWeight(0.02f); }
      else {
        S.happy = S.happy < 4 ? S.happy + 1 : 4; addWeight(0.04f); S.snacks++;
        if (S.snacks >= 4 && !S.sick && frand() < 0.4f) { S.sick = 1; S.doses = 1; S.sickT = 0; }
      }
      sfx(SFX_GOOD);
      break;
    case A_REFUSE: sfx(SFX_ERR); break;
    case A_VET:
      if (S.doses) S.doses--;
      if (!S.doses) { S.sick = 0; S.sickT = 0; play(A_HAPPY, 6, 300); }
      else sfx(SFX_CLICK);
      break;
    case A_MUCK:
      S.poops = 0;
      if (a.a) play(A_HAPPY, 6, 300); else sfx(SFX_CLICK);
      break;
    case A_TRAIN:
      if (S.falseCall) { S.falseCall = 0; S.needT[N_TRAIN] = 0; S.disc = S.disc + 25 > 100 ? 100 : S.disc + 25; sfx(SFX_GOOD); }
      else { if (S.happy) S.happy--; sfx(SFX_ERR); }
      break;
    default: sfx(SFX_GOOD); break;
  }
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
void Game::feed(int k) {
  if (k == 0) {
    if (S.hunger >= 4 || refuses()) { play(A_REFUSE, 6, 320); return; }
    play(A_EAT, 6, 450, 0);
  } else {
    if (refuses()) { play(A_REFUSE, 6, 320); return; }
    play(A_EAT, 6, 450, 1);
  }
}

void Game::activate(int i) {
  bool ok = !S.asleep && S.lights && S.form != F_EGG;
  switch (i) {
    case 0: if (!ok) { sfx(SFX_ERR); return; } mode_ = M_MENU; menu_ = 0; menuIdx_ = 0; break;
    case 1: mode_ = M_MENU; menu_ = 1; menuIdx_ = S.lights ? 0 : 1; break;
    case 2:
      if (!ok) { sfx(SFX_ERR); return; }
      if (S.sick || refuses()) { play(A_REFUSE, 6, 320); sfx(SFX_CLICK); return; }
      mode_ = M_GAME; g_ = GallopGame{0, 0, 0, 0, false, 1};
      break;
    case 3:
      if (!ok) { sfx(SFX_ERR); return; }
      if (!S.sick) play(A_REFUSE, 6, 320); else play(A_VET, 7, 380);
      break;
    case 4: if (!ok) { sfx(SFX_ERR); return; } play(A_MUCK, 20, 70, S.poops); break;
    case 5: mode_ = M_STATUS; page_ = 0; break;
    case 6: if (!ok) { sfx(SFX_ERR); return; } play(A_TRAIN, 6, 380); break;
  }
  sfx(SFX_CLICK);
}

void Game::press(Btn b, uint32_t ms) {
  nowMs_ = ms;
  dirty_ = true;
  if (anim_.active) return;

  if (mode_ == M_CLOCK) {
    if (b == BTN_B) { mode_ = M_SET_H; setH_ = localHour(S.t); setM_ = localMin(S.t); }
    else mode_ = M_IDLE;
    sfx(SFX_CLICK);
    return;
  }
  if (mode_ == M_SET_H || mode_ == M_SET_M) {
    int &v = mode_ == M_SET_H ? setH_ : setM_;
    int mod = mode_ == M_SET_H ? 24 : 60;
    if (b == BTN_A) v = (v + 1) % mod;
    else if (b == BTN_C) v = (v + mod - 1) % mod;
    else if (mode_ == M_SET_H) mode_ = M_SET_M;
    else { mode_ = M_IDLE; clockReq_ = true; sfx(SFX_GOOD); return; }
    sfx(SFX_CLICK);
    return;
  }
  if (S.dead) {
    if (b == BTN_B) { reset(S.t); saveReq_ = true; sfx(SFX_GOOD); }
    else if (b == BTN_C) { mode_ = M_CLOCK; sfx(SFX_CLICK); }
    else sfx(SFX_CLICK);
    return;
  }
  if (S.form == F_EGG && mode_ == M_IDLE) {
    if (b == BTN_C) mode_ = M_CLOCK;
    sfx(SFX_CLICK);
    return;
  }
  switch (mode_) {
    case M_STATUS:
      if (b == BTN_C) mode_ = M_IDLE; else page_ = (page_ + 1) % 5;
      sfx(SFX_CLICK);
      break;
    case M_MENU:
      if (b == BTN_A) { menuIdx_ ^= 1; sfx(SFX_CLICK); }
      else if (b == BTN_B) {
        mode_ = M_IDLE; sfx(SFX_CLICK);
        if (menu_ == 0) feed(menuIdx_);
        else S.lights = menuIdx_ == 0;
      } else { mode_ = M_IDLE; sfx(SFX_CLICK); }
      break;
    case M_GAME: gameInput(b); break;
    default:
      if (b == BTN_A) { sel_ = (sel_ + 1) % 7; sfx(SFX_CLICK); }
      else if (b == BTN_B) { if (sel_ < 0) sfx(SFX_CLICK); else activate(sel_); }
      else { if (sel_ >= 0) sel_ = -1; else mode_ = M_CLOCK; sfx(SFX_CLICK); }
  }
}

void Game::gameInput(Btn b) {
  if (b == BTN_C) { mode_ = M_IDLE; sfx(SFX_CLICK); return; }
  if (g_.phase != 0) return;
  int8_t guess = b == BTN_A ? -1 : 1;
  g_.turn = (rnd() & 1) ? -1 : 1;
  g_.win = guess == g_.turn;
  if (g_.win) g_.wins++;
  g_.phase = 1; g_.f = 0;
  sfx(g_.win ? SFX_WIN : SFX_LOSE);
}
void Game::stepGame() {
  g_.f++;
  if (g_.phase == 1 && g_.f >= 3) {
    g_.round++; g_.f = 0;
    g_.phase = g_.round >= 5 ? 2 : 0;
  } else if (g_.phase == 2 && g_.f >= 4) {
    mode_ = M_IDLE;
    if (g_.wins >= 3) { S.happy = S.happy < 4 ? S.happy + 1 : 4; addWeight(-0.02f); play(A_HAPPY, 6, 300); }
    else { addWeight(-0.01f); sfx(SFX_ERR); }
  }
}

int Game::maxX() const {
  if (S.form == F_EGG) return 0;
  return W - (S.poops > 1 ? 12 : S.poops ? 6 : 0) - horseW(S.form);
}
void Game::walk() {
  float r = frand(), slow = S.sick ? 0.5f : 1.0f;
  if (r < 0.15f) petDir_ = -petDir_;
  else if (r < 0.15f + 0.6f * slow) petX_ += petDir_;
  int mx = maxX() > 0 ? maxX() : 0;
  if (petX_ <= 0) { petX_ = 0; petDir_ = 1; }
  if (petX_ >= mx) { petX_ = mx; petDir_ = -1; }
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------
void Game::ageText(char *out, uint8_t form, uint16_t age) const {
  if (form == F_FOAL) strcpy(out, "NEWBORN");
  else if (age == 1) strcpy(out, "1 YR");
  else sprintf(out, "%u YRS", (unsigned)age);
}

void Game::render(uint8_t *fb) const {
  fill(fb, 0);
  if (anim_.active) drawAnim(fb);
  else if (mode_ == M_CLOCK || mode_ == M_SET_H || mode_ == M_SET_M) drawClock(fb);
  else if (mode_ == M_STATUS) drawStatus(fb);
  else if (mode_ == M_MENU) drawMenu(fb);
  else if (mode_ == M_GAME) drawGame(fb);
  else drawIdle(fb);
}

void Game::drawIdle(uint8_t *fb) const {
  if (S.dead) { drawDead(fb); return; }
  if (S.form == F_EGG) { int wob = (S.st > HATCH_S - 60 && (beat_ & 1)) ? 1 : 0; blit(fb, EGG, 12 + wob, H - 10); return; }
  int w = horseW(S.form);
  if (!S.lights) {
    fill(fb, 1);
    if (S.asleep) { blit(fb, ZZ, 22 + (beat_ & 1) * 4, 3 - (beat_ & 1) * 2, false, 0); blit(fb, ZZ, 12, 6 + (beat_ & 1), false, 0); }
    return;
  }
  for (int i = 0; i < S.poops; i++) drawPile(fb, i, beat_);
  if (S.asleep) {
    horse(fb, S.form, petX_, 0, petDir_ < 0, 0, true);
    int top = H - horseH(S.form, true);
    int zx = petDir_ > 0 ? (petX_ + w - 2 < 28 ? petX_ + w - 2 : 28) : (petX_ - 3 > 0 ? petX_ - 3 : 0);
    blit(fb, ZZ, zx, top - 6 - (int)(beat_ & 1) > 0 ? top - 6 - (int)(beat_ & 1) : 0);
    if (beat_ & 1) blit(fb, ZZ, clampi(zx + (petDir_ > 0 ? -4 : 4), 0, 28), top - 12 > 0 ? top - 12 : 0);
  } else {
    horse(fb, S.form, petX_, beat_ & 1, petDir_ < 0);
  }
  if (S.sick) blit(fb, SKULL, clampi(petDir_ > 0 ? petX_ + w + 1 : petX_ - 6, 0, 27), 0);
}

void Game::drawDead(uint8_t *fb) const {
  uint8_t f = S.deadForm < F_FOAL ? (uint8_t)F_FOAL : S.deadForm;
  int w = horseW(f), x = centerX(f);
  if ((beat_ / 6) % 2 == 0) {
    const HorseSprite &h = HORSES[f];
    int y = 3 + (beat_ & 1);
    blit(fb, h.body, x, y);
    blit(fb, h.legs[0], x, y + h.body.h);
    for (int i = 0; i < 4; i++) px(fb, x + w - 5 + i, y - 2);
    blit(fb, WING[beat_ & 1], x + 3, y + h.body.h - 6);
  } else {
    char a[12];
    ageText(a, S.deadForm, S.deadAge);
    textC(fb, "PASTURE", 1); textC(fb, a, 8); textC(fb, "B=NEW", 14);
  }
}

void Game::drawMenu(uint8_t *fb) const {
  static const char *FEED[2] = {"HAY", "APPLE"}, *LIGHT[2] = {"ON", "OFF"};
  const char **items = menu_ == 0 ? FEED : LIGHT;
  for (int i = 0; i < 2; i++) {
    int y = i == 0 ? 3 : 12;
    if (menuIdx_ == i) text(fb, ">", 1, y);
    text(fb, items[i], 6, y);
    if (menu_ == 0) { const Bitmap &p = i == 0 ? HAY : APPLE; blit(fb, p, W - bw(p) - 1, y); }
  }
}

void Game::drawStatus(uint8_t *fb) const {
  char buf[16];
  switch (page_) {
    case 0: textC(fb, INFO[S.form].name, 3); ageText(buf, S.form, S.age); textC(fb, buf, 12); break;
    case 1:
      sprintf(buf, "%sHH", INFO[S.form].hh); textC(fb, buf, 3);
      sprintf(buf, "%dLB", weight()); textC(fb, buf, 12);
      break;
    case 2: {
      textC(fb, "TRAINED", 2);
      for (int x = 1; x <= 30; x++) { px(fb, x, 10); px(fb, x, 15); }
      for (int y = 10; y <= 15; y++) { px(fb, 1, y); px(fb, 30, y); }
      int n = (26 * S.disc + 50) / 100;
      for (int x = 0; x < n; x++) { px(fb, 3 + x, 12); px(fb, 3 + x, 13); }
      break;
    }
    case 3:
    case 4:
      textC(fb, page_ == 3 ? "HUNGRY" : "HAPPY", 2);
      for (int i = 0; i < 4; i++) blit(fb, i < (page_ == 3 ? S.hunger : S.happy) ? HEART_FULL : HEART_EMPTY, i * 8 + 1, 11);
      break;
  }
}

void Game::drawClock(uint8_t *fb) const {
  int h24 = mode_ == M_CLOCK ? localHour(S.t) : setH_;
  int m = mode_ == M_CLOCK ? localMin(S.t) : setM_;
  // Showing the time: 12-hour with AM/PM. Setting it: 24-hour, with the part being set blinking.
  int h = h24;
  if (mode_ == M_CLOCK) { h = h24 % 12; if (!h) h = 12; }
  bool blinkH = mode_ == M_SET_H && (beat_ & 1), blinkM = mode_ == M_SET_M && (beat_ & 1);
  char hs[4], ms[4];
  snprintf(hs, sizeof hs, mode_ == M_CLOCK ? "%2d" : "%02d", h);
  snprintf(ms, sizeof ms, "%02d", m);
  int x = (W - tw("00:00")) / 2;  // fixed layout so blinking doesn't shift anything
  if (!blinkH) { if (hs[0] == ' ') text(fb, hs + 1, x + 4, 4); else text(fb, hs, x, 4); }
  text(fb, ":", x + 8, 4);
  if (!blinkM) text(fb, ms, x + 12, 4);
  if (mode_ == M_CLOCK) textC(fb, h24 >= 12 ? "PM" : "AM", 12);
  else textC(fb, mode_ == M_SET_H ? "SET HR" : "SET MIN", 12);
}

void Game::drawGame(uint8_t *fb) const {
  uint8_t f = S.form;
  int x = centerX(f);
  char buf[12];
  if (g_.phase == 0) {
    sprintf(buf, "<%d/5>", g_.round + 1);
    textC(fb, buf, 0);
    horse(fb, f, x, 0, beat_ & 1);
  } else if (g_.phase == 1) {
    textC(fb, g_.win ? "YES!" : "NO", 0);
    int step = g_.f + 1 < 3 ? g_.f + 1 : 3;
    horse(fb, f, clampi(x + g_.turn * step * 2, 0, W - horseW(f)), g_.f & 1, g_.turn < 0);
  } else {
    sprintf(buf, "%d OF 5", g_.wins);
    textC(fb, buf, 3);
    textC(fb, g_.wins >= 3 ? "WIN!" : "LOSE", 12);
  }
}

void Game::drawAnim(uint8_t *fb) const {
  const Anim &a = anim_;
  int f = a.f;
  uint8_t form = S.form;
  switch (a.type) {
    case A_EAT: {
      const Bitmap &food = a.a == 0 ? HAY : APPLE;
      int w = horseW(form), fw = bw(food), x = (W - (w + 1 + fw)) / 2;
      horse(fb, form, x, f & 1, false, f & 1);
      int bite = ((fw + 2) / 3) * (f / 2);
      blit(fb, food, x + w + 1, H - food.h, false, 1, fw - bite);
      break;
    }
    case A_REFUSE: textC(fb, "NO", 0); horse(fb, form, centerX(form), 0, (f & 1) == 0); break;
    case A_HAPPY: {
      horse(fb, form, centerX(form), f & 1, false, (f & 1) ? -2 : 0);
      if (f & 1) { blit(fb, SPARK, 1, 2); blit(fb, SPARK, 28, 4); }
      else { blit(fb, SPARK, 4, 6); blit(fb, SPARK, 25, 1); }
      break;
    }
    case A_VET: {
      int x = W - horseW(form) - 1;
      horse(fb, form, x, 0, true);
      if (f < 5) blit(fb, SYRINGE, f * 2 < x - 7 ? f * 2 : x - 7, H - 9);
      else if (f & 1) blit(fb, SPARK, x + 2, H - 15);
      break;
    }
    case A_MUCK: {
      int lx = W + 2 - f * 2;
      for (int i = 0; i < S.poops; i++) if (PILES[i][0] + 5 < lx) drawPile(fb, i, f >> 2);
      horse(fb, form, petX_, 0, petDir_ < 0);
      for (int y = 0; y < H; y++) { int o = (y + f) % 4 < 2 ? 0 : 1; px(fb, lx + o, y); px(fb, lx + o + 2, y); }
      break;
    }
    case A_TRAIN: {
      int x = centerX(form) > 6 ? centerX(form) : 6;
      horse(fb, form, x, 0, f >= 3 ? (f % 2 == 0) : false);
      for (int y = 4; y <= 10; y++) { px(fb, 1, y); px(fb, 2, y); }
      px(fb, 1, 12); px(fb, 2, 12); px(fb, 1, 13); px(fb, 2, 13);
      break;
    }
    case A_HATCH:
      if (f < 6) blit(fb, EGG, 12 + ((f & 1) ? 1 : -1), H - 10);
      else if (f < 8) { blit(fb, EGG, 12, H - 10); px(fb, 14, 13, 0); px(fb, 15, 14); px(fb, 16, 13, 0); px(fb, 17, 14); px(fb, 13, 14); }
      else { horse(fb, F_FOAL, centerX(F_FOAL), f & 1, false, (f & 1) ? -1 : 0); blit(fb, SPARK, 3, 3); blit(fb, SPARK, 26, 2); }
      break;
    case A_EVOLVE: {
      uint8_t ff = f < 6 ? a.a : a.b;
      if (ff < F_FOAL) ff = F_FOAL;
      if ((f & 1) == 0) { fill(fb, 1); horse(fb, ff, centerX(ff), 0, false, 0, false, 0); }
      else horse(fb, ff, centerX(ff), 0, false);
      break;
    }
  }
}

}  // namespace sm
