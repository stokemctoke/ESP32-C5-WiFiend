#ifndef WIFI_CAPTURES_H
#define WIFI_CAPTURES_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char    type;        // 'P' = PMKID, 'H' = Handshake
    char    ssid[17];
    uint8_t bssid[6];
    uint8_t client_mac[6];
} cap_entry_t;

void wifi_captures_init(void);
void wifi_captures_enter(void);
void wifi_captures_scroll_up(void);
void wifi_captures_scroll_down(void);
void wifi_captures_select(void);
void wifi_captures_stop(void);
void wifi_captures_render(void);

// Returns true if the caller should exit the module (we were in the top-level
// list view), false if we just popped a sub-state and the module should stay
// open. Lets LONG_PRESS act as "back" from detail / confirm sheets.
bool wifi_captures_back(void);

bool wifi_captures_needs_refresh(void);
bool wifi_captures_is_active(void);

// Accessors for other modules (e.g. HTTP server) that need to iterate the
// in-memory parsed entries. Indices are 0..get_count()-1. Caller must not
// retain the pointer across a reload/select/delete operation.
uint16_t            wifi_captures_get_count(void);
const cap_entry_t  *wifi_captures_get_entry(uint16_t idx);
void                wifi_captures_reload(void);

// Returns the on-disk hashcat log path for the given entry type ('P' or 'H'),
// or NULL if unknown. Used by the HTTP server to stream raw file downloads.
const char         *wifi_captures_log_path(char type);

// Writes the full hashcat-22000 source line (including trailing newline) for
// the entry at idx into buf. Returns the number of bytes written (excluding
// the null terminator), or 0 if the index is invalid or the source line
// cannot be located. buf should be at least 768 bytes for full handshake
// lines.
#include <stddef.h>
size_t              wifi_captures_get_entry_line(uint16_t idx, char *buf, size_t bufsz);

#endif
