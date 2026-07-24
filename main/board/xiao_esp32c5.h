#ifndef XIAO_ESP32C5_H
#define XIAO_ESP32C5_H

#include "driver/gpio.h"

// Seeed Studio XIAO ESP32-C5 — WiFiend Mini perfboard pin map.
// https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/

#define BOARD_NAME "XIAO ESP32-C5"

// EC11 rotary encoder
#define PIN_ENCODER_CLK   GPIO_NUM_9   // D9
#define PIN_ENCODER_DT    GPIO_NUM_10  // D10
#define PIN_ENCODER_SW    GPIO_NUM_7   // D3

// WS2812B status LED
#define PIN_NEOPIXEL      GPIO_NUM_8   // D8

// SSD1306 OLED (I2C)
#define PIN_OLED_SDA      GPIO_NUM_23  // D4
#define PIN_OLED_SCL      GPIO_NUM_24  // D5
#define PIN_OLED_I2C_HZ   400000

// Onboard LiPo sense — SGM40567 + /2 divider (Seeed / WiFuxx)
#define PIN_BATTERY_ADC      GPIO_NUM_6   // BAT_VOLT
#define PIN_BATTERY_ADC_EN   GPIO_NUM_26  // BAT_VOLT_EN — HIGH enables divider

// Onboard yellow user LED (optional; not wired on perfboard)
#define PIN_USER_LED      GPIO_NUM_27

// XIAO BOOT button (active LOW). Same as WiFuxx — NOT the encoder SW pin.
#define PIN_BOOT_BUTTON   GPIO_NUM_28

#endif
