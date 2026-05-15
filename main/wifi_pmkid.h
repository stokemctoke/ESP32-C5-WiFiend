#ifndef WIFI_PMKID_H
#define WIFI_PMKID_H

#include <stdint.h>
#include <stdbool.h>

#define PMKID_MAX_CAPTURES 20

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t client_mac[6];
    uint8_t pmkid[16];
    uint8_t channel;
} pmkid_capture_t;

void    wifi_pmkid_init(void);
void    wifi_pmkid_enter(void);
void    wifi_pmkid_scroll_up(void);
void    wifi_pmkid_scroll_down(void);
void    wifi_pmkid_select(void);
void    wifi_pmkid_view_next(void);
void    wifi_pmkid_stop(void);
void    wifi_pmkid_render(void);

bool    wifi_pmkid_is_running(void);
bool    wifi_pmkid_needs_refresh(void);
bool    wifi_pmkid_is_in_picker(void);
bool    wifi_pmkid_is_captured(void);
uint8_t wifi_pmkid_get_count(void);

#endif
