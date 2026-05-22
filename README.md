[![Ko-Fi](https://img.shields.io/badge/Ko--Fi-Support%20Me-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/stoke)
[![My Website](https://img.shields.io/badge/Website-stokemctoke.com-FAA307)](https://stokemctoke.com)
[![Platform: ESP32-C5](https://img.shields.io/badge/Platform-ESP32--C5-blue)](https://www.espressif.com/en/products/socs/esp32-c5)

# ESP32-C5 WiFiend

![image](https://github.com/stokemctoke/ESP32-C5-WiFiend/blob/master/WiFiend_Github-Banner.png)

> **⚠️ Active Development**
> The WiFi feature set is functional and field-tested on the perfboard prototype. Polishing and transfer-method work in progress. Expect occasional breaking changes between commits. Not yet ready for general distribution.

An interactive, menu-driven WiFi pen-testing handheld built on the XIAO ESP32-C5. Navigate modes with a rotary encoder, monitor status on a 0.96" OLED, and control everything from a compact perfboard build powered by a 3.7V LiPo.

Built from scratch in ESP-IDF C. Dual-band WiFi 6 (802.11ax) with a deauth frame injection engine, channel scanner, client sniffer, evil-twin AP with captive portal, STA connect, PMKID capture, WPA handshake capture, and on-device capture management — all menu-driven from a single rotary encoder. Captures are written to LittleFS on the 8MB flash and exportable in hashcat-22000 format. A **Remote WebUI** serves a phone-friendly monitoring/scan/capture dashboard over its own soft-AP, and a **Games** menu (Pong, Conway's Game of Life, Reaction Test) with persistent hi-scores rounds out the build.

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
│ > WiFi           │
│   Bluetooth      │    top-level categories
│   Games          │
│   Settings       │
└──────────────────┘
```

The main menu is organised into categories. **WiFi** holds Scan, Client Sniff, AP Mode, Deauth, STA Connect, PMKID, Handshake, Captures, Remote WebUI and Ch Chart; **Games** holds Pong, Game of Life and Reaction Test; **Settings** holds Device Info; **Bluetooth** is a stub for the next initiative. Submenus show a "Long-press = Back" hint.

NeoPixel colour per mode: green = idle, yellow = scanning / hunting, cyan = results / captures view / WebUI, red = deauth, magenta = STA connect / games, and a rainbow cycle while Game of Life runs.

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

**Platform / Hardware**
- [x] Project scaffold — ESP-IDF v5.5.1, ESP32-C5 target, patched libnet80211.a wired in
- [x] SSD1306 OLED driver — dirty-page framebuffer, new I2C master API
- [x] OLED header layout — yellow zone: title + power indicator + contextual status; blue zone: content
- [x] WS2812B NeoPixel — led_strip v3 RMT driver, brightness control, hardware wired on perfboard
- [x] Battery ADC — firmware only (GPIO4, oneshot API, LiPo voltage → percentage); hardware on v2
- [x] Menu system — 4-level stack, wrap-around navigation, header-aware rendering
- [x] EC11 rotary encoder — PCNT quadrature decoding, 10µs glitch filter, polled SW with 20ms debounce
- [x] Boot splash — custom WiFiend graphic, fullscreen bitmap on boot
- [x] First hardware prototype — perfboard v1 built and working (XIAO, OLED, encoder, NeoPixel, WS2812B SMD)
- [x] **8MB flash + LittleFS** — custom partition table (3MB factory app + 4.9MB LittleFS storage). Capture logs persist across power cycles.

**WiFi Reconnaissance**
- [x] **WiFi Scanner** — full active scan across all channels; animated spinner during scan; results sorted by RSSI; scrollable list showing SSID (hidden networks labelled), RSSI, auth mode; encoder-driven scroll with position indicator; AP detail screen: full SSID, BSSID (OUI/NIC split), band (2.4/5GHz), channel, RSSI, auth mode, pairwise cipher, PHY modes (b/g/n/ax), WPS flag
- [x] **Client Sniffer** — promiscuous mode 802.11 frame sniffing; auto channel-hops 2.4GHz 1–13 (500ms dwell); captures clients from probe requests, association frames, and data frames (ToDS); scrollable client list showing last 3 MAC bytes, RSSI, associated (A) or probe-only (P); detail screen: full client MAC, RSSI, channel, frame count, associated AP BSSID
- [x] **Channel Chart** — 2.4GHz channel occupancy bar chart showing AP density per channel

**WiFi Attack**
- [x] **Deauth Attack** — AP picker from last scan results; broadcast deauth frame injection via patched libnet80211 + esp_wifi_80211_tx; live stats screen showing target SSID/BSSID, channel, frames sent, pps rate, elapsed time; long-press to stop
- [x] **AP Mode (Evil Twin) + Captive Portal** — SSID picker clones any nearby AP; open network on same channel; auto-scans if no results; live client screen shows IP, channel, connected MACs with [NEW] tag and age. DNS hijacker on UDP/53 resolves all hostnames to the device. HTTP server serves a polished login page mimicking iOS/Android system captive portals. Submitted passwords are captured, logged to serial, and displayed prominently on the OLED. Long-press stops AP and restores STA mode.
- [x] **STA Connect** — connect to a scanned AP using a stored or entered passphrase; live status with IP, RSSI, gateway

**WiFi Cracking Captures**
- [x] **PMKID Capture** — forged auth + association request elicits EAPOL M1 from the target AP; RSN IE PMKID extraction; result saved as a hashcat-22000 line (`WPA*02*…`) to `/lfs/pmkid.log`; 30-attempt task with live progress; deduplicated by PMKID hash
- [x] **WPA Handshake Capture** — passive listen for EAPOL M1+M2 between real clients and a target AP; pairs by replay counter; reconstructs the M2 EAPOL frame with MIC zeroed; saves a hashcat-22000 line (`WPA*02*MIC*…*ANONCE*EAPOL*MP`) to `/lfs/handshakes.log`. CLICK during hunting fires a 48-frame deauth burst (alternating broadcast + targeted) to force re-authentication. Header shows live `M1:x M2:y` counters.
- [x] **Captures Menu** — on-device viewer for all saved PMKID and handshake captures; parses each hashcat line for SSID + BSSID + client MAC. Per-entry detail sheet with **Dump to Serial** (single capture's hashcat line via USB-Serial-JTAG CDC, ready to paste into hashcat) and **Delete this** (rewrites log in place). Bulk actions: **Dump All Serial** and **Clear All** (with confirmation).

**Remote WebUI**
- [x] **Remote WebUI dashboard** — dedicated `WiFiend-Remote` soft-AP serves a phone-friendly single-page web app (WebSocket-driven) at 192.168.4.1. Branded to match stokemctoke.com colours, with a live activity-log panel showing exactly what the device is doing. Tabs: **Scan** (run/browse APs on a real screen), **Capture** (PMKID/Handshake hunts + view/download saved `.hc22000` files straight to the phone), **Tools** (client sniffer, STA connect), **System** (device info, downloads, exit). Active radio-commandeering attacks (deauth, clone AP) are intentionally device-only — they take over the single radio and would drop the web link — so the dashboard stays reliable.

**Games**
- [x] **Pong** — encoder-controlled paddle vs. a CPU that ramps from clumsy to ruthless; score = win streak, with the CPU starting tougher each win. Ball speeds up over a round; play field kept in the blue zone.
- [x] **Conway's Game of Life** — 64×32 toroidal grid; encoder-dialled deterministic seed (digit-by-digit entry); self-reseeds if the colony dies; NeoPixel rainbow cycle while running.
- [x] **Reaction Test** — NeoPixel flashes a colour; match the shuffled colour-word and click within the level window. 3 lives, window tightens 0.1s per level; faster = more points.
- [x] **Persistent hi-scores** — shared NVS-backed top-10 table (arcade 3-letter names, scrollable) for Pong and Reaction; survives reboots and reflashes.

**Other**
- [x] **Device Info** — stateful screen showing MAC, free heap, flash size, chip revision, IDF version, uptime, WiFi mode

### Upcoming

**Transfer methods** (reusable across all on-device capture features)
- [ ] BLE file transfer — Nordic UART Service streams captures to a phone; ties into wardriving phone-link plans

**Bluetooth menu** (next major initiative, after WiFi side is fully polished)
- [ ] Bluetooth submenu — BLE scan, BLE advertise/spam, BLE sniffer, phone-link mode

**WiFi polish**
- [ ] Fix Client Sniff click-to-render delay (rotary encoder twist needed for first render)
- [ ] WiFi Client Sniffer 5GHz — extend channel hop table to include 5GHz channels

**Hardware**
- [ ] Battery power system — TP4056 charger, slide switch, LiPo (v2 hardware build)
- [ ] RC filter — 100Ω + 10–100nF ceramic caps on encoder CLK/DT (v2 hardware)

### Future Ideas
- [ ] 3D printed enclosure
- [ ] PCB layout for v3 (will add SD card via SPI + GPS UART, freeing flash for app code)
- [ ] Wardriving mode — pair with phone over BLE for GPS coords; log SSID/BSSID/RSSI/channel/auth/GPS to LittleFS
- [ ] Multi-screen dashboard — TCA9548A I2C mux driving up to 8 × SSD1306 OLEDs simultaneously. Each screen showing a different data view: 2.4GHz channel chart, 5GHz channel chart, AP list, deauth status, device stats. ~5 full refreshes/sec across all screens at 400kHz I2C. 8KB framebuffer total — trivial on C5. Would set this apart from every other WiFi tool out there.

---

## Firmware Structure

```
main/
├── main.c             — boot sequence, encoder event handler, menu callbacks,
│                        LittleFS mount, mode dispatch
├── ssd1306.c/h        — OLED driver (dirty-page framebuffer, I2C, header rendering)
├── encoder.c/h        — EC11 rotary encoder (PCNT quadrature, polled SW debounce)
├── buttons.c/h        — generic button input helpers
├── neopixel.c/h       — WS2812B status LED (RMT)
├── battery.c/h        — LiPo ADC reading + percentage
├── menu.c/h           — menu stack, navigation, rendering
├── wifi_scan.c/h      — WiFi scanner (active scan, animated spinner, AP detail view)
├── wifi_sniffer.c/h   — promiscuous client sniffer, channel-hop
├── wifi_ap.c/h        — evil-twin AP mode (uses captive_portal for HTTP/DNS)
├── wifi_sta.c/h       — STA connect
├── wifi_attack.c/h    — deauth engine
├── wifi_pmkid.c/h     — PMKID capture (auth/assoc forge → EAPOL M1 → RSN IE PMKID)
├── wifi_handshake.c/h — WPA 4-way handshake capture (M1+M2 pairing, optional deauth)
├── wifi_captures.c/h  — on-device viewer + per-entry detail sheet + serial dump
├── captive_portal.c/h — DNS hijacker + HTTP login page for evil-twin
├── captures_http.c    — HTTP serving of capture logs
├── wifi_webui.c/h     — Remote WebUI dashboard (soft-AP, HTTP + WebSocket)
├── webui_html.h       — embedded single-page web app (HTML/CSS/JS)
├── game_pong.c/h      — Pong (win-streak hi-score)
├── game_life.c/h      — Conway's Game of Life (seeded, rainbow LED)
├── game_react.c/h     — Reaction Test (lives, shrinking window, hi-score)
├── hiscore.c/h        — shared NVS-backed top-10 hi-score table
└── boot_bitmap.h      — splash screen bitmap
patched_libnet/
└── libnet80211.a      — patched WiFi lib for raw frame TX (deauth + assoc forge)
partitions.csv         — custom 8MB layout: 3MB factory + 4.9MB LittleFS storage
```

---

## Legal

> Deauthentication attacks are illegal against networks you do not own or have explicit written permission to test. Laws vary by jurisdiction. Use WiFiend only on your own equipment or in authorised penetration testing environments. The author accepts no responsibility for misuse.
