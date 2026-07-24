#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <stdint.h>
#include <stdbool.h>

#define CP_MAX_CAPTURES 8
#define CP_PASSWORD_MAX 64
#define CP_DNS_LOG_MAX  8
#define CP_DNS_HOST_MAX 32

typedef struct {
    char     password[CP_PASSWORD_MAX];
    int64_t  captured_at_us;
} cp_capture_t;

void captive_portal_start(const char *ssid);
void captive_portal_stop(void);
uint8_t captive_portal_get_count(void);
const cp_capture_t *captive_portal_get_latest(void);   // NULL if no captures

uint8_t     captive_portal_get_dns_log_count(void);
const char *captive_portal_get_dns_log(uint8_t idx);   // 0 = newest, NULL if invalid

#endif
