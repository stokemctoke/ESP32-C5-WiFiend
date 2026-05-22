#ifndef HISCORE_H
#define HISCORE_H

// Reusable top-10 high-score table, persisted in NVS under a per-game "board"
// name. Survives reboots and reflashes. Names are arcade-style 3 letters.

#define HS_COUNT     10
#define HS_NAME_LEN  4     // 3 letters + NUL

typedef struct {
    char name[HS_NAME_LEN];
    int  score;
} hiscore_entry_t;

void hiscore_load(const char *board, hiscore_entry_t *table);   // fills HS_COUNT entries
void hiscore_save(const char *board, hiscore_entry_t *table);

// Rank (0-based) a score would occupy, or -1 if it doesn't make the table.
int  hiscore_rank(const hiscore_entry_t *table, int score);
void hiscore_insert(hiscore_entry_t *table, int rank, const char *name, int score);

// Draw a scrollable table (5 rows from `scroll`) with title + optional hint.
void hiscore_render_table(const char *title, const hiscore_entry_t *table,
                          int scroll, const char *hint);

#endif
