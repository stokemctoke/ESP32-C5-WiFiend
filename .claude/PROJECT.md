# WiFiend Xiao — session handoff

> **For AI agents:** Read this file at the start of every session before planning or editing code.

Last updated: **2026-06-30**

**Active branch:** `wifiend-xiao` (created from `master`; not yet pushed)

---

## Product line & branches

Same repo, **two hardware lines** on separate git branches:

| Name | Branch | Hardware | Status |
|------|--------|----------|--------|
| **WiFiend Xiao** | `wifiend-xiao` | Seeed XIAO ESP32-C5 perfboard (OLED + EC11 + NeoPixel + LiPo) | **Active** — max out fixed wiring in software |
| **WiFiend Dev** | `wifiend-dev` | ESP32-C5-DevKitC-1 (or custom PCB), SD, GPS, extra GPIO | **Not started** — branch when Xiao is ready / Dev hardware exists |

**Current session:** work on `wifiend-xiao`. `master` may stay as release/default until branches are cut.

**When Dev starts:** create `wifiend-dev` from a stable Xiao point; shared modules (`deauth_engine`, BLE stack, WebUI patterns) merge/cherry-pick between branches as needed.

Do not add DevKit wiring, SD-card support, or GPS/wardriving on the **Xiao branch** unless the user explicitly rescopes.

---

## Out of scope on Xiao (→ WiFiend Dev)

- **SD card** (SPI module on free header pins — deferred)
- **Wardriving** (GPS + drive logging, CSV/PCAP bulk logs)
- **GPS UART**
- **LoRa / CC1101 / sub-GHz**
- **DevKit-specific pin maps**
- **Multi-screen I2C mux dashboard** (hardware-dependent)

Captures stay on **LittleFS** (~4.9 MB today; ~3.9 MB if/when dual 2 MB OTA partitions are added).

---

## Locked pin map (do not change)

Source of truth: `main/board/xiao_esp32c5.h`

| Function | GPIO | XIAO pin |
|----------|------|----------|
| Encoder CLK | 9 | D9 |
| Encoder DT | 10 | D10 |
| Encoder SW | 7 | D3 |
| NeoPixel | 8 | D8 |
| OLED SDA / SCL | 23 / 24 | D4 / D5 |
| LiPo ADC / enable | 6 / 26 | board pads |

**Free header pins (unused on perfboard):** D0=GPIO1, D1=GPIO0, D2=GPIO25, D6=GPIO11, D7=GPIO12 — reserved for WiFiend Dev (SD/GPS), not wired on Xiao prototype.

Hardware SPI (D8/D9/D10) is consumed by NeoPixel + encoder; do not assume hardware SPI is free.

---

## Build environment

- **Target:** `esp32c5`
- **ESP-IDF:** v5.5.1 — `/home/stoke/Github-Repos/ESP32-Firmwares/ESP-IDF/ESP-IDF-5.5.1`
- **Flash:** 8 MB (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`)
- **Partitions:** `partitions.csv` — 3 MB factory app + ~4.9 MB LittleFS (OTA not yet enabled)
- **Build:** `source …/export.sh && idf.py build`
- **Last known binary size:** ~1.46 MiB (`WiFiend.bin`, Jul 2026)

**Patched lib:** `patched_libnet/libnet80211.a` — required for raw deauth TX (linked in `main/CMakeLists.txt`).

---

## Architecture decisions

- **Modular ESP-IDF C** — one feature per `main/*.c`; no monolithic `main.c` like WiFuxx.
- **Port algorithms, not repos** — WiFuxx is reference only (`ESP32-C5_WiFuxx_Auto-Dualband-Deauth` at `/home/stoke/Github-Repos/ESP32-Firmwares/ESP32-C5_WiFuxx_Auto-Dualband-Deauth`). Do not submodule or link it.
- **WiFiend WebUI** stays separate from WiFuxx WebUI; attacks (deauth, evil twin) are device-only (single radio).
- **`deauth_engine.c/h`** — shared 802.11 TX core; `wifi_attack.c` and `wifi_handshake.c` are thin callers.
- **`radio_mgr.c/h`** — planned but not implemented; needed before `wifi_monitor`, ESP-NOW, 802.15.4 modes.

---

## Completed (recent)

- [x] **`main/board/xiao_esp32c5.h`** — locked pin map
- [x] **Battery** — GPIO6/26 from WiFuxx (ADC curve, EMA, auto-detect, `battery_tick()` in main loop)
- [x] **Deauth engine** — WiFuxx attack loop in `deauth_engine.c`; wired into `wifi_attack.c` + `wifi_handshake.c` burst
- [x] **5 GHz client sniffer hop** — dual-band channel table in `wifi_sniffer.c` (400/300 ms dwell)
- [x] Baseline (pre-session): dual-band scan, evil twin, PMKID/handshake, captures, Remote WebUI, BLE Phase 1, games

**Uncommitted in working tree (as of last session):** battery, deauth_engine, wifi_attack/handshake/sniffer refactors, board header, README updates.

---

## In scope — Xiao roadmap (priority order)

### Track 0 — foundation
1. **PMKID / Handshake first-render lag** — main-loop refresh like sniffer fix
2. **LittleFS file explorer** — Settings: list/delete/dump `/lfs/*`
3. **`radio_mgr.c/h`** — exclusive WiFi/BLE/monitor mode switching

### Track A — polish
- OUI vendor lookup (WiFi scan + sniffer detail)
- BLE 1.5 (company names, Apple Continuity, raw adv hex, sort-by-RSSI)
- WebUI BLE tab
- WebUI **OTA** (2×2 MB partitions — see below)

### Track B — WiFi recon (Xiao subset, **no wardriving**)
- `wifi_monitor` — beacon/probe/EAPOL parsers, on-device channel activity overlay
- Log to LittleFS only (small text); **no** CSV drive logs, **no** SD

### Track C — WiFi offense (remaining)
- Multi-target deauth rotation, disassoc, probe/auth flood
- Evil twin polish

### Tracks D–F
- BLE active (GATT, BadBLE, NUS, spam) — on-chip only
- ESP-NOW + 802.15.4 — after `radio_mgr`
- Serial CLI, saved WiFi profiles, Settings expansion

---

## OTA notes (planned, not implemented)

- Target: **2×2 MB OTA** slots + ~3.9 MB LittleFS on 8 MB flash
- Current app ~1.46 MB → ~548 KiB headroom per slot; adequate for Xiao roadmap if binary growth is watched
- Upload via Remote WebUI System tab; needs `partitions.csv` change + `esp_ota_ops` handler in `wifi_webui.c`

---

## Key files

| Path | Role |
|------|------|
| `main/main.c` | Boot, menu dispatch, mode callbacks |
| `main/board/xiao_esp32c5.h` | Pin map |
| `main/deauth_engine.c/h` | Deauth TX core |
| `main/wifi_sniffer.c/h` | Client sniffer + dual-band hop |
| `main/battery.c/h` | LiPo sense |
| `main/wifi_webui.c/h` + `webui_html.h` | Remote dashboard |
| `partitions.csv` | Flash layout |
| `README.md` | User-facing docs + feature checklist |

---

## External references

- **WiFuxx (port source):** `/home/stoke/Github-Repos/ESP32-Firmwares/ESP32-C5_WiFuxx_Auto-Dualband-Deauth`
- **Cursor plan (may drift):** `~/.cursor/plans/rf_tool_expansion_cd8fcf5a.plan.md`
- **XIAO wiki:** https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/

---

## Session checklist for agents

1. Read this file.
2. Confirm branch scope: **`wifiend-xiao`** — no SD/wardriving/DevKit unless user says otherwise.
3. Do not change locked pins without explicit user approval.
4. Match existing code style; minimal diffs.
5. Run `idf.py build` after substantive firmware changes.
6. Update **this file** and `README.md` when completing roadmap items or changing decisions.
7. Do not git commit unless the user asks.

---

## WiFiend Dev (future branch — do not implement on `wifiend-xiao`)

When `wifiend-dev` is created, expect:

- ESP32-C5-DevKitC-1 or custom PCB
- SD card on soft SPI (D0/D1/D2/D6 — documented in chat, not in Xiao firmware)
- GPS on UART (D6/D7)
- Wardriving CSV/PCAP at scale
- Possibly different partition table / more flash
- Port and merge shared modules from `wifiend-xiao`: `deauth_engine`, battery (if same circuit), BLE, WebUI
