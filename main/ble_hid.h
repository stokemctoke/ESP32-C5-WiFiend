#ifndef BLE_HID_H
#define BLE_HID_H

#include "encoder.h"

// Minimal BadBLE-style HID keyboard peripheral (compact / partial).

void ble_hid_enter(void);
void ble_hid_exit(void);
void ble_hid_tick(void);
void ble_hid_input(encoder_event_t e);

#endif
