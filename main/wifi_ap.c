#include "wifi_ap.h"
#include "wifi_scan.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_ap";

#define MAX_AP_CLIENTS  4
#define AP_IP           "192.168.4.1"
#define NEW_CLIENT_SECS 15      // how long a client shows [NEW]

typedef struct {
    uint8_t mac[6];
    int64_t connected_at_us;
} ap_client_t;

typedef enum {
    AP_STATE_PICK = 0,
    AP_STATE_RUNNING,
} ap_state_t;

static ap_state_t      state         = AP_STATE_PICK;
static ap_client_t     clients[MAX_AP_CLIENTS];
static uint8_t         client_count  = 0;
static SemaphoreHandle_t client_mutex = NULL;

static wifi_ap_info_t  targets[MAX_SCAN_RESULTS];
static uint16_t        target_count  = 0;
static uint16_t        scroll_offset = 0;
static uint16_t        selected_idx  = 0;

static char    ap_ssid[33];
static uint8_t ap_channel;

// ---------- WiFi event handlers ----------

static void on_client_connect(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    if (client_count < MAX_AP_CLIENTS) {
        memcpy(clients[client_count].mac, ev->mac, 6);
        clients[client_count].connected_at_us = esp_timer_get_time();
        client_count++;
        ESP_LOGI(TAG, "Client connected: %02X:%02X:%02X:%02X:%02X:%02X",
                 ev->mac[0], ev->mac[1], ev->mac[2],
                 ev->mac[3], ev->mac[4], ev->mac[5]);
    }
    xSemaphoreGive(client_mutex);
}

static void on_client_disconnect(void *arg, esp_event_base_t base,
                                  int32_t id, void *data) {
    wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < client_count; i++) {
        if (memcmp(clients[i].mac, ev->mac, 6) == 0) {
            memmove(&clients[i], &clients[i + 1],
                    (client_count - i - 1) * sizeof(ap_client_t));
            client_count--;
            ESP_LOGI(TAG, "Client disconnected");
            break;
        }
    }
    xSemaphoreGive(client_mutex);
}

// ---------- public API ----------

void wifi_ap_init(void) {
    if (!client_mutex) client_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "AP module ready");
}

void wifi_ap_enter(void) {
    state         = AP_STATE_PICK;
    scroll_offset = 0;
    selected_idx  = 0;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
        return;
    }

    // No scan results — auto-scan with cheeky notice (same as deauth)
    ssd1306_clear_buffer();
    ssd1306_draw_header("AP Mode", "lazy scan...");
    ssd1306_draw_string(0, 3, "Too lazy 2 scan");
    ssd1306_draw_string(0, 4, "first? Fine...");
    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));

    wifi_scan_start();

    scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
    }
}

void wifi_ap_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_ap_scroll_down(void) {
    if (target_count > 0 && selected_idx < target_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

void wifi_ap_select(void) {
    if (target_count == 0) return;

    strncpy(ap_ssid, targets[selected_idx].ssid, 32);
    ap_ssid[32] = '\0';
    if (!ap_ssid[0]) strncpy(ap_ssid, "Free_WiFi", 32);    // fallback for hidden APs
    ap_channel = targets[selected_idx].channel;

    // Switch WiFi to AP mode — clone the selected SSID, open network
    esp_wifi_stop();

    wifi_config_t cfg = { 0 };
    memcpy(cfg.ap.ssid, ap_ssid, strlen(ap_ssid));
    cfg.ap.ssid_len      = (uint8_t)strlen(ap_ssid);
    cfg.ap.channel       = ap_channel;
    cfg.ap.authmode      = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = MAX_AP_CLIENTS;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                on_client_connect, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                on_client_disconnect, NULL);

    xSemaphoreTake(client_mutex, portMAX_DELAY);
    client_count = 0;
    memset(clients, 0, sizeof(clients));
    xSemaphoreGive(client_mutex);

    state = AP_STATE_RUNNING;
    ESP_LOGI(TAG, "Evil twin up: \"%s\" ch%u", ap_ssid, ap_channel);
}

void wifi_ap_stop(void) {
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                  on_client_connect);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                  on_client_disconnect);

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    xSemaphoreTake(client_mutex, portMAX_DELAY);
    client_count = 0;
    xSemaphoreGive(client_mutex);

    state = AP_STATE_PICK;
    ESP_LOGI(TAG, "AP stopped — WiFi restored to STA mode");
}

bool wifi_ap_is_running(void) {
    return state == AP_STATE_RUNNING;
}

// ---------- display ----------

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
        ssd1306_draw_header("AP Mode", "No targets");
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_flush();
        return;
    }

    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, target_count,
             auth_short(targets[selected_idx].security));
    ssd1306_draw_header("AP Mode", status);

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

    ssd1306_draw_header("AP Mode", ap_ssid[0] ? ap_ssid : "Free_WiFi");

    xSemaphoreTake(client_mutex, portMAX_DELAY);
    uint8_t  count      = client_count;
    ap_client_t snap[MAX_AP_CLIENTS];
    memcpy(snap, clients, sizeof(ap_client_t) * count);
    xSemaphoreGive(client_mutex);

    // Row 2: IP address
    ssd1306_draw_string(0, 2, "IP: " AP_IP);

    // Row 3: channel + client count
    char line[24];
    snprintf(line, sizeof(line), "Ch:%-2u  %u cli", ap_channel, count);
    ssd1306_draw_string(0, 3, line);

    // Rows 4–6: connected clients (last 3 MAC bytes + age indicator)
    if (count == 0) {
        ssd1306_draw_string(0, 4, " Waiting...");
    } else {
        int64_t now = esp_timer_get_time();
        for (uint8_t i = 0; i < count && i < 3; i++) {
            int64_t raw_age = (now - snap[i].connected_at_us) / 1000000LL;
            if (raw_age < 0) raw_age = 0;
            uint32_t age_s = (uint32_t)raw_age;
            char age[8];
            if (age_s < 60) {
                snprintf(age, sizeof(age), "%2us", (unsigned)(age_s % 100));
            } else {
                snprintf(age, sizeof(age), "%2um", (unsigned)((age_s / 60) % 100));
            }
            snprintf(line, sizeof(line), " %02X:%02X:%02X %s%s",
                     snap[i].mac[3], snap[i].mac[4], snap[i].mac[5],
                     age_s < NEW_CLIENT_SECS ? "[NEW]" : "     ",
                     age);
            ssd1306_draw_string(0, 4 + i, line);
        }
    }

    ssd1306_draw_string(0, 7, "LN>stop");
    ssd1306_flush();
}

void wifi_ap_render(void) {
    if (state == AP_STATE_RUNNING) {
        render_running();
    } else {
        render_picker();
    }
}
