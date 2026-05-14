#ifndef ENCODER_H
#define ENCODER_H

typedef enum {
    ENCODER_CW,
    ENCODER_CCW,
    ENCODER_CLICK,
    ENCODER_LONG_PRESS,
} encoder_event_t;

typedef void (*encoder_callback_t)(encoder_event_t event);

void encoder_init(void);
void encoder_set_callback(encoder_callback_t cb);

#endif
