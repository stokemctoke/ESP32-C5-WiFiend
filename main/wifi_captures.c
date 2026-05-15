#include "wifi_captures.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "captures";

#define MAX_CAPTURE_ENTRIES 32
#define PMKID_LOG_PATH      "/lfs/pmkid.log"
#define HANDSHAKE_LOG_PATH  "/lfs/handshakes.log"

typedef enum {
    CAP_VIEW_LIST = 0,
    CAP_VIEW_DUMPING,
    CAP_VIEW_CONFIRM_CLEAR,
} cap_view_t;

typedef struct {
    char    type;        // 'P' = PMKID, 'H' = Handshake
    char    ssid[17];
    uint8_t bssid[6];
} cap_entry_t;

static cap_entry_t entries[MAX_CAPTURE_ENTRIES];
static uint16_t    entry_count   = 0;
static uint16_t    pmkid_count   = 0;
static uint16_t    hshk_count    = 0;
static uint16_t    selected_idx  = 0;
static uint16_t    scroll_offset = 0;
static cap_view_t  view_state    = CAP_VIEW_LIST;

static volatile bool active  = false;
static volatile bool refresh = false;

// ---------- helpers ----------

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a hashcat-22000 line: WPA*TT*MIC*BSSID*CLIENT*SSID_HEX*...
// Extracts BSSID and SSID (decoded from hex) into `out`.
static bool parse_line(const char *line, cap_entry_t *out, char type) {
    int         star = 0;
    const char *bssid_start = NULL, *bssid_end = NULL;
    const char *ssid_start  = NULL, *ssid_end  = NULL;
    const char *p = line;

    while (*p && *p != '\n' && *p != '\r') {
        if (*p == '*') {
            star++;
            if      (star == 3) bssid_start = p + 1;
            else if (star == 4) { bssid_end = p; ssid_start = NULL; }
            else if (star == 5) ssid_start = p + 1;
            else if (star == 6) { ssid_end = p; break; }
        }
        p++;
    }
    if (!ssid_end) ssid_end = p;
    if (!bssid_start || !bssid_end || !ssid_start) return false;
    if (bssid_end - bssid_start != 12) return false;

    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(bssid_start[i * 2]);
        int lo = hex_nibble(bssid_start[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out->bssid[i] = (uint8_t)((hi << 4) | lo);
    }

    int ssid_hex_len = (int)(ssid_end - ssid_start);
    int ssid_len     = ssid_hex_len / 2;
    if (ssid_len > 16) ssid_len = 16;
    for (int i = 0; i < ssid_len; i++) {
        int hi = hex_nibble(ssid_start[i * 2]);
        int lo = hex_nibble(ssid_start[i * 2 + 1]);
        if (hi < 0 || lo < 0) { out->ssid[i] = '?'; }
        else                  { out->ssid[i] = (char)((hi << 4) | lo); }
    }
    out->ssid[ssid_len] = '\0';
    out->type           = type;
    return true;
}

static void load_file(const char *path, char type, uint16_t *count_out) {
    *count_out = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char buf[768];
    while (entry_count < MAX_CAPTURE_ENTRIES && fgets(buf, sizeof(buf), fp)) {
        if (parse_line(buf, &entries[entry_count], type)) {
            entry_count++;
            (*count_out)++;
        }
    }
    fclose(fp);
}

static void load_captures(void) {
    entry_count = 0;
    pmkid_count = 0;
    hshk_count  = 0;
    load_file(PMKID_LOG_PATH,     'P', &pmkid_count);
    load_file(HANDSHAKE_LOG_PATH, 'H', &hshk_count);
}

static void dump_file(const char *path, const char *label) {
    printf("\n===== %s (%s) =====\n", label, path);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("(no file)\n");
        return;
    }
    char buf[768];
    int  n = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        printf("%s", buf);
        if (buf[strlen(buf) - 1] != '\n') printf("\n");
        n++;
    }
    fclose(fp);
    printf("===== %d entries =====\n\n", n);
}

static void dump_all_to_serial(void) {
    printf("\n\n========================================\n");
    printf("  WiFiend Captures Dump\n");
    printf("========================================\n");
    dump_file(PMKID_LOG_PATH,     "PMKID Captures");
    dump_file(HANDSHAKE_LOG_PATH, "Handshake Captures");
    printf("========================================\n");
    printf("  Dump complete. Copy lines above.\n");
    printf("========================================\n\n");
}

static void clear_all(void) {
    remove(PMKID_LOG_PATH);
    remove(HANDSHAKE_LOG_PATH);
    load_captures();
    selected_idx  = 0;
    scroll_offset = 0;
}

// ---------- public API ----------

void wifi_captures_init(void) {
    ESP_LOGI(TAG, "Captures module ready");
}

void wifi_captures_enter(void) {
    load_captures();
    selected_idx  = 0;
    scroll_offset = 0;
    view_state    = CAP_VIEW_LIST;
    active        = true;
    refresh       = true;
}

// Total selectable rows = entries + 2 action items (Dump, Clear)
static uint16_t total_rows(void) { return entry_count + 2; }

void wifi_captures_scroll_up(void) {
    if (view_state != CAP_VIEW_LIST) return;
    if (selected_idx > 0) {
        selected_idx--;
        if (selected_idx < scroll_offset) scroll_offset = selected_idx;
    }
    refresh = true;
}

void wifi_captures_scroll_down(void) {
    if (view_state != CAP_VIEW_LIST) return;
    if (selected_idx + 1 < total_rows()) {
        selected_idx++;
        if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
    }
    refresh = true;
}

void wifi_captures_select(void) {
    if (view_state == CAP_VIEW_CONFIRM_CLEAR) {
        clear_all();
        view_state = CAP_VIEW_LIST;
        refresh    = true;
        return;
    }
    if (view_state != CAP_VIEW_LIST) return;

    if (selected_idx == entry_count) {
        // "Dump Serial"
        view_state = CAP_VIEW_DUMPING;
        refresh    = true;
        wifi_captures_render();
        dump_all_to_serial();
        view_state = CAP_VIEW_LIST;
        refresh    = true;
    } else if (selected_idx == entry_count + 1) {
        // "Clear All"
        view_state = CAP_VIEW_CONFIRM_CLEAR;
        refresh    = true;
    }
    // selecting an entry row: no detail view (lines are too long for OLED)
}

void wifi_captures_stop(void) {
    active     = false;
    view_state = CAP_VIEW_LIST;
}

bool wifi_captures_is_active(void)     { return active; }
bool wifi_captures_needs_refresh(void) { bool v = refresh; refresh = false; return v; }

// ---------- render ----------

static void render_list(void) {
    ssd1306_clear_buffer();
    char status[32];
    snprintf(status, sizeof(status), "P:%u H:%u",
             (unsigned)pmkid_count, (unsigned)hshk_count);
    status[15] = '\0';
    ssd1306_draw_header("CAPTURES", status);

    uint16_t rows = total_rows();
    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= rows) break;

        char line[32];
        if (idx < entry_count) {
            cap_entry_t *e = &entries[idx];
            const char *ssid = e->ssid[0] ? e->ssid : "(hidden)";
            snprintf(line, sizeof(line), "%c%c %-12.12s",
                     (idx == selected_idx) ? '>' : ' ',
                     e->type, ssid);
        } else if (idx == entry_count) {
            snprintf(line, sizeof(line), "%c[Dump Serial]",
                     (idx == selected_idx) ? '>' : ' ');
        } else {
            snprintf(line, sizeof(line), "%c[Clear All]",
                     (idx == selected_idx) ? '>' : ' ');
        }
        line[16] = '\0';
        ssd1306_draw_string(0, row + 2, line);
    }

    if (entry_count == 0 && total_rows() <= 6) {
        ssd1306_draw_string(0, 7, "LN>back");
    } else {
        ssd1306_draw_string(0, 7, "CK>act LN>back");
    }
    ssd1306_flush();
}

static void render_dumping(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("CAPTURES", "DUMP");
    ssd1306_draw_string(0, 3, "Dumping to");
    ssd1306_draw_string(0, 4, "serial...");
    ssd1306_draw_string(0, 6, "Open monitor");
    ssd1306_draw_string(0, 7, "to copy lines");
    ssd1306_flush();
}

static void render_confirm_clear(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("CAPTURES", "CLEAR?");
    ssd1306_draw_string(0, 3, "Delete ALL");
    ssd1306_draw_string(0, 4, "saved captures?");
    char line[17];
    snprintf(line, sizeof(line), "(%u total)",
             (unsigned)(pmkid_count + hshk_count));
    ssd1306_draw_string(0, 5, line);
    ssd1306_draw_string(0, 7, "CK>YES LN>NO");
    ssd1306_flush();
}

void wifi_captures_render(void) {
    switch (view_state) {
        case CAP_VIEW_LIST:          render_list();          break;
        case CAP_VIEW_DUMPING:       render_dumping();       break;
        case CAP_VIEW_CONFIRM_CLEAR: render_confirm_clear(); break;
    }
}
