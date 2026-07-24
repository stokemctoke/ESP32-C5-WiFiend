# WiFiend Xiao — session handoff

> **For AI agents:** Read this file at the start of every session before planning or editing code.

Last updated: **2026-07-24**

**Active branch:** `wifiend-xiao`

---

## Product line & branches

| Name | Branch | Hardware | Status |
|------|--------|----------|--------|
| **WiFiend Xiao** | `wifiend-xiao` | Seeed XIAO ESP32-C5 perfboard | **Active** |
| **WiFiend Dev** | `wifiend-dev` | DevKit / custom PCB, SD, GPS | Not started |

Do not add SD/GPS/wardriving/DevKit wiring on the Xiao branch.

---

## Locked pin map (do not change)

Source: `main/board/xiao_esp32c5.h` — Encoder 9/10/7, NeoPixel 8, OLED 23/24, LiPo 6/26.

---

## Build

- ESP-IDF v5.5.1, target `esp32c5`, 8 MB flash
- **Partitions (OTA):** 2×2 MB `ota_0`/`ota_1` + ~3.875 MB LittleFS (`partitions.csv`)
- **Binary size (2026-07-24):** ~1.58 MiB (~79% of 2 MB OTA slot)
- First flash after OTA partition change must erase/reflash fully (`idf.py -p PORT erase-flash flash`)
- Patched lib: `patched_libnet/libnet80211.a`

---

## Completed (plan Tracks 0–F)

### Track 0 / foundation
- [x] `board/xiao_esp32c5.h`
- [x] Battery GPIO6/26 (WiFuxx port)
- [x] `radio_mgr.c/h`
- [x] LittleFS file explorer (Settings → File Explorer)
- [x] PMKID/Handshake first-render lag fix (unconditional main-loop refresh)

### Track A
- [x] 5 GHz client sniffer hop
- [x] OUI vendor lookup (`oui_lookup.c`)
- [x] BLE 1.5 — company names, Apple Continuity, raw hex, RSSI sort
- [x] WebUI BLE Devices tab
- [x] WebUI OTA upload (System tab, `POST /ota`)

### Track B
- [x] `wifi_monitor` — Beacons / Probes / EAPOL / Activity (no GPS wardriving)

### Track C
- [x] WiFuxx deauth engine + profiles (Broadcast / Targeted / Disassoc / Probe Flood)
- [x] Evil twin DNS query log on OLED

### Track D
- [x] GATT Explorer, Notify Mon, BLE Spam, BadBLE HID (minimal), Adv Logger, NUS Link

### Track E
- [x] ESP-NOW Recon + 802.15.4 Sniff (RF / IoT menu)

### Track F
- [x] Serial CLI (`wifiend>` on UART0)
- [x] WiFi profiles NVS API
- [x] Settings screen (LED, hop dwell, legal ack, bursts)
- [x] Dual OTA partitions

### Deferred
- [ ] WiFiend Dev — SD, GPS, wardriving, LoRa/CC1101 (`wifiend-dev` branch)

---

## Menu map

- **WiFi:** Scan, Client Sniff, WiFi Monitor, AP Mode, Deauth, STA, PMKID, Handshake, Captures, WebUI, Ch Chart
- **Bluetooth:** Scanner, Classifier, Beacons, Hunter, GATT, Notify, Spam, BadBLE, Adv Logger, NUS
- **RF / IoT:** ESP-NOW Recon, 802.15.4 Sniff
- **Games:** Pong, Life, Reaction
- **Settings:** Device Info, Settings UI, File Explorer

---

## Session checklist

1. Read this file.
2. Scope = Xiao only (no SD/wardriving).
3. Do not change locked pins without approval.
4. `idf.py build` after substantive changes.
5. Update this file + README when finishing features.
6. Do not commit unless asked.
