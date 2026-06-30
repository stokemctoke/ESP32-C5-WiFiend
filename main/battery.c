#include "battery.h"
#include "board/xiao_esp32c5.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

#define BATTERY_DIVIDER_RATIO  2
#define BATTERY_SAMPLES        16
#define BATTERY_EMA_DEN        8
#define BATTERY_PRESENT_MIN_MV 2500
#define BATTERY_UPDATE_MS      5000

static adc_oneshot_unit_handle_t batt_adc  = NULL;
static adc_cali_handle_t         batt_cali = NULL;
static adc_channel_t             batt_chan;
static int                       batt_mv    = 0;
static bool                      batt_valid = false;
static int64_t                   last_update_us = 0;

static const struct { uint16_t mv; uint8_t pct; } lipo_curve[] = {
    {3270, 0}, {3600, 5}, {3700, 12}, {3730, 20}, {3750, 28}, {3770, 34}, {3790, 40},
    {3820, 47}, {3850, 55}, {3870, 62}, {3910, 70}, {3950, 77}, {3990, 83}, {4060, 90},
    {4110, 94}, {4160, 98}, {4200, 100},
};

static int mv_to_percent(int mv) {
    const int n = (int)(sizeof(lipo_curve) / sizeof(lipo_curve[0]));
    if (mv <= (int)lipo_curve[0].mv)     return 0;
    if (mv >= (int)lipo_curve[n - 1].mv) return 100;
    for (int i = 1; i < n; i++) {
        if (mv < (int)lipo_curve[i].mv) {
            int lo_mv = (int)lipo_curve[i - 1].mv, hi_mv = (int)lipo_curve[i].mv;
            int lo_p  = lipo_curve[i - 1].pct,     hi_p  = lipo_curve[i].pct;
            return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
        }
    }
    return 100;
}

static void battery_sample(void) {
    if (!batt_adc) return;

    gpio_set_level(PIN_BATTERY_ADC_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    int acc = 0, n = 0, raw;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        if (adc_oneshot_read(batt_adc, batt_chan, &raw) == ESP_OK) {
            acc += raw;
            n++;
        }
    }

    gpio_set_level(PIN_BATTERY_ADC_EN, 0);
    if (n == 0) return;

    int raw_avg = acc / n, mv_node;
    if (batt_cali) {
        if (adc_cali_raw_to_voltage(batt_cali, raw_avg, &mv_node) != ESP_OK) return;
    } else {
        mv_node = raw_avg * 3300 / 4095;
    }

    int sample_mv = mv_node * BATTERY_DIVIDER_RATIO;
    batt_mv = batt_valid ? batt_mv + (sample_mv - batt_mv) / BATTERY_EMA_DEN : sample_mv;
    batt_valid = true;
    last_update_us = esp_timer_get_time();
}

void battery_init(void) {
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << PIN_BATTERY_ADC_EN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&en));
    gpio_set_level(PIN_BATTERY_ADC_EN, 0);

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_BATTERY_ADC, &unit, &batt_chan) != ESP_OK ||
        unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not ADC1", (int)PIN_BATTERY_ADC);
        return;
    }

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &batt_adc) != ESP_OK) {
        batt_adc = NULL;
        return;
    }

    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(batt_adc, batt_chan, &ccfg));

    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = ADC_UNIT_1,
        .chan     = batt_chan,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &batt_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable — readings approximate");
        batt_cali = NULL;
    }

    ESP_LOGI(TAG, "Battery sense GPIO%d (ADC1 ch%d), enable GPIO%d",
             (int)PIN_BATTERY_ADC, (int)batt_chan, (int)PIN_BATTERY_ADC_EN);

    battery_sample();
}

void battery_tick(void) {
    if (!batt_adc) return;
    int64_t now = esp_timer_get_time();
    if (last_update_us == 0 ||
        (now - last_update_us) >= (int64_t)BATTERY_UPDATE_MS * 1000) {
        battery_sample();
    }
}

uint16_t battery_read_mv(void) {
    battery_tick();
    if (!batt_valid || batt_mv < BATTERY_PRESENT_MIN_MV) return 0;
    return (uint16_t)batt_mv;
}

uint8_t battery_get_percentage(void) {
    battery_tick();
    if (!batt_valid || batt_mv < BATTERY_PRESENT_MIN_MV) return BATTERY_INVALID;
    int pct = mv_to_percent(batt_mv);
    if (pct < 0)   return 0;
    if (pct > 100) return 100;
    return (uint8_t)pct;
}
