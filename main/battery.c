#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

// ESP32-C5 ADC1 covers GPIO0-6 only. GPIO4 = ADC1_CH4 is the battery sense pin.
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_4
#define BATTERY_ADC_UNIT    ADC_UNIT_1

#define LIPO_FULL_MV   4200
#define LIPO_EMPTY_MV  3000
#define LIPO_USABLE_MV (LIPO_FULL_MV - LIPO_EMPTY_MV)

// Assumes a 100k/100k voltage divider halving the battery voltage.
// Adjust if your divider ratio differs.
#define VOLTAGE_DIVIDER_RATIO 2.0f

static adc_oneshot_unit_handle_t adc_handle = NULL;

void battery_init(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &config));

    ESP_LOGI(TAG, "Battery ADC initialized: GPIO4 (ADC1_CH4)");
}

uint16_t battery_read_mv(void) {
    if (!adc_handle) return 0;

    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw));

    // 12-bit ADC (0-4095) against 3.3 V reference, scaled back up through divider
    uint32_t mv = (uint32_t)(((float)raw / 4095.0f) * 3300.0f * VOLTAGE_DIVIDER_RATIO);
    return (uint16_t)mv;
}

uint8_t battery_get_percentage(void) {
    uint16_t mv = battery_read_mv();

    if (mv < 2800 || mv > 4400) return BATTERY_INVALID;
    if (mv >= LIPO_FULL_MV)     return 100;
    if (mv <= LIPO_EMPTY_MV)    return 0;

    return (uint8_t)(((uint32_t)(mv - LIPO_EMPTY_MV) * 100) / LIPO_USABLE_MV);
}
