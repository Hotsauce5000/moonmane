// Desktop check of the game logic: runs whole lifetimes in fast-forward.
// Build: g++ -std=c++17 -O2 -I../Stablemate host_test.cpp ../Stablemate/game.cpp -o host_test
#include "game.h"
#include <cstdio>
#include <cstring>

using namespace sm;

static const char *FORMS[] = {"egg", "foal", "yearling", "arabian", "pony", "mule"};
static int64_t T0 = (int64_t)20386 * 86400 + 16 * 3600;  // a 4 PM start

struct Sim {
  Game g;
  int64_t t = T0;
  uint32_t ms = 0;
  void sec(int n = 1) {
    for (int i = 0; i < n; i++) {
      t++;
      g.advanceTo(t, false);
      for (int k = 0; k < 10; k++) { ms += 100; g.update(ms); }
      while (g.popSfx() != SFX_NONE) {}
      g.takeCallStarted();
    }
  }
  void press(Btn b) { g.press(b, ms); sec(1); }
  void waitIdle() { int n = 0; while (g.busy() && n++ < 120) sec(1); }
  void select(int icon) {
    for (int i = 0; i < 8 && g.selectedIcon() != icon; i++) press(BTN_A);
  }
  void use(int icon) { waitIdle(); select(icon); press(BTN_B); }
};

static void report(Sim &s, const char *label) {
  const State &S = s.g.state();
  printf("%-34s t+%7.2fh form=%-8s age=%2u hunger=%u happy=%u disc=%3u mistakes=%u poops=%u sick=%u asleep=%u dead=%u\n",
         label, (s.t - T0) / 3600.0, FORMS[S.form], S.age, S.hunger, S.happy, S.disc, S.mistakes, S.poops, S.sick,
         S.asleep, S.dead);
}

static int caretaker(bool good, int maxDays) {
  Sim s;
  s.g.seed(12345);
  s.g.reset(T0);
  uint8_t lastForm = 255;
  int64_t formAt[6] = {0};
  bool lastAsleep = false;
  int sleeps = 0;
  while (s.t - T0 < (int64_t)maxDays * 86400) {
    s.sec(good ? 30 : 600);
    const State &S = s.g.state();
    if (S.form != lastForm) {
      lastForm = S.form; formAt[S.form] = s.t;
      char l[64]; snprintf(l, sizeof l, "became %s", FORMS[S.form]); report(s, l);
    }
    if (S.asleep != lastAsleep) {
      lastAsleep = S.asleep;
      if (S.asleep && S.form >= F_YEARLING && sleeps++ < 2) {
        int64_t d = (s.t % 86400); printf("   fell asleep at %02d:%02d\n", (int)(d / 3600), (int)(d % 3600 / 60));
      }
    }
    if (S.dead) { report(s, "went to pasture"); break; }
    if (!good) continue;
    if (S.asleep) {
      if (S.lights) { s.use(1); s.press(BTN_A); s.press(BTN_B); }
      continue;
    }
    if (S.form == F_EGG) continue;
    if (S.falseCall) { s.use(6); s.waitIdle(); }
    if (S.sick) { s.use(3); s.waitIdle(); }
    if (S.poops) { s.use(4); s.waitIdle(); }
    if (S.hunger < 3) { s.use(0); s.press(BTN_B); s.waitIdle(); }
    if (S.happy < 2) {
      s.use(2);
      for (int r = 0; r < 5; r++) { s.press(BTN_A); s.sec(3); }
      s.sec(6); s.waitIdle();
    }
  }
  report(s, good ? "END (good care)" : "END (neglect)");
  if (good) {
    printf("   hatched after %lld s, yearling after %lld min, adult after %.1f h\n",
           (long long)(formAt[F_FOAL] - T0), (long long)((formAt[F_YEARLING] - formAt[F_FOAL]) / 60),
           ((formAt[F_ARAB] + formAt[F_PONY] + formAt[F_MULE]) - formAt[F_FOAL]) / 3600.0);
  }
  return s.g.state().dead;
}

int main() {
  // rendering smoke test across every mode
  Game g; g.seed(7); g.reset(T0);
  uint8_t fb[W * H];
  g.render(fb);
  int on = 0; for (int i = 0; i < W * H; i++) on += fb[i];
  printf("egg frame lit dots: %d\n", on);

  puts("== good care ==");
  caretaker(true, 30);
  puts("== neglect ==");
  caretaker(false, 10);

  // clock-setting flow
  Sim s; s.g.seed(3); s.g.reset(T0);
  s.press(BTN_C);              // clock
  s.press(BTN_B);              // set hour
  s.press(BTN_A); s.press(BTN_A);
  s.press(BTN_B);              // set minute
  s.press(BTN_C);
  s.press(BTN_B);              // confirm
  int h, m;
  bool got = s.g.takeClockRequest(h, m);
  printf("clock request: %s %02d:%02d (expect 18:59)\n", got ? "yes" : "no", h, m);
  return 0;
}
