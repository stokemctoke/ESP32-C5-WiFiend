#include "wifi_scan.h"
#include "oui_lookup.h"
#include "ssd1306.h"
#include "neopixel.h"
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

static uint16_t wifi_scan_run(bool animate) {
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

    if (animate) {
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
            neopixel_pulse(COLOR_YELLOW);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        neopixel_set_color(COLOR_YELLOW);
    } else {
        EventBits_t bits = xEventGroupWaitBits(
            s_scan_done, SCAN_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(45000));
        if (!(bits & SCAN_DONE_BIT)) {
            ESP_LOGE(TAG, "Scan timed out");
            esp_wifi_scan_stop();
            return 0;
        }
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

uint16_t wifi_scan_start(void) {
    return wifi_scan_run(true);
}

uint16_t wifi_scan_start_quiet(void) {
    return wifi_scan_run(false);
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

    // BSSID — vendor from OUI lookup, else hex OUI bytes
    const char *vendor = oui_lookup(ap->bssid);
    if (vendor) {
        snprintf(row, sizeof(row), "%.16s", vendor);
    } else {
        snprintf(row, sizeof(row), "OUI:%02X:%02X:%02X",
                 ap->bssid[0], ap->bssid[1], ap->bssid[2]);
    }
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

// ---------- channel chart ----------

// Bar chart geometry (pixels, y=0 is top of display)
#define CHART_LEFT_X   6    // left margin; 13 bars × 9px = 117px, (128-117+1)/2 ≈ 6
#define CHART_BAR_W    8    // bar width in pixels (one char wide — fits a label)
#define CHART_BAR_STEP 9    // bar pitch: 8px bar + 1px gap
#define CHART_BASELINE 55   // y=55 (page 6, bit 7) — bottom of bar area
#define CHART_TOP_Y    17   // y=17 — maximum bar top (1px into blue zone for breathing room)
#define CHART_MAX_H   (CHART_BASELINE - CHART_TOP_Y + 1)  // 39 pixels max bar height

typedef struct { uint8_t ch; uint8_t count; int8_t rssi; } ch5_entry_t;

static uint8_t     chart_sel_24 = 1;    // selected 2.4GHz channel (1–13)
static bool        chart_is_5g  = false;
static uint8_t     chart_5g_sel = 0;
static ch5_entry_t ch5_list[MAX_SCAN_RESULTS];
static uint8_t     ch5_count    = 0;

void wifi_scan_chart_next(void) {
    if (!chart_is_5g) {
        chart_sel_24 = (chart_sel_24 < 13) ? chart_sel_24 + 1 : 1;
    } else if (ch5_count > 0) {
        chart_5g_sel = (chart_5g_sel + 1) % ch5_count;
    }
}

void wifi_scan_chart_prev(void) {
    if (!chart_is_5g) {
        chart_sel_24 = (chart_sel_24 > 1) ? chart_sel_24 - 1 : 13;
    } else if (ch5_count > 0) {
        chart_5g_sel = (chart_5g_sel == 0) ? ch5_count - 1 : chart_5g_sel - 1;
    }
}

void wifi_scan_chart_toggle(void) {
    chart_is_5g = !chart_is_5g;
}

static void render_chart_24(uint8_t count[14], int8_t best[14]) {
    uint8_t max_c = 1;
    for (uint8_t ch = 1; ch <= 13; ch++) if (count[ch] > max_c) max_c = count[ch];

    ssd1306_clear_buffer();

    char status[20];
    uint8_t n = count[chart_sel_24];
    if (n > 0) {
        snprintf(status, sizeof(status), "2.4G Ch:%2u %uAP", chart_sel_24, n);
    } else {
        snprintf(status, sizeof(status), "2.4G Ch:%2u clear", chart_sel_24);
    }
    ssd1306_draw_header("Ch Chart", status);

    ssd1306_hline(CHART_LEFT_X, CHART_BASELINE, 13 * CHART_BAR_STEP - 1);

    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (count[ch] == 0) continue;
        uint8_t h = (uint8_t)(2 + ((uint16_t)count[ch] * (CHART_MAX_H - 2)) / max_c);
        if (h > CHART_MAX_H) h = CHART_MAX_H;
        uint8_t x = CHART_LEFT_X + (ch - 1) * CHART_BAR_STEP;
        ssd1306_fill_rect(x, CHART_BASELINE - h + 1, CHART_BAR_W, h);
    }

    for (uint8_t ch = 1; ch <= 13; ch++) {
        uint8_t x = CHART_LEFT_X + (ch - 1) * CHART_BAR_STEP;
        char c = (ch <= 9) ? ('0' + ch) : ('A' + ch - 10);
        ssd1306_draw_char(x, 7, c);
    }

    // Invert selected channel column in bar area (y=16–55, pages 2–6)
    {
        uint8_t x = CHART_LEFT_X + (chart_sel_24 - 1) * CHART_BAR_STEP;
        ssd1306_invert_rect(x, 16, CHART_BAR_W, 40);
    }

    ssd1306_flush();
}

static void render_chart_5g(void) {
    ssd1306_clear_buffer();

    if (ch5_count == 0) {
        ssd1306_draw_header("Ch Chart", "5GHz No APs");
        ssd1306_draw_string(0, 4, " Run WiFi Scan");
        ssd1306_flush();
        return;
    }

    if (chart_5g_sel >= ch5_count) chart_5g_sel = 0;

    char status[20];
    snprintf(status, sizeof(status), "5GHz Ch:%u %uAP",
             ch5_list[chart_5g_sel].ch, ch5_list[chart_5g_sel].count);
    ssd1306_draw_header("Ch Chart", status);

    // Show up to 6 channels; scroll window follows selection
    uint8_t start = (chart_5g_sel >= 5) ? chart_5g_sel - 4 : 0;
    for (uint8_t r = 0; r < 6 && start + r < ch5_count; r++) {
        ch5_entry_t *e = &ch5_list[start + r];
        char line[17];
        snprintf(line, sizeof(line), "%c%3u: %2u  %4d",
                 (start + r == chart_5g_sel) ? '>' : ' ',
                 e->ch, e->count, (int)e->rssi);
        ssd1306_draw_string(0, r + 2, line);
    }

    ssd1306_flush();
}

void wifi_scan_render_chart(void) {
    if (result_count == 0) {
        ssd1306_clear_buffer();
        ssd1306_draw_header("Ch Chart", "No scan data");
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_draw_string(0, 6, "CLK=2.4G/5G");
        ssd1306_draw_string(0, 7, "LN=menu");
        ssd1306_flush();
        return;
    }

    // Build channel tables from scan results in one pass
    uint8_t count24[14] = {0};
    int8_t  best24[14];
    memset(best24, -100, sizeof(best24));
    ch5_count = 0;

    for (uint16_t i = 0; i < result_count; i++) {
        uint8_t ch = results[i].channel;
        if (ch >= 1 && ch <= 13) {
            count24[ch]++;
            if (results[i].rssi > best24[ch]) best24[ch] = results[i].rssi;
        } else if (ch > 14) {
            bool found = false;
            for (uint8_t j = 0; j < ch5_count; j++) {
                if (ch5_list[j].ch == ch) {
                    ch5_list[j].count++;
                    if (results[i].rssi > ch5_list[j].rssi) ch5_list[j].rssi = results[i].rssi;
                    found = true;
                    break;
                }
            }
            if (!found && ch5_count < MAX_SCAN_RESULTS) {
                ch5_list[ch5_count++] = (ch5_entry_t){ ch, 1, results[i].rssi };
            }
        }
    }

    // Sort 5GHz list by channel number
    for (uint8_t i = 0; i < ch5_count; i++) {
        for (uint8_t j = i + 1; j < ch5_count; j++) {
            if (ch5_list[j].ch < ch5_list[i].ch) {
                ch5_entry_t tmp = ch5_list[i];
                ch5_list[i]     = ch5_list[j];
                ch5_list[j]     = tmp;
            }
        }
    }

    if (chart_is_5g) render_chart_5g();
    else             render_chart_24(count24, best24);
}
