#ifndef WIFI_HANDSHAKE_H
#define WIFI_HANDSHAKE_H

#include <stdint.h>
#include <stdbool.h>

#define HANDSHAKE_MAX_CAPTURES 10

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t client_mac[6];
    uint8_t channel;
} handshake_summary_t;

void    wifi_handshake_init(void);
void    wifi_handshake_enter(void);
void    wifi_handshake_scroll_up(void);
void    wifi_handshake_scroll_down(void);
void    wifi_handshake_select(void);
void    wifi_handshake_view_next(void);
void    wifi_handshake_stop(void);
void    wifi_handshake_render(void);

bool         wifi_handshake_is_running(void);
bool         wifi_handshake_needs_refresh(void);
bool         wifi_handshake_is_in_picker(void);
bool         wifi_handshake_is_captured(void);
uint8_t      wifi_handshake_get_count(void);
const char  *wifi_handshake_get_target(void);
uint16_t     wifi_handshake_get_m1(void);
uint16_t     wifi_handshake_get_m2(void);
uint16_t     wifi_handshake_get_deauth(void);

#endif
