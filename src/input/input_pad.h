#pragma once

#include <stdbool.h>

typedef struct {
    bool multi_controller;
    bool touch_hold_mouse;
    bool swap_face_buttons;
    bool swap_mouse_buttons;
    bool rumble;
    bool motion;
    bool touchpad;
    bool lightbar;
} input_options_t;

// Initialize scePad and announce the controller to Moonlight.
int input_init(void);
void input_shutdown(void);

// Stream-time input options; set before starting a stream.
void input_set_options(const input_options_t *opts);

// Refresh extra pads from the logged-in users and return the host gamepad mask.
int input_active_mask(void);

// Read the pad and send state. Returns true if the user requests quit
// (the external flag).
bool input_poll(void);

// Request quit from another thread (connectionTerminated, etc.).
void input_request_quit(void);
bool input_should_quit(void);

// True once after OPTIONS + TOUCHPAD were held ~1 s.
bool input_take_menu_request(void);
// The host sees every button and finger let go, before the menu takes the pad.
void input_release_all(void);

// Host rumble request for controller 0..3; motor levels are 0..65535. Reaches
// the pad from input_poll, which coalesces them.
void input_set_rumble(unsigned short controller, unsigned short low_freq, unsigned short high_freq);
// Requests from the host and sends to the pad since the last call.
void input_take_rumble_stats(unsigned *requests, unsigned *sends);
// Host request for gyroscope or accelerometer reports; a rate of 0 stops them.
void input_set_motion_rate(unsigned short controller, unsigned char type, unsigned short rate_hz);
// Host request for a light bar color.
void input_set_led(unsigned short controller, unsigned char r, unsigned char g, unsigned char b);
// A stream that ends must not leave a pad vibrating or a game's color on its light bar.
void input_stop_feedback(void);

// Reset quit/combo/controller announcement (before entering menu or stream).
void input_reset(void);

/* Mark current pad state as "already seen" (no edges). Call when
 * entering the menu after OPTIONS+TOUCHPAD to avoid instant re-launch. */
void input_menu_absorb(void);

/* Wait until launch buttons are released (OPTIONS/X/O). timeout_ms. */
void input_menu_wait_release(unsigned mask, int timeout_ms);

// --- Menu mode (UI): read pad without sending anything to Moonlight ---
#define MENU_BTN_UP       0x0001u
#define MENU_BTN_DOWN     0x0002u
#define MENU_BTN_LEFT     0x0004u
#define MENU_BTN_RIGHT    0x0008u
#define MENU_BTN_CROSS    0x0010u
#define MENU_BTN_CIRCLE   0x0020u
#define MENU_BTN_SQUARE   0x0040u
#define MENU_BTN_TRIANGLE 0x0080u
#define MENU_BTN_L1       0x0100u
#define MENU_BTN_R1       0x0200u
#define MENU_BTN_OPTIONS  0x0400u

// pressed: edges this frame (with D-pad auto-repeat); held: state.
// Returns 0 if pad was read, <0 if no controller.
int input_menu_poll(unsigned *pressed, unsigned *held);
