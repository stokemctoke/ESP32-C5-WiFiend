# WiFiend Quick Start Guide

## What's Ready Now

✅ **Complete hardware driver layer:**
- OLED display (SSD1306) with optimized framebuffer
- 3-way thumbstick with debounce (50ms) + long-hold detection (500ms)
- WS2812B NeoPixel status LED with brightness control
- Battery LiPo ADC reading + percentage calculation
- Interactive menu system with submenu support

✅ **User interaction model:**
- **Up/Down**: Navigate menu (wraps around)
- **Center short press** (<500ms): Select current item
- **Center long hold** (>500ms): Go back to parent menu
- Visual feedback on OLED ("Hold CENTER to back")
- NeoPixel color changes per mode (green=idle, yellow=scanner, red=attack, etc)

✅ **Boot sequence:**
1. WiFiNugget splash screen (2 sec)
2. Green NeoPixel ready indicator
3. Main menu drops to screen
4. Battery % displayed in Device Info
5. Ready for interaction

## Pin Configuration

| Function | GPIO | Notes |
|----------|------|-------|
| Thumbstick Up | GPIO3 | Pull-up input, ADC1_CH3 |
| Thumbstick Down | GPIO6 | Pull-up input, ADC1_CH6 |
| Thumbstick Center | GPIO7 | Pull-up input |
| NeoPixel | GPIO8 | RMT peripheral |
| Battery ADC | GPIO4 | ADC1_CH4 — ESP32-C5 ADC only on GPIO0–6 |
| Spare | GPIO9 | Available |
| Spare | GPIO10 | Available (digital only) |

> **Note:** ESP32-C5 ADC1 covers GPIO0–6 only. GPIO7 and above are digital-only. Battery sense wire goes to GPIO4 via a 100k/100k voltage divider.

## Building & Flashing

### 1. Install ESP-IDF v6.0

```bash
mkdir -p ~/Github-Repos && cd ~/Github-Repos
git clone --recursive --branch v6.0 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c5
. ./export.sh
```

### 2. Build WiFiend

```bash
cd ~/Github-Repos/ESP32-C5_WiFiend
idf.py build
```

### 3. Flash to XIAO ESP32-C5

```bash
# Flash + open serial monitor
idf.py -p /dev/ttyACM0 flash monitor

# Or just flash (no monitor)
idf.py -p /dev/ttyACM0 flash
```

Replace `/dev/ttyACM0` with your device's serial port. Use `ls /dev/tty*` to find it.

### 4. Test Boot Sequence

You should see:
1. WiFiNugget splash on OLED (2 sec)
2. Green LED on
3. Main menu appears:
   ```
   > WiFi Scanner
     AP Mode
     Deauth Attack
     STA Connect
     Device Info
   ```

### 5. Test Interaction

Try these:
- **Up/Down**: Navigate menu items
- **Center (short)**: Select "Device Info" to see battery % and MAC address
- **Center (long hold)**: Go back from Device Info submenu

## What's Stubbed (Not Yet Implemented)

These modules have placeholder implementations - ready for next phase:

- `wifi_scan.c` — WiFi network scanning + display
- `wifi_ap.c` — Access point mode
- `wifi_sta.c` — Station mode (connect to AP)
- `wifi_attack.c` — Deauthentication engine (needs WiFuxx code integration)

## Next Steps

### Phase 1: WiFi Scanning (Priority)
1. Extract scan logic from WiFuxx main.c
2. Implement `wifi_scan_start()` with ESP-IDF WiFi APIs
3. Create submenu for scan results
4. Add AP selection → details display

### Phase 2: WiFi Modes
1. AP mode with MAC-derived SSID + NVS PSK storage
2. STA mode with connect/disconnect
3. Show IP + connection status on OLED

### Phase 3: Deauth Attack
1. Lift deauth frame functions from WiFuxx main.c
2. Integrate with `wifi_attack.c`
3. Add attack mode menu item
4. Display statistics (pps, elapsed time, target count)

## Troubleshooting

### Build Fails: "libnet80211.a not found"

The patched WiFi library is in `./patched_libnet/` and the build system uses it automatically. If the build still fails, check:

```bash
ls -la patched_libnet/libnet80211.a
```

Should show the file exists.

### Serial Monitor Shows Garbage

Check baud rate. Default is 115200. In `idf.py monitor`, press Ctrl+] to exit.

### Buttons Not Responding

1. Check GPIO wiring to pull-up resistors
2. Verify GPIO3, GPIO6, GPIO7 are pulled HIGH at rest
3. Test with serial output: each button press should log to console

### NeoPixel Not Lighting

1. Check GPIO8 wiring to WS2812B data line
2. Verify power to LED (3.3V or 5V depending on module)
3. Check GPIO8 isn't conflicting with RMT usage

### Battery Shows 0%

1. Check voltage divider ratio (should be ×2 for typical dividers)
2. Verify GPIO10 reads correct ADC value: `adc_oneshot_read()`
3. Check battery connector polarity

## Development Workflow

```bash
# 1. Edit source files in main/
# 2. Rebuild
idf.py build

# 3. Flash + monitor
idf.py -p /dev/ttyACM0 flash monitor

# 4. Watch logs, test interaction
# 5. Ctrl+C to stop monitor
# 6. Repeat from step 1
```

## File Organization

- `main/ssd1306.c|h` — OLED driver (lifted from WiFuxx)
- `main/buttons.c|h` — Thumbstick input + debounce
- `main/neopixel.c|h` — WS2812B LED control
- `main/battery.c|h` — LiPo ADC reading
- `main/menu.c|h` — Menu system with stack
- `main/main.c` — Boot sequence + event loop
- `main/wifi_*.c|h` — WiFi modules (stubs, ready for implementation)
- `patched_libnet/libnet80211.a` — WiFi library patch for deauth support
- `SETUP.md` — Detailed installation instructions
- `CMakeLists.txt` — Top-level ESP-IDF project config

## References

- [ESP-IDF v6.0 Documentation](https://docs.espressif.com/projects/esp-idf/en/v6.0/)
- [WiFuxx Reference Implementation](https://github.com/stokemctoke/WiFuxx_ESP32-C5-Auto-Dualband-Deauth)
- [XIAO ESP32-C5 Pinout](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
