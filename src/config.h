// Persistent client configuration (simple INI).
#pragma once

#include <stdbool.h>
#include <Limelight.h>

#define CONFIG_DIR_PS4 "/data/moonlight"
#define CONFIG_MAX_HOST 128
#define CONFIG_MAX_APP  128

typedef struct {
    STREAM_CONFIGURATION stream; // embedded: width/height/fps/bitrate/...

    char host[CONFIG_MAX_HOST];
    char app_name[CONFIG_MAX_APP]; // name or numeric id
    char debug_host[64];
    char host_mac[18]; // for Wake PC; remembered from the last connection

    bool sops;
    bool local_audio;
    bool prefer_hw;
    bool videodec2_spike;
    bool enable_file_log; // writes /data/moonlight/debug.log
    bool show_stats; // on-screen perf overlay (FPS/decode/convert/present/KB per frame)
    bool paired_ok; // runtime only

    /* Settings mirroring the official Moonlight client. */
    bool auto_bitrate;        // recompute bitrate when resolution/FPS change
    bool unlock_bitrate;      // raise the bitrate ceiling from 150 to 500 Mbps
    bool vsync;               // false = flip without waiting for vblank (tearing)
    bool frame_pacing;        // present on a regular schedule at stream.fps
    bool force_hw;            // no silent fallback to software decode
    bool quit_app_after;      // quit the host app when leaving the stream
    bool keep_awake;          // stop the console entering rest mode mid-stream
    bool multi_controller;    // one host gamepad per logged-in PS4 user
    bool touch_hold_mouse;    // finger on touchpad: R stick moves, R2/L2 click
    bool swap_face_buttons;   // swap A/B and X/Y
    bool swap_mouse_buttons;  // swap left/right mouse buttons
    bool pad_rumble;          // the host may vibrate the pad
    bool pad_motion;          // send gyroscope and accelerometer on request
    bool pad_touchpad;        // send touchpad touches to the host
    bool pad_lightbar;        // the host may set the light bar color
    int flip_queue_max;       // frames allowed to wait for a flip (1 = lowest latency)
    bool enable_mdns;         // find PCs on the local network
    bool connection_warnings; // on-screen notice when the connection degrades
    bool config_warnings;     // notice before streaming about risky settings

    /* Videodec2 tuning, A/B-able on console without a rebuild. Rev-2 defaults
     * (depth=2 + AU ONION) are the fast path; files with cfg_rev < 2 get them
     * re-applied on load. See docs/CONSOLE_VALIDATE.md. */
    int dec_pipeline_depth; // frames in flight inside Videodec2 (1..VIDEODEC2_MAX_FB)
    int dec_thread_prio;    // Orbis priority of the Vdec CPU worker (lower = higher)
    int slices_per_frame;   // CAPABILITY_SLICES_PER_FRAME asked of the host
    bool dec_au_onion;      // AU bitstream in cacheable ONION (false = WC_GARLIC)
    bool dec_fb_garlic;     // force decoder framebuffer to WC_GARLIC (skip ONION alias)
    int bgra_workers;       // NV12->BGRA convert threads (1..6)
    int bgra_nt;            // -1 auto, 0 cached stores, 1 streaming stores
} app_config_t;

/* Default bitrate (kbps) for a mode: interpolated by pixel count, scaled by FPS. */
int config_default_bitrate(int width, int height, int fps);
void config_set_defaults(app_config_t *cfg);
void config_ensure_dir(const char *dir);
int config_load(app_config_t *cfg, const char *dir);
int config_save(const app_config_t *cfg, const char *dir);
