#include "stream_menu.h"

#include "ui_assets.h"
#include "../input/input_pad.h"

#include <stdio.h>

enum { ROW_RESUME, ROW_STATS, ROW_PACING, ROW_BITRATE, ROW_DISCONNECT, ROW_QUIT, ROW_COUNT };

#define BITRATE_STEP_KBPS 5000
#define BITRATE_MIN_KBPS 5000

#define COL_DIALOG 0xFF424242u
#define COL_TEXT 0xFFFFFFFFu
#define COL_HINT 0xB3FFFFFFu
#define COL_HIGHLIGHT 0x29FFFFFFu
#define COL_ACCENT 0xFFCE93D8u
#define COL_ON_ACCENT 0xFF303030u
#define COL_CONTROL 0x1FFFFFFFu
#define COL_WARNING 0xFFFFB74Du

#define MENU_W 640
#define ROW_H 58
#define TITLE_H 76
#define HINT_H 92
#define MENU_H (TITLE_H + ROW_COUNT * ROW_H + HINT_H)
#define MENU_X ((1920 - MENU_W) / 2)
#define MENU_Y ((1080 - MENU_H) / 2)
#define PAD_X 28
#define BOX_SIZE 26

void stream_menu_open(stream_menu_t *m, bool stats, bool pacing, int bitrate_kbps, int bitrate_max_kbps) {
    m->sel = ROW_RESUME;
    m->quit_armed = false;
    m->stats = stats;
    m->pacing = pacing;
    m->bitrate_kbps = bitrate_kbps;
    m->bitrate_opened_kbps = bitrate_kbps;
    m->bitrate_max_kbps = bitrate_max_kbps;
}

static void change_bitrate(stream_menu_t *m, int dir) {
    int b = m->bitrate_kbps + dir * BITRATE_STEP_KBPS;
    /* Back onto the grid, from a value set elsewhere in another step. */
    b -= b % BITRATE_STEP_KBPS;
    if (dir < 0 && m->bitrate_kbps % BITRATE_STEP_KBPS)
        b += BITRATE_STEP_KBPS;
    if (b < BITRATE_MIN_KBPS)
        b = BITRATE_MIN_KBPS;
    if (b > m->bitrate_max_kbps)
        b = m->bitrate_max_kbps;
    m->bitrate_kbps = b;
}

stream_menu_action_t stream_menu_input(stream_menu_t *m, unsigned pressed) {
    if (pressed & (MENU_BTN_CIRCLE | MENU_BTN_OPTIONS))
        return STREAM_MENU_RESUME;
    if (pressed & (MENU_BTN_UP | MENU_BTN_DOWN)) {
        m->sel = (m->sel + ((pressed & MENU_BTN_UP) ? ROW_COUNT - 1 : 1)) % ROW_COUNT;
        m->quit_armed = false;
    }
    if (m->sel == ROW_BITRATE && (pressed & (MENU_BTN_LEFT | MENU_BTN_RIGHT)))
        change_bitrate(m, (pressed & MENU_BTN_LEFT) ? -1 : 1);
    if (!(pressed & MENU_BTN_CROSS))
        return STREAM_MENU_STAY;

    switch (m->sel) {
    case ROW_RESUME:
        return STREAM_MENU_RESUME;
    case ROW_STATS:
        m->stats = !m->stats;
        return STREAM_MENU_STAY;
    case ROW_PACING:
        m->pacing = !m->pacing;
        return STREAM_MENU_STAY;
    case ROW_DISCONNECT:
        return STREAM_MENU_DISCONNECT;
    case ROW_QUIT:
        if (m->quit_armed)
            return STREAM_MENU_QUIT_GAME;
        m->quit_armed = true;
        return STREAM_MENU_STAY;
    default:
        return STREAM_MENU_STAY;
    }
}

static void draw_check(ui_surface_t *s, int right, int y, bool on) {
    int x = right - BOX_SIZE;
    int top = y + (ROW_H - BOX_SIZE) / 2;
    ui_round_rect(s, x, top, BOX_SIZE, BOX_SIZE, 3, on ? COL_ACCENT : COL_CONTROL);
    if (on)
        ui_icon(s, x + (BOX_SIZE - ui_icon_check.w) / 2, top + (BOX_SIZE - ui_icon_check.h) / 2,
                &ui_icon_check, COL_ON_ACCENT);
}

void stream_menu_draw(ui_surface_t *s, const stream_menu_t *m) {
    static const char *const labels[ROW_COUNT] = {
        "Resume", "Performance stats", "Frame pacing", "Video bitrate", "Disconnect", "Quit game",
    };
    ui_round_rect(s, MENU_X, MENU_Y, MENU_W, MENU_H, 3, COL_DIALOG);
    ui_font_text(s, &ui_font_title, MENU_X + PAD_X, MENU_Y + (TITLE_H - ui_font_title.line_h) / 2, COL_TEXT, "Moonlight");

    const int right = MENU_X + MENU_W - PAD_X;
    for (int i = 0; i < ROW_COUNT; i++) {
        int y = MENU_Y + TITLE_H + i * ROW_H;
        int text_y = y + (ROW_H - ui_font_body.line_h) / 2;
        if (i == m->sel)
            ui_blend_rect(s, MENU_X, y, MENU_W, ROW_H, COL_HIGHLIGHT);
        const char *label = (i == ROW_QUIT && m->quit_armed) ? "Press X again to quit the game" : labels[i];
        ui_font_text(s, &ui_font_body, MENU_X + PAD_X, text_y, (i == ROW_QUIT && m->quit_armed) ? COL_WARNING : COL_TEXT, label);

        if (i == ROW_STATS) {
            draw_check(s, right, y, m->stats);
        } else if (i == ROW_PACING) {
            draw_check(s, right, y, m->pacing);
        } else if (i == ROW_BITRATE) {
            char value[32];
            snprintf(value, sizeof(value), "<  %d.%d Mbps  >", m->bitrate_kbps / 1000, (m->bitrate_kbps % 1000) / 100);
            ui_font_text(s, &ui_font_body, right - ui_font_w(&ui_font_body, value), text_y, COL_ACCENT, value);
        }
    }

    int hint_y = MENU_Y + TITLE_H + ROW_COUNT * ROW_H + 14;
    if (m->bitrate_kbps != m->bitrate_opened_kbps)
        ui_font_text(s, &ui_font_small, MENU_X + PAD_X, hint_y, COL_WARNING, "The stream restarts on resume to apply the new bitrate.");
    ui_font_text(s, &ui_font_small, MENU_X + PAD_X, hint_y + 34, COL_HINT, "X  select      O  resume      Left / Right  change");
}

void stream_menu_bounds(int *x, int *y, int *w, int *h) {
    *x = MENU_X;
    *y = MENU_Y;
    *w = MENU_W;
    *h = MENU_H;
}

