#include "ble_core.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "ble_core";

static bool          s_inited        = false;
static volatile bool s_ready         = false;
static uint8_t       s_own_addr_type = 0;

static void on_reset(int reason) {
    ESP_LOGW(TAG, "controller reset; reason=%d", reason);
    s_ready = false;
}

static void on_sync(void) {
    // Make sure we have an identity address, then pick the best type to use.
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) ESP_LOGE(TAG, "ensure_addr rc=%d", rc);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }

    s_ready = true;
    ESP_LOGI(TAG, "BLE host synced (own_addr_type=%d)", s_own_addr_type);
}

static void host_task(void *param) {
    nimble_port_run();              // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_core_init(void) {
    if (s_inited) return;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", err);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    nimble_port_freertos_init(host_task);
    s_inited = true;
    ESP_LOGI(TAG, "NimBLE init started");
}

bool ble_core_is_ready(void) {
    return s_ready;
}

uint8_t ble_core_own_addr_type(void) {
    return s_own_addr_type;
}
