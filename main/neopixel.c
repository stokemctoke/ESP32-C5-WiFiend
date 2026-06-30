#include "neopixel.h"
#include "board/xiao_esp32c5.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "neopixel";

#define NEOPIXEL_GPIO  PIN_NEOPIXEL
#define NEOPIXEL_COUNT 1

// Pulse parameters
#define PULSE_TIMER_US (25 * 1000)   // 40 Hz refresh
#define PULSE_PHASES   120           // ~3.0 s breathing cycle at 40 Hz
#define PULSE_PEAK     45            // brighter than ambient (15) so we get many distinct levels

static led_strip_handle_t led_strip = NULL;
static uint8_t            current_brightness = 15;

// ---- pulse state ----
static esp_timer_handle_t s_pulse_timer = NULL;
static pixel_color_t      s_pulse_base;
static volatile bool      s_pulse_active = false;
static uint16_t           s_pulse_phase  = 0;

static void write_raw(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_strip) return;
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

// Cubic gamma — pushes more steps to the low end so dim transitions look smooth.
static uint8_t gamma_apply(uint8_t v) {
    return (uint8_t)((uint32_t)v * v * v / (255u * 255u));
}

static void pulse_cb(void *arg) {
    if (!s_pulse_active) return;
    s_pulse_phase = (uint16_t)((s_pulse_phase + 1) % PULSE_PHASES);

    // Triangle wave 0..255..0 across PULSE_PHASES.
    uint16_t half = PULSE_PHASES / 2;
    uint16_t lin  = (s_pulse_phase < half)
                    ? (s_pulse_phase * 255 / half)
                    : ((PULSE_PHASES - s_pulse_phase) * 255 / half);
    uint8_t  g    = gamma_apply((uint8_t)lin);

    // Map gamma 0..255 to brightness 0..PULSE_PEAK; pulse bypasses
    // current_brightness so it has its own (wider) dynamic range.
    uint8_t  amp  = (uint8_t)((g * PULSE_PEAK) / 255);

    write_raw((s_pulse_base.red   * amp) / 255,
              (s_pulse_base.green * amp) / 255,
              (s_pulse_base.blue  * amp) / 255);
}

static void ensure_pulse_timer(void) {
    if (s_pulse_timer) return;
    const esp_timer_create_args_t args = {
        .callback        = pulse_cb,
        .name            = "neopixel_pulse",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&args, &s_pulse_timer);
}

static bool same_color(const pixel_color_t *a, const pixel_color_t *b) {
    return a->red == b->red && a->green == b->green && a->blue == b->blue;
}

// ---- public API ----

void neopixel_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds       = NEOPIXEL_COUNT,
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "NeoPixel initialized on GPIO%d (RMT)", NEOPIXEL_GPIO);
}

void neopixel_set_color(pixel_color_t color) {
    // Explicit colour overrides any active breathing.
    if (s_pulse_active) {
        s_pulse_active = false;
        if (s_pulse_timer) esp_timer_stop(s_pulse_timer);
    }
    write_raw((color.red   * current_brightness) / 255,
              (color.green * current_brightness) / 255,
              (color.blue  * current_brightness) / 255);
}

void neopixel_set_brightness(uint8_t brightness) {
    current_brightness = brightness;
}

// Idempotent: first call (or call with a new base colour) starts/restarts a
// background breathing animation driven by a dedicated 40 Hz timer. Subsequent
// calls with the same colour are cheap no-ops, so callers can invoke it from
// their render tick without resetting the phase.
void neopixel_pulse(pixel_color_t base) {
    ensure_pulse_timer();
    if (s_pulse_active && same_color(&s_pulse_base, &base)) return;
    s_pulse_base   = base;
    s_pulse_phase  = 0;
    s_pulse_active = true;
    esp_timer_stop(s_pulse_timer);
    esp_timer_start_periodic(s_pulse_timer, PULSE_TIMER_US);
}
