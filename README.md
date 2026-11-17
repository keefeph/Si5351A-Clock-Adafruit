# Si5351A Web Clock Generator (NodeMCU V3 / ESP8266)

A web page, served by the NodeMCU, that sets CLK0, CLK1 and CLK2 of an Si5351A board to any frequency from **2.5 kHz to 200 MHz**. Settings are saved in flash and restored at power-up.

## Wiring

| NodeMCU V3        | Si5351A board |
|-------------------|---------------|
| 3V3               | VIN / 3V3     |
| GND               | GND           |
| D2 (GPIO4)        | SDA           |
| D1 (GPIO5)        | SCL           |

Most breakouts (Adafruit and the common purple clones) already include I²C pull-ups. Use short leads, and terminate the RF outputs into 50 Ω if you need clean waveforms at high frequency.

## Build and upload

1. In the Arduino IDE, open **Preferences → Additional Boards Manager URLs** and add
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`. Then install **esp8266** from the Boards Manager.
2. Open `Si5351_WebClock/Si5351_WebClock.ino` and select **NodeMCU 1.0 (ESP-12E Module)**.
3. Optional: set `WIFI_SSID` and `WIFI_PASS` at the top of the sketch. If you leave them empty, or the connection fails, the board starts its own access point: **Si5351-Clock**, password `si5351clk`, at http://192.168.4.1.
4. Upload, then open Serial Monitor at 115200 baud to see the IP address. On your own network you can also use **http://si5351.local**.

No extra libraries are needed. The Si5351 driver is included.

## Using the page

- Each clock card has an on/off switch, a frequency box (Hz/kHz/MHz), a drive-strength setting (2–8 mA) and quick presets.
- Each card shows the **frequency actually produced**, its error from your target, which PLL it uses, and the divider mode (integer / fractional / divide-by-4).
- **Calibration:** set CLK0 to 10 MHz and measure it with a frequency counter. Then enter `25000000 × measured / 10000000` as the crystal frequency and click **Save**.

## How the chip's limits are handled (`si5351_plan.h`)

| Constraint | Value |
|---|---|
| Output range | 2.5 kHz – 200 MHz |
| PLL VCO | 600–900 MHz, two PLLs (A, B) shared by three outputs |
| MultiSynth divider | 8–2048 fractional, or exactly 4 or 6 |
| Above 112.5 MHz | integer ÷6 (112.5–150 MHz) or ÷4 (150–200 MHz), so the VCO must equal exactly 4× or 6× the output |
| Below 500 kHz | R divider ÷2 … ÷128 is used |

The planner tries every way of assigning the enabled outputs to PLL A and B. For each PLL it picks a VCO that is an even multiple of one output, so that output gets a low-jitter integer divider; the other outputs on that PLL use fractional dividers. It keeps the arrangement with the smallest total error. Most combinations come out exact. In a 200,000-case random test, the worst error was 0.07 Hz. That worst case is an output above 112.5 MHz, whose fixed integer divider leaves only the PLL fraction to set the frequency.

Combinations the chip physically can't make are rejected with a message, and the outputs are left unchanged. Examples:
- three different outputs above 112.5 MHz that can't share a VCO;
- an output below about 3.4 kHz together with two outputs that each need a PLL above 870 MHz.

When a change moves a PLL, outputs on that PLL glitch briefly.

## HTTP API

```
GET  /api/state                       -> JSON with settings, actual frequencies, PLL/divider details
GET|POST /api/set?f0=10000000&en0=1   -> apply; returns the same JSON, or {"ok":false,"error":...}
```
Parameters (send any subset): `f0 f1 f2` (Hz), `en0 en1 en2` (0/1), `d0 d1 d2` (drive mA: 2/4/6/8), `xtal` (Hz), `load` (6/8/10 pF).

```
curl "http://si5351.local/api/set?f0=14074000&f1=7074000&en1=1&f2=125000000&en2=1"
```

## Files

- `Si5351_WebClock.ino`: Wi-Fi, web server, settings storage
- `si5351_plan.h`: frequency planner and register encoding (plain C++, tested on PC)
- `si5351_hw.h`: I²C register driver
- `web_page.h`: the web UI
- `test/test_plan.cpp`: planner tests (`g++ -O2 -I../Si5351_WebClock test_plan.cpp && ./a.out`)
