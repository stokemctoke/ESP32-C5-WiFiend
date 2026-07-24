#ifndef FS_BROWSER_H
#define FS_BROWSER_H

#include <stdint.h>
#include <stdbool.h>

void fs_browser_init(void);
void fs_browser_enter(void);
void fs_browser_scroll_up(void);
void fs_browser_scroll_down(void);
void fs_browser_select(void);
void fs_browser_stop(void);
void fs_browser_render(void);

bool fs_browser_back(void);
bool fs_browser_needs_refresh(void);
bool fs_browser_is_active(void);

#endif
