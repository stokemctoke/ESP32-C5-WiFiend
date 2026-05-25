#include "ble_beacon.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "ble_ident.h"
#include "ssd1306.h"
#include <stdio.h>
#include <string.h>

static volatile bool s_active;
static volatile bool s_show_detail;
static volatile bool s_want_disc;
static int s_selected;
static int s_scroll;

// Build a filtered index of result entries that are beacons. Returns count.
// Caller must hold the scan lock.
static int filter_beacons(uint16_t *idx_out, int max) {
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    int n = 0;
    for (uint16_t i = 0; i < cnt && n < max; i++)
        if (r[i].beacon_type != BLE_BEACON_NONE) idx_out[n++] = i;
    return n;
}

static void render_list(void) {
    uint16_t idx[MAX_BLE_RESULTS];
    ble_scan_lock();
    int n = filter_beacons(idx, MAX_BLE_RESULTS);
    if (s_selected >= n)          s_selected = n > 0 ? n - 1 : 0;
    if (s_scroll > s_selected)    s_scroll   = s_selected;
    if (s_selected >= s_scroll+6) s_scroll   = s_selected - 5;

    char status[24];
    snprintf(status, sizeof(status), "%d/%d", n > 0 ? s_selected + 1 : 0, n);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Beacons", status);

    if (n == 0) {
        ssd1306_draw_string(0, 4, "  scanning...");
    } else {
        const ble_dev_info_t *r = ble_scan_get_results(NULL);
        for (int row = 0; row < 6 && s_scroll + row < n; row++) {
            int i = idx[s_scroll + row];
            const ble_dev_info_t *e = &r[i];
            const char *tag = (e->beacon_type == BLE_BEACON_IBEACON) ? "iB " : "Ed ";
            const char *nm  = e->name[0] ? e->name :
                              (e->beacon_type == BLE_BEACON_IBEACON ? "iBeacon" : "Eddystone");
            char line[24];
            snprintf(line, sizeof(line), "%c%s%-8.8s%4d",
                     (s_scroll + row == s_selected) ? '>' : ' ', tag, nm, e->rssi);
            ssd1306_draw_string(0, 2 + row, line);
        }
    }
    ble_scan_unlock();
    ssd1306_flush();
}

static void render_detail(void) {
    uint16_t idx[MAX_BLE_RESULTS];
    ble_scan_lock();
    int n = filter_beacons(idx, MAX_BLE_RESULTS);
    if (n == 0) { ble_scan_unlock(); render_list(); return; }
    if (s_selected >= n) s_selected = n - 1;
    const ble_dev_info_t *r = ble_scan_get_results(NULL);
    const ble_dev_info_t *e = &r[idx[s_selected]];

    ssd1306_clear_buffer();
    char status[24];
    snprintf(status, sizeof(status), "%4d dBm", e->rssi);
    ssd1306_draw_header(e->beacon_type == BLE_BEACON_IBEACON ? "iBeacon" : "Eddystone", status);

    char l[20];
    if (e->beacon_type == BLE_BEACON_IBEACON) {
        ibeacon_t ib;
        if (ble_decode_ibeacon(e->raw, e->raw_len, &ib)) {
            snprintf(l, sizeof(l), "%02X%02X%02X%02X%02X%02X%02X%02X",
                     ib.uuid[0], ib.uuid[1], ib.uuid[2], ib.uuid[3],
                     ib.uuid[4], ib.uuid[5], ib.uuid[6], ib.uuid[7]);
            ssd1306_draw_string(0, 2, l);
            snprintf(l, sizeof(l), "%02X%02X%02X%02X%02X%02X%02X%02X",
                     ib.uuid[8],  ib.uuid[9],  ib.uuid[10], ib.uuid[11],
                     ib.uuid[12], ib.uuid[13], ib.uuid[14], ib.uuid[15]);
            ssd1306_draw_string(0, 3, l);
            snprintf(l, sizeof(l), "Major: %5u", ib.major);  ssd1306_draw_string(0, 4, l);
            snprintf(l, sizeof(l), "Minor: %5u", ib.minor);  ssd1306_draw_string(0, 5, l);
            snprintf(l, sizeof(l), "Power: %4d dBm", ib.power); ssd1306_draw_string(0, 6, l);
        }
    } else {
        eddystone_t ed;
        if (ble_decode_eddystone(e->raw, e->raw_len, &ed)) {
            if (ed.frame == 0x10) {
                ssd1306_draw_string(0, 2, "URL frame");
                char buf[17];
                snprintf(buf, sizeof(buf), "%-16.16s", ed.url);
                ssd1306_draw_string(0, 3, buf);
                if (strlen(ed.url) > 16) {
                    snprintf(buf, sizeof(buf), "%-16.16s", ed.url + 16);
                    ssd1306_draw_string(0, 4, buf);
                }
                if (strlen(ed.url) > 32) {
                    snprintf(buf, sizeof(buf), "%-16.16s", ed.url + 32);
                    ssd1306_draw_string(0, 5, buf);
                }
                snprintf(l, sizeof(l), "Power:%4d dBm", ed.tx_power);
                ssd1306_draw_string(0, 6, l);
            } else if (ed.frame == 0x00) {
                ssd1306_draw_string(0, 2, "UID frame");
                snprintf(l, sizeof(l), "NS:%02X%02X%02X%02X%02X",
                         ed.namespace_id[0], ed.namespace_id[1], ed.namespace_id[2],
                         ed.namespace_id[3], ed.namespace_id[4]);
                ssd1306_draw_string(0, 3, l);
                snprintf(l, sizeof(l), "   %02X%02X%02X%02X%02X",
                         ed.namespace_id[5], ed.namespace_id[6], ed.namespace_id[7],
                         ed.namespace_id[8], ed.namespace_id[9]);
                ssd1306_draw_string(0, 4, l);
                snprintf(l, sizeof(l), "I:%02X%02X%02X%02X%02X%02X",
                         ed.instance[0], ed.instance[1], ed.instance[2],
                         ed.instance[3], ed.instance[4], ed.instance[5]);
                ssd1306_draw_string(0, 5, l);
                snprintf(l, sizeof(l), "Power:%4d dBm", ed.tx_power);
                ssd1306_draw_string(0, 6, l);
            } else if (ed.frame == 0x20) {
                ssd1306_draw_string(0, 2, "TLM (telemetry)");
            }
        }
    }
    ssd1306_draw_string(0, 7, "Click=back");
    ble_scan_unlock();
    ssd1306_flush();
}

void ble_beacon_enter(void) {
    ble_scan_clear();
    s_active = true; s_show_detail = false;
    s_selected = 0;  s_scroll = 0;
    s_want_disc = true;
}

void ble_beacon_exit(void) {
    s_active = false; s_want_disc = false;
    ble_scan_disc_stop();
}

void ble_beacon_tick(void) {
    if (!s_active) return;
    if (s_want_disc && ble_core_is_ready()) ble_scan_disc_start();
    if (s_show_detail) render_detail();
    else               render_list();
}

void ble_beacon_input(encoder_event_t e) {
    if (!s_active) return;
    if (s_show_detail) {
        if (e == ENCODER_CLICK) s_show_detail = false;
        return;
    }
    if      (e == ENCODER_CW)    s_selected++;          // clamp happens in render
    else if (e == ENCODER_CCW)   { if (s_selected > 0) s_selected--; }
    else if (e == ENCODER_CLICK) s_show_detail = true;
}
