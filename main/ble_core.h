#ifndef BLE_CORE_H
#define BLE_CORE_H

#include <stdbool.h>
#include <stdint.h>

// One-time NimBLE host/controller bring-up. Safe to call repeatedly — only the
// first call does work. The host runs in its own task; ble_core_is_ready()
// turns true once the controller has synced and an identity address is set.
void    ble_core_init(void);
bool    ble_core_is_ready(void);
uint8_t ble_core_own_addr_type(void);

#endif
