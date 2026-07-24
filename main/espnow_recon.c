#include "espnow_recon.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "espnow";

#define LOG_PATH       "/lfs/espnow.log"
#define LOG_CAP_BYTES  32768
#define HOP_DWELL_MS   250

static const uint8_t hop_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
};
#define HOP_COUNT (sizeof(hop_channels) / sizeof(hop_channels[0]))

static espnow_peer_t peers[ESPNOW_RECON_MAX_PEERS];
static uint16_t      peer_count    = 0;
static uint16_t      selected_idx  = 0;
static uint16_t      scroll_offset = 0;
static bool          running       = false;
static bool          stack_ready   = false;
static bool          log_enabled   = true;

static SemaphoreHandle_t data_mutex = NULL;
static TaskHandle_t      hop_task_h = NULL;

static void rotate_log_if_needed(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    if (sz < LOG_CAP_BYTES) return;

    char oldpath[48];
    snprintf(oldpath, sizeof(oldpath), "%s.old", path);
    remove(oldpath);
    rename(path, oldpath);
}

static void append_peer_log(const espnow_peer_t *p) {
    if (!log_enabled) return;
    rotate_log_if_needed(LOG_PATH);
    FILE *fp = fopen(LOG_PATH, "a");
    if (!fp) return;
    fprintf(fp, "%02X:%02X:%02X:%02X:%02X:%02X,%d,%lu\n",
            p->mac[0], p->mac[1], p->mac[2],
            p->mac[3], p->mac[4], p->mac[5],
            (int)p->rssi, (unsigned long)p->pkt_count);
    fclose(fp);
}

static int find_peer(const uint8_t *mac) {
    for (int i = 0; i < (int)peer_count; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void record_peer(const uint8_t *mac, int8_t rssi) {
    xSemaphoreTake(data_mutex, portMAX_DELAY);

    int idx = find_peer(mac);
    if (idx < 0) {
        if (peer_count >= ESPNOW_RECON_MAX_PEERS) {
            xSemaphoreGive(data_mutex);
            return;
        }
        idx = (int)peer_count++;
        memset(&peers[idx], 0, sizeof(peers[idx]));
        memcpy(peers[idx].mac, mac, 6);
        peers[idx].logged = false;
    }

    peers[idx].rssi         = rssi;
    peers[idx].pkt_count++;
    peers[idx].last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (!peers[idx].logged) {
        peers[idx].logged = true;
        append_peer_log(&peers[idx]);
    }

    xSemaphoreGive(data_mutex);
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len) {
    (void)data;
    (void)len;
    if (!info || !info->src_addr) return;

    int8_t rssi = -128;
    if (info->rx_ctrl) rssi = info->rx_ctrl->rssi;

    record_peer(info->src_addr, rssi);
}

static bool ensure_wifi_sta(void) {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed");
            return false;
        }
    }

    if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_STA) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            ESP_LOGE(TAG, "set STA mode failed");
            return false;
        }
    }

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return false;
    }
    return true;
}

static bool ensure_broadcast_peer(void) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (esp_now_is_peer_exist(bcast)) return true;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, bcast, 6);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add broadcast peer: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ensure_stack(void) {
    if (stack_ready) return true;

    if (!ensure_wifi_sta()) return false;

    esp_err_t err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register recv cb: %s", esp_err_to_name(err));
        return false;
    }

    if (!ensure_broadcast_peer()) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return false;
    }

    stack_ready = true;
    return true;
}

static void hop_task(void *arg) {
    (void)arg;
    uint8_t hop_idx = 0;

    while (running) {
        uint8_t ch = hop_channels[hop_idx];
        hop_idx = (uint8_t)((hop_idx + 1) % HOP_COUNT);

        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_MS));
    }
    hop_task_h = NULL;
    vTaskDelete(NULL);
}

void espnow_recon_init(void) {
    if (!data_mutex) {
        data_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "ESP-NOW recon init OK");
}

void espnow_recon_enter(void) {
    selected_idx  = 0;
    scroll_offset = 0;
    ESP_LOGI(TAG, "ESP-NOW recon enter");
}

void espnow_recon_start(void) {
    if (running) return;
    if (!radio_mgr_enter(RADIO_MODE_ESPNOW)) {
        ESP_LOGE(TAG, "radio_mgr_enter failed");
        return;
    }
    if (!ensure_stack()) {
        radio_mgr_leave(RADIO_MODE_ESPNOW);
        return;
    }

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    peer_count    = 0;
    selected_idx  = 0;
    scroll_offset = 0;
    memset(peers, 0, sizeof(peers));
    xSemaphoreGive(data_mutex);

    running = true;
    xTaskCreate(hop_task, "espnow_hop", 3072, NULL, 4, &hop_task_h);
    ESP_LOGI(TAG, "ESP-NOW recon started");
}

void espnow_recon_stop(void) {
    if (!running) return;

    running = false;
    vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_MS + 50));

    if (stack_ready) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        stack_ready = false;
    }

    radio_mgr_leave(RADIO_MODE_ESPNOW);
    ESP_LOGI(TAG, "ESP-NOW recon stopped — %u peers", (unsigned)peer_count);
}

bool espnow_recon_is_running(void) {
    return running;
}

void espnow_recon_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    }
}

void espnow_recon_scroll_down(void) {
    if (peer_count == 0) return;
    if (selected_idx + 1 < peer_count) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
    }
}

static void fmt_mac(const uint8_t *mac, char *out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void espnow_recon_render(void) {
    char status[24];
    snprintf(status, sizeof(status), "%u/%u",
             peer_count ? (unsigned)(selected_idx + 1) : 0,
             (unsigned)peer_count);

    ssd1306_clear_buffer();
    ssd1306_draw_header("ESP-NOW", status);

    if (peer_count == 0) {
        ssd1306_draw_string(0, 4, running ? "  Listening..." : "  Stopped");
        ssd1306_flush();
        return;
    }

    if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;

    for (uint16_t r = 0; r < 6 && (scroll_offset + r) < peer_count; r++) {
        uint16_t i = scroll_offset + r;
        char mac[18];
        fmt_mac(peers[i].mac, mac, sizeof(mac));
        char row[24];
        snprintf(row, sizeof(row), "%c%-11.11s%4d",
                 (i == selected_idx) ? '>' : ' ',
                 mac + 9, peers[i].rssi);
        ssd1306_draw_string(0, 2 + r, row);
    }
    ssd1306_flush();
}

uint16_t espnow_recon_peer_count(void) {
    return peer_count;
}

const espnow_peer_t *espnow_recon_get_peer(uint16_t idx) {
    if (idx >= peer_count) return NULL;
    return &peers[idx];
}
