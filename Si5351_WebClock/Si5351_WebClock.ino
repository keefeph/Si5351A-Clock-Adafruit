/*
  Si5351A three-output clock generator with web control
  Board : NodeMCU V3 / NodeMCU 1.0 (ESP-12E, ESP8266)
  Chip  : Si5351A breakout (I2C address 0x60, 25 MHz crystal)

  Wiring (NodeMCU -> Si5351A board)
    3V3 -> VIN (or 3V3)      GND -> GND
    D2 (GPIO4) -> SDA        D1 (GPIO5) -> SCL

  Arduino IDE: install "esp8266" board package, select "NodeMCU 1.0 (ESP-12E Module)".
  No extra libraries needed.

  Web UI : http://si5351.local  (or the IP printed on Serial at 115200)
  API    : GET  /api/state
           POST/GET /api/set   any of: f0 f1 f2 (Hz)  en0 en1 en2 (0/1)
                               d0 d1 d2 (drive mA: 2/4/6/8)  xtal (Hz)  load (6/8/10 pF)
           e.g.  curl "http://si5351.local/api/set?f0=10000000&en0=1"
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <Wire.h>
#include <stdlib.h>
#include "si5351_plan.h"
#include "si5351_hw.h"
#include "web_page.h"

// ------------------------- user configuration -------------------------
const char* WIFI_SSID = "";              // your Wi-Fi; leave "" to always run as access point
const char* WIFI_PASS = "";
const char* AP_SSID   = "Si5351-Clock";  // fallback access point (http://192.168.4.1)
const char* AP_PASS   = "si5351clk";     // min 8 characters
const char* HOSTNAME  = "si5351";        // -> http://si5351.local
const int   PIN_SDA   = 4;               // D2
const int   PIN_SCL   = 5;               // D1
// ---------------------------------------------------------------------

using namespace si5351plan;

struct ClockCfg { uint8_t en; uint8_t drive; uint32_t freq; };
struct Settings {
  uint32_t magic;
  uint32_t xtal;
  uint8_t  load;
  ClockCfg clk[3];
};
const uint32_t MAGIC = 0x53353102;   // bump to reset stored settings

Settings  cfg;
Plan      plan;
bool      planOk = false;
Si5351Hw  si;
ESP8266WebServer server(80);
String    netInfo;

void defaults(Settings& s) {
  s.magic = MAGIC;
  s.xtal  = 25000000;
  s.load  = 10;
  s.clk[0] = {1, 8, 10000000};
  s.clk[1] = {0, 8, 1000000};
  s.clk[2] = {0, 8, 25000000};
}

void loadSettings() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);
  bool valid = cfg.magic == MAGIC && cfg.xtal >= XTAL_MIN_HZ && cfg.xtal <= XTAL_MAX_HZ;
  for (int i = 0; i < 3 && valid; i++)
    valid = cfg.clk[i].freq >= OUT_MIN_HZ && cfg.clk[i].freq <= OUT_MAX_HZ;
  if (!valid) defaults(cfg);
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

const char* buildPlan(const Settings& s, Plan& p) {
  bool en[3]; uint32_t f[3];
  for (int i = 0; i < 3; i++) { en[i] = s.clk[i].en; f[i] = s.clk[i].freq; }
  return makePlan(s.xtal, en, f, p);
}

bool applyHardware(const Settings& s, const Plan& p, bool loadChanged) {
  if (!si.isReady() || loadChanged) {
    if (!si.begin(s.load)) return false;
  }
  uint8_t drv[3] = {s.clk[0].drive, s.clk[1].drive, s.clk[2].drive};
  return si.apply(p, drv);
}

// ----------------------------- JSON -----------------------------------
String fracStr(const Frac& f) {
  String s(f.a);
  if (f.b) { s += " + "; s += f.b; s += "/"; s += f.c; }
  return s;
}

String stateJson() {
  String j;
  j.reserve(1100);
  j += F("{\"ok\":true,\"chip\":");
  j += si.isReady() ? "true" : "false";
  j += F(",\"net\":\""); j += netInfo;
  j += F("\",\"xtal\":"); j += cfg.xtal;
  j += F(",\"load\":"); j += cfg.load;
  j += F(",\"min\":"); j += OUT_MIN_HZ;
  j += F(",\"max\":"); j += OUT_MAX_HZ;
  j += F(",\"pll\":[");
  for (int g = 0; g < 2; g++) {
    const PllPlan& p = plan.pll[g];
    if (g) j += ',';
    j += F("{\"used\":"); j += (planOk && p.used) ? "true" : "false";
    j += F(",\"vco\":"); j += String(p.vcoActual, 3);
    j += F(",\"mult\":\""); j += fracStr(p.mult); j += F("\"}");
  }
  j += F("],\"clk\":[");
  for (int i = 0; i < 3; i++) {
    const OutPlan& o = plan.out[i];
    bool on = planOk && cfg.clk[i].en;
    if (i) j += ',';
    j += F("{\"en\":"); j += on ? "true" : "false";
    j += F(",\"f\":"); j += cfg.clk[i].freq;
    j += F(",\"drive\":"); j += cfg.clk[i].drive;
    j += F(",\"actual\":"); j += on ? String(o.actual, 3) : String("0");
    j += F(",\"pll\":\""); j += (o.pll ? 'B' : 'A');
    j += F("\",\"ms\":\""); j += fracStr(o.ms);
    j += F("\",\"r\":"); j += (1u << o.rdiv);
    j += F(",\"mode\":\"");
    j += o.divBy4 ? F("divide-by-4") : (o.ms.b == 0 ? F("integer") : F("fractional"));
    j += F("\"}");
  }
  j += F("]}");
  return j;
}

void sendError(int code, const String& msg) {
  String j = F("{\"ok\":false,\"error\":\"");
  j += msg;
  j += F("\"}");
  server.send(code, "application/json", j);
}

// --------------------------- handlers ---------------------------------
bool argNumber(const String& name, double& out) {
  if (!server.hasArg(name)) return false;
  String v = server.arg(name);
  v.trim();
  char* end;
  out = strtod(v.c_str(), &end);
  return end != v.c_str() && isfinite(out);
}

void handleSet() {
  Settings s = cfg;
  double v;
  for (int i = 0; i < 3; i++) {
    String n(i);
    if (server.hasArg(String("f") + n)) {
      if (!argNumber(String("f") + n, v) || v < OUT_MIN_HZ - 0.5 || v > OUT_MAX_HZ + 0.5)
        return sendError(400, String("CLK") + n + ": frequency must be 2500 Hz to 200000000 Hz");
      s.clk[i].freq = (uint32_t)(v + 0.5);
    }
    if (argNumber(String("en") + n, v)) s.clk[i].en = v != 0;
    if (argNumber(String("d") + n, v)) {
      int ma = (int)v;
      if (ma != 2 && ma != 4 && ma != 6 && ma != 8) return sendError(400, "Drive must be 2, 4, 6 or 8 mA");
      s.clk[i].drive = (uint8_t)ma;
    }
  }
  if (server.hasArg("xtal")) {
    if (!argNumber("xtal", v) || v < XTAL_MIN_HZ || v > XTAL_MAX_HZ)
      return sendError(400, "Crystal frequency must be 24000000 to 28000000 Hz");
    s.xtal = (uint32_t)(v + 0.5);
  }
  if (argNumber("load", v)) {
    int pf = (int)v;
    if (pf != 6 && pf != 8 && pf != 10) return sendError(400, "Load must be 6, 8 or 10 pF");
    s.load = (uint8_t)pf;
  }

  Plan p;
  const char* err = buildPlan(s, p);
  if (err) return sendError(400, err);

  bool loadChanged = s.load != cfg.load;
  cfg = s; plan = p; planOk = true;
  saveSettings();
  if (!applyHardware(cfg, plan, loadChanged))
    return sendError(500, "Settings saved, but the Si5351 did not respond on I2C. Check wiring/power.");
  server.send(200, "application/json", stateJson());
}

// ----------------------------- setup ----------------------------------
void startWifi() {
  WiFi.persistent(false);
  if (strlen(WIFI_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to %s", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(250); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      netInfo = String(WIFI_SSID) + " · " + WiFi.localIP().toString();
      Serial.printf("Connected, IP %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("Wi-Fi failed, starting access point");
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  netInfo = String("AP ") + AP_SSID + " · " + WiFi.softAPIP().toString();
  Serial.printf("Access point \"%s\" (pass \"%s\"), IP %s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\nSi5351A web clock"));

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  loadSettings();

  const char* err = buildPlan(cfg, plan);
  if (err) {                                // stored settings unusable -> defaults
    Serial.println(err);
    defaults(cfg);
    err = buildPlan(cfg, plan);
  }
  planOk = (err == nullptr);
  if (planOk && applyHardware(cfg, plan, true)) Serial.println(F("Si5351 configured"));
  else Serial.println(F("Si5351 not found on I2C (0x60) - check wiring"));

  startWifi();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, [] {
    if (!si.isReady() && planOk) applyHardware(cfg, plan, true);   // retry if chip was missing
    server.send(200, "application/json", stateJson());
  });
  server.on("/api/set", handleSet);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println(F("Web server started"));
}

void loop() {
  server.handleClient();
  MDNS.update();
}
