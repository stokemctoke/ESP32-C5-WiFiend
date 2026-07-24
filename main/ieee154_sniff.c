#include "ieee154_sniff.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#if CONFIG_IEEE802154_ENABLED
#include "esp_ieee802154.h"
#endif

static const char *TAG = "ieee154";

#define HOP_DWELL_MS 300

static const uint8_t ieee154_channels[] = {
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
};
#define IEEE154_CH_COUNT (sizeof(ieee154_channels) / sizeof(ieee154_channels[0]))

static ieee154_sniff_stats_t   stats;
static ieee154_frame_summary_t last_frame;
static bool                    running = false;
static bool                    wifi_was_up = false;
static TaskHandle_t            hop_task_h = NULL;
static portMUX_TYPE            stats_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *frame_type_name(uint8_t t) {
    switch (t & 0x07) {
        case 0: return "Beacon";
        case 1: return "Data";
        case 2: return "Ack";
        case 3: return "Cmd";
        default: return "?";
    }
}

#if CONFIG_IEEE802154_ENABLED

static void parse_frame(const uint8_t *data, esp_ieee802154_frame_info_t *info,
                        ieee154_frame_summary_t *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return;

    uint8_t psdu_len = data[0];
    if (psdu_len < 5) return;

    uint16_t fc = (uint16_t)(data[1] | (data[2] << 8));
    out->frame_type = (uint8_t)(fc & 0x07);
    out->channel = info ? info->channel : 0;
    out->rssi    = info ? info->rssi : (int8_t)data[psdu_len];
    out->lqi     = info ? info->lqi : (uint8_t)data[psdu_len + 1];

    uint8_t dst_mode = (uint8_t)((fc >> 10) & 0x03);
    uint8_t src_mode = (uint8_t)((fc >> 14) & 0x03);
    uint8_t off = 4;

    if (dst_mode == 2) {
        if (off + 4 > psdu_len) return;
        out->dst_pan = (uint16_t)(data[off] | (data[off + 1] << 8));
        out->dst_short = (uint16_t)(data[off + 2] | (data[off + 3] << 8));
        out->has_dst_short = true;
        off += 4;
    } else if (dst_mode == 3) {
        if (off + 10 > psdu_len) return;
        out->dst_pan = (uint16_t)(data[off] | (data[off + 1] << 8));
        off += 2;
        memcpy(out->dst_ext, data + off, 8);
        out->has_dst_ext = true;
        off += 8;
    }

    bool pan_comp = (fc & (1 << 6)) != 0;
    if (src_mode == 2) {
        if (!pan_comp) {
            if (off + 2 > psdu_len) return;
            out->src_pan = (uint16_t)(data[off] | (data[off + 1] << 8));
            off += 2;
        } else if (out->has_dst_short || out->has_dst_ext) {
            out->src_pan = out->dst_pan;
        }
        if (off + 2 > psdu_len) return;
        out->src_short = (uint16_t)(data[off] | (data[off + 1] << 8));
        out->has_src_short = true;
    } else if (src_mode == 3) {
        if (!pan_comp) {
            if (off + 2 > psdu_len) return;
            out->src_pan = (uint16_t)(data[off] | (data[off + 1] << 8));
            off += 2;
        } else if (out->has_dst_short || out->has_dst_ext) {
            out->src_pan = out->dst_pan;
        }
        if (off + 8 > psdu_len) return;
        memcpy(out->src_ext, data + off, 8);
        out->has_src_ext = true;
    }
}

static void IRAM_ATTR rx_done_cb(uint8_t *data, esp_ieee802154_frame_info_t *frame_info) {
    ieee154_frame_summary_t summary;
    parse_frame(data, frame_info, &summary);

    portENTER_CRITICAL_ISR(&stats_mux);
    stats.total++;
    switch (summary.frame_type) {
        case 0: stats.beacon++; break;
        case 1: stats.data++; break;
        case 2: stats.ack++; break;
        case 3: stats.mac_cmd++; break;
        default: stats.other++; break;
    }
    last_frame = summary;
    portEXIT_CRITICAL_ISR(&stats_mux);

    esp_ieee802154_receive_handle_done(data);
    esp_ieee802154_receive();
}

static esp_ieee802154_event_cb_list_t s_cb_list = {
    .rx_done_cb = rx_done_cb,
};

static bool start_radio(void) {
    esp_err_t err = esp_ieee802154_event_callback_list_register(s_cb_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cb register: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ieee802154_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable: %s", esp_err_to_name(err));
        esp_ieee802154_event_callback_list_unregister();
        return false;
    }

    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_panid(0xFFFF);
    esp_ieee802154_set_short_address(0xFFFE);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_channel(ieee154_channels[0]);

    err = esp_ieee802154_receive();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receive: %s", esp_err_to_name(err));
        esp_ieee802154_disable();
        esp_ieee802154_event_callback_list_unregister();
        return false;
    }
    return true;
}

static void stop_radio(void) {
    esp_ieee802154_disable();
    esp_ieee802154_event_callback_list_unregister();
}

#endif

static void hop_task(void *arg) {
    (void)arg;
    uint8_t idx = 0;

#if CONFIG_IEEE802154_ENABLED
    while (running) {
        uint8_t ch = ieee154_channels[idx];
        idx = (uint8_t)((idx + 1) % IEEE154_CH_COUNT);
        esp_ieee802154_set_channel(ch);
        esp_ieee802154_receive();
        vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_MS));
    }
#endif
    hop_task_h = NULL;
    vTaskDelete(NULL);
}

void ieee154_sniff_init(void) {
#if !CONFIG_IEEE802154_ENABLED
    ESP_LOGW(TAG, "IEEE802154 disabled in sdkconfig");
#else
    ESP_LOGI(TAG, "802.15.4 sniff init OK");
#endif
}

void ieee154_sniff_enter(void) {
    ESP_LOGI(TAG, "802.15.4 sniff enter (WiFi must be stopped)");
}

void ieee154_sniff_start(void) {
    if (running) return;

#if !CONFIG_IEEE802154_ENABLED
    ESP_LOGE(TAG, "IEEE802154 not enabled");
    return;
#endif

    if (!radio_mgr_enter(RADIO_MODE_IEEE154)) {
        ESP_LOGE(TAG, "radio_mgr_enter failed");
        return;
    }

    wifi_was_up = (esp_wifi_stop() == ESP_OK);
    ESP_LOGI(TAG, "WiFi %s for 802.15.4", wifi_was_up ? "stopped" : "already down");

#if CONFIG_IEEE802154_ENABLED
    portENTER_CRITICAL(&stats_mux);
    memset(&stats, 0, sizeof(stats));
    memset(&last_frame, 0, sizeof(last_frame));
    portEXIT_CRITICAL(&stats_mux);

    if (!start_radio()) {
        if (wifi_was_up) esp_wifi_start();
        radio_mgr_leave(RADIO_MODE_IEEE154);
        return;
    }
#endif

    running = true;
    xTaskCreate(hop_task, "i154_hop", 3072, NULL, 4, &hop_task_h);
    ESP_LOGI(TAG, "802.15.4 sniff started");
}

void ieee154_sniff_stop(void) {
    if (!running) return;

    running = false;
    vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_MS + 50));

#if CONFIG_IEEE802154_ENABLED
    stop_radio();
#endif

    if (wifi_was_up) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        }
        wifi_was_up = false;
    }

    radio_mgr_leave(RADIO_MODE_IEEE154);
    ESP_LOGI(TAG, "802.15.4 sniff stopped — %lu frames",
             (unsigned long)stats.total);
}

bool ieee154_sniff_is_running(void) {
    return running;
}

const ieee154_sniff_stats_t *ieee154_sniff_get_stats(void) {
    return &stats;
}

const ieee154_frame_summary_t *ieee154_sniff_get_last_frame(void) {
    return &last_frame;
}

void ieee154_sniff_render(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("802.15.4", running ? "RX" : "OFF");

    char line[24];
    snprintf(line, sizeof(line), "Tot:%lu B:%lu D:%lu",
             (unsigned long)stats.total,
             (unsigned long)stats.beacon,
             (unsigned long)stats.data);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "Ack:%lu Cmd:%lu",
             (unsigned long)stats.ack,
             (unsigned long)stats.mac_cmd);
    ssd1306_draw_string(0, 3, line);

    if (stats.total == 0) {
        ssd1306_draw_string(0, 5, running ? "  Listening ch11-26" : "  Not running");
        ssd1306_flush();
        return;
    }

    snprintf(line, sizeof(line), "Last:%s ch%02u r%4d",
             frame_type_name(last_frame.frame_type),
             (unsigned)last_frame.channel,
             (int)last_frame.rssi);
    ssd1306_draw_string(0, 5, line);

    if (last_frame.has_src_short) {
        snprintf(line, sizeof(line), "Src:%04X pan%04X",
                 (unsigned)last_frame.src_short,
                 (unsigned)last_frame.src_pan);
    } else if (last_frame.has_dst_short) {
        snprintf(line, sizeof(line), "Dst:%04X pan%04X",
                 (unsigned)last_frame.dst_short,
                 (unsigned)last_frame.dst_pan);
    } else {
        snprintf(line, sizeof(line), "PAN dst%04X src%04X",
                 (unsigned)last_frame.dst_pan,
                 (unsigned)last_frame.src_pan);
    }
    ssd1306_draw_string(0, 6, line);
    ssd1306_flush();
}
