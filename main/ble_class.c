#include "ble_class.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "ble_ident.h"
#include "ssd1306.h"
#include <stdio.h>

static volatile bool s_active;
static volatile bool s_want_disc;
static int s_selected;
static int s_scroll;

static void render(void) {
    ble_scan_lock();
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    if (s_selected >= cnt)           s_selected = cnt ? cnt - 1 : 0;
    if (s_scroll > s_selected)       s_scroll   = s_selected;
    if (s_selected >= s_scroll + 6)  s_scroll   = s_selected - 5;

    char status[24];
    snprintf(status, sizeof(status), "%u/%u", cnt ? (unsigned)s_selected + 1 : 0, (unsigned)cnt);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Classify", status);

    if (cnt == 0) {
        ssd1306_draw_string(0, 4, "  scanning...");
    } else {
        for (uint16_t row = 0; row < 6 && s_scroll + row < cnt; row++) {
            uint16_t i = s_scroll + row;
            const ble_dev_info_t *e = &r[i];
            const char *type = ble_classify_device(e);
            const char *nm   = e->name[0] ? e->name : "(no name)";
            char line[24];
            // selection char + 7-char type + 5-char name + 4-char rssi = 16
            snprintf(line, sizeof(line), "%c%-7.7s%-5.5s%4d",
                     (i == s_selected) ? '>' : ' ', type, nm, e->rssi);
            ssd1306_draw_string(0, 2 + row, line);
        }
    }
    ble_scan_unlock();
    ssd1306_flush();
}

void ble_class_enter(void) {
    ble_scan_clear();
    s_active = true; s_selected = 0; s_scroll = 0; s_want_disc = true;
}

void ble_class_exit(void) {
    s_active = false; s_want_disc = false;
    ble_scan_disc_stop();
}

void ble_class_tick(void) {
    if (!s_active) return;
    if (s_want_disc && ble_core_is_ready()) ble_scan_disc_start();
    render();
}

void ble_class_input(encoder_event_t e) {
    if (!s_active) return;
    if      (e == ENCODER_CW)  { uint16_t cnt = 0; ble_scan_get_results(&cnt);
                                 if (cnt > 0 && s_selected + 1 < (int)cnt) s_selected++; }
    else if (e == ENCODER_CCW) { if (s_selected > 0) s_selected--; }
}
