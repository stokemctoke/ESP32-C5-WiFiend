#include "wifi_sta.h"
#include "esp_log.h"

static const char *TAG = "wifi_sta";

void wifi_sta_init(void) {
    ESP_LOGI(TAG, "WiFi STA module stub");
}

bool wifi_sta_connect(const char *ssid, const char *password) {
    ESP_LOGI(TAG, "Connecting to %s", ssid);
    return false;
}

void wifi_sta_stop(void) {
    ESP_LOGI(TAG, "Disconnecting");
}

bool wifi_sta_is_connected(void) {
    return false;
}

wifi_connect_state_t wifi_sta_get_state(void) {
    return WIFI_CONNECT_IDLE;
}

const char* wifi_sta_get_ip(void) {
    return "0.0.0.0";
}

void wifi_sta_display_status(void) {
    ESP_LOGI(TAG, "Display STA status");
}
