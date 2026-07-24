#ifndef ESPNOW_RECON_H
#define ESPNOW_RECON_H

#include <stdint.h>
#include <stdbool.h>

#define ESPNOW_RECON_MAX_PEERS 20

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t pkt_count;
    uint32_t last_seen_ms;
    bool     logged;
} espnow_peer_t;

void espnow_recon_init(void);
void espnow_recon_enter(void);
void espnow_recon_start(void);
void espnow_recon_stop(void);
void espnow_recon_scroll_up(void);
void espnow_recon_scroll_down(void);
void espnow_recon_render(void);

bool espnow_recon_is_running(void);

uint16_t espnow_recon_peer_count(void);
const espnow_peer_t *espnow_recon_get_peer(uint16_t idx);

#endif
