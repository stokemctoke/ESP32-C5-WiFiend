#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>

void ssd1306_init(void);
void ssd1306_clear_buffer(void);
void ssd1306_clear_screen(void);
void ssd1306_clear_page(uint8_t page);
void ssd1306_draw_char(uint8_t x, uint8_t page, char c);
void ssd1306_draw_string(uint8_t x, uint8_t page, const char *str);
void ssd1306_draw_bitmap_fullscreen(const uint8_t *bitmap);
void ssd1306_draw_header(const char *title, const char *status);
void ssd1306_flush(void);

#endif
