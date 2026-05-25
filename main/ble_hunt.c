#include "ble_hunt.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "ssd1306.h"
#include "neopixel.h"
#include <stdio.h>
#include <string.h>

static volatile bool s_active;
static volatile bool s_tracking;
static volatile bool s_want_disc;
static int s_picker_sel;
static int s_picker_scroll;
static uint8_t s_tgt_addr[6];

static pixel_color_t rssi_color(int rssi) {
    if (rssi >= -50) return COLOR_GREEN;
    if (rssi >= -65) return MAKE_COLOR(0, 255, 128);
    if (rssi >= -75) return COLOR_YELLOW;
    if (rssi >= -85) return MAKE_COLOR(255, 128, 0);
    return COLOR_RED;
}

static const ble_dev_info_t *find_target(void) {
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    for (uint16_t i = 0; i < cnt; i++)
        if (memcmp(r[i].addr, s_tgt_addr, 6) == 0) return &r[i];
    return NULL;
}

static void render_picker(void) {
    ble_scan_lock();
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    if (s_picker_sel >= cnt)             s_picker_sel    = cnt ? cnt - 1 : 0;
    if (s_picker_scroll > s_picker_sel)  s_picker_scroll = s_picker_sel;
    if (s_picker_sel >= s_picker_scroll + 6) s_picker_scroll = s_picker_sel - 5;

    char status[24];
    snprintf(status, sizeof(status), "%u/%u", cnt ? (unsigned)s_picker_sel + 1 : 0, (unsigned)cnt);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Hunt:pick", status);
    if (cnt == 0) {
        ssd1306_draw_string(0, 4, "  scanning...");
    } else {
        for (uint16_t row = 0; row < 6 && s_picker_scroll + row < cnt; row++) {
            uint16_t i = s_picker_scroll + row;
            const ble_dev_info_t *e = &r[i];
            const char *nm = e->name[0] ? e->name : "(no name)";
            char line[24];
            snprintf(line, sizeof(line), "%c%-9.9s%4d",
                     (i == s_picker_sel) ? '>' : ' ', nm, e->rssi);
            ssd1306_draw_string(0, 2 + row, line);
        }
    }
    ble_scan_unlock();
    ssd1306_flush();
}

static void render_track(void) {
    ble_scan_lock();
    const ble_dev_info_t *e = find_target();

    if (!e) {
        ble_scan_unlock();
        ssd1306_clear_buffer();
        ssd1306_draw_header("Hunt", "...");
        ssd1306_draw_string(0, 3, "  searching...");
        ssd1306_draw_string(0, 7, "Click=pick again");
        neopixel_set_color(COLOR_BLUE);
        ssd1306_flush();
        return;
    }

    char status[24];
    snprintf(status, sizeof(status), "%4d dBm", e->rssi);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Hunt", status);
    char nm[17]; snprintf(nm, sizeof(nm), "%-16.16s", e->name[0] ? e->name : "(no name)");
    ssd1306_draw_string(0, 2, nm);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e->addr[5], e->addr[4], e->addr[3], e->addr[2], e->addr[1], e->addr[0]);
    ssd1306_draw_string(0, 3, mac);

    // RSSI bar: map -100..-30 dBm -> 0..128 px
    int r = e->rssi; if (r < -100) r = -100; if (r > -30) r = -30;
    int bar = (r + 100) * 128 / 70;
    ssd1306_fill_rect(0, 36, bar, 10);
    // Peak marker
    int rp = e->rssi_peak; if (rp < -100) rp = -100; if (rp > -30) rp = -30;
    int peak_x = (rp + 100) * 128 / 70;
    if (peak_x > 1 && peak_x < 128) ssd1306_fill_rect(peak_x - 1, 33, 2, 3);

    char l[17];
    snprintf(l, sizeof(l), "Peak:%4d dBm", e->rssi_peak);
    ssd1306_draw_string(0, 6, l);
    ssd1306_draw_string(0, 7, "Click=pick again");
    int rssi = e->rssi;
    ble_scan_unlock();
    neopixel_set_color(rssi_color(rssi));
    ssd1306_flush();
}

void ble_hunt_enter(void) {
    ble_scan_clear();
    s_active = true; s_tracking = false;
    s_picker_sel = 0; s_picker_scroll = 0;
    s_want_disc = true;
}

void ble_hunt_exit(void) {
    s_active = false; s_want_disc = false;
    ble_scan_disc_stop();
}

void ble_hunt_tick(void) {
    if (!s_active) return;
    if (s_want_disc && ble_core_is_ready()) ble_scan_disc_start();
    if (s_tracking) {
        render_track();   // sets NeoPixel by RSSI
    } else {
        render_picker();
        neopixel_pulse(COLOR_BLUE);
    }
}

void ble_hunt_input(encoder_event_t e) {
    if (!s_active) return;
    if (s_tracking) {
        if (e == ENCODER_CLICK) s_tracking = false;
        return;
    }
    if (e == ENCODER_CW) {
        uint16_t cnt = 0; ble_scan_get_results(&cnt);
        if (cnt > 0 && s_picker_sel + 1 < (int)cnt) s_picker_sel++;
    } else if (e == ENCODER_CCW) {
        if (s_picker_sel > 0) s_picker_sel--;
    } else if (e == ENCODER_CLICK) {
        ble_scan_lock();
        uint16_t cnt = 0;
        const ble_dev_info_t *r = ble_scan_get_results(&cnt);
        if (cnt > 0) {
            memcpy(s_tgt_addr, r[s_picker_sel].addr, 6);
            s_tracking = true;
        }
        ble_scan_unlock();
    }
}
