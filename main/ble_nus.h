#ifndef BLE_NUS_H
#define BLE_NUS_H

#include "encoder.h"

// Nordic UART Service phone link: DUMP / STAT commands over NUS.

void ble_nus_enter(void);
void ble_nus_exit(void);
void ble_nus_tick(void);
void ble_nus_input(encoder_event_t e);

#endif
