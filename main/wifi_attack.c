#include "wifi_attack.h"
#include "wifi_scan.h"
#include "wifi_sniffer.h"
#include "deauth_engine.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_attack";

typedef enum {
    ATTACK_STATE_PICK = 0,
    ATTACK_STATE_RUNNING,
} attack_state_t;

typedef enum {
    PROFILE_BROADCAST = 0,
    PROFILE_TARGETED,
    PROFILE_DISASSOC,
    PROFILE_PROBE,
    PROFILE_COUNT,
} attack_profile_t;

static attack_state_t     state         = ATTACK_STATE_PICK;
static attack_profile_t   profile       = PROFILE_BROADCAST;
static uint16_t           scroll_offset = 0;
static uint16_t           selected_idx  = 0;
static uint16_t           target_count  = 0;

static wifi_ap_info_t     targets[MAX_SCAN_RESULTS];

static uint8_t  target_bssid[6];
static uint8_t  target_channel;
static char     target_ssid[33];
static uint8_t  target_client[6];
static bool     have_target_client      = false;
static int64_t  attack_start_us         = 0;

static const char *profile_name(attack_profile_t p) {
    switch (p) {
        case PROFILE_BROADCAST: return "Broadcast";
        case PROFILE_TARGETED:  return "Targeted";
        case PROFILE_DISASSOC:  return "Disassoc";
        case PROFILE_PROBE:     return "Probe Flood";
        default:                return "?";
    }
}

static bool find_sniff_client_for_bssid(const uint8_t bssid[6], uint8_t out_mac[6]) {
    uint16_t n = wifi_sniff_get_count();
    for (uint16_t i = 0; i < n; i++) {
        const wifi_client_t *c = wifi_sniff_get_client(i);
        if (!c || !c->associated) continue;
        if (memcmp(c->ap_bssid, bssid, 6) != 0) continue;
        memcpy(out_mac, c->mac, 6);
        return true;
    }
    return false;
}

static attack_profile_t next_available_profile(attack_profile_t cur) {
    for (int step = 1; step <= PROFILE_COUNT; step++) {
        attack_profile_t next = (attack_profile_t)((cur + step) % PROFILE_COUNT);
        if (next == PROFILE_TARGETED) {
            if (!find_sniff_client_for_bssid(target_bssid, target_client)) {
                continue;
            }
            have_target_client = true;
        }
        return next;
    }
    return PROFILE_BROADCAST;
}

void wifi_attack_init(void) {
    deauth_engine_init();
    ESP_LOGI(TAG, "Attack module ready");
}

void wifi_attack_enter(void) {
    state         = ATTACK_STATE_PICK;
    profile       = PROFILE_BROADCAST;
    scroll_offset = 0;
    selected_idx  = 0;
    have_target_client = false;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
        return;
    }

    ssd1306_clear_buffer();
    ssd1306_draw_header("Deauth", "lazy scan...");
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

void wifi_attack_cycle_profile(void) {
    if (target_count == 0 || state != ATTACK_STATE_PICK) return;

    memcpy(target_bssid, targets[selected_idx].bssid, 6);
    profile = next_available_profile(profile);
    ESP_LOGI(TAG, "Profile: %s", profile_name(profile));
}

void wifi_attack_start(void) {
    if (target_count == 0) return;

    memcpy(target_bssid, targets[selected_idx].bssid, 6);
    target_channel = targets[selected_idx].channel;
    strncpy(target_ssid, targets[selected_idx].ssid, 32);
    target_ssid[32] = '\0';

    have_target_client = find_sniff_client_for_bssid(target_bssid, target_client);
    if (profile == PROFILE_TARGETED && !have_target_client) {
        profile = PROFILE_BROADCAST;
    }

    deauth_target_t t = {0};
    memcpy(t.bssid, target_bssid, 6);
    t.channel = target_channel;

    deauth_engine_set_mode(profile == PROFILE_DISASSOC);

    if (!radio_mgr_enter(RADIO_MODE_WIFI_ATTACK)) {
        ESP_LOGE(TAG, "Radio busy — cannot start attack");
        return;
    }

    attack_start_us = esp_timer_get_time();
    state           = ATTACK_STATE_RUNNING;

    switch (profile) {
        case PROFILE_TARGETED:
            deauth_engine_start_targeted(&t, target_client);
            break;
        case PROFILE_PROBE:
            deauth_engine_start_probe_flood(&t, target_ssid);
            break;
        case PROFILE_DISASSOC:
        case PROFILE_BROADCAST:
        default:
            deauth_engine_start(&t, 1);
            break;
    }

    ESP_LOGI(TAG, "Attack started (%s): %s ch%u",
             profile_name(profile), target_ssid, target_channel);
}

void wifi_attack_stop(void) {
    if (!deauth_engine_is_running()) {
        state = ATTACK_STATE_PICK;
        return;
    }
    deauth_engine_stop();
    radio_mgr_leave(RADIO_MODE_WIFI_ATTACK);
    state = ATTACK_STATE_PICK;
    ESP_LOGI(TAG, "Attack stopped: %lu frames sent",
             (unsigned long)deauth_engine_get_frames());
}

bool wifi_attack_is_running(void) {
    return deauth_engine_is_running();
}

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
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
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

    char prof_line[17];
    snprintf(prof_line, sizeof(prof_line), "%s LN>start", profile_name(profile));
    ssd1306_draw_string(0, 7, prof_line);

    ssd1306_flush();
}

static void render_running(void) {
    ssd1306_clear_buffer();

    int64_t elapsed_s = (esp_timer_get_time() - attack_start_us) / 1000000;
    if (elapsed_s < 1) elapsed_s = 1;

    char elapsed[16];
    int64_t m = elapsed_s / 60;
    int64_t s = elapsed_s % 60;
    snprintf(elapsed, sizeof(elapsed), "%02lld:%02lld", m, s);
    ssd1306_draw_string(0, 0, target_ssid[0] ? target_ssid : "Hidden");
    ssd1306_draw_string(0, 1, elapsed);

    char line[17];
    uint32_t frames = deauth_engine_get_frames();
    uint32_t pps    = deauth_engine_get_pps();

    snprintf(line, sizeof(line), "%-10.10s C%-3u",
             profile_name(profile), (unsigned)target_channel);
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "%02X:%02X:%02X",
             target_bssid[0], target_bssid[1], target_bssid[2]);
    ssd1306_draw_string(0, 3, line);
    snprintf(line, sizeof(line), "%02X:%02X:%02X",
             target_bssid[3], target_bssid[4], target_bssid[5]);
    ssd1306_draw_string(0, 4, line);

    if (profile == PROFILE_TARGETED && have_target_client) {
        snprintf(line, sizeof(line), "Cl:%02X:%02X:%02X",
                 target_client[3], target_client[4], target_client[5]);
        ssd1306_draw_string(0, 5, line);
    } else {
        snprintf(line, sizeof(line), "Sent: %lu", (unsigned long)frames);
        ssd1306_draw_string(0, 5, line);
    }

    if (profile == PROFILE_TARGETED && have_target_client) {
        snprintf(line, sizeof(line), "Sent:%lu ~%lup/s",
                 (unsigned long)frames, (unsigned long)pps);
    } else {
        snprintf(line, sizeof(line), "~%lu pps LN>stop", (unsigned long)pps);
    }
    ssd1306_draw_string(0, 6, line);

    ssd1306_flush();
}

void wifi_attack_render(void) {
    if (state == ATTACK_STATE_RUNNING && deauth_engine_is_running()) {
        render_running();
    } else {
        if (state == ATTACK_STATE_RUNNING) state = ATTACK_STATE_PICK;
        render_picker();
    }
}

uint32_t    wifi_attack_get_frames(void)     { return deauth_engine_get_frames(); }
const char *wifi_attack_get_target(void)     { return target_ssid; }
const char *wifi_attack_get_profile(void)    { return profile_name(profile); }
int64_t     wifi_attack_get_elapsed_ms(void) {
    if (!deauth_engine_is_running() || attack_start_us == 0) return 0;
    return (esp_timer_get_time() - attack_start_us) / 1000LL;
}
