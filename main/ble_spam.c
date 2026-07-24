#include "ble_spam.h"
#include "ble_core.h"
#include "radio_mgr.h"
#include "ssd1306.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_spam";

typedef enum {
    SPAM_APPLE = 0,
    SPAM_FASTPAIR,
    SPAM_IBEACON,
    SPAM_EDDYSTONE,
    SPAM_COUNT,
} spam_mode_t;

static const char *s_mode_names[] = {
    "Apple Near",
    "Fast Pair",
    "iBeacon",
    "Eddystone",
};

static volatile bool s_active;
static volatile bool s_running;
static int           s_sel;
static spam_mode_t   s_mode;
static uint32_t      s_tx_count;
static uint8_t       s_phase;

static int build_apple_adv(struct ble_hs_adv_fields *f, uint8_t *buf, size_t bufsz) {
    // Apple Continuity Nearby Info (0x10) — rotating filler bytes.
    uint8_t payload[] = {
        0x4C, 0x00, 0x10, 0x05, 0x01, 0x00,
        s_phase, (uint8_t)(s_phase ^ 0xA5), 0x00, 0x00, 0x10, 0x00, 0x00, 0x00
    };
    if (sizeof(payload) > bufsz) return BLE_HS_EMSGSIZE;
    memcpy(buf, payload, sizeof(payload));
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f->mfg_data = buf;
    f->mfg_data_len = sizeof(payload);
    return 0;
}

static int build_fastpair_adv(struct ble_hs_adv_fields *f, uint8_t *buf, size_t bufsz) {
    uint8_t payload[] = { 0x2C, 0xFE, 0x00, s_phase, 0x42, 0x01 };
    if (sizeof(payload) > bufsz) return BLE_HS_EMSGSIZE;
    memcpy(buf, payload, sizeof(payload));
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f->uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xFE2C) };
    f->num_uuids16 = 1;
    f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = buf;
    f->svc_data_uuid16_len = sizeof(payload);
    return 0;
}

static int build_ibeacon_adv(struct ble_hs_adv_fields *f, uint8_t *buf, size_t bufsz) {
    uint8_t payload[25];
    payload[0] = 0x4C; payload[1] = 0x00; payload[2] = 0x02; payload[3] = 0x15;
    memset(&payload[4], 0xAA, 16);
    payload[20] = 0x00; payload[21] = 0x01;  // major
    payload[22] = s_phase; payload[23] = 0x00; // minor
    payload[24] = 0xC5; // measured power
    if (sizeof(payload) > bufsz) return BLE_HS_EMSGSIZE;
    memcpy(buf, payload, sizeof(payload));
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f->mfg_data = buf;
    f->mfg_data_len = sizeof(payload);
    return 0;
}

static int build_eddystone_adv(struct ble_hs_adv_fields *f, uint8_t *buf, size_t bufsz) {
    uint8_t payload[] = { 0xAA, 0xFE, 0x10, 0xF0, 0x03, 'e', 's', 'p', 0x00, '3', '2', 0x07 };
    if (sizeof(payload) > bufsz) return BLE_HS_EMSGSIZE;
    memcpy(buf, payload, sizeof(payload));
    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f->uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(0xFEAA) };
    f->num_uuids16 = 1;
    f->uuids16_is_complete = 1;
    f->svc_data_uuid16 = buf;
    f->svc_data_uuid16_len = sizeof(payload);
    return 0;
}

static int apply_adv(spam_mode_t mode) {
    struct ble_hs_adv_fields fields;
    uint8_t buf[31];
    int rc = 0;
    switch (mode) {
        case SPAM_APPLE:     rc = build_apple_adv(&fields, buf, sizeof(buf)); break;
        case SPAM_FASTPAIR:  rc = build_fastpair_adv(&fields, buf, sizeof(buf)); break;
        case SPAM_IBEACON:   rc = build_ibeacon_adv(&fields, buf, sizeof(buf)); break;
        case SPAM_EDDYSTONE: rc = build_eddystone_adv(&fields, buf, sizeof(buf)); break;
        default: return BLE_HS_EINVAL;
    }
    if (rc != 0) return rc;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_NON;
    ap.disc_mode = BLE_GAP_DISC_MODE_NON;
    return ble_gap_adv_start(ble_core_own_addr_type(), NULL, BLE_HS_FOREVER, &ap, NULL, NULL);
}

static void stop_adv(void) {
    ble_gap_adv_stop();
}

static void render_picker(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("BLE Spam", s_running ? "RUN" : "pick");
    for (int i = 0; i < SPAM_COUNT && i < 6; i++) {
        char line[20];
        snprintf(line, sizeof(line), "%c %s", (i == s_sel) ? '>' : ' ', s_mode_names[i]);
        ssd1306_draw_string(0, 2 + i, line);
    }
    ssd1306_draw_string(0, 7, s_running ? "Click=stop" : "Click=start");
    ssd1306_flush();
}

static void render_running(void) {
    char status[24];
    snprintf(status, sizeof(status), "%lu tx", (unsigned long)s_tx_count);
    ssd1306_clear_buffer();
    ssd1306_draw_header(s_mode_names[s_mode], status);
    ssd1306_draw_string(0, 3, "  TX active");
    ssd1306_draw_string(0, 4, "  rotating adv");
    ssd1306_draw_string(0, 7, "Click=stop");
    ssd1306_flush();
}

void ble_spam_enter(void) {
    s_active = true;
    s_running = false;
    s_sel = 0;
    s_mode = SPAM_APPLE;
    s_tx_count = 0;
    s_phase = 0;
    radio_mgr_enter(RADIO_MODE_BLE_ADV_TX);
}

void ble_spam_exit(void) {
    s_active = false;
    stop_adv();
    radio_mgr_leave(RADIO_MODE_BLE_ADV_TX);
}

void ble_spam_tick(void) {
    if (!s_active) return;
    if (s_running) {
        if (!ble_core_is_ready()) {
            render_running();
            return;
        }
        stop_adv();
        s_phase++;
        if (apply_adv(s_mode) == 0) s_tx_count++;
        render_running();
    } else {
        render_picker();
    }
}

void ble_spam_input(encoder_event_t e) {
    if (!s_active) return;
    if (s_running) {
        if (e == ENCODER_CLICK) {
            stop_adv();
            s_running = false;
        }
        return;
    }
    switch (e) {
        case ENCODER_CW:
            if (s_sel + 1 < SPAM_COUNT) s_sel++;
            break;
        case ENCODER_CCW:
            if (s_sel > 0) s_sel--;
            break;
        case ENCODER_CLICK:
            s_mode = (spam_mode_t)s_sel;
            if (ble_core_is_ready() && apply_adv(s_mode) == 0) {
                s_running = true;
                s_tx_count = 1;
            } else {
                ESP_LOGW(TAG, "adv start failed / core not ready");
            }
            break;
        default: break;
    }
}
