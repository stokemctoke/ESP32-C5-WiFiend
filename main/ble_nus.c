#include "ble_nus.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "battery.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "esp_system.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ble_nus";

static const ble_uuid128_t nus_svc_uuid  = BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E);
static const ble_uuid128_t nus_rx_uuid   = BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E);
static const ble_uuid128_t nus_tx_uuid   = BLE_UUID128_INIT(0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E);

static volatile bool s_active;
static volatile bool s_advertising;
static uint16_t      s_conn;
static bool          s_connected;
static bool          s_svcs_registered;
static uint16_t      s_tx_handle;
static char          s_status[20];

static void nus_tx(const char *msg) {
    if (!s_connected || !msg) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
    if (om) ble_gatts_notify_custom(s_conn, s_tx_handle, om);
}

static void cmd_dump(void) {
    FILE *fp = fopen("/lfs/handshakes.log", "r");
    if (!fp) {
        nus_tx("NOLOG\n");
        return;
    }
    char line[64];
    while (fgets(line, sizeof(line), fp)) nus_tx(line);
    fclose(fp);
    nus_tx("END\n");
}

static void cmd_stat(void) {
    char buf[64];
    uint16_t mv = battery_read_mv();
    uint8_t pct = battery_get_percentage();
    snprintf(buf, sizeof(buf), "heap=%lu bat=%umV %u%%\n",
             (unsigned long)esp_get_free_heap_size(), (unsigned)mv, (unsigned)pct);
    nus_tx(buf);
}

static void handle_cmd(const char *cmd) {
    if (strncmp(cmd, "DUMP", 4) == 0) cmd_dump();
    else if (strncmp(cmd, "STAT", 4) == 0) cmd_stat();
    else nus_tx("ERR\n");
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char buf[32];
    uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len >= sizeof(buf)) om_len = sizeof(buf) - 1;
    os_mbuf_copydata(ctxt->om, 0, om_len, buf);
    buf[om_len] = '\0';
    handle_cmd(buf);
    snprintf(s_status, sizeof(s_status), "cmd:%.8s", buf);
    return 0;
}

static const struct ble_gatt_svc_def s_nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = NULL,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static void start_adv(void);

static int gap_event_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            s_connected = (ev->connect.status == 0);
            s_conn = s_connected ? ev->connect.conn_handle : 0;
            snprintf(s_status, sizeof(s_status), s_connected ? "linked" : "fail");
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_conn = 0;
            snprintf(s_status, sizeof(s_status), "adv");
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
    f.name = (uint8_t *)"WiFiend-NUS";
    f.name_len = strlen("WiFiend-NUS");
    f.name_is_complete = 1;
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
    ble_gatts_count_cfg(s_nus_svcs);
    ble_gatts_add_svcs(s_nus_svcs);
    s_svcs_registered = true;
}

static void render(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("NUS Link", s_status);
    ssd1306_draw_string(0, 3, "DUMP / STAT");
    ssd1306_draw_string(0, 4, "over phone UART");
    ssd1306_draw_string(0, 6, s_connected ? " connected" : " advertising");
    ssd1306_draw_string(0, 7, "Long>back");
    ssd1306_flush();
}

void ble_nus_enter(void) {
    strncpy(s_status, "adv", sizeof(s_status));
    s_active = true;
    s_connected = false;
    register_svcs_once();
    radio_mgr_enter(RADIO_MODE_BLE_ACTIVE);
    start_adv();
}

void ble_nus_exit(void) {
    s_active = false;
    ble_gap_adv_stop();
    if (s_connected) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_advertising = false;
    radio_mgr_leave(RADIO_MODE_BLE_ACTIVE);
}

void ble_nus_tick(void) {
    if (!s_active) return;
    if (!s_advertising && !s_connected) start_adv();
    render();
}

void ble_nus_input(encoder_event_t e) {
    (void)e;
}
