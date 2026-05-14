#include "wifi_attack.h"
#include "esp_log.h"

static const char *TAG = "wifi_attack";
static attack_target_t targets[MAX_ATTACK_TARGETS];
static uint16_t target_count = 0;
static bool attacking = false;

void wifi_attack_init(void) {
    ESP_LOGI(TAG, "WiFi attack module stub");
}

uint16_t wifi_attack_scan_and_filter(int rssi_threshold_24, int rssi_threshold_5) {
    ESP_LOGI(TAG, "Scanning and filtering targets");
    return 0;
}

const attack_target_t* wifi_attack_get_targets(uint16_t *count) {
    if (count) *count = target_count;
    return targets;
}

bool wifi_attack_start(void) {
    ESP_LOGI(TAG, "Starting attack");
    attacking = true;
    return true;
}

void wifi_attack_stop(void) {
    ESP_LOGI(TAG, "Stopping attack");
    attacking = false;
}

bool wifi_attack_is_running(void) {
    return attacking;
}

void wifi_attack_display_status(void) {
    ESP_LOGI(TAG, "Display attack status");
}
