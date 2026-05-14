#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdint.h>

#define MAX_SCAN_RESULTS 20

#define WIFIEND_PHY_11B  0x01
#define WIFIEND_PHY_11G  0x02
#define WIFIEND_PHY_11N  0x04
#define WIFIEND_PHY_11AX 0x10

typedef struct {
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t bssid[6];
    uint8_t security;   // wifi_auth_mode_t
    uint8_t cipher;     // pairwise cipher (wifi_cipher_type_t)
    uint8_t phy_flags;  // WIFIEND_PHY_* bitmask
    uint8_t wps;
} wifi_ap_info_t;

void wifi_scan_init(void);
uint16_t wifi_scan_start(void);
const wifi_ap_info_t *wifi_scan_get_results(uint16_t *count);
void wifi_scan_scroll_up(void);
void wifi_scan_scroll_down(void);
void wifi_scan_render(void);
void wifi_scan_render_detail(void);
void wifi_scan_chart_next(void);
void wifi_scan_chart_prev(void);
void wifi_scan_chart_toggle(void);
void wifi_scan_render_chart(void);

#endif
