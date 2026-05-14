#include "menu.h"
#include "ssd1306.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "menu";

#define MENU_STACK_DEPTH 4

typedef struct {
    menu_item_t *items;
    uint16_t count;
    uint16_t current_index;
} menu_level_t;

static menu_level_t menu_stack[MENU_STACK_DEPTH];
static uint8_t stack_depth = 0;

static menu_level_t* current_menu(void) {
    if (stack_depth == 0) return NULL;
    return &menu_stack[stack_depth - 1];
}

void menu_init(menu_item_t *items, uint16_t count) {
    memset(menu_stack, 0, sizeof(menu_stack));
    stack_depth = 1;
    menu_stack[0].items = items;
    menu_stack[0].count = count;
    menu_stack[0].current_index = 0;
    ESP_LOGI(TAG, "Menu stack initialized (main menu: %d items)", count);
}

void menu_navigate_up(void) {
    menu_level_t *menu = current_menu();
    if (!menu) return;

    if (menu->current_index > 0) {
        menu->current_index--;
    } else {
        menu->current_index = menu->count - 1;
    }
}

void menu_navigate_down(void) {
    menu_level_t *menu = current_menu();
    if (!menu) return;

    if (menu->current_index < menu->count - 1) {
        menu->current_index++;
    } else {
        menu->current_index = 0;
    }
}

void menu_push_submenu(menu_item_t *items, uint16_t count) {
    if (stack_depth >= MENU_STACK_DEPTH) {
        ESP_LOGW(TAG, "Menu stack overflow");
        return;
    }

    stack_depth++;
    menu_stack[stack_depth - 1].items = items;
    menu_stack[stack_depth - 1].count = count;
    menu_stack[stack_depth - 1].current_index = 0;
    ESP_LOGD(TAG, "Pushed submenu (depth: %d)", stack_depth);
}

void menu_pop(void) {
    if (stack_depth > 1) {
        stack_depth--;
        ESP_LOGD(TAG, "Popped menu (depth: %d)", stack_depth);
    }
}

void menu_select_current(void) {
    menu_level_t *menu = current_menu();
    if (!menu || menu->current_index >= menu->count) return;

    menu_item_t *item = &menu->items[menu->current_index];
    if (item->on_select) {
        item->on_select();
    }
}

void menu_render(void) {
    menu_level_t *menu = current_menu();
    if (!menu) return;

    ssd1306_clear_buffer();
    ssd1306_draw_header("WiFiend", stack_depth > 1 ? "< Long-press=Back" : "");

    uint16_t visible_items = (menu->count < 6) ? menu->count : 6;
    uint16_t start_index = 0;

    if (menu->current_index >= 6) {
        start_index = menu->current_index - 5;
    }

    for (uint16_t i = 0; i < visible_items; i++) {
        uint16_t item_idx = start_index + i;
        const char *prefix = (item_idx == menu->current_index) ? "> " : "  ";
        char line[20];
        snprintf(line, sizeof(line), "%s%s", prefix, menu->items[item_idx].label);
        ssd1306_draw_string(0, i + 2, line);
    }

    ssd1306_flush();
}
