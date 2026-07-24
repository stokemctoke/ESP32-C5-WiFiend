#include "boot_mode.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_mode";

RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_boot_dest;

uint32_t boot_mode_consume(void) {
    uint32_t dest = BOOT_DEST_MENU;
    if (s_boot_magic == BOOT_MAGIC) {
        dest = s_boot_dest;
        ESP_LOGI(TAG, "boot dest=%lu", (unsigned long)dest);
    }
    s_boot_magic = 0;
    s_boot_dest  = 0;
    return dest;
}

void boot_mode_reboot(uint32_t dest) {
    ESP_LOGW(TAG, "reboot into dest=%lu", (unsigned long)dest);
    s_boot_magic = BOOT_MAGIC;
    s_boot_dest  = dest;
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}
