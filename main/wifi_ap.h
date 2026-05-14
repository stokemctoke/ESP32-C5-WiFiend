#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdint.h>
#include <stdbool.h>

void wifi_ap_init(void);
void wifi_ap_start(void);
void wifi_ap_stop(void);
bool wifi_ap_is_running(void);
uint8_t wifi_ap_get_client_count(void);
void wifi_ap_display_status(void);

#endif
