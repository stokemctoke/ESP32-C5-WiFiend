#include "settings.h"
#include "ssd1306.h"
#include "neopixel.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "settings";

#define NVS_NS           "wifiend"
#define KEY_LED          "led_brt"
#define KEY_HOP24        "hop24"
#define KEY_HOP5         "hop5"
#define KEY_LEGAL        "legal"
#define KEY_BURST24      "burst24"
#define KEY_BURST5       "burst5"
#define KEY_HOME_SSID    "home_ssid"
#define KEY_HOME_PASS    "home_pass"

#define DEFAULT_LED      40
#define DEFAULT_HOP24    400
#define DEFAULT_HOP5     300
#define DEFAULT_BURST24  30
#define DEFAULT_BURST5   50

#define SETTING_COUNT    6

typedef enum {
    SET_LED = 0,
    SET_HOP24,
    SET_HOP5,
    SET_LEGAL,
    SET_BURST24,
    SET_BURST5,
} setting_id_t;

static uint8_t  led_brightness = DEFAULT_LED;
static uint16_t hop_dwell_24   = DEFAULT_HOP24;
static uint16_t hop_dwell_5    = DEFAULT_HOP5;
static bool     legal_ack      = false;
static uint8_t  burst_24       = DEFAULT_BURST24;
static uint8_t  burst_5        = DEFAULT_BURST5;
static char     home_ssid[33]  = {0};
static char     home_pass[65]  = {0};

static uint8_t  selected_idx   = 0;
static uint8_t  scroll_offset  = 0;
static bool     active         = false;
static bool     refresh        = false;

static void apply_led(void) {
    uint8_t mapped = (uint8_t)((uint16_t)led_brightness * 255 / 100);
    neopixel_set_brightness(mapped);
}

static void clamp_defaults(void) {
    if (led_brightness > 100) led_brightness = DEFAULT_LED;
    if (hop_dwell_24 < 50 || hop_dwell_24 > 5000) hop_dwell_24 = DEFAULT_HOP24;
    if (hop_dwell_5 < 50 || hop_dwell_5 > 5000) hop_dwell_5 = DEFAULT_HOP5;
    if (burst_24 < 5 || burst_24 > 120) burst_24 = DEFAULT_BURST24;
    if (burst_5 < 5 || burst_5 > 120) burst_5 = DEFAULT_BURST5;
}

void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t u8;
    uint16_t u16;

    if (nvs_get_u8(h, KEY_LED, &u8) == ESP_OK) led_brightness = u8;
    if (nvs_get_u16(h, KEY_HOP24, &u16) == ESP_OK) hop_dwell_24 = u16;
    if (nvs_get_u16(h, KEY_HOP5, &u16) == ESP_OK) hop_dwell_5 = u16;
    if (nvs_get_u8(h, KEY_LEGAL, &u8) == ESP_OK) legal_ack = (u8 != 0);
    if (nvs_get_u8(h, KEY_BURST24, &u8) == ESP_OK) burst_24 = u8;
    if (nvs_get_u8(h, KEY_BURST5, &u8) == ESP_OK) burst_5 = u8;

    size_t len = sizeof(home_ssid);
    if (nvs_get_str(h, KEY_HOME_SSID, home_ssid, &len) != ESP_OK) home_ssid[0] = '\0';
    len = sizeof(home_pass);
    if (nvs_get_str(h, KEY_HOME_PASS, home_pass, &len) != ESP_OK) home_pass[0] = '\0';

    nvs_close(h);
    clamp_defaults();
    apply_led();
}

void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed");
        return;
    }

    nvs_set_u8(h, KEY_LED, led_brightness);
    nvs_set_u16(h, KEY_HOP24, hop_dwell_24);
    nvs_set_u16(h, KEY_HOP5, hop_dwell_5);
    nvs_set_u8(h, KEY_LEGAL, legal_ack ? 1 : 0);
    nvs_set_u8(h, KEY_BURST24, burst_24);
    nvs_set_u8(h, KEY_BURST5, burst_5);
    nvs_set_str(h, KEY_HOME_SSID, home_ssid);
    nvs_set_str(h, KEY_HOME_PASS, home_pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved");
}

void settings_init(void) {
    settings_load();
    ESP_LOGI(TAG, "Settings init OK");
}

uint8_t settings_get_led_brightness(void) { return led_brightness; }
void settings_set_led_brightness(uint8_t pct) {
    led_brightness = pct > 100 ? 100 : pct;
    apply_led();
}

uint16_t settings_get_hop_dwell_24(void) { return hop_dwell_24; }
void settings_set_hop_dwell_24(uint16_t ms) { hop_dwell_24 = ms; }

uint16_t settings_get_hop_dwell_5(void) { return hop_dwell_5; }
void settings_set_hop_dwell_5(uint16_t ms) { hop_dwell_5 = ms; }

bool settings_get_legal_ack(void) { return legal_ack; }
void settings_set_legal_ack(bool ack) { legal_ack = ack; }

uint8_t settings_get_burst_24(void) { return burst_24; }
void settings_set_burst_24(uint8_t n) { burst_24 = n; }

uint8_t settings_get_burst_5(void) { return burst_5; }
void settings_set_burst_5(uint8_t n) { burst_5 = n; }

const char *settings_get_home_ssid(void) { return home_ssid; }
const char *settings_get_home_pass(void) { return home_pass; }

void settings_set_home_wifi(const char *ssid, const char *pass) {
    if (ssid && ssid[0]) {
        strncpy(home_ssid, ssid, sizeof(home_ssid) - 1);
        home_ssid[sizeof(home_ssid) - 1] = '\0';
    }
    // Empty pass means "leave unchanged" (WiFuxx parity).
    if (pass && pass[0]) {
        strncpy(home_pass, pass, sizeof(home_pass) - 1);
        home_pass[sizeof(home_pass) - 1] = '\0';
    }
    settings_save();
}

void settings_enter(void) {
    selected_idx  = 0;
    scroll_offset = 0;
    active        = true;
    refresh       = true;
}

void settings_exit(void) {
    active = false;
}

void settings_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    }
    refresh = true;
}

void settings_scroll_down(void) {
    if (selected_idx + 1 < SETTING_COUNT) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
    }
    refresh = true;
}

static void cycle_setting(setting_id_t id) {
    switch (id) {
        case SET_LED:
            led_brightness = (led_brightness + 5) % 105;
            apply_led();
            break;
        case SET_HOP24:
            hop_dwell_24 += 50;
            if (hop_dwell_24 > 2000) hop_dwell_24 = 100;
            break;
        case SET_HOP5:
            hop_dwell_5 += 50;
            if (hop_dwell_5 > 2000) hop_dwell_5 = 100;
            break;
        case SET_LEGAL:
            legal_ack = !legal_ack;
            break;
        case SET_BURST24:
            burst_24 += 5;
            if (burst_24 > 100) burst_24 = 10;
            break;
        case SET_BURST5:
            burst_5 += 5;
            if (burst_5 > 100) burst_5 = 10;
            break;
    }
    settings_save();
}

void settings_select(void) {
    cycle_setting((setting_id_t)selected_idx);
    refresh = true;
}

bool settings_back(void) {
    return true;
}

bool settings_is_active(void) { return active; }

bool settings_needs_refresh(void) {
    bool v = refresh;
    refresh = false;
    return v;
}

static void format_row(setting_id_t id, char *line, size_t linesz) {
    switch (id) {
        case SET_LED:
            snprintf(line, linesz, "LED: %3u%%", (unsigned)led_brightness);
            break;
        case SET_HOP24:
            snprintf(line, linesz, "Hop 2G: %u ms", (unsigned)hop_dwell_24);
            break;
        case SET_HOP5:
            snprintf(line, linesz, "Hop 5G: %u ms", (unsigned)hop_dwell_5);
            break;
        case SET_LEGAL:
            snprintf(line, linesz, "Legal: %s", legal_ack ? "YES" : "NO");
            break;
        case SET_BURST24:
            snprintf(line, linesz, "Burst 2G: %u", (unsigned)burst_24);
            break;
        case SET_BURST5:
            snprintf(line, linesz, "Burst 5G: %u", (unsigned)burst_5);
            break;
    }
}

void settings_render(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("SETTINGS", "CK=change");

    for (uint8_t row = 0; row < 6; row++) {
        uint8_t idx = scroll_offset + row;
        if (idx >= SETTING_COUNT) break;

        char val[20];
        format_row((setting_id_t)idx, val, sizeof(val));
        char line[32];
        snprintf(line, sizeof(line), "%c%s",
                 (idx == selected_idx) ? '>' : ' ',
                 val);
        line[16] = '\0';
        ssd1306_draw_string(0, row + 2, line);
    }

    ssd1306_draw_string(0, 7, "LN>back");
    ssd1306_flush();
}
