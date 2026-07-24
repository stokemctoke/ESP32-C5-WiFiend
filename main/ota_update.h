#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*ota_progress_cb_t)(uint8_t percent, void *ctx);

// Validate the first chunk contains a plausible ESP image header.
bool ota_update_validate_header(const uint8_t *data, size_t len);

// Begin writing to the next OTA slot. image_size may be 0 if unknown.
esp_err_t ota_update_begin(size_t image_size);

// Stream firmware bytes. Call only after a successful begin().
esp_err_t ota_update_write(const void *data, size_t len);

// Finalize, optionally mark the slot bootable. Does not reboot.
esp_err_t ota_update_end(bool set_boot);

// Abort an in-progress update and discard partial data.
void ota_update_abort(void);

// Progress helpers (0–100, or 0 if total size unknown).
uint8_t ota_update_get_percent(void);
bool    ota_update_is_active(void);

// Optional callback invoked after each successful write when total size is known.
void ota_update_set_progress_cb(ota_progress_cb_t cb, void *ctx);

#endif
