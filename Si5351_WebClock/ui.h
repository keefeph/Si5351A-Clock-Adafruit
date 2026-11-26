// ui.h — front panel: I2C OLED (SSD1306 / SH1106, 128x64) + KY-040 rotary encoder.
//
// Included from Si5351_WebClock.ino AFTER the globals it uses:
//   cfg, plan, planOk, si, netIp, cfgRev, commitSettings(), HW_FAIL,
//   PIN_SDA, PIN_SCL, PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, ENC_STEPS_PER_DETENT, ENC_REVERSE, OLED_SH1106
//
// Controls
//   Main screen : turn = select CLK0/1/2   press = edit clock   hold = output on/off
//   Edit screen : turn = tune by step       press = next step (1 Hz .. 100 MHz)   hold = back
#pragma once
#include <U8g2lib.h>

namespace ui {

#if OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#endif

bool haveOled = false;

// ---------------- rotary encoder (interrupt-driven quadrature decoder) ----------------
volatile int16_t encRaw = 0;
volatile uint8_t encPrev = 0;
const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR encIsr() {
  uint8_t s = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encPrev = (uint8_t)(((encPrev << 2) | s) & 0x0F);
  encRaw += QDEC[encPrev];
}

int takeDetents() {
  noInterrupts();
  int16_t r = encRaw;
  int d = r / ENC_STEPS_PER_DETENT;
  encRaw = (int16_t)(r - d * ENC_STEPS_PER_DETENT);
  interrupts();
  return ENC_REVERSE ? -d : d;
}

// ---------------- push button (debounced, short / long) ----------------
enum Btn { NONE, SHORT, LONG };
bool btnRawLast = false, btnDown = false, longFired = false;
uint32_t btnChange = 0, btnT = 0;

Btn readButton() {
  bool pressed = digitalRead(PIN_ENC_SW) == LOW;
  uint32_t now = millis();
  if (pressed != btnRawLast) { btnRawLast = pressed; btnChange = now; }
  if (now - btnChange < 25) return NONE;
  if (pressed && !btnDown) { btnDown = true; btnT = now; longFired = false; }
  else if (pressed && btnDown && !longFired && now - btnT >= 700) { longFired = true; return LONG; }
  else if (!pressed && btnDown) { btnDown = false; if (!longFired) return SHORT; }
  return NONE;
}

// ---------------- state ----------------
enum Screen { MAIN, EDIT };
Screen screen = MAIN;
uint8_t sel = 0;
uint8_t stepExp = 3;                       // 10^3 Hz = 1 kHz
int32_t pending = 0;                       // detents not yet applied
uint32_t lastApply = 0, lastDraw = 0, seenRev = 0xFFFFFFFF, toastUntil = 0;
bool dirty = true;
const char* toastMsg = "";

const uint32_t POW10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
const char* const STEP_NAME[9] = {"1 Hz", "10 Hz", "100 Hz", "1 kHz", "10 kHz",
                                  "100 kHz", "1 MHz", "10 MHz", "100 MHz"};

void showToast(const char* m) { toastMsg = m; toastUntil = millis() + 1800; dirty = true; }

void applyTune() {
  if (!pending) return;
  Settings s = cfg;
  int64_t f = (int64_t)s.clk[sel].freq + (int64_t)pending * POW10[stepExp];
  pending = 0;
  if (f < (int64_t)si5351plan::OUT_MIN_HZ) f = si5351plan::OUT_MIN_HZ;
  if (f > (int64_t)si5351plan::OUT_MAX_HZ) f = si5351plan::OUT_MAX_HZ;
  if ((uint32_t)f == s.clk[sel].freq && s.clk[sel].en) return;
  s.clk[sel].freq = (uint32_t)f;
  s.clk[sel].en = 1;                       // tuning a clock switches it on
  const char* err = commitSettings(s, false);
  if (err == HW_FAIL) showToast("Si5351 not\nresponding");
  else if (err) showToast("Not possible with\nthe other clocks");
}

// ---------------- drawing ----------------
void fmtMHz(char* b, size_t n, uint32_t f) {
  snprintf(b, n, "%lu.%06lu", (unsigned long)(f / 1000000UL), (unsigned long)(f % 1000000UL));
}

void drawToast() {
  oled.setFont(u8g2_font_6x10_tf);
  oled.setDrawColor(0); oled.drawBox(6, 18, 116, 30);
  oled.setDrawColor(1); oled.drawFrame(6, 18, 116, 30);
  char line[24]; const char* p = toastMsg; int y = 30;
  while (*p && y <= 44) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 18) len = 18;
    memcpy(line, p, len); line[len] = 0;
    oled.drawStr(64 - (int)len * 3, y, line);
    y += 12;
    if (!nl) break;
    p = nl + 1;
  }
}

void drawMain() {
  char b[32], fm[16];
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 8, "Si5351A");
  const char* st = si.isReady() ? "chip OK" : "NO CHIP";
  oled.drawStr(128 - (int)strlen(st) * 6, 8, st);
  oled.drawHLine(0, 10, 128);
  for (int i = 0; i < 3; i++) {
    int top = 12 + i * 14;
    fmtMHz(fm, sizeof fm, cfg.clk[i].freq);
    snprintf(b, sizeof b, "CLK%d%11s MHz", i, fm);
    bool on = cfg.clk[i].en && planOk;
    if (i == sel) { oled.setDrawColor(1); oled.drawBox(0, top, 128, 13); oled.setDrawColor(0); }
    oled.drawStr(1, top + 10, b);
    if (on) oled.drawDisc(123, top + 6, 3); else oled.drawCircle(123, top + 6, 3);
    oled.setDrawColor(1);
  }
  oled.drawStr(0, 63, netIp.c_str());
}

void drawEdit() {
  char b[32];
  const ClockCfg& c = cfg.clk[sel];
  const si5351plan::OutPlan& o = plan.out[sel];
  bool on = c.en && planOk;

  oled.setFont(u8g2_font_6x10_tf);
  snprintf(b, sizeof b, "CLK%d %s", sel, on ? "ON" : "OFF");
  oled.drawStr(0, 8, b);
  if (on) {
    snprintf(b, sizeof b, "PLL %c %s", o.pll ? 'B' : 'A',
             o.divBy4 ? "div4" : (o.ms.b == 0 ? "int" : "frac"));
    oled.drawStr(128 - (int)strlen(b) * 6, 8, b);
  }
  oled.drawHLine(0, 10, 128);

  // frequency in Hz, grouped "200,000,000", padded so the step digit always exists
  char digits[12];
  int nd = snprintf(digits, sizeof digits, "%lu", (unsigned long)c.freq);
  char padded[12];
  int need = stepExp + 1 > nd ? stepExp + 1 : nd;
  memset(padded, '0', need - nd);
  memcpy(padded + (need - nd), digits, nd + 1);
  char grp[16]; int gi = 0;
  for (int i = 0; i < need; i++) {
    grp[gi++] = padded[i];
    int left = need - 1 - i;
    if (left > 0 && left % 3 == 0) grp[gi++] = ',';
  }
  grp[gi] = 0;

  const int right = 114, cw = 10;
  oled.setFont(u8g2_font_10x20_tf);
  oled.drawStr(right - gi * cw, 34, grp);
  int pos = stepExp + stepExp / 3;                       // chars from the right
  oled.drawBox(right - (pos + 1) * cw, 37, cw - 1, 2);   // step cursor
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(116, 34, "Hz");

  snprintf(b, sizeof b, "step %s", STEP_NAME[stepExp]);
  oled.drawStr(0, 50, b);
  if (on) {
    double e = o.actual - (double)c.freq;
    if (fabs(e) < 0.0005) strcpy(b, "exact");
    else snprintf(b, sizeof b, "%+.3fHz", e);
    oled.drawStr(128 - (int)strlen(b) * 6, 50, b);
  }
  oled.drawStr(0, 63, "push:step hold:back");
}

void draw() {
  oled.clearBuffer();
  if (screen == MAIN) drawMain(); else drawEdit();
  if (toastUntil) drawToast();
  oled.sendBuffer();
}

// ---------------- public ----------------
void splash(const char* l1, const char* l2) {
  if (!haveOled) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 8, "Si5351A Clock");
  oled.drawHLine(0, 10, 128);
  oled.drawStr(0, 30, l1);
  oled.drawStr(0, 44, l2);
  oled.sendBuffer();
}

void begin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);

  const uint8_t addrs[2] = {0x3C, 0x3D};
  for (uint8_t a : addrs) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      oled.setI2CAddress(a << 1);
      oled.setBusClock(400000);
      oled.begin();
      haveOled = true;
      break;
    }
  }
}

void poll() {
  uint32_t now = millis();
  int d = takeDetents();
  Btn btn = readButton();

  if (screen == MAIN) {
    if (d) { int v = ((int)sel + d) % 3; if (v < 0) v += 3; sel = (uint8_t)v; dirty = true; }
    if (btn == SHORT) { screen = EDIT; dirty = true; }
    else if (btn == LONG) {
      Settings s = cfg;
      s.clk[sel].en = !s.clk[sel].en;
      const char* err = commitSettings(s, true);
      if (err == HW_FAIL) showToast("Si5351 not\nresponding");
      else if (err) showToast("Not possible with\nthe other clocks");
      dirty = true;
    }
  } else {
    if (d) { pending += d; dirty = true; }
    if (pending && now - lastApply >= 50) { applyTune(); lastApply = now; }
    if (btn == SHORT) { stepExp = (uint8_t)((stepExp + 1) % 9); dirty = true; }
    else if (btn == LONG) { applyTune(); screen = MAIN; dirty = true; }
  }

  if (cfgRev != seenRev) { seenRev = cfgRev; dirty = true; }
  if (toastUntil && (int32_t)(now - toastUntil) >= 0) { toastUntil = 0; dirty = true; }
  if (now - lastDraw >= 2000) dirty = true;            // refresh status / IP
  if (dirty && haveOled && now - lastDraw >= 40) { draw(); dirty = false; lastDraw = now; }
}

}  // namespace ui
