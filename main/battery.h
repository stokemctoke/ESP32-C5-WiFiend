#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

#define BATTERY_INVALID 0xFF

void battery_init(void);
void battery_tick(void);              // refresh every ~5 s (call from main loop)
bool battery_is_present(void);
uint16_t battery_read_mv(void);       // 0 if no LiPo / USB-only
uint8_t battery_get_percentage(void); // BATTERY_INVALID when no LiPo detected

#endif
