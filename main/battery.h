#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

#define BATTERY_INVALID 0xFF

void battery_init(void);
void battery_tick(void);              // call periodically; refreshes reading every ~5 s
uint16_t battery_read_mv(void);
uint8_t battery_get_percentage(void); // BATTERY_INVALID when no LiPo detected (USB-only)

#endif
