#ifndef WIFI_ATTACK_H
#define WIFI_ATTACK_H

#include <stdint.h>
#include <stdbool.h>

void wifi_attack_init(void);
void wifi_attack_enter(void);       // load AP list from last scan, enter picker
bool wifi_attack_has_targets(void);
void wifi_attack_scroll_up(void);
void wifi_attack_scroll_down(void);
void wifi_attack_select(void);      // confirm selected AP and start attack
void wifi_attack_stop(void);
bool wifi_attack_is_running(void);
void wifi_attack_render(void);      // renders picker or live attack stats

#endif
