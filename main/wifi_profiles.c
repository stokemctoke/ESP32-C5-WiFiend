#include "wifi_profiles.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_prof";

#define NVS_NS "wifi_prof"

static void key_ssid(uint8_t index, char *buf, size_t n) {
    snprintf(buf, n, "p%u_ssid", (unsigned)index);
}

static void key_pw(uint8_t index, char *buf, size_t n) {
    snprintf(buf, n, "p%u_pw", (unsigned)index);
}

static bool slot_used_in_nvs(uint8_t index) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    char k[12];
    key_ssid(index, k, sizeof(k));
    char ssid[WIFI_PROFILE_SSID_LEN + 1] = {0};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, k, ssid, &len);
    nvs_close(h);
    return (err == ESP_OK && ssid[0] != '\0');
}

void wifi_profiles_init(void) {
    ESP_LOGI(TAG, "Profiles init — %u saved", (unsigned)wifi_profiles_count());
}

bool wifi_profiles_save(uint8_t index, const char *ssid, const char *password) {
    if (index >= WIFI_PROFILES_MAX || !ssid || !ssid[0]) return false;
    if (!password) password = "";

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed");
        return false;
    }

    char k[12];
    key_ssid(index, k, sizeof(k));
    esp_err_t err = nvs_set_str(h, k, ssid);
    if (err == ESP_OK) {
        key_pw(index, k, sizeof(k));
        err = nvs_set_str(h, k, password);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved profile %u: %s", (unsigned)index, ssid);
    return true;
}

bool wifi_profiles_load(uint8_t index, wifi_profile_t *out) {
    if (index >= WIFI_PROFILES_MAX || !out) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    memset(out, 0, sizeof(*out));
    char k[12];
    key_ssid(index, k, sizeof(k));
    size_t len = sizeof(out->ssid);
    esp_err_t err = nvs_get_str(h, k, out->ssid, &len);
    if (err != ESP_OK || out->ssid[0] == '\0') {
        nvs_close(h);
        return false;
    }

    key_pw(index, k, sizeof(k));
    len = sizeof(out->password);
    err = nvs_get_str(h, k, out->password, &len);
    if (err != ESP_OK) out->password[0] = '\0';

    nvs_close(h);
    return true;
}

uint8_t wifi_profiles_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < WIFI_PROFILES_MAX; i++) {
        if (slot_used_in_nvs(i)) n++;
    }
    return n;
}

bool wifi_profiles_delete(uint8_t index) {
    if (index >= WIFI_PROFILES_MAX) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    char k[12];
    key_ssid(index, k, sizeof(k));
    nvs_erase_key(h, k);
    key_pw(index, k, sizeof(k));
    nvs_erase_key(h, k);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool wifi_profiles_get_ssid(uint8_t index, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0 || index >= WIFI_PROFILES_MAX) return false;

    wifi_profile_t prof;
    if (!wifi_profiles_load(index, &prof)) return false;

    strncpy(buf, prof.ssid, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return true;
}
