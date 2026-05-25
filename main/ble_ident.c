#include "ble_ident.h"
#include <string.h>
#include <stdio.h>

// Walk the AD-structure list of a raw advertisement and return a pointer to the
// payload (and its length) of the first structure with the given AD type.
static const uint8_t *find_ad(const uint8_t *adv, uint8_t len, uint8_t type, uint8_t *out_len) {
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t l = adv[i];
        if (l == 0) break;
        if (i + 1 + l > len) break;
        if (adv[i + 1] == type) {
            *out_len = (uint8_t)(l - 1);
            return &adv[i + 2];
        }
        i += l + 1;
    }
    return NULL;
}

bool ble_decode_ibeacon(const uint8_t *adv, uint8_t len, ibeacon_t *out) {
    uint8_t mlen = 0;
    const uint8_t *m = find_ad(adv, len, 0xFF, &mlen);   // manufacturer specific
    if (!m || mlen < 25) return false;
    // Apple company 0x004C, iBeacon type 0x02, length 0x15
    if (!(m[0] == 0x4C && m[1] == 0x00 && m[2] == 0x02 && m[3] == 0x15)) return false;
    memcpy(out->uuid, &m[4], 16);
    out->major = (uint16_t)((m[20] << 8) | m[21]);
    out->minor = (uint16_t)((m[22] << 8) | m[23]);
    out->power = (int8_t)m[24];
    return true;
}

static const char *eddystone_scheme(uint8_t c) {
    switch (c) {
        case 0x00: return "http://www.";
        case 0x01: return "https://www.";
        case 0x02: return "http://";
        case 0x03: return "https://";
        default:   return "";
    }
}

static const char *eddystone_suffix(uint8_t c) {
    static const char *t[] = { ".com/", ".org/", ".edu/", ".net/", ".info/",
                               ".biz/", ".gov/", ".com", ".org", ".edu",
                               ".net", ".info", ".biz", ".gov" };
    return (c < 14) ? t[c] : NULL;
}

bool ble_decode_eddystone(const uint8_t *adv, uint8_t len, eddystone_t *out) {
    uint8_t slen = 0;
    const uint8_t *s = find_ad(adv, len, 0x16, &slen);   // service data, 16-bit UUID
    if (!s || slen < 3) return false;
    if (!(s[0] == 0xAA && s[1] == 0xFE)) return false;   // 0xFEAA little-endian

    memset(out, 0, sizeof(*out));
    out->frame = s[2];

    if (out->frame == 0x10 && slen >= 5) {           // URL
        out->tx_power = (int8_t)s[3];
        size_t pos = 0;
        const char *scheme = eddystone_scheme(s[4]);
        strncpy(out->url, scheme, sizeof(out->url) - 1);
        pos = strlen(out->url);
        for (uint8_t i = 5; i < slen && pos < sizeof(out->url) - 1; i++) {
            const char *suf = eddystone_suffix(s[i]);
            if (suf) {
                for (const char *p = suf; *p && pos < sizeof(out->url) - 1; p++)
                    out->url[pos++] = *p;
            } else {
                out->url[pos++] = (char)s[i];
            }
        }
        out->url[pos] = '\0';
    } else if (out->frame == 0x00 && slen >= 20) {   // UID
        out->tx_power = (int8_t)s[3];
        memcpy(out->namespace_id, &s[4], 10);
        memcpy(out->instance, &s[14], 6);
    } else if (out->frame == 0x20) {                 // TLM (telemetry) — no decode here
        // left as frame type only
    }
    return true;
}

const char *ble_classify_device(const ble_dev_info_t *d) {
    if (d->beacon_type == BLE_BEACON_IBEACON)   return "iBeacon";
    if (d->beacon_type == BLE_BEACON_EDDYSTONE) return "Eddystone";

    switch (d->svc_uuid16) {
        case 0x1812: return "HID";
        case 0x180D: return "HeartRate";
        case 0x180F: return "Battery";
        case 0xFE2C: return "FastPair";
        case 0xFD6F: return "ExpNotif";   // exposure notification
        default: break;
    }

    switch (d->appearance >> 6) {   // GAP appearance category
        case 1: return "Phone";
        case 2: return "Computer";
        case 3: return "Wearable";
        case 5: return "Display";
        default: break;
    }

    switch (d->company_id) {
        case 0x004C: return "Apple";
        case 0x0075: return "Samsung";
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0059: return "Nordic";
        default: break;
    }

    return "Unknown";
}
