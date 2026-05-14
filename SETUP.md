# WiFiend Firmware Setup Guide

## Prerequisites

### ESP-IDF v6.0 Installation

WiFiend targets **ESP-IDF v6.0** (March 2026) with stable ESP32-C5 support.

```bash
# Create a workspace directory
mkdir -p ~/Github-Repos && cd ~/Github-Repos

# Clone ESP-IDF v6.0
git clone --recursive --branch v6.0 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf

# Install tools for ESP32-C5
./install.sh esp32c5

# Source the environment (do this in every terminal session)
. ./export.sh
```

**Verify ESP-IDF is ready:**
```bash
idf.py --version
```

You should see: `ESP-IDF v6.0` or later.

---

## Patched WiFi Library (Required for Deauthentication)

Deauthentication frames require a patched WiFi library. The patched `libnet80211.a` is included in `./patched_libnet/`.

### How It Works

Espressif's closed-source `libnet80211.a` blocks deauth frames (bytes 0xd0, 0xa0) for security/regulatory reasons. The patch overrides this check, allowing raw deauth frame transmission.

### Applying the Patch (Automatic)

The build system **automatically uses the patched library** from `./patched_libnet/libnet80211.a`.

If you're upgrading ESP-IDF versions and the patch no longer works:

1. **For ESP-IDF v6.0:** Try the existing patched lib first
2. **If it fails to link:** Either
   - Fall back to ESP-IDF v5.5.1 (proven to work)
   - Contact the AnvilBrain project for an updated patch

---

## Building WiFiend

### First Build

```bash
cd ~/Github-Repos/ESP32-C5_WiFiend

# Full clean build
idf.py fullclean
idf.py build
```

### Flashing to ESP32-C5

```bash
# Replace /dev/ttyACM0 with your device's serial port
idf.py -p /dev/ttyACM0 flash monitor
```

### Subsequent Builds

```bash
idf.py build                          # Just compile
idf.py -p /dev/ttyACM0 flash         # Flash without monitor
idf.py -p /dev/ttyACM0 flash monitor # Flash + open serial monitor
```

---

## Troubleshooting

### "idf.py: command not found"

You haven't sourced the ESP-IDF environment. In your terminal:

```bash
cd ~/Github-Repos/esp-idf
. ./export.sh
```

### Build fails with "libnet80211.a" linker errors

The patched library might not be compatible with your ESP-IDF version.

**Options:**
1. Fall back to ESP-IDF v5.5.1 (proven to work with the patched lib)
2. Search for an updated patch for ESP-IDF v6.0
3. Skip deauth features and use standard WiFi functions only

### Port not found during flash

Check your USB connection and find the port:

```bash
ls /dev/ttyACM*     # Linux/Mac
ls /dev/ttyUSB*     # Linux alternate
```

Or use:
```bash
idf.py -p /dev/ttyACM0 flash --before=no_reset monitor
```

---

## Development Workflow

### Recommended Build Loop

```bash
# 1. Make code changes
# 2. Rebuild and test
idf.py build && idf.py -p /dev/ttyACM0 flash monitor

# 3. Watch serial output, Ctrl+C to stop monitor
# 4. Go to step 1
```

### Cleaning Between Builds

If you encounter strange build errors:

```bash
idf.py fullclean    # Full clean (slow but thorough)
idf.py clean        # Faster clean (deletes build dir)
```

---

## ESP-IDF v6.0 vs v5.5.1

| Feature | v6.0 | v5.5.1 |
|---------|------|--------|
| ESP32-C5 Status | Production ready | Initial support |
| WiFi 6 Support | Full | Good |
| Stability | Latest patches | Stable baseline |
| Deauth Support | Patched lib required | Patched lib required |

**Migration Path:** If v6.0 fails, fall back to v5.5.1 by:

```bash
cd ~/Github-Repos/esp-idf
git checkout v5.5.1
. ./export.sh
```

Then rebuild WiFiend.

---

## Next Steps

1. ✅ Install ESP-IDF v6.0
2. ✅ Source the environment
3. ✅ Run `idf.py build` to verify setup
4. ✅ Flash to XIAO ESP32-C5 and test boot sequence
5. Configure GPIO pins in `main/buttons.c` and `main/battery.c`
6. Implement button/battery ADC handlers
7. Flesh out WiFi modules
