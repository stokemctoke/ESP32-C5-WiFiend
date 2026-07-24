#include "ble_advlog.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ble_advlog";

#define LOG_PATH      "/lfs/ble.log"
#define LOG_CAP_BYTES (256 * 1024)

static volatile bool s_active;
static volatile bool s_logging;
static volatile bool s_want_disc;
static uint32_t      s_logged_count;
static uint32_t      s_last_logged_ms[MAX_BLE_RESULTS];

static void rotate_log_if_needed(void) {
    FILE *fp = fopen(LOG_PATH, "r");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    if (sz < LOG_CAP_BYTES) return;
    remove(LOG_PATH ".old");
    rename(LOG_PATH, LOG_PATH ".old");
}

static void append_line(const ble_dev_info_t *e) {
    rotate_log_if_needed();
    FILE *fp = fopen(LOG_PATH, "a");
    if (!fp) return;
    fprintf(fp, "%02X:%02X:%02X:%02X:%02X:%02X,%d,\"%s\",%04X\n",
            e->addr[5], e->addr[4], e->addr[3], e->addr[2], e->addr[1], e->addr[0],
            (int)e->rssi,
            e->name[0] ? e->name : "",
            e->company_id);
    fclose(fp);
    s_logged_count++;
}

static void process_results(void) {
    if (!s_logging) return;
    ble_scan_lock();
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    for (uint16_t i = 0; i < cnt && i < MAX_BLE_RESULTS; i++) {
        if (r[i].last_seen_ms != s_last_logged_ms[i]) {
            append_line(&r[i]);
            s_last_logged_ms[i] = r[i].last_seen_ms;
        }
    }
    ble_scan_unlock();
}

static void render(void) {
    char status[24];
    snprintf(status, sizeof(status), "%s %lu",
             s_logging ? "REC" : "OFF", (unsigned long)s_logged_count);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Adv Logger", status);
    ssd1306_draw_string(0, 3, s_logging ? "  logging..." : "  paused");
    ssd1306_draw_string(0, 4, LOG_PATH);
    ssd1306_draw_string(0, 6, "Click=toggle log");
    ssd1306_draw_string(0, 7, "Long>back");
    ssd1306_flush();
}

void ble_advlog_enter(void) {
    ble_scan_clear();
    memset(s_last_logged_ms, 0, sizeof(s_last_logged_ms));
    s_logged_count = 0;
    s_active = true;
    s_logging = true;
    s_want_disc = true;
    radio_mgr_enter(RADIO_MODE_BLE_RECON);
}

void ble_advlog_exit(void) {
    s_active = false;
    s_logging = false;
    s_want_disc = false;
    ble_scan_disc_stop();
    radio_mgr_leave(RADIO_MODE_BLE_RECON);
}

void ble_advlog_tick(void) {
    if (!s_active) return;
    if (s_want_disc && ble_core_is_ready()) ble_scan_disc_start();
    process_results();
    render();
}

void ble_advlog_input(encoder_event_t e) {
    if (!s_active) return;
    if (e == ENCODER_CLICK) s_logging = !s_logging;
}
