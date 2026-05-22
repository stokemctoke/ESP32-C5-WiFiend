#include "game_life.h"
#include "ssd1306.h"
#include "neopixel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

// Conway's Game of Life on a toroidal (wrapping) grid of 2px cells.
// The user dials a seed on the entry screen; the seed drives a deterministic
// xorshift PRNG, so the same number always reproduces the same starting soup.

#define COLS        64      // 128 / 2
#define ROWS        32      // 64 / 2
#define CELL        2
#define STEP_MS     150     // ~6-7 generations/sec
#define INIT_FILL   34      // % of cells alive at start

typedef enum { LIFE_SEED, LIFE_RUN } life_state_t;

static TaskHandle_t  s_task    = NULL;
static volatile bool s_running = false;        // sim task alive
static volatile life_state_t s_state = LIFE_SEED;

static uint8_t grid[ROWS][COLS];
static uint8_t next_grid[ROWS][COLS];
static uint8_t seed_d[4] = {0, 0, 0, 1};   // thousands, hundreds, tens, units
static int     edit_field = 0;             // 0..3 = digits, 4 = START
static uint32_t gen;
static uint32_t rng;

static int seed_value(void) {
    return seed_d[0] * 1000 + seed_d[1] * 100 + seed_d[2] * 10 + seed_d[3];
}

static uint32_t xrand(void) {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return x;
}

// Colour wheel for the NeoPixel rainbow cycle while the sim runs.
static pixel_color_t wheel(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85)  return MAKE_COLOR(255 - pos * 3, 0, pos * 3);
    if (pos < 170) { pos -= 85;  return MAKE_COLOR(0, pos * 3, 255 - pos * 3); }
    pos -= 170;    return MAKE_COLOR(pos * 3, 255 - pos * 3, 0);
}

static void seed_grid(int seed) {
    rng = (uint32_t)seed * 2654435761u + 1u;   // spread the seed, avoid 0
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            grid[r][c] = (xrand() % 100) < INIT_FILL ? 1 : 0;
    gen = 0;
}

static int population(void) {
    int n = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            n += grid[r][c];
    return n;
}

static void step(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (!dr && !dc) continue;
                    int rr = (r + dr + ROWS) % ROWS;
                    int cc = (c + dc + COLS) % COLS;
                    n += grid[rr][cc];
                }
            }
            next_grid[r][c] = (grid[r][c]) ? (n == 2 || n == 3) : (n == 3);
        }
    }
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            grid[r][c] = next_grid[r][c];
    gen++;
}

static void render_sim(void) {
    ssd1306_clear_buffer();
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (grid[r][c]) ssd1306_fill_rect(c * CELL, r * CELL, CELL, CELL);
    ssd1306_flush();
}

static void render_seed(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("Game of Life", "");

    // Four digits, one highlighted as the active field
    const uint8_t base_x = 34, spacing = 14;
    for (int i = 0; i < 4; i++) {
        uint8_t x = base_x + i * spacing;
        ssd1306_draw_char(x, 3, '0' + seed_d[i]);
        if (edit_field == i) ssd1306_invert_rect(x - 1, 24, 10, 8);
    }

    // START field
    ssd1306_draw_string(44, 5, "START");
    if (edit_field == 4) ssd1306_invert_rect(42, 40, 44, 8);

    ssd1306_draw_string(0, 7, "Rot=set Click=next");
    ssd1306_flush();
}

static void life_task(void *arg) {
    uint8_t hue = 0;
    while (s_running && s_state == LIFE_RUN) {
        neopixel_set_color(wheel(hue));
        hue += 5;                       // smooth rainbow cycle (~7.5s per loop)
        render_sim();
        step();
        // Self-sustaining: if the soup dies out, reseed from the running PRNG
        // so the toy keeps producing patterns instead of going black forever.
        if (population() == 0) seed_grid((int)(xrand() & 0x3FF));
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static void start_sim(void) {
    seed_grid(seed_value());
    s_state   = LIFE_RUN;
    s_running = true;
    xTaskCreate(life_task, "life", 4096, NULL, 4, &s_task);
}

void game_life_enter(void) {
    s_state = LIFE_SEED;
    edit_field = 0;
    render_seed();
}

void game_life_stop(void) {
    s_running = false;   // task observes and self-deletes
    s_state   = LIFE_SEED;
}

void game_life_input(encoder_event_t e) {
    if (s_state == LIFE_SEED) {
        switch (e) {
            case ENCODER_CW:
                if (edit_field < 4) seed_d[edit_field] = (seed_d[edit_field] + 1) % 10;
                render_seed();
                break;
            case ENCODER_CCW:
                if (edit_field < 4) seed_d[edit_field] = (seed_d[edit_field] + 9) % 10;
                render_seed();
                break;
            case ENCODER_CLICK:
                if (edit_field < 4) { edit_field++; render_seed(); }   // advance to next field
                else                  start_sim();                      // START
                break;
            default: break;
        }
    } else {  // LIFE_RUN
        if (e == ENCODER_CLICK) {     // back to the seed screen to re-roll
            s_running  = false;
            s_state    = LIFE_SEED;
            edit_field = 0;
            neopixel_set_color(COLOR_MAGENTA);   // stop the rainbow on the seed screen
            render_seed();
        }
    }
}

bool game_life_is_running(void) {
    return s_running || s_state == LIFE_SEED;
}
