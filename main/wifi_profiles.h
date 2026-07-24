#ifndef WIFI_PROFILES_H
#define WIFI_PROFILES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIFI_PROFILES_MAX 5
#define WIFI_PROFILE_SSID_LEN 32
#define WIFI_PROFILE_PW_LEN   64

typedef struct {
    char ssid[WIFI_PROFILE_SSID_LEN + 1];
    char password[WIFI_PROFILE_PW_LEN + 1];
} wifi_profile_t;

void wifi_profiles_init(void);

// Save profile at index (0..WIFI_PROFILES_MAX-1). Returns false if index invalid or NVS error.
bool wifi_profiles_save(uint8_t index, const char *ssid, const char *password);

// Load profile at index. Returns false if slot empty or index invalid.
bool wifi_profiles_load(uint8_t index, wifi_profile_t *out);

// Number of saved (non-empty) profiles.
uint8_t wifi_profiles_count(void);

// Clear a slot. Returns false if index invalid.
bool wifi_profiles_delete(uint8_t index);

// Copy SSID for index into buf. Returns false if slot empty.
bool wifi_profiles_get_ssid(uint8_t index, char *buf, size_t buf_len);

#endif
