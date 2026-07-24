#include "wifi_webui.h"
#include "webui_html.h"
#include "wifi_scan.h"
#include "wifi_sniffer.h"
#include "wifi_attack.h"
#include "wifi_ap.h"
#include "wifi_sta.h"
#include "wifi_pmkid.h"
#include "wifi_handshake.h"
#include "wifi_captures.h"
#include "ssd1306.h"
#include "neopixel.h"
#include "battery.h"
#include "ota_update.h"
#include "ota_github.h"
#include "boot_mode.h"
#include "settings.h"
#include "ble_core.h"
#include "ble_scan.h"
#include "ble_ident.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "webui";

#define MAX_WS_CLIENTS   4
#define AP_MAX_CONN      4

static httpd_handle_t    s_httpd       = NULL;
static volatile bool     s_running     = false;
static volatile uint8_t  s_clients     = 0;
static volatile bool     s_refresh     = false;
static volatile bool     s_restarting  = false;
static SemaphoreHandle_t s_ws_mutex    = NULL;
static int               s_ws_fds[MAX_WS_CLIENTS];

static webui_op_start_cb_t s_op_start_cb = NULL;
static webui_op_stop_cb_t  s_op_stop_cb  = NULL;
static volatile bool       s_ble_scanning = false;

// ---------- WS client registry ----------

static void ws_fd_add(int fd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] < 0) { s_ws_fds[i] = fd; break; }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_fd_remove(int fd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) { s_ws_fds[i] = -1; break; }
    }
    xSemaphoreGive(s_ws_mutex);
}

// ---------- broadcast helpers ----------

static void broadcast_text(const char *json) {
    if (!s_httpd || !json) return;
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0) {
            if (httpd_ws_send_frame_async(s_httpd, s_ws_fds[i], &f) != ESP_OK) {
                s_ws_fds[i] = -1;
            }
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void send_to_fd(int fd, const char *json) {
    if (!s_httpd || fd < 0 || !json) return;
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    httpd_ws_send_frame_async(s_httpd, fd, &f);
}

// Broadcast a short status line to all WS clients. Wrapped in a JSON {"event":"log","msg":...}.
// Use sparingly — these surface in the on-screen activity log.
static void emit_log(const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "ui: %s", msg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "log");
    cJSON_AddStringToObject(root, "msg", msg);
    char *s = cJSON_PrintUnformatted(root);
    if (s) { broadcast_text(s); free(s); }
    cJSON_Delete(root);
}

// ---------- WiFi event handlers ----------

static void on_ap_connect(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (s_clients < 255) s_clients++;
    s_refresh = true;
    ESP_LOGI(TAG, "Client connected (%u total)", (unsigned)s_clients);
    broadcast_text("{\"event\":\"wifi_ready\"}");
}

static void on_ap_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (s_clients > 0) s_clients--;
    s_refresh = true;
    ESP_LOGI(TAG, "Client disconnected (%u total)", (unsigned)s_clients);
    // Remove the fd from registry; client will WS-reconnect
    wifi_event_ap_stadisconnected_t *ev = data;
    (void)ev;
}

// ---------- AP/HTTP setup internal helper ----------

static void register_ws_event_handlers(void) {
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                               on_ap_connect, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                               on_ap_disconnect, NULL);
}

static void unregister_ws_event_handlers(void) {
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                 on_ap_connect);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                 on_ap_disconnect);
}

// Forward-declared — defined after HTTP handlers
static void register_uri_handlers(void);

static void start_ap_and_httpd(void) {
    wifi_config_t cfg = {0};
    const char *ssid = WEBUI_AP_SSID;
    size_t slen = strlen(ssid);
    memcpy(cfg.ap.ssid, ssid, slen);
    cfg.ap.ssid_len       = (uint8_t)slen;
    cfg.ap.channel        = WEBUI_AP_CHANNEL;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = AP_MAX_CONN;
    cfg.ap.ssid_hidden    = 0;

    // SoftAP-only (same as evil twin). APSTA left STA half half-associated and
    // often prevented phones from seeing / joining WiFiend-Remote reliably.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode AP failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config AP failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
        return;
    }

    register_ws_event_handlers();
    vTaskDelay(pdMS_TO_TICKS(400));

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers  = 14;
    hcfg.max_open_sockets  = 7;
    hcfg.max_resp_headers  = 8;
    hcfg.recv_wait_timeout = 30;
    hcfg.send_wait_timeout = 30;
    hcfg.lru_purge_enable  = true;
    hcfg.stack_size        = 8192;

    if (httpd_start(&s_httpd, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        s_httpd = NULL;
        return;
    }
    register_uri_handlers();
    ESP_LOGI(TAG, "WebUI up: SSID=%s IP=%s heap=%lu",
             WEBUI_AP_SSID, WEBUI_AP_IP,
             (unsigned long)esp_get_free_heap_size());
}

static void stop_ap_and_httpd(void) {
    unregister_ws_event_handlers();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_stop();
}

// ---------- cJSON state builder ----------

static cJSON *build_ble_array(void) {
    cJSON *arr = cJSON_CreateArray();
    ble_scan_lock();
    uint16_t cnt = 0;
    const ble_dev_info_t *devs = ble_scan_get_results(&cnt);
    for (uint16_t i = 0; i < cnt; i++) {
        const ble_dev_info_t *d = &devs[i];
        cJSON *dev = cJSON_CreateObject();
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[5], d->addr[4], d->addr[3],
                 d->addr[2], d->addr[1], d->addr[0]);
        cJSON_AddStringToObject(dev, "addr", addr);
        cJSON_AddStringToObject(dev, "name", d->name[0] ? d->name : "");
        cJSON_AddNumberToObject(dev, "rssi", d->rssi);
        cJSON_AddStringToObject(dev, "type", ble_classify_device(d));
        cJSON_AddItemToArray(arr, dev);
    }
    ble_scan_unlock();
    return arr;
}

static void push_ble_update(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "ble_update");
    cJSON_AddBoolToObject(root, "scanning", s_ble_scanning);
    ble_scan_lock();
    uint16_t cnt = 0;
    ble_scan_get_results(&cnt);
    cJSON_AddNumberToObject(root, "count", cnt);
    ble_scan_unlock();
    cJSON_AddItemToObject(root, "devices", build_ble_array());
    char *s = cJSON_PrintUnformatted(root);
    if (s) { broadcast_text(s); free(s); }
    cJSON_Delete(root);
}

static cJSON *build_state_json(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "state");

    // AP list from last scan
    uint16_t cnt = 0;
    const wifi_ap_info_t *aps = wifi_scan_get_results(&cnt);
    cJSON *ap_arr = cJSON_AddArrayToObject(root, "aps");
    for (uint16_t i = 0; i < cnt; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "security", aps[i].security);
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        cJSON_AddStringToObject(ap, "bssid", bs);
        cJSON_AddItemToArray(ap_arr, ap);
    }

    cJSON_AddBoolToObject(root, "attack_running",  wifi_attack_is_running());
    cJSON_AddBoolToObject(root, "pmkid_running",   wifi_pmkid_is_running());
    cJSON_AddBoolToObject(root, "hs_running",      wifi_handshake_is_running());
    cJSON_AddBoolToObject(root, "ap_running",      wifi_ap_is_running());
    cJSON_AddBoolToObject(root, "sta_connected",   wifi_sta_is_connected());

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddNumberToObject(root, "heap_kb", (double)(esp_get_free_heap_size() / 1024));
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON_AddNumberToObject(root, "flash_mb", (double)(flash_size / (1024 * 1024)));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char idf[16];
    strncpy(idf, esp_get_idf_version(), 15);
    idf[15] = '\0';
    cJSON_AddStringToObject(root, "idf_ver", idf);

    uint8_t batt = battery_get_percentage();
    if (batt != 0xFF) cJSON_AddNumberToObject(root, "battery_pct", batt);

    cJSON_AddNumberToObject(root, "capture_count", wifi_captures_get_count());
    cJSON_AddBoolToObject(root, "ble_scanning", s_ble_scanning);
    cJSON_AddItemToObject(root, "ble_devices", build_ble_array());

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->label[0])
        cJSON_AddStringToObject(root, "ota_slot", running->label);

    cJSON_AddStringToObject(root, "fw", ota_github_fw_version());
    cJSON_AddStringToObject(root, "home_ssid", settings_get_home_ssid());

    return root;
}

static void push_state_to_fd(int fd) {
    cJSON *root = build_state_json();
    char *s = cJSON_PrintUnformatted(root);
    if (s) { send_to_fd(fd, s); free(s); }
    cJSON_Delete(root);
}

static void __attribute__((unused)) push_state_all(void) {
    cJSON *root = build_state_json();
    char *s = cJSON_PrintUnformatted(root);
    if (s) { broadcast_text(s); free(s); }
    cJSON_Delete(root);
}

// ---------- scan result push ----------

static void push_scan_done(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "scan_done");
    uint16_t cnt = 0;
    const wifi_ap_info_t *aps = wifi_scan_get_results(&cnt);
    cJSON *arr = cJSON_AddArrayToObject(root, "aps");
    for (uint16_t i = 0; i < cnt; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "security", aps[i].security);
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        cJSON_AddStringToObject(ap, "bssid", bs);
        cJSON_AddItemToArray(arr, ap);
    }
    char *s = cJSON_PrintUnformatted(root);
    if (s) { broadcast_text(s); free(s); }
    cJSON_Delete(root);
}

// ---------- captures JSON endpoint ----------

static esp_err_t captures_json_handler(httpd_req_t *req) {
    cJSON *arr = cJSON_CreateArray();
    uint16_t n = wifi_captures_get_count();
    for (uint16_t i = 0; i < n; i++) {
        const cap_entry_t *e = wifi_captures_get_entry(i);
        if (!e) continue;
        cJSON *c = cJSON_CreateObject();
        char type[2] = { e->type, 0 };
        cJSON_AddStringToObject(c, "type", type);
        cJSON_AddStringToObject(c, "ssid", e->ssid[0] ? e->ssid : "");
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 e->bssid[0], e->bssid[1], e->bssid[2],
                 e->bssid[3], e->bssid[4], e->bssid[5]);
        cJSON_AddStringToObject(c, "bssid", bs);
        cJSON_AddNumberToObject(c, "index", i);
        cJSON_AddItemToArray(arr, c);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) {
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, s, strlen(s));
    free(s);
    return ESP_OK;
}

// ---------- log file handlers (mirrors captures_http.c) ----------

static esp_err_t serve_log(httpd_req_t *req, const char *path,
                            const char *fname) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        const char *msg = "(no captures of this type yet)\n";
        httpd_resp_send(req, msg, strlen(msg));
        return ESP_OK;
    }
    char buf[512];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, nr) != ESP_OK) break;
    }
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pmkid_log_handler(httpd_req_t *req) {
    return serve_log(req, wifi_captures_log_path('P'), "wifiend-pmkid.hc22000");
}

static esp_err_t handshake_log_handler(httpd_req_t *req) {
    return serve_log(req, wifi_captures_log_path('H'), "wifiend-handshakes.hc22000");
}

static esp_err_t entry_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req); return ESP_OK;
    }
    char idx_str[16];
    if (httpd_query_key_value(query, "i", idx_str, sizeof(idx_str)) != ESP_OK) {
        httpd_resp_send_404(req); return ESP_OK;
    }
    int idx = atoi(idx_str);
    if (idx < 0 || idx >= wifi_captures_get_count()) {
        httpd_resp_send_404(req); return ESP_OK;
    }
    char line[768];
    size_t n = wifi_captures_get_entry_line((uint16_t)idx, line, sizeof(line));
    if (n == 0) { httpd_resp_send_404(req); return ESP_OK; }
    const cap_entry_t *e = wifi_captures_get_entry((uint16_t)idx);
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"wifiend-%s-%d.hc22000\"",
             (e && e->type == 'P') ? "pmkid" : "handshake", idx);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_send(req, line, n);
    return ESP_OK;
}

// ---------- OTA firmware upload ----------

#define OTA_RECV_BUF 4096

static void ota_progress_broadcast(uint8_t pct, void *ctx) {
    (void)ctx;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"event\":\"ota\",\"state\":\"writing\",\"percent\":%u}", (unsigned)pct);
    broadcast_text(buf);
}

static esp_err_t ota_send_json(httpd_req_t *req, bool ok, const char *msg) {
    char body[192];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg ? msg : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req) {
    if (req->method != HTTP_POST) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    if (ota_update_is_active()) {
        return ota_send_json(req, false, "OTA already in progress");
    }

    size_t total = req->content_len;
    if (total == 0) {
        return ota_send_json(req, false, "Empty body");
    }
    if (total > 0x200000) {
        return ota_send_json(req, false, "Image too large for OTA slot");
    }

    emit_log("OTA upload started (%u bytes)", (unsigned)total);
    broadcast_text("{\"event\":\"ota\",\"state\":\"start\",\"percent\":0}");

    ota_update_set_progress_cb(ota_progress_broadcast, NULL);
    esp_err_t err = ota_update_begin(total);
    if (err != ESP_OK) {
        ota_update_set_progress_cb(NULL, NULL);
        broadcast_text("{\"event\":\"ota\",\"state\":\"error\",\"msg\":\"begin failed\"}");
        return ota_send_json(req, false, "OTA begin failed");
    }

    uint8_t *buf = malloc(OTA_RECV_BUF);
    if (!buf) {
        ota_update_abort();
        ota_update_set_progress_cb(NULL, NULL);
        return ota_send_json(req, false, "Out of memory");
    }

    size_t received = 0;
    bool header_ok = false;

    while (received < total) {
        int want = (int)((total - received > OTA_RECV_BUF) ? OTA_RECV_BUF : (total - received));
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0) {
            free(buf);
            ota_update_abort();
            ota_update_set_progress_cb(NULL, NULL);
            broadcast_text("{\"event\":\"ota\",\"state\":\"error\",\"msg\":\"recv failed\"}");
            emit_log("OTA recv failed at %u/%u", (unsigned)received, (unsigned)total);
            return ota_send_json(req, false, "Upload interrupted");
        }

        if (!header_ok) {
            if (!ota_update_validate_header(buf, (size_t)r)) {
                free(buf);
                ota_update_abort();
                ota_update_set_progress_cb(NULL, NULL);
                broadcast_text("{\"event\":\"ota\",\"state\":\"error\",\"msg\":\"bad header\"}");
                emit_log("OTA rejected: invalid image header");
                return ota_send_json(req, false, "Invalid firmware image");
            }
            header_ok = true;
        }

        err = ota_update_write(buf, (size_t)r);
        if (err != ESP_OK) {
            free(buf);
            ota_update_abort();
            ota_update_set_progress_cb(NULL, NULL);
            broadcast_text("{\"event\":\"ota\",\"state\":\"error\",\"msg\":\"write failed\"}");
            return ota_send_json(req, false, "OTA write failed");
        }
        received += (size_t)r;
    }
    free(buf);
    ota_update_set_progress_cb(NULL, NULL);

    err = ota_update_end(true);
    if (err != ESP_OK) {
        broadcast_text("{\"event\":\"ota\",\"state\":\"error\",\"msg\":\"finalize failed\"}");
        return ota_send_json(req, false, "OTA finalize failed");
    }

    broadcast_text("{\"event\":\"ota\",\"state\":\"done\",\"percent\":100}");
    emit_log("OTA complete — rebooting");
    ota_send_json(req, true, "OK — rebooting");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ---------- SPA root handler ----------

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t total = sizeof(WEBUI_HTML) - 1;
    size_t sent  = 0;
    while (sent < total) {
        size_t chunk = (total - sent > 4096) ? 4096 : (total - sent);
        if (httpd_resp_send_chunk(req, WEBUI_HTML + sent, (ssize_t)chunk) != ESP_OK) break;
        sent += chunk;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---------- WiFi-restart coordination ----------

static void wifi_restart_begin(void) {
    s_restarting = true;
    s_refresh    = true;
    broadcast_text("{\"event\":\"wifi_restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(80));
    stop_ap_and_httpd();
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;
    xSemaphoreGive(s_ws_mutex);
}

static void wifi_restart_finish(void) {
    start_ap_and_httpd();
    s_restarting = false;
    s_refresh    = true;
}

// ---------- scroll-to-index helper (for picker-based modules) ----------

static void scroll_to(void (*down_fn)(void), uint16_t idx) {
    for (uint16_t i = 0; i < idx; i++) down_fn();
}

// Find index of BSSID in current scan results. Returns UINT16_MAX if not found.
static uint16_t find_ap_idx(const char *bssid_str) {
    uint16_t cnt = 0;
    const wifi_ap_info_t *aps = wifi_scan_get_results(&cnt);
    for (uint16_t i = 0; i < cnt; i++) {
        char bs[18];
        snprintf(bs, sizeof(bs), "%02X:%02X:%02X:%02X:%02X:%02X",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        if (strcasecmp(bs, bssid_str) == 0) return i;
    }
    return UINT16_MAX;
}

// ---------- FreeRTOS tasks for WiFi-restarting operations ----------

static void scan_task(void *arg) {
    (void)arg;
    // SoftAP-only mode has no STA iface — scans require STA or APSTA.
    // Prefer APSTA so the phone can stay associated while we scan.
    emit_log("WiFi scan: enabling STA (AP stays up)...");
    broadcast_text("{\"event\":\"log\",\"msg\":\"Scanning APs…\"}");
    if (s_op_start_cb) s_op_start_cb("scan");

    wifi_mode_t prev = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APSTA failed (%s) — falling back to STA restart",
                 esp_err_to_name(err));
        emit_log("APSTA failed — brief SoftAP restart for scan");
        wifi_restart_begin();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        wifi_scan_start_quiet();
        wifi_restart_finish();
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
        wifi_scan_start_quiet();
        // Restore AP-only SoftAP (more reliable join than permanent APSTA).
        if (prev == WIFI_MODE_AP || prev == WIFI_MODE_NULL) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
    }

    if (s_op_stop_cb) s_op_stop_cb("scan");
    uint16_t cnt = 0;
    wifi_scan_get_results(&cnt);
    emit_log("Scan complete: %u AP%s", (unsigned)cnt, cnt == 1 ? "" : "s");
    push_scan_done();
    vTaskDelete(NULL);
}

static void ble_scan_task(void *arg) {
    (void)arg;
    emit_log("BLE scan starting (heap=%lu)",
             (unsigned long)esp_get_free_heap_size());
    ble_core_init();

    // Prefer no WiFi power-save while BLE listens (coex friendlier).
    esp_wifi_set_ps(WIFI_PS_NONE);

    for (int i = 0; i < 150 && !ble_core_is_ready(); i++)
        vTaskDelay(pdMS_TO_TICKS(100));

    if (!ble_core_is_ready()) {
        emit_log("BLE core not ready (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        s_ble_scanning = false;
        broadcast_text("{\"event\":\"ble_update\",\"scanning\":false,\"count\":0,\"devices\":[]}");
        vTaskDelete(NULL);
        return;
    }

    ble_scan_clear();
    if (s_op_start_cb) s_op_start_cb("ble");
    s_ble_scanning = true;
    push_ble_update();

    if (!ble_scan_disc_start()) {
        emit_log("BLE discovery failed to start");
        s_ble_scanning = false;
        if (s_op_stop_cb) s_op_stop_cb("ble");
        push_ble_update();
        vTaskDelete(NULL);
        return;
    }

    for (int tick = 0; tick < 60 && s_running && s_ble_scanning; tick++) {
        push_ble_update();
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    ble_scan_disc_stop();
    s_ble_scanning = false;
    if (s_op_stop_cb) s_op_stop_cb("ble");
    push_ble_update();

    ble_scan_lock();
    uint16_t cnt = 0;
    ble_scan_get_results(&cnt);
    ble_scan_unlock();
    emit_log("BLE scan done: %u device%s", (unsigned)cnt, cnt == 1 ? "" : "s");
    vTaskDelete(NULL);
}

static void sniff_monitor_task(void *arg) {
    if (s_op_start_cb) s_op_start_cb("sniff");
    wifi_sniff_start();
    emit_log("Client sniffer started");
    while (s_running) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event", "sniff_update");
        uint16_t sc = wifi_sniff_get_count();
        cJSON_AddNumberToObject(root, "count", sc);
        cJSON *arr = cJSON_AddArrayToObject(root, "clients");
        for (uint16_t i = 0; i < sc && i < 20; i++) {
            const wifi_client_t *c = wifi_sniff_get_client(i);
            if (!c) continue;
            cJSON *cl = cJSON_CreateObject();
            char ms[18];
            snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X",
                     c->mac[0], c->mac[1], c->mac[2],
                     c->mac[3], c->mac[4], c->mac[5]);
            cJSON_AddStringToObject(cl, "mac", ms);
            cJSON_AddNumberToObject(cl, "rssi", c->rssi);
            cJSON_AddNumberToObject(cl, "channel", c->channel);
            cJSON_AddItemToArray(arr, cl);
        }
        char *s = cJSON_PrintUnformatted(root);
        if (s) { broadcast_text(s); free(s); }
        cJSON_Delete(root);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    if (s_op_stop_cb) s_op_stop_cb("sniff");
    emit_log("Client sniffer stopped: %u client%s seen",
             (unsigned)wifi_sniff_get_count(),
             wifi_sniff_get_count() == 1 ? "" : "s");
    vTaskDelete(NULL);
}

static void pmkid_task(void *arg) {
    char *bssid = (char *)arg;
    emit_log("PMKID hunt: switching to STA mode (WebUI will briefly reconnect)");
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_restart_begin();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_pmkid_enter();
    uint16_t idx = find_ap_idx(bssid);
    if (idx != UINT16_MAX) scroll_to(wifi_pmkid_scroll_down, idx);
    free(bssid);

    if (s_op_start_cb) s_op_start_cb("pmkid");
    wifi_pmkid_select();

    wifi_restart_finish();
    emit_log("PMKID hunt started: target %s", wifi_pmkid_get_target());

    while (wifi_pmkid_is_running()) {
        const char *state_str = wifi_pmkid_is_captured() ? "captured" : "hunting";
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"pmkid_update\",\"state\":\"%s\",\"frames\":%u,\"target\":\"%s\",\"count\":%u}",
                 state_str, (unsigned)wifi_pmkid_get_frames(),
                 wifi_pmkid_get_target(), (unsigned)wifi_pmkid_get_count());
        broadcast_text(buf);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    const char *final = wifi_pmkid_is_captured() ? "captured" : "failed";
    char final_buf[128];
    snprintf(final_buf, sizeof(final_buf),
             "{\"event\":\"pmkid_update\",\"state\":\"%s\",\"target\":\"%s\",\"count\":%u}",
             final, wifi_pmkid_get_target(), (unsigned)wifi_pmkid_get_count());
    broadcast_text(final_buf);
    if (s_op_stop_cb) s_op_stop_cb("pmkid");
    emit_log("PMKID hunt done: %s (%u capture%s)",
             final, (unsigned)wifi_pmkid_get_count(),
             wifi_pmkid_get_count() == 1 ? "" : "s");
    vTaskDelete(NULL);
}

static void hs_task(void *arg) {
    char *bssid = (char *)arg;
    emit_log("Handshake hunt: switching to STA mode (WebUI will briefly reconnect)");
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_restart_begin();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_handshake_enter();
    uint16_t idx = find_ap_idx(bssid);
    if (idx != UINT16_MAX) scroll_to(wifi_handshake_scroll_down, idx);
    free(bssid);

    if (s_op_start_cb) s_op_start_cb("hs");
    wifi_handshake_select();

    wifi_restart_finish();
    emit_log("Handshake hunt started: target %s", wifi_handshake_get_target());

    while (wifi_handshake_is_running()) {
        const char *state_str = wifi_handshake_is_captured() ? "captured" : "hunting";
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"hs_update\",\"state\":\"%s\","
                 "\"m1_seen\":%u,\"m2_seen\":%u,\"deauth_sent\":%u,\"target\":\"%s\"}",
                 state_str,
                 (unsigned)wifi_handshake_get_m1(),
                 (unsigned)wifi_handshake_get_m2(),
                 (unsigned)wifi_handshake_get_deauth(),
                 wifi_handshake_get_target());
        broadcast_text(buf);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    const char *final = wifi_handshake_is_captured() ? "captured" : "failed";
    char final_buf[128];
    snprintf(final_buf, sizeof(final_buf),
             "{\"event\":\"hs_update\",\"state\":\"%s\",\"target\":\"%s\",\"count\":%u}",
             final, wifi_handshake_get_target(), (unsigned)wifi_handshake_get_count());
    broadcast_text(final_buf);
    if (s_op_stop_cb) s_op_stop_cb("hs");
    emit_log("Handshake hunt done: %s", final);
    vTaskDelete(NULL);
}

typedef struct { char ssid[33]; char password[65]; uint8_t security; } sta_connect_arg_t;

static void sta_task(void *arg) {
    sta_connect_arg_t *a = (sta_connect_arg_t *)arg;
    emit_log("STA connect: %s (WebUI will briefly reconnect)", a->ssid);
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_restart_begin();

    if (s_op_start_cb) s_op_start_cb("sta");
    wifi_sta_connect_direct(a->ssid, a->security, a->password);
    free(a);

    wifi_restart_finish();

    for (int tick = 0; tick < 60 && s_running; tick++) {
        wifi_connect_state_t st = wifi_sta_get_state();
        const char *state_str = (st == WIFI_CONNECT_CONNECTED)   ? "connected"  :
                                (st == WIFI_CONNECT_CONNECTING)   ? "connecting" :
                                (st == WIFI_CONNECT_FAILED)       ? "failed"     : "idle";
        char buf[128];
        const char *ip = wifi_sta_get_ip();
        if (ip && ip[0])
            snprintf(buf, sizeof(buf), "{\"event\":\"sta_update\",\"state\":\"%s\",\"ip\":\"%s\"}",
                     state_str, ip);
        else
            snprintf(buf, sizeof(buf), "{\"event\":\"sta_update\",\"state\":\"%s\"}", state_str);
        broadcast_text(buf);
        if (st == WIFI_CONNECT_CONNECTED) {
            emit_log("STA connected: %s", ip ? ip : "(no IP)");
            break;
        }
        if (st == WIFI_CONNECT_FAILED) {
            emit_log("STA connect failed");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_op_stop_cb) s_op_stop_cb("sta");
    vTaskDelete(NULL);
}

// ---------- command dispatch ----------

static void handle_command(httpd_req_t *req, const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *cmd_j = cJSON_GetObjectItem(root, "cmd");
    const char *cmd = cmd_j ? cJSON_GetStringValue(cmd_j) : NULL;
    if (!cmd) { cJSON_Delete(root); return; }

    if (!strcmp(cmd, "get_state")) {
        cJSON *state = build_state_json();
        char *s = cJSON_PrintUnformatted(state);
        if (s) {
            send_to_fd(httpd_req_to_sockfd(req), s);
            free(s);
        }
        cJSON_Delete(state);

    } else if (!strcmp(cmd, "scan_start")) {
        xTaskCreate(scan_task, "webui_scan", 4096, NULL, 4, NULL);

    } else if (!strcmp(cmd, "scan_get")) {
        push_scan_done();

    } else if (!strcmp(cmd, "sniff_start")) {
        xTaskCreate(sniff_monitor_task, "webui_sniff", 4096, NULL, 4, NULL);

    } else if (!strcmp(cmd, "sniff_stop")) {
        wifi_sniff_stop();
        if (s_op_stop_cb) s_op_stop_cb("sniff");
        broadcast_text("{\"event\":\"sniff_update\",\"count\":0,\"clients\":[]}");

    } else if (!strcmp(cmd, "sta_connect")) {
        cJSON *ssid_j = cJSON_GetObjectItem(root, "ssid");
        cJSON *pw_j   = cJSON_GetObjectItem(root, "password");
        cJSON *sec_j  = cJSON_GetObjectItem(root, "security");
        if (ssid_j && cJSON_IsString(ssid_j)) {
            sta_connect_arg_t *a = calloc(1, sizeof(sta_connect_arg_t));
            strncpy(a->ssid, ssid_j->valuestring, 32);
            if (pw_j && cJSON_IsString(pw_j))
                strncpy(a->password, pw_j->valuestring, 64);
            a->security = (sec_j && cJSON_IsNumber(sec_j)) ?
                          (uint8_t)sec_j->valuedouble : 3;
            xTaskCreate(sta_task, "webui_sta", 4096, a, 4, NULL);
        }

    } else if (!strcmp(cmd, "sta_stop")) {
        wifi_sta_stop();
        if (s_op_stop_cb) s_op_stop_cb("sta");
        broadcast_text("{\"event\":\"sta_update\",\"state\":\"idle\"}");

    } else if (!strcmp(cmd, "pmkid_start")) {
        cJSON *b = cJSON_GetObjectItem(root, "bssid");
        if (b && cJSON_IsString(b)) {
            char *bssid = strdup(b->valuestring);
            xTaskCreate(pmkid_task, "webui_pmkid", 4096, bssid, 4, NULL);
        }

    } else if (!strcmp(cmd, "pmkid_stop")) {
        emit_log("Stopping PMKID hunt");
        wifi_pmkid_stop();
        if (s_op_stop_cb) s_op_stop_cb("pmkid");

    } else if (!strcmp(cmd, "hs_start")) {
        cJSON *b = cJSON_GetObjectItem(root, "bssid");
        if (b && cJSON_IsString(b)) {
            char *bssid = strdup(b->valuestring);
            xTaskCreate(hs_task, "webui_hs", 4096, bssid, 4, NULL);
        }

    } else if (!strcmp(cmd, "hs_deauth")) {
        emit_log("Sending deauth burst to flush handshake");
        wifi_handshake_select();

    } else if (!strcmp(cmd, "hs_stop")) {
        emit_log("Stopping handshake hunt");
        wifi_handshake_stop();
        if (s_op_stop_cb) s_op_stop_cb("hs");

    } else if (!strcmp(cmd, "exit")) {
        broadcast_text("{\"event\":\"server_stopping\"}");
        vTaskDelay(pdMS_TO_TICKS(50));
        wifi_webui_stop();

    } else if (!strcmp(cmd, "ble_scan_start")) {
        if (!s_ble_scanning)
            xTaskCreate(ble_scan_task, "webui_ble", 6144, NULL, 4, NULL);

    } else if (!strcmp(cmd, "ble_scan_stop")) {
        s_ble_scanning = false;
        ble_scan_disc_stop();
        if (s_op_stop_cb) s_op_stop_cb("ble");
        push_ble_update();
    }

    cJSON_Delete(root);
}

// ---------- WebSocket handler ----------

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_fd_add(fd);
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        push_state_to_fd(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK || frame.len == 0 || frame.len > 512) {
        if (frame.len == 0 && frame.type == HTTPD_WS_TYPE_CLOSE) {
            ws_fd_remove(httpd_req_to_sockfd(req));
        }
        return ESP_OK;
    }

    uint8_t buf[513] = {0};
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) return ESP_FAIL;

    handle_command(req, (char *)buf);
    return ESP_OK;
}

// POST /update — save home Wi-Fi, reboot into GitHub OTA mode (WiFuxx parity).
// Body: {"ssid":"...","pass":"..."}  (pass optional = leave unchanged)
static esp_err_t update_post_handler(httpd_req_t *req) {
    char buf[256];
    int to_read = req->content_len;
    if (to_read <= 0 || to_read >= (int)sizeof(buf)) to_read = (int)sizeof(buf) - 1;

    int received = 0;
    while (received < to_read) {
        int r = httpd_req_recv(req, buf + received, to_read - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        received += r;
    }
    buf[received > 0 ? received : 0] = '\0';

    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    cJSON *ssid_j = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(j, "pass");
    const char *ssid = (ssid_j && cJSON_IsString(ssid_j)) ? ssid_j->valuestring : NULL;
    const char *pass = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : NULL;

    if (ssid && ssid[0])
        settings_set_home_wifi(ssid, pass);
    cJSON_Delete(j);

    if (settings_get_home_ssid()[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no ssid\"}");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "GitHub OTA requested (home AP '%s')", settings_get_home_ssid());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    boot_mode_reboot(BOOT_DEST_OTA);
    return ESP_OK;
}

// ---------- URI handler registration ----------

static void register_uri_handlers(void) {
    httpd_uri_t r_root    = { .uri = "/",               .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t r_ws      = { .uri = "/ws",             .method = HTTP_GET, .handler = ws_handler,
                               .is_websocket = true };
    httpd_uri_t r_pmkid   = { .uri = "/pmkid.log",      .method = HTTP_GET, .handler = pmkid_log_handler };
    httpd_uri_t r_hshk    = { .uri = "/handshakes.log", .method = HTTP_GET, .handler = handshake_log_handler };
    httpd_uri_t r_entry   = { .uri = "/entry",          .method = HTTP_GET, .handler = entry_handler };
    httpd_uri_t r_caps    = { .uri = "/captures",       .method = HTTP_GET, .handler = captures_json_handler };
    httpd_uri_t r_ota     = { .uri = "/ota",            .method = HTTP_POST, .handler = ota_handler };
    httpd_uri_t r_update  = { .uri = "/update",         .method = HTTP_POST, .handler = update_post_handler };

    httpd_register_uri_handler(s_httpd, &r_root);
    httpd_register_uri_handler(s_httpd, &r_ws);
    httpd_register_uri_handler(s_httpd, &r_pmkid);
    httpd_register_uri_handler(s_httpd, &r_hshk);
    httpd_register_uri_handler(s_httpd, &r_entry);
    httpd_register_uri_handler(s_httpd, &r_caps);
    httpd_register_uri_handler(s_httpd, &r_ota);
    httpd_register_uri_handler(s_httpd, &r_update);
}

// ---------- OLED render ----------

void wifi_webui_render(void) {
    char hdr_r[12];
    snprintf(hdr_r, sizeof(hdr_r), "%u client%s",
             (unsigned)s_clients, s_clients == 1 ? "" : "s");

    ssd1306_clear_buffer();
    ssd1306_draw_header("Remote WebUI", hdr_r);
    ssd1306_draw_string(0, 2, "WiFiend-Remote");
    ssd1306_draw_string(0, 3, "192.168.4.1");

    char line[17];
    if (!s_httpd) {
        ssd1306_draw_string(0, 4, "HTTP FAIL");
        ssd1306_draw_string(0, 5, "Check serial log");
    } else if (s_restarting) {
        ssd1306_draw_string(0, 4, "WiFi restart..");
    } else if (s_clients == 0) {
        ssd1306_draw_string(0, 4, "Join AP, then");
        ssd1306_draw_string(0, 5, "open 192.168.4.1");
    } else {
        snprintf(line, sizeof(line), "%u connected", (unsigned)s_clients);
        ssd1306_draw_string(0, 4, line);
        ssd1306_draw_string(0, 5, "UI serving");
    }

    uint8_t batt = battery_get_percentage();
    if (batt != 0xFF)
        snprintf(line, sizeof(line), "Bat:%u%% LN>back", (unsigned)batt);
    else
        snprintf(line, sizeof(line), "Bat:-- LN>back");
    ssd1306_draw_string(0, 7, line);
    ssd1306_flush();
}

// ---------- Public API ----------

void wifi_webui_init(void) {
    if (!s_ws_mutex) s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;
}

void wifi_webui_enter(void) {
    if (s_running) return;
    s_clients    = 0;
    s_restarting = false;
    s_httpd      = NULL;

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(150));

    start_ap_and_httpd();
    s_running = true;
    s_refresh = true;

    // Warm NimBLE early so Scan BLE is ready when the user taps it.
    ble_core_init();

    if (!s_httpd) {
        ESP_LOGE(TAG, "WebUI HTTP server did not start");
    }
}

void wifi_webui_stop(void) {
    if (!s_running) return;
    s_ble_scanning = false;
    ble_scan_disc_stop();
    // Halt any operation that could outlive the WebUI (e.g. a cross-band deauth
    // the user is escaping from via the encoder after the phone dropped).
    if (wifi_attack_is_running()) wifi_attack_stop();
    s_running = false;   // signals monitor tasks (sniffer) to exit their loops
    stop_ap_and_httpd();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_running    = false;
    s_clients    = 0;
    s_refresh    = false;
    s_restarting = false;
    ESP_LOGI(TAG, "WebUI stopped");
}

bool wifi_webui_is_running(void)    { return s_running; }
bool wifi_webui_needs_refresh(void) {
    bool v = s_refresh;
    s_refresh = false;
    return v;
}

void wifi_webui_set_op_callbacks(webui_op_start_cb_t on_start,
                                 webui_op_stop_cb_t  on_stop) {
    s_op_start_cb = on_start;
    s_op_stop_cb  = on_stop;
}
