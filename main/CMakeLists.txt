idf_component_register(
    SRCS
        main.c
        ssd1306.c
        encoder.c
        neopixel.c
        battery.c
        menu.c
        wifi_scan.c
        wifi_sniffer.c
        wifi_ap.c
        wifi_sta.c
        wifi_attack.c
        captive_portal.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        driver
        led_strip
        esp_adc
        esp_hw_support
        lwip
        spi_flash
)

# Patched libnet80211 must appear before ESP-IDF's stock version so its
# symbols take precedence, enabling unrestricted raw deauth frame TX.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    ${CMAKE_SOURCE_DIR}/patched_libnet/libnet80211.a
    -Wl,--allow-multiple-definition)
