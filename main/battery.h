#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

#define BATTERY_INVALID 0xFF

void battery_init(void);
uint16_t battery_read_mv(void);
uint8_t battery_get_percentage(void); // returns BATTERY_INVALID when no valid LiPo detected

#endif
