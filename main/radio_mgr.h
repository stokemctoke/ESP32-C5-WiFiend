#ifndef RADIO_MGR_H
#define RADIO_MGR_H

#include <stdbool.h>

// Central exclusive radio-mode arbitration for WiFiend Xiao.
// Call radio_mgr_enter() before starting a mode; radio_mgr_leave() on exit.

typedef enum {
    RADIO_MODE_IDLE = 0,
    RADIO_MODE_WIFI_SCAN,
    RADIO_MODE_WIFI_SNIFF,
    RADIO_MODE_WIFI_MONITOR,
    RADIO_MODE_WIFI_ATTACK,
    RADIO_MODE_WIFI_AP,
    RADIO_MODE_WIFI_STA,
    RADIO_MODE_WIFI_CAPTURE,   // PMKID / handshake
    RADIO_MODE_WIFI_WEBUI,
    RADIO_MODE_BLE_ACTIVE,     // GATT / notify / BadBLE
    RADIO_MODE_BLE_ADV_TX,     // spam / beacon TX
    RADIO_MODE_BLE_RECON,      // scan / class / beacon / hunt (coexist OK)
    RADIO_MODE_IEEE154,
    RADIO_MODE_ESPNOW,
} radio_mode_t;

void         radio_mgr_init(void);
radio_mode_t radio_mgr_current(void);

// Stops the previous exclusive mode (if any) then claims `mode`.
// Returns false if mode is unknown / cannot be claimed.
bool radio_mgr_enter(radio_mode_t mode);

// Release `mode` if it is current; returns to IDLE.
void radio_mgr_leave(radio_mode_t mode);

// True when current mode is IDLE or a BLE recon mode that coexists with WiFi STA.
bool radio_mgr_wifi_idle(void);

const char *radio_mgr_mode_name(radio_mode_t mode);

#endif
