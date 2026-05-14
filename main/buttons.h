#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BUTTON_UP = 0,
    BUTTON_DOWN = 1,
    BUTTON_CENTER = 2,
    BUTTON_MAX
} button_id_t;

typedef enum {
    BUTTON_PRESS = 0,
    BUTTON_RELEASE = 1
} button_event_t;

typedef void (*button_callback_t)(button_id_t btn, button_event_t event);

void buttons_init(void);
void buttons_set_callback(button_callback_t cb);
bool buttons_is_pressed(button_id_t btn);

#endif
