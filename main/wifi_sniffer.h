#ifndef WIFI_SNIFFER_H
#define WIFI_SNIFFER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_SNIFF_CLIENTS 30

typedef struct {
    uint8_t  mac[6];
    uint8_t  ap_bssid[6];   // zero if only seen in probes
    int8_t   rssi;
    uint16_t frame_count;
    uint8_t  channel;
    bool     associated;     // true if seen in data/assoc frames (not just probes)
} wifi_client_t;

void wifi_sniff_init(void);
void wifi_sniff_start(void);
void wifi_sniff_stop(void);
void wifi_sniff_scroll_up(void);
void wifi_sniff_scroll_down(void);
void wifi_sniff_render(void);
void wifi_sniff_render_detail(void);
uint16_t                wifi_sniff_get_count(void);
const wifi_client_t    *wifi_sniff_get_client(uint16_t idx);

#endif
