#ifndef BLE_HUNT_H
#define BLE_HUNT_H

#include "encoder.h"

// RSSI proximity locator. Picker first, then a live-RSSI tracking view that
// also drives the NeoPixel by signal strength.

void ble_hunt_enter(void);
void ble_hunt_exit(void);
void ble_hunt_tick(void);
void ble_hunt_input(encoder_event_t e);

#endif
