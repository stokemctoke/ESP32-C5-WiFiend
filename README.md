[![Ko-Fi](https://img.shields.io/badge/Ko--Fi-Support%20Me-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/stoke)
[![My Website](https://img.shields.io/badge/Website-stokemctoke.com-FAA307)](https://stokemctoke.com)
[![Platform: ESP32-C5](https://img.shields.io/badge/Platform-ESP32--C5-blue)](https://www.espressif.com/en/products/socs/esp32-c5)

# ESP32-C5 WiFiend

![image](https://github.com/stokemctoke/ESP32-C5-WiFiend/blob/master/WiFiend_Github-Banner.png)

> **⚠️ Early Development — Work in Progress**
> This project is in active early-stage development. Hardware is on its first working perfboard prototype, core firmware is functional but WiFi features are stubbed. Expect breaking changes between commits. Not ready for general use.

An interactive, menu-driven WiFi hacking handheld built on the XIAO ESP32-C5. Navigate modes with a rotary encoder, monitor status on a 0.96" OLED, and control everything from a compact perfboard build powered by a 3.7V LiPo.

Built from scratch in ESP-IDF C. Dual-band WiFi 6 (802.11ax) with a deauth frame injection engine, channel scanner, AP mode, and STA connect — all menu-driven from a single rotary encoder.

---

## Hardware

| Part | Detail |
|------|--------|
| MCU | Seeed Studio XIAO ESP32-C5 |
| Display | 0.96" SSD1306 OLED, 128×64, I2C (blue/yellow split) |
| Input | EC11 rotary encoder — rotate to scroll, click to select, long-press to go back |
| Status LED | WS2812B NeoPixel (×1) |
| Power | 3.7V LiPo + TP4056 charger + slide switch *(v2 hardware)* |

### Pin Configuration

| Function | GPIO | Notes |
|----------|------|-------|
| Encoder CLK | GPIO9 | RC filtered (100Ω + 10–100nF ceramic) |
| Encoder DT | GPIO10 | RC filtered (100Ω + 10–100nF ceramic) |
| Encoder SW | GPIO7 | Pull-up input |
| NeoPixel | GPIO8 | RMT peripheral |
| OLED SDA | GPIO23 | I2C, 400kHz |
| OLED SCL | GPIO24 | I2C, 400kHz |

---

## Interaction

| Action | Result |
|--------|--------|
| Rotate CW | Scroll down |
| Rotate CCW | Scroll up |
| Click (< 500ms) | Select |
| Long press (≥ 500ms) | Go back |

---

## Display Layout

The 0.96" SSD1306 has a yellow/blue physical colour split. The firmware uses this deliberately:

```
┌──────────────────┐  ← yellow zone (pages 0–1, top 16px)
│ WiFiend      USB │    title left, battery/power right
│                  │    contextual status line
├──────────────────┤  ← blue zone (pages 2–7, bottom 48px)
│ > WiFi Scanner   │
│   AP Mode        │    menu items / screen content
│   Deauth Attack  │
│   STA Connect    │
│   Device Info    │
└──────────────────┘
```

NeoPixel colour per mode: green = idle, yellow = scanning in progress, cyan = scanner results/detail, red = deauth, magenta = STA connect.

---

## Build

### Requirements

- ESP-IDF v5.5.1 (tested) or v6.0 (target)
- ESP32-C5 target
- Patched `libnet80211.a` (included in `patched_libnet/`) — required for deauth frame injection

### Compile & Flash

```bash
# Source ESP-IDF (adjust path to your install)
. ~/Github-Repos/ESP32-Firmwares/ESP-IDF/ESP-IDF-5.5.1/export.sh

cd ESP32-C5_WiFiend
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

---

## Progress

### Done
- [x] Project scaffold — ESP-IDF v5.5.1, ESP32-C5 target, patched libnet80211.a wired in
- [x] SSD1306 OLED driver — dirty-page framebuffer, new I2C master API
- [x] OLED header layout — yellow zone: title + power indicator + contextual status; blue zone: content
- [x] WS2812B NeoPixel — led_strip v3 RMT driver, brightness control, hardware wired on perfboard
- [x] Battery ADC — firmware only (GPIO4, oneshot API, LiPo voltage → percentage); hardware on v2
- [x] Menu system — 4-level stack, wrap-around navigation, header-aware rendering
- [x] EC11 rotary encoder — PCNT quadrature decoding, 10µs glitch filter, polled SW with 20ms debounce
- [x] Boot splash — custom WiFiend graphic, fullscreen bitmap on boot
- [x] First hardware prototype — perfboard v1 built and working (XIAO, OLED, encoder, NeoPixel, WS2812B SMD)
- [x] **WiFi Scanner** — full active scan across all channels; animated spinner during scan; results sorted by RSSI; scrollable list showing SSID (hidden networks labelled), RSSI, auth mode; encoder-driven scroll with position indicator; AP detail screen: full SSID, BSSID (OUI/NIC split), band (2.4/5GHz), channel, RSSI, auth mode, pairwise cipher, PHY modes (b/g/n/ax), WPS flag
- [x] **Client Sniffer** — promiscuous mode 802.11 frame sniffing; auto channel-hops 2.4GHz 1–13 (500ms dwell); captures clients from probe requests, association frames, and data frames (ToDS); scrollable client list showing last 3 MAC bytes, RSSI, associated (A) or probe-only (P); detail screen: full client MAC, RSSI, channel, frame count, associated AP BSSID

### Upcoming
- [ ] WiFi Client Sniffer 5GHz — extend channel hop table to include 5GHz channels
- [ ] WiFi Scanner channel bar chart — 2.4GHz bar graph (encoder scrolls), 5GHz text summary
- [x] **AP Mode (Evil Twin) + Captive Portal** — SSID picker clones any nearby AP; open network on same channel; auto-scans if no results; live client screen shows IP, channel, connected MACs with [NEW] tag and age. DNS hijacker on UDP/53 resolves all hostnames to the device. HTTP server serves a polished login page mimicking iOS/Android system captive portals. Submitted passwords are captured, logged to serial, and displayed prominently on the OLED. Long-press stops AP and restores STA mode.
- [ ] STA Connect — connect to scanned AP, display IP/status
- [x] **Deauth Attack** — AP picker from last scan results; broadcast deauth frame injection via patched libnet80211 + esp_wifi_80211_tx; live stats screen showing target SSID/BSSID, channel, frames sent, pps rate, elapsed time; long-press to stop
- [ ] Battery power system — TP4056 charger, slide switch, LiPo (v2 hardware build)
- [ ] RC filter — 100Ω + 10–100nF ceramic caps on encoder CLK/DT (v2 hardware)

### Future Ideas
- [ ] 3D printed enclosure
- [ ] PCB layout for v3
- [ ] Multi-screen dashboard — TCA9548A I2C mux driving up to 8 × SSD1306 OLEDs simultaneously. Each screen showing a different data view: 2.4GHz channel chart, 5GHz channel chart, AP list, deauth status, device stats. ~5 full refreshes/sec across all screens at 400kHz I2C. 8KB framebuffer total — trivial on C5. Would set this apart from every other WiFi tool out there.

---

## Firmware Structure

```
main/
├── main.c          — boot sequence, encoder event handler, menu callbacks
├── ssd1306.c/h     — OLED driver (dirty-page framebuffer, I2C, header rendering)
├── encoder.c/h     — EC11 rotary encoder (PCNT quadrature, polled SW debounce)
├── neopixel.c/h    — WS2812B status LED (RMT)
├── battery.c/h     — LiPo ADC reading + percentage
├── menu.c/h        — menu stack, navigation, rendering
├── wifi_scan.c/h   — WiFi scanner (active scan, animated spinner, AP detail view)
├── wifi_ap.c/h     — AP mode (stub)
├── wifi_sta.c/h    — STA connect (stub)
├── wifi_attack.c/h — deauth engine (stub)
└── boot_bitmap.h   — splash screen bitmap
patched_libnet/
└── libnet80211.a   — patched WiFi lib for raw frame TX (deauth support)
```

---

## Legal

> Deauthentication attacks are illegal against networks you do not own or have explicit written permission to test. Laws vary by jurisdiction. Use WiFiend only on your own equipment or in authorised penetration testing environments. The author accepts no responsibility for misuse.
