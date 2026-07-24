#ifndef BLE_NOTIFY_H
#define BLE_NOTIFY_H

#include "encoder.h"

// Subscribe to the first notifiable GATT characteristic and show live values.

void ble_notify_enter(void);
void ble_notify_exit(void);
void ble_notify_tick(void);
void ble_notify_input(encoder_event_t e);

#endif
