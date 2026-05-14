#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_CONNECT_IDLE = 0,
    WIFI_CONNECT_CONNECTING = 1,
    WIFI_CONNECT_CONNECTED = 2,
    WIFI_CONNECT_FAILED = 3,
} wifi_connect_state_t;

void wifi_sta_init(void);
bool wifi_sta_connect(const char *ssid, const char *password);
void wifi_sta_stop(void);
bool wifi_sta_is_connected(void);
wifi_connect_state_t wifi_sta_get_state(void);
const char* wifi_sta_get_ip(void);
void wifi_sta_display_status(void);

#endif
