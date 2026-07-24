#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_MONITOR_BEACONS 40
#define MAX_MONITOR_PROBES  40

#define MON_SEC_WEP  0x01
#define MON_SEC_WPA  0x02
#define MON_SEC_WPA2 0x04
#define MON_SEC_WPA3 0x08

typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  sec_flags;   // MON_SEC_* bitmask
    bool     hidden;
    bool     logged;      // beacon already written to /lfs/beacons.log
} monitor_beacon_t;

typedef struct {
    uint8_t client_mac[6];
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint16_t count;
} monitor_probe_t;

typedef struct {
    uint16_t m1;
    uint16_t m2;
    uint16_t m3;
    uint16_t m4;
} monitor_eapol_t;

void wifi_monitor_init(void);
void wifi_monitor_enter(void);
void wifi_monitor_start(void);
void wifi_monitor_stop(void);
void wifi_monitor_scroll_up(void);
void wifi_monitor_scroll_down(void);
void wifi_monitor_select(void);
void wifi_monitor_render(void);

bool wifi_monitor_is_running(void);

uint16_t wifi_monitor_beacon_count(void);
uint16_t wifi_monitor_probe_count(void);
const monitor_beacon_t *wifi_monitor_get_beacon(uint16_t idx);
const monitor_probe_t  *wifi_monitor_get_probe(uint16_t idx);
const monitor_eapol_t  *wifi_monitor_get_eapol(void);

#endif
