#ifndef BLE_CLASS_H
#define BLE_CLASS_H

#include "encoder.h"

// Device classifier: scanner list annotated with a device-type label derived
// from company ID, service UUIDs, appearance, and beacon type.

void ble_class_enter(void);
void ble_class_exit(void);
void ble_class_tick(void);
void ble_class_input(encoder_event_t e);

#endif
