#include "wifi_sta.h"
#include "wifi_scan.h"
#include "ssd1306.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_sta";

// ---------- character wheel ----------
// Layout: printable chars, then [DEL] at CHAR_SET_LEN, then [OK] at CHAR_SET_LEN+1.
// CCW from first printable wraps to [OK]; CW from [OK] wraps back to first printable.

static const char CHAR_SET[] =
    " abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!@#$%^&*()-_+=.,/";

#define CHAR_SET_LEN   ((uint8_t)(sizeof(CHAR_SET) - 1))   // 80 printable chars
#define CHAR_POS_DEL   CHAR_SET_LEN                         // position 80
#define CHAR_POS_OK    ((uint8_t)(CHAR_SET_LEN + 1))        // position 81
#define CHAR_POS_TOTAL ((uint8_t)(CHAR_SET_LEN + 2))        // 82 total positions

// ---------- state ----------

typedef enum {
    STA_STATE_PICK,
    STA_STATE_PASSWORD,
    STA_STATE_CONNECTING,
    STA_STATE_CONNECTED,
    STA_STATE_FAILED,
} sta_state_t;

static sta_state_t          sta_state  = STA_STATE_PICK;
static wifi_connect_state_t conn_state = WIFI_CONNECT_IDLE;
static volatile bool        sta_refresh_pending = false;

// AP picker
static wifi_ap_info_t targets[MAX_SCAN_RESULTS];
static uint16_t       target_count  = 0;
static uint16_t       scroll_offset = 0;
static uint16_t       selected_idx  = 0;

// Selected AP
static char    sel_ssid[33];
static uint8_t sel_channel;
static uint8_t sel_security;

// Password entry
#define STA_PW_MAX 64
static char    pw_buf[STA_PW_MAX + 1];
static uint8_t pw_len   = 0;
static uint8_t char_pos = 1;   // start at 'a' (index 1)

// Connection result
static char ip_str[16];

// ---------- WiFi event handlers ----------

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (sta_state != STA_STATE_CONNECTING) return;
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
    conn_state          = WIFI_CONNECT_CONNECTED;
    sta_state           = STA_STATE_CONNECTED;
    sta_refresh_pending = true;
    ESP_LOGI(TAG, "Connected — IP: %s", ip_str);
}

static void on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (sta_state != STA_STATE_CONNECTING) return;
    wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
    ESP_LOGW(TAG, "STA disconnected reason %d", ev->reason);
    conn_state          = WIFI_CONNECT_FAILED;
    sta_state           = STA_STATE_FAILED;
    sta_refresh_pending = true;
}

// ---------- helpers ----------

static const char *auth_short(uint8_t sec) {
    switch (sec) {
        case 0: return "OPEN"; case 1: return "WEP ";
        case 2: return "WPA "; case 3: return "WPA2";
        case 6: return "WPA3"; case 7: return "WP23";
        default: return "WPA2";
    }
}

static char wheel_char(uint8_t pos) {
    if (pos == CHAR_POS_OK)          return 'K';   // abbreviated: oK
    if (pos == CHAR_POS_DEL)         return '<';   // like backspace arrow
    if (CHAR_SET[pos] == ' ')        return '_';   // visible space
    return CHAR_SET[pos];
}

static void do_connect(void) {
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, sel_ssid, strlen(sel_ssid));
    if (pw_len > 0) {
        memcpy(cfg.sta.password, pw_buf, pw_len);
    }
    cfg.sta.threshold.authmode = (sel_security == 0)
                                 ? WIFI_AUTH_OPEN
                                 : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    conn_state = WIFI_CONNECT_CONNECTING;
    sta_state  = STA_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting to \"%s\" ch%u", sel_ssid, sel_channel);
}

// ---------- public API ----------

void wifi_sta_init(void) {
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,          on_got_ip,          NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,   on_sta_disconnected, NULL);
    ESP_LOGI(TAG, "WiFi STA module ready");
}

void wifi_sta_enter(void) {
    sta_state    = STA_STATE_PICK;
    conn_state   = WIFI_CONNECT_IDLE;
    scroll_offset = 0;
    selected_idx  = 0;
    memset(pw_buf, 0, sizeof(pw_buf));
    pw_len   = 0;
    char_pos = 1;

    const wifi_ap_info_t *scan = wifi_scan_get_results(&target_count);
    if (target_count > MAX_SCAN_RESULTS) target_count = MAX_SCAN_RESULTS;
    if (scan && target_count > 0) {
        memcpy(targets, scan, sizeof(wifi_ap_info_t) * target_count);
        return;
    }

    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", "lazy scan...");
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

void wifi_sta_scroll_up(void) {
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset)
            scroll_offset = selected_idx;
    }
}

void wifi_sta_scroll_down(void) {
    if (target_count > 0 && selected_idx < target_count - 1) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6)
            scroll_offset = selected_idx - 5;
    }
}

void wifi_sta_select(void) {
    if (target_count == 0) return;

    strncpy(sel_ssid, targets[selected_idx].ssid, 32);
    sel_ssid[32] = '\0';
    if (!sel_ssid[0]) strncpy(sel_ssid, "Hidden", 32);
    sel_channel  = targets[selected_idx].channel;
    sel_security = targets[selected_idx].security;

    memset(pw_buf, 0, sizeof(pw_buf));
    pw_len   = 0;
    char_pos = 1;   // start wheel at 'a'

    if (sel_security == 0) {
        do_connect();   // open — no password needed
    } else {
        sta_state = STA_STATE_PASSWORD;
    }
}

void wifi_sta_char_next(void) {
    char_pos = (char_pos + 1) % CHAR_POS_TOTAL;
}

void wifi_sta_char_prev(void) {
    char_pos = (char_pos == 0) ? (CHAR_POS_TOTAL - 1) : (char_pos - 1);
}

void wifi_sta_char_append(void) {
    if (char_pos == CHAR_POS_OK) {
        do_connect();
    } else if (char_pos == CHAR_POS_DEL) {
        if (pw_len > 0) {
            pw_buf[--pw_len] = '\0';
        }
    } else if (pw_len < STA_PW_MAX) {
        pw_buf[pw_len++] = CHAR_SET[char_pos];
        pw_buf[pw_len]   = '\0';
    }
}

void wifi_sta_pw_cancel(void) {
    memset(pw_buf, 0, sizeof(pw_buf));
    pw_len    = 0;
    char_pos  = 1;
    sta_state = STA_STATE_PICK;
}

void wifi_sta_stop(void) {
    if (conn_state == WIFI_CONNECT_CONNECTED ||
        conn_state == WIFI_CONNECT_CONNECTING) {
        esp_wifi_disconnect();
    }
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    memset(pw_buf, 0, sizeof(pw_buf));
    pw_len     = 0;
    conn_state = WIFI_CONNECT_IDLE;
    sta_state  = STA_STATE_PICK;
    ESP_LOGI(TAG, "STA stopped");
}

bool wifi_sta_is_connected(void)   { return conn_state == WIFI_CONNECT_CONNECTED; }
bool wifi_sta_is_connecting(void)  { return sta_state  == STA_STATE_CONNECTING; }
bool wifi_sta_is_in_picker(void)   { return sta_state  == STA_STATE_PICK; }
bool wifi_sta_is_in_password(void) { return sta_state  == STA_STATE_PASSWORD; }

bool wifi_sta_needs_refresh(void) {
    bool v = sta_refresh_pending;
    sta_refresh_pending = false;
    return v;
}

wifi_connect_state_t wifi_sta_get_state(void) { return conn_state; }
const char *wifi_sta_get_ip(void) { return ip_str; }

// ---------- display ----------

static void render_picker(void) {
    ssd1306_clear_buffer();
    if (target_count == 0) {
        ssd1306_draw_header("STA Connect", "No APs");
        ssd1306_draw_string(0, 3, " Run WiFi Scan");
        ssd1306_draw_string(0, 4, " first");
        ssd1306_flush();
        return;
    }
    char status[16];
    snprintf(status, sizeof(status), "%u/%u %s",
             selected_idx + 1, target_count,
             auth_short(targets[selected_idx].security));
    ssd1306_draw_header("STA Connect", status);
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

static void render_password(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", sel_ssid);

    // Row 2: typed password — always shows the most recent 13 chars
    char line[24];
    uint8_t start = (pw_len > 13) ? pw_len - 13 : 0;
    snprintf(line, sizeof(line), "PW>%-13.13s", pw_buf + start);
    ssd1306_draw_string(0, 2, line);

    // Row 3: password length counter
    snprintf(line, sizeof(line), "Len:%-3u", (unsigned)pw_len);
    ssd1306_draw_string(0, 3, line);

    // Row 4: current selection
    char label[8];
    if (char_pos == CHAR_POS_OK) {
        strcpy(label, "[OK]");
    } else if (char_pos == CHAR_POS_DEL) {
        strcpy(label, "[DEL]");
    } else if (CHAR_SET[char_pos] == ' ') {
        strcpy(label, "[SPC]");
    } else {
        snprintf(label, sizeof(label), "[%c]", CHAR_SET[char_pos]);
    }
    snprintf(line, sizeof(line), "> %-12s", label);
    ssd1306_draw_string(0, 4, line);

    // Row 5: wheel context — show 2 neighbours either side
    uint8_t p0 = (char_pos + CHAR_POS_TOTAL - 2) % CHAR_POS_TOTAL;
    uint8_t p1 = (char_pos + CHAR_POS_TOTAL - 1) % CHAR_POS_TOTAL;
    uint8_t p3 = (char_pos + 1) % CHAR_POS_TOTAL;
    uint8_t p4 = (char_pos + 2) % CHAR_POS_TOTAL;
    snprintf(line, sizeof(line), "%c %c [%c] %c %c",
             wheel_char(p0), wheel_char(p1), wheel_char(char_pos),
             wheel_char(p3), wheel_char(p4));
    ssd1306_draw_string(0, 5, line);

    // Rows 6–7: hints
    ssd1306_draw_string(0, 6, "CLK=add");
    ssd1306_draw_string(0, 7, "LN=back [OK]=go");
    ssd1306_flush();
}

static void render_connecting(void) {
    static uint8_t tick = 0;
    static const char spinner[] = "|/-\\";
    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", sel_ssid);
    tick++;
    char line[16];
    snprintf(line, sizeof(line), " %c Connecting", spinner[tick % 4]);
    ssd1306_draw_string(0, 4, line);
    ssd1306_draw_string(0, 7, "LN=cancel");
    ssd1306_flush();
}

static void render_connected(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", sel_ssid);
    ssd1306_draw_string(0, 2, "Connected!");
    char line[20];
    snprintf(line, sizeof(line), "IP:%-13.13s", ip_str);
    ssd1306_draw_string(0, 4, line);
    ssd1306_draw_string(0, 7, "LN=disconnect");
    ssd1306_flush();
}

static void render_failed(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", sel_ssid);
    ssd1306_draw_string(0, 3, " Failed!");
    ssd1306_draw_string(0, 4, " Wrong PW?");
    ssd1306_draw_string(0, 6, "CLK=retry");
    ssd1306_draw_string(0, 7, "LN=back");
    ssd1306_flush();
}

void wifi_sta_render(void) {
    switch (sta_state) {
        case STA_STATE_PICK:       render_picker();     break;
        case STA_STATE_PASSWORD:   render_password();   break;
        case STA_STATE_CONNECTING: render_connecting(); break;
        case STA_STATE_CONNECTED:  render_connected();  break;
        case STA_STATE_FAILED:     render_failed();     break;
    }
}
