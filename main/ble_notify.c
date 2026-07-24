#include "ble_notify.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_notify";

typedef enum {
    ST_PICK = 0,
    ST_CONNECTING,
    ST_DISCOVERING,
    ST_MONITOR,
} notify_state_t;

static volatile bool s_active;
static volatile bool s_want_disc;
static notify_state_t s_state;
static int           s_picker_sel;
static int           s_picker_scroll;
static uint16_t      s_conn;
static bool          s_connected;
static uint16_t      s_cccd_handle;
static uint8_t       s_last_data[20];
static uint8_t       s_last_len;
static uint32_t      s_notify_count;

static uint16_t s_svc_starts[8];
static uint16_t s_svc_ends[8];
static int      s_svc_count;
static int      s_disc_idx;
static volatile bool s_disc_pending;

static int write_cccd_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg);
static int disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg);
static int disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg);

static void subscribe_cccd(void) {
    if (!s_cccd_handle) {
        s_state = ST_PICK;
        s_want_disc = true;
        return;
    }
    uint16_t val = 1;
    ble_gattc_write_flat(s_conn, s_cccd_handle, &val, sizeof(val), write_cccd_cb, NULL);
}

static void disc_next_svc(void) {
    while (s_disc_idx < s_svc_count && !s_cccd_handle) {
        s_disc_pending = true;
        int rc = ble_gattc_disc_all_chrs(s_conn, s_svc_starts[s_disc_idx],
                                         s_svc_ends[s_disc_idx], disc_chr_cb, NULL);
        if (rc != 0) {
            s_disc_pending = false;
            s_disc_idx++;
            continue;
        }
        return;
    }
    if (s_cccd_handle) subscribe_cccd();
    else {
        s_state = ST_PICK;
        s_want_disc = true;
    }
}

static int write_cccd_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (err->status == 0) s_state = ST_MONITOR;
    else {
        ESP_LOGW(TAG, "cccd write %d", err->status);
        s_state = ST_PICK;
        s_want_disc = true;
    }
    return 0;
}

static int disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg) {
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) {
        s_disc_pending = false;
        s_disc_idx++;
        disc_next_svc();
        return 0;
    }
    if (err->status != 0) {
        s_disc_pending = false;
        s_disc_idx++;
        disc_next_svc();
        return 0;
    }
    if (!s_cccd_handle && (chr->properties & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE))) {
        s_cccd_handle = chr->val_handle + 1;
    }
    return 0;
}

static int disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
    (void)conn; (void)arg;
    if (err->status == BLE_HS_EDONE) {
        s_disc_idx = 0;
        disc_next_svc();
        return 0;
    }
    if (err->status != 0) {
        s_state = ST_PICK;
        s_want_disc = true;
        return 0;
    }
    if (s_svc_count < 8) {
        s_svc_starts[s_svc_count] = svc->start_handle;
        s_svc_ends[s_svc_count] = svc->end_handle;
        s_svc_count++;
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status == 0) {
                s_conn = ev->connect.conn_handle;
                s_connected = true;
                s_state = ST_DISCOVERING;
                s_svc_count = 0;
                s_disc_idx = 0;
                s_cccd_handle = 0;
                ble_gattc_disc_all_svcs(s_conn, disc_svc_cb, NULL);
            } else {
                s_state = ST_PICK;
                s_want_disc = true;
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_conn = 0;
            s_cccd_handle = 0;
            if (s_active) {
                s_state = ST_PICK;
                s_want_disc = true;
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            struct os_mbuf *om = ev->notify_rx.om;
            s_last_len = OS_MBUF_PKTLEN(om);
            if (s_last_len > sizeof(s_last_data)) s_last_len = sizeof(s_last_data);
            os_mbuf_copydata(om, 0, s_last_len, s_last_data);
            s_notify_count++;
            break;
        }
        default: break;
    }
    return 0;
}

static void disconnect(void) {
    if (s_connected) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_connected = false;
    s_conn = 0;
    s_cccd_handle = 0;
    s_last_len = 0;
}

static void start_connect(const ble_dev_info_t *dev) {
    ble_scan_disc_stop();
    struct ble_gap_conn_params cp = {0};
    cp.scan_itvl = 0x0010;
    cp.scan_window = 0x0010;
    cp.itvl_min = BLE_GAP_CONN_ITVL_MS(24);
    cp.itvl_max = BLE_GAP_CONN_ITVL_MS(40);
    cp.latency = 0;
    cp.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000);

    ble_addr_t addr;
    memcpy(addr.val, dev->addr, 6);
    addr.type = dev->addr_type;
    s_state = ST_CONNECTING;
    s_notify_count = 0;
    s_last_len = 0;
    int rc = ble_gap_connect(ble_core_own_addr_type(), &addr, 30000, &cp,
                             gap_event_cb, NULL);
    if (rc != 0) {
        s_state = ST_PICK;
        s_want_disc = true;
    }
}

static void render_picker(void) {
    ble_scan_lock();
    uint16_t cnt = 0;
    const ble_dev_info_t *r = ble_scan_get_results(&cnt);
    if (s_picker_sel >= (int)cnt) s_picker_sel = cnt ? (int)cnt - 1 : 0;
    if (s_picker_scroll > s_picker_sel) s_picker_scroll = s_picker_sel;
    if (s_picker_sel >= s_picker_scroll + 6) s_picker_scroll = s_picker_sel - 5;

    char status[24];
    snprintf(status, sizeof(status), "%u/%u", cnt ? (unsigned)s_picker_sel + 1 : 0, (unsigned)cnt);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Notify:pick", status);
    if (cnt == 0) {
        ssd1306_draw_string(0, 4, "  scanning...");
    } else {
        for (int row = 0; row < 6 && s_picker_scroll + row < (int)cnt; row++) {
            int i = s_picker_scroll + row;
            const char *nm = r[i].name[0] ? r[i].name : "(no name)";
            char line[24];
            snprintf(line, sizeof(line), "%c%-9.9s%4d",
                     (i == s_picker_sel) ? '>' : ' ', nm, r[i].rssi);
            ssd1306_draw_string(0, 2 + row, line);
        }
    }
    ssd1306_draw_string(0, 7, "Click=connect");
    ble_scan_unlock();
    ssd1306_flush();
}

static void render_monitor(void) {
    char status[24];
    snprintf(status, sizeof(status), "#%lu", (unsigned long)s_notify_count);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Notify", status);

    char line[20] = {0};
    if (s_last_len == 0) {
        ssd1306_draw_string(0, 4, "  waiting...");
    } else {
        size_t pos = 0;
        for (uint8_t i = 0; i < s_last_len && i < 6; i++)
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X ", s_last_data[i]);
        ssd1306_draw_string(0, 4, line);
        if (s_last_len > 6) {
            pos = 0;
            memset(line, 0, sizeof(line));
            for (uint8_t i = 6; i < s_last_len; i++)
                pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X ", s_last_data[i]);
            ssd1306_draw_string(0, 5, line);
        }
    }
    ssd1306_draw_string(0, 7, "Click=stop");
    ssd1306_flush();
}

void ble_notify_enter(void) {
    ble_scan_clear();
    s_active = true;
    s_want_disc = true;
    s_state = ST_PICK;
    s_picker_sel = 0;
    s_picker_scroll = 0;
    s_connected = false;
    radio_mgr_enter(RADIO_MODE_BLE_ACTIVE);
}

void ble_notify_exit(void) {
    s_active = false;
    s_want_disc = false;
    disconnect();
    ble_scan_disc_stop();
    radio_mgr_leave(RADIO_MODE_BLE_ACTIVE);
}

void ble_notify_tick(void) {
    if (!s_active) return;
    if (s_want_disc && s_state == ST_PICK && ble_core_is_ready())
        ble_scan_disc_start();
    if (s_state == ST_DISCOVERING && !s_disc_pending && s_disc_idx < s_svc_count)
        disc_next_svc();

    switch (s_state) {
        case ST_PICK: render_picker(); break;
        case ST_CONNECTING:
            ssd1306_clear_buffer();
            ssd1306_draw_header("Notify", "...");
            ssd1306_draw_string(0, 4, "  connect...");
            ssd1306_flush();
            break;
        case ST_DISCOVERING:
            ssd1306_clear_buffer();
            ssd1306_draw_header("Notify", "...");
            ssd1306_draw_string(0, 4, "  subscribe...");
            ssd1306_flush();
            break;
        case ST_MONITOR: render_monitor(); break;
    }
}

void ble_notify_input(encoder_event_t e) {
    if (!s_active) return;
    if (s_state != ST_PICK) {
        if (e == ENCODER_CLICK) {
            disconnect();
            s_state = ST_PICK;
            s_want_disc = true;
        }
        return;
    }
    switch (e) {
        case ENCODER_CW: {
            uint16_t cnt = 0;
            ble_scan_lock();
            ble_scan_get_results(&cnt);
            ble_scan_unlock();
            if (cnt > 0 && s_picker_sel + 1 < (int)cnt) s_picker_sel++;
            break;
        }
        case ENCODER_CCW:
            if (s_picker_sel > 0) s_picker_sel--;
            break;
        case ENCODER_CLICK: {
            ble_scan_lock();
            uint16_t cnt = 0;
            const ble_dev_info_t *r = ble_scan_get_results(&cnt);
            if (cnt > 0 && s_picker_sel < (int)cnt)
                start_connect(&r[s_picker_sel]);
            ble_scan_unlock();
            break;
        }
        default: break;
    }
}
