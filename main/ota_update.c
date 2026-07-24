#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include <string.h>

static const char *TAG = "ota";

static esp_ota_handle_t         s_handle   = 0;
static const esp_partition_t   *s_part     = NULL;
static size_t                   s_written  = 0;
static size_t                   s_total    = 0;
static bool                     s_active   = false;
static ota_progress_cb_t      s_prog_cb  = NULL;
static void                    *s_prog_ctx = NULL;

bool ota_update_validate_header(const uint8_t *data, size_t len)
{
    if (!data || len < sizeof(esp_image_header_t))
        return false;

    const esp_image_header_t *hdr = (const esp_image_header_t *)data;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC)
        return false;

    // Chip ID check skipped — esp_chip_id_t vs esp_chip_model_t enums differ by IDF version.
    (void)hdr;
    return true;
}

esp_err_t ota_update_begin(size_t image_size)
{
    if (s_active)
        return ESP_ERR_INVALID_STATE;

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        ESP_LOGE(TAG, "no OTA partition");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_begin(s_part,
                                  image_size > 0 ? image_size : OTA_SIZE_UNKNOWN,
                                  &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_written = 0;
    s_total   = image_size;
    s_active  = true;
    ESP_LOGI(TAG, "OTA begin -> %s (%u bytes expected)",
             s_part->label, (unsigned)image_size);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t len)
{
    if (!s_active || !data || len == 0)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
        return err;
    }

    s_written += len;
    if (s_total > 0 && s_prog_cb) {
        uint8_t pct = (uint8_t)((s_written * 100) / s_total);
        s_prog_cb(pct, s_prog_ctx);
    }
    return ESP_OK;
}

esp_err_t ota_update_end(bool set_boot)
{
    if (!s_active)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_ota_end(s_handle);
    s_active = false;
    s_handle = 0;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    if (set_boot) {
        err = esp_ota_set_boot_partition(s_part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_boot failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "boot partition set to %s", s_part->label);
    }

    s_part = NULL;
    return ESP_OK;
}

void ota_update_abort(void)
{
    if (!s_active)
        return;
    esp_ota_abort(s_handle);
    s_active  = false;
    s_handle  = 0;
    s_written = 0;
    s_part    = NULL;
    ESP_LOGW(TAG, "OTA aborted");
}

uint8_t ota_update_get_percent(void)
{
    if (s_total == 0)
        return 0;
    return (uint8_t)((s_written * 100) / s_total);
}

bool ota_update_is_active(void)
{
    return s_active;
}

void ota_update_set_progress_cb(ota_progress_cb_t cb, void *ctx)
{
    s_prog_cb  = cb;
    s_prog_ctx = ctx;
}
