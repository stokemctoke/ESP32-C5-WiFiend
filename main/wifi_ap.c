#include "wifi_ap.h"
#include "esp_log.h"

static const char *TAG = "wifi_ap";

void wifi_ap_init(void) {
    ESP_LOGI(TAG, "WiFi AP module stub");
}

void wifi_ap_start(void) {
    ESP_LOGI(TAG, "Starting AP mode");
}

void wifi_ap_stop(void) {
    ESP_LOGI(TAG, "Stopping AP mode");
}

bool wifi_ap_is_running(void) {
    return false;
}

uint8_t wifi_ap_get_client_count(void) {
    return 0;
}

void wifi_ap_display_status(void) {
    ESP_LOGI(TAG, "Display AP status");
}
