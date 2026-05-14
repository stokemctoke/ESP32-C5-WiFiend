#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include <stdint.h>

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} pixel_color_t;

void neopixel_init(void);
void neopixel_set_color(pixel_color_t color);
void neopixel_set_brightness(uint8_t brightness);

// MAKE_COLOR avoids colliding with ESP-IDF LCD component's RGB macro
#define MAKE_COLOR(r, g, b) ((pixel_color_t){r, g, b})
#define COLOR_RED     MAKE_COLOR(255,   0,   0)
#define COLOR_GREEN   MAKE_COLOR(  0, 255,   0)
#define COLOR_BLUE    MAKE_COLOR(  0,   0, 255)
#define COLOR_YELLOW  MAKE_COLOR(255, 255,   0)
#define COLOR_CYAN    MAKE_COLOR(  0, 255, 255)
#define COLOR_MAGENTA MAKE_COLOR(255,   0, 255)
#define COLOR_WHITE   MAKE_COLOR(255, 255, 255)
#define COLOR_OFF     MAKE_COLOR(  0,   0,   0)

#endif
