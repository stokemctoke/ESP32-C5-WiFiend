#include "hiscore.h"
#include "ssd1306.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

#define VISIBLE_ROWS 5

void hiscore_load(const char *board, hiscore_entry_t *table) {
    for (int i = 0; i < HS_COUNT; i++) { strcpy(table[i].name, "---"); table[i].score = 0; }
    nvs_handle_t h;
    if (nvs_open(board, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(hiscore_entry_t) * HS_COUNT;
        nvs_get_blob(h, "tbl", table, &sz);   // leaves defaults if key absent
        nvs_close(h);
    }
}

void hiscore_save(const char *board, hiscore_entry_t *table) {
    nvs_handle_t h;
    if (nvs_open(board, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "tbl", table, sizeof(hiscore_entry_t) * HS_COUNT);
        nvs_commit(h);
        nvs_close(h);
    }
}

int hiscore_rank(const hiscore_entry_t *table, int score) {
    if (score <= 0) return -1;
    for (int i = 0; i < HS_COUNT; i++)
        if (score > table[i].score) return i;
    return -1;
}

void hiscore_insert(hiscore_entry_t *table, int rank, const char *name, int score) {
    for (int i = HS_COUNT - 1; i > rank; i--) table[i] = table[i - 1];
    strncpy(table[rank].name, name, HS_NAME_LEN - 1);
    table[rank].name[HS_NAME_LEN - 1] = '\0';
    table[rank].score = score;
}

void hiscore_render_table(const char *title, const hiscore_entry_t *table,
                          int scroll, const char *hint) {
    if (scroll < 0) scroll = 0;
    if (scroll > HS_COUNT - VISIBLE_ROWS) scroll = HS_COUNT - VISIBLE_ROWS;

    ssd1306_clear_buffer();
    ssd1306_draw_header(title, "");
    for (int r = 0; r < VISIBLE_ROWS; r++) {
        int i = scroll + r;
        if (i >= HS_COUNT) break;
        char line[17];
        snprintf(line, sizeof(line), "%2d %-3s %6d", i + 1, table[i].name, table[i].score);
        ssd1306_draw_string(0, 2 + r, line);
    }
    if (hint) ssd1306_draw_string(0, 7, hint);
    ssd1306_flush();
}
