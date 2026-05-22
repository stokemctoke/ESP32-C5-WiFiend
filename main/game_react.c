#include "game_react.h"
#include "ssd1306.h"
#include "neopixel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "hiscore.h"
#include <stdio.h>
#include <string.h>

// Reaction test: the NeoPixel flashes one of three colours; the player scrolls
// to the matching colour WORD (positions shuffled each round) and clicks within
// the level's time window. Faster correct clicks earn more of the 100 points.
// 3 lives; window tightens 0.1s per level. Top-10 hi-scores persist in NVS.

#define COLORS      3
#define WINDOW_MS   5000
#define WINDOW_MIN  400        // window floor so it stays humanly possible
#define MAX_PTS     100
#define START_LIVES 3

typedef enum { MODE_PLAY, MODE_NAME, MODE_TABLE } react_mode_t;

static TaskHandle_t  s_task    = NULL;
static volatile bool s_running = false;

static volatile react_mode_t mode      = MODE_PLAY;
static volatile int     cursor        = 1;     // play: slot 0/1/2
static volatile int     letter_idx    = 0;     // name entry: 0..25 (A..Z)
static volatile int     table_scroll  = 0;
static volatile bool    click_pending = false;
static volatile int64_t click_us      = 0;

static int score, level, lives;

static const char  *names[COLORS]  = { "RED", "GREEN", "BLUE" };
static const uint8_t word_x[COLORS] = { 2, 46, 88 };   // fits widest word at the right slot
static int slot_color[COLORS];                          // which colour shows at each slot

static hiscore_entry_t table[HS_COUNT];

static pixel_color_t color_of(int i) {
    if (i == 0) return COLOR_RED;
    if (i == 1) return COLOR_GREEN;
    return COLOR_BLUE;
}

static int level_window(void) {
    int w = WINDOW_MS - (level - 1) * 100;
    return (w < WINDOW_MIN) ? WINDOW_MIN : w;
}

static void shuffle_slots(void) {
    for (int i = 0; i < COLORS; i++) slot_color[i] = i;
    for (int i = COLORS - 1; i > 0; i--) {
        int j = (int)(esp_random() % (i + 1));
        int t = slot_color[i]; slot_color[i] = slot_color[j]; slot_color[j] = t;
    }
}

// ---------- rendering ----------

static void draw_top(void) {
    char hdr[12];
    snprintf(hdr, sizeof(hdr), "%d", score);
    ssd1306_draw_header("React", hdr);
}

static void render_ready(void) {
    ssd1306_clear_buffer();
    draw_top();
    ssd1306_draw_string(0, 4, "  Get ready...");
    ssd1306_flush();
}

static void render_play(int remain_ms, int window_ms) {
    ssd1306_clear_buffer();
    draw_top();
    char info[17];
    snprintf(info, sizeof(info), "Lvl %d  Lives %d", level, lives);
    ssd1306_draw_string(0, 2, info);
    for (int i = 0; i < COLORS; i++)
        ssd1306_draw_string(word_x[i], 4, names[slot_color[i]]);
    int len = (int)strlen(names[slot_color[cursor]]) * 8;
    ssd1306_invert_rect(word_x[cursor] - 2, 30, len + 4, 12);
    int bar = 128 * remain_ms / window_ms;
    if (bar < 0) bar = 0;
    ssd1306_fill_rect(0, 60, bar, 3);
    ssd1306_flush();
}

static void render_result(int pts, const char *res) {
    ssd1306_clear_buffer();
    draw_top();
    char info[17];
    snprintf(info, sizeof(info), "Lvl %d  Lives %d", level, lives);
    ssd1306_draw_string(0, 2, info);
    ssd1306_draw_string(40, 4, res);
    char line[16];
    if (pts > 0) snprintf(line, sizeof(line), "+%d pts", pts);
    else         snprintf(line, sizeof(line), "lost a life");
    ssd1306_draw_string(28, 6, line);
    ssd1306_flush();
}

static void render_name(const char *name, int slot) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("React", "NEW HISCORE");
    char line[17];
    snprintf(line, sizeof(line), "Score: %d", score);
    ssd1306_draw_string(0, 2, line);
    const uint8_t base_x = 46, spacing = 12;
    for (int i = 0; i < 3; i++) {
        ssd1306_draw_char(base_x + i * spacing, 4, name[i]);
        if (i == slot) ssd1306_invert_rect(base_x + i * spacing - 1, 32, 10, 8);
    }
    ssd1306_draw_string(0, 7, "Rot=ltr Click=next");
    ssd1306_flush();
}

// ---------- helpers ----------

static void sleep_ms(int ms) {     // chunked so the game stops promptly on exit
    for (int t = 0; t < ms && s_running; t += 20)
        vTaskDelay(pdMS_TO_TICKS(20));
}

// Arcade 3-letter name entry. Returns false if interrupted by exit.
static bool name_entry(char out[HS_NAME_LEN]) {
    mode = MODE_NAME;
    char name[3] = { 'A', 'A', 'A' };
    int slot = 0;
    letter_idx = 0;
    click_pending = false;
    while (s_running && slot < 3) {
        name[slot] = 'A' + letter_idx;
        render_name(name, slot);
        if (click_pending) {
            click_pending = false;
            slot++;
            letter_idx = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_running) return false;
    out[0] = name[0]; out[1] = name[1]; out[2] = name[2]; out[3] = '\0';
    return true;
}

// ---------- main task ----------

static void react_task(void *arg) {
    score = 0; level = 1; lives = START_LIVES;
    while (s_running) {
        int window = level_window();

        mode = MODE_PLAY;
        neopixel_set_color(COLOR_OFF);
        render_ready();
        sleep_ms(700 + (int)(esp_random() % 900));   // random anti-anticipation delay
        if (!s_running) break;

        shuffle_slots();
        int target = (int)(esp_random() % COLORS);    // colour the LED will show
        cursor        = 1;
        click_pending = false;
        neopixel_set_color(color_of(target));
        int64_t t0 = esp_timer_get_time();

        int pts = 0;
        const char *res = "TOO SLOW";
        bool done = false;
        while (s_running && !done) {
            int elapsed = (int)((esp_timer_get_time() - t0) / 1000);
            if (click_pending) {
                click_pending = false;
                int e = (int)((click_us - t0) / 1000);
                if (e < 0) e = 0;
                if (slot_color[cursor] != target) { pts = 0; res = "WRONG"; }
                else if (e > window)               { pts = 0; res = "TOO SLOW"; }
                else { pts = (window - e) * MAX_PTS / window; if (pts < 1) pts = 1; res = "HIT!"; }
                done = true;
                break;
            }
            if (elapsed >= window) { pts = 0; res = "TOO SLOW"; break; }
            render_play(window - elapsed, window);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_running) break;

        score += pts;
        if (pts == 0) lives--;
        render_result(pts, res);
        neopixel_set_color(pts > 0 ? color_of(target) : COLOR_RED);
        sleep_ms(1200);

        if (lives <= 0) {
            neopixel_set_color(COLOR_RED);
            int rank = hiscore_rank(table, score);
            if (rank >= 0) {
                char name[HS_NAME_LEN];
                if (!name_entry(name)) break;          // exited mid-entry
                hiscore_insert(table, rank, name, score);
                hiscore_save("react", table);
            }
            mode = MODE_TABLE;
            table_scroll = 0;
            click_pending = false;
            while (s_running && !click_pending) {
                hiscore_render_table("Hi-Scores", table, table_scroll, "Click: PlayAgain");
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            click_pending = false;
            if (!s_running) break;
            score = 0; level = 1; lives = START_LIVES;  // restart
            continue;
        }
        level++;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---------- public API ----------

void game_react_enter(void) {
    if (s_running) return;
    hiscore_load("react", table);
    mode = MODE_PLAY;
    s_running = true;
    xTaskCreate(react_task, "react", 4096, NULL, 4, &s_task);
}

void game_react_stop(void) {
    s_running = false;
}

void game_react_input(encoder_event_t e) {
    if (mode == MODE_NAME) {
        switch (e) {
            case ENCODER_CW:    letter_idx = (letter_idx + 1) % 26; break;
            case ENCODER_CCW:   letter_idx = (letter_idx + 25) % 26; break;
            case ENCODER_CLICK: click_pending = true; break;
            default: break;
        }
        return;
    }
    if (mode == MODE_TABLE) {
        switch (e) {
            case ENCODER_CW:    if (table_scroll < HS_COUNT - 5) table_scroll++; break;
            case ENCODER_CCW:   if (table_scroll > 0)            table_scroll--; break;
            case ENCODER_CLICK: click_pending = true; break;
            default: break;
        }
        return;
    }
    // MODE_PLAY
    switch (e) {
        case ENCODER_CW:    if (cursor < COLORS - 1) cursor++; break;
        case ENCODER_CCW:   if (cursor > 0)          cursor--; break;
        case ENCODER_CLICK: click_us = esp_timer_get_time(); click_pending = true; break;
        default: break;
    }
}

bool game_react_is_running(void) {
    return s_running;
}
