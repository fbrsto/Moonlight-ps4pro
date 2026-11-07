/* The menu shown over the stream, opened by holding OPTIONS + touchpad. */
#ifndef STREAM_MENU_H
#define STREAM_MENU_H

#include <stdbool.h>

#include "ui_draw.h"

typedef enum {
    STREAM_MENU_STAY,
    STREAM_MENU_RESUME,
    STREAM_MENU_DISCONNECT,
    STREAM_MENU_QUIT_GAME,
} stream_menu_action_t;

typedef struct {
    int sel;
    bool quit_armed; /* quitting loses unsaved progress, so it takes a second press */
    bool stats;
    bool pacing;
    int bitrate_kbps;
    int bitrate_opened_kbps;
    int bitrate_max_kbps;
} stream_menu_t;

void stream_menu_open(stream_menu_t *m, bool stats, bool pacing, int bitrate_kbps, int bitrate_max_kbps);
/* pressed: MENU_BTN_* edges from input_menu_poll. */
stream_menu_action_t stream_menu_input(stream_menu_t *m, unsigned pressed);
void stream_menu_draw(ui_surface_t *s, const stream_menu_t *m);
/* The area stream_menu_draw writes to, on a 1920x1080 surface. */
void stream_menu_bounds(int *x, int *y, int *w, int *h);

#endif
