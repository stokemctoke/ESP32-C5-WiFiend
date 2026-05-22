#ifndef GAME_PONG_H
#define GAME_PONG_H

#include <stdbool.h>
#include "encoder.h"

void game_pong_enter(void);                 // reset state + start the game task
void game_pong_stop(void);                  // signal the task to exit
void game_pong_input(encoder_event_t e);    // feed encoder rotation / click
bool game_pong_is_running(void);

#endif
