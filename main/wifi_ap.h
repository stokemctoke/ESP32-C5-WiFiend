#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdint.h>
#include <stdbool.h>

void wifi_ap_init(void);
void wifi_ap_enter(void);       // load SSID picker from last scan
void wifi_ap_scroll_up(void);
void wifi_ap_scroll_down(void);
void wifi_ap_select(void);      // confirm SSID clone and start AP
void wifi_ap_stop(void);
bool         wifi_ap_is_running(void);
void         wifi_ap_render(void);
const char  *wifi_ap_get_ssid(void);
uint8_t      wifi_ap_get_client_count(void);

#endif
