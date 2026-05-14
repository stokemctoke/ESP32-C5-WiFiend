#include "buttons.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

#define GPIO_UP     GPIO_NUM_3
#define GPIO_DOWN   GPIO_NUM_6
#define GPIO_CENTER GPIO_NUM_7

static const gpio_num_t gpio_map[BUTTON_MAX] = {
    [BUTTON_UP]     = GPIO_UP,
    [BUTTON_DOWN]   = GPIO_DOWN,
    [BUTTON_CENTER] = GPIO_CENTER,
};

static button_callback_t button_cb = NULL;
static bool button_state[BUTTON_MAX] = {false};

static void button_handler_task(void *arg) {
    static bool last_state[BUTTON_MAX] = {false};
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));

        for (button_id_t btn = 0; btn < BUTTON_MAX; btn++) {
            bool raw = !gpio_get_level(gpio_map[btn]);

            if (raw == last_state[btn]) continue;

            vTaskDelay(pdMS_TO_TICKS(50));

            bool confirmed = !gpio_get_level(gpio_map[btn]);
            if (confirmed != last_state[btn]) {
                last_state[btn] = confirmed;
                button_state[btn] = confirmed;
                if (button_cb) {
                    button_cb(btn, confirmed ? BUTTON_PRESS : BUTTON_RELEASE);
                }
            }
        }
    }
}

void buttons_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_UP) | (1ULL << GPIO_DOWN) | (1ULL << GPIO_CENTER),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    xTaskCreate(button_handler_task, "buttons", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Buttons: UP=GPIO%d DOWN=GPIO%d CENTER=GPIO%d",
             GPIO_UP, GPIO_DOWN, GPIO_CENTER);
}

void buttons_set_callback(button_callback_t cb) {
    button_cb = cb;
}

bool buttons_is_pressed(button_id_t btn) {
    if (btn >= BUTTON_MAX) return false;
    return button_state[btn];
}
