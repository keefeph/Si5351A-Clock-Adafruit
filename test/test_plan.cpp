// Host test for si5351_plan.h:  g++ -O2 -I../Si5351_WebClock test_plan.cpp -o t && ./t
#include <cstdio>
#include <cstdlib>
#include <random>
#include "si5351_plan.h"
using namespace si5351plan;

static int fails = 0;

// Independently verify a plan against the chip's rules.
static bool verify(const Plan& p, const bool en[3], const uint32_t f[3], double& maxErr, bool print) {
  for (int g = 0; g < 2; g++) {
    if (!p.pll[g].used) continue;
    const Frac& m = p.pll[g].mult;
    if (m.c == 0 || m.c > DEN_MAX || m.b >= m.c || m.a < 15 || m.a > 90) { printf("bad PLL frac\n"); return false; }
    if (p.pll[g].vcoActual < VCO_MIN_HZ - 1 || p.pll[g].vcoActual > VCO_MAX_HZ + 1) { printf("VCO out of range %f\n", p.pll[g].vcoActual); return false; }
  }
  for (int i = 0; i < 3; i++) {
    if (!en[i]) continue;
    const OutPlan& o = p.out[i];
    if (!p.pll[o.pll].used) { printf("unused pll\n"); return false; }
    const Frac& d = o.ms;
    if (d.c == 0 || d.c > DEN_MAX || d.b >= d.c) { printf("bad ms frac\n"); return false; }
    bool okDiv = (d.b == 0 && (d.a == 4 || d.a == 6)) || (d.a >= 8 && (d.a < 2048 || (d.a == 2048 && d.b == 0)));
    if (!okDiv) { printf("bad MS div %u+%u/%u\n", d.a, d.b, d.c); return false; }
    if (d.a == 4 && !o.divBy4) return false;
    if (o.rdiv > 7) return false;
    double act = p.pll[o.pll].vcoActual / (d.a + (double)d.b / d.c) / (1 << o.rdiv);
    double e = fabs(act - f[i]);
    if (e > maxErr) maxErr = e;
    if (print)
      printf("  CLK%d %12u Hz -> PLL%c VCO %.6f MHz  MS %u+%u/%u%s R=%d  actual %.6f Hz  err %.2e Hz\n",
             i, f[i], 'A' + o.pll, p.pll[o.pll].vcoActual / 1e6, d.a, d.b, d.c, o.divBy4 ? " (div4)" : "",
             1 << o.rdiv, act, act - f[i]);
  }
  return true;
}

static void show(uint32_t a, uint32_t b, uint32_t c, bool expectOk = true, uint32_t xtal = 25000000) {
  bool en[3] = {true, true, true};
  uint32_t f[3] = {a, b, c};
  Plan p;
  const char* err = makePlan(xtal, en, f, p);
  printf("[%u, %u, %u]%s\n", a, b, c, err ? " -> ERROR" : "");
  if (err) { printf("  %s\n", err); if (expectOk) fails++; return; }
  if (!expectOk) { printf("  expected failure but got a plan\n"); fails++; }
  double me = 0;
  if (!verify(p, en, f, me, true)) fails++;
}

int main() {
  // register encoding sanity: 600 MHz VCO = 24 * 25 MHz -> P1 = 128*24-512 = 2560
  uint32_t p1, p2, p3; Frac fr = {24, 0, 1}; fracToP(fr, p1, p2, p3);
  if (p1 != 2560 || p2 != 0 || p3 != 1) { printf("encode fail\n"); fails++; }
  Frac f2 = {36, 1, 3}; fracToP(f2, p1, p2, p3);  // 128*36 + floor(128/3)=42 -512 = 4138 ; p2 = 128 - 3*42 = 2
  if (p1 != 4138 || p2 != 2 || p3 != 3) { printf("encode fail 2\n"); fails++; }

  show(10000000, 1000000, 25000000);
  show(14074000, 7074000, 3573000);
  show(200000000, 150000000, 10000000);
  show(133333333, 7123456, 2500);
  show(160000000, 160000000, 100000000);
  show(112500000, 100000000, 32768);
  show(2500, 2500, 200000000);
  show(10000001, 10000003, 10000007);
  show(199999999, 123456789, 1000, false);     // 1000 Hz < min
  show(180000000, 130000000, 120000000);        // 180 & 120 share a 720 MHz VCO
  show(180000000, 130000000, 125000000, false); // 3 incompatible high outputs
  show(130110866, 108614000, 2768, false);      // low + near-limit outputs: impossible
  show(10000000, 1000000, 25000000, true, 27000000);

  // random sweep
  std::mt19937 rng(1);
  auto rnd = [&]() -> uint32_t {
    // log-uniform 2.5k..200M, sometimes round numbers
    double lf = log(2500.0) + (log(2e8) - log(2500.0)) * (rng() / 4294967296.0);
    uint32_t f = (uint32_t)exp(lf);
    if (rng() % 3 == 0) f = (f / 1000) * 1000;
    if (f < 2500) f = 2500;
    if (f > 200000000) f = 200000000;
    return f;
  };
  int ok = 0, nop = 0; double worst = 0; int exact = 0;
  for (int t = 0; t < 200000; t++) {
    bool en[3] = {rng() % 5 != 0, rng() % 5 != 0, rng() % 5 != 0};
    uint32_t f[3] = {rnd(), rnd(), rnd()};
    uint32_t xtal = 25000000 + (rng() % 2001) - 1000;
    Plan p;
    const char* err = makePlan(xtal, en, f, p);
    int high = 0, low = 0;
    for (int i = 0; i < 3; i++) { if (en[i] && f[i] > MS_FRAC_MAX_HZ) high++; if (en[i] && f[i] < 3500) low++; }
    if (err) {
      nop++;
      if (high < 2 && !low) {  // then a plan must always exist
        printf("unexpected failure: %u %u %u en %d%d%d xtal %u\n", f[0], f[1], f[2], en[0], en[1], en[2], xtal);
        fails++; if (fails > 10) break;
      }
      continue;
    }
    double me = 0;
    if (!verify(p, en, f, me, false)) { printf("verify fail %u %u %u\n", f[0], f[1], f[2]); fails++; if (fails > 10) break; }
    if (me > worst) { worst = me; }
    if (me < 1e-6) exact++;
    ok++;
  }
  printf("\nrandom: %d planned, %d rejected, worst error %.3e Hz, %d exact (<1uHz)\n", ok, nop, worst, exact);
  printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
