#ifndef BLE_ADVLOG_H
#define BLE_ADVLOG_H

#include "encoder.h"

// Append BLE advertisements to /lfs/ble.log while scanning.

void ble_advlog_enter(void);
void ble_advlog_exit(void);
void ble_advlog_tick(void);
void ble_advlog_input(encoder_event_t e);

#endif
