#ifndef GAME_LIFE_H
#define GAME_LIFE_H

#include <stdbool.h>
#include "encoder.h"

void game_life_enter(void);                 // show the seed-entry screen
void game_life_stop(void);                  // stop the sim task, return to idle
void game_life_input(encoder_event_t e);    // dial seed / start / re-roll
bool game_life_is_running(void);

#endif
