#include "ble_hid.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_hid";
static const char *NVS_NS = "ble_hid";
static const char *NVS_KEY = "payload";

#define HID_PAYLOAD_MAX 32

static volatile bool s_active;
static volatile bool s_advertising;
static char          s_payload[HID_PAYLOAD_MAX + 1];
static uint16_t      s_conn;
static bool          s_connected;
static bool          s_svcs_registered;

static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xC0
};

static uint16_t s_hid_map_handle;
static uint16_t s_hid_report_handle;

static void start_adv(void);

static uint8_t hid_key_for_char(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x04 + (c - 'A'));
    if (c == ' ') return 0x2C;
    if (c == '\n') return 0x28;
    return 0;
}

static int map_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map))
               ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t report[8] = {0};
        return os_mbuf_append(ctxt->om, report, sizeof(report)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_hid_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4B),
                .access_cb = map_access,
                .val_handle = &s_hid_map_handle,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D),
                .access_cb = report_access,
                .val_handle = &s_hid_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static void load_payload(void) {
    strncpy(s_payload, "hello", sizeof(s_payload) - 1);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_payload);
        nvs_get_str(h, NVS_KEY, s_payload, &len);
        nvs_close(h);
    }
}

static void send_key(uint16_t conn, uint8_t key, bool down) {
    uint8_t report[8] = {0};
    if (down && key) report[2] = key;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om) ble_gatts_notify_custom(conn, s_hid_report_handle, om);
}

static void type_payload(uint16_t conn) {
    for (const char *p = s_payload; *p; p++) {
        uint8_t k = hid_key_for_char(*p);
        if (!k) continue;
        send_key(conn, k, true);
        vTaskDelay(pdMS_TO_TICKS(30));
        send_key(conn, k, false);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void type_task(void *arg) {
    type_payload((uint16_t)(uintptr_t)arg);
    vTaskDelete(NULL);
}

static int gap_event_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            s_connected = (ev->connect.status == 0);
            s_conn = s_connected ? ev->connect.conn_handle : 0;
            if (s_connected)
                xTaskCreate(type_task, "hid_type", 2048, (void *)(uintptr_t)s_conn, 5, NULL);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_conn = 0;
            if (s_active) start_adv();
            break;
        default: break;
    }
    return 0;
}

static void start_adv(void) {
    if (!ble_core_is_ready()) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)"WiFiend-KB";
    f.name_len = strlen("WiFiend-KB");
    f.name_is_complete = 1;
    f.appearance = 961;
    f.appearance_is_present = 1;
    f.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0x1812) };
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;
    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(ble_core_own_addr_type(), NULL, BLE_HS_FOREVER, &ap,
                          gap_event_cb, NULL) == 0)
        s_advertising = true;
}

static void register_svcs_once(void) {
    if (s_svcs_registered) return;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_hid_svcs);
    ble_gatts_add_svcs(s_hid_svcs);
    s_svcs_registered = true;
}

static void render(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("BadBLE HID", s_connected ? "LINK" : "ADV");
    ssd1306_draw_string(0, 3, "Payload:");
    char line[20];
    snprintf(line, sizeof(line), " %-16.16s", s_payload);
    ssd1306_draw_string(0, 4, line);
    ssd1306_draw_string(0, 6, s_connected ? " typing..." : " wait connect");
    ssd1306_draw_string(0, 7, "Long>back");
    ssd1306_flush();
}

void ble_hid_enter(void) {
    load_payload();
    s_active = true;
    s_connected = false;
    s_conn = 0;
    register_svcs_once();
    radio_mgr_enter(RADIO_MODE_BLE_ACTIVE);
    start_adv();
}

void ble_hid_exit(void) {
    s_active = false;
    ble_gap_adv_stop();
    if (s_connected) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_advertising = false;
    radio_mgr_leave(RADIO_MODE_BLE_ACTIVE);
}

void ble_hid_tick(void) {
    if (!s_active) return;
    if (!s_advertising && !s_connected) start_adv();
    render();
}

void ble_hid_input(encoder_event_t e) {
    (void)e;
}
