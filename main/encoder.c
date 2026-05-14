#include "encoder.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "encoder";

#define GPIO_CLK            GPIO_NUM_9
#define GPIO_DT             GPIO_NUM_10
#define GPIO_SW             GPIO_NUM_7
#define LONG_PRESS_MS       500
#define DEBOUNCE_MS         20
#define PCNT_HIGH_LIMIT     100
#define PCNT_LOW_LIMIT     -100
#define GLITCH_FILTER_NS    10000
#define POLL_INTERVAL_MS    10

static encoder_callback_t user_cb = NULL;
static pcnt_unit_handle_t pcnt_unit = NULL;

static void encoder_task(void *arg) {
    int count = 0;
    bool sw_prev = true;
    TickType_t sw_press_tick = 0;
    TickType_t sw_last_change = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        pcnt_unit_get_count(pcnt_unit, &count);
        if (count != 0) {
            encoder_event_t evt = (count > 0) ? ENCODER_CW : ENCODER_CCW;
            pcnt_unit_clear_count(pcnt_unit);
            if (user_cb) user_cb(evt);
        }

        bool sw_now = gpio_get_level(GPIO_SW);
        if (sw_now != sw_prev && (now - sw_last_change) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
            sw_last_change = now;
            sw_prev = sw_now;
            if (!sw_now) {
                sw_press_tick = now;
            } else {
                uint32_t hold_ms = (now - sw_press_tick) * portTICK_PERIOD_MS;
                encoder_event_t evt = (hold_ms >= LONG_PRESS_MS) ? ENCODER_LONG_PRESS : ENCODER_CLICK;
                if (user_cb) user_cb(evt);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void encoder_init(void) {
    // --- PCNT unit ---
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit  = PCNT_LOW_LIMIT,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &pcnt_unit));

    // Hardware glitch filter — supplements RC filter on CLK/DT lines
    pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = GLITCH_FILTER_NS,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_cfg));

    // Single channel: CLK as edge signal, DT as level signal.
    // Quadrature rule: on CLK falling edge, DT=high → CW (+1), DT=low → CCW (-1).
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = GPIO_CLK,
        .level_gpio_num = GPIO_DT,
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_cfg, &chan));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan,
        PCNT_CHANNEL_EDGE_ACTION_HOLD,      // CLK rising  → ignore
        PCNT_CHANNEL_EDGE_ACTION_INCREASE)); // CLK falling → increment

    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,     // DT=1 → keep increment (CW)
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE)); // DT=0 → invert to decrement (CCW)

    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));

    // --- SW GPIO — polled, no ISR ---
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << GPIO_SW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_cfg));

    xTaskCreate(encoder_task, "encoder", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Encoder init: CLK=GPIO%d DT=GPIO%d SW=GPIO%d",
             GPIO_CLK, GPIO_DT, GPIO_SW);
}

void encoder_set_callback(encoder_callback_t cb) {
    user_cb = cb;
}
