#include <stdio.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
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
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_chip_info.h"

static const char *TAG = "main";

static volatile bool scanner_active  = false;
static volatile bool sniffer_active  = false;
static volatile bool attack_active   = false;
static volatile bool ap_active       = false;
static volatile bool sta_active      = false;
static volatile bool chart_active    = false;

// Forward declarations for mutual callback references
static void encoder_event_handler(encoder_event_t event);
static void scanner_encoder_handler(encoder_event_t event);
static void sniffer_encoder_handler(encoder_event_t event);
static void sniffer_detail_encoder_handler(encoder_event_t event);
static void attack_encoder_handler(encoder_event_t event);
static void ap_encoder_handler(encoder_event_t event);
static void chart_encoder_handler(encoder_event_t event);

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

static void ap_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_ap_scroll_down();
            wifi_ap_render();
            break;
        case ENCODER_CCW:
            wifi_ap_scroll_up();
            wifi_ap_render();
            break;
        case ENCODER_CLICK:
            if (!wifi_ap_is_running()) {
                wifi_ap_select();       // clone SSID and start AP
                neopixel_set_color(COLOR_BLUE);
            }
            wifi_ap_render();
            break;
        case ENCODER_LONG_PRESS:
            wifi_ap_stop();
            ap_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_ap_mode(void) {
    ap_active = true;
    encoder_set_callback(ap_encoder_handler);
    wifi_ap_enter();
    wifi_ap_render();
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

static void sta_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            if (wifi_sta_is_in_picker())   wifi_sta_scroll_down();
            else if (wifi_sta_is_in_password()) wifi_sta_char_next();
            wifi_sta_render();
            break;
        case ENCODER_CCW:
            if (wifi_sta_is_in_picker())   wifi_sta_scroll_up();
            else if (wifi_sta_is_in_password()) wifi_sta_char_prev();
            wifi_sta_render();
            break;
        case ENCODER_CLICK:
            if (wifi_sta_is_in_picker())        wifi_sta_select();
            else if (wifi_sta_is_in_password()) wifi_sta_char_append();
            else                                wifi_sta_enter();   // retry from failed/connected
            wifi_sta_render();
            break;
        case ENCODER_LONG_PRESS:
            if (wifi_sta_is_in_password()) {
                wifi_sta_pw_cancel();
                wifi_sta_render();
            } else {
                wifi_sta_stop();
                sta_active = false;
                encoder_set_callback(encoder_event_handler);
                neopixel_set_color(COLOR_GREEN);
                menu_render();
            }
            break;
    }
}

static void menu_sta_connect(void) {
    sta_active = true;
    neopixel_set_color(COLOR_MAGENTA);
    encoder_set_callback(sta_encoder_handler);
    wifi_sta_enter();
    wifi_sta_render();
}

static void chart_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_scan_chart_next();
            wifi_scan_render_chart();
            break;
        case ENCODER_CCW:
            wifi_scan_chart_prev();
            wifi_scan_render_chart();
            break;
        case ENCODER_CLICK:
            wifi_scan_chart_toggle();
            wifi_scan_render_chart();
            break;
        case ENCODER_LONG_PRESS:
            chart_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_ch_chart(void) {
    chart_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(chart_encoder_handler);
    wifi_scan_render_chart();
}

static void menu_device_info(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char hdr[10];
    snprintf(hdr, sizeof(hdr), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    ssd1306_clear_buffer();
    ssd1306_draw_header("Device Info", hdr);

    // Buffer larger than display width so snprintf never sees a truncation
    // risk; we hard-terminate at 16 chars before drawing.
    char line[32];

    snprintf(line, sizeof(line), "MAC:%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    line[16] = '\0';
    ssd1306_draw_string(0, 2, line);

    snprintf(line, sizeof(line), "Heap:%lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    line[16] = '\0';
    ssd1306_draw_string(0, 3, line);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(line, sizeof(line), "Flash:%lu MB",
             (unsigned long)(flash_size / (1024 * 1024)));
    line[16] = '\0';
    ssd1306_draw_string(0, 4, line);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(line, sizeof(line), "Rev:%d IDF:%s",
             chip.revision, esp_get_idf_version());
    line[16] = '\0';
    ssd1306_draw_string(0, 5, line);

    int64_t up_s = esp_timer_get_time() / 1000000LL;
    if (up_s < 0) up_s = 0;
    int up_d  = (int)(up_s / 86400);
    int up_h  = (int)((up_s / 3600) % 24);
    int up_m  = (int)((up_s / 60) % 60);
    int up_sc = (int)(up_s % 60);
    if (up_d > 0)
        snprintf(line, sizeof(line), "Up:%dd %02dh%02dm", up_d, up_h, up_m);
    else
        snprintf(line, sizeof(line), "Up:%02dh %02dm %02ds", up_h, up_m, up_sc);
    line[16] = '\0';
    ssd1306_draw_string(0, 6, line);

    wifi_mode_t wmode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wmode);
    const char *ms = (wmode == WIFI_MODE_STA)    ? "STA"    :
                     (wmode == WIFI_MODE_AP)     ? "AP"     :
                     (wmode == WIFI_MODE_APSTA)  ? "AP+STA" : "None";
    snprintf(line, sizeof(line), "WiFi: %s", ms);
    line[16] = '\0';
    ssd1306_draw_string(0, 7, line);

    ssd1306_flush();
    vTaskDelay(pdMS_TO_TICKS(5000));
}

static menu_item_t main_menu[] = {
    {.label = "WiFi Scan",     .on_select = menu_wifi_scanner},
    {.label = "Client Sniff",  .on_select = menu_wifi_sniffer},
    {.label = "AP Mode",       .on_select = menu_ap_mode},
    {.label = "Deauth Attack", .on_select = menu_deauth_attack},
    {.label = "STA Connect",   .on_select = menu_sta_connect},
    {.label = "Ch Chart",      .on_select = menu_ch_chart},
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
            if (!scanner_active && !attack_active && !ap_active && !sta_active && !chart_active) menu_render();
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

    // Default netifs MUST be created before esp_wifi_init() so the default
    // event handlers (which start DHCP on AP_START) are registered early
    // enough to handle the first WIFI_EVENT_AP_START.
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

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
        if (!scanner_active && !sniffer_active && !attack_active && !ap_active && !sta_active && !chart_active) menu_render();
        if (attack_active && wifi_attack_is_running()) wifi_attack_render();
        if (ap_active && wifi_ap_is_running()) wifi_ap_render();
        if (sta_active && (wifi_sta_is_connecting() || wifi_sta_needs_refresh())) wifi_sta_render();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
