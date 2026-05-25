#include "ble_scan.h"
#include "ble_core.h"
#include "ble_ident.h"
#include "ssd1306.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_scan";

static ble_dev_info_t   s_results[MAX_BLE_RESULTS];
static uint16_t         s_count;
static uint16_t         s_selected;
static uint16_t         s_scroll;
static SemaphoreHandle_t s_mutex;
static volatile bool    s_discing;

// Scanner tool state
static volatile bool s_tool_active;
static volatile bool s_show_detail;
static volatile bool s_want_disc;     // tick will start disc once core is ready

// ---------- shared-core helpers ----------

static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

void ble_scan_lock(void)   { ensure_mutex(); xSemaphoreTake(s_mutex, portMAX_DELAY); }
void ble_scan_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

const ble_dev_info_t *ble_scan_get_results(uint16_t *count) {
    if (count) *count = s_count;
    return s_results;
}

void ble_scan_clear(void) {
    ble_scan_lock();
    s_count    = 0;
    s_selected = 0;
    s_scroll   = 0;
    memset(s_results, 0, sizeof(s_results));
    ble_scan_unlock();
}

// Find entry by addr (linear scan; MAX_BLE_RESULTS is small). Returns -1 if absent.
static int find_idx(const uint8_t *addr) {
    for (uint16_t i = 0; i < s_count; i++)
        if (memcmp(s_results[i].addr, addr, 6) == 0) return (int)i;
    return -1;
}

// Apply one disc report to the results table (called under mutex).
static void apply_report(struct ble_gap_disc_desc *d) {
    int idx = find_idx(d->addr.val);
    ble_dev_info_t *e;
    if (idx < 0) {
        if (s_count >= MAX_BLE_RESULTS) return;     // table full — drop further new devices
        e = &s_results[s_count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->addr, d->addr.val, 6);
        e->addr_type  = d->addr.type;
        e->company_id = 0xFFFF;
        e->tx_pwr     = 127;
        e->rssi_peak  = -128;
    } else {
        e = &s_results[idx];
    }

    e->rssi         = d->rssi;
    if (d->rssi > e->rssi_peak) e->rssi_peak = d->rssi;
    e->evt_type     = d->event_type;
    e->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;

    if (f.name && f.name_len > 0) {
        uint8_t n = f.name_len < (BLE_NAME_LEN - 1) ? f.name_len : (BLE_NAME_LEN - 1);
        memcpy(e->name, f.name, n);
        e->name[n] = '\0';
    }
    if (f.mfg_data && f.mfg_data_len >= 2)
        e->company_id = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));
    if (f.num_uuids16 > 0 && f.uuids16)
        e->svc_uuid16 = f.uuids16[0].value;
    if (f.appearance_is_present) e->appearance = f.appearance;
    if (f.tx_pwr_lvl_is_present) e->tx_pwr     = f.tx_pwr_lvl;
    e->flags = f.flags;

    // Beacon detect: store raw for the data-bearing report and tag the type.
    if ((f.mfg_data && f.mfg_data_len > 0) || (f.svc_data_uuid16 && f.svc_data_uuid16_len > 0)) {
        uint8_t n = d->length_data < 31 ? d->length_data : 31;
        memcpy(e->raw, d->data, n);
        e->raw_len = n;

        ibeacon_t ib;
        eddystone_t ed;
        if (ble_decode_ibeacon(e->raw, e->raw_len, &ib))        e->beacon_type = BLE_BEACON_IBEACON;
        else if (ble_decode_eddystone(e->raw, e->raw_len, &ed)) e->beacon_type = BLE_BEACON_EDDYSTONE;
    }
}

// ---------- GAP event callback (runs on the NimBLE host task) ----------

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            ble_scan_lock();
            apply_report(&event->disc);
            ble_scan_unlock();
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "disc complete reason=%d", event->disc_complete.reason);
            s_discing = false;
            break;
        default: break;
    }
    return 0;
}

// ---------- disc lifecycle ----------

void ble_scan_disc_start(void) {
    if (s_discing) return;
    if (!ble_core_is_ready()) {
        ESP_LOGW(TAG, "disc_start: core not ready");
        return;
    }
    struct ble_gap_disc_params dp = {0};
    dp.passive          = 0;   // active: solicit scan responses (better names)
    dp.filter_duplicates = 0;  // keep RSSI updates for hunter / fresh data
    int rc = ble_gap_disc(ble_core_own_addr_type(), BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return;
    }
    s_discing = true;
    ESP_LOGI(TAG, "BLE discovery started");
}

void ble_scan_disc_stop(void) {
    if (!s_discing) return;
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "disc_cancel rc=%d", rc);
    s_discing = false;
}

// ---------- helpers for the scanner UI ----------

static void fmt_addr_full(const ble_dev_info_t *e, char *out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             e->addr[5], e->addr[4], e->addr[3], e->addr[2], e->addr[1], e->addr[0]);
}
static const char *addr_type_str(uint8_t t) {
    return (t == BLE_ADDR_PUBLIC) ? "Pub" : "Rnd";
}
static bool evt_is_connectable(uint8_t et) {
    return et == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || et == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
}

// ---------- rendering ----------

static void render_list(void) {
    char status[24];
    snprintf(status, sizeof(status), "%u/%u", s_count ? (unsigned)(s_selected + 1) : 0, (unsigned)s_count);
    ssd1306_clear_buffer();
    ssd1306_draw_header("BLE Scan", status);

    if (s_count == 0) {
        ssd1306_draw_string(0, 4, "  Scanning...");
        ssd1306_flush();
        return;
    }

    // Adjust scroll window to keep selected visible (6 rows on pages 2-7)
    if (s_selected < s_scroll)         s_scroll = s_selected;
    if (s_selected >= s_scroll + 6)    s_scroll = s_selected - 5;

    for (uint16_t r = 0; r < 6 && (s_scroll + r) < s_count; r++) {
        uint16_t i = s_scroll + r;
        const ble_dev_info_t *e = &s_results[i];
        const char *nm = e->name[0] ? e->name : "(no name)";
        char row[24];
        snprintf(row, sizeof(row), "%c%-10.10s%4d",
                 (i == s_selected) ? '>' : ' ', nm, e->rssi);
        ssd1306_draw_string(0, 2 + r, row);
    }
    ssd1306_flush();
}

static void render_detail(void) {
    if (s_count == 0) { render_list(); return; }
    const ble_dev_info_t *e = &s_results[s_selected];

    char status[24];
    snprintf(status, sizeof(status), "%4d dBm", e->rssi);
    ssd1306_clear_buffer();
    ssd1306_draw_header("BLE Dev", status);

    char line[20];
    snprintf(line, sizeof(line), "%-16.16s", e->name[0] ? e->name : "(no name)");
    ssd1306_draw_string(0, 2, line);

    char mac[18]; fmt_addr_full(e, mac, sizeof(mac));
    ssd1306_draw_string(0, 3, mac);

    snprintf(line, sizeof(line), "%s %s  Sv:%04X",
             addr_type_str(e->addr_type),
             evt_is_connectable(e->evt_type) ? "Con" : "NoC",
             e->svc_uuid16);
    ssd1306_draw_string(0, 4, line);

    snprintf(line, sizeof(line), "Co:%04X Ap:%04X",
             e->company_id, e->appearance);
    ssd1306_draw_string(0, 5, line);

    snprintf(line, sizeof(line), "Type: %s", ble_classify_device(e));
    ssd1306_draw_string(0, 6, line);

    if (e->beacon_type == BLE_BEACON_IBEACON)        ssd1306_draw_string(0, 7, "iBeacon payload");
    else if (e->beacon_type == BLE_BEACON_EDDYSTONE) ssd1306_draw_string(0, 7, "Eddystone payload");
    else ssd1306_draw_string(0, 7, "Long>back");
    ssd1306_flush();
}

// ---------- scanner tool API ----------

void ble_scan_enter(void) {
    ensure_mutex();
    ble_scan_clear();
    s_tool_active = true;
    s_show_detail = false;
    s_want_disc   = true;       // tick will start disc once core is ready
}

void ble_scan_exit(void) {
    s_tool_active = false;
    s_want_disc   = false;
    ble_scan_disc_stop();
}

void ble_scan_tick(void) {
    if (!s_tool_active) return;
    if (s_want_disc && !s_discing && ble_core_is_ready()) {
        ble_scan_disc_start();
    }
    ble_scan_lock();
    if (s_show_detail) render_detail();
    else               render_list();
    ble_scan_unlock();
}

void ble_scan_input(encoder_event_t e) {
    if (!s_tool_active) return;
    if (s_show_detail) {
        if (e == ENCODER_CLICK) s_show_detail = false;
        return;
    }
    switch (e) {
        case ENCODER_CW:
            if (s_count > 0 && s_selected + 1 < s_count) s_selected++;
            break;
        case ENCODER_CCW:
            if (s_selected > 0) s_selected--;
            break;
        case ENCODER_CLICK:
            if (s_count > 0) s_show_detail = true;
            break;
        default: break;
    }
}
