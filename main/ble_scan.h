#ifndef BLE_SCAN_H
#define BLE_SCAN_H

#include <stdint.h>
#include <stdbool.h>
#include "encoder.h"

#define MAX_BLE_RESULTS 24
#define BLE_NAME_LEN    32

#define BLE_BEACON_NONE      0
#define BLE_BEACON_IBEACON   1
#define BLE_BEACON_EDDYSTONE 2

// One deduped BLE device discovered during a scan.
typedef struct {
    uint8_t  addr[6];         // NimBLE order (LSB first)
    uint8_t  addr_type;       // BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM
    int8_t   rssi;            // most recent
    int8_t   rssi_peak;       // strongest seen (for the hunter)
    uint8_t  evt_type;        // adv PDU type (connectable/scannable/non-conn)
    char     name[BLE_NAME_LEN];
    uint16_t company_id;      // manufacturer company ID, 0xFFFF if none
    uint16_t svc_uuid16;      // first advertised 16-bit service UUID, 0 if none
    uint16_t appearance;      // GAP appearance, 0 if none
    int8_t   tx_pwr;          // adv tx power, 127 if absent
    uint8_t  flags;           // adv flags byte
    uint8_t  beacon_type;     // BLE_BEACON_*
    uint8_t  raw[31];         // raw adv payload (for beacon decoders)
    uint8_t  raw_len;
    uint32_t last_seen_ms;
} ble_dev_info_t;

// ---- shared scan core (disc + results), reused by all BLE views ----
void ble_scan_disc_start(void);   // begin GAP discovery (once core is ready)
void ble_scan_disc_stop(void);
void ble_scan_clear(void);
void ble_scan_lock(void);         // guard result reads from the host-task writer
void ble_scan_unlock(void);
const ble_dev_info_t *ble_scan_get_results(uint16_t *count);  // call while locked

// ---- BLE Scanner tool (list + detail) ----
void ble_scan_enter(void);
void ble_scan_exit(void);
void ble_scan_tick(void);              // re-render current view (call ~10Hz)
void ble_scan_input(encoder_event_t e);

#endif
