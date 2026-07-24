#include "fs_browser.h"
#include "ssd1306.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "fs_browser";

#define LFS_ROOT            "/lfs"
#define MAX_FS_ENTRIES      32
#define DETAIL_ACTION_COUNT 3

typedef enum {
    FS_VIEW_LIST = 0,
    FS_VIEW_DETAIL,
    FS_VIEW_DUMPING,
} fs_view_t;

typedef struct {
    char     name[24];
    char     path[48];
    int32_t  size;
} fs_entry_t;

static const char *detail_action_label[DETAIL_ACTION_COUNT] = {
    "Dump to Serial",
    "Delete this",
    "Back",
};

static fs_entry_t  entries[MAX_FS_ENTRIES];
static uint16_t    entry_count       = 0;
static uint16_t    selected_idx      = 0;
static uint16_t    scroll_offset     = 0;
static uint16_t    detail_entry_idx  = 0;
static uint8_t     detail_action_idx = 0;
static fs_view_t   view_state        = FS_VIEW_LIST;
static volatile bool active          = false;
static volatile bool refresh         = false;

static void load_files(void) {
    entry_count = 0;

    DIR *dir = opendir(LFS_ROOT);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", LFS_ROOT);
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && entry_count < MAX_FS_ENTRIES) {
        if (de->d_name[0] == '.') continue;

        char path[48];
        snprintf(path, sizeof(path), "%s/%.40s", LFS_ROOT, de->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        fs_entry_t *e = &entries[entry_count];
        strncpy(e->name, de->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        e->size = (int32_t)st.st_size;
        entry_count++;
    }
    closedir(dir);
}

static void dump_file(const fs_entry_t *e) {
    printf("\n===== %s (%ld bytes) =====\n", e->path, (long)e->size);
    FILE *fp = fopen(e->path, "r");
    if (!fp) {
        printf("(cannot open)\n");
        return;
    }

    char buf[256];
    int  lines = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        printf("%s", buf);
        if (buf[strlen(buf) - 1] != '\n') printf("\n");
        lines++;
    }
    fclose(fp);
    printf("===== %d line(s) =====\n\n", lines);
}

static void delete_file(const fs_entry_t *e) {
    if (unlink(e->path) == 0) {
        ESP_LOGI(TAG, "Deleted %s", e->path);
    } else {
        ESP_LOGW(TAG, "Delete failed: %s", e->path);
    }
}

static void format_size(int32_t size, char *out, size_t outsz) {
    if (size < 0) {
        snprintf(out, outsz, "?");
    } else if (size >= 1024) {
        snprintf(out, outsz, "%ldK", (long)(size / 1024));
    } else {
        snprintf(out, outsz, "%ld", (long)size);
    }
}

void fs_browser_init(void) {
    ESP_LOGI(TAG, "FS browser ready");
}

void fs_browser_enter(void) {
    load_files();
    selected_idx      = 0;
    scroll_offset     = 0;
    detail_action_idx = 0;
    view_state        = FS_VIEW_LIST;
    active            = true;
    refresh           = true;
}

void fs_browser_scroll_up(void) {
    if (view_state == FS_VIEW_LIST) {
        if (selected_idx > 0) {
            selected_idx--;
            if (selected_idx < scroll_offset) scroll_offset = selected_idx;
        }
    } else if (view_state == FS_VIEW_DETAIL) {
        if (detail_action_idx > 0) detail_action_idx--;
    }
    refresh = true;
}

void fs_browser_scroll_down(void) {
    if (view_state == FS_VIEW_LIST) {
        if (entry_count > 0 && selected_idx + 1 < entry_count) {
            selected_idx++;
            if (selected_idx >= scroll_offset + 6) scroll_offset = selected_idx - 5;
        }
    } else if (view_state == FS_VIEW_DETAIL) {
        if (detail_action_idx + 1 < DETAIL_ACTION_COUNT) detail_action_idx++;
    }
    refresh = true;
}

void fs_browser_select(void) {
    if (view_state == FS_VIEW_DETAIL) {
        fs_entry_t *e = &entries[detail_entry_idx];
        switch (detail_action_idx) {
            case 0:
                view_state = FS_VIEW_DUMPING;
                refresh    = true;
                fs_browser_render();
                dump_file(e);
                view_state = FS_VIEW_DETAIL;
                refresh    = true;
                break;
            case 1:
                delete_file(e);
                load_files();
                if (entry_count == 0) {
                    selected_idx = 0;
                } else if (selected_idx >= entry_count) {
                    selected_idx = entry_count - 1;
                }
                if (selected_idx < scroll_offset) scroll_offset = selected_idx;
                view_state = FS_VIEW_LIST;
                refresh    = true;
                break;
            case 2:
                view_state = FS_VIEW_LIST;
                refresh    = true;
                break;
        }
        return;
    }

    if (view_state != FS_VIEW_LIST || entry_count == 0) return;

    detail_entry_idx  = selected_idx;
    detail_action_idx = 0;
    view_state        = FS_VIEW_DETAIL;
    refresh           = true;
}

bool fs_browser_back(void) {
    if (view_state == FS_VIEW_DETAIL) {
        view_state = FS_VIEW_LIST;
        refresh    = true;
        return false;
    }
    return true;
}

void fs_browser_stop(void) {
    active     = false;
    view_state = FS_VIEW_LIST;
}

bool fs_browser_is_active(void) { return active; }

bool fs_browser_needs_refresh(void) {
    bool v = refresh;
    refresh = false;
    return v;
}

static void render_list(void) {
    ssd1306_clear_buffer();

    char status[16];
    snprintf(status, sizeof(status), "%u files", (unsigned)entry_count);
    ssd1306_draw_header("LFS FILES", status);

    if (entry_count == 0) {
        ssd1306_draw_string(0, 3, " (empty /lfs)");
        ssd1306_draw_string(0, 7, "LN>back");
        ssd1306_flush();
        return;
    }

    for (uint8_t row = 0; row < 6; row++) {
        uint16_t idx = scroll_offset + row;
        if (idx >= entry_count) break;

        fs_entry_t *e = &entries[idx];
        char sz[8];
        format_size(e->size, sz, sizeof(sz));
        char line[32];
        snprintf(line, sizeof(line), "%c%-8.8s %4s",
                 (idx == selected_idx) ? '>' : ' ',
                 e->name, sz);
        line[16] = '\0';
        ssd1306_draw_string(0, row + 2, line);
    }

    ssd1306_draw_string(0, 7, "CK>detail LN>bk");
    ssd1306_flush();
}

static void render_detail(void) {
    ssd1306_clear_buffer();
    fs_entry_t *e = &entries[detail_entry_idx];

    char status[16];
    snprintf(status, sizeof(status), "%u/%u",
             (unsigned)(detail_entry_idx + 1), (unsigned)entry_count);
    ssd1306_draw_header("FILE", status);

    char line[32];
    snprintf(line, sizeof(line), "%.16s", e->name);
    ssd1306_draw_string(0, 2, line);

    char sz[16];
    format_size(e->size, sz, sizeof(sz));
    snprintf(line, sizeof(line), "Size: %s bytes", sz);
    line[16] = '\0';
    ssd1306_draw_string(0, 3, line);

    for (uint8_t i = 0; i < DETAIL_ACTION_COUNT; i++) {
        snprintf(line, sizeof(line), "%c%s",
                 (i == detail_action_idx) ? '>' : ' ',
                 detail_action_label[i]);
        line[16] = '\0';
        ssd1306_draw_string(0, 4 + i, line);
    }

    ssd1306_draw_string(0, 7, "CK>act LN>back");
    ssd1306_flush();
}

static void render_dumping(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("LFS FILES", "DUMP");
    ssd1306_draw_string(0, 3, "Dumping to");
    ssd1306_draw_string(0, 4, "serial...");
    ssd1306_draw_string(0, 6, "Open monitor");
    ssd1306_draw_string(0, 7, "to copy lines");
    ssd1306_flush();
}

void fs_browser_render(void) {
    switch (view_state) {
        case FS_VIEW_LIST:   render_list();   break;
        case FS_VIEW_DETAIL: render_detail(); break;
        case FS_VIEW_DUMPING: render_dumping(); break;
    }
}
