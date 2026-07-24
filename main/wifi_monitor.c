#include "wifi_monitor.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_mon";

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
#define SUBTYPE_BEACON     8
#define SUBTYPE_PROBE_REQ  4

#define MGMT_HDR_LEN 24
#define BEACON_FIXED_LEN 12

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

#define LOG_CAP_BYTES      32768
typedef enum {
    MON_VIEW_BEACONS = 0,
    MON_VIEW_PROBES,
    MON_VIEW_EAPOL,
    MON_VIEW_ACTIVITY,
} monitor_view_t;

static monitor_beacon_t beacons[MAX_MONITOR_BEACONS];
static monitor_probe_t  probes[MAX_MONITOR_PROBES];
static monitor_eapol_t  eapol_counts;
static uint32_t         chan_activity[HOP_COUNT];

static uint16_t      beacon_count  = 0;
static uint16_t      probe_count   = 0;
static uint16_t      selected_idx  = 0;
static uint16_t      scroll_offset = 0;
static monitor_view_t current_view = MON_VIEW_BEACONS;
static uint8_t       current_chan  = 1;
static bool          mon_running   = false;

#define PROBE_LOG_Q  8
static monitor_probe_t probe_log_q[PROBE_LOG_Q];
static uint8_t         probe_log_q_len = 0;

static SemaphoreHandle_t data_mutex = NULL;
static TaskHandle_t      hop_task_h = NULL;

// ---------- tag / IE helpers ----------

static bool oui_is_wpa(const uint8_t *data, uint8_t len) {
    return len >= 4 &&
           data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2 && data[3] == 0x01;
}

static bool rsn_has_wpa3_akm(const uint8_t *rsn, uint8_t len) {
    if (len < 8) return false;
    uint16_t off = 2 + 4;  // version + group cipher
    if (off + 2 > len) return false;
    uint16_t pw = (uint16_t)(rsn[off] | (rsn[off + 1] << 8));
    off += 2 + pw * 4;
    if (off + 2 > len) return false;
    uint16_t ak = (uint16_t)(rsn[off] | (rsn[off + 1] << 8));
    off += 2;
    for (uint16_t i = 0; i < ak && off + 4 <= len; i++, off += 4) {
        if (rsn[off] == 0x00 && rsn[off + 1] == 0x0F &&
            rsn[off + 2] == 0xAC && rsn[off + 3] == 0x08) {
            return true;
        }
    }
    return false;
}

static void parse_tags(const uint8_t *tags, uint16_t tags_len,
                       char *ssid_out, bool *hidden_out, uint8_t *chan_out,
                       uint8_t *sec_out, bool have_cap, uint16_t capability) {
    bool ssid_seen = false;

    if (have_cap && (capability & 0x0010)) {
        *sec_out |= MON_SEC_WEP;
    }

    for (uint16_t i = 0; i + 2 <= tags_len; ) {
        uint8_t tag = tags[i];
        uint8_t len = tags[i + 1];
        if ((uint16_t)(i + 2 + len) > tags_len) break;
        const uint8_t *data = tags + i + 2;

        if (tag == 0) {
            ssid_seen = true;
            if (len == 0) {
                *hidden_out = true;
                ssid_out[0] = '\0';
            } else {
                uint8_t copy = len > 32 ? 32 : len;
                memcpy(ssid_out, data, copy);
                ssid_out[copy] = '\0';
                *hidden_out = false;
            }
        } else if (tag == 3 && len >= 1 && chan_out) {
            *chan_out = data[0];
        } else if (tag == 48 && len >= 2) {
            *sec_out |= MON_SEC_WPA2;
            if (rsn_has_wpa3_akm(data, len)) {
                *sec_out |= MON_SEC_WPA3;
            }
        } else if (tag == 221 && oui_is_wpa(data, len)) {
            *sec_out |= MON_SEC_WPA;
        }

        i += 2 + len;
    }

    if (!ssid_seen) {
        *hidden_out = true;
        ssid_out[0] = '\0';
    }
}

static const char *sec_short(uint8_t flags) {
    if (flags & MON_SEC_WPA3) return "W3";
    if (flags & MON_SEC_WPA2) return "W2";
    if (flags & MON_SEC_WPA)  return "WP";
    if (flags & MON_SEC_WEP)  return "WE";
    return "OP";
}

// ---------- LittleFS logging ----------

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

static void append_beacon_log(const monitor_beacon_t *b) {
    rotate_log_if_needed("/lfs/beacons.log");
    FILE *fp = fopen("/lfs/beacons.log", "a");
    if (!fp) return;
    fprintf(fp, "%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%u,%d,0x%02X\n",
            b->bssid[0], b->bssid[1], b->bssid[2],
            b->bssid[3], b->bssid[4], b->bssid[5],
            b->hidden ? "" : b->ssid,
            (unsigned)b->channel, (int)b->rssi, b->sec_flags);
    fclose(fp);
}

static void append_probe_log(const monitor_probe_t *p) {
    rotate_log_if_needed("/lfs/probes.log");
    FILE *fp = fopen("/lfs/probes.log", "a");
    if (!fp) return;
    fprintf(fp, "%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%d,%u\n",
            p->client_mac[0], p->client_mac[1], p->client_mac[2],
            p->client_mac[3], p->client_mac[4], p->client_mac[5],
            p->ssid[0] ? p->ssid : "*",
            (int)p->rssi, (unsigned)p->channel);
    fclose(fp);
}

static void queue_probe_log(const monitor_probe_t *p) {
    if (probe_log_q_len >= PROBE_LOG_Q) return;
    probe_log_q[probe_log_q_len++] = *p;
}

static void flush_probe_log_queue(void) {
    while (probe_log_q_len > 0) {
        append_probe_log(&probe_log_q[0]);
        probe_log_q_len--;
        if (probe_log_q_len > 0) {
            memmove(probe_log_q, probe_log_q + 1,
                    probe_log_q_len * sizeof(probe_log_q[0]));
        }
    }
}

// ---------- record stores ----------

static int find_beacon(const uint8_t *bssid) {
    for (int i = 0; i < (int)beacon_count; i++) {
        if (memcmp(beacons[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void record_beacon(const uint8_t *bssid, int8_t rssi,
                          const char *ssid, bool hidden, uint8_t channel,
                          uint8_t sec_flags) {
    xSemaphoreTake(data_mutex, portMAX_DELAY);

    int idx = find_beacon(bssid);
    if (idx < 0) {
        if (beacon_count >= MAX_MONITOR_BEACONS) {
            xSemaphoreGive(data_mutex);
            return;
        }
        idx = (int)beacon_count++;
        memcpy(beacons[idx].bssid, bssid, 6);
        beacons[idx].logged = false;
    }

    monitor_beacon_t *b = &beacons[idx];
    b->rssi      = rssi;
    b->channel   = channel ? channel : current_chan;
    b->hidden    = hidden;
    b->sec_flags = sec_flags;
    strncpy(b->ssid, ssid, 32);
    b->ssid[32] = '\0';

    bool new_bssid = !b->logged;
    if (new_bssid) {
        b->logged = true;
        xSemaphoreGive(data_mutex);
        append_beacon_log(b);
        return;
    }

    xSemaphoreGive(data_mutex);
}

static int find_probe(const uint8_t *mac, const char *ssid) {
    for (int i = 0; i < (int)probe_count; i++) {
        if (memcmp(probes[i].client_mac, mac, 6) == 0 &&
            strcmp(probes[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void record_probe(const uint8_t *mac, const char *ssid, int8_t rssi) {
    if ((mac[0] & 0x01) != 0) return;

    xSemaphoreTake(data_mutex, portMAX_DELAY);

    int idx = find_probe(mac, ssid);
    bool is_new = (idx < 0);
    if (is_new) {
        if (probe_count >= MAX_MONITOR_PROBES) {
            xSemaphoreGive(data_mutex);
            return;
        }
        idx = (int)probe_count++;
        memcpy(probes[idx].client_mac, mac, 6);
        strncpy(probes[idx].ssid, ssid, 32);
        probes[idx].ssid[32] = '\0';
        probes[idx].count = 0;
    }

    probes[idx].rssi    = rssi;
    probes[idx].channel = current_chan;
    probes[idx].count++;

    if (is_new) {
        monitor_probe_t snapshot = probes[idx];
        queue_probe_log(&snapshot);
    }

    xSemaphoreGive(data_mutex);
}

static void bump_channel_activity(void) {
    for (uint8_t i = 0; i < HOP_COUNT; i++) {
        if (hop_channels[i] == current_chan) {
            chan_activity[i]++;
            return;
        }
    }
}

static void count_eapol_frame(const uint8_t *frame, uint16_t frame_len) {
    uint8_t from_ds = FC_FROM_DS(frame[1]);
    uint8_t to_ds   = FC_TO_DS(frame[1]);
    if (from_ds == to_ds) return;

    uint8_t hdr_len = (FC_SUBTYPE(frame[0]) & 0x08) ? 26 : 24;
    if (frame_len < (uint16_t)(hdr_len + 8 + 99)) return;

    const uint8_t *llc = frame + hdr_len;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[6] != 0x88 || llc[7] != 0x8E) return;

    const uint8_t *eapol = llc + 8;
    if (eapol[1] != 3) return;
    if (eapol[4] != 2) return;

    uint16_t key_info = (uint16_t)((eapol[5] << 8) | eapol[6]);
    bool ack  = (key_info & 0x0080) != 0;
    bool mic  = (key_info & 0x0100) != 0;
    bool inst = (key_info & 0x0040) != 0;
    bool pair = (key_info & 0x0008) != 0;
    if (!pair) return;

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (from_ds && ack && !mic) {
        eapol_counts.m1++;
    } else if (to_ds && !ack && mic && !inst) {
        eapol_counts.m2++;
    } else if (from_ds && ack && mic && inst) {
        eapol_counts.m3++;
    } else if (to_ds && !ack && mic && inst) {
        eapol_counts.m4++;
    }
    xSemaphoreGive(data_mutex);
}

// ---------- promiscuous callback ----------

static void handle_beacon(const uint8_t *frame, uint16_t frame_len, int8_t rssi) {
    if (frame_len < MGMT_HDR_LEN + BEACON_FIXED_LEN + 2) return;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)frame;
    const uint8_t *body = frame + MGMT_HDR_LEN;
    uint16_t cap = (uint16_t)(body[10] | (body[11] << 8));
    const uint8_t *tags = body + BEACON_FIXED_LEN;
    uint16_t tags_len = frame_len - MGMT_HDR_LEN - BEACON_FIXED_LEN;

    char ssid[33];
    bool hidden = false;
    uint8_t channel = 0;
    uint8_t sec = 0;
    parse_tags(tags, tags_len, ssid, &hidden, &channel, &sec, true, cap);

    record_beacon(hdr->addr3, rssi, ssid, hidden, channel, sec);
}

static void handle_probe_req(const uint8_t *frame, uint16_t frame_len, int8_t rssi) {
    if (frame_len < MGMT_HDR_LEN + 2) return;

    const ieee80211_hdr_t *hdr = (const ieee80211_hdr_t *)frame;
    const uint8_t *tags = frame + MGMT_HDR_LEN;
    uint16_t tags_len = frame_len - MGMT_HDR_LEN;

    char ssid[33];
    bool hidden = false;
    uint8_t sec = 0;
    parse_tags(tags, tags_len, ssid, &hidden, NULL, &sec, false, 0);

    record_probe(hdr->addr2, ssid, rssi);
}

static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < (int)sizeof(ieee80211_hdr_t)) return;

    const uint8_t *frame = pkt->payload;
    uint16_t frame_len = (uint16_t)pkt->rx_ctrl.sig_len;
    if (frame_len > 4) frame_len -= 4;  // strip FCS when present

    uint8_t fc0 = frame[0];
    uint8_t fc1 = frame[1];
    uint8_t ftype = FC_TYPE(fc0);
    uint8_t fsub  = FC_SUBTYPE(fc0);
    int8_t  rssi  = pkt->rx_ctrl.rssi;

    if (ftype == TYPE_MGMT) {
        bump_channel_activity();
        if (fsub == SUBTYPE_BEACON) {
            handle_beacon(frame, frame_len, rssi);
        } else if (fsub == SUBTYPE_PROBE_REQ) {
            handle_probe_req(frame, frame_len, rssi);
        }
    } else if (ftype == TYPE_DATA) {
        bump_channel_activity();
        count_eapol_frame(frame, frame_len);
    }
}

static void hop_task(void *arg) {
    uint8_t hop_idx = 0;
    uint8_t flush_ticks = 0;
    while (mon_running) {
        current_chan = hop_channels[hop_idx];
        esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
        hop_idx = (hop_idx + 1) % HOP_COUNT;
        uint16_t dwell = (current_chan <= 14) ? HOP_DWELL_24 : HOP_DWELL_5;
        vTaskDelay(pdMS_TO_TICKS(dwell));

        if (++flush_ticks >= 12) {
            flush_ticks = 0;
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            flush_probe_log_queue();
            xSemaphoreGive(data_mutex);
        }
    }

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    flush_probe_log_queue();
    xSemaphoreGive(data_mutex);
    vTaskDelete(NULL);
}

// ---------- render helpers ----------

static uint16_t list_count_for_view(void) {
    switch (current_view) {
        case MON_VIEW_BEACONS: return beacon_count;
        case MON_VIEW_PROBES:  return probe_count;
        default:               return 0;
    }
}

static void render_beacons(void) {
    char status[20];
    snprintf(status, sizeof(status), "%u/%u %s",
             beacon_count ? selected_idx + 1 : 0, beacon_count,
             sec_short(beacon_count ? beacons[selected_idx].sec_flags : 0));
    ssd1306_draw_header("Beacons", status);

    if (beacon_count == 0) {
        ssd1306_draw_string(0, 3, " Listening...");
        return;
    }

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= beacon_count) break;
        const monitor_beacon_t *b = &beacons[idx];
        const char *ssid = b->hidden || !b->ssid[0] ? "Hidden" : b->ssid;
        char line[17];
        snprintf(line, sizeof(line), "%c%-8.8s %4d",
                 (idx == selected_idx) ? '>' : ' ',
                 ssid, (int)b->rssi);
        ssd1306_draw_string(0, row + 2, line);
    }
}

static void render_probes(void) {
    char status[20];
    snprintf(status, sizeof(status), "%u/%u",
             probe_count ? selected_idx + 1 : 0, probe_count);
    ssd1306_draw_header("Probes", status);

    if (probe_count == 0) {
        ssd1306_draw_string(0, 3, " Listening...");
        return;
    }

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= probe_count) break;
        const monitor_probe_t *p = &probes[idx];
        char line[17];
        snprintf(line, sizeof(line), "%c%02X:%02X:%02X %4d",
                 (idx == selected_idx) ? '>' : ' ',
                 p->client_mac[3], p->client_mac[4], p->client_mac[5],
                 (int)p->rssi);
        ssd1306_draw_string(0, row + 2, line);
    }
}

static void render_eapol(void) {
    monitor_eapol_t e;
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    e = eapol_counts;
    xSemaphoreGive(data_mutex);

    char status[16];
    snprintf(status, sizeof(status), "Ch:%3u", (unsigned)current_chan);
    ssd1306_draw_header("EAPOL", status);

    char line[17];
    snprintf(line, sizeof(line), "M1 (AP):   %5u", (unsigned)e.m1);
    ssd1306_draw_string(0, 2, line);
    snprintf(line, sizeof(line), "M2 (STA):  %5u", (unsigned)e.m2);
    ssd1306_draw_string(0, 3, line);
    snprintf(line, sizeof(line), "M3 (AP):   %5u", (unsigned)e.m3);
    ssd1306_draw_string(0, 4, line);
    snprintf(line, sizeof(line), "M4 (STA):  %5u", (unsigned)e.m4);
    ssd1306_draw_string(0, 5, line);
    ssd1306_draw_string(0, 6, "Pairwise keys");
    ssd1306_draw_string(0, 7, "Click>view");
}

static void render_activity(void) {
    uint32_t local[HOP_COUNT];
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    memcpy(local, chan_activity, sizeof(local));
    xSemaphoreGive(data_mutex);

    uint32_t peak = 1;
    for (uint8_t i = 0; i < HOP_COUNT; i++) {
        if (local[i] > peak) peak = local[i];
    }

    // Top 6 channels by activity
    uint8_t order[6];
    uint32_t vals[6];
    for (uint8_t n = 0; n < 6; n++) {
        uint8_t best_i = 0;
        uint32_t best_v = 0;
        for (uint8_t i = 0; i < HOP_COUNT; i++) {
            bool used = false;
            for (uint8_t k = 0; k < n; k++) {
                if (order[k] == i) { used = true; break; }
            }
            if (used) continue;
            if (local[i] >= best_v) {
                best_v = local[i];
                best_i = i;
            }
        }
        order[n] = best_i;
        vals[n]  = local[best_i];
    }

    char status[16];
    const char *band = (current_chan > 14) ? "5G" : "2G";
    snprintf(status, sizeof(status), "Ch:%3u %s", (unsigned)current_chan, band);
    ssd1306_draw_header("Activity", status);

    for (uint8_t row = 0; row < 6; row++) {
        uint8_t ch = hop_channels[order[row]];
        int bar = (int)(vals[row] * 70 / peak);
        if (bar > 70) bar = 70;
        char line[17];
        snprintf(line, sizeof(line), "%3u %5u", (unsigned)ch, (unsigned)vals[row]);
        ssd1306_draw_string(0, row + 2, line);
        ssd1306_fill_rect(48, (uint8_t)(16 + row * 8), (uint8_t)bar, 6);
    }
}

// ---------- public API ----------

void wifi_monitor_init(void) {
    if (!data_mutex) {
        data_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Monitor init OK");
}

void wifi_monitor_enter(void) {
    current_view   = MON_VIEW_BEACONS;
    selected_idx   = 0;
    scroll_offset  = 0;
    ESP_LOGI(TAG, "Monitor enter");
}

void wifi_monitor_start(void) {
    if (mon_running) return;
    if (!radio_mgr_enter(RADIO_MODE_WIFI_MONITOR)) {
        ESP_LOGE(TAG, "radio_mgr_enter failed");
        return;
    }

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    beacon_count  = 0;
    probe_count   = 0;
    selected_idx  = 0;
    scroll_offset = 0;
    probe_log_q_len = 0;
    memset(beacons, 0, sizeof(beacons));
    memset(probes, 0, sizeof(probes));
    memset(&eapol_counts, 0, sizeof(eapol_counts));
    memset(chan_activity, 0, sizeof(chan_activity));
    xSemaphoreGive(data_mutex);

    mon_running = true;

    esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
    esp_wifi_set_promiscuous(true);

    xTaskCreate(hop_task, "mon_hop", 3072, NULL, 4, &hop_task_h);

    ESP_LOGI(TAG, "Monitor started");
}

void wifi_monitor_stop(void) {
    if (!mon_running) return;

    mon_running = false;
    vTaskDelay(pdMS_TO_TICKS(HOP_DWELL_24 + 100));
    hop_task_h = NULL;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    radio_mgr_leave(RADIO_MODE_WIFI_MONITOR);

    ESP_LOGI(TAG, "Monitor stopped — %u APs, %u probes",
             (unsigned)beacon_count, (unsigned)probe_count);
}

bool wifi_monitor_is_running(void) {
    return mon_running;
}

void wifi_monitor_scroll_up(void) {
    if (current_view != MON_VIEW_BEACONS && current_view != MON_VIEW_PROBES) return;
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_monitor_scroll_down(void) {
    if (current_view != MON_VIEW_BEACONS && current_view != MON_VIEW_PROBES) return;

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    uint16_t count = list_count_for_view();
    xSemaphoreGive(data_mutex);

    if (count > 0 && selected_idx < count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

void wifi_monitor_select(void) {
    current_view = (monitor_view_t)((current_view + 1) % 4);
    selected_idx  = 0;
    scroll_offset = 0;
}

void wifi_monitor_render(void) {
    ssd1306_clear_buffer();

    xSemaphoreTake(data_mutex, portMAX_DELAY);
    if (current_view == MON_VIEW_BEACONS || current_view == MON_VIEW_PROBES) {
        uint16_t count = list_count_for_view();
        if (count > 0 && selected_idx >= count)
            selected_idx = count - 1;
    }
    switch (current_view) {
        case MON_VIEW_BEACONS:  render_beacons();  break;
        case MON_VIEW_PROBES:   render_probes();   break;
        case MON_VIEW_EAPOL:    render_eapol();    break;
        case MON_VIEW_ACTIVITY: render_activity(); break;
    }
    xSemaphoreGive(data_mutex);

    ssd1306_draw_string(0, 7, "Click>view");
    ssd1306_flush();
}

uint16_t wifi_monitor_beacon_count(void) {
    return beacon_count;
}

uint16_t wifi_monitor_probe_count(void) {
    return probe_count;
}

const monitor_beacon_t *wifi_monitor_get_beacon(uint16_t idx) {
    if (idx >= beacon_count) return NULL;
    return &beacons[idx];
}

const monitor_probe_t *wifi_monitor_get_probe(uint16_t idx) {
    if (idx >= probe_count) return NULL;
    return &probes[idx];
}

const monitor_eapol_t *wifi_monitor_get_eapol(void) {
    return &eapol_counts;
}
