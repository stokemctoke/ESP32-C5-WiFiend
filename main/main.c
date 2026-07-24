#include <stdio.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board/xiao_esp32c5.h"
#include "boot_mode.h"
#include "ota_github.h"

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
#include "esp_littlefs.h"
#include "wifi_pmkid.h"
#include "wifi_handshake.h"
#include "wifi_captures.h"
#include "wifi_webui.h"
#include "game_pong.h"
#include "game_life.h"
#include "game_react.h"
#include "ble_core.h"
#include "ble_scan.h"
#include "ble_class.h"
#include "ble_beacon.h"
#include "ble_hunt.h"
#include "ble_gatt.h"
#include "ble_notify.h"
#include "ble_spam.h"
#include "ble_hid.h"
#include "ble_advlog.h"
#include "ble_nus.h"
#include "radio_mgr.h"
#include "wifi_monitor.h"
#include "fs_browser.h"
#include "settings.h"
#include "espnow_recon.h"
#include "ieee154_sniff.h"
#include "wifi_profiles.h"
#include "cli.h"

static const char *TAG = "main";

// BOOT button (GPIO28): hold 2s → reboot into Remote WebUI (WiFuxx parity).
#define BOOT_HOLD_MS       2000
#define BOOT_POLL_MS       20

static volatile bool scanner_active  = false;
static volatile bool sniffer_active  = false;
static volatile bool monitor_active  = false;
static volatile bool attack_active   = false;
static volatile bool ap_active       = false;
static volatile bool sta_active      = false;
static volatile bool chart_active    = false;
static volatile bool info_active     = false;
static volatile bool pmkid_active    = false;
static volatile bool handshake_active = false;
static volatile bool captures_active  = false;
static volatile bool webui_active     = false;
static volatile bool game_active      = false;
static volatile bool life_active      = false;
static volatile bool react_active     = false;
static volatile bool ble_scan_active   = false;
static volatile bool ble_class_active  = false;
static volatile bool ble_beacon_active = false;
static volatile bool ble_hunt_active   = false;
static volatile bool ble_gatt_active   = false;
static volatile bool ble_notify_active = false;
static volatile bool ble_spam_active   = false;
static volatile bool ble_hid_active    = false;
static volatile bool ble_advlog_active = false;
static volatile bool ble_nus_active    = false;
static volatile bool fs_browser_active = false;
static volatile bool settings_ui_active = false;
static volatile bool espnow_active     = false;
static volatile bool ieee154_active    = false;

// Forward declarations for mutual callback references
static void encoder_event_handler(encoder_event_t event);
static void scanner_encoder_handler(encoder_event_t event);
static void sniffer_encoder_handler(encoder_event_t event);
static void sniffer_detail_encoder_handler(encoder_event_t event);
static void attack_encoder_handler(encoder_event_t event);
static void ap_encoder_handler(encoder_event_t event);
static void chart_encoder_handler(encoder_event_t event);
static void info_encoder_handler(encoder_event_t event);
static void info_render(void);
static void pmkid_encoder_handler(encoder_event_t event);
static void handshake_encoder_handler(encoder_event_t event);
static void captures_encoder_handler(encoder_event_t event);
static void webui_encoder_handler(encoder_event_t event);
static void game_encoder_handler(encoder_event_t event);
static void life_encoder_handler(encoder_event_t event);
static void react_encoder_handler(encoder_event_t event);
static void ble_scan_encoder_handler(encoder_event_t event);
static void ble_class_encoder_handler(encoder_event_t event);
static void ble_beacon_encoder_handler(encoder_event_t event);
static void ble_hunt_encoder_handler(encoder_event_t event);
static void ble_gatt_encoder_handler(encoder_event_t event);
static void ble_notify_encoder_handler(encoder_event_t event);
static void ble_spam_encoder_handler(encoder_event_t event);
static void ble_hid_encoder_handler(encoder_event_t event);
static void ble_advlog_encoder_handler(encoder_event_t event);
static void ble_nus_encoder_handler(encoder_event_t event);
static void monitor_encoder_handler(encoder_event_t event);
static void fs_browser_encoder_handler(encoder_event_t event);
static void settings_ui_encoder_handler(encoder_event_t event);
static void espnow_encoder_handler(encoder_event_t event);
static void ieee154_encoder_handler(encoder_event_t event);

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
        radio_mgr_leave(RADIO_MODE_WIFI_SNIFF);
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
            radio_mgr_leave(RADIO_MODE_WIFI_SNIFF);
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
    radio_mgr_enter(RADIO_MODE_WIFI_SNIFF);
    wifi_sniff_start();
    wifi_sniff_render();
}

static void monitor_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_monitor_scroll_down();
            wifi_monitor_render();
            break;
        case ENCODER_CCW:
            wifi_monitor_scroll_up();
            wifi_monitor_render();
            break;
        case ENCODER_CLICK:
            wifi_monitor_select();
            wifi_monitor_render();
            break;
        case ENCODER_LONG_PRESS:
            wifi_monitor_stop();
            radio_mgr_leave(RADIO_MODE_WIFI_MONITOR);
            monitor_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_wifi_monitor(void) {
    neopixel_set_color(COLOR_CYAN);
    monitor_active = true;
    encoder_set_callback(monitor_encoder_handler);
    wifi_monitor_enter();
    wifi_monitor_start();
    wifi_monitor_render();
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
                wifi_attack_cycle_profile();
            }
            wifi_attack_render();
            break;
        case ENCODER_LONG_PRESS:
            if (wifi_attack_is_running()) {
                wifi_attack_stop();
            } else {
                wifi_attack_start();
                neopixel_set_color(COLOR_RED);
            }
            if (!wifi_attack_is_running()) {
                attack_active = false;
                encoder_set_callback(encoder_event_handler);
                neopixel_set_color(COLOR_GREEN);
                menu_render();
                break;
            }
            wifi_attack_render();
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

static void pmkid_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            if (wifi_pmkid_is_in_picker()) wifi_pmkid_scroll_down();
            wifi_pmkid_render();
            break;
        case ENCODER_CCW:
            if (wifi_pmkid_is_in_picker()) wifi_pmkid_scroll_up();
            wifi_pmkid_render();
            break;
        case ENCODER_CLICK:
            if (wifi_pmkid_is_in_picker())   wifi_pmkid_select();
            else if (wifi_pmkid_is_captured()) wifi_pmkid_view_next();
            else                               wifi_pmkid_enter();  // retry from failed
            wifi_pmkid_render();
            break;
        case ENCODER_LONG_PRESS:
            wifi_pmkid_stop();
            pmkid_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_pmkid(void) {
    pmkid_active = true;
    neopixel_set_color(COLOR_YELLOW);
    encoder_set_callback(pmkid_encoder_handler);
    wifi_pmkid_enter();
    wifi_pmkid_render();
}

static void handshake_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            if (wifi_handshake_is_in_picker()) wifi_handshake_scroll_down();
            wifi_handshake_render();
            break;
        case ENCODER_CCW:
            if (wifi_handshake_is_in_picker()) wifi_handshake_scroll_up();
            wifi_handshake_render();
            break;
        case ENCODER_CLICK:
            if (wifi_handshake_is_captured())   wifi_handshake_view_next();
            else                                wifi_handshake_select();  // pick AP or fire deauth
            wifi_handshake_render();
            break;
        case ENCODER_LONG_PRESS:
            wifi_handshake_stop();
            handshake_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_handshake(void) {
    handshake_active = true;
    neopixel_set_color(COLOR_YELLOW);
    encoder_set_callback(handshake_encoder_handler);
    wifi_handshake_enter();
    wifi_handshake_render();
}

static void captures_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            wifi_captures_scroll_down();
            wifi_captures_render();
            break;
        case ENCODER_CCW:
            wifi_captures_scroll_up();
            wifi_captures_render();
            break;
        case ENCODER_CLICK:
            wifi_captures_select();
            wifi_captures_render();
            break;
        case ENCODER_LONG_PRESS:
            if (wifi_captures_back()) {
                wifi_captures_stop();
                captures_active = false;
                encoder_set_callback(encoder_event_handler);
                neopixel_set_color(COLOR_GREEN);
                menu_render();
            } else {
                wifi_captures_render();
            }
            break;
    }
}

static void menu_captures(void) {
    captures_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(captures_encoder_handler);
    wifi_captures_enter();
    wifi_captures_render();
}

static void on_webui_op_start(const char *op) {
    if      (!strcmp(op, "scan"))    scanner_active   = true;
    else if (!strcmp(op, "sniff"))   sniffer_active   = true;
    else if (!strcmp(op, "attack"))  attack_active    = true;
    else if (!strcmp(op, "ap"))      ap_active        = true;
    else if (!strcmp(op, "sta"))     sta_active       = true;
    else if (!strcmp(op, "pmkid"))   pmkid_active     = true;
    else if (!strcmp(op, "hs"))      handshake_active = true;
}

static void on_webui_op_stop(const char *op) {
    if      (!strcmp(op, "scan"))    scanner_active   = false;
    else if (!strcmp(op, "sniff"))   sniffer_active   = false;
    else if (!strcmp(op, "attack"))  attack_active    = false;
    else if (!strcmp(op, "ap"))      ap_active        = false;
    else if (!strcmp(op, "sta"))     sta_active       = false;
    else if (!strcmp(op, "pmkid"))   pmkid_active     = false;
    else if (!strcmp(op, "hs"))      handshake_active = false;
    neopixel_set_color(COLOR_CYAN);
}

static void webui_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        wifi_webui_stop();
        webui_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    }
}

static void menu_webui(void) {
    webui_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(webui_encoder_handler);
    wifi_webui_set_op_callbacks(on_webui_op_start, on_webui_op_stop);
    wifi_webui_enter();
    wifi_webui_render();
}

// Hold BOOT 2s anywhere (except already in WebUI) → clean reboot into WebUI.
static void boot_button_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "BOOT button GPIO%d — hold %ds for Remote WebUI",
             (int)PIN_BOOT_BUTTON, BOOT_HOLD_MS / 1000);

    uint32_t held_ms = 0;
    while (1) {
        int level = gpio_get_level(PIN_BOOT_BUTTON);
        if (level == 0) {   // active LOW = pressed
            held_ms += BOOT_POLL_MS;
            if (held_ms >= BOOT_HOLD_MS) {
                if (webui_active) {
                    // Already serving — ignore until release (no reboot loop).
                    while (gpio_get_level(PIN_BOOT_BUTTON) == 0)
                        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
                    held_ms = 0;
                    continue;
                }
                ESP_LOGW(TAG, "BOOT held %lums → Remote WebUI",
                         (unsigned long)held_ms);
                boot_mode_reboot(BOOT_DEST_WEBUI);
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }
}

static void game_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        game_pong_stop();
        game_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        game_pong_input(event);
    }
}

static void menu_game(void) {
    game_active = true;
    neopixel_set_color(COLOR_MAGENTA);
    encoder_set_callback(game_encoder_handler);
    game_pong_enter();   // game renders from its own task
}

static void life_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        game_life_stop();
        life_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        game_life_input(event);
    }
}

static void menu_life(void) {
    life_active = true;
    neopixel_set_color(COLOR_MAGENTA);
    encoder_set_callback(life_encoder_handler);
    game_life_enter();   // seed screen, then sim renders from its own task
}

static void react_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        game_react_stop();
        react_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        game_react_input(event);
    }
}

static void menu_react(void) {
    react_active = true;
    encoder_set_callback(react_encoder_handler);
    game_react_enter();  // reaction test runs from its own task (drives the LED)
}

static void ble_scan_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_scan_exit();
        ble_scan_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_scan_input(event);
    }
}
static void menu_ble_scan(void) {
    ble_scan_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_scan_encoder_handler);
    ble_core_init();
    ble_scan_enter();
}

static void ble_class_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_class_exit();
        ble_class_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_class_input(event);
    }
}
static void menu_ble_class(void) {
    ble_class_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_class_encoder_handler);
    ble_core_init();
    ble_class_enter();
}

static void ble_beacon_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_beacon_exit();
        ble_beacon_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_beacon_input(event);
    }
}
static void menu_ble_beacon(void) {
    ble_beacon_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_beacon_encoder_handler);
    ble_core_init();
    ble_beacon_enter();
}

static void ble_hunt_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_hunt_exit();
        ble_hunt_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_hunt_input(event);
    }
}
static void menu_ble_hunt(void) {
    ble_hunt_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_hunt_encoder_handler);
    ble_core_init();
    ble_hunt_enter();
}

static void ble_gatt_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_gatt_exit();
        ble_gatt_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_gatt_input(event);
    }
}
static void menu_ble_gatt(void) {
    ble_gatt_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_gatt_encoder_handler);
    ble_core_init();
    ble_gatt_enter();
}

static void ble_notify_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_notify_exit();
        ble_notify_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_notify_input(event);
    }
}
static void menu_ble_notify(void) {
    ble_notify_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_notify_encoder_handler);
    ble_core_init();
    ble_notify_enter();
}

static void ble_spam_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_spam_exit();
        ble_spam_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_spam_input(event);
    }
}
static void menu_ble_spam(void) {
    ble_spam_active = true;
    neopixel_set_color(COLOR_MAGENTA);
    encoder_set_callback(ble_spam_encoder_handler);
    ble_core_init();
    ble_spam_enter();
}

static void ble_hid_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_hid_exit();
        ble_hid_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_hid_input(event);
    }
}
static void menu_ble_hid(void) {
    ble_hid_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(ble_hid_encoder_handler);
    ble_core_init();
    ble_hid_enter();
}

static void ble_advlog_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_advlog_exit();
        ble_advlog_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_advlog_input(event);
    }
}
static void menu_ble_advlog(void) {
    ble_advlog_active = true;
    neopixel_set_color(COLOR_BLUE);
    encoder_set_callback(ble_advlog_encoder_handler);
    ble_core_init();
    ble_advlog_enter();
}

static void ble_nus_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ble_nus_exit();
        ble_nus_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ble_nus_input(event);
    }
}
static void menu_ble_nus(void) {
    ble_nus_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(ble_nus_encoder_handler);
    ble_core_init();
    ble_nus_enter();
}

static void info_render(void) {
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

    uint16_t batt_mv = battery_read_mv();
    uint8_t  batt_pct = battery_get_percentage();
    if (battery_is_present() && batt_pct != BATTERY_INVALID)
        snprintf(line, sizeof(line), "Batt:%umV %u%%",
                 (unsigned)batt_mv, (unsigned)batt_pct);
    else
        snprintf(line, sizeof(line), "Batt: USB/none");
    line[16] = '\0';
    ssd1306_draw_string(0, 3, line);

    snprintf(line, sizeof(line), "Heap:%lu KB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    line[16] = '\0';
    ssd1306_draw_string(0, 4, line);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(line, sizeof(line), "Flash:%lu MB",
             (unsigned long)(flash_size / (1024 * 1024)));
    line[16] = '\0';
    ssd1306_draw_string(0, 5, line);

    int64_t up_s = esp_timer_get_time() / 1000000LL;
    if (up_s < 0) up_s = 0;
    int up_h  = (int)((up_s / 3600) % 24);
    int up_m  = (int)((up_s / 60) % 60);
    int up_sc = (int)(up_s % 60);
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
}

static void info_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        info_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        // CW / CCW / CLICK all refresh the screen (heap and uptime tick)
        info_render();
    }
}

static void menu_device_info(void) {
    info_active = true;
    neopixel_set_color(COLOR_WHITE);
    encoder_set_callback(info_encoder_handler);
    info_render();
}

static void fs_browser_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            fs_browser_scroll_down();
            fs_browser_render();
            break;
        case ENCODER_CCW:
            fs_browser_scroll_up();
            fs_browser_render();
            break;
        case ENCODER_CLICK:
            fs_browser_select();
            fs_browser_render();
            break;
        case ENCODER_LONG_PRESS:
            if (fs_browser_back()) {
                fs_browser_stop();
                fs_browser_active = false;
                encoder_set_callback(encoder_event_handler);
                neopixel_set_color(COLOR_GREEN);
                menu_render();
            } else {
                fs_browser_render();
            }
            break;
    }
}

static void menu_fs_browser(void) {
    fs_browser_active = true;
    neopixel_set_color(COLOR_CYAN);
    encoder_set_callback(fs_browser_encoder_handler);
    fs_browser_enter();
    fs_browser_render();
}

static void settings_ui_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            settings_scroll_down();
            settings_render();
            break;
        case ENCODER_CCW:
            settings_scroll_up();
            settings_render();
            break;
        case ENCODER_CLICK:
            settings_select();
            settings_render();
            break;
        case ENCODER_LONG_PRESS:
            settings_exit();
            settings_ui_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_settings_ui(void) {
    settings_ui_active = true;
    neopixel_set_color(COLOR_WHITE);
    encoder_set_callback(settings_ui_encoder_handler);
    settings_enter();
    settings_render();
}

static void espnow_encoder_handler(encoder_event_t event) {
    switch (event) {
        case ENCODER_CW:
            espnow_recon_scroll_down();
            espnow_recon_render();
            break;
        case ENCODER_CCW:
            espnow_recon_scroll_up();
            espnow_recon_render();
            break;
        case ENCODER_CLICK:
            espnow_recon_render();
            break;
        case ENCODER_LONG_PRESS:
            espnow_recon_stop();
            espnow_active = false;
            encoder_set_callback(encoder_event_handler);
            neopixel_set_color(COLOR_GREEN);
            menu_render();
            break;
    }
}

static void menu_espnow(void) {
    espnow_active = true;
    neopixel_set_color(COLOR_MAGENTA);
    encoder_set_callback(espnow_encoder_handler);
    espnow_recon_enter();
    espnow_recon_start();
    espnow_recon_render();
}

static void ieee154_encoder_handler(encoder_event_t event) {
    if (event == ENCODER_LONG_PRESS) {
        ieee154_sniff_stop();
        ieee154_active = false;
        encoder_set_callback(encoder_event_handler);
        neopixel_set_color(COLOR_GREEN);
        menu_render();
    } else {
        ieee154_sniff_render();
    }
}

static void menu_ieee154(void) {
    ieee154_active = true;
    neopixel_set_color(COLOR_YELLOW);
    encoder_set_callback(ieee154_encoder_handler);
    ieee154_sniff_enter();
    ieee154_sniff_start();
    ieee154_sniff_render();
}

static menu_item_t wifi_menu[] = {
    {.label = "WiFi Scan",     .on_select = menu_wifi_scanner},
    {.label = "Client Sniff",  .on_select = menu_wifi_sniffer},
    {.label = "WiFi Monitor",  .on_select = menu_wifi_monitor},
    {.label = "AP Mode",       .on_select = menu_ap_mode},
    {.label = "Deauth Attack", .on_select = menu_deauth_attack},
    {.label = "STA Connect",   .on_select = menu_sta_connect},
    {.label = "PMKID Capture", .on_select = menu_pmkid},
    {.label = "Handshake Cap", .on_select = menu_handshake},
    {.label = "Captures",      .on_select = menu_captures},
    {.label = "Remote WebUI",  .on_select = menu_webui},
    {.label = "Ch Chart",      .on_select = menu_ch_chart},
};

static menu_item_t bluetooth_menu[] = {
    {.label = "BLE Scanner",   .on_select = menu_ble_scan},
    {.label = "Classifier",    .on_select = menu_ble_class},
    {.label = "Beacons",       .on_select = menu_ble_beacon},
    {.label = "Device Hunter", .on_select = menu_ble_hunt},
    {.label = "GATT Explorer", .on_select = menu_ble_gatt},
    {.label = "Notify Mon",    .on_select = menu_ble_notify},
    {.label = "BLE Spam",      .on_select = menu_ble_spam},
    {.label = "BadBLE HID",    .on_select = menu_ble_hid},
    {.label = "Adv Logger",    .on_select = menu_ble_advlog},
    {.label = "NUS Link",      .on_select = menu_ble_nus},
};

static menu_item_t games_menu[] = {
    {.label = "Pong",          .on_select = menu_game},
    {.label = "Game of Life",  .on_select = menu_life},
    {.label = "Reaction Test", .on_select = menu_react},
};

static menu_item_t rf_menu[] = {
    {.label = "ESP-NOW Recon", .on_select = menu_espnow},
    {.label = "802.15.4 Sniff",.on_select = menu_ieee154},
};

static menu_item_t settings_menu[] = {
    {.label = "Device Info",   .on_select = menu_device_info},
    {.label = "Settings",      .on_select = menu_settings_ui},
    {.label = "File Explorer", .on_select = menu_fs_browser},
};

static void open_wifi_menu(void)      { menu_push_submenu(wifi_menu,      sizeof(wifi_menu)      / sizeof(wifi_menu[0])); }
static void open_bluetooth_menu(void) { menu_push_submenu(bluetooth_menu, sizeof(bluetooth_menu) / sizeof(bluetooth_menu[0])); }
static void open_games_menu(void)     { menu_push_submenu(games_menu,     sizeof(games_menu)     / sizeof(games_menu[0])); }
static void open_rf_menu(void)        { menu_push_submenu(rf_menu,        sizeof(rf_menu)        / sizeof(rf_menu[0])); }
static void open_settings_menu(void)  { menu_push_submenu(settings_menu,  sizeof(settings_menu)  / sizeof(settings_menu[0])); }

static menu_item_t main_menu[] = {
    {.label = "WiFi",      .on_select = open_wifi_menu},
    {.label = "Bluetooth", .on_select = open_bluetooth_menu},
    {.label = "RF / IoT",  .on_select = open_rf_menu},
    {.label = "Games",     .on_select = open_games_menu},
    {.label = "Settings",  .on_select = open_settings_menu},
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
            if (!scanner_active && !sniffer_active && !monitor_active && !attack_active && !ap_active && !sta_active && !chart_active && !info_active && !pmkid_active && !handshake_active && !captures_active && !webui_active && !game_active && !life_active && !react_active && !ble_scan_active && !ble_class_active && !ble_beacon_active && !ble_hunt_active && !ble_gatt_active && !ble_notify_active && !ble_spam_active && !ble_hid_active && !ble_advlog_active && !ble_nus_active && !fs_browser_active && !settings_ui_active && !espnow_active && !ieee154_active) menu_render();
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

static void ota_github_task(void *arg) {
    (void)arg;
    ota_github_run();   // never returns (always reboots)
    vTaskDelete(NULL);
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

    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path            = "/lfs",
        .partition_label      = "storage",
        .format_if_mount_failed = true,
    };
    if (esp_vfs_littlefs_register(&lfs_conf) != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed — captures won't be saved");
    }

    wifi_scan_init();
    wifi_sniff_init();
    wifi_monitor_init();
    wifi_ap_init();
    wifi_sta_init();
    wifi_attack_init();
    wifi_pmkid_init();
    wifi_handshake_init();
    wifi_captures_init();
    wifi_webui_init();
    wifi_profiles_init();
    radio_mgr_init();
    settings_init();
    fs_browser_init();
    espnow_recon_init();
    ieee154_sniff_init();
    cli_init();

    menu_init(main_menu, sizeof(main_menu) / sizeof(main_menu[0]));
    encoder_set_callback(encoder_event_handler);

    uint32_t boot_dest = boot_mode_consume();
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);

    if (boot_dest == BOOT_DEST_OTA) {
        ESP_LOGI(TAG, "BOOT request: GitHub OTA update");
        // Generous stack — TLS + HTTPS OTA is heavy. Never returns.
        xTaskCreate(ota_github_task, "ota_gh", 10240, NULL, 5, NULL);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (boot_dest == BOOT_DEST_WEBUI) {
        ESP_LOGI(TAG, "BOOT request: starting Remote WebUI");
        menu_webui();
    }

    ESP_LOGI(TAG, "Boot complete fw=%s (BOOT 2s=WebUI)", ota_github_fw_version());

    while (1) {
        if (!scanner_active && !sniffer_active && !monitor_active && !attack_active && !ap_active && !sta_active && !chart_active && !info_active && !pmkid_active && !handshake_active && !captures_active && !webui_active && !game_active && !life_active && !react_active && !ble_scan_active && !ble_class_active && !ble_beacon_active && !ble_hunt_active && !ble_gatt_active && !ble_notify_active && !ble_spam_active && !ble_hid_active && !ble_advlog_active && !ble_nus_active && !fs_browser_active && !settings_ui_active && !espnow_active && !ieee154_active) menu_render();
        if (attack_active && wifi_attack_is_running()) wifi_attack_render();
        if (ap_active && wifi_ap_is_running()) wifi_ap_render();
        if (sta_active && (wifi_sta_is_connecting() || wifi_sta_needs_refresh())) wifi_sta_render();
        // Unconditional refresh while active (fixes first-render lag)
        if (pmkid_active) wifi_pmkid_render();
        if (handshake_active) wifi_handshake_render();
        if (captures_active && wifi_captures_needs_refresh()) wifi_captures_render();
        if (webui_active) wifi_webui_render();
        if (sniffer_active) { wifi_sniff_render(); neopixel_pulse(COLOR_MAGENTA); }
        if (monitor_active) { wifi_monitor_render(); neopixel_pulse(COLOR_CYAN); }
        if (fs_browser_active && fs_browser_needs_refresh()) fs_browser_render();
        if (settings_ui_active && settings_needs_refresh()) settings_render();
        if (espnow_active) { espnow_recon_render(); neopixel_pulse(COLOR_MAGENTA); }
        if (ieee154_active) { ieee154_sniff_render(); neopixel_pulse(COLOR_YELLOW); }
        if (ble_scan_active)   { ble_scan_tick();   neopixel_pulse(COLOR_BLUE); }
        if (ble_class_active)  { ble_class_tick();  neopixel_pulse(COLOR_BLUE); }
        if (ble_beacon_active) { ble_beacon_tick(); neopixel_pulse(COLOR_BLUE); }
        if (ble_hunt_active)     ble_hunt_tick();
        if (ble_gatt_active)   { ble_gatt_tick();   neopixel_pulse(COLOR_BLUE); }
        if (ble_notify_active) { ble_notify_tick(); neopixel_pulse(COLOR_BLUE); }
        if (ble_spam_active)   { ble_spam_tick();   neopixel_pulse(COLOR_MAGENTA); }
        if (ble_hid_active)    { ble_hid_tick();    neopixel_pulse(COLOR_CYAN); }
        if (ble_advlog_active) { ble_advlog_tick(); neopixel_pulse(COLOR_BLUE); }
        if (ble_nus_active)    { ble_nus_tick();    neopixel_pulse(COLOR_CYAN); }
        battery_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
