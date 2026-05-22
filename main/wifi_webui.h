#ifndef WIFI_WEBUI_H
#define WIFI_WEBUI_H

#include <stdbool.h>

#define WEBUI_AP_SSID    "WiFiend-Remote"
#define WEBUI_AP_IP      "192.168.4.1"
#define WEBUI_AP_CHANNEL  6

typedef void (*webui_op_start_cb_t)(const char *op);
typedef void (*webui_op_stop_cb_t)(const char *op);

void    wifi_webui_init(void);
void    wifi_webui_enter(void);
void    wifi_webui_stop(void);
void    wifi_webui_render(void);
bool    wifi_webui_is_running(void);
bool    wifi_webui_needs_refresh(void);
void    wifi_webui_set_op_callbacks(webui_op_start_cb_t on_start,
                                    webui_op_stop_cb_t  on_stop);

#endif
