#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_CONNECT_IDLE = 0,
    WIFI_CONNECT_CONNECTING = 1,
    WIFI_CONNECT_CONNECTED = 2,
    WIFI_CONNECT_FAILED = 3,
} wifi_connect_state_t;

void wifi_sta_init(void);
void wifi_sta_enter(void);
void wifi_sta_scroll_up(void);
void wifi_sta_scroll_down(void);
void wifi_sta_select(void);         // picker → password entry or direct connect (open)
void wifi_sta_char_next(void);
void wifi_sta_char_prev(void);
void wifi_sta_char_append(void);    // add char; [OK]=connect, [DEL]=backspace
void wifi_sta_pw_cancel(void);      // back to picker from password entry
void wifi_sta_stop(void);
void wifi_sta_render(void);

bool wifi_sta_is_connected(void);
bool wifi_sta_is_connecting(void);
bool wifi_sta_is_in_picker(void);
bool wifi_sta_is_in_password(void);
bool wifi_sta_needs_refresh(void);  // true once after async state change
wifi_connect_state_t wifi_sta_get_state(void);
const char *wifi_sta_get_ip(void);

#endif
