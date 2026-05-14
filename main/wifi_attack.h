#ifndef WIFI_ATTACK_H
#define WIFI_ATTACK_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_ATTACK_TARGETS 10

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint32_t packets_sent;
} attack_target_t;

void wifi_attack_init(void);
uint16_t wifi_attack_scan_and_filter(int rssi_threshold_24, int rssi_threshold_5);
const attack_target_t* wifi_attack_get_targets(uint16_t *count);
bool wifi_attack_start(void);
void wifi_attack_stop(void);
bool wifi_attack_is_running(void);
void wifi_attack_display_status(void);

#endif
