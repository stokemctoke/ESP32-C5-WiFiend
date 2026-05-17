#ifndef CAPTURES_HTTP_H
#define CAPTURES_HTTP_H

#include <stdint.h>
#include <stdbool.h>

#define CAPTURES_HTTP_SSID "WiFiend-Files"
#define CAPTURES_HTTP_IP   "192.168.4.1"

void    captures_http_start(void);
void    captures_http_stop(void);
bool    captures_http_is_running(void);
uint8_t captures_http_get_client_count(void);

// Returns true if the server state (e.g. client count) changed since the last
// call. Clears the flag. Used by the OLED render loop to refresh the screen.
bool    captures_http_consume_change(void);

#endif
