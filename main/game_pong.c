#include "game_pong.h"
#include "ssd1306.h"
#include "hiscore.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <stdio.h>

// Player paddle (left) is encoder-controlled; right paddle is a beatable AI.
// Structure: a GAME is first to 5 points; a ROUND is best-of-3 games. You bank
// 5 points per game won and a 10-point bonus per round won. Points accumulate
// across an endless run; each round won makes the CPU start tougher. Losing a
// round (CPU takes 2 games) ends the run. Total score goes on the top-10 board.

#define SCREEN_W      128
#define FIELD_TOP      16
#define FIELD_BOTTOM   63
#define FRAME_MS_BASE  33
#define FRAME_MS_MIN   15
#define PADDLE_H       12
#define PADDLE_W        3
#define BALL_SZ         3
#define PLAYER_X        2
#define AI_X          (SCREEN_W - 2 - PADDLE_W)
#define PADDLE_STEP     5

#define GAME_POINTS     5    // points to win one game
#define GAMES_TO_WIN    2    // games to win a round (best of 3)
#define PTS_PER_GAME    5    // score awarded per game won
#define PTS_PER_ROUND  10    // bonus awarded per round won

typedef enum { PONG_PLAY, PONG_GAME, PONG_WON, PONG_OVER, PONG_NAME, PONG_TABLE } pong_mode_t;

static TaskHandle_t   s_task    = NULL;
static volatile bool  s_running = false;
static volatile pong_mode_t mode = PONG_PLAY;

static volatile int player_y;
static volatile int  letter_idx    = 0;
static volatile int  table_scroll  = 0;
static volatile bool click_pending = false;

static int ai_y;
static int ball_x, ball_y;
static int ball_dx, ball_dy;
static int score_p, score_ai;          // points in the current game
static int player_games, cpu_games;    // games won this round
static int rounds_won;                 // rounds won this run (drives difficulty)
static int score;                      // total accumulated points (the hi-score)
static bool game_over;                 // current game finished
static int64_t game_start_us;

static hiscore_entry_t table[HS_COUNT];

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void reset_ball(int dir) {
    ball_x  = SCREEN_W / 2;
    ball_y  = (FIELD_TOP + FIELD_BOTTOM) / 2;
    ball_dx = dir * 2;
    ball_dy = (esp_random() & 1) ? 1 : -1;
}

static void new_game(void) {           // one game within the current round
    player_y  = (FIELD_TOP + FIELD_BOTTOM) / 2 - PADDLE_H / 2;
    ai_y      = player_y;
    score_p   = 0;
    score_ai  = 0;
    game_over = false;
    game_start_us = esp_timer_get_time();
    reset_ball((esp_random() & 1) ? 1 : -1);
}

static void new_round(void) {          // fresh best-of-3, keeps total score + streak
    player_games = 0;
    cpu_games    = 0;
    new_game();
}

static void reset_run(void) {          // brand new run
    score      = 0;
    rounds_won = 0;
    new_round();
}

static void tick(void) {
    if (game_over) return;

    ball_x += ball_dx;
    ball_y += ball_dy;

    if (ball_y <= FIELD_TOP) { ball_y = FIELD_TOP; ball_dy = -ball_dy; }
    if (ball_y >= FIELD_BOTTOM - BALL_SZ) { ball_y = FIELD_BOTTOM - BALL_SZ; ball_dy = -ball_dy; }

    // Player paddle (left)
    if (ball_dx < 0 && ball_x <= PLAYER_X + PADDLE_W) {
        if (ball_y + BALL_SZ >= player_y && ball_y <= player_y + PADDLE_H) {
            ball_x  = PLAYER_X + PADDLE_W;
            ball_dx = -ball_dx;
            int off = (ball_y + BALL_SZ / 2) - (player_y + PADDLE_H / 2);
            ball_dy = clampi(ball_dy + off / 4, -3, 3);
            if (ball_dy == 0) ball_dy = 1;
        } else if (ball_x <= PLAYER_X) {
            score_ai++;
            if (score_ai >= GAME_POINTS) game_over = true;
            else reset_ball(1);
        }
    }

    // AI paddle (right)
    if (ball_dx > 0 && ball_x + BALL_SZ >= AI_X) {
        if (ball_y + BALL_SZ >= ai_y && ball_y <= ai_y + PADDLE_H) {
            ball_x  = AI_X - BALL_SZ;
            ball_dx = -ball_dx;
        } else if (ball_x + BALL_SZ >= AI_X + PADDLE_W) {
            score_p++;
            if (score_p >= GAME_POINTS) game_over = true;
            else reset_ball(-1);
        }
    }

    // Difficulty: within-game points + a bonus for rounds already won, so the
    // CPU starts clumsy in round 1 and sharpens the deeper you climb.
    int prog = score_p + score_ai + rounds_won * 3;
    int ai_speed = 1 + prog / 6;
    if (ai_speed > 3) ai_speed = 3;
    int tol = 12 - prog;
    if (tol < 1) tol = 1;

    int ai_center   = ai_y + PADDLE_H / 2;
    int ball_center = ball_y + BALL_SZ / 2;
    if (ai_center < ball_center - tol)      ai_y += ai_speed;
    else if (ai_center > ball_center + tol) ai_y -= ai_speed;
    ai_y = clampi(ai_y, FIELD_TOP, FIELD_BOTTOM - PADDLE_H);
}

static void render_game(void) {
    ssd1306_clear_buffer();
    char s[17];
    snprintf(s, sizeof(s), "Pts %d-%d", score_p, score_ai);
    ssd1306_draw_string(0, 0, s);
    snprintf(s, sizeof(s), "Gm %d-%d", player_games, cpu_games);
    ssd1306_draw_string(72, 0, s);
    ssd1306_hline(0, FIELD_TOP - 2, SCREEN_W);

    for (int y = FIELD_TOP; y < FIELD_BOTTOM; y += 6)
        ssd1306_fill_rect(SCREEN_W / 2, y, 1, 3);

    ssd1306_fill_rect(PLAYER_X, player_y, PADDLE_W, PADDLE_H);
    ssd1306_fill_rect(AI_X,     ai_y,     PADDLE_W, PADDLE_H);
    ssd1306_fill_rect(ball_x,   ball_y,   BALL_SZ,  BALL_SZ);
    ssd1306_flush();
}

static void render_game_result(bool player_won) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("Pong", "");
    ssd1306_draw_string(20, 3, player_won ? "GAME TO YOU" : "GAME TO CPU");
    char l[17];
    snprintf(l, sizeof(l), "Games %d-%d  S%d", player_games, cpu_games, score);
    ssd1306_draw_string(0, 5, l);
    ssd1306_draw_string(0, 7, "Click: next game");
    ssd1306_flush();
}

static void render_round_won(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("Pong", "");
    ssd1306_draw_string(20, 2, "ROUND WON!");
    char l[17];
    snprintf(l, sizeof(l), "+%d bonus", PTS_PER_ROUND);
    ssd1306_draw_string(36, 4, l);
    snprintf(l, sizeof(l), "Score %d  Rd %d", score, rounds_won);
    ssd1306_draw_string(0, 6, l);
    ssd1306_draw_string(0, 7, "Click: next round");
    ssd1306_flush();
}

static void render_over(void) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("Pong", "GAME OVER");
    char l[17];
    snprintf(l, sizeof(l), "Score: %d", score);
    ssd1306_draw_string(28, 3, l);
    snprintf(l, sizeof(l), "Rounds won: %d", rounds_won);
    ssd1306_draw_string(8, 5, l);
    ssd1306_draw_string(0, 7, "Click: scores");
    ssd1306_flush();
}

static void render_name(const char *name, int slot) {
    ssd1306_clear_buffer();
    ssd1306_draw_header("Pong", "NEW HISCORE");
    char l[17];
    snprintf(l, sizeof(l), "Score: %d", score);
    ssd1306_draw_string(0, 2, l);
    const uint8_t base_x = 46, spacing = 12;
    for (int i = 0; i < 3; i++) {
        ssd1306_draw_char(base_x + i * spacing, 4, name[i]);
        if (i == slot) ssd1306_invert_rect(base_x + i * spacing - 1, 32, 10, 8);
    }
    ssd1306_draw_string(0, 7, "Rot=ltr Click=next");
    ssd1306_flush();
}

static void wait_click(void) {
    click_pending = false;
    while (s_running && !click_pending) vTaskDelay(pdMS_TO_TICKS(20));
    click_pending = false;
}

static bool name_entry(char out[HS_NAME_LEN]) {
    mode = PONG_NAME;
    char name[3] = { 'A', 'A', 'A' };
    int slot = 0;
    letter_idx = 0;
    click_pending = false;
    while (s_running && slot < 3) {
        name[slot] = 'A' + letter_idx;
        render_name(name, slot);
        if (click_pending) { click_pending = false; slot++; letter_idx = 0; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_running) return false;
    out[0] = name[0]; out[1] = name[1]; out[2] = name[2]; out[3] = '\0';
    return true;
}

static void run_game(void) {
    mode = PONG_PLAY;
    while (s_running && !game_over) {
        tick();
        render_game();
        int64_t es = (esp_timer_get_time() - game_start_us) / 1000000;
        int delay = FRAME_MS_BASE - (int)es;
        if (delay < FRAME_MS_MIN) delay = FRAME_MS_MIN;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static void show_table(void) {
    mode = PONG_TABLE;
    table_scroll = 0;
    click_pending = false;
    while (s_running && !click_pending) {
        hiscore_render_table("Pong Hi", table, table_scroll, "Click: PlayAgain");
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    click_pending = false;
}

static void pong_task(void *arg) {
    reset_run();
    while (s_running) {
        run_game();
        if (!s_running) break;

        bool player_won_game = (score_p >= GAME_POINTS);
        if (player_won_game) { score += PTS_PER_GAME; player_games++; }
        else                   cpu_games++;

        if (player_games >= GAMES_TO_WIN) {           // round won
            score += PTS_PER_ROUND;
            rounds_won++;
            mode = PONG_WON;
            render_round_won();
            wait_click();
            if (!s_running) break;
            new_round();
            continue;
        }

        if (cpu_games >= GAMES_TO_WIN) {              // round lost -> run over
            int rank = hiscore_rank(table, score);
            if (rank >= 0) {
                char name[HS_NAME_LEN];
                if (!name_entry(name)) break;
                hiscore_insert(table, rank, name, score);
                hiscore_save("pong", table);
            } else {
                mode = PONG_OVER;
                render_over();
                wait_click();
                if (!s_running) break;
            }
            show_table();
            if (!s_running) break;
            reset_run();
            continue;
        }

        // round still going — show the game result, then next game
        mode = PONG_GAME;
        render_game_result(player_won_game);
        wait_click();
        if (!s_running) break;
        new_game();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void game_pong_enter(void) {
    if (s_running) return;
    hiscore_load("pong", table);
    mode = PONG_PLAY;
    reset_run();
    s_running = true;
    xTaskCreate(pong_task, "pong", 4096, NULL, 4, &s_task);
}

void game_pong_stop(void) {
    s_running = false;
}

void game_pong_input(encoder_event_t e) {
    switch (mode) {
        case PONG_PLAY:
            if      (e == ENCODER_CW)  player_y = clampi(player_y + PADDLE_STEP, FIELD_TOP, FIELD_BOTTOM - PADDLE_H);
            else if (e == ENCODER_CCW) player_y = clampi(player_y - PADDLE_STEP, FIELD_TOP, FIELD_BOTTOM - PADDLE_H);
            break;
        case PONG_NAME:
            if      (e == ENCODER_CW)    letter_idx = (letter_idx + 1) % 26;
            else if (e == ENCODER_CCW)   letter_idx = (letter_idx + 25) % 26;
            else if (e == ENCODER_CLICK) click_pending = true;
            break;
        case PONG_TABLE:
            if      (e == ENCODER_CW)    { if (table_scroll < HS_COUNT - 5) table_scroll++; }
            else if (e == ENCODER_CCW)   { if (table_scroll > 0)            table_scroll--; }
            else if (e == ENCODER_CLICK) click_pending = true;
            break;
        default:   // PONG_GAME, PONG_WON, PONG_OVER
            if (e == ENCODER_CLICK) click_pending = true;
            break;
    }
}

bool game_pong_is_running(void) {
    return s_running;
}
