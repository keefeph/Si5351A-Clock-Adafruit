// si5351_plan.h — frequency planner for the Si5351A (3-output, crystal input)
//
// Pure C++ (no Arduino dependencies) so it can be unit-tested on a PC.
//
// Signal path per output:  XTAL -> PLL (A or B, 600..900 MHz VCO)
//                               -> MultiSynth divider -> R divider (1..128) -> CLKx
//
// Chip limits enforced here (Silicon Labs Si5351 datasheet / AN619):
//   * Output frequency          : 2.5 kHz .. 200 MHz
//   * PLL VCO                   : 600 .. 900 MHz, feedback ratio a + b/c, c <= 1048575
//   * MultiSynth divider        : 8 .. 2048 (fractional), or exactly 4 or 6 (integer)
//                                 -> outputs above 112.5 MHz need an integer divider (4 or 6)
//                                    and therefore a VCO dedicated to that frequency
//   * R divider                 : 1, 2, 4 .. 128  (used for outputs below 500 kHz)
//   * Only two PLLs exist, so three outputs must share them.
//
// Strategy: try every assignment of enabled outputs to PLL A/B. For each PLL pick a VCO
// that is an even-integer multiple of one of its outputs (integer MultiSynth = lowest jitter);
// the other outputs on that PLL use fractional MultiSynth dividers. The assignment with
// the smallest total frequency error (then the most integer dividers) wins.

#pragma once
#include <stdint.h>
#include <math.h>

namespace si5351plan {

const uint32_t OUT_MIN_HZ     = 2500UL;
const uint32_t OUT_MAX_HZ     = 200000000UL;
const uint32_t VCO_MIN_HZ     = 600000000UL;
const uint32_t VCO_MAX_HZ     = 900000000UL;
const uint32_t MS_FRAC_MAX_HZ = 112500000UL;  // VCO_MAX / 8
const uint32_t MS_IN_MIN_HZ   = 500000UL;     // engage R divider below this
const uint32_t DEN_MAX        = 1048575UL;    // 20-bit denominator
const uint32_t XTAL_MIN_HZ    = 24000000UL;   // allow calibrated 25/27 MHz crystals
const uint32_t XTAL_MAX_HZ    = 28000000UL;

struct Frac { uint32_t a, b, c; };            // a + b/c

struct OutPlan {
  bool     enabled;
  uint32_t target;    // requested Hz
  uint8_t  pll;       // 0 = PLL A, 1 = PLL B
  uint8_t  rdiv;      // log2 of R divider (0..7)
  Frac     ms;        // MultiSynth divider
  bool     divBy4;    // special divide-by-4 mode (150..200 MHz)
  bool     msInt;     // MS_INT bit (even integer divider)
  double   actual;    // Hz actually produced (given the crystal value)
};

struct PllPlan {
  bool     used;
  uint32_t vcoTarget; // Hz
  Frac     mult;      // VCO / XTAL
  double   vcoActual; // Hz
};

struct Plan {
  uint32_t xtal;
  PllPlan  pll[2];
  OutPlan  out[3];
};

static inline uint64_t gcd64(uint64_t a, uint64_t b) {
  while (b) { uint64_t t = a % b; a = b; b = t; }
  return a;
}

// Best rational approximation of num/den as a + b/c with c <= DEN_MAX
// (continued fractions with semiconvergent step).
static inline Frac approx(uint64_t num, uint64_t den) {
  Frac f;
  f.a = (uint32_t)(num / den);
  uint64_t rem = num % den;
  if (rem == 0) { f.b = 0; f.c = 1; return f; }
  uint64_t g = gcd64(rem, den);
  uint64_t n = rem / g, d = den / g;
  if (d <= DEN_MAX) { f.b = (uint32_t)n; f.c = (uint32_t)d; return f; }

  uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
  uint64_t x = n, y = d;
  while (y) {
    uint64_t t = x / y;
    if (q1 && t > (DEN_MAX - q0) / q1) {
      uint64_t k  = (DEN_MAX - q0) / q1;
      uint64_t ps = p0 + k * p1, qs = q0 + k * q1;
      double target = (double)n / (double)d;
      double e1 = fabs((double)p1 / (double)q1 - target);
      double e2 = fabs((double)ps / (double)qs - target);
      if (k > 0 && e2 < e1) { p1 = ps; q1 = qs; }
      break;
    }
    uint64_t p2 = p0 + t * p1, q2 = q0 + t * q1;
    p0 = p1; q0 = q1; p1 = p2; q1 = q2;
    uint64_t r = x % y; x = y; y = r;
  }
  if (p1 >= q1)      { f.a += 1; f.b = 0; f.c = 1; }
  else if (p1 == 0)  { f.b = 0; f.c = 1; }
  else               { f.b = (uint32_t)p1; f.c = (uint32_t)q1; }
  return f;
}

static inline double fracValue(const Frac& f) {
  return (double)f.a + (double)f.b / (double)f.c;
}

// Smallest R divider that lifts the MultiSynth output to >= 500 kHz.
static inline uint8_t chooseRdiv(uint32_t f) {
  uint8_t r = 0;
  while (r < 7 && ((uint64_t)f << r) < MS_IN_MIN_HZ) r++;
  return r;
}

// Can a MultiSynth produce fms from this VCO?
static inline bool msOk(uint32_t vco, uint32_t fms) {
  uint64_t v = vco;
  if (fms > MS_FRAC_MAX_HZ) return v == 4ULL * fms || v == 6ULL * fms;
  return v >= 8ULL * fms && v <= 2048ULL * fms;
}

static inline uint32_t gcd32(uint32_t a, uint32_t b) {
  while (b) { uint32_t t = a % b; a = b; b = t; }
  return a;
}

static inline long scoreVco(uint32_t v, const uint32_t* fms, int n, uint32_t xtal) {
  if (v < VCO_MIN_HZ || v > VCO_MAX_HZ) return -1;
  long score = 0;
  for (int k = 0; k < n; k++) {
    if (!msOk(v, fms[k])) return -1;
    if (v % fms[k] == 0) score += 100;               // integer MultiSynth
  }
  if (xtal / gcd32(v, xtal) <= DEN_MAX) score += 10;   // PLL ratio exactly representable
  if (v % xtal == 0) score += 1;                      // integer PLL
  return score;
}

// Choose a VCO frequency that serves every output in the group.
static inline bool pickVco(const uint32_t* fms, int n, uint32_t xtal, uint32_t& best) {
  long bestScore = -1;
  best = 0;
  uint64_t lo = VCO_MIN_HZ, hi = VCO_MAX_HZ;   // common window for fallback
  for (int m = 0; m < n; m++) {
    uint32_t f = fms[m];
    uint32_t dmin, dmax;
    if (f > MS_FRAC_MAX_HZ) { dmin = 4; dmax = 6; }
    else {
      dmin = (VCO_MIN_HZ + f - 1) / f;
      if (dmin < 8) dmin = 8;
      if (dmin & 1) dmin++;
      dmax = VCO_MAX_HZ / f;
      if (dmax > 2048) dmax = 2048;
      if (8ULL * f > lo) lo = 8ULL * f;
      if (2048ULL * f < hi) hi = 2048ULL * f;
    }
    for (uint32_t d = dmin; d <= dmax; d += 2) {
      uint64_t v64 = (uint64_t)d * f;
      if (v64 < VCO_MIN_HZ || v64 > VCO_MAX_HZ) continue;
      uint32_t v = (uint32_t)v64;
      long s = scoreVco(v, fms, n, xtal);
      if (s > bestScore || (s == bestScore && s >= 0 && v > best)) { bestScore = s; best = v; }
    }
  }
  if (bestScore < 0 && lo <= hi) {             // no even-integer candidate: all fractional
    uint32_t v = (uint32_t)((lo + hi) / 2);
    long s = scoreVco(v, fms, n, xtal);
    if (s >= 0) { bestScore = s; best = v; }
  }
  return bestScore >= 0;
}

static inline void setPll(PllPlan& p, uint32_t vco, uint32_t xtal) {
  p.used = true;
  p.vcoTarget = vco;
  p.mult = approx(vco, xtal);
  p.vcoActual = (double)xtal * fracValue(p.mult);
}

static inline bool buildMs(const PllPlan& pll, uint32_t xtal, uint32_t fms, OutPlan& o) {
  uint32_t v = pll.vcoTarget;
  if (fms > MS_FRAC_MAX_HZ || v % fms == 0) {
    uint32_t d = v / fms;
    o.ms.a = d; o.ms.b = 0; o.ms.c = 1;
    o.divBy4 = (d == 4);
    o.msInt  = (d % 2 == 0);
  } else {
    uint64_t num = (uint64_t)xtal * ((uint64_t)pll.mult.a * pll.mult.c + pll.mult.b);
    uint64_t den = (uint64_t)pll.mult.c * fms;
    o.ms = approx(num, den);
    o.divBy4 = false;
    o.msInt  = (o.ms.b == 0 && (o.ms.a % 2) == 0);
    if (o.ms.a < 8 || o.ms.a > 2048 || (o.ms.a == 2048 && o.ms.b)) return false;
  }
  o.actual = pll.vcoActual / fracValue(o.ms) / (double)(1u << o.rdiv);
  return true;
}

// Returns nullptr on success, otherwise a human-readable error.
static inline const char* makePlan(uint32_t xtal, const bool en[3], const uint32_t freq[3], Plan& out) {
  if (xtal < XTAL_MIN_HZ || xtal > XTAL_MAX_HZ) return "Crystal frequency must be 24-28 MHz";

  uint32_t fms[3] = {0, 0, 0};
  uint8_t  rdiv[3] = {0, 0, 0};
  int idx[3], n = 0;
  for (int i = 0; i < 3; i++) {
    if (!en[i]) continue;
    if (freq[i] < OUT_MIN_HZ || freq[i] > OUT_MAX_HZ) return "Frequency must be 2.5 kHz - 200 MHz";
    rdiv[i] = chooseRdiv(freq[i]);
    fms[i]  = freq[i] << rdiv[i];
    idx[n++] = i;
  }

  Plan best;
  bool have = false;
  double bestErr = 0;
  int bestInt = -1;
  int combos = n ? (1 << (n - 1)) : 1;   // first enabled output pinned to PLL A

  for (int mask = 0; mask < combos; mask++) {
    Plan p;
    p.xtal = xtal;
    for (int g = 0; g < 2; g++) { p.pll[g].used = false; p.pll[g].vcoTarget = 0;
      p.pll[g].mult.a = 0; p.pll[g].mult.b = 0; p.pll[g].mult.c = 1; p.pll[g].vcoActual = 0; }
    for (int i = 0; i < 3; i++) {
      OutPlan& o = p.out[i];
      o.enabled = en[i]; o.target = freq[i]; o.pll = 0; o.rdiv = rdiv[i];
      o.ms.a = 0; o.ms.b = 0; o.ms.c = 1; o.divBy4 = false; o.msInt = false; o.actual = 0;
    }

    uint32_t gf[2][3];
    int gn[2] = {0, 0};
    for (int k = 0; k < n; k++) {
      int g = (k == 0) ? 0 : ((mask >> (k - 1)) & 1);
      gf[g][gn[g]++] = fms[idx[k]];
      p.out[idx[k]].pll = (uint8_t)g;
    }

    bool ok = true;
    for (int g = 0; g < 2 && ok; g++) {
      if (!gn[g]) continue;
      uint32_t v;
      if (!pickVco(gf[g], gn[g], xtal, v)) ok = false;
      else setPll(p.pll[g], v, xtal);
    }
    if (!ok) continue;

    double err = 0;
    int ints = 0;
    for (int k = 0; k < n && ok; k++) {
      OutPlan& o = p.out[idx[k]];
      if (!buildMs(p.pll[o.pll], xtal, fms[idx[k]], o)) { ok = false; break; }
      err += fabs(o.actual - (double)o.target);
      if (o.ms.b == 0) ints++;
    }
    if (!ok) continue;

    if (!have || err < bestErr - 1e-6 || (fabs(err - bestErr) <= 1e-6 && ints > bestInt)) {
      best = p; bestErr = err; bestInt = ints; have = true;
    }
  }

  if (!have)
    return "This combination can't be generated together. The chip has only 2 PLLs (600-900 MHz VCO): "
           "each output above 112.5 MHz needs a VCO of exactly 4x or 6x its frequency, and outputs "
           "below ~3.4 kHz can't share a PLL with outputs above ~100 MHz. Change or disable one clock.";
  out = best;
  return nullptr;
}

// ---- register encoding (AN619) ----

static inline void fracToP(const Frac& f, uint32_t& p1, uint32_t& p2, uint32_t& p3) {
  uint32_t fl = (uint32_t)((128ULL * f.b) / f.c);
  p1 = 128UL * f.a + fl - 512UL;
  p2 = 128UL * f.b - f.c * fl;
  p3 = f.c;
}

// 8-byte parameter block; byte2Hi carries R_DIV / DIVBY4 bits for MultiSynth blocks.
static inline void packRegs(uint8_t* r, uint32_t p1, uint32_t p2, uint32_t p3, uint8_t byte2Hi) {
  r[0] = (uint8_t)(p3 >> 8);
  r[1] = (uint8_t)(p3);
  r[2] = (uint8_t)(byte2Hi | ((p1 >> 16) & 0x03));
  r[3] = (uint8_t)(p1 >> 8);
  r[4] = (uint8_t)(p1);
  r[5] = (uint8_t)(((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F));
  r[6] = (uint8_t)(p2 >> 8);
  r[7] = (uint8_t)(p2);
}

}  // namespace si5351plan
