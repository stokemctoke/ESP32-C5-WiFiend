#ifndef BLE_SPAM_H
#define BLE_SPAM_H

#include "encoder.h"

// BLE advertisement spam / custom beacon TX (Apple, Fast Pair, iBeacon, Eddystone).

void ble_spam_enter(void);
void ble_spam_exit(void);
void ble_spam_tick(void);
void ble_spam_input(encoder_event_t e);

#endif
