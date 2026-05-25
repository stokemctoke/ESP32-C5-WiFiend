#ifndef BLE_IDENT_H
#define BLE_IDENT_H

#include <stdint.h>
#include <stdbool.h>
#include "ble_scan.h"

// Pure helpers over scan results / raw adv bytes — no NimBLE calls.

typedef struct {
    uint8_t  uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t   power;
} ibeacon_t;

typedef struct {
    uint8_t frame;            // 0x00 UID, 0x10 URL, 0x20 TLM
    int8_t  tx_power;
    char    url[48];          // decoded (URL frame)
    uint8_t namespace_id[10]; // UID frame
    uint8_t instance[6];      // UID frame
} eddystone_t;

// Short device-type label from company ID, service UUID, appearance, beacon type.
const char *ble_classify_device(const ble_dev_info_t *d);

// Decode iBeacon / Eddystone from a raw advertisement payload. Returns false if
// the payload is not that beacon type.
bool ble_decode_ibeacon(const uint8_t *adv, uint8_t len, ibeacon_t *out);
bool ble_decode_eddystone(const uint8_t *adv, uint8_t len, eddystone_t *out);

#endif
