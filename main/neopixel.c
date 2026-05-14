#include "neopixel.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "neopixel";

#define NEOPIXEL_GPIO GPIO_NUM_8
#define NEOPIXEL_COUNT 1

static led_strip_handle_t led_strip = NULL;
static uint8_t current_brightness = 10;

void neopixel_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds = NEOPIXEL_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

    led_strip_clear(led_strip);

    ESP_LOGI(TAG, "NeoPixel initialized on GPIO%d (RMT)", NEOPIXEL_GPIO);
}

void neopixel_set_color(pixel_color_t color) {
    if (!led_strip) return;

    uint8_t r = (color.red * current_brightness) / 255;
    uint8_t g = (color.green * current_brightness) / 255;
    uint8_t b = (color.blue * current_brightness) / 255;

    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

void neopixel_set_brightness(uint8_t brightness) {
    current_brightness = brightness;
}
