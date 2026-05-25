#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include "encoder.h"

// Filtered view over the BLE scan results: only iBeacon/Eddystone devices,
// with a per-beacon detail page that decodes the payload.

void ble_beacon_enter(void);
void ble_beacon_exit(void);
void ble_beacon_tick(void);
void ble_beacon_input(encoder_event_t e);

#endif
