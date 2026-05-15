#ifndef WIFI_CAPTURES_H
#define WIFI_CAPTURES_H

#include <stdint.h>
#include <stdbool.h>

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

#endif
