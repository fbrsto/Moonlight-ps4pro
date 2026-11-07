#include "input_pad.h"
#include "../log.h"

#include <Limelight.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

static int32_t s_pad = -1;
static atomic_bool s_quit;
static uint64_t s_combo_start_us;
static int s_combo_fired;
static int s_menu_requested;
static int s_announced;
static int32_t s_pad_user = -1;
static int s_rumble_error_logged;

/* Extra pads (controllers 1..3), one per other logged-in PS4 user. */
#define EXTRA_PADS 3
#define PAD_RESCAN_US 2000000ull
typedef struct {
    int32_t handle; /* <= 0 when unused; valid only after input_init */
    int32_t user;
    int announced;
    int connected;
} extra_pad_t;
static extra_pad_t s_extra[EXTRA_PADS];
static uint64_t s_last_rescan_us;

static input_options_t s_opts;

/* Feedback and sensors, per controller (0 is the primary pad). */
#define PAD_COUNT (EXTRA_PADS + 1)
#define MAX_TOUCHES ORBIS_PAD_MAX_TOUCH_NUM
#define DEFAULT_TOUCH_W 1920
#define DEFAULT_TOUCH_H 943
#define RUMBLE_MIN_INTERVAL_US 50000ull
#define RUMBLE_MIN_CHANGE 3 /* of 255: no one feels less */
#define RAD_TO_DEG 57.29578f
#define G_TO_MS2 9.80665f
typedef struct {
    int active;
    uint8_t id;
    uint16_t x, y;
} pad_touch_t;
static pad_touch_t s_touches[PAD_COUNT][MAX_TOUCHES];
static int s_touch_w = DEFAULT_TOUCH_W, s_touch_h = DEFAULT_TOUCH_H;
/* Written by the host's callbacks, read by the poll loop. */
static atomic_uint s_accel_interval_us[PAD_COUNT];
static atomic_uint s_gyro_interval_us[PAD_COUNT];
static uint64_t s_accel_next_us[PAD_COUNT];
static uint64_t s_gyro_next_us[PAD_COUNT];
static atomic_int s_lightbar_changed[PAD_COUNT];
static int s_rumble_on[PAD_COUNT];
static atomic_uint s_rumble_wanted[PAD_COUNT];
static unsigned s_rumble_sent[PAD_COUNT];
static uint64_t s_rumble_sent_us[PAD_COUNT];
static atomic_uint s_rumble_requests;
static unsigned s_rumble_sends;

/* Gamepad-as-mouse + touchpad state (primary pad only). */
#define STICK_DEADZONE 20
#define TRIGGER_CLICK_LEVEL 64
#define MOUSE_MOVE_DIV 1400   /* full deflection: ~8 px per 4 ms poll */
static int s_accum_x, s_accum_y;
static int s_btn_left, s_btn_right;

/* Menu-mode state (edges + D-pad auto-repeat). */
static unsigned s_menu_prev;
static uint64_t s_menu_rpt_start_us;
static uint64_t s_menu_rpt_last_us;

static uint64_t now_us(void) {
    // sceKernelGetProcessTime is in us per scene docs.
    extern uint64_t sceKernelGetProcessTime(void);
    return sceKernelGetProcessTime();
}

// PS4 stick: 0..255 with center ~128. Moonlight expects -32768..32767.
static short stick_to_short(uint8_t v) {
    int centered = ((int)v - 128) * 256;
    if (centered < -32768) centered = -32768;
    if (centered > 32767) centered = 32767;
    return (short)centered;
}

static short stick_to_short_inverted(uint8_t v) {
    int inverted = -(int)stick_to_short(v);
    if (inverted > 32767) inverted = 32767;
    return (short)inverted;
}

int input_init(void) {
    atomic_store(&s_quit, false);
    s_combo_start_us = 0;
    s_announced = 0;

    int rc = scePadInit();
    if (rc < 0) {
        LOGE("input: scePadInit failed: 0x%08x", rc);
        return -1;
    }

    int32_t userId = 0;
    sceUserServiceGetInitialUser(&userId);
    s_pad = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (s_pad < 0) {
        LOGE("input: scePadOpen failed: 0x%08x", s_pad);
        return -1;
    }
    s_pad_user = userId;
    OrbisPadInformation info;
    memset(&info, 0, sizeof(info));
    if (scePadGetControllerInformation(s_pad, &info) >= 0 && info.touchResolutionX && info.touchResolutionY) {
        s_touch_w = info.touchResolutionX;
        s_touch_h = info.touchResolutionY;
    }
    memset(s_extra, 0, sizeof(s_extra));
    s_last_rescan_us = 0;

    LOGI("input: pad open (handle=%d, user=%d)", s_pad, userId);
    return 0;
}

static void close_extra(extra_pad_t *e) {
    if (e->handle > 0)
        scePadClose(e->handle);
    memset(e, 0, sizeof(*e));
}

void input_shutdown(void) {
    for (int i = 0; i < EXTRA_PADS; i++)
        close_extra(&s_extra[i]);
    if (s_pad >= 0) {
        scePadClose(s_pad);
        s_pad = -1;
    }
}

void input_set_options(const input_options_t *opts) {
    s_opts = *opts;
}

/* Match open extra pads against the logged-in users. */
static void rescan_extra_pads(void) {
    if (s_pad < 0)
        return;

    OrbisUserServiceLoginUserIdList list;
    memset(&list, 0, sizeof(list));
    int have_list = s_opts.multi_controller &&
                    sceUserServiceGetLoginUserIdList(&list) == 0;

    /* Drop pads whose user logged out, or all of them if the option is off. */
    for (int i = 0; i < EXTRA_PADS; i++) {
        extra_pad_t *e = &s_extra[i];
        if (e->handle <= 0)
            continue;
        int still_in = 0;
        for (int u = 0; have_list && u < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; u++)
            if (list.userId[u] == e->user)
                still_in = 1;
        if (!still_in) {
            LOGI("input: closing extra pad %d (user=%d)", i + 1, e->user);
            close_extra(e);
        }
    }
    if (!have_list)
        return;

    for (int u = 0; u < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; u++) {
        int32_t user = list.userId[u];
        if (user == ORBIS_USER_SERVICE_USER_ID_INVALID || user <= 0 || user == s_pad_user)
            continue;
        int known = 0, free_slot = -1;
        for (int i = 0; i < EXTRA_PADS; i++) {
            if (s_extra[i].handle > 0 && s_extra[i].user == user)
                known = 1;
            else if (s_extra[i].handle <= 0 && free_slot < 0)
                free_slot = i;
        }
        if (known || free_slot < 0)
            continue;
        int32_t h = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
        if (h <= 0) {
            LOGW("input: scePadOpen(user=%d) failed: 0x%08x", user, h);
            continue;
        }
        s_extra[free_slot].handle = h;
        s_extra[free_slot].user = user;
        LOGI("input: extra pad %d open (handle=%d, user=%d)", free_slot + 1, h, user);
    }
}

static int current_mask(void) {
    int mask = 0x1; /* the primary pad is always announced */
    for (int i = 0; i < EXTRA_PADS; i++)
        if (s_extra[i].handle > 0 && s_extra[i].connected)
            mask |= 1 << (i + 1);
    return mask;
}

int input_active_mask(void) {
    rescan_extra_pads();
    for (int i = 0; i < EXTRA_PADS; i++) {
        if (s_extra[i].handle <= 0)
            continue;
        OrbisPadData pad;
        memset(&pad, 0, sizeof(pad));
        s_extra[i].connected = scePadReadState(s_extra[i].handle, &pad) >= 0 && pad.connected;
    }
    return current_mask();
}

void input_request_quit(void) {
    atomic_store(&s_quit, true);
}

/* The host sends 0..65535; the pad takes 0..255. A faint request must not become 0. */
static uint8_t rumble_level(unsigned short value) {
    return (value && value < 256) ? 1 : (uint8_t)(value >> 8);
}

/* Open pad for a controller number, or -1. */
static int32_t pad_handle(unsigned controller) {
    if (controller == 0)
        return s_pad;
    if (controller < PAD_COUNT && s_extra[controller - 1].handle > 0)
        return s_extra[controller - 1].handle;
    return -1;
}

/* Encoded as (large << 8) | small: one value the poll loop reads atomically. */
void input_set_rumble(unsigned short controller, unsigned short low_freq, unsigned short high_freq) {
    if (controller >= PAD_COUNT || !s_opts.rumble)
        return;
    atomic_store(&s_rumble_wanted[controller],
                 ((unsigned)rumble_level(low_freq) << 8) | rumble_level(high_freq));
    atomic_fetch_add(&s_rumble_requests, 1);
}

static void send_rumble(int number, int32_t handle, unsigned level, uint64_t now) {
    OrbisPadVibeParam vibe = {
        .lgMotor = (uint8_t)(level >> 8),
        .smMotor = (uint8_t)level,
    };
    int32_t rc = scePadSetVibration(handle, &vibe);
    s_rumble_sent[number] = level;
    s_rumble_sent_us[number] = now;
    s_rumble_sends++;
    /* Only starts and stops: a game can ask for many of these per second. */
    int on = level != 0;
    if (on != s_rumble_on[number]) {
        s_rumble_on[number] = on;
        LOGI("input: rumble %s on pad %d (large=%u small=%u) => 0x%08x", on ? "start" : "stop",
             number, level >> 8, level & 0xFF, (unsigned)rc);
    }
    if (rc < 0 && !s_rumble_error_logged) {
        s_rumble_error_logged = 1;
        LOGW("input: scePadSetVibration failed: 0x%08x", rc);
    }
}

static int rumble_close(unsigned a, unsigned b) {
    int large = (int)(a >> 8) - (int)(b >> 8);
    int small = (int)(a & 0xFF) - (int)(b & 0xFF);
    return abs(large) <= RUMBLE_MIN_CHANGE && abs(small) <= RUMBLE_MIN_CHANGE;
}

/* Each send is a Bluetooth report to the pad, so games must not set the pace. Starts
 * and stops go at once, so that short jolts stay short; the rest waits its turn. */
static void apply_rumble(uint64_t now) {
    for (int i = 0; i < PAD_COUNT; i++) {
        unsigned wanted = atomic_load(&s_rumble_wanted[i]);
        unsigned sent = s_rumble_sent[i];
        if (wanted == sent)
            continue;
        if (wanted != 0 && sent != 0 &&
            (now - s_rumble_sent_us[i] < RUMBLE_MIN_INTERVAL_US || rumble_close(wanted, sent)))
            continue;
        int32_t handle = pad_handle((unsigned)i);
        if (handle >= 0)
            send_rumble(i, handle, wanted, now);
    }
}

void input_take_rumble_stats(unsigned *requests, unsigned *sends) {
    *requests = atomic_exchange(&s_rumble_requests, 0);
    *sends = s_rumble_sends;
    s_rumble_sends = 0;
}

void input_set_motion_rate(unsigned short controller, unsigned char type, unsigned short rate_hz) {
    int32_t handle = pad_handle(controller);
    if (handle < 0 || !s_opts.motion)
        return;
    unsigned interval = rate_hz ? 1000000u / rate_hz : 0;
    if (type == LI_MOTION_TYPE_ACCEL)
        atomic_store(&s_accel_interval_us[controller], interval);
    else if (type == LI_MOTION_TYPE_GYRO)
        atomic_store(&s_gyro_interval_us[controller], interval);
    else
        return;
    int wanted = atomic_load(&s_accel_interval_us[controller]) ||
                 atomic_load(&s_gyro_interval_us[controller]);
    scePadSetMotionSensorState(handle, wanted);
}

void input_set_led(unsigned short controller, unsigned char r, unsigned char g, unsigned char b) {
    int32_t handle = pad_handle(controller);
    if (handle < 0 || !s_opts.lightbar)
        return;
    OrbisPadColor color = { .r = r, .g = g, .b = b, .a = 255 };
    if (scePadSetLightBar(handle, &color) >= 0)
        atomic_store(&s_lightbar_changed[controller], 1);
}

void input_stop_feedback(void) {
    for (unsigned short i = 0; i < PAD_COUNT; i++) {
        int32_t handle = pad_handle(i);
        if (handle < 0)
            continue;
        atomic_store(&s_rumble_wanted[i], 0);
        if (s_rumble_sent[i])
            send_rumble(i, handle, 0, now_us());
        if (atomic_load(&s_accel_interval_us[i]) || atomic_load(&s_gyro_interval_us[i]))
            scePadSetMotionSensorState(handle, 0);
        atomic_store(&s_accel_interval_us[i], 0);
        atomic_store(&s_gyro_interval_us[i], 0);
        if (atomic_exchange(&s_lightbar_changed[i], 0))
            scePadResetLightBar(handle);
    }
}

bool input_take_menu_request(void) {
    bool requested = s_menu_requested != 0;
    s_menu_requested = 0;
    return requested;
}

void input_reset(void) {
    atomic_store(&s_quit, false);
    s_combo_start_us = 0;
    s_combo_fired = 0;
    s_menu_requested = 0;
    s_announced = 0;
    s_menu_prev = 0;
    s_menu_rpt_start_us = 0;
    s_menu_rpt_last_us = 0;
    for (int i = 0; i < EXTRA_PADS; i++)
        s_extra[i].announced = 0;
    s_accum_x = s_accum_y = 0;
    s_btn_left = s_btn_right = 0;
    memset(s_touches, 0, sizeof(s_touches));
    memset(s_accel_next_us, 0, sizeof(s_accel_next_us));
    memset(s_gyro_next_us, 0, sizeof(s_gyro_next_us));
    for (int i = 0; i < PAD_COUNT; i++) {
        atomic_store(&s_accel_interval_us[i], 0);
        atomic_store(&s_gyro_interval_us[i], 0);
    }
}

static unsigned pad_to_menu_mask(const OrbisPadData *pad) {
    unsigned h = 0;
    if (pad->buttons & ORBIS_PAD_BUTTON_UP)        h |= MENU_BTN_UP;
    if (pad->buttons & ORBIS_PAD_BUTTON_DOWN)      h |= MENU_BTN_DOWN;
    if (pad->buttons & ORBIS_PAD_BUTTON_LEFT)      h |= MENU_BTN_LEFT;
    if (pad->buttons & ORBIS_PAD_BUTTON_RIGHT)     h |= MENU_BTN_RIGHT;
    if (pad->buttons & ORBIS_PAD_BUTTON_CROSS)     h |= MENU_BTN_CROSS;
    if (pad->buttons & ORBIS_PAD_BUTTON_CIRCLE)    h |= MENU_BTN_CIRCLE;
    if (pad->buttons & ORBIS_PAD_BUTTON_SQUARE)    h |= MENU_BTN_SQUARE;
    if (pad->buttons & ORBIS_PAD_BUTTON_TRIANGLE)  h |= MENU_BTN_TRIANGLE;
    if (pad->buttons & ORBIS_PAD_BUTTON_L1)        h |= MENU_BTN_L1;
    if (pad->buttons & ORBIS_PAD_BUTTON_R1)        h |= MENU_BTN_R1;
    if (pad->buttons & ORBIS_PAD_BUTTON_OPTIONS)   h |= MENU_BTN_OPTIONS;
    return h;
}

void input_menu_absorb(void) {
    if (s_pad < 0)
        return;
    OrbisPadData pad;
    memset(&pad, 0, sizeof(pad));
    if (scePadReadState(s_pad, &pad) < 0)
        return;
    s_menu_prev = pad_to_menu_mask(&pad);
    /* Touchpad is not in menu mask but OPTIONS is: already covered. */
    s_menu_rpt_start_us = 0;
    s_menu_rpt_last_us = 0;
}

void input_menu_wait_release(unsigned mask, int timeout_ms) {
    if (s_pad < 0 || !mask)
        return;
    for (int i = 0; i < timeout_ms; i++) {
        OrbisPadData pad;
        memset(&pad, 0, sizeof(pad));
        if (scePadReadState(s_pad, &pad) == 0) {
            unsigned h = pad_to_menu_mask(&pad);
            if (pad.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD)
                h |= MENU_BTN_OPTIONS; /* treat touchpad as "held" */
            if ((h & mask) == 0) {
                s_menu_prev = h;
                return;
            }
            s_menu_prev = h; /* keep absorbing while held */
        }
        sceKernelUsleep(1000);
    }
    input_menu_absorb();
}

int input_menu_poll(unsigned *pressed, unsigned *held) {
    *pressed = 0;
    *held = 0;
    if (s_pad < 0)
        return -1;

    OrbisPadData pad;
    memset(&pad, 0, sizeof(pad));
    if (scePadReadState(s_pad, &pad) < 0)
        return -1;

    unsigned h = pad_to_menu_mask(&pad);
    unsigned edges = h & ~s_menu_prev;

    /* Auto-repeat D-pad only: 350 ms delay, then every 90 ms. */
    const unsigned dpad = MENU_BTN_UP | MENU_BTN_DOWN | MENU_BTN_LEFT | MENU_BTN_RIGHT;
    uint64_t now = now_us();
    if (h & dpad) {
        if (edges & dpad) {
            s_menu_rpt_start_us = now;
            s_menu_rpt_last_us = now;
        } else if (now - s_menu_rpt_start_us > 350000ull &&
                   now - s_menu_rpt_last_us > 90000ull) {
            edges |= h & dpad;
            s_menu_rpt_last_us = now;
        }
    } else {
        s_menu_rpt_start_us = 0;
    }

    s_menu_prev = h;
    *pressed = edges;
    *held = h;
    return 0;
}

bool input_should_quit(void) {
    return atomic_load(&s_quit);
}

static void announce_controller(int number, int mask) {
    // DualShock 4: PS type; the options decide which extras the host may use.
    uint16_t caps = LI_CCAP_ANALOG_TRIGGERS;
    if (s_opts.rumble)
        caps |= LI_CCAP_RUMBLE;
    if (s_opts.motion)
        caps |= LI_CCAP_ACCEL | LI_CCAP_GYRO;
    if (s_opts.touchpad)
        caps |= LI_CCAP_TOUCHPAD;
    if (s_opts.lightbar)
        caps |= LI_CCAP_RGB_LED;
    LiSendControllerArrivalEvent(
        (uint8_t)number, (uint16_t)mask, LI_CTYPE_PS,
        0xFFFFFFFF, // generic supported buttons
        caps);
    LiSendControllerBatteryEvent((uint8_t)number, LI_BATTERY_STATE_FULL, 100);
}

static void send_controller(int number, int mask, const OrbisPadData *pad, uint32_t raw) {
    int a = A_FLAG, b = B_FLAG, x = X_FLAG, y = Y_FLAG;
    if (s_opts.swap_face_buttons) {
        a = B_FLAG; b = A_FLAG;
        x = Y_FLAG; y = X_FLAG;
    }

    int buttons = 0;
    if (raw & ORBIS_PAD_BUTTON_UP)        buttons |= UP_FLAG;
    if (raw & ORBIS_PAD_BUTTON_DOWN)      buttons |= DOWN_FLAG;
    if (raw & ORBIS_PAD_BUTTON_LEFT)      buttons |= LEFT_FLAG;
    if (raw & ORBIS_PAD_BUTTON_RIGHT)     buttons |= RIGHT_FLAG;
    if (raw & ORBIS_PAD_BUTTON_CROSS)     buttons |= a;
    if (raw & ORBIS_PAD_BUTTON_CIRCLE)    buttons |= b;
    if (raw & ORBIS_PAD_BUTTON_SQUARE)    buttons |= x;
    if (raw & ORBIS_PAD_BUTTON_TRIANGLE)  buttons |= y;
    if (raw & ORBIS_PAD_BUTTON_L1)        buttons |= LB_FLAG;
    if (raw & ORBIS_PAD_BUTTON_R1)        buttons |= RB_FLAG;
    if (raw & ORBIS_PAD_BUTTON_L3)        buttons |= LS_CLK_FLAG;
    if (raw & ORBIS_PAD_BUTTON_R3)        buttons |= RS_CLK_FLAG;
    if (raw & ORBIS_PAD_BUTTON_OPTIONS)   buttons |= PLAY_FLAG;
    /* The PS4 keeps Share for itself, and games read either button: send both. */
    if (raw & ORBIS_PAD_BUTTON_TOUCH_PAD)
        buttons |= BACK_FLAG | TOUCHPAD_FLAG;

    // Y axis inverted: PS4 grows downward, Moonlight upward.
    short lx = stick_to_short(pad->leftStick.x);
    short ly = stick_to_short_inverted(pad->leftStick.y);
    short rx = stick_to_short(pad->rightStick.x);
    short ry = stick_to_short_inverted(pad->rightStick.y);

    LiSendMultiControllerEvent((short)number, (short)mask, buttons,
                               pad->analogButtons.l2, pad->analogButtons.r2,
                               lx, ly, rx, ry);
}

/* Polls do not land exactly on the requested rate: keep to a schedule so that the
 * average holds, and restart it after a pause instead of sending a burst. */
static int motion_due(uint64_t *next_us, unsigned interval, uint64_t now) {
    if (!interval || now < *next_us)
        return 0;
    *next_us = (*next_us && now - *next_us < interval) ? *next_us + interval : now + interval;
    return 1;
}

static void send_motion(int number, const OrbisPadData *pad, uint64_t now) {
    if (motion_due(&s_accel_next_us[number], atomic_load(&s_accel_interval_us[number]), now)) {
        LiSendControllerMotionEvent((uint8_t)number, LI_MOTION_TYPE_ACCEL,
                                    pad->acell.x * G_TO_MS2, pad->acell.y * G_TO_MS2,
                                    pad->acell.z * G_TO_MS2);
    }
    if (motion_due(&s_gyro_next_us[number], atomic_load(&s_gyro_interval_us[number]), now)) {
        LiSendControllerMotionEvent((uint8_t)number, LI_MOTION_TYPE_GYRO,
                                    pad->vel.x * RAD_TO_DEG, pad->vel.y * RAD_TO_DEG,
                                    pad->vel.z * RAD_TO_DEG);
    }
}

static float touch_coord(uint16_t value, int size) {
    float f = (float)value / (float)(size - 1);
    return f > 1.0f ? 1.0f : f;
}

static void send_touch(int number, uint8_t type, const pad_touch_t *t) {
    LiSendControllerTouchEvent((uint8_t)number, type, t->id, touch_coord(t->x, s_touch_w),
                               touch_coord(t->y, s_touch_h), 1.0f);
}

/* Reports only what changed since the last poll. */
static void send_touches(int number, const OrbisPadData *pad) {
    if (!s_opts.touchpad || !(LiGetHostFeatureFlags() & LI_FF_CONTROLLER_TOUCH_EVENTS))
        return;
    pad_touch_t *slots = s_touches[number];
    int fingers = pad->touch.fingers > MAX_TOUCHES ? MAX_TOUCHES : pad->touch.fingers;

    for (int s = 0; s < MAX_TOUCHES; s++) {
        if (!slots[s].active)
            continue;
        int still_down = 0;
        for (int f = 0; f < fingers; f++)
            still_down |= pad->touch.touch[f].finger == slots[s].id;
        if (!still_down) {
            send_touch(number, LI_TOUCH_EVENT_UP, &slots[s]);
            slots[s].active = 0;
        }
    }
    for (int f = 0; f < fingers; f++) {
        const OrbisPadTouch *cur = &pad->touch.touch[f];
        pad_touch_t *slot = NULL;
        pad_touch_t *free_slot = NULL;
        for (int s = 0; s < MAX_TOUCHES; s++) {
            if (slots[s].active && slots[s].id == cur->finger)
                slot = &slots[s];
            else if (!slots[s].active && !free_slot)
                free_slot = &slots[s];
        }
        if (slot) {
            if (slot->x != cur->x || slot->y != cur->y) {
                slot->x = cur->x;
                slot->y = cur->y;
                send_touch(number, LI_TOUCH_EVENT_MOVE, slot);
            }
        } else if (free_slot) {
            free_slot->active = 1;
            free_slot->id = cur->finger;
            free_slot->x = cur->x;
            free_slot->y = cur->y;
            send_touch(number, LI_TOUCH_EVENT_DOWN, free_slot);
        }
    }
}

static void set_mouse_button(int *state, int want, int button) {
    if (*state == want)
        return;
    *state = want;
    LiSendMouseButtonEvent(want ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
}

static void apply_mouse_buttons(int left, int right) {
    set_mouse_button(&s_btn_left, left, s_opts.swap_mouse_buttons ? BUTTON_RIGHT : BUTTON_LEFT);
    set_mouse_button(&s_btn_right, right, s_opts.swap_mouse_buttons ? BUTTON_LEFT : BUTTON_RIGHT);
}

/* Signed squared stick deflection past the deadzone; 0 inside it. */
static int stick_curve(uint8_t v) {
    int d = (int)v - 128;
    int mag = (d < 0 ? -d : d) - STICK_DEADZONE;
    if (mag <= 0)
        return 0;
    return d < 0 ? -(mag * mag) : mag * mag;
}

static void stick_mouse_move(uint8_t x, uint8_t y) {
    s_accum_x += stick_curve(x);
    s_accum_y += stick_curve(y);
    int dx = s_accum_x / MOUSE_MOVE_DIV;
    int dy = s_accum_y / MOUSE_MOVE_DIV;
    s_accum_x %= MOUSE_MOVE_DIV;
    s_accum_y %= MOUSE_MOVE_DIV;
    if (dx || dy)
        LiSendMouseMoveEvent((short)dx, (short)dy);
}

void input_release_all(void) {
    int mask = current_mask();
    for (int i = 0; i < PAD_COUNT; i++) {
        if (i == 0 || (s_extra[i - 1].handle > 0 && s_extra[i - 1].connected))
            LiSendMultiControllerEvent((short)i, (short)mask, 0, 0, 0, 0, 0, 0, 0);
        for (int t = 0; t < MAX_TOUCHES; t++) {
            if (s_touches[i][t].active) {
                send_touch(i, LI_TOUCH_EVENT_UP, &s_touches[i][t]);
                s_touches[i][t].active = 0;
            }
        }
    }
    apply_mouse_buttons(0, 0);
    s_combo_start_us = 0;
    input_menu_absorb();
}

bool input_poll(void) {
    if (s_pad < 0)
        return input_should_quit();

    OrbisPadData pad;
    memset(&pad, 0, sizeof(pad));
    if (scePadReadState(s_pad, &pad) < 0)
        return input_should_quit();

    uint64_t now = now_us();
    if (s_last_rescan_us == 0 || now - s_last_rescan_us > PAD_RESCAN_US) {
        s_last_rescan_us = now;
        rescan_extra_pads();
    }

    /* Read extra pads first: the mask sent with every event must cover them. */
    OrbisPadData extra[EXTRA_PADS];
    for (int i = 0; i < EXTRA_PADS; i++) {
        extra_pad_t *e = &s_extra[i];
        memset(&extra[i], 0, sizeof(extra[i]));
        if (e->handle <= 0)
            continue;
        e->connected = scePadReadState(e->handle, &extra[i]) >= 0 && extra[i].connected;
        if (!e->connected)
            e->announced = 0; /* announce again when it comes back */
    }
    int mask = current_mask();

    if (!s_announced) {
        announce_controller(0, mask);
        s_announced = 1;
    }
    for (int i = 0; i < EXTRA_PADS; i++) {
        extra_pad_t *e = &s_extra[i];
        if (e->handle <= 0 || !e->connected)
            continue;
        if (!e->announced) {
            announce_controller(i + 1, mask);
            e->announced = 1;
        }
        send_controller(i + 1, mask, &extra[i], extra[i].buttons);
        send_motion(i + 1, &extra[i], now);
        send_touches(i + 1, &extra[i]);
    }

    const uint32_t options = ORBIS_PAD_BUTTON_OPTIONS;
    const uint32_t touch_btn = ORBIS_PAD_BUTTON_TOUCH_PAD;

    uint32_t raw = pad.buttons;
    /* Not while OPTIONS is held: that press belongs to the quit combo. */
    if (pad.buttons & options)
        raw &= ~touch_btn;

    /* A finger resting on the touchpad, no press needed, like Steam + stick. */
    int touch_hold = s_opts.touch_hold_mouse && pad.touch.fingers >= 1;

    if (touch_hold) {
        /* The right stick and triggers drive the mouse, so the host must not
         * see them too; every other control keeps working. */
        OrbisPadData held = pad;
        held.rightStick.x = 128;
        held.rightStick.y = 128;
        held.analogButtons.l2 = 0;
        held.analogButtons.r2 = 0;
        send_controller(0, mask, &held, raw);
        stick_mouse_move(pad.rightStick.x, pad.rightStick.y);
    } else {
        send_controller(0, mask, &pad, raw);
    }

    send_motion(0, &pad, now);
    send_touches(0, &pad);
    apply_rumble(now);

    apply_mouse_buttons(touch_hold && pad.analogButtons.r2 > TRIGGER_CLICK_LEVEL,
                        touch_hold && pad.analogButtons.l2 > TRIGGER_CLICK_LEVEL);

    // Menu combo: OPTIONS + TOUCHPAD for ~1 s; once per hold.
    const uint32_t combo = options | touch_btn;
    if ((pad.buttons & combo) == combo) {
        if (!s_combo_start_us) {
            s_combo_start_us = now;
        } else if (!s_combo_fired && now - s_combo_start_us > 1000000ull) {
            LOGI("input: menu combo detected");
            s_combo_fired = 1;
            s_menu_requested = 1;
        }
    } else {
        s_combo_start_us = 0;
        s_combo_fired = 0;
    }

    /* Never leave a button stuck down on the host. */
    if (input_should_quit())
        apply_mouse_buttons(0, 0);

    return input_should_quit();
}
