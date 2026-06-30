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

// Continuous multi-target / dual-band attack (promiscuous on while running).
void deauth_engine_start(const deauth_target_t *targets, uint16_t count);
void deauth_engine_stop(void);
bool     deauth_engine_is_running(void);
uint32_t deauth_engine_get_frames(void);
uint32_t deauth_engine_get_pps(void);

// Blocking burst for handshake flush etc. Does not toggle promiscuous mode.
// If client_mac is non-NULL, alternates broadcast and client-directed frames.
uint16_t deauth_engine_burst(const uint8_t bssid[6], uint8_t channel,
                             const uint8_t *client_mac, uint16_t frame_count,
                             uint16_t gap_ms);

#endif
