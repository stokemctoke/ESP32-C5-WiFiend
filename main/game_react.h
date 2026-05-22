#ifndef GAME_REACT_H
#define GAME_REACT_H

#include <stdbool.h>
#include "encoder.h"

void game_react_enter(void);                // start the reaction-test task
void game_react_stop(void);                 // signal the task to exit
void game_react_input(encoder_event_t e);   // move cursor / click
bool game_react_is_running(void);

#endif
