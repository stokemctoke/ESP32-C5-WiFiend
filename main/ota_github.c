#include "ota_github.h"
#include "boot_mode.h"
#include "settings.h"
#include "ssd1306.h"
#include "neopixel.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ota_github";

#define OTA_GITHUB_OWNER    "stokemctoke"
#define OTA_GITHUB_REPO     "ESP32-C5-WiFiend"
#define OTA_ASSET_NAME      "WiFiend.bin"
#define OTA_WIFI_TIMEOUT_MS 20000
#define OTA_WIFI_MAX_RETRY  5

#define OTA_WIFI_CONNECTED_BIT BIT0
#define OTA_WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static int s_wifi_retry;

const char *ota_github_fw_version(void) {
    return esp_app_get_description()->version;
}

static void parse_semver(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static int semver_cmp(const char *a, const char *b) {
    int va[3], vb[3];
    parse_semver(a, va);
    parse_semver(b, vb);
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    return 0;
}

static void ota_show(const char *l1, const char *l2, const char *l3) {
    ssd1306_clear_buffer();
    ssd1306_draw_string(0, 0, ">> OTA UPDATE");
    if (l1) ssd1306_draw_string(0, 2, l1);
    if (l2) ssd1306_draw_string(0, 4, l2);
    if (l3) ssd1306_draw_string(0, 6, l3);
    ssd1306_flush();
}

static void ota_progress(int pct) {
    char line[17];
    snprintf(line, sizeof(line), "Writing %d%%", pct);
    ssd1306_clear_page(4);
    ssd1306_draw_string(0, 4, line);
    ssd1306_flush();
}

static void ota_wifi_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < OTA_WIFI_MAX_RETRY) {
            s_wifi_retry++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_wifi_retry, OTA_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, OTA_WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_events, OTA_WIFI_CONNECTED_BIT);
    }
}

static bool ota_wifi_connect(const char *ssid, const char *pass) {
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return false;
    s_wifi_retry = 0;
    xEventGroupClearBits(s_wifi_events, OTA_WIFI_CONNECTED_BIT | OTA_WIFI_FAIL_BIT);

    esp_event_handler_instance_t inst_wifi = NULL, inst_ip = NULL;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        ota_wifi_event_handler, NULL, &inst_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        ota_wifi_event_handler, NULL, &inst_ip);

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wc = {0};
    size_t sl = strnlen(ssid, sizeof(wc.sta.ssid) - 1);
    size_t pl = strnlen(pass, sizeof(wc.sta.password) - 1);
    memcpy(wc.sta.ssid, ssid, sl);
    memcpy(wc.sta.password, pass, pl);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, OTA_WIFI_CONNECTED_BIT | OTA_WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(OTA_WIFI_TIMEOUT_MS));

    if (inst_wifi) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    if (inst_ip)   esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);

    return (bits & OTA_WIFI_CONNECTED_BIT) != 0;
}

static bool github_latest_tag(char *tag_out, size_t tag_sz) {
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/%s/releases/latest",
             OTA_GITHUB_OWNER, OTA_GITHUB_REPO);

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "WiFiend-OTA");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            char buf[3072];
            int off = 0, r;
            while (off < (int)sizeof(buf) - 1 &&
                   (r = esp_http_client_read(c, buf + off, sizeof(buf) - 1 - off)) > 0)
                off += r;
            buf[off] = '\0';

            char *p = strstr(buf, "\"tag_name\"");
            if (p && (p = strchr(p, ':')) && (p = strchr(p, '"'))) {
                p++;
                char *e = strchr(p, '"');
                if (e && (size_t)(e - p) < tag_sz) {
                    memcpy(tag_out, p, (size_t)(e - p));
                    tag_out[e - p] = '\0';
                    ok = true;
                }
            }
            if (!ok) ESP_LOGE(TAG, "tag_name not found in GitHub response");
        } else {
            ESP_LOGE(TAG, "GitHub API HTTP %d", status);
        }
    } else {
        ESP_LOGE(TAG, "GitHub API connection failed");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static esp_err_t ota_http_client_init_cb(esp_http_client_handle_t c) {
    esp_http_client_set_header(c, "User-Agent", "WiFiend-OTA");
    return ESP_OK;
}

static bool ota_download_and_flash(const char *tag) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/%s/%s/releases/download/%s/%s",
             OTA_GITHUB_OWNER, OTA_GITHUB_REPO, tag, OTA_ASSET_NAME);
    ESP_LOGI(TAG, "downloading %s", url);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        // GitHub redirect signed URLs need a large TX buffer.
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config         = &http_cfg,
        .http_client_init_cb = ota_http_client_init_cb,
    };

    esp_https_ota_handle_t h = NULL;
    if (esp_https_ota_begin(&ota_cfg, &h) != ESP_OK || !h) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed");
        return false;
    }

    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(h);
        int pct = (total > 0) ? (read * 100 / total) : 0;
        if (pct != last_pct) {
            last_pct = pct;
            ota_progress(pct);
        }
    }

    bool ok = (err == ESP_OK) && esp_https_ota_is_complete_data_received(h);
    if (esp_https_ota_finish(h) != ESP_OK) ok = false;
    if (!ok) ESP_LOGE(TAG, "download/flash failed: %s", esp_err_to_name(err));
    return ok;
}

void ota_github_run(void) {
    const char *ssid = settings_get_home_ssid();
    const char *pass = settings_get_home_pass();

    neopixel_set_color(COLOR_CYAN);

    if (!ssid || ssid[0] == '\0') {
        ota_show("No Wi-Fi saved", "Set it in the", "WebUI Update card");
        vTaskDelay(pdMS_TO_TICKS(4000));
        boot_mode_reboot(BOOT_DEST_WEBUI);
    }

    ota_show("Joining Wi-Fi:", ssid, "connecting...");
    if (!ota_wifi_connect(ssid, pass ? pass : "")) {
        ota_show("Wi-Fi FAILED", "Check name/pass", "in WebUI, retry");
        vTaskDelay(pdMS_TO_TICKS(5000));
        boot_mode_reboot(BOOT_DEST_WEBUI);
    }

    neopixel_set_color(COLOR_YELLOW);
    ota_show("Connected.", "Checking GitHub", "for updates...");
    char tag[32];
    if (!github_latest_tag(tag, sizeof(tag))) {
        ota_show("Check FAILED", "No internet?", "Retry via WebUI");
        vTaskDelay(pdMS_TO_TICKS(5000));
        boot_mode_reboot(BOOT_DEST_WEBUI);
    }

    const char *cur = ota_github_fw_version();
    // Oversized; ssd1306_draw_string clips at 16 chars.
    char l_cur[48], l_new[48];
    snprintf(l_cur, sizeof(l_cur), "have %.16s", cur);
    snprintf(l_new, sizeof(l_new), "new  %.16s", tag);
    ESP_LOGI(TAG, "current %s, latest %s", cur, tag);

    if (semver_cmp(tag, cur) <= 0) {
        ota_show("Up to date :)", l_cur, l_new);
        vTaskDelay(pdMS_TO_TICKS(6000));
        boot_mode_reboot(BOOT_DEST_MENU);
    }

    neopixel_set_color(COLOR_RED);
    ota_show("Updating!", l_new, "DO NOT UNPLUG");
    vTaskDelay(pdMS_TO_TICKS(1200));

    if (ota_download_and_flash(tag)) {
        ota_show("Update OK!", l_new, "rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2500));
        boot_mode_reboot(BOOT_DEST_MENU);
    } else {
        ota_show("Update FAILED", "kept old fw", "Retry via WebUI");
        vTaskDelay(pdMS_TO_TICKS(5000));
        boot_mode_reboot(BOOT_DEST_WEBUI);
    }
}
