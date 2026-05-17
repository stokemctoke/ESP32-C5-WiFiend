#include "captures_http.h"
#include "wifi_captures.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "caphttp";

#define AP_CHANNEL          6
#define AP_MAX_CONNECTIONS  4

static httpd_handle_t   httpd          = NULL;
static volatile bool    running        = false;
static volatile uint8_t client_count   = 0;
static volatile bool    state_changed  = false;

// ---------- WiFi event handlers ----------

static void on_client_connect(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    if (client_count < 255) client_count++;
    state_changed = true;
    ESP_LOGI(TAG, "Client connected (%u total)", (unsigned)client_count);
}

static void on_client_disconnect(void *arg, esp_event_base_t base,
                                 int32_t id, void *data) {
    if (client_count > 0) client_count--;
    state_changed = true;
    ESP_LOGI(TAG, "Client disconnected (%u total)", (unsigned)client_count);
}

// ---------- HTTP handlers ----------

static const char INDEX_TOP[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>WiFiend Captures</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:16px;}"
".wrap{max-width:640px;margin:0 auto;}"
"h1{color:#58a6ff;font-size:22px;margin:8px 0 4px;}"
".sub{color:#8b949e;font-size:13px;margin-bottom:18px;}"
".section{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px;margin-bottom:14px;}"
".section h2{margin:0 0 10px;font-size:15px;color:#e6edf3;}"
".cap{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:8px 0;}"
".type{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:600;letter-spacing:.4px;}"
".type-P{background:#1f6feb;color:#fff;}"
".type-H{background:#238636;color:#fff;}"
".ssid{font-weight:600;margin:6px 0 4px;font-size:15px;}"
".mac{font-family:ui-monospace,monospace;color:#8b949e;font-size:12px;margin:2px 0;}"
"a.btn{display:inline-block;background:#238636;color:#fff;text-decoration:none;padding:7px 13px;border-radius:6px;margin-top:8px;font-size:13px;font-weight:500;}"
"a.btn:hover{background:#2ea043;}"
"a.btn.alt{background:#1f6feb;}"
"a.btn.alt:hover{background:#388bfd;}"
".empty{color:#8b949e;text-align:center;padding:30px 0;}"
"</style></head><body><div class='wrap'>"
"<h1>WiFiend Captures</h1>"
"<div class='sub'>Pen-test capture exporter &middot; hashcat-22000 format</div>";

static const char INDEX_BOTTOM[] =
"<div class='sub' style='text-align:center;margin-top:20px;'>"
"Save lines as a .hc22000 file and run: <code style='background:#161b22;padding:2px 6px;border-radius:4px;'>hashcat -m 22000 file.hc22000 wordlist.txt</code>"
"</div></div></body></html>";

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, INDEX_TOP, sizeof(INDEX_TOP) - 1);

    uint16_t n = wifi_captures_get_count();

    // Bulk download section
    char buf[1024];
    int  len;

    len = snprintf(buf, sizeof(buf),
        "<div class='section'>"
        "<h2>Bulk download (%u total)</h2>"
        "<a href='/pmkid.log' class='btn alt'>All PMKID</a> "
        "<a href='/handshakes.log' class='btn alt'>All Handshakes</a>"
        "</div>"
        "<div class='section'>"
        "<h2>Individual captures</h2>",
        (unsigned)n);
    if (len > 0) httpd_resp_send_chunk(req, buf, len);

    if (n == 0) {
        const char *empty = "<div class='empty'>No captures yet. Run PMKID or Handshake capture first.</div>";
        httpd_resp_send_chunk(req, empty, strlen(empty));
    } else {
        for (uint16_t i = 0; i < n; i++) {
            const cap_entry_t *e = wifi_captures_get_entry(i);
            if (!e) continue;
            const char *label = (e->type == 'P') ? "PMKID" : "Handshake";
            const char *ssid  = e->ssid[0] ? e->ssid : "(hidden)";

            len = snprintf(buf, sizeof(buf),
                "<div class='cap'>"
                "<span class='type type-%c'>%s</span>"
                "<div class='ssid'>%s</div>"
                "<div class='mac'>BSSID:&nbsp;&nbsp;%02x:%02x:%02x:%02x:%02x:%02x</div>"
                "<div class='mac'>Client: %02x:%02x:%02x:%02x:%02x:%02x</div>"
                "<a href='/entry?i=%u' class='btn'>Download line</a>"
                "</div>",
                e->type, label, ssid,
                e->bssid[0], e->bssid[1], e->bssid[2],
                e->bssid[3], e->bssid[4], e->bssid[5],
                e->client_mac[0], e->client_mac[1], e->client_mac[2],
                e->client_mac[3], e->client_mac[4], e->client_mac[5],
                (unsigned)i);
            if (len > 0) httpd_resp_send_chunk(req, buf, len);
        }
    }

    httpd_resp_send_chunk(req, "</div>", 6);
    httpd_resp_send_chunk(req, INDEX_BOTTOM, sizeof(INDEX_BOTTOM) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t serve_log_file(httpd_req_t *req, const char *path,
                                const char *download_name) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", download_name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        const char *msg = "(no captures of this type yet)\n";
        httpd_resp_send(req, msg, strlen(msg));
        return ESP_OK;
    }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t pmkid_log_handler(httpd_req_t *req) {
    return serve_log_file(req, wifi_captures_log_path('P'),
                          "wifiend-pmkid.hc22000");
}

static esp_err_t handshake_log_handler(httpd_req_t *req) {
    return serve_log_file(req, wifi_captures_log_path('H'),
                          "wifiend-handshakes.hc22000");
}

static esp_err_t entry_handler(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    char idx_str[16];
    if (httpd_query_key_value(query, "i", idx_str, sizeof(idx_str)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    int idx = atoi(idx_str);
    if (idx < 0 || idx >= wifi_captures_get_count()) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    char line[768];
    size_t n = wifi_captures_get_entry_line((uint16_t)idx, line, sizeof(line));
    if (n == 0) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    const cap_entry_t *e = wifi_captures_get_entry((uint16_t)idx);
    char cd[96];
    snprintf(cd, sizeof(cd), "attachment; filename=\"wifiend-%s-%d.hc22000\"",
             (e && e->type == 'P') ? "pmkid" : "handshake", idx);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_send(req, line, n);
    return ESP_OK;
}

// ---------- API ----------

void captures_http_start(void) {
    if (running) return;
    client_count = 0;

    // Switch to AP mode with a known SSID, open network, channel 6
    esp_wifi_stop();

    wifi_config_t cfg = { 0 };
    const char *ssid = CAPTURES_HTTP_SSID;
    size_t      slen = strlen(ssid);
    memcpy(cfg.ap.ssid, ssid, slen);
    cfg.ap.ssid_len       = (uint8_t)slen;
    cfg.ap.channel        = AP_CHANNEL;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                               on_client_connect, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                               on_client_disconnect, NULL);

    // Brief settle before opening port 80
    vTaskDelay(pdMS_TO_TICKS(200));

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 8;
    hcfg.lru_purge_enable = true;

    if (httpd_start(&httpd, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t r_index = { .uri = "/",                .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t r_pmkid = { .uri = "/pmkid.log",       .method = HTTP_GET, .handler = pmkid_log_handler };
    httpd_uri_t r_hshk  = { .uri = "/handshakes.log",  .method = HTTP_GET, .handler = handshake_log_handler };
    httpd_uri_t r_entry = { .uri = "/entry",           .method = HTTP_GET, .handler = entry_handler };
    httpd_register_uri_handler(httpd, &r_index);
    httpd_register_uri_handler(httpd, &r_pmkid);
    httpd_register_uri_handler(httpd, &r_hshk);
    httpd_register_uri_handler(httpd, &r_entry);

    running = true;
    ESP_LOGI(TAG, "Captures HTTP server up: SSID=%s IP=%s",
             CAPTURES_HTTP_SSID, CAPTURES_HTTP_IP);
}

void captures_http_stop(void) {
    if (!running) return;

    if (httpd) {
        httpd_stop(httpd);
        httpd = NULL;
    }

    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                 on_client_connect);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                 on_client_disconnect);

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    client_count = 0;
    running      = false;
    ESP_LOGI(TAG, "Captures HTTP server stopped");
}

bool captures_http_is_running(void)       { return running; }
uint8_t captures_http_get_client_count(void) { return client_count; }

bool captures_http_consume_change(void) {
    bool v = state_changed;
    state_changed = false;
    return v;
}
