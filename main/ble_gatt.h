#ifndef BLE_GATT_H
#define BLE_GATT_H

#include "encoder.h"

// GATT explorer: connect to a scanned device and list services/characteristics.

void ble_gatt_enter(void);
void ble_gatt_exit(void);
void ble_gatt_tick(void);
void ble_gatt_input(encoder_event_t e);

#endif
