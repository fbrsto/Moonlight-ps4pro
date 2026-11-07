// Menu with the official Moonlight client's pages, sizes and Material dark theme
// (see moonlight-qt app/gui/*.qml), drawn in software for the PS4.

#include "ui_menu.h"
#include "ui_draw.h"
#include "ui_assets.h"
#include "ui_boxart.h"
#include "../log.h"
#include "../input/input_pad.h"
#include "../video/video.h"
#include "../gamestream/gs_errors.h"
#include "../gamestream/mdns.h"
#include "../gamestream/wol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __ORBIS__
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#endif

/* A host that answered discovery counts as gone after this long without a reply. */
#define PC_OFFLINE_AFTER_MS 8000
/* Keep showing the wake attempt, then try a connection, for networks where
 * discovery cannot see the PC come back. */
#define PC_WAKE_DISPLAY_US 60000000ull
#define PC_WAKE_RETRY_US 30000000ull
#define PC_RECONNECT_MIN_GAP_US 15000000ull

#define UI_W 1920
#define UI_H 1080
#define UI_MAX_APPS 64
#define MOONLIGHT_PS4_VERSION "1.3.13"

/* Material dark, accent Purple, primary #3F51B5: moonlight-qt app/main.cpp. */
#define COL_BG        0xFF303030u
#define COL_PRIMARY   0xFF3F51B5u
#define COL_ACCENT    0xFFCE93D8u
#define COL_TEXT      0xFFFFFFFFu
#define COL_HINT      0xB3FFFFFFu
#define COL_DISABLED  0x4DFFFFFFu
#define COL_DIALOG    0xFF424242u
#define COL_HIGHLIGHT 0x29FFFFFFu
#define COL_FRAME     0x1FFFFFFFu
#define COL_CONTROL   0x1FFFFFFFu
#define COL_TRACK     0x4DFFFFFFu
#define COL_SCRIM     0x99000000u
#define COL_GROUP     0xFF87CEEBu /* "skyblue" in SettingsView.qml */
#define COL_ROUND_BTN 0xD0808080u /* resume/quit buttons in AppView.qml */
#define COL_ON_ACCENT 0xFF303030u

#define TOOLBAR_H 72
#define GRID_TOP (TOOLBAR_H + 20)
#define GRID_MIN_MARGIN 10
/* PcView.qml */
#define PC_CELL_W 310
#define PC_CELL_H 330
#define PC_ITEM_W 300
#define PC_ITEM_H 320
/* AppView.qml */
#define APP_CELL_W 230
#define APP_CELL_H 297
#define APP_ITEM_W 220
#define APP_ITEM_H 287
#define APP_ART_Y 10

/* --- shared widgets --- */

static void draw_toolbar_base(ui_surface_t *s) {
    ui_rect(s, 0, 0, UI_W, TOOLBAR_H, COL_PRIMARY);
    /* Material elevation under the bar. */
    for (int i = 0; i < 6; i++)
        ui_blend_rect(s, 0, TOOLBAR_H + i, UI_W, 1, (uint32_t)(0x30 - i * 8) << 24);
}

static void draw_dialog_box(ui_surface_t *s, int x, int y, int w, int h) {
    for (int i = 6; i > 0; i--)
        ui_round_rect(s, x - i, y - i + 3, w + i * 2, h + i * 2, 6, 0x10000000u);
    ui_round_rect(s, x, y, w, h, 3, COL_DIALOG);
}

/* --- status screen --- */

static int s_splash_hidden;

static void ui_hide_splash_once(void) {
#ifdef __ORBIS__
    if (s_splash_hidden)
        return;
    (void)sceSystemServiceHideSplashScreen();
    s_splash_hidden = 1;
#endif
}

void ui_show_status(const char *title, const char *line1, const char *line2) {
    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui_show_status: video_ui_begin failed");
        ui_hide_splash_once(); /* better than eternal splash */
        return;
    }

    size_t pitch = (size_t)UI_W * 4u;
    size_t bytes = pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(bytes);
    if (!staging) {
        ui_hide_splash_once();
        return;
    }

    ui_surface_t s = { staging, (int)pitch, UI_W, UI_H };
    ui_clear(&s, COL_BG);
    draw_toolbar_base(&s);
    const char *bar = "Computers";
    ui_font_text(&s, &ui_font_title, (UI_W - ui_font_w(&ui_font_title, bar)) / 2,
                 (TOOLBAR_H - ui_font_title.line_h) / 2, COL_TEXT, bar);

    const int dw = 860, dh = 260, dx = (UI_W - dw) / 2, dy = (UI_H - dh) / 2;
    draw_dialog_box(&s, dx, dy, dw, dh);
    int ty = dy + 24;
    ui_font_text(&s, &ui_font_title, dx + 24, ty, COL_TEXT, title ? title : "");
    ty += ui_font_title.line_h + 12;
    if (line1 && line1[0])
        ty += ui_font_para(&s, &ui_font_body, dx + 24, ty, dw - 48, COL_TEXT, line1) + 8;
    if (line2 && line2[0])
        ui_font_para(&s, &ui_font_body, dx + 24, ty, dw - 48, COL_HINT, line2);

    for (int i = 0; i < 4; i++) {
        (void)video_ui_present(staging, (int)pitch);
        usleep(16000);
    }
    ui_hide_splash_once();
    free(staging);
    LOGI("ui_show_status: '%s' | '%s' | '%s'",
         title ? title : "", line1 ? line1 : "", line2 ? line2 : "");
}

/* --- pages --- */

typedef enum {
    PAGE_PCS = 0,
    PAGE_APPS,
    PAGE_SETTINGS,
} ui_page_t;

/* The page survives a stream: leaving a game comes back to the app grid. */
static ui_page_t s_page = PAGE_PCS;
/* Survives the menu closing, or a PC that answers discovery but refuses the
 * connection would be retried in a tight loop. */
static uint64_t s_last_auto_reconnect_us;

typedef enum {
    DLG_NONE = 0,
    DLG_QUIT,        /* quit the running app */
    DLG_QUIT_LAUNCH, /* quit the running app and launch the selected one */
    DLG_DELETE_PC,
} ui_dialog_t;

typedef enum {
    ACT_NONE = 0,
    ACT_VIEW_APPS,
    ACT_WAKE,
    ACT_REFRESH,
    ACT_DELETE_PC,
    ACT_LAUNCH,
    ACT_QUIT,
} ui_action_t;

#define MENU_MAX 6
typedef struct {
    int active;
    int x, y, n, sel;
    struct {
        char label[48];
        ui_action_t action; /* ACT_NONE: disabled header */
    } items[MENU_MAX];
} ui_popup_t;

/* --- settings table --- */

enum {
    SET_NONE = 0,
    SET_RES, SET_FPS, SET_BITRATE, SET_BITRATE_DEFAULT, SET_VSYNC, SET_FRAME_PACING,
    SET_AUDIO, SET_MUTE_HOST,
    SET_SOPS, SET_QUIT_AFTER,
    SET_CONN_WARNINGS, SET_CONFIG_WARNINGS, SET_KEEP_AWAKE,
    SET_SWAP_MOUSE,
    SET_SWAP_FACE, SET_MULTI_CONTROLLER, SET_TOUCH_HOLD_MOUSE,
    SET_PAD_RUMBLE, SET_PAD_MOTION, SET_PAD_TOUCHPAD, SET_PAD_LIGHTBAR,
    SET_DECODER, SET_UNLOCK_BITRATE, SET_MDNS, SET_SHOW_STATS, SET_FILE_LOG,
    SET_DEBUG_HOST,
};

typedef enum {
    IT_COLUMN,  /* start of the next column */
    IT_GROUP,   /* group box title */
    IT_LABEL,   /* title + description, not focusable */
    IT_COMBO,
    IT_COMBO_SAME_ROW, /* combo to the right of the previous one */
    IT_SLIDER,
    IT_BUTTON_SAME_ROW, /* to the right of the slider */
    IT_CHECK,
    IT_FIELD,
} item_type_t;

typedef struct {
    item_type_t type;
    int id;
    const char *text;
    const char *desc;
} set_item_t;

#define BITRATE_STEP_KBPS 500
#define BITRATE_MIN_KBPS 500
#define BITRATE_MAX_KBPS 150000
#define BITRATE_MAX_UNLOCKED_KBPS 500000

/* Wording and order follow SettingsView.qml, minus what the PS4 cannot do. */
static const set_item_t k_items[] = {
    { IT_GROUP, 0, "Basic Settings", NULL },
    { IT_LABEL, 0, "Resolution and FPS", "Setting values too high for your PC or network connection may cause lag, stuttering, or errors." },
    { IT_COMBO, SET_RES, NULL, NULL },
    { IT_COMBO_SAME_ROW, SET_FPS, NULL, NULL },
    { IT_LABEL, SET_BITRATE, "Video bitrate:", "Lower the bitrate on slower connections. Raise the bitrate to increase image quality." },
    { IT_SLIDER, SET_BITRATE, NULL, NULL },
    { IT_BUTTON_SAME_ROW, SET_BITRATE_DEFAULT, NULL, NULL },
    { IT_CHECK, SET_VSYNC, "V-Sync", NULL },
    { IT_CHECK, SET_FRAME_PACING, "Frame pacing (steadier FPS, slightly more latency)", NULL },
    { IT_GROUP, 0, "Audio Settings", NULL },
    { IT_LABEL, 0, "Audio configuration", "Surround needs a matching PS4 audio output." },
    { IT_COMBO, SET_AUDIO, NULL, NULL },
    { IT_CHECK, SET_MUTE_HOST, "Mute host PC speakers while streaming", NULL },
    { IT_GROUP, 0, "Host Settings", NULL },
    { IT_CHECK, SET_SOPS, "Optimize game settings for streaming", NULL },
    { IT_CHECK, SET_QUIT_AFTER, "Quit app on host PC after ending stream", NULL },
    { IT_GROUP, 0, "UI Settings", NULL },
    { IT_CHECK, SET_CONN_WARNINGS, "Show connection quality warnings", NULL },
    { IT_CHECK, SET_CONFIG_WARNINGS, "Show configuration warnings", NULL },
    { IT_CHECK, SET_KEEP_AWAKE, "Keep the console awake while streaming", NULL },
    { IT_COLUMN, 0, NULL, NULL },
    { IT_GROUP, 0, "Input Settings", NULL },
    { IT_CHECK, SET_SWAP_MOUSE, "Swap left and right mouse buttons", NULL },
    { IT_GROUP, 0, "Gamepad Settings", NULL },
    { IT_CHECK, SET_SWAP_FACE, "Swap A/B and X/Y gamepad buttons", NULL },
    { IT_CHECK, SET_MULTI_CONTROLLER, "Use one gamepad per logged-in PS4 user", NULL },
    { IT_CHECK, SET_TOUCH_HOLD_MOUSE, "Finger on touchpad: right stick moves mouse, R2/L2 click", NULL },
    { IT_CHECK, SET_PAD_RUMBLE, "Rumble", NULL },
    { IT_CHECK, SET_PAD_MOTION, "Motion controls (gyroscope and accelerometer)", NULL },
    { IT_CHECK, SET_PAD_TOUCHPAD, "Send touchpad touches to games", NULL },
    { IT_CHECK, SET_PAD_LIGHTBAR, "Let games set the light bar color", NULL },
    { IT_GROUP, 0, "Advanced Settings", NULL },
    { IT_LABEL, 0, "Video decoder", "Automatic tries hardware, then software." },
    { IT_COMBO, SET_DECODER, NULL, NULL },
    { IT_CHECK, SET_UNLOCK_BITRATE, "Unlock bitrate limit (Experimental)", NULL },
    { IT_CHECK, SET_MDNS, "Automatically find PCs on the local network (Recommended)", NULL },
    { IT_CHECK, SET_SHOW_STATS, "Show performance stats while streaming", NULL },
    { IT_CHECK, SET_FILE_LOG, "Write a debug log to /data/moonlight/debug.log", NULL },
    { IT_LABEL, 0, "Debug host", "PC that receives UDP logs on port 9999." },
    { IT_FIELD, SET_DEBUG_HOST, NULL, NULL },
};
#define ITEM_COUNT ((int)(sizeof(k_items) / sizeof(k_items[0])))

/* Combo contents. */
/* The last two need the decoder to accept them; output stays 1080p either way. */
#define RES_COUNT 6
static const char *const k_res_labels[RES_COUNT] = { "540p", "720p", "900p", "1080p", "1440p", "4K" };
static const int k_res_modes[RES_COUNT][2] = {
    {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};
#define FPS_PRESETS 2
#define FPS_CUSTOM FPS_PRESETS /* combo entry that opens the number keyboard */
#define FPS_MIN 10
#define FPS_MAX 60 /* the console's video output runs at 60 Hz */
static const char *const k_fps_labels[] = { "30 FPS", "60 FPS", "Custom..." };
static const int k_fps_values[FPS_PRESETS] = { 30, 60 };
static const char *const k_audio_labels[] = { "Stereo", "5.1 surround", "7.1 surround" };
static const char *const k_decoder_labels[] = { "Automatic", "Force software", "Force hardware" };

static int combo_options(int id, const char *const **labels) {
    switch (id) {
    case SET_RES:      *labels = k_res_labels; return RES_COUNT;
    case SET_FPS:      *labels = k_fps_labels; return FPS_PRESETS + 1;
    case SET_AUDIO:    *labels = k_audio_labels; return 3;
    case SET_DECODER:  *labels = k_decoder_labels; return 3;
    default:           *labels = NULL; return 0;
    }
}

/* Current option, or -1 when the INI holds a value outside the list. */
static int combo_get(const app_config_t *c, int id) {
    switch (id) {
    case SET_RES:
        for (int i = 0; i < RES_COUNT; i++)
            if (c->stream.width == k_res_modes[i][0] && c->stream.height == k_res_modes[i][1])
                return i;
        return -1;
    case SET_FPS:
        for (int i = 0; i < FPS_PRESETS; i++)
            if (c->stream.fps == k_fps_values[i])
                return i;
        return FPS_CUSTOM;
    case SET_AUDIO:
        if (c->stream.audioConfiguration == AUDIO_CONFIGURATION_51_SURROUND) return 1;
        if (c->stream.audioConfiguration == AUDIO_CONFIGURATION_71_SURROUND) return 2;
        return 0;
    case SET_DECODER:
        return !c->prefer_hw ? 1 : (c->force_hw ? 2 : 0);
    default:
        return -1;
    }
}

static void apply_auto_bitrate(app_config_t *c) {
    if (c->auto_bitrate)
        c->stream.bitrate = config_default_bitrate(c->stream.width, c->stream.height, c->stream.fps);
}

static void combo_set(app_config_t *c, int id, int v) {
    switch (id) {
    case SET_RES:
        c->stream.width = k_res_modes[v][0];
        c->stream.height = k_res_modes[v][1];
        apply_auto_bitrate(c);
        break;
    case SET_FPS:
        c->stream.fps = k_fps_values[v];
        apply_auto_bitrate(c);
        break;
    case SET_AUDIO:
        c->stream.audioConfiguration = v == 1 ? AUDIO_CONFIGURATION_51_SURROUND
                                     : v == 2 ? AUDIO_CONFIGURATION_71_SURROUND
                                              : AUDIO_CONFIGURATION_STEREO;
        break;
    case SET_DECODER:
        c->prefer_hw = v != 1;
        c->force_hw = v == 2;
        break;
    default:
        break;
    }
}

static void combo_text(const app_config_t *c, int id, char *out, size_t cap) {
    const char *const *labels;
    int n = combo_options(id, &labels);
    int v = combo_get(c, id);
    if (id == SET_FPS && v == FPS_CUSTOM)
        snprintf(out, cap, "%d FPS", c->stream.fps);
    else if (v >= 0 && v < n)
        snprintf(out, cap, "%s", labels[v]);
    else if (id == SET_RES)
        snprintf(out, cap, "%dx%d", c->stream.width, c->stream.height);
    else
        out[0] = '\0';
}

static int check_get(const app_config_t *c, int id) {
    switch (id) {
    case SET_VSYNC:            return c->vsync;
    case SET_FRAME_PACING:     return c->frame_pacing;
    case SET_MUTE_HOST:        return !c->local_audio;
    case SET_SOPS:             return c->sops;
    case SET_QUIT_AFTER:       return c->quit_app_after;
    case SET_CONN_WARNINGS:    return c->connection_warnings;
    case SET_CONFIG_WARNINGS:  return c->config_warnings;
    case SET_KEEP_AWAKE:       return c->keep_awake;
    case SET_SWAP_MOUSE:       return c->swap_mouse_buttons;
    case SET_SWAP_FACE:        return c->swap_face_buttons;
    case SET_MULTI_CONTROLLER: return c->multi_controller;
    case SET_TOUCH_HOLD_MOUSE: return c->touch_hold_mouse;
    case SET_PAD_RUMBLE:       return c->pad_rumble;
    case SET_PAD_MOTION:       return c->pad_motion;
    case SET_PAD_TOUCHPAD:     return c->pad_touchpad;
    case SET_PAD_LIGHTBAR:     return c->pad_lightbar;
    case SET_UNLOCK_BITRATE:   return c->unlock_bitrate;
    case SET_MDNS:             return c->enable_mdns;
    case SET_SHOW_STATS:       return c->show_stats;
    case SET_FILE_LOG:         return c->enable_file_log;
    default:                   return 0;
    }
}

/* On-screen keyboard. */
#define OSK_COLS 10
#define OSK_ROWS 4
static const char k_osk_chars[OSK_ROWS][OSK_COLS + 1] = {
    "0123456789",
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ.-:_",
};
#define OSK_TARGET_HOST 1
#define OSK_TARGET_DEBUG 2
#define OSK_TARGET_FPS 3
#define OSK_FPS_MAX_DIGITS 2

typedef struct {
    char name[CONFIG_MAX_APP];
    int id;
} ui_app_t;

typedef struct {
    app_config_t *cfg;
    gs_server_t *server;
    const char *config_dir;

    ui_app_t apps[UI_MAX_APPS];
    int app_ids[UI_MAX_APPS];
    int napps;
    int apps_err; /* 0 ok, 1 applist failed, 2 no connection */

    ui_page_t page;
    ui_page_t settings_return;
    ui_dialog_t dialog;
    ui_popup_t popup;

    int sel_pc; /* card index: the saved PC first, then discovered ones */
    int sel_app;
    int app_first_row;

    int sel_item;   /* index into k_items */
    int combo_open; /* dropdown list on sel_item */
    int combo_sel;

    int osk_active;
    int osk_target;
    int osk_row, osk_col;
    char osk_buf[CONFIG_MAX_HOST];

    uint64_t wake_sent_us; /* 0: no wake attempt in progress */
    int wake_retried;

    int host_dirty; /* host changed: reconnect required */
    int result;     /* -1000 = stay in loop */
} ui_state_t;

static void fetch_apps(ui_state_t *st) {
    st->napps = 0;
    if (!st->server) {
        st->apps_err = 2;
        return;
    }
    (void)gs_refresh_status(st->server);
    app_entry_t *list = NULL;
    if (gs_applist(st->server, &list) != GS_OK) {
        LOGW("ui: gs_applist failed: %s", gs_error ? gs_error : "?");
        st->apps_err = 1;
        return;
    }
    for (app_entry_t *a = list; a && st->napps < UI_MAX_APPS; a = a->next) {
        snprintf(st->apps[st->napps].name, sizeof(st->apps[st->napps].name),
                 "%s", a->name ? a->name : "?");
        st->apps[st->napps].id = a->id;
        st->app_ids[st->napps] = a->id;
        st->napps++;
    }
    xml_applist_free(list);
    st->apps_err = 0;
    LOGI("ui: applist %d apps currentGame=%d", st->napps,
         st->server->currentGame);
}

static int running_game(const ui_state_t *st) {
    return st->server ? st->server->currentGame : 0;
}

static const char *active_app_name(const ui_state_t *st) {
    int id = running_game(st);
    for (int i = 0; id && i < st->napps; i++) {
        if (st->apps[i].id == id)
            return st->apps[i].name;
    }
    return NULL;
}

static const char *pc_name(const ui_state_t *st) {
    if (st->server && st->server->hostname[0] && !st->host_dirty)
        return st->server->hostname;
    return st->cfg->host;
}

static int pc_online(const ui_state_t *st) {
    if (st->server == NULL || st->host_dirty)
        return 0;
    /* A PC discovery never heard from stays online: discovery may simply not
     * reach it. Only one that answered, then stopped, is known to be gone. */
    if (st->cfg->enable_mdns) {
        int age = mdns_host_age_ms(st->cfg->host, st->server->hostname);
        if (age > PC_OFFLINE_AFTER_MS)
            return 0;
    }
    return 1;
}

static int pc_waking(const ui_state_t *st) {
    return st->wake_sent_us != 0 && !pc_online(st) &&
           sceKernelGetProcessTime() - st->wake_sent_us < PC_WAKE_DISPLAY_US;
}

/* Discovered PCs worth a card of their own: not the one already saved. */
static int found_cards(const ui_state_t *st, const mdns_host_t **out) {
    int n = 0;
    if (!st->cfg->enable_mdns)
        return 0;
    for (int i = 0; i < mdns_count() && n < MDNS_MAX_HOSTS; i++) {
        const mdns_host_t *h = mdns_host(i);
        if (!strcasecmp(h->addr, st->cfg->host) || !strcasecmp(h->name, st->cfg->host))
            continue;
        if (pc_online(st) && !strcasecmp(h->name, st->server->hostname))
            continue; /* the same PC, saved under another address */
        out[n++] = h;
    }
    return n;
}

static void quit_active_stream(ui_state_t *st) {
    if (running_game(st) == 0)
        return;
    int id = st->server->currentGame;
    const char *name = active_app_name(st);
    LOGI("ui: closing active stream id=%d name='%s'", id, name ? name : "?");
    if (gs_quit_app(st->server) != GS_OK) {
        LOGW("ui: gs_quit_app failed: %s", gs_error ? gs_error : "?");
        (void)gs_refresh_status(st->server);
    } else {
        LOGI("ui: stream closed");
        LOGN("Stream closed");
    }
}

static void preselect_app(ui_state_t *st) {
    st->sel_app = 0;
    for (int i = 0; i < st->napps; i++) {
        if (!strcasecmp(st->apps[i].name, st->cfg->app_name)) {
            st->sel_app = i;
            break;
        }
    }
}

static void save_cfg(ui_state_t *st) {
    if (config_save(st->cfg, st->config_dir) != 0)
        LOGW("ui: config_save failed");
}

/* --- OSK --- */

static void osk_open(ui_state_t *st, int target) {
    st->osk_active = 1;
    st->osk_target = target;
    st->osk_row = 0;
    st->osk_col = 0;
    if (target == OSK_TARGET_FPS) {
        st->osk_buf[0] = '\0';
        return;
    }
    const char *cur = (target == OSK_TARGET_HOST) ? st->cfg->host : st->cfg->debug_host;
    snprintf(st->osk_buf, sizeof(st->osk_buf), "%s", cur);
}

/* The FPS keyboard shows the digits row only. */
static int osk_rows(const ui_state_t *st) {
    return st->osk_target == OSK_TARGET_FPS ? 1 : OSK_ROWS;
}

static void osk_accept(ui_state_t *st) {
    if (st->osk_target == OSK_TARGET_FPS) {
        if (st->osk_buf[0]) {
            int fps = atoi(st->osk_buf);
            if (fps < FPS_MIN) fps = FPS_MIN;
            if (fps > FPS_MAX) fps = FPS_MAX;
            st->cfg->stream.fps = fps;
            apply_auto_bitrate(st->cfg);
            save_cfg(st);
        }
        st->osk_active = 0;
        return;
    }
    if (st->osk_target == OSK_TARGET_HOST) {
        if (strcmp(st->cfg->host, st->osk_buf) != 0) {
            snprintf(st->cfg->host, sizeof(st->cfg->host), "%s", st->osk_buf);
            st->cfg->host_mac[0] = '\0'; /* that MAC belonged to the old PC */
            st->host_dirty = 1;
        }
        save_cfg(st);
        st->osk_active = 0;
        /* After writing host: reconnect when leaving the keyboard. */
        if (st->cfg->host[0] && st->host_dirty)
            st->result = UI_MENU_RECONNECT;
        return;
    }
    snprintf(st->cfg->debug_host, sizeof(st->cfg->debug_host), "%s", st->osk_buf);
    save_cfg(st);
    st->osk_active = 0;
}

static void osk_input(ui_state_t *st, unsigned pr) {
    const int rows = osk_rows(st);
    if (pr & MENU_BTN_UP)    st->osk_row = (st->osk_row + rows - 1) % rows;
    if (pr & MENU_BTN_DOWN)  st->osk_row = (st->osk_row + 1) % rows;
    if (pr & MENU_BTN_LEFT)  st->osk_col = (st->osk_col + OSK_COLS - 1) % OSK_COLS;
    if (pr & MENU_BTN_RIGHT) st->osk_col = (st->osk_col + 1) % OSK_COLS;
    if (pr & MENU_BTN_CROSS) {
        size_t len = strlen(st->osk_buf);
        size_t cap = st->osk_target == OSK_TARGET_FPS ? OSK_FPS_MAX_DIGITS + 1 : sizeof(st->osk_buf);
        if (len + 1 < cap) {
            st->osk_buf[len] = k_osk_chars[st->osk_row][st->osk_col];
            st->osk_buf[len + 1] = '\0';
        }
    }
    if (pr & MENU_BTN_SQUARE) {
        size_t len = strlen(st->osk_buf);
        if (len)
            st->osk_buf[len - 1] = '\0';
    }
    if (pr & MENU_BTN_CIRCLE)
        st->osk_active = 0;
    if (pr & MENU_BTN_OPTIONS)
        osk_accept(st);
}

/* --- actions --- */

static void refresh(ui_state_t *st) {
    if (st->host_dirty || !st->server) {
        save_cfg(st);
        if (st->cfg->host[0])
            st->result = UI_MENU_RECONNECT;
    } else {
        fetch_apps(st);
        preselect_app(st);
    }
}

static void launch_selected(ui_state_t *st) {
    if (st->napps > 0) {
        snprintf(st->cfg->app_name, sizeof(st->cfg->app_name), "%s",
                 st->apps[st->sel_app].name);
    }
    save_cfg(st);
    LOGI("ui: launch app='%s' (sel=%d host_dirty=%d)",
         st->cfg->app_name, st->sel_app, st->host_dirty);
    st->result = st->host_dirty ? UI_MENU_RECONNECT : UI_MENU_LAUNCH;
}

static void popup_add(ui_popup_t *m, const char *label, ui_action_t action) {
    if (m->n >= MENU_MAX)
        return;
    snprintf(m->items[m->n].label, sizeof(m->items[m->n].label), "%s", label);
    m->items[m->n].action = action;
    m->n++;
}

/* First selectable entry. */
static void popup_open(ui_popup_t *m, int x, int y) {
    m->active = 1;
    m->x = x;
    m->y = y;
    m->sel = 0;
    while (m->sel < m->n - 1 && m->items[m->sel].action == ACT_NONE)
        m->sel++;
}

static void open_pc_menu(ui_state_t *st) {
    ui_popup_t *m = &st->popup;
    m->n = 0;
    int online = pc_online(st);
    popup_add(m, online ? "PC Status: Online" : "PC Status: Offline", ACT_NONE);
    if (online)
        popup_add(m, "View All Apps", ACT_VIEW_APPS);
    else if (st->cfg->host_mac[0])
        popup_add(m, "Wake PC", ACT_WAKE);
    popup_add(m, "Refresh", ACT_REFRESH);
    popup_add(m, "Delete PC", ACT_DELETE_PC);
    popup_open(m, GRID_MIN_MARGIN + 150, GRID_TOP + 180);
}

static void app_item_pos(const ui_state_t *st, int index, int *x, int *y);

static void open_app_menu(ui_state_t *st) {
    ui_popup_t *m = &st->popup;
    m->n = 0;
    int running = running_game(st) != 0 && st->apps[st->sel_app].id == running_game(st);
    popup_add(m, running ? "Resume Game" : "Launch Game", ACT_LAUNCH);
    if (running)
        popup_add(m, "Quit Game", ACT_QUIT);
    int x, y;
    app_item_pos(st, st->sel_app, &x, &y);
    popup_open(m, x + 110, y + 140);
}

static void popup_input(ui_state_t *st, unsigned pr) {
    ui_popup_t *m = &st->popup;
    if (pr & MENU_BTN_CIRCLE) {
        m->active = 0;
        return;
    }
    if (pr & (MENU_BTN_UP | MENU_BTN_DOWN)) {
        int step = (pr & MENU_BTN_UP) ? m->n - 1 : 1;
        int r = m->sel;
        for (int i = 0; i < m->n; i++) {
            r = (r + step) % m->n;
            if (m->items[r].action != ACT_NONE)
                break;
        }
        m->sel = r;
    }
    if (!(pr & MENU_BTN_CROSS))
        return;
    ui_action_t act = m->items[m->sel].action;
    if (act == ACT_NONE)
        return;
    m->active = 0;
    switch (act) {
    case ACT_VIEW_APPS: st->page = PAGE_APPS; break;
    case ACT_WAKE:
        (void)wol_send(st->cfg->host_mac, st->cfg->host);
        st->wake_sent_us = sceKernelGetProcessTime();
        st->wake_retried = 0;
        break;
    case ACT_REFRESH:   refresh(st); break;
    case ACT_DELETE_PC: st->dialog = DLG_DELETE_PC; break;
    case ACT_LAUNCH:    launch_selected(st); break;
    case ACT_QUIT:      st->dialog = DLG_QUIT; break;
    default: break;
    }
}

static void dialog_input(ui_state_t *st, unsigned pr) {
    if (pr & MENU_BTN_CIRCLE) {
        st->dialog = DLG_NONE;
        return;
    }
    if (!(pr & MENU_BTN_CROSS))
        return;
    ui_dialog_t what = st->dialog;
    st->dialog = DLG_NONE;
    if (what == DLG_QUIT) {
        quit_active_stream(st);
    } else if (what == DLG_QUIT_LAUNCH) {
        launch_selected(st); /* stream_play quits the other app first */
    } else if (what == DLG_DELETE_PC) {
        st->cfg->host[0] = '\0';
        st->cfg->host_mac[0] = '\0';
        st->wake_sent_us = 0;
        st->server = NULL;
        st->host_dirty = 0;
        st->napps = 0;
        st->apps_err = 2;
        save_cfg(st);
    }
}

/* --- page input --- */

static void open_settings(ui_state_t *st) {
    st->settings_return = st->page;
    st->page = PAGE_SETTINGS;
    st->combo_open = 0;
}

static void pcs_input(ui_state_t *st, unsigned pr) {
    if (pr & MENU_BTN_R1) {
        open_settings(st);
        return;
    }
    if (pr & MENU_BTN_L1) {
        osk_open(st, OSK_TARGET_HOST);
        return;
    }

    const mdns_host_t *found[MDNS_MAX_HOSTS];
    int nfound = found_cards(st, found);
    int has_saved = st->cfg->host[0] != '\0';
    int cards = has_saved + nfound;
    if (cards == 0)
        return;
    if (st->sel_pc >= cards)
        st->sel_pc = cards - 1;
    if (pr & MENU_BTN_LEFT)
        st->sel_pc = (st->sel_pc + cards - 1) % cards;
    if (pr & MENU_BTN_RIGHT)
        st->sel_pc = (st->sel_pc + 1) % cards;

    if (!has_saved || st->sel_pc > 0) {
        /* A discovered PC: it becomes the saved one, then pairing runs if needed. */
        if (pr & MENU_BTN_CROSS) {
            const mdns_host_t *h = found[st->sel_pc - has_saved];
            snprintf(st->cfg->host, sizeof(st->cfg->host), "%s", h->addr);
            st->cfg->host_mac[0] = '\0';
            st->host_dirty = 1;
            st->sel_pc = 0;
            save_cfg(st);
            st->result = UI_MENU_RECONNECT;
        }
        return;
    }

    if (pr & MENU_BTN_TRIANGLE)
        refresh(st);
    if (pr & MENU_BTN_OPTIONS) {
        open_pc_menu(st);
    } else if (pr & MENU_BTN_CROSS) {
        /* Offline PCs open the context menu, as in PcView.qml. */
        if (pc_online(st))
            st->page = PAGE_APPS;
        else
            open_pc_menu(st);
    }
}

/* Grid layout from CenteredGridView.qml. */
static int grid_per_row(int cell_w) {
    return (UI_W - 2 * GRID_MIN_MARGIN) / cell_w;
}

static int grid_margin(int cell_w, int count) {
    int avail = UI_W - 2 * GRID_MIN_MARGIN;
    if (avail / cell_w < count && avail >= cell_w)
        return (avail % cell_w) / 2;
    return GRID_MIN_MARGIN;
}

static void app_item_pos(const ui_state_t *st, int index, int *x, int *y) {
    int per_row = grid_per_row(APP_CELL_W);
    *x = GRID_MIN_MARGIN + grid_margin(APP_CELL_W, st->napps) + (index % per_row) * APP_CELL_W;
    *y = GRID_TOP + (index / per_row - st->app_first_row) * APP_CELL_H;
}

static void apps_input(ui_state_t *st, unsigned pr) {
    int per_row = grid_per_row(APP_CELL_W);
    if (st->napps > 0) {
        int i = st->sel_app;
        if ((pr & MENU_BTN_LEFT) && i > 0)
            i--;
        if ((pr & MENU_BTN_RIGHT) && i + 1 < st->napps)
            i++;
        if ((pr & MENU_BTN_UP) && i >= per_row)
            i -= per_row;
        if (pr & MENU_BTN_DOWN) {
            /* The last row may be short: land on its final item. */
            if (i + per_row < st->napps)
                i += per_row;
            else if (i / per_row < (st->napps - 1) / per_row)
                i = st->napps - 1;
        }
        st->sel_app = i;
    }
    if (pr & MENU_BTN_R1) {
        open_settings(st);
        return;
    }
    if (pr & MENU_BTN_CIRCLE) {
        st->page = PAGE_PCS;
        return;
    }
    if (pr & MENU_BTN_TRIANGLE)
        refresh(st);
    if (st->napps == 0) {
        if ((pr & MENU_BTN_CROSS) && st->apps_err)
            st->result = UI_MENU_RECONNECT;
        return;
    }
    if (pr & MENU_BTN_OPTIONS) {
        open_app_menu(st);
    } else if (pr & MENU_BTN_CROSS) {
        int running = running_game(st);
        if (running != 0 && st->apps[st->sel_app].id == running)
            open_app_menu(st); /* AppView.qml: resume or quit */
        else if (running != 0)
            st->dialog = DLG_QUIT_LAUNCH;
        else
            launch_selected(st);
    }
}

static int bitrate_default(const app_config_t *c) {
    return config_default_bitrate(c->stream.width, c->stream.height, c->stream.fps);
}

static int item_focusable(const app_config_t *c, int i) {
    switch (k_items[i].type) {
    case IT_COMBO:
    case IT_COMBO_SAME_ROW:
    case IT_SLIDER:
    case IT_CHECK:
    case IT_FIELD:
        return 1;
    case IT_BUTTON_SAME_ROW:
        return c->stream.bitrate != bitrate_default(c);
    default:
        return 0;
    }
}

static void check_toggle(ui_state_t *st, int id) {
    app_config_t *c = st->cfg;
    switch (id) {
    case SET_VSYNC:            c->vsync = !c->vsync; break;
    case SET_FRAME_PACING:     c->frame_pacing = !c->frame_pacing; break;
    case SET_MUTE_HOST:        c->local_audio = !c->local_audio; break;
    case SET_SOPS:             c->sops = !c->sops; break;
    case SET_QUIT_AFTER:       c->quit_app_after = !c->quit_app_after; break;
    case SET_CONN_WARNINGS:    c->connection_warnings = !c->connection_warnings; break;
    case SET_CONFIG_WARNINGS:  c->config_warnings = !c->config_warnings; break;
    case SET_KEEP_AWAKE:       c->keep_awake = !c->keep_awake; break;
    case SET_SWAP_MOUSE:       c->swap_mouse_buttons = !c->swap_mouse_buttons; break;
    case SET_SWAP_FACE:        c->swap_face_buttons = !c->swap_face_buttons; break;
    case SET_MULTI_CONTROLLER: c->multi_controller = !c->multi_controller; break;
    case SET_TOUCH_HOLD_MOUSE: c->touch_hold_mouse = !c->touch_hold_mouse; break;
    case SET_PAD_RUMBLE:       c->pad_rumble = !c->pad_rumble; break;
    case SET_PAD_MOTION:       c->pad_motion = !c->pad_motion; break;
    case SET_PAD_TOUCHPAD:     c->pad_touchpad = !c->pad_touchpad; break;
    case SET_PAD_LIGHTBAR:     c->pad_lightbar = !c->pad_lightbar; break;
    case SET_UNLOCK_BITRATE:
        c->unlock_bitrate = !c->unlock_bitrate;
        if (!c->unlock_bitrate && c->stream.bitrate > BITRATE_MAX_KBPS)
            c->stream.bitrate = BITRATE_MAX_KBPS;
        break;
    case SET_MDNS:             c->enable_mdns = !c->enable_mdns; break;
    case SET_SHOW_STATS:       c->show_stats = !c->show_stats; break;
    case SET_FILE_LOG: {
        char path[512];
        c->enable_file_log = !c->enable_file_log;
        snprintf(path, sizeof(path), "%s/debug.log", st->config_dir);
        log_set_file_enabled(c->enable_file_log ? 1 : 0, path);
        break;
    }
    default: break;
    }
}

static void combo_input(ui_state_t *st, unsigned pr) {
    const char *const *labels;
    int id = k_items[st->sel_item].id;
    int n = combo_options(id, &labels);
    if (pr & MENU_BTN_CIRCLE) {
        st->combo_open = 0;
        return;
    }
    if (pr & MENU_BTN_UP)
        st->combo_sel = (st->combo_sel + n - 1) % n;
    if (pr & MENU_BTN_DOWN)
        st->combo_sel = (st->combo_sel + 1) % n;
    if (pr & MENU_BTN_CROSS) {
        st->combo_open = 0;
        if (id == SET_FPS && st->combo_sel == FPS_CUSTOM) {
            osk_open(st, OSK_TARGET_FPS);
            return;
        }
        combo_set(st->cfg, id, st->combo_sel);
        save_cfg(st);
    }
}

static void settings_input(ui_state_t *st, unsigned pr) {
    app_config_t *c = st->cfg;
    if (pr & MENU_BTN_CIRCLE) {
        st->page = st->settings_return;
        return;
    }
    if (pr & (MENU_BTN_UP | MENU_BTN_DOWN)) {
        int step = (pr & MENU_BTN_UP) ? ITEM_COUNT - 1 : 1;
        int r = st->sel_item;
        do {
            r = (r + step) % ITEM_COUNT;
        } while (!item_focusable(c, r));
        st->sel_item = r;
    }
    if (pr & (MENU_BTN_L1 | MENU_BTN_R1)) {
        /* Jump to the first item of the other column. */
        int column_at = 0;
        for (int i = 0; i < ITEM_COUNT; i++)
            if (k_items[i].type == IT_COLUMN)
                column_at = i;
        int r = st->sel_item < column_at ? column_at : 0;
        while (!item_focusable(c, r))
            r++;
        st->sel_item = r;
    }

    const set_item_t *it = &k_items[st->sel_item];
    int dir = (pr & MENU_BTN_LEFT) ? -1 : (pr & MENU_BTN_RIGHT) ? 1 : 0;
    int activate = (pr & MENU_BTN_CROSS) != 0;
    if (!dir && !activate)
        return;

    switch (it->type) {
    case IT_COMBO:
    case IT_COMBO_SAME_ROW:
        if (activate) {
            int cur = combo_get(c, it->id);
            st->combo_open = 1;
            st->combo_sel = cur >= 0 ? cur : 0;
        }
        return;
    case IT_SLIDER: {
        if (!dir)
            return;
        int max = c->unlock_bitrate ? BITRATE_MAX_UNLOCKED_KBPS : BITRATE_MAX_KBPS;
        int b = c->stream.bitrate + dir * BITRATE_STEP_KBPS;
        if (b < BITRATE_MIN_KBPS) b = BITRATE_MIN_KBPS;
        if (b > max) b = max;
        c->stream.bitrate = b;
        c->auto_bitrate = false; /* a manual value must survive mode changes */
        break;
    }
    case IT_BUTTON_SAME_ROW:
        if (!activate)
            return;
        c->auto_bitrate = true;
        apply_auto_bitrate(c);
        /* The button just hid itself: move back to the slider. */
        st->sel_item--;
        break;
    case IT_CHECK:
        if (!activate)
            return;
        check_toggle(st, it->id);
        break;
    case IT_FIELD:
        if (activate)
            osk_open(st, OSK_TARGET_DEBUG);
        return;
    default:
        return;
    }
    save_cfg(st);
}

/* --- drawing --- */

static void draw_toolbar(ui_state_t *st, ui_surface_t *s) {
    draw_toolbar_base(s);
    const int icon_y = (TOOLBAR_H - ui_icon_settings.h) / 2;
    const int hint_y = (TOOLBAR_H - ui_font_small.line_h) / 2;
    if (st->page != PAGE_PCS)
        ui_icon(s, 10, icon_y, &ui_icon_back, COL_TEXT);

    char title[128];
    if (st->page == PAGE_PCS)
        snprintf(title, sizeof(title), "Computers");
    else if (st->page == PAGE_APPS)
        snprintf(title, sizeof(title), "%s", pc_name(st));
    else
        snprintf(title, sizeof(title), "Settings");
    int tw = ui_font_w(&ui_font_title, title);
    int max_w = UI_W - 400;
    ui_font_fit(s, &ui_font_title, (UI_W - (tw < max_w ? tw : max_w)) / 2,
                (TOOLBAR_H - ui_font_title.line_h) / 2, max_w, COL_TEXT, title);

    int x = UI_W - 10 - ui_icon_settings.w;
    if (st->page == PAGE_SETTINGS) {
        const char *v = "Version " MOONLIGHT_PS4_VERSION;
        ui_font_text(s, &ui_font_body, UI_W - 20 - ui_font_w(&ui_font_body, v),
                     (TOOLBAR_H - ui_font_body.line_h) / 2, COL_TEXT, v);
        return;
    }
    ui_icon(s, x, icon_y, &ui_icon_settings, COL_TEXT);
    ui_font_text(s, &ui_font_small, x - 28, hint_y, COL_HINT, "R1");
    if (st->page == PAGE_PCS) {
        x -= 28 + 16 + ui_icon_add.w;
        ui_icon(s, x, icon_y, &ui_icon_add, COL_TEXT);
        ui_font_text(s, &ui_font_small, x - 26, hint_y, COL_HINT, "L1");
    }
}

static void draw_pcs(ui_state_t *st, ui_surface_t *s) {
    const mdns_host_t *found[MDNS_MAX_HOSTS];
    int nfound = found_cards(st, found);
    int has_saved = st->cfg->host[0] != '\0';
    int cards = has_saved + nfound;
    if (cards == 0) {
        ui_font_wrap(s, &ui_font_title, 100, TOOLBAR_H, UI_W - 200, UI_H - TOOLBAR_H, 3, COL_TEXT,
                     st->cfg->enable_mdns
                         ? "Searching for compatible hosts on your local network..."
                         : "Automatic PC discovery is disabled. Add your PC manually.");
        return;
    }

    int sel = st->sel_pc < cards ? st->sel_pc : cards - 1;
    int per_row = grid_per_row(PC_CELL_W);
    int margin = grid_margin(PC_CELL_W, cards);
    for (int i = 0; i < cards; i++) {
        int x = GRID_MIN_MARGIN + margin + (i % per_row) * PC_CELL_W;
        int y = GRID_TOP + (i / per_row) * PC_CELL_H;
        int cx = x + PC_ITEM_W / 2;
        int cy = y + 100;
        if (i == sel)
            ui_round_rect(s, x, y, PC_ITEM_W, PC_ITEM_H, 2, COL_HIGHLIGHT);
        ui_icon(s, cx - 100, y, &ui_icon_desktop, COL_TEXT);
        if (has_saved && i == 0) {
            if (!pc_online(st))
                ui_icon(s, cx - 37, cy - 37 - 18, &ui_icon_warning, COL_TEXT);
            else if (!st->server->paired)
                ui_icon(s, cx - 35, cy - 35 - 16, &ui_icon_lock, COL_TEXT);
            ui_font_wrap(s, &ui_font_name, x, y + 200, PC_ITEM_W, 0, 1, COL_TEXT, pc_name(st));
            const char *status = pc_waking(st) ? "Waking PC..." : pc_online(st) ? "Online" : "Offline";
            ui_font_text(s, &ui_font_body, cx - ui_font_w(&ui_font_body, status) / 2, y + 272,
                         pc_online(st) ? COL_ACCENT : COL_HINT, status);
        } else {
            /* Not contacted yet, so not known to be paired: the official lock. */
            ui_icon(s, cx - 35, cy - 35 - 16, &ui_icon_lock, COL_TEXT);
            ui_font_wrap(s, &ui_font_name, x, y + 200, PC_ITEM_W, 0, 2, COL_TEXT,
                         found[i - has_saved]->name);
        }
    }
}

static void draw_apps(ui_state_t *st, ui_surface_t *s) {
    if (st->apps_err == 2 || st->apps_err == 1) {
        const char *msg = st->apps_err == 2 ? "Unable to connect to the specified PC."
                                            : "Could not load the app list. Press TRIANGLE to try again.";
        ui_font_wrap(s, &ui_font_title, 100, TOOLBAR_H, UI_W - 200, UI_H - TOOLBAR_H, 3, COL_TEXT, msg);
        return;
    }
    if (st->napps == 0) {
        ui_font_wrap(s, &ui_font_title, 100, TOOLBAR_H, UI_W - 200, UI_H - TOOLBAR_H, 3, COL_TEXT,
                     "This computer doesn't seem to have any applications or some applications are hidden");
        return;
    }

    int per_row = grid_per_row(APP_CELL_W);
    int rows_visible = (UI_H - GRID_TOP - 5) / APP_CELL_H;
    int sel_row = st->sel_app / per_row;
    if (sel_row < st->app_first_row)
        st->app_first_row = sel_row;
    if (sel_row >= st->app_first_row + rows_visible)
        st->app_first_row = sel_row - rows_visible + 1;

    for (int i = st->app_first_row * per_row; i < st->napps; i++) {
        if (i / per_row - st->app_first_row >= rows_visible)
            break;
        int x, y;
        app_item_pos(st, i, &x, &y);
        if (i == st->sel_app)
            ui_round_rect(s, x, y, APP_ITEM_W, APP_ITEM_H, 2, COL_HIGHLIGHT);

        int ax = x + (APP_ITEM_W - UI_BOXART_W) / 2;
        int ay = y + APP_ART_Y;
        int placeholder = 1;
        const uint32_t *art = ui_boxart_get(st->apps[i].id, &placeholder);
        if (art)
            ui_image(s, ax, ay, UI_BOXART_W, UI_BOXART_H, art);

        int running = running_game(st) != 0 && st->apps[i].id == running_game(st);
        if (placeholder) {
            int text_h = running ? 175 : UI_BOXART_H;
            ui_font_wrap(s, &ui_font_app, ax + 20, ay + UI_BOXART_H - text_h, UI_BOXART_W - 40,
                         text_h, 5, COL_TEXT, st->apps[i].name);
        }
        if (running) {
            int ccx = ax + UI_BOXART_W / 2, ccy = ay + UI_BOXART_H / 2;
            int rx = placeholder ? ccx - 47 : ccx, ry = placeholder ? ccy - 75 : ccy - 60;
            int qx = placeholder ? ccx + 47 : ccx, qy = placeholder ? ccy - 75 : ccy + 60;
            ui_circle(s, rx, ry, 42, COL_ROUND_BTN);
            ui_icon(s, rx - 37, ry - 37, &ui_icon_play, COL_TEXT);
            ui_circle(s, qx, qy, 42, COL_ROUND_BTN);
            ui_icon(s, qx - 37, qy - 37, &ui_icon_stop, COL_TEXT);
        }
    }

    /* Scroll bar. */
    int rows = (st->napps + per_row - 1) / per_row;
    if (rows > rows_visible) {
        int track_h = UI_H - TOOLBAR_H;
        int bar_h = track_h * rows_visible / rows;
        int bar_y = TOOLBAR_H + track_h * st->app_first_row / rows;
        ui_round_rect(s, UI_W - 10, bar_y, 6, bar_h, 3, COL_TRACK);
    }
}

/* Settings layout. */
#define SET_TOP (TOOLBAR_H + 10)
#define COL1_X 10
#define COL1_W 940
#define COL2_X 960
#define COL2_W 930
#define GROUP_PAD 10
#define GROUP_TITLE_H 30
#define GROUP_GAP 10
#define ROW_GAP 4
#define CHECK_H 38
#define COMBO_H 42
#define SLIDER_W 500
#define SLIDER_H 38
#define COMBO_PAD 16
#define COMBO_ARROW_W 32
#define MENU_ROW_H 48
#define CHECK_BOX 22
#define COMBO_TEXT_Y ((COMBO_H - ui_font_body.line_h) / 2)
#define CHECK_TEXT_Y ((CHECK_H - ui_font_body.line_h) / 2)

static int combo_width(const app_config_t *c, int id) {
    const char *const *labels;
    int n = combo_options(id, &labels);
    int w = 0;
    for (int i = 0; i < n; i++) {
        int t = ui_font_w(&ui_font_body, labels[i]);
        if (t > w)
            w = t;
    }
    char cur[64];
    combo_text(c, id, cur, sizeof(cur));
    int t = ui_font_w(&ui_font_body, cur);
    if (t > w)
        w = t;
    return w + COMBO_PAD * 2 + COMBO_ARROW_W;
}

static void draw_focus(ui_surface_t *s, int x, int y, int w, int h) {
    ui_round_rect(s, x - 4, y - 2, w + 8, h + 4, 2, COL_HIGHLIGHT);
}

static void draw_combo(ui_state_t *st, ui_surface_t *s, int id, int x, int y, int focus) {
    char text[64];
    combo_text(st->cfg, id, text, sizeof(text));
    int w = combo_width(st->cfg, id);
    ui_round_rect(s, x, y, w, COMBO_H, 2, COL_CONTROL);
    if (focus)
        ui_round_rect(s, x, y, w, COMBO_H, 2, COL_HIGHLIGHT);
    ui_font_text(s, &ui_font_body, x + COMBO_PAD, y + COMBO_TEXT_Y, COL_TEXT, text);
    ui_triangle_down(s, x + w - COMBO_ARROW_W + 6, y + COMBO_H / 2 - 3, 12, COL_TEXT);
}

static void draw_check(ui_state_t *st, ui_surface_t *s, const set_item_t *it, int x, int y, int w, int focus) {
    if (focus)
        draw_focus(s, x, y, w, CHECK_H);
    int box = CHECK_BOX, bx = x + 4, by = y + (CHECK_H - box) / 2;
    if (check_get(st->cfg, it->id)) {
        ui_round_rect(s, bx, by, box, box, 2, COL_ACCENT);
        ui_icon(s, bx + 1, by + 1, &ui_icon_check, COL_ON_ACCENT);
    } else {
        ui_round_rect(s, bx, by, box, box, 2, COL_HINT);
        ui_round_rect(s, bx + 2, by + 2, box - 4, box - 4, 1, focus ? 0xFF535353u : COL_BG);
    }
    ui_font_text(s, &ui_font_body, x + 40, y + CHECK_TEXT_Y, COL_TEXT, it->text);
}

static void draw_slider(ui_state_t *st, ui_surface_t *s, int x, int y, int focus) {
    const app_config_t *c = st->cfg;
    int max = c->unlock_bitrate ? BITRATE_MAX_UNLOCKED_KBPS : BITRATE_MAX_KBPS;
    int v = c->stream.bitrate;
    if (v > max) v = max;
    int w = SLIDER_W - 24;
    int pos = (int)((long)(v - BITRATE_MIN_KBPS) * w / (max - BITRATE_MIN_KBPS));
    int tx = x + 12, cy = y + SLIDER_H / 2;
    if (focus)
        draw_focus(s, x, y, SLIDER_W, SLIDER_H);
    ui_rect(s, tx, cy - 1, w, 2, 0xFF7E7E7Eu);
    ui_rect(s, tx, cy - 1, pos, 2, COL_ACCENT);
    ui_circle(s, tx + pos, cy, 10, COL_ACCENT);
}

/* Which item the open combo belongs to is drawn last, above its neighbours. */
static void draw_combo_popup(ui_state_t *st, ui_surface_t *s, int id, int x, int y) {
    const char *const *labels;
    int n = combo_options(id, &labels);
    int w = combo_width(st->cfg, id);
    int h = n * COMBO_H + 8;
    if (y + h > UI_H - 10)
        y = UI_H - 10 - h;
    draw_dialog_box(s, x, y, w, h);
    for (int i = 0; i < n; i++) {
        int ry = y + 4 + i * COMBO_H;
        if (i == st->combo_sel)
            ui_blend_rect(s, x, ry, w, COMBO_H, COL_HIGHLIGHT);
        int cur = combo_get(st->cfg, id) == i;
        ui_font_text(s, &ui_font_body, x + COMBO_PAD, ry + COMBO_TEXT_Y, cur ? COL_ACCENT : COL_TEXT, labels[i]);
    }
}

static void draw_settings(ui_state_t *st, ui_surface_t *s) {
    const app_config_t *c = st->cfg;
    int col_x = COL1_X, col_w = COL1_W;
    int y = SET_TOP;
    int group_y = -1;
    int prev_row_end = 0, prev_row_y = 0;
    int popup_id = 0, popup_x = 0, popup_y = 0;

    /* Group frames sit under the content, so find each group's end in a first pass. */
    for (int pass = 0; pass < 2; pass++) {
        col_x = COL1_X;
        col_w = COL1_W;
        y = SET_TOP;
        group_y = -1;

#define CLOSE_GROUP()                                                              \
    do {                                                                           \
        if (group_y >= 0) {                                                        \
            y += GROUP_PAD;                                                        \
            if (pass == 0) {                                                       \
                int fy = group_y + GROUP_TITLE_H;                                  \
                ui_round_rect(s, col_x, fy, col_w, y - fy, 2, COL_FRAME);          \
                ui_round_rect(s, col_x + 1, fy + 1, col_w - 2, y - fy - 2, 2, COL_BG); \
            }                                                                      \
            y += GROUP_GAP;                                                        \
            group_y = -1;                                                          \
        }                                                                          \
    } while (0)

        for (int i = 0; i < ITEM_COUNT; i++) {
            const set_item_t *it = &k_items[i];
            int ix = col_x + GROUP_PAD, iw = col_w - GROUP_PAD * 2;
            int focus = pass == 1 && i == st->sel_item;
            switch (it->type) {
            case IT_COLUMN:
                CLOSE_GROUP();
                col_x = COL2_X;
                col_w = COL2_W;
                y = SET_TOP;
                break;
            case IT_GROUP:
                CLOSE_GROUP();
                group_y = y;
                if (pass == 1)
                    ui_font_text(s, &ui_font_body, col_x, y + 1, COL_GROUP, it->text);
                y += GROUP_TITLE_H + GROUP_PAD;
                break;
            case IT_LABEL:
                if (pass == 1) {
                    char title[64];
                    if (it->id == SET_BITRATE)
                        snprintf(title, sizeof(title), "%s %.1f Mbps", it->text,
                                 c->stream.bitrate / 1000.0);
                    else
                        snprintf(title, sizeof(title), "%s", it->text);
                    ui_font_text(s, &ui_font_body, ix, y, COL_TEXT, title);
                    ui_font_text(s, &ui_font_small, ix, y + ui_font_body.line_h, COL_HINT, it->desc);
                }
                y += ui_font_body.line_h + ui_font_small.line_h + ROW_GAP;
                break;
            case IT_COMBO:
            case IT_COMBO_SAME_ROW: {
                int cx = ix;
                if (it->type == IT_COMBO_SAME_ROW) {
                    cx = prev_row_end + ROW_GAP;
                    y = prev_row_y;
                }
                if (pass == 1)
                    draw_combo(st, s, it->id, cx, y, focus);
                if (focus && st->combo_open) {
                    popup_id = it->id;
                    popup_x = cx;
                    popup_y = y;
                }
                prev_row_y = y;
                prev_row_end = cx + combo_width(c, it->id);
                y += COMBO_H + ROW_GAP;
                break;
            }
            case IT_SLIDER:
                if (pass == 1)
                    draw_slider(st, s, ix, y, focus);
                prev_row_y = y;
                prev_row_end = ix + SLIDER_W;
                y += SLIDER_H + ROW_GAP;
                break;
            case IT_BUTTON_SAME_ROW:
                if (pass == 1 && c->stream.bitrate != bitrate_default(c)) {
                    char label[48];
                    snprintf(label, sizeof(label), "Use Default (%d Mbps)", bitrate_default(c) / 1000);
                    int bw = ui_font_w(&ui_font_body, label) + COMBO_PAD * 2;
                    int bx = prev_row_end + ROW_GAP + 8;
                    ui_round_rect(s, bx, prev_row_y - 2, bw, COMBO_H, 2, COL_CONTROL);
                    if (focus)
                        ui_round_rect(s, bx, prev_row_y - 2, bw, COMBO_H, 2, COL_HIGHLIGHT);
                    ui_font_text(s, &ui_font_body, bx + COMBO_PAD, prev_row_y - 2 + COMBO_TEXT_Y, COL_TEXT, label);
                }
                break;
            case IT_CHECK:
                if (pass == 1)
                    draw_check(st, s, it, ix, y, iw, focus);
                y += CHECK_H;
                break;
            case IT_FIELD: {
                if (pass == 1) {
                    if (focus)
                        draw_focus(s, ix, y, 400, COMBO_H);
                    ui_font_fit(s, &ui_font_body, ix + 4, y + COMBO_TEXT_Y, 392,
                                c->debug_host[0] ? COL_TEXT : COL_HINT,
                                c->debug_host[0] ? c->debug_host : "None");
                    ui_rect(s, ix, y + COMBO_H - 2, 400, 2, focus ? COL_ACCENT : COL_HINT);
                }
                y += COMBO_H + ROW_GAP;
                break;
            }
            }
        }
        CLOSE_GROUP();
#undef CLOSE_GROUP
    }

    if (popup_id)
        draw_combo_popup(st, s, popup_id, popup_x, popup_y);
}

static void draw_popup(ui_state_t *st, ui_surface_t *s) {
    ui_popup_t *m = &st->popup;
    int w = 0;
    for (int i = 0; i < m->n; i++) {
        int t = ui_font_w(&ui_font_body, m->items[i].label);
        if (t > w)
            w = t;
    }
    w += 48;
    int h = m->n * MENU_ROW_H + 8;
    draw_dialog_box(s, m->x, m->y, w, h);
    for (int i = 0; i < m->n; i++) {
        int ry = m->y + 4 + i * MENU_ROW_H;
        if (i == m->sel && m->items[i].action != ACT_NONE)
            ui_blend_rect(s, m->x, ry, w, MENU_ROW_H, COL_HIGHLIGHT);
        ui_font_text(s, &ui_font_body, m->x + 24, ry + (MENU_ROW_H - ui_font_body.line_h) / 2,
                     m->items[i].action == ACT_NONE ? COL_DISABLED : COL_TEXT, m->items[i].label);
    }
}

static void draw_dialog(ui_state_t *st, ui_surface_t *s) {
    char line[256];
    const char *name = active_app_name(st);
    if (st->dialog == DLG_DELETE_PC)
        snprintf(line, sizeof(line), "Are you sure you want to remove '%s'?", pc_name(st));
    else
        snprintf(line, sizeof(line), "Are you sure you want to quit %s? Any unsaved progress will be lost.",
                 name ? name : "this app");

    ui_blend_rect(s, 0, 0, UI_W, UI_H, COL_SCRIM);
    const int w = 780, h = 210, x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    draw_dialog_box(s, x, y, w, h);
    ui_font_para(s, &ui_font_body, x + 24, y + 24, w - 48, COL_TEXT, line);
    const char *yes = "X  Yes", *no = "O  No";
    int nx = x + w - 24 - ui_font_w(&ui_font_body, no);
    ui_font_text(s, &ui_font_body, nx, y + h - 50, COL_ACCENT, no);
    ui_font_text(s, &ui_font_body, nx - 40 - ui_font_w(&ui_font_body, yes), y + h - 50, COL_ACCENT, yes);
}

static void draw_osk(ui_state_t *st, ui_surface_t *s) {
    const int w = 800, h = 490, x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_blend_rect(s, 0, 0, UI_W, UI_H, COL_SCRIM);
    draw_dialog_box(s, x, y, w, h);

    const char *label = st->osk_target == OSK_TARGET_HOST ? "Enter the IP address of your host PC:"
                      : st->osk_target == OSK_TARGET_FPS  ? "Custom FPS (10 to 60):"
                                                          : "Debug host:";
    ui_font_text(s, &ui_font_body, x + 24, y + 24, COL_TEXT, label);

    char line[CONFIG_MAX_HOST + 2];
    snprintf(line, sizeof(line), "%s_", st->osk_buf);
    ui_font_fit(s, &ui_font_title, x + 24, y + 58, w - 48, COL_TEXT, line);
    ui_rect(s, x + 24, y + 104, w - 48, 2, COL_ACCENT);

    const int cell = 68, gx = x + (w - cell * OSK_COLS) / 2, gy = y + 128;
    for (int r = 0; r < osk_rows(st); r++) {
        for (int c = 0; c < OSK_COLS; c++) {
            int kx = gx + c * cell, ky = gy + r * cell;
            int sel = r == st->osk_row && c == st->osk_col;
            ui_round_rect(s, kx + 3, ky + 3, cell - 6, cell - 6, 3, sel ? COL_ACCENT : COL_CONTROL);
            char ch[2] = { k_osk_chars[r][c], 0 };
            ui_font_text(s, &ui_font_title, kx + (cell - ui_font_w(&ui_font_title, ch)) / 2,
                         ky + (cell - ui_font_title.line_h) / 2,
                         sel ? COL_ON_ACCENT : COL_TEXT, ch);
        }
    }

    const char *hint = "X  type      []  erase      O  Cancel      OPTIONS  OK";
    ui_font_text(s, &ui_font_body, x + (w - ui_font_w(&ui_font_body, hint)) / 2, y + h - 44, COL_ACCENT, hint);
}

static void draw_all(ui_state_t *st, ui_surface_t *s) {
    ui_clear(s, COL_BG);

    if (st->page == PAGE_PCS)
        draw_pcs(st, s);
    else if (st->page == PAGE_APPS)
        draw_apps(st, s);
    else
        draw_settings(st, s);
    draw_toolbar(st, s);

    if (st->popup.active)
        draw_popup(st, s);
    if (st->dialog != DLG_NONE)
        draw_dialog(st, s);
    if (st->osk_active)
        draw_osk(st, s);
}

int ui_menu_run(app_config_t *cfg, gs_server_t *server, const char *config_dir) {
    ui_state_t st;
    memset(&st, 0, sizeof(st));
    st.cfg = cfg;
    st.server = server;
    st.config_dir = config_dir;
    st.result = -1000;
    /* First focusable settings item. */
    while (!item_focusable(cfg, st.sel_item))
        st.sel_item++;
    /* No host / no server: nothing but the Computers page is usable. */
    st.page = (!cfg->host[0] || !server) ? PAGE_PCS : s_page;
    ui_boxart_set_host(cfg->host);

    if (video_ui_begin(UI_W, UI_H) != 0) {
        LOGE("ui: video_ui_begin failed");
        return -1;
    }
    LOGI("ui: menu open (server=%s)", server ? "OK" : "NULL");

    /* Offscreen staging: drawing NEVER touches the on-screen buffer. */
    size_t stage_pitch = (size_t)UI_W * 4u;
    size_t stage_bytes = stage_pitch * (size_t)UI_H;
    uint8_t *staging = (uint8_t *)malloc(stage_bytes);
    if (!staging) {
        LOGE("ui: staging malloc failed");
        video_ui_end();
        return -1;
    }

    /* First frame NOW: don't hide splash until we have an image (avoids black). */
    {
        ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
        draw_all(&st, &s);
        for (int i = 0; i < 3; i++) {
            if (video_ui_present(staging, (int)stage_pitch) == 0)
                break;
            usleep(8000);
        }
        ui_hide_splash_once();
    }

    fetch_apps(&st);
    preselect_app(&st);

    /*
     * After OPTIONS+TOUCHPAD from stream, OPTIONS is still held. If s_menu_prev=0,
     * the OPTIONS/X edge would relaunch instantly. Wait for release + absorb.
     */
    input_menu_wait_release(
        MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_TRIANGLE,
        2000);
    input_menu_absorb();

    int dirty = 1;
    int no_pad = 0;
    int idle_frames = 0;
    int grace = 45; /* ~0.7 s without accepting launch */
    int shown_online = pc_online(&st);
    int shown_waking = 0;

    while (st.result == -1000) {
        unsigned pr = 0, held = 0;
        if (input_menu_poll(&pr, &held) != 0) {
            if (++no_pad > 180) {
                if (cfg->host[0] && st.napps > 0) {
                    LOGW("ui: no pad; auto-launching app '%s'", cfg->app_name);
                    st.result = UI_MENU_LAUNCH;
                    break;
                }
                no_pad = 0; /* no host: stay in menu */
            }
        } else {
            no_pad = 0;
        }

        if (grace > 0) {
            grace--;
            /* During grace: navigate OK, but don't launch / change host. */
            pr &= ~(MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_OPTIONS |
                    MENU_BTN_TRIANGLE);
            if (held & (MENU_BTN_OPTIONS | MENU_BTN_CROSS | MENU_BTN_CIRCLE))
                input_menu_absorb();
        }

        if (pr) {
            if (st.osk_active)
                osk_input(&st, pr);
            else if (st.dialog != DLG_NONE)
                dialog_input(&st, pr);
            else if (st.popup.active)
                popup_input(&st, pr);
            else if (st.page == PAGE_PCS)
                pcs_input(&st, pr);
            else if (st.page == PAGE_APPS)
                apps_input(&st, pr);
            else if (st.combo_open)
                combo_input(&st, pr);
            else
                settings_input(&st, pr);
            dirty = 1;
        }

        /* Exit NOW: don't redraw or flip (SubmitFlip VSYNC can hang
         * with high pending / cur=-1 → eternal black screen). */
        if (st.result != -1000) {
            LOGI("ui: leaving menu (result=%d)", st.result);
            break;
        }

        /* On every page, or a return to Computers would find the PC's last
         * reply too old and flash Offline. */
        if (cfg->enable_mdns && mdns_poll())
            dirty = 1;

        int online_now = pc_online(&st);
        int waking_now = pc_waking(&st);
        if (online_now != shown_online || waking_now != shown_waking) {
            shown_online = online_now;
            shown_waking = waking_now;
            dirty = 1;
            /* The card behind an open menu can change meaning: close the menu. */
            if (st.page == PAGE_PCS)
                st.popup.active = 0;
        }

        /* A PC that was unreachable when the menu opened has no connection yet:
         * make one as soon as it answers (or, after a wake, once anyway). */
        if (!st.server && cfg->host[0] && !st.host_dirty && st.page == PAGE_PCS &&
            !st.osk_active && !st.popup.active && st.dialog == DLG_NONE) {
            uint64_t now = sceKernelGetProcessTime();
            int age = cfg->enable_mdns ? mdns_host_age_ms(cfg->host, NULL) : -1;
            int answered = age >= 0 && age <= PC_OFFLINE_AFTER_MS;
            int wake_due = st.wake_sent_us && !st.wake_retried &&
                           now - st.wake_sent_us > PC_WAKE_RETRY_US;
            if (wake_due)
                st.wake_retried = 1;
            if ((answered && now - s_last_auto_reconnect_us > PC_RECONNECT_MIN_GAP_US) || wake_due) {
                s_last_auto_reconnect_us = now;
                LOGI("ui: %s, reconnecting", answered ? "PC answered" : "wake attempt elapsed");
                st.result = UI_MENU_RECONNECT;
                break;
            }
        }

        /* Box art arrives one image at a time so the pad stays responsive. */
        if (!pr && st.page == PAGE_APPS && st.server && !st.osk_active &&
            ui_boxart_load_next(st.server, st.app_ids, st.napps))
            dirty = 1;

        if (dirty || ++idle_frames >= 3) {
            idle_frames = 0;
            ui_surface_t s = { staging, (int)stage_pitch, UI_W, UI_H };
            if (dirty)
                draw_all(&st, &s);
            if (video_ui_present(staging, (int)stage_pitch) != 0)
                dirty = 1;
            else {
                dirty = 0;
                ui_hide_splash_once();
            }
        }
        usleep(8000);
    }

    mdns_stop();

    /* Never reopen on Settings: come back to the page it was opened from. */
    s_page = st.page == PAGE_SETTINGS ? st.settings_return : st.page;

    LOGI("ui: freeing staging...");
    free(staging);
    LOGI("ui: closing present...");
    video_ui_end();
    LOGI("ui: menu closed => %d (app='%s' host='%s')",
         st.result, cfg->app_name, cfg->host);
    return st.result;
}

