// si5351_hw.h — minimal Si5351A register driver (I2C via Wire), applies a Plan.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "si5351_plan.h"

class Si5351Hw {
public:
  static const uint8_t ADDR = 0x60;

  // loadPf: crystal load capacitance 6, 8 or 10 pF
  bool begin(uint8_t loadPf) {
    pllValid[0] = pllValid[1] = false;
    ready = false;
    if (!probe()) return false;

    uint32_t t0 = millis();                       // wait for SYS_INIT to clear
    while (read(0) & 0x80) { if (millis() - t0 > 500) break; delay(5); }

    bool ok = write(3, 0xFF);                     // all outputs disabled
    for (uint8_t r = 16; r <= 23; r++) ok &= write(r, 0x80);   // power down drivers
    ok &= write(15, 0x00);                        // both PLLs from XTAL
    ok &= write(149, 0x00);                       // spread spectrum off
    for (uint8_t r = 165; r <= 167; r++) ok &= write(r, 0x00); // phase offsets 0
    ok &= setLoad(loadPf);
    ready = ok;
    return ok;
  }

  bool probe() {
    Wire.beginTransmission(ADDR);
    return Wire.endTransmission() == 0;
  }

  bool setLoad(uint8_t pf) {
    uint8_t code = (pf == 6) ? 1 : (pf == 8) ? 2 : 3;
    return write(183, (uint8_t)((code << 6) | 0x12));
  }

  // driveMa: 2, 4, 6 or 8 mA per output
  bool apply(const si5351plan::Plan& p, const uint8_t driveMa[3]) {
    using namespace si5351plan;
    bool ok = true;
    uint8_t resetMask = 0;
    uint8_t regs[8];
    uint32_t p1, p2, p3;

    // PLL A (reg 26) / PLL B (reg 34): write only when changed to avoid needless glitches
    for (int g = 0; g < 2; g++) {
      if (!p.pll[g].used) continue;
      fracToP(p.pll[g].mult, p1, p2, p3);
      packRegs(regs, p1, p2, p3, 0);
      if (!pllValid[g] || memcmp(regs, lastPll[g], 8) != 0) {
        ok &= writeBulk(26 + 8 * g, regs, 8);
        memcpy(lastPll[g], regs, 8);
        pllValid[g] = true;
        resetMask |= g ? 0x80 : 0x20;
      }
    }

    uint8_t oe = 0xFF;   // register 3: 1 = disabled
    for (int i = 0; i < 3; i++) {
      const OutPlan& o = p.out[i];
      if (!o.enabled) { ok &= write(16 + i, 0x80); continue; }
      if (o.divBy4) { p1 = 0; p2 = 0; p3 = 1; }
      else fracToP(o.ms, p1, p2, p3);
      packRegs(regs, p1, p2, p3, (uint8_t)((o.rdiv << 4) | (o.divBy4 ? 0x0C : 0x00)));
      ok &= writeBulk(42 + 8 * i, regs, 8);
      uint8_t ma  = driveMa[i];
      uint8_t drv = ma <= 2 ? 0 : ma <= 4 ? 1 : ma <= 6 ? 2 : 3;
      // CLKx_CTRL: MS_INT | MS_SRC | CLK_SRC = MultiSynth (0b11) | drive
      ok &= write(16 + i, (uint8_t)((o.msInt ? 0x40 : 0) | (o.pll ? 0x20 : 0) | 0x0C | drv));
      oe &= (uint8_t)~(1 << i);
    }

    if (resetMask) ok &= write(177, resetMask);   // PLL soft reset
    ok &= write(3, oe);

    if (!ok) { pllValid[0] = pllValid[1] = false; ready = false; }
    else ready = true;
    return ok;
  }

  bool isReady() const { return ready; }

private:
  bool ready = false;
  bool pllValid[2] = {false, false};
  uint8_t lastPll[2][8];

  bool write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
  }

  bool writeBulk(uint8_t reg, const uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(buf, len);
    return Wire.endTransmission() == 0;
  }

  uint8_t read(uint8_t reg) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    if (Wire.requestFrom((uint8_t)ADDR, (uint8_t)1) != 1) return 0xFF;
    return Wire.read();
  }
};
