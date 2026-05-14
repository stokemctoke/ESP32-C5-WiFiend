#include "wifi_scan.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_scan";

static wifi_ap_info_t results[MAX_SCAN_RESULTS];
static uint16_t result_count = 0;
static uint16_t scroll_offset = 0;
static uint16_t selected_idx = 0;

static EventGroupHandle_t s_scan_done;
#define SCAN_DONE_BIT BIT0

// Static to avoid stack allocation inside encoder task callback
static wifi_ap_record_t ap_records[MAX_SCAN_RESULTS];

static void on_scan_done(void *arg, esp_event_base_t base,
                         int32_t id, void *data) {
    xEventGroupSetBits(s_scan_done, SCAN_DONE_BIT);
}

void wifi_scan_init(void) {
    s_scan_done = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               on_scan_done, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi scan init OK");
}

uint16_t wifi_scan_start(void) {
    result_count = 0;
    scroll_offset = 0;
    selected_idx = 0;

    xEventGroupClearBits(s_scan_done, SCAN_DONE_BIT);

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);  // non-blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    // Animate while WiFi driver scans — spinner on one row, rest of screen static
    static const char spin[] = "|/-\\";
    uint8_t frame = 0;
    ssd1306_clear_buffer();
    ssd1306_draw_header("WiFi Scan", "Scanning...");
    ssd1306_flush();

    while (!(xEventGroupGetBits(s_scan_done) & SCAN_DONE_BIT)) {
        char row[17];
        snprintf(row, sizeof(row), "  Scanning... %c", spin[frame++ & 3]);
        ssd1306_draw_string(0, 4, row);
        ssd1306_flush();
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    uint16_t count = MAX_SCAN_RESULTS;
    err = esp_wifi_scan_get_ap_records(&count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Get records failed: %s", esp_err_to_name(err));
        return 0;
    }

    result_count = count;
    for (uint16_t i = 0; i < count; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, 32);
        results[i].ssid[32] = '\0';
        results[i].channel  = ap_records[i].primary;
        results[i].rssi     = ap_records[i].rssi;
        results[i].security = (uint8_t)ap_records[i].authmode;
        results[i].cipher   = (uint8_t)ap_records[i].pairwise_cipher;
        results[i].wps      = ap_records[i].wps;
        results[i].phy_flags =
            (ap_records[i].phy_11b  ? WIFIEND_PHY_11B  : 0) |
            (ap_records[i].phy_11g  ? WIFIEND_PHY_11G  : 0) |
            (ap_records[i].phy_11n  ? WIFIEND_PHY_11N  : 0) |
            (ap_records[i].phy_11ax ? WIFIEND_PHY_11AX : 0);
        memcpy(results[i].bssid, ap_records[i].bssid, 6);
    }

    // Sort by RSSI descending (strongest first)
    for (uint16_t i = 0; i < result_count; i++) {
        for (uint16_t j = i + 1; j < result_count; j++) {
            if (results[j].rssi > results[i].rssi) {
                wifi_ap_info_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    ESP_LOGI(TAG, "Scan complete: %d APs", result_count);
    return result_count;
}

const wifi_ap_info_t *wifi_scan_get_results(uint16_t *count) {
    if (count) *count = result_count;
    return results;
}

void wifi_scan_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_scan_scroll_down(void) {
    if (result_count > 0 && selected_idx < result_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

static const char *cipher_label(uint8_t cipher) {
    switch (cipher) {
        case 0: return "No ";
        case 1: return "WEP";
        case 2: return "WEP";
        case 3: return "TKI";
        case 4: return "AES";
        case 5: return "T+A";
        case 6: return "ACM";
        case 8: return "GCM";
        case 9: return "G25";
        default: return "???";
    }
}

static const char *auth_label(uint8_t sec) {
    switch (sec) {
        case 0: return "OPEN";
        case 1: return "WEP ";
        case 2: return "WPA ";
        case 3: return "WPA2";
        case 4: return "WPA2";
        case 5: return "WPAE";
        case 6: return "WPA3";
        case 7: return "WP23";
        default: return "????";
    }
}

void wifi_scan_render(void) {
    ssd1306_clear_buffer();

    if (result_count == 0) {
        ssd1306_draw_header("WiFi Scan", "No APs found");
        ssd1306_draw_string(0, 3, " Long>menu");
        ssd1306_flush();
        return;
    }

    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, result_count,
             auth_label(results[selected_idx].security));
    ssd1306_draw_header("WiFi Scan", status);

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= result_count) break;

        const char *ssid = results[idx].ssid[0] ? results[idx].ssid : "Hidden";
        char line[17];
        snprintf(line, sizeof(line), "%c%-10.10s %4d",
                 (idx == selected_idx) ? '>' : ' ',
                 ssid,
                 (int)results[idx].rssi);
        ssd1306_draw_string(0, row + 2, line);
    }

    ssd1306_flush();
}

void wifi_scan_render_detail(void) {
    if (result_count == 0) return;
    wifi_ap_info_t *ap = &results[selected_idx];
    const char *ssid = ap->ssid[0] ? ap->ssid : "Hidden";

    ssd1306_clear_buffer();

    // Yellow zone: SSID — wraps to page 1 if > 16 chars, else nav hint on page 1
    ssd1306_draw_string(0, 0, ssid);
    if (strlen(ssid) > 16) {
        ssd1306_draw_string(0, 1, ssid + 16);
    } else {
        ssd1306_draw_string(0, 1, "Any>list LN>menu");
    }

    // Blue zone: AP details
    char row[17];

    // Band + channel
    snprintf(row, sizeof(row), "Band:%-4s Ch:%-3u",
             ap->channel > 14 ? "5GHz" : "2.4G", ap->channel);
    ssd1306_draw_string(0, 2, row);

    // BSSID — OUI (manufacturer bytes) and NIC (device bytes)
    snprintf(row, sizeof(row), "OUI:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2]);
    ssd1306_draw_string(0, 3, row);
    snprintf(row, sizeof(row), "NIC:%02X:%02X:%02X",
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    ssd1306_draw_string(0, 4, row);

    // RSSI
    snprintf(row, sizeof(row), "RSSI: %d dBm", (int)ap->rssi);
    ssd1306_draw_string(0, 5, row);

    // Security + pairwise cipher
    snprintf(row, sizeof(row), "Sec:%-4s Enc:%-3s",
             auth_label(ap->security), cipher_label(ap->cipher));
    ssd1306_draw_string(0, 6, row);

    // PHY modes (b/g/n/6=ax) + WPS
    char phy[5] = {0};
    int p = 0;
    if (ap->phy_flags & WIFIEND_PHY_11B)  phy[p++] = 'b';
    if (ap->phy_flags & WIFIEND_PHY_11G)  phy[p++] = 'g';
    if (ap->phy_flags & WIFIEND_PHY_11N)  phy[p++] = 'n';
    if (ap->phy_flags & WIFIEND_PHY_11AX) phy[p++] = '6';
    snprintf(row, sizeof(row), "PHY:%-4s WPS:%c",
             phy, ap->wps ? 'Y' : 'N');
    ssd1306_draw_string(0, 7, row);

    ssd1306_flush();
}
