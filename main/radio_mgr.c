#include "radio_mgr.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "radio_mgr";

static radio_mode_t current = RADIO_MODE_IDLE;

void radio_mgr_init(void) {
    current = RADIO_MODE_IDLE;
    ESP_LOGI(TAG, "Radio manager ready");
}

radio_mode_t radio_mgr_current(void) {
    return current;
}

const char *radio_mgr_mode_name(radio_mode_t mode) {
    switch (mode) {
        case RADIO_MODE_IDLE:         return "IDLE";
        case RADIO_MODE_WIFI_SCAN:    return "WIFI_SCAN";
        case RADIO_MODE_WIFI_SNIFF:   return "WIFI_SNIFF";
        case RADIO_MODE_WIFI_MONITOR: return "WIFI_MONITOR";
        case RADIO_MODE_WIFI_ATTACK:  return "WIFI_ATTACK";
        case RADIO_MODE_WIFI_AP:      return "WIFI_AP";
        case RADIO_MODE_WIFI_STA:     return "WIFI_STA";
        case RADIO_MODE_WIFI_CAPTURE: return "WIFI_CAPTURE";
        case RADIO_MODE_WIFI_WEBUI:   return "WIFI_WEBUI";
        case RADIO_MODE_BLE_ACTIVE:   return "BLE_ACTIVE";
        case RADIO_MODE_BLE_ADV_TX:   return "BLE_ADV_TX";
        case RADIO_MODE_BLE_RECON:    return "BLE_RECON";
        case RADIO_MODE_IEEE154:      return "IEEE154";
        case RADIO_MODE_ESPNOW:       return "ESPNOW";
        default:                      return "?";
    }
}

static bool is_exclusive(radio_mode_t mode) {
    switch (mode) {
        case RADIO_MODE_IDLE:
        case RADIO_MODE_BLE_RECON:
            return false;
        default:
            return true;
    }
}

static void teardown_wifi_promiscuous(void) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
}

bool radio_mgr_enter(radio_mode_t mode) {
    if (mode == current) return true;

    // Leaving an exclusive mode: drop promiscuous if we owned WiFi sniff/monitor/attack.
    if (is_exclusive(current)) {
        switch (current) {
            case RADIO_MODE_WIFI_SNIFF:
            case RADIO_MODE_WIFI_MONITOR:
            case RADIO_MODE_WIFI_ATTACK:
            case RADIO_MODE_WIFI_CAPTURE:
                teardown_wifi_promiscuous();
                break;
            case RADIO_MODE_IEEE154:
            case RADIO_MODE_ESPNOW:
                // Caller is responsible for stopping those stacks before leave.
                break;
            default:
                break;
        }
        ESP_LOGI(TAG, "leave %s → enter %s",
                 radio_mgr_mode_name(current), radio_mgr_mode_name(mode));
    } else if (is_exclusive(mode) && current != RADIO_MODE_IDLE) {
        ESP_LOGI(TAG, "enter %s (was %s)",
                 radio_mgr_mode_name(mode), radio_mgr_mode_name(current));
    }

    current = mode;
    return true;
}

void radio_mgr_leave(radio_mode_t mode) {
    if (current != mode) return;
    if (is_exclusive(mode)) {
        switch (mode) {
            case RADIO_MODE_WIFI_SNIFF:
            case RADIO_MODE_WIFI_MONITOR:
            case RADIO_MODE_WIFI_ATTACK:
            case RADIO_MODE_WIFI_CAPTURE:
                teardown_wifi_promiscuous();
                break;
            default:
                break;
        }
    }
    current = RADIO_MODE_IDLE;
}

bool radio_mgr_wifi_idle(void) {
    return current == RADIO_MODE_IDLE || current == RADIO_MODE_BLE_RECON;
}
