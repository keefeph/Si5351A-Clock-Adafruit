/*
  Si5351_Diag — hardware check for the Si5351A web clock (NodeMCU / ESP8266)

  Upload, open Serial Monitor at 115200 baud, press the board's RST button and read the report.
  It checks: why the board last reset, supply voltage, the I2C bus (Si5351A + OLED),
  Wi-Fi (opens access point "Si5351-Test", page at http://192.168.4.1), and the encoder.
  Needs the same "U8g2" library as the main sketch.
*/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>

ADC_MODE(ADC_VCC);   // lets ESP.getVcc() read the 3.3 V supply (A0 is unused here)

const int SDA_PIN = 4, SCL_PIN = 5;            // D2, D1
const int ENC_A = 14, ENC_B = 12, ENC_SW = 13; // D5, D6, D7

#define OLED_SH1106 1   // 1 = SH1106 (1.3"), 0 = SSD1306 (0.96"); text shifted ~2 px left = wrong setting
#if OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);
#endif
ESP8266WebServer server(80);
String report;
bool haveOled = false;

void out(const String& s) { Serial.println(s); report += s + "\n"; }

bool present(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

String scan(int sda, int scl) {
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  String found;
  for (uint8_t a = 1; a < 127; a++) if (present(a)) { found += " 0x"; found += String(a, HEX); }
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  out("\n===== Si5351 hardware check =====");
  out("Reset reason : " + ESP.getResetReason());
  out("Supply (3V3) : " + String(ESP.getVcc() / 1000.0, 2) + " V   (should be 3.0 - 3.6 V)");
  out("Free heap    : " + String(ESP.getFreeHeap()) + " bytes");

  // I2C lines should idle HIGH (pulled up). LOW = short, missing power on a module, or wrong pin.
  pinMode(SDA_PIN, INPUT); pinMode(SCL_PIN, INPUT);
  delay(5);
  int sdaIdle = digitalRead(SDA_PIN), sclIdle = digitalRead(SCL_PIN);
  out(String("I2C idle     : SDA(D2)=") + (sdaIdle ? "HIGH ok" : "LOW  <-- problem") +
      "  SCL(D1)=" + (sclIdle ? "HIGH ok" : "LOW  <-- problem"));

  String f = scan(SDA_PIN, SCL_PIN);
  out("I2C devices  :" + (f.length() ? f : String(" none")));
  bool si = present(0x60);
  bool o3c = present(0x3C), o3d = present(0x3D);
  out(String("  Si5351A 0x60: ") + (si ? "FOUND" : "missing"));
  out(String("  OLED 0x3C/3D: ") + (o3c ? "FOUND at 0x3C" : o3d ? "FOUND at 0x3D" : "missing"));
  if (!si && !o3c && !o3d) {
    String s = scan(SCL_PIN, SDA_PIN);
    if (s.length()) out("  >>> Devices answer with D1/D2 SWAPPED:" + s + "  -> swap the SDA and SCL wires");
    Wire.begin(SDA_PIN, SCL_PIN);
  }

  if (o3c || o3d) {
    oled.setI2CAddress((o3c ? 0x3C : 0x3D) << 1);
    oled.begin();
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 10, "Display test OK");
    oled.drawStr(0, 24, si ? "Si5351A found" : "Si5351A MISSING");
    oled.sendBuffer();
    haveOled = true;
    out("  OLED shows a test message now. If text is shifted ~2 px or garbled, flip OLED_SH1106.");
  }

  pinMode(ENC_A, INPUT_PULLUP); pinMode(ENC_B, INPUT_PULLUP); pinMode(ENC_SW, INPUT_PULLUP);
  out(String("Encoder idle : CLK(D5)=") + digitalRead(ENC_A) + " DT(D6)=" + digitalRead(ENC_B) +
      " SW(D7)=" + digitalRead(ENC_SW) + "   (SW should be 1 when not pressed)");

  WiFi.mode(WIFI_AP);
  bool ap = WiFi.softAP("Si5351-Test");
  out(String("Wi-Fi AP     : ") + (ap ? "\"Si5351-Test\" (no password) -> http://192.168.4.1" : "FAILED"));
  server.on("/", [] { server.send(200, "text/plain", report); });
  server.begin();
  out("Now turn the encoder and press it; changes are printed below.");
  out("If this banner keeps repeating, the board is resetting: check power.");
}

void loop() {
  server.handleClient();
  static int la = -1, lb = -1, ls = -1;
  static long steps = 0;
  static uint32_t beat = 0;
  int a = digitalRead(ENC_A), b = digitalRead(ENC_B), s = digitalRead(ENC_SW);
  if (a != la || b != lb) {
    if (la >= 0) { steps++; Serial.printf("encoder CLK=%d DT=%d  (changes: %ld)\n", a, b, steps); }
    la = a; lb = b;
  }
  static long shown = -1;
  static int shownS = -1;
  if (s != ls) {
    if (ls >= 0) Serial.println(s ? "button released" : "button PRESSED");
    ls = s;
  }
  if (haveOled && (steps != shown || s != shownS)) {   // live encoder panel on lines 3-5
    shown = steps; shownS = s;
    char t[32];
    oled.setDrawColor(0); oled.drawBox(0, 28, 128, 36); oled.setDrawColor(1);
    snprintf(t, sizeof t, "CLK(D5)=%d  DT(D6)=%d", a, b);
    oled.drawStr(0, 38, t);
    snprintf(t, sizeof t, "changes: %ld", steps);
    oled.drawStr(0, 50, t);
    oled.drawStr(0, 62, s ? "button: up" : "button: PRESSED");
    oled.sendBuffer();
  }
  if (millis() - beat > 5000) {
    beat = millis();
    Serial.printf("alive  %lus  supply %.2f V  AP clients %d\n", (unsigned long)(millis() / 1000),
                  ESP.getVcc() / 1000.0, WiFi.softAPgetStationNum());
  }
}
