#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

void settings_init(void);
void settings_load(void);
void settings_save(void);

uint8_t  settings_get_led_brightness(void);
void     settings_set_led_brightness(uint8_t pct);

uint16_t settings_get_hop_dwell_24(void);
void     settings_set_hop_dwell_24(uint16_t ms);

uint16_t settings_get_hop_dwell_5(void);
void     settings_set_hop_dwell_5(uint16_t ms);

bool     settings_get_legal_ack(void);
void     settings_set_legal_ack(bool ack);

uint8_t  settings_get_burst_24(void);
void     settings_set_burst_24(uint8_t n);

uint8_t  settings_get_burst_5(void);
void     settings_set_burst_5(uint8_t n);

// Home Wi-Fi used by GitHub OTA (join STA, check releases).
const char *settings_get_home_ssid(void);
const char *settings_get_home_pass(void);
void        settings_set_home_wifi(const char *ssid, const char *pass);

void settings_enter(void);
void settings_exit(void);
void settings_render(void);
void settings_scroll_up(void);
void settings_scroll_down(void);
void settings_select(void);

bool settings_back(void);
bool settings_is_active(void);
bool settings_needs_refresh(void);

#endif
