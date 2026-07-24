#ifndef IEEE154_SNIFF_H
#define IEEE154_SNIFF_H

#include <stdint.h>
#include <stdbool.h>

// IEEE 802.15.4 promiscuous sniff for ESP32-C5.
// Requires CONFIG_IEEE802154_ENABLED=y. WiFi is stopped while active (shared radio).

typedef struct {
    uint8_t  frame_type;   // 0=Beacon, 1=Data, 2=Ack, 3=MAC Cmd
    uint16_t dst_pan;
    uint16_t src_pan;
    uint16_t dst_short;
    uint16_t src_short;
    uint8_t  dst_ext[8];
    uint8_t  src_ext[8];
    bool     has_dst_short;
    bool     has_src_short;
    bool     has_dst_ext;
    bool     has_src_ext;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  lqi;
} ieee154_frame_summary_t;

typedef struct {
    uint32_t total;
    uint32_t beacon;
    uint32_t data;
    uint32_t ack;
    uint32_t mac_cmd;
    uint32_t other;
} ieee154_sniff_stats_t;

void ieee154_sniff_init(void);
void ieee154_sniff_enter(void);
void ieee154_sniff_start(void);
void ieee154_sniff_stop(void);
void ieee154_sniff_render(void);

bool ieee154_sniff_is_running(void);

const ieee154_sniff_stats_t *ieee154_sniff_get_stats(void);
const ieee154_frame_summary_t *ieee154_sniff_get_last_frame(void);

#endif
