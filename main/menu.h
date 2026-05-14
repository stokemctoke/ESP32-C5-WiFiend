#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *label;
    void (*on_select)(void);
} menu_item_t;

void menu_init(menu_item_t *items, uint16_t count);
void menu_navigate_up(void);
void menu_navigate_down(void);
void menu_select_current(void);
void menu_push_submenu(menu_item_t *items, uint16_t count);
void menu_pop(void);
void menu_render(void);

#endif
