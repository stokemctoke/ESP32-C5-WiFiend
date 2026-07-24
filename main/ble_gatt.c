#include "ble_gatt.h"
#include "ble_scan.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_gatt";

#define MAX_GATT_SVC  8
#define MAX_GATT_CHR  24

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t       start;
    uint16_t       end;
} gatt_svc_t;

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t       val_handle;
    uint8_t        props;
} gatt_chr_t;

typedef enum {
    ST_PICK = 0,
    ST_CONNECTING,
    ST_DISCOVERING,
    ST_LIST,
} gatt_state_t;

static volatile bool s_active;
static volatile bool s_want_disc;
static gatt_state_t  s_state;
static int           s_picker_sel;
static int           s_picker_scroll;
static int           s_list_sel;
static int           s_list_scroll;
static uint16_t      s_conn;
static bool          s_connected;

static gatt_svc_t s_svcs[MAX_GATT_SVC];
static int        s_svc_count;
static gatt_chr_t s_chrs[MAX_GATT_CHR];
static int        s_chr_count;
static int        s_disc_svc_idx;
static volatile bool s_disc_pending;

static int disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg);

static void fmt_uuid_short(const ble_uuid_any_t *u, char *out, size_t n) {
    if (u->u.type == BLE_UUID_TYPE_16)
        snprintf(out, n, "%04X", u->u16.value);
    else
        snprintf(out, n, "128b");
}

static void fmt_props(uint8_t p, char *out, size_t n) {
    snprintf(out, n, "%c%c%c%c",
             (p & BLE_GATT_CHR_F_READ)     ? 'R' : '-',
             (p & BLE_GATT_CHR_F_WRITE)    ? 'W' : '-',
             (p & BLE_GATT_CHR_F_NOTIFY)   ? 'N' : '-',
             (p & BLE_GATT_CHR_F_INDICATE) ? 'I' : '-');
}

static void disconnect(void) {
    if (s_connected) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_connected = false;
    }
    s_conn = 0;
    s_svc_count = 0;
    s_chr_count = 0;
    s_disc_svc_idx = 0;
}

static void disc_next_svc(void) {
    if (s_disc_svc_idx >= s_svc_count) {
        s_state = ST_LIST;
        s_list_sel = 0;
        s_list_scroll = 0;
        s_disc_pending = false;
        return;
    }
    gatt_svc_t *svc = &s_svcs[s_disc_svc_idx];
    s_disc_pending = true;
    int rc = ble_gattc_disc_all_chrs(s_conn, svc->start, svc->end, disc_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "disc chrs rc=%d", rc);
        s_disc_pending = false;
        s_state = ST_LIST;
    }
}

static int disc_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (err->status == BLE_HS_EDONE) {
        s_disc_svc_idx++;
        s_disc_pending = false;
        disc_next_svc();
        return 0;
    }
    if (err->status != 0) {
        ESP_LOGW(TAG, "disc chr status=%d", err->status);
        s_disc_pending = false;
        s_state = ST_LIST;
        return 0;
    }
    if (s_chr_count < MAX_GATT_CHR) {
        gatt_chr_t *c = &s_chrs[s_chr_count++];
        c->uuid = chr->uuid;
        c->val_handle = chr->val_handle;
        c->props = chr->properties;
    }
    return 0;
}

static int disc_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
    if (err->status == BLE_HS_EDONE) {
        s_disc_svc_idx = 0;
        disc_next_svc();
        return 0;
    }
    if (err->status != 0) {
        ESP_LOGW(TAG, "disc svc status=%d", err->status);
        s_state = ST_LIST;
        return 0;
    }
    if (s_svc_count < MAX_GATT_SVC) {
        gatt_svc_t *e = &s_svcs[s_svc_count++];
        e->uuid = svc->uuid;
        e->start = svc->start_handle;
        e->end = svc->end_handle;
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *ev, void *arg) {
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status == 0) {
                s_conn = ev->connect.conn_handle;
                s_connected = true;
                s_state = ST_DISCOVERING;
                s_svc_count = 0;
                s_chr_count = 0;
                s_disc_svc_idx = 0;
                ble_gattc_disc_all_svcs(s_conn, disc_svc_cb, NULL);
            } else {
                ESP_LOGW(TAG, "connect failed %d", ev->connect.status);
                s_state = ST_PICK;
                s_want_disc = true;
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_conn = 0;
            if (s_active && s_state != ST_PICK) {
                s_state = ST_PICK;
                s_want_disc = true;
            }
            break;
        default: break;
    }
    return 0;
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
    int rc = ble_gap_connect(ble_core_own_addr_type(), &addr, 30000, &cp,
                             gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
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
    ssd1306_draw_header("GATT:pick", status);
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

static void render_status(const char *msg) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("GATT", "...");
    ssd1306_draw_string(0, 4, msg);
    ssd1306_flush();
}

static void render_list(void) {
    if (s_list_sel >= s_chr_count) s_list_sel = s_chr_count ? s_chr_count - 1 : 0;
    if (s_list_scroll > s_list_sel) s_list_scroll = s_list_sel;
    if (s_list_sel >= s_list_scroll + 6) s_list_scroll = s_list_sel - 5;

    char status[24];
    snprintf(status, sizeof(status), "%d/%d", s_chr_count ? s_list_sel + 1 : 0, s_chr_count);
    ssd1306_clear_buffer();
    ssd1306_draw_header("GATT", status);

    if (s_chr_count == 0) {
        ssd1306_draw_string(0, 4, "  no chars");
    } else {
        for (int row = 0; row < 6 && s_list_scroll + row < s_chr_count; row++) {
            int i = s_list_scroll + row;
            char uuid[8], props[8], line[24];
            fmt_uuid_short(&s_chrs[i].uuid, uuid, sizeof(uuid));
            fmt_props(s_chrs[i].props, props, sizeof(props));
            snprintf(line, sizeof(line), "%c%s %s h%04X",
                     (i == s_list_sel) ? '>' : ' ', uuid, props, s_chrs[i].val_handle);
            ssd1306_draw_string(0, 2 + row, line);
        }
    }
    ssd1306_draw_string(0, 7, "Click=disconnect");
    ssd1306_flush();
}

void ble_gatt_enter(void) {
    ble_scan_clear();
    s_active = true;
    s_want_disc = true;
    s_state = ST_PICK;
    s_picker_sel = 0;
    s_picker_scroll = 0;
    s_connected = false;
    s_conn = 0;
    radio_mgr_enter(RADIO_MODE_BLE_ACTIVE);
}

void ble_gatt_exit(void) {
    s_active = false;
    s_want_disc = false;
    disconnect();
    ble_scan_disc_stop();
    radio_mgr_leave(RADIO_MODE_BLE_ACTIVE);
}

void ble_gatt_tick(void) {
    if (!s_active) return;
    if (s_want_disc && s_state == ST_PICK && ble_core_is_ready())
        ble_scan_disc_start();
    if (s_state == ST_DISCOVERING && !s_disc_pending && s_disc_svc_idx < s_svc_count)
        disc_next_svc();

    switch (s_state) {
        case ST_PICK:       render_picker(); break;
        case ST_CONNECTING: render_status("  connect..."); break;
        case ST_DISCOVERING:render_status("  discover..."); break;
        case ST_LIST:       render_list(); break;
    }
}

void ble_gatt_input(encoder_event_t e) {
    if (!s_active) return;
    if (s_state == ST_LIST) {
        switch (e) {
            case ENCODER_CW:
                if (s_chr_count > 0 && s_list_sel + 1 < s_chr_count) s_list_sel++;
                break;
            case ENCODER_CCW:
                if (s_list_sel > 0) s_list_sel--;
                break;
            case ENCODER_CLICK:
                disconnect();
                s_state = ST_PICK;
                s_want_disc = true;
                break;
            default: break;
        }
        return;
    }
    if (s_state != ST_PICK) return;
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
