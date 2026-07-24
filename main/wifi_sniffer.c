#include "wifi_sniffer.h"
#include "oui_lookup.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_sniff";

// 802.11 frame header — enough to identify type and extract addresses
typedef struct {
    uint8_t frame_ctrl[2];
    uint8_t duration[2];
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint8_t seq_ctrl[2];
} __attribute__((packed)) ieee80211_hdr_t;

#define FC_TYPE(fc0)    (((fc0) >> 2) & 0x03)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)
#define FC_TO_DS(fc1)   ((fc1) & 0x01)
#define FC_FROM_DS(fc1) (((fc1) >> 1) & 0x01)

#define TYPE_MGMT 0
#define TYPE_DATA 2
#define SUBTYPE_PROBE_REQ   4
#define SUBTYPE_ASSOC_REQ   0
#define SUBTYPE_REASSOC_REQ 2

// 2.4 GHz + 5 GHz — hop through all supported channels (dual-band scan)
static const uint8_t hop_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
    149, 153, 157, 161, 165,
};
#define HOP_COUNT     (sizeof(hop_channels) / sizeof(hop_channels[0]))
#define HOP_DWELL_24  400
#define HOP_DWELL_5   300

static wifi_client_t clients[MAX_SNIFF_CLIENTS];
static uint16_t      client_count   = 0;
static uint16_t      selected_idx   = 0;
static uint16_t      scroll_offset  = 0;
static uint8_t       current_chan   = 1;
static bool          sniff_running  = false;

static SemaphoreHandle_t client_mutex = NULL;
static TaskHandle_t      hop_task_h   = NULL;

static bool is_multicast(const uint8_t *mac) {
    return (mac[0] & 0x01) != 0;
}

static bool mac_is_zero(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) if (mac[i]) return false;
    return true;
}

// Returns index of existing entry or -1
static int find_client(const uint8_t *mac) {
    for (int i = 0; i < client_count; i++) {
        if (memcmp(clients[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void record_client(const uint8_t *mac, const uint8_t *ap_bssid,
                           int8_t rssi, bool associated) {
    if (is_multicast(mac)) return;

    xSemaphoreTake(client_mutex, portMAX_DELAY);

    int idx = find_client(mac);
    if (idx < 0) {
        if (client_count >= MAX_SNIFF_CLIENTS) {
            xSemaphoreGive(client_mutex);
            return;
        }
        idx = client_count++;
        memcpy(clients[idx].mac, mac, 6);
        clients[idx].frame_count = 0;
        clients[idx].associated  = false;
        memset(clients[idx].ap_bssid, 0, 6);
    }

    clients[idx].rssi        = rssi;
    clients[idx].channel     = current_chan;
    clients[idx].frame_count++;

    // Upgrade from probe-only to associated if we see data/assoc frames
    if (associated && ap_bssid && !mac_is_zero(ap_bssid)) {
        clients[idx].associated = true;
        memcpy(clients[idx].ap_bssid, ap_bssid, 6);
    }

    xSemaphoreGive(client_mutex);
}

static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < (int)sizeof(ieee80211_hdr_t)) return;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)pkt->payload;
    uint8_t  fc0  = hdr->frame_ctrl[0];
    uint8_t  fc1  = hdr->frame_ctrl[1];
    uint8_t  ftype = FC_TYPE(fc0);
    uint8_t  fsub  = FC_SUBTYPE(fc0);
    int8_t   rssi  = pkt->rx_ctrl.rssi;

    if (ftype == TYPE_MGMT) {
        if (fsub == SUBTYPE_PROBE_REQ) {
            // addr2 = client, no AP yet
            record_client(hdr->addr2, NULL, rssi, false);
        } else if (fsub == SUBTYPE_ASSOC_REQ || fsub == SUBTYPE_REASSOC_REQ) {
            // addr2 = client, addr3 = AP BSSID
            record_client(hdr->addr2, hdr->addr3, rssi, true);
        }
    } else if (ftype == TYPE_DATA) {
        if (FC_TO_DS(fc1) && !FC_FROM_DS(fc1)) {
            // Client → AP: addr2=client, addr1=AP BSSID
            record_client(hdr->addr2, hdr->addr1, rssi, true);
        }
        // FromDS frames (AP→client) reveal client addr1, but we can't confirm
        // association direction cleanly without more state — skip for now.
    }
}

static void hop_task(void *arg) {
    uint8_t hop_idx = 0;
    while (sniff_running) {
        current_chan = hop_channels[hop_idx];
        esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
        hop_idx = (hop_idx + 1) % HOP_COUNT;
        uint16_t dwell = (current_chan <= 14) ? HOP_DWELL_24 : HOP_DWELL_5;
        vTaskDelay(pdMS_TO_TICKS(dwell));
    }
    vTaskDelete(NULL);
}

void wifi_sniff_init(void) {
    if (!client_mutex) {
        client_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Sniffer init OK");
}

void wifi_sniff_start(void) {
    client_count  = 0;
    selected_idx  = 0;
    scroll_offset = 0;
    memset(clients, 0, sizeof(clients));

    sniff_running = true;

    esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
    esp_wifi_set_promiscuous(true);

    xTaskCreate(hop_task, "sniff_hop", 2048, NULL, 4, &hop_task_h);

    ESP_LOGI(TAG, "Sniffer started");
}

void wifi_sniff_stop(void) {
    sniff_running = false;
    // hop_task exits on its own; allow one full dwell period
    vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_24 + 100));
    hop_task_h = NULL;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    ESP_LOGI(TAG, "Sniffer stopped — %d clients", client_count);
}

uint16_t wifi_sniff_get_count(void) {
    return client_count;
}

const wifi_client_t *wifi_sniff_get_client(uint16_t idx) {
    if (idx >= client_count) return NULL;
    return &clients[idx];
}

void wifi_sniff_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_sniff_scroll_down(void) {
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    uint16_t count = client_count;
    xSemaphoreGive(client_mutex);

    if (count > 0 && selected_idx < count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

void wifi_sniff_render(void) {
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    uint16_t count = client_count;

    char status[20];
    const char *band = (current_chan > 14) ? "5G" : "2G";
    snprintf(status, sizeof(status), "Ch:%3u %s %2u",
             current_chan, band, count);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Client Sniff", status);

    if (count == 0) {
        ssd1306_draw_string(0, 3, " Listening...");
        xSemaphoreGive(client_mutex);
        ssd1306_flush();
        return;
    }

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= count) break;

        // Show last 3 MAC bytes, RSSI, association indicator
        char line[17];
        snprintf(line, sizeof(line), "%c%02X:%02X:%02X %4d%s",
                 (idx == selected_idx) ? '>' : ' ',
                 clients[idx].mac[3], clients[idx].mac[4], clients[idx].mac[5],
                 (int)clients[idx].rssi,
                 clients[idx].associated ? "A" : "P");
        ssd1306_draw_string(0, row + 2, line);
    }

    xSemaphoreGive(client_mutex);
    ssd1306_flush();
}

void wifi_sniff_render_detail(void) {
    xSemaphoreTake(client_mutex, portMAX_DELAY);
    if (client_count == 0) {
        xSemaphoreGive(client_mutex);
        return;
    }
    wifi_client_t c = clients[selected_idx];   // local copy — release lock fast
    xSemaphoreGive(client_mutex);

    ssd1306_clear_buffer();

    // Yellow zone: MAC split across two rows (full MAC is 17 chars, display is 16)
    char line[20];
    snprintf(line, sizeof(line), "%02X:%02X:%02X",
             c.mac[0], c.mac[1], c.mac[2]);
    ssd1306_draw_string(0, 0, line);
    const char *vendor = oui_lookup(c.mac);
    if (vendor) {
        snprintf(line, sizeof(line), "%.12s LN>bk", vendor);
    } else {
        snprintf(line, sizeof(line), "%02X:%02X:%02X LN>bk",
                 c.mac[3], c.mac[4], c.mac[5]);
    }
    ssd1306_draw_string(0, 1, line);

    // Blue zone
    snprintf(line, sizeof(line), "RSSI: %d dBm", (int)c.rssi);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "Ch:%u  Frm:%u", c.channel, c.frame_count);
    ssd1306_draw_string(0, 3, line);

    if (c.associated) {
        snprintf(line, sizeof(line), "AP %02X:%02X:%02X",
                 c.ap_bssid[0], c.ap_bssid[1], c.ap_bssid[2]);
        ssd1306_draw_string(0, 4, line);
        snprintf(line, sizeof(line), "   %02X:%02X:%02X",
                 c.ap_bssid[3], c.ap_bssid[4], c.ap_bssid[5]);
        ssd1306_draw_string(0, 5, line);
    } else {
        ssd1306_draw_string(0, 4, "Probe only");
        ssd1306_draw_string(0, 5, "No AP seen");
    }

    snprintf(line, sizeof(line), "%s",
             c.associated ? "Status: Assoc'd" : "Status: Probing");
    ssd1306_draw_string(0, 6, line);

    ssd1306_flush();
}
