#include <stdio.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ssd1306.h"
#include "encoder.h"
#include "neopixel.h"
#include "battery.h"
#include "menu.h"
#include "wifi_scan.h"
#include "wifi_ap.h"
#include "wifi_sta.h"
#include "wifi_attack.h"
#include "wifi_sniffer.h"
#include "boot_bitmap.h"
#include "esp_mac.h"

static const char *TAG = "main";

static volatile bool scanner_active  = false;
static volatile bool sniffer_active  = false;
static volatile bool attack_active   = false;

// Forward declarations for mutual callback references
static void encoder_event_handler(encoder_event_t event);
static void scanner_encoder_handler(encoder_event_t event);
static void sniffer_encoder_handler(encoder_event_t event);
static void sniffer_detail_encoder_handler(encoder_event_t event);
static void attack_encoder_handler(encoder_event_t event);

static void detail_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        scanner_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        encoder_set_callback(scanner_encoder_handler);
        wifi_scan_render();
    }
}

static void scanner_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_scan_scroll_down();
            wifi_scan_render();
            break;
        case ENCODER_CCW:
            wifi_scan_scroll_up();
            wifi_scan_render();
            break;
        case ENCODER_CLICK:
            encoder_set_callback(detail_encoder_handler);
            wifi_scan_render_detail();
            break;
        case ENCODER_LONG_PRESS:
            scanner_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void sniffer_detail_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        wifi_sniff_stop();
        sniffer_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        encoder_set_callback(sniffer_encoder_handler);
        wifi_sniff_render();
    }
}

static void sniffer_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_sniff_scroll_down();
            wifi_sniff_render();
            break;
        case ENCODER_CCW:
            wifi_sniff_scroll_up();
            wifi_sniff_render();
            break;
        case ENCODER_CLICK:
            encoder_set_callback(sniffer_detail_encoder_handler);
            wifi_sniff_render_detail();
            break;
        case ENCODER_LONG_PRESS:
            wifi_sniff_stop();
            sniffer_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_wifi_sniffer(void) {
    neopixel_set_color(COLOR_MAGENTA);
    sniffer_active = true;
    encoder_set_callback(sniffer_encoder_handler);
    wifi_sniff_start();
    wifi_sniff_render();
}

static void menu_wifi_scanner(void) {
    neopixel_set_color(COLOR_YELLOW);
    // Set active before scan so menu_render() doesn't overwrite the display
    scanner_active = true;
    encoder_set_callback(scanner_encoder_handler);
    wifi_scan_start();  // non-blocking internally — animates while scanning
    neopixel_set_color(COLOR_CYAN);
    wifi_scan_render();  // shows results or "No APs found" + "Long>menu"
}

static void menu_ap_mode(void) {
    ESP_LOGI(TAG, "AP Mode selected");
    neopixel_set_color(COLOR_CYAN);
    ssd1306_clear_buffer();
    ssd1306_draw_header("AP Mode", "Coming soon...");
    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(3000));
    neopixel_set_color(COLOR_GREEN);
}

static void attack_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_attack_scroll_down();
            wifi_attack_render();
            break;
        case ENCODER_CCW:
            wifi_attack_scroll_up();
            wifi_attack_render();
            break;
        case ENCODER_CLICK:
            if (!wifi_attack_is_running()) {
                wifi_attack_select();   // confirm AP and start attack
                neopixel_set_color(COLOR_RED);
            }
            wifi_attack_render();
            break;
        case ENCODER_LONG_PRESS:
            wifi_attack_stop();
            attack_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_deauth_attack(void) {
    attack_active = true;
    encoder_set_callback(attack_encoder_handler);
    wifi_attack_enter();    // load AP list from last scan
    wifi_attack_render();   // show picker (or "run scanner first" if empty)
}

static void menu_sta_connect(void) {
    ESP_LOGI(TAG, "STA Connect selected");
    neopixel_set_color(COLOR_MAGENTA);
    ssd1306_clear_buffer();
    ssd1306_draw_header("STA Connect", "Coming soon...");
    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(3000));
    neopixel_set_color(COLOR_GREEN);
}

static void menu_device_info(void) {
    ESP_LOGI(TAG, "Device Info selected");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X", mac[2], mac[3], mac[4], mac[5]);

    ssd1306_clear_buffer();
    ssd1306_draw_header("Device Info", mac_str);

    char heap_str[20];
    snprintf(heap_str, sizeof(heap_str), "Heap: %lu KB", esp_get_free_heap_size() / 1024);
    ssd1306_draw_string(0, 2, heap_str);

    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(5000));
}

static menu_item_t main_menu[] = {
    {.label = "WiFi Scanner",  .on_select = menu_wifi_scanner},
    {.label = "Client Sniff",  .on_select = menu_wifi_sniffer},
    {.label = "AP Mode",       .on_select = menu_ap_mode},
    {.label = "Deauth Attack", .on_select = menu_deauth_attack},
    {.label = "STA Connect",   .on_select = menu_sta_connect},
    {.label = "Device Info",   .on_select = menu_device_info},
};

static void encoder_event_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            menu_navigate_down();
            menu_render();
            break;
        case ENCODER_CCW:
            menu_navigate_up();
            menu_render();
            break;
        case ENCODER_CLICK:
            menu_select_current();
            if (!scanner_active && !attack_active) menu_render();
            break;
        case ENCODER_LONG_PRESS:
            menu_pop();
            menu_render();
            break;
    }
}

static void display_boot_splash(void) {
    ssd1306_draw_bitmap_fullscreen(boot_bitmap);
    vTaskDelay(pdMS_TO_TICKS(2000));
}

void app_main(void) {
    ESP_LOGI(TAG, "WiFiend v1.0 - ESP32-C5 WiFi Hacking Handheld");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ssd1306_init();
    neopixel_init();
    battery_init();
    encoder_init();

    display_boot_splash();

    neopixel_set_color(COLOR_GREEN);

    wifi_scan_init();
    wifi_sniff_init();
    wifi_ap_init();
    wifi_sta_init();
    wifi_attack_init();

    menu_init(main_menu, sizeof(main_menu) / sizeof(main_menu[0]));
    encoder_set_callback(encoder_event_handler);

    ESP_LOGI(TAG, "Boot complete");

    while (1) {
        if (!scanner_active && !sniffer_active && !attack_active) menu_render();
        // Refresh attack stats display while running
        if (attack_active && wifi_attack_is_running()) wifi_attack_render();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
