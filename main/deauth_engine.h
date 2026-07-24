#ifndef DEAUTH_ENGINE_H
#define DEAUTH_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

// Shared 802.11 deauth TX core (ported from WiFuxx attack loop).

#define DEAUTH_MAX_TARGETS 20

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
} deauth_target_t;

void deauth_engine_init(void);

// Select management frame subtype for start/burst variants (deauth 0xC0 vs disassoc 0xA0).
void deauth_engine_set_mode(bool disassoc);

// Continuous multi-target / dual-band attack (promiscuous on while running).
void deauth_engine_start(const deauth_target_t *targets, uint16_t count);

// Continuous attack on one AP, alternating broadcast + client-directed frames.
void deauth_engine_start_targeted(const deauth_target_t *ap, const uint8_t client_mac[6]);

// Continuous probe-request flood on one channel (uses STA MAC as SA).
void deauth_engine_start_probe_flood(const deauth_target_t *ap, const char *ssid);

void deauth_engine_stop(void);
bool     deauth_engine_is_running(void);
uint32_t deauth_engine_get_frames(void);
uint32_t deauth_engine_get_pps(void);

// Blocking burst for handshake flush etc. Does not toggle promiscuous mode.
// If client_mac is non-NULL, alternates broadcast and client-directed frames.
// Respects deauth_engine_set_mode() for frame subtype.
uint16_t deauth_engine_burst(const uint8_t bssid[6], uint8_t channel,
                             const uint8_t *client_mac, uint16_t frame_count,
                             uint16_t gap_ms);

// Blocking probe-request burst via esp_wifi_80211_tx. Does not toggle promiscuous.
uint16_t deauth_engine_probe_flood(const uint8_t *src_mac, const char *ssid,
                                   uint8_t channel, uint16_t count,
                                   uint16_t gap_ms);

#endif
