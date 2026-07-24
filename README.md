[![Ko-Fi](https://img.shields.io/badge/Ko--Fi-Support%20Me-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/stoke)
[![My Website](https://img.shields.io/badge/Website-stokemctoke.com-FAA307)](https://stokemctoke.com)
[![Platform: ESP32-C5](https://img.shields.io/badge/Platform-ESP32--C5-blue)](https://www.espressif.com/en/products/socs/esp32-c5)

# WiFiend Xiao

![image](https://github.com/stokemctoke/ESP32-C5-WiFiend/blob/master/WiFiend_Github-Banner.png)

> **WiFiend Xiao** — branch `wifiend-xiao` / `master`. Seeed XIAO ESP32-C5 perfboard handheld (OLED + encoder + NeoPixel + LiPo).  
> **WiFiend Dev** — branch `wifiend-dev` (when ready). Bigger board: SD card, GPS, wardriving, extra GPIO.

> **⚠️ Active Development**
> The WiFi feature set is functional and field-tested on the perfboard prototype. Expect occasional breaking changes between commits.

An interactive, menu-driven WiFi pen-testing handheld built on the XIAO ESP32-C5. Navigate modes with a rotary encoder, monitor status on a 0.96" OLED, and control everything from a compact perfboard build powered by a 3.7V LiPo.

Built from scratch in ESP-IDF C. Dual-band WiFi 6 (802.11ax) with a deauth frame injection engine, channel scanner, client sniffer, evil-twin AP with captive portal, STA connect, PMKID capture, WPA handshake capture, and on-device capture management — all menu-driven from a single rotary encoder. Captures are written to LittleFS on the 8MB flash and exportable in hashcat-22000 format. A **Remote WebUI** serves a phone-friendly dashboard over its own soft-AP (or hold the XIAO **BOOT** button for 2 seconds), with GitHub-release OTA, BLE tools, RF/IoT recon, and a **Games** menu with persistent hi-scores.

---

## Hardware

| Part       | Detail                                                                         |
| ---------- | ------------------------------------------------------------------------------ |
| MCU        | Seeed Studio XIAO ESP32-C5                                                     |
| Display    | 0.96" SSD1306 OLED, 128×64, I2C (blue/yellow split)                            |
| Input      | EC11 rotary encoder — rotate to scroll, click to select, long-press to go back |
| Status LED | WS2812B NeoPixel (×1)                                                          |
| Power      | 3.7V LiPo on XIAO B+/B− pads (onboard SGM40567 charger)                        |

### Pin Configuration

| Function    | GPIO   | Notes                                           |
| ----------- | ------ | ----------------------------------------------- |
| Encoder CLK | GPIO9  | RC filtered (100Ω + 10–100nF ceramic)           |
| Encoder DT  | GPIO10 | RC filtered (100Ω + 10–100nF ceramic)           |
| Encoder SW  | GPIO7  | Pull-up input                                   |
| NeoPixel    | GPIO8  | RMT peripheral                                  |
| OLED SDA    | GPIO23 | I2C, 400kHz                                     |
| OLED SCL    | GPIO24 | I2C, 400kHz                                     |
| LiPo sense  | GPIO6  | Onboard XIAO divider (ADC1 ch5); enable GPIO26  |
| LiPo enable | GPIO26 | Drive HIGH during ADC sample (auto in firmware) |
| BOOT button | GPIO28 | Hold 2s → reboot into Remote WebUI              |

Pin map lives in [`main/board/xiao_esp32c5.h`](main/board/xiao_esp32c5.h).

---

## Interaction

| Action               | Result                                      |
| -------------------- | ------------------------------------------- |
| Rotate CW            | Scroll down                                 |
| Rotate CCW           | Scroll up                                   |
| Click (< 500ms)      | Select                                      |
| Long press (≥ 500ms) | Go back                                     |
| Hold BOOT 2s         | Reboot into Remote WebUI (`WiFiend-Remote`) |

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

The main menu is organised into categories. **WiFi** holds Scan, Client Sniff, Monitor, AP Mode, Deauth, STA Connect, PMKID, Handshake, Captures, Remote WebUI and Ch Chart; **Bluetooth** holds Scanner, Classifier, Beacons, Hunter, GATT, Notify, Spam, BadBLE, Adv Logger, and NUS; **RF / IoT** holds ESP-NOW and 802.15.4 sniff; **Games** holds Pong, Game of Life and Reaction Test; **Settings** holds Device Info, Settings, and File Explorer. Submenus show a "Long-press = Back" hint.

NeoPixel colour per mode: green = idle, yellow = WiFi scanning, cyan = results / captures view / WebUI, red = deauth, magenta = sniffer / STA connect / games, blue = BLE recon, and a rainbow cycle while Game of Life runs. Active scans (WiFi, sniffer, BLE) drive a smooth gamma-corrected "breathing" animation via a dedicated 40 Hz timer, so the LED visibly signals work in progress without coarse stepping.

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

First flash after a partition-table change needs a full erase:

```bash
idf.py -p /dev/ttyACM0 erase-flash flash
```

Or install a [GitHub Release](https://github.com/stokemctoke/ESP32-C5-WiFiend/releases) `WiFiend.bin` via the WebUI **Check for Update** flow (joins your home Wi-Fi and pulls the latest release) / manual upload.

---

## Progress

### Done

**Platform / Hardware**

- [x] Project scaffold — ESP-IDF v5.5.1, ESP32-C5 target, patched libnet80211.a wired in
- [x] SSD1306 OLED driver — dirty-page framebuffer, new I2C master API
- [x] OLED header layout — yellow zone: title + power indicator + contextual status; blue zone: content
- [x] WS2812B NeoPixel — led_strip v3 RMT driver, brightness control, hardware wired on perfboard
- [x] Battery ADC — XIAO onboard GPIO6/26 sense, LiPo charge curve (from WiFuxx), % on OLED + WebUI
- [x] Menu system — 4-level stack, wrap-around navigation, header-aware rendering
- [x] EC11 rotary encoder — PCNT quadrature decoding, 10µs glitch filter, polled SW with 20ms debounce
- [x] Boot splash — custom WiFiend graphic, fullscreen bitmap on boot
- [x] First hardware prototype — perfboard v1 built and working (XIAO, OLED, encoder, NeoPixel, WS2812B SMD)
- [x] **8MB flash + LittleFS** — dual 2 MB OTA slots (`ota_0` / `ota_1`) + ~3.875 MB LittleFS. Capture logs persist across power cycles.

**WiFi Reconnaissance**

- [x] **WiFi Scanner** — full active scan across all channels; animated spinner during scan; results sorted by RSSI; scrollable list showing SSID (hidden networks labelled), RSSI, auth mode; encoder-driven scroll with position indicator; AP detail screen: full SSID, BSSID (OUI/NIC split), band (2.4/5GHz), channel, RSSI, auth mode, pairwise cipher, PHY modes (b/g/n/ax), WPS flag
- [x] **Client Sniffer** — promiscuous mode 802.11 frame sniffing; auto channel-hops 2.4 GHz (ch 1–13) and 5 GHz (36–165, 400/300 ms dwell); captures clients from probe requests, association frames, and data frames (ToDS); scrollable client list showing last 3 MAC bytes, RSSI, associated (A) or probe-only (P); detail screen: full client MAC, RSSI, channel, frame count, associated AP BSSID
- [x] **Channel Chart** — 2.4GHz channel occupancy bar chart showing AP density per channel

**WiFi Attack**

- [x] **Deauth Attack** — AP picker from last scan; WiFuxx-style deauth engine (`deauth_engine.c`) with rolling sequence numbers, multi-reason codes, channel hopping, and dual-band burst rates (~2500 pps target); live stats (SSID/BSSID, channel, frames sent, pps, elapsed); long-press to stop
- [x] **AP Mode (Evil Twin) + Captive Portal** — SSID picker clones any nearby AP; open network on same channel; auto-scans if no results; live client screen shows IP, channel, connected MACs with [NEW] tag and age. DNS hijacker on UDP/53 resolves all hostnames to the device. HTTP server serves a polished login page mimicking iOS/Android system captive portals. Submitted passwords are captured, logged to serial, and displayed prominently on the OLED. Long-press stops AP and restores STA mode.
- [x] **STA Connect** — connect to a scanned AP using a stored or entered passphrase; live status with IP, RSSI, gateway

**WiFi Cracking Captures**

- [x] **PMKID Capture** — forged auth + association request elicits EAPOL M1 from the target AP; RSN IE PMKID extraction; result saved as a hashcat-22000 line (`WPA*02*…`) to `/lfs/pmkid.log`; 30-attempt task with live progress; deduplicated by PMKID hash
- [x] **WPA Handshake Capture** — passive listen for EAPOL M1+M2 between real clients and a target AP; pairs by replay counter; reconstructs the M2 EAPOL frame with MIC zeroed; saves a hashcat-22000 line (`WPA*02*MIC*…*ANONCE*EAPOL*MP`) to `/lfs/handshakes.log`. CLICK during hunting fires a 48-frame deauth burst (alternating broadcast + targeted) to force re-authentication. Header shows live `M1:x M2:y` counters.
- [x] **Captures Menu** — on-device viewer for all saved PMKID and handshake captures; parses each hashcat line for SSID + BSSID + client MAC. Per-entry detail sheet with **Dump to Serial** (single capture's hashcat line via USB-Serial-JTAG CDC, ready to paste into hashcat) and **Delete this** (rewrites log in place). Bulk actions: **Dump All Serial** and **Clear All** (with confirmation).

**Remote WebUI**

- [x] **Remote WebUI dashboard** — dedicated `WiFiend-Remote` soft-AP serves a phone-friendly single-page web app (WebSocket-driven) at 192.168.4.1. Hold **BOOT 2s** for the same entry path as WiFuxx. Tabs: **Scan**, **Capture**, **Tools**, **BLE**, **System**. WiFi scan briefly uses APSTA so the SoftAP stays up; BLE scan warms NimBLE under SoftAP.
- [x] **GitHub OTA** — System → Check for Update saves home Wi-Fi creds, reboots into STA, queries `releases/latest`, and installs `WiFiend.bin` when newer than `version.txt`. Manual `.bin` upload still available.

**Games**

- [x] **Pong** — encoder-controlled paddle vs. a CPU that ramps from clumsy to ruthless; score = win streak, with the CPU starting tougher each win. Ball speeds up over a round; play field kept in the blue zone.
- [x] **Conway's Game of Life** — 64×32 toroidal grid; encoder-dialled deterministic seed (digit-by-digit entry); self-reseeds if the colony dies; NeoPixel rainbow cycle while running.
- [x] **Reaction Test** — NeoPixel flashes a colour; match the shuffled colour-word and click within the level window. 3 lives, window tightens 0.1s per level; faster = more points.
- [x] **Persistent hi-scores** — shared NVS-backed top-10 table (arcade 3-letter names, scrollable) for Pong and Reaction; survives reboots and reflashes.

**Bluetooth (BLE — Phase 1: read-only recon)**

- [x] **BLE Scanner** — NimBLE observer-mode active GAP discovery; deduped result table sorted by RSSI; per-device detail with MAC, address type, company name, Continuity label, raw adv hex
- [x] **Device Classifier** — type labels from company ID + service UUIDs + appearance + Apple Continuity
- [x] **Beacons** — filtered iBeacon + Eddystone view with full payload decode
- [x] **Device Hunter** — RSSI proximity locator with NeoPixel ramp
- [x] **Wi-Fi / BLE coexistence** — NimBLE concurrent with Wi-Fi STA

**Bluetooth (BLE — Phase 2: active)**

- [x] **GATT Explorer** — connect, enumerate services + characteristics
- [x] **Notify Monitor** — subscribe to first notifiable characteristic, live hex
- [x] **BLE Spam / Beacon TX** — Apple Continuity, Fast Pair, iBeacon, Eddystone-URL
- [x] **BadBLE HID** — minimal keyboard peripheral with NVS payload
- [x] **Advertisement Logger** — `/lfs/ble.log` with rotation
- [x] **NUS Link** — DUMP / STAT over Nordic UART Service

**WiFi Monitor / RF**

- [x] **WiFi Monitor** — Beacons / Probes / EAPOL counters / channel activity (LittleFS logs; no GPS wardriving)
- [x] **ESP-NOW Recon** — peer discovery list
- [x] **802.15.4 Sniff** — frame-type / PAN counters (WiFi stopped while active)

**Platform**

- [x] **`radio_mgr`** — exclusive radio-mode arbitration
- [x] **OUI vendor lookup** — WiFi scan + sniffer detail
- [x] **LittleFS file explorer** — Settings → list / dump / delete
- [x] **Settings screen** — LED brightness, hop dwell, legal ack, burst sizes (NVS)
- [x] **Serial CLI** — `wifiend>` on UART0 (scan, battery, heap, mode, ls, cat, deauth stop)
- [x] **WebUI OTA** — dual 2 MB OTA partitions; GitHub check + manual upload
- [x] **WebUI BLE tab** — live BLE device list from phone browser
- [x] **BOOT → WebUI** — GPIO28 hold 2s reboots into Remote WebUI
- [x] **PMKID / Handshake render refresh** — main-loop redraw while active

**Polish**

- [x] **Smooth NeoPixel breathing**
- [x] **Client Sniffer auto-refresh** + dual-band hop

**Other**

- [x] **Device Info**

### Upcoming / polish leftovers

- [ ] Font-match the WebUI to stokemctoke.com
- [ ] GATT Device Name (0x2A00) resolver for unnamed BLE devices
- [ ] WiFi STA profile picker UI (NVS API exists)

### Future Ideas (WiFiend Xiao)

- [ ] 3D printed enclosure
- [ ] RC filter / PCB polish on current XIAO build

### WiFiend Dev (branch `wifiend-dev` — not started)

- [ ] ESP32-C5-DevKitC-1 or custom PCB with SD card (SPI), GPS (UART), extra GPIO
- [ ] Wardriving — GPS + SSID/BSSID/RSSI/channel/auth logging at drive scale
- [ ] Multi-screen dashboard — TCA9548A I2C mux + multiple SSD1306 OLEDs

---

## Firmware Structure

```
main/
├── main.c             — boot sequence, encoder event handler, menu callbacks,
│                        LittleFS mount, mode dispatch
├── board/xiao_esp32c5.h — locked XIAO pin map (WiFiend Xiao)
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
├── deauth_engine.c/h  — shared 802.11 deauth TX core (WiFuxx attack loop)
├── wifi_attack.c/h    — deauth UI (picker + stats)
├── wifi_pmkid.c/h     — PMKID capture (auth/assoc forge → EAPOL M1 → RSN IE PMKID)
├── wifi_handshake.c/h — WPA 4-way handshake capture (M1+M2 pairing, optional deauth)
├── wifi_captures.c/h  — on-device viewer + per-entry detail sheet + serial dump
├── captive_portal.c/h — DNS hijacker + HTTP login page for evil-twin
├── captures_http.c    — HTTP serving of capture logs
├── wifi_webui.c/h     — Remote WebUI dashboard (soft-AP, HTTP + WebSocket)
├── webui_html.h       — embedded single-page web app (HTML/CSS/JS)
├── ota_update.c/h     — streamed OTA write helpers (manual upload)
├── ota_github.c/h     — GitHub releases/latest check + esp_https_ota
├── boot_mode.c/h      — RTC boot destination (WebUI / OTA)
├── version.txt        — PROJECT_VER / OTA semver compare
├── ble_core.c/h       — NimBLE host bring-up (init, sync, host task)
├── ble_scan.c/h       — shared GAP discovery + result table + Scanner UI
├── ble_ident.c/h      — pure helpers: device classifier + iBeacon/Eddystone decoders
├── ble_class.c/h      — Classifier view (scan list + type labels)
├── ble_beacon.c/h     — Beacon decoder view
├── ble_hunt.c/h       — Device Hunter (RSSI proximity, NeoPixel ramp)
├── game_pong.c/h      — Pong (win-streak hi-score)
├── game_life.c/h      — Conway's Game of Life (seeded, rainbow LED)
├── game_react.c/h     — Reaction Test (lives, shrinking window, hi-score)
├── hiscore.c/h        — shared NVS-backed top-10 hi-score table
└── boot_bitmap.h      — splash screen bitmap
patched_libnet/
└── libnet80211.a      — patched WiFi lib for raw frame TX (deauth + assoc forge)
partitions.csv         — 8MB OTA layout: 2×2MB ota_0/ota_1 + ~3.875MB LittleFS
```

---

## Legal

> Deauthentication attacks are illegal against networks you do not own or have explicit written permission to test. Laws vary by jurisdiction. Use WiFiend only on your own equipment or in authorised penetration testing environments. The author accepts no responsibility for misuse.
