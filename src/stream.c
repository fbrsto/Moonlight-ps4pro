// Orchestration: identity -> connect -> menu UI -> launch -> stream -> menu.

#include "stream.h"
#include "log.h"
#include "config.h"
#include "gamestream/client.h"
#include "gamestream/gs_http.h"
#include "gamestream/gs_errors.h"
#include "audio/audio_orbis.h"
#include "video/video.h"
#include "input/input_pad.h"
#include "ui/ui_menu.h"
#include "ui/stream_menu.h"
#ifdef __ORBIS__
#include "orbis/mem_stats.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

#ifdef __ORBIS__
#include <orbis/SystemService.h>
#endif

/* The menu used to stop here; above it the decoder budget is untested. */
#define VALIDATED_BITRATE_KBPS 50000
#define CONFIG_WARNING_MS 3000
/* Pad read interval while streaming: part of the input delay. */
#define POLL_INTERVAL_US 4000
#define POLL_TICKS_PER_SECOND 250
/* Kept on the console, so a stream that ended can be explained afterwards. */
#define STREAM_END_LOG_MAX_BYTES (64 * 1024)
#define LOG_LINE_MAX 160
#define MEMORY_LOG_EVERY_SECONDS 10
#define MENU_RELEASE_WAIT_MS 500
/* stream_play: the connection dropped while the game was left running. */
#define STREAM_LOST 1
/* stream_play: start again at once, to apply a setting. */
#define STREAM_RESTART 2
#define RECONNECT_ATTEMPTS 5
#define RECONNECT_WAIT_MS 3000
#define RECONNECT_POLL_MS 50
/* A stream that held this long was fine: the next drop starts the attempts over. */
#define RECONNECT_STABLE_SECONDS 60

static gs_server_t s_server;
static client_identity_t s_id;
static volatile int s_connected;
static char s_stream_title[CONFIG_MAX_APP];
static int s_warn_connection;
static const char *s_config_dir = "";
static int s_last_app_id;
static int s_last_stream_ticks;
static stream_menu_t s_menu;

/* As the Settings slider in ui_menu.c. */
#define MENU_BITRATE_MAX_KBPS 150000
#define MENU_BITRATE_MAX_UNLOCKED_KBPS 500000

static pthread_mutex_t s_end_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_last_lib_line[LOG_LINE_MAX]; /* the library logs an error just before it gives up */
static char s_end_lib_line[LOG_LINE_MAX];
static int s_end_error;

static void cl_stage_starting(int stage) {
    const char *name = LiGetStageName(stage);
    LOGI("connection: stage START %s (%d)", name, stage);
}
static void cl_stage_failed(int stage, int errorCode) {
    const char *name = LiGetStageName(stage);
    LOGE("connection: stage FAIL %s (%d) err=%d", name, stage, errorCode);
}
static void cl_connection_started(void) {
    s_connected = 1;
    LOGI("connection: streaming active");
    if (s_stream_title[0])
        LOGN("%s", s_stream_title);
}
static void cl_connection_terminated(int errorCode) {
    LOGE("connection: terminated err=%d", errorCode);
    pthread_mutex_lock(&s_end_lock);
    s_end_error = errorCode;
    memcpy(s_end_lib_line, s_last_lib_line, sizeof(s_end_lib_line));
    pthread_mutex_unlock(&s_end_lock);
    s_connected = 0;
    input_request_quit();
}
static void cl_log(const char *format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    // Limelight sometimes omits \n
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n')
        log_printf("[Li] %s", buf);
    else
        log_printf("[Li] %s\n", buf);
    pthread_mutex_lock(&s_end_lock);
    snprintf(s_last_lib_line, sizeof(s_last_lib_line), "%.*s", (int)strcspn(buf, "\r\n"), buf);
    pthread_mutex_unlock(&s_end_lock);
}

static void format_memory(char *out, size_t cap) {
#ifdef __ORBIS__
    mem_stats_t m;
    mem_stats_get(&m);
    snprintf(out, cap,
             "heap=%lldKB pool=%lldKB blocks=%lld [%lld %lld %lld %lld %lld %lld] flex_free=%lldKB fail=%llu last_fail=%llu",
             (long long)(m.live_bytes / 1024), (long long)(m.pool_bytes / 1024), (long long)m.live_blocks,
             (long long)m.bucket_blocks[0], (long long)m.bucket_blocks[1],
             (long long)m.bucket_blocks[2], (long long)m.bucket_blocks[3],
             (long long)m.bucket_blocks[4], (long long)m.bucket_blocks[5],
             (long long)(m.flexible_free_bytes < 0 ? -1 : m.flexible_free_bytes / 1024),
             (unsigned long long)m.failures, (unsigned long long)m.last_failed_size);
#else
    snprintf(out, cap, "unavailable");
#endif
}

static void log_stream_end(const char *how) {
    char path[512];
    snprintf(path, sizeof(path), "%s/stream_end.log", s_config_dir);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    if (ftell(f) > STREAM_END_LOG_MAX_BYTES) {
        fclose(f);
        f = fopen(path, "w");
        if (!f)
            return;
    }
    char when[32] = "";
    time_t now = time(NULL);
    struct tm *tm_now = gmtime(&now);
    if (tm_now)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S UTC", tm_now);
    char memory[200];
    format_memory(memory, sizeof(memory));
    pthread_mutex_lock(&s_end_lock);
    fprintf(f, "%s %s err=%d last=\"%s\" %s\n", when, how, s_end_error, s_end_lib_line, memory);
    pthread_mutex_unlock(&s_end_lock);
    fclose(f);
}

/* Packets per second, so losses show in the UDP log while playing. */
static void log_network_stats(RTP_VIDEO_STATS *prev) {
    const RTP_VIDEO_STATS *cur = LiGetRTPVideoStats();
    if (!cur)
        return;
    uint32_t rtt = 0, rtt_var = 0;
    (void)LiGetEstimatedRttInfo(&rtt, &rtt_var);
    LOGI("  net: video=%u fec=%u recovered=%u failed=%u oos=%u invalid=%u rtt=%ums var=%ums",
         cur->packetCountVideo - prev->packetCountVideo,
         cur->packetCountFec - prev->packetCountFec,
         cur->packetCountFecRecovered - prev->packetCountFecRecovered,
         cur->packetCountFecFailed - prev->packetCountFecFailed,
         cur->packetCountOOS - prev->packetCountOOS,
         cur->packetCountInvalid - prev->packetCountInvalid,
         rtt, rtt_var);
    *prev = *cur;
}
static void cl_status(int status) {
    LOGI("connection: statusUpdate %d", status);
    if (s_warn_connection)
        video_set_conn_warning(status == CONN_STATUS_POOR);
}

/* Show a notice for a few seconds; usleep() rejects a full second or more. */
static void show_timed_notice(const char *title, const char *line1, const char *line2) {
    ui_show_status(title, line1, line2);
    for (int ms = 0; ms < CONFIG_WARNING_MS; ms += 100)
        usleep(100000);
}

static int resolve_app_id(gs_server_t *server, const char *app_name) {
    LOGI("resolve_app_id('%s')...", app_name);

    app_entry_t *list = NULL;
    int rc = gs_applist(server, &list);
    if (rc != GS_OK) {
        LOGE("gs_applist failed: %d %s", rc, gs_error ? gs_error : "");
        return 0;
    }
    int count = 0;
    for (app_entry_t *a = list; a; a = a->next) {
        LOGI("  app[%d]=%d '%s'", count, a->id, a->name);
        count++;
    }
    int app_id = xml_applist_resolve(list, app_name);
    xml_applist_free(list);
    LOGI("resolve_app_id => %d (%d apps)", app_id, count);
    return app_id;
}

/* serverinfo + pairing with the current host from cfg. */
static int stream_connect(app_config_t *cfg, const char *config_dir) {
    char pin[8] = {0};
    char line[160];

    snprintf(line, sizeof(line), "Host: %s", cfg->host);
    ui_show_status("MOONLIGHT PS4", "Connecting...", line);

    LOGI("gs_init(%s:47989)...", cfg->host);
    if (gs_init(&s_server, &s_id, cfg->host, 47989, config_dir) != GS_OK) {
        LOGE("gs_init FAIL: %s", gs_error ? gs_error : "?");
        ui_show_status("ERROR", "Could not connect",
                       gs_error ? gs_error : "gs_init FAIL");
        return -1;
    }
    LOGI("server %s ver=%s paired=%d https=%u codecs=0x%x currentGame=%d",
         cfg->host, s_server.appVersion, s_server.paired, s_server.httpsPort,
         s_server.serverCodecModeSupport, s_server.currentGame);

    if (s_server.mac[0] && strcasecmp(s_server.mac, cfg->host_mac) != 0) {
        snprintf(cfg->host_mac, sizeof(cfg->host_mac), "%s", s_server.mac);
        LOGI("host MAC remembered: %s", cfg->host_mac);
        (void)config_save(cfg, config_dir);
    }

    if (!s_server.paired) {
        char path[512];
        config_ensure_dir(config_dir);
        snprintf(path, sizeof(path), "%s/pin.txt", config_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(pin, sizeof(pin), f))
                pin[strcspn(pin, " \r\n")] = '\0';
            fclose(f);
            LOGI("pin.txt read: '%s'", pin);
        } else {
            LOGI("pin.txt does not exist yet");
        }
        if (strlen(pin) != 4) {
            unsigned char rnd[2];
            rng_fill(&s_id, rnd, sizeof(rnd));
            snprintf(pin, sizeof(pin), "%04u", (rnd[0] << 8 | rnd[1]) % 10000u);
            f = fopen(path, "w");
            if (f) {
                fputs(pin, f);
                fclose(f);
                LOGI("PIN written to %s = %s", path, pin);
            } else {
                LOGE("fwrite pin FAIL path=%s errno=%d", path, errno);
            }
        }

        char pin_line[128];
        snprintf(pin_line, sizeof(pin_line),
                 "Please enter %s on your host PC. This dialog will close when pairing is completed.",
                 pin);
        ui_show_status("Pairing", pin_line,
                       "If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.");
        LOGI(">>> PAIRING PIN=%s  -> https://%s:47990/pin  (timeout 120s)",
             pin, cfg->host);

        http_set_timeout_ms(120000);
        int pair_rc = gs_pair(&s_server, pin);
        http_set_timeout_ms(0);
        LOGI("gs_pair => %d (%s)", pair_rc, gs_error ? gs_error : "ok");
        if (pair_rc != GS_OK) {
            ui_show_status("ERROR", "Pairing failed",
                           gs_error ? gs_error : "gs_pair FAIL");
            return -1;
        }
        if (remove(path) != 0)
            LOGW("could not delete %s errno=%d", path, errno);
        else
            LOGI("pin.txt deleted after pairing OK");
        ui_show_status("MOONLIGHT PS4", "Paired OK", "Opening menu...");
    } else {
        LOGI("already paired; skip pair");
    }
    return 0;
}

/* One stream session: launch + LiStartConnection + loop until exit.
 * When reconnecting, a failure must leave the running game alone. */
static int stream_play(app_config_t *cfg, int reconnecting) {
    s_connected = 0;
    s_last_stream_ticks = 0;
    snprintf(s_stream_title, sizeof(s_stream_title), "%s", cfg->app_name);

    int app_id = resolve_app_id(&s_server, cfg->app_name);
    if (!app_id) {
        LOGE("app not found: '%s'", cfg->app_name);
        return -1;
    }
    LOGI("launch app_id=%d name='%s' %dx%d@%d bitrate=%d sops=%d localAudio=%d currentGame=%d",
         app_id, cfg->app_name, cfg->stream.width, cfg->stream.height,
         cfg->stream.fps, cfg->stream.bitrate, cfg->sops, cfg->local_audio,
         s_server.currentGame);
    s_last_app_id = app_id;

    input_options_t input_opts = {
        .multi_controller = cfg->multi_controller,
        .touch_hold_mouse = cfg->touch_hold_mouse,
        .swap_face_buttons = cfg->swap_face_buttons,
        .swap_mouse_buttons = cfg->swap_mouse_buttons,
        .rumble = cfg->pad_rumble,
        .motion = cfg->pad_motion,
        .touchpad = cfg->pad_touchpad,
        .lightbar = cfg->pad_lightbar,
    };
    input_set_options(&input_opts);
    int pad_mask = input_active_mask();
    LOGI("stream: gamepad mask=0x%x audio=%d channels", pad_mask,
         CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(cfg->stream.audioConfiguration));

    /* If Sunshine already has another app active, cancel before launching. */
    if (s_server.currentGame != 0 && s_server.currentGame != app_id) {
        LOGI("stream: currentGame=%d != %d → cancel previous",
             s_server.currentGame, app_id);
        (void)gs_quit_app(&s_server);
    }

    if (gs_start_app(&s_server, &cfg->stream, app_id, cfg->sops, cfg->local_audio, pad_mask) != GS_OK) {
        LOGE("gs_start_app FAIL: %s", gs_error ? gs_error : "?");
        /* Retry: force clean launch after cancel. */
        if (!reconnecting && s_server.currentGame != 0) {
            LOGI("stream: retry after cancel");
            (void)gs_quit_app(&s_server);
            if (gs_start_app(&s_server, &cfg->stream, app_id, cfg->sops, cfg->local_audio, pad_mask) == GS_OK) {
                LOGI("gs_start_app OK (retry)");
                goto start_ok;
            }
            LOGE("gs_start_app retry FAIL: %s", gs_error ? gs_error : "?");
        }
        return -1;
    }
start_ok:
    LOGI("gs_start_app OK");

    input_reset();

    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_starting;
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_connection_started;
    cl.connectionTerminated = cl_connection_terminated;
    cl.logMessage = cl_log;
    cl.connectionStatusUpdate = cl_status;
    cl.rumble = input_set_rumble;
    cl.setMotionEventState = input_set_motion_rate;
    cl.setControllerLED = input_set_led;

    DECODER_RENDERER_CALLBACKS dr = video_callbacks_ffmpeg;
    int prefer_ycbcr = 0; /* the menu has already opened the screen in BGRA */
    const char *decoder_reason = "default";
    int sw_fallback = 0;
#if ML_ENABLE_VIDEODEC2
    if (!cfg->prefer_hw) {
        decoder_reason = "prefer_hw=false";
    } else {
        int probe = video_orbis_probe(cfg->stream.width, cfg->stream.height);
        if (probe == 0) {
            video_orbis_set_tuning(cfg->dec_pipeline_depth, cfg->dec_thread_prio,
                                   cfg->dec_au_onion, cfg->dec_fb_garlic);
            dr = video_callbacks_orbis;
            if (cfg->slices_per_frame > 0)
                dr.capabilities = CAPABILITY_SLICES_PER_FRAME(cfg->slices_per_frame);
            decoder_reason = "probe OK";
        } else if (cfg->force_hw) {
            LOGE("stream: HW probe failed rc=%d and decoder is forced to hardware", probe);
            gs_quit_app(&s_server);
            show_timed_notice("ERROR", "Hardware decoder unavailable",
                              "Set Video decoder to Auto or Software in SETTINGS");
            return -1;
        } else {
            LOGW("stream: HW probe failed rc=%d; fallback SW", probe);
            decoder_reason = "probe FAIL";
            sw_fallback = 1;
        }
    }
#else
    decoder_reason = "ML_ENABLE_VIDEODEC2=0";
#endif
    LOGI("stream: decoder=%s (prefer_hw=%s, %s)",
         dr.submitDecodeUnit == video_callbacks_ffmpeg.submitDecodeUnit ? "SW" : "HW",
         cfg->prefer_hw ? "true" : "false", decoder_reason);
    AUDIO_RENDERER_CALLBACKS ar = audio_callbacks_orbis;

    video_set_show_stats(cfg->show_stats);
    video_set_vsync(cfg->vsync);
    video_set_frame_pacing(cfg->frame_pacing ? cfg->stream.fps : 0);
    video_set_flip_queue_max(cfg->flip_queue_max);
    video_set_conn_warning(0);
    s_warn_connection = cfg->connection_warnings;

    if (cfg->config_warnings) {
        const char *warning = NULL;
        if (sw_fallback)
            warning = "Hardware decoder unavailable: using slower software decode";
        else if (cfg->stream.width * cfg->stream.height > 1920 * 1080)
            warning = "Above 1080p is experimental, and the PS4 still displays 1080p";
        else if (cfg->stream.bitrate > VALIDATED_BITRATE_KBPS)
            warning = "Bitrate above 50 Mbps: the PS4 may not keep up";
        if (warning) {
            LOGW("stream: config warning: %s", warning);
            show_timed_notice("WARNING", warning, "Turn this notice off in SETTINGS");
            video_ui_end();
        }
    }

    LOGI("LiStartConnection begin...");
    int ret = LiStartConnection(&s_server.serverInfo, &cfg->stream, &cl, &dr, &ar,
                                &prefer_ycbcr, sizeof(prefer_ycbcr), NULL, 0);
    LOGI("LiStartConnection => %d", ret);
    if (ret != 0) {
        if (!reconnecting)
            gs_quit_app(&s_server);
        return -1;
    }

    LOGI("main loop; OPTIONS+TOUCHPAD 1s to quit");

    pthread_mutex_lock(&s_end_lock);
    s_end_error = 0;
    s_end_lib_line[0] = '\0';
    pthread_mutex_unlock(&s_end_lock);

    RTP_VIDEO_STATS net_prev;
    memset(&net_prev, 0, sizeof(net_prev));
    int ticks = 0;
    int menu_open = 0, restart = 0, quit_game = 0;
    for (;;) {
        if (menu_open) {
            if (input_should_quit())
                break;
            unsigned pressed = 0, held = 0;
            (void)input_menu_poll(&pressed, &held);
            stream_menu_action_t action = stream_menu_input(&s_menu, pressed);
            if (s_menu.stats != cfg->show_stats) {
                cfg->show_stats = s_menu.stats;
                video_set_show_stats(cfg->show_stats);
                (void)config_save(cfg, s_config_dir);
            }
            if (s_menu.pacing != cfg->frame_pacing) {
                cfg->frame_pacing = s_menu.pacing;
                video_set_frame_pacing(cfg->frame_pacing ? cfg->stream.fps : 0);
                (void)config_save(cfg, s_config_dir);
            }
            if (action != STREAM_MENU_STAY) {
                menu_open = 0;
                video_set_stream_menu(NULL);
                if (action == STREAM_MENU_DISCONNECT)
                    break;
                if (action == STREAM_MENU_QUIT_GAME) {
                    quit_game = 1;
                    break;
                }
                if (s_menu.bitrate_kbps != s_menu.bitrate_opened_kbps) {
                    cfg->stream.bitrate = s_menu.bitrate_kbps;
                    cfg->auto_bitrate = false;
                    (void)config_save(cfg, s_config_dir);
                    restart = 1;
                    break;
                }
                /* The press that closed the menu must not reach the game. */
                input_menu_wait_release(MENU_BTN_CROSS | MENU_BTN_CIRCLE | MENU_BTN_OPTIONS, MENU_RELEASE_WAIT_MS);
            }
        } else {
            if (input_poll())
                break;
            if (input_take_menu_request()) {
                input_release_all();
                stream_menu_open(&s_menu, cfg->show_stats, cfg->frame_pacing, cfg->stream.bitrate,
                                 cfg->unlock_bitrate ? MENU_BITRATE_MAX_UNLOCKED_KBPS : MENU_BITRATE_MAX_KBPS);
                video_set_stream_menu(&s_menu);
                menu_open = 1;
            }
        }
        usleep(POLL_INTERVAL_US);
        if (++ticks % POLL_TICKS_PER_SECOND == 0) {
#ifdef __ORBIS__
            /* A mouse-only or watch-only session gives the OS no reason to stay up. */
            if (cfg->keep_awake)
                sceSystemServicePowerTick();
#endif
            video_stats_t st;
            video_get_stats(&st);
            LOGI("stats t=%d connected=%d frames=%u decodes=%u drop=%u",
                 ticks, s_connected, st.frames, st.decodes, st.dropped);
            unsigned queue_avg, queue_max;
            video_take_flip_queue_stats(&queue_avg, &queue_max);
            LOGI("  flip queue avg=%u.%02u max=%u", queue_avg / 100, queue_avg % 100, queue_max);
            log_network_stats(&net_prev);
            unsigned flip_waits, flip_wait_us;
            video_take_flip_wait_stats(&flip_waits, &flip_wait_us);
            LOGI("  flip waits=%u total=%ums", flip_waits, flip_wait_us / 1000);
            if (ticks % (POLL_TICKS_PER_SECOND * MEMORY_LOG_EVERY_SECONDS) == 0) {
                char memory[200];
                format_memory(memory, sizeof(memory));
                LOGI("  mem: %s", memory);
                unsigned rumble_requests, rumble_sends;
                input_take_rumble_stats(&rumble_requests, &rumble_sends);
                if (rumble_requests)
                    LOGI("  rumble: requests=%u sent=%u in %ds", rumble_requests, rumble_sends,
                         MEMORY_LOG_EVERY_SECONDS);
            }
            if (st.frames || st.decodes) {
                double nd = st.decodes ? (double)st.decodes : 1.0;
                double nf = st.frames ? (double)st.frames : 1.0;
                LOGI("  decode=%.1fms convert=%.1fms (bounce=%.1f bgra=%.1f) present=%.1fms",
                     (st.decode_us_total / nd) / 1000.0,
                     st.frames ? (st.convert_us_total / nf) / 1000.0 : 0.0,
                     st.frames ? (st.bounce_us_total / nf) / 1000.0 : 0.0,
                     st.frames ? (st.bgra_us_total / nf) / 1000.0 : 0.0,
                     st.frames ? (st.present_us_total / nf) / 1000.0 : 0.0);
                LOGI("  au=%.2fms %.0fKB/frame", (st.au_us_total / nd) / 1000.0,
                     (st.au_bytes_total / nd) / 1024.0);
                video_reset_stats();
            }
        }
        if (!s_connected && ticks > POLL_TICKS_PER_SECOND * 2) {
            LOGE("stream: connection lost after %d ticks", ticks);
            break;
        }
    }

    LOGI("leaving loop (quit or disconnect)");
    /* A dropped connection must not close the game: only a deliberate exit does. */
    int user_quit = s_connected;
    video_set_stream_menu(NULL);
    log_stream_end(restart ? "restart" : user_quit ? "quit" : "lost");
    int end_error = s_end_error;
    s_last_stream_ticks = ticks;
    LiStopConnection();
    input_stop_feedback();
    if (restart && user_quit) {
        LOGI("stream_play: restarting at %d kbps", cfg->stream.bitrate);
        return STREAM_RESTART;
    }
    if ((cfg->quit_app_after || quit_game) && user_quit) {
        if (gs_quit_app(&s_server) != GS_OK)
            LOGW("stream: quit after stream failed: %s", gs_error ? gs_error : "?");
        LOGN("Stream closed");
        LOGI("stream_play done OK (app closed) → return to menu");
        return 0;
    }
    /* No gs_quit_app: leave the game on Sunshine for resume → "paused". */
    if (user_quit)
        LOGN("Stream paused");
    else
        LOGN("Connection lost (error %d)", end_error);
    LOGI("stream_play done OK → return to menu");
    /* The host ends a stream cleanly when its game is closed: nothing to resume. */
    return (user_quit || end_error == ML_ERROR_GRACEFUL_TERMINATION) ? 0 : STREAM_LOST;
}

/* Waits between attempts; returns 0 if the user asked to go back to the menu. */
static int reconnect_wait(int attempt) {
    char line[64];
    snprintf(line, sizeof(line), "Reconnecting: attempt %d of %d", attempt, RECONNECT_ATTEMPTS);
    ui_show_status("CONNECTION LOST", line, "Press O to go back to the menu");
    input_reset();
    for (int ms = 0; ms < RECONNECT_WAIT_MS; ms += RECONNECT_POLL_MS) {
        unsigned pressed = 0, held = 0;
        if (input_menu_poll(&pressed, &held) == 0 && (pressed & MENU_BTN_CIRCLE))
            return 0;
        usleep(RECONNECT_POLL_MS * 1000);
    }
    return 1;
}

/* Returns 0 to go back to the menu, -1 if the host could not be reached again. */
static int stream_reconnect(app_config_t *cfg) {
    int attempt = 1;
    while (attempt <= RECONNECT_ATTEMPTS) {
        if (!reconnect_wait(attempt)) {
            LOGI("reconnect: cancelled");
            return 0;
        }
        if (gs_refresh_status(&s_server) != GS_OK) {
            LOGW("reconnect: attempt %d, host unreachable: %s", attempt, gs_error ? gs_error : "?");
            attempt++;
            continue;
        }
        /* Launching would start the game again if it was closed on the PC. */
        if (s_server.currentGame != s_last_app_id) {
            LOGI("reconnect: game %d no longer running (current=%d)", s_last_app_id, s_server.currentGame);
            return 0;
        }
        LOGI("reconnect: attempt %d", attempt);
        video_ui_end();
        int rc = stream_play(cfg, 1);
        LOGI("reconnect: stream_play => %d after %d ticks", rc, s_last_stream_ticks);
        if (rc == 0 || rc == STREAM_RESTART)
            return rc;
        attempt = (rc == STREAM_LOST && s_last_stream_ticks >= POLL_TICKS_PER_SECOND * RECONNECT_STABLE_SECONDS)
                      ? 1 : attempt + 1;
    }
    LOGW("reconnect: gave up after %d attempts", RECONNECT_ATTEMPTS);
    return -1;
}

int stream_run(app_config_t *cfg, const char *config_dir) {
    s_connected = 0;
    s_config_dir = config_dir;

    LOGI("identity_init(%s)...", config_dir);
    if (identity_init(&s_id, config_dir) != GS_OK) {
        LOGE("identity_init FAIL: %s", gs_error ? gs_error : "?");
        return -1;
    }
    LOGI("identity_init OK");
    http_init(&s_id, 1);
    LOGI("http_init OK (verbose)");

    LOGI("input_init...");
    if (input_init() != 0) {
        LOGW("input_init failed; menu without pad (auto-launch)");
    } else {
        LOGI("input_init OK");
    }

    int connected = 0;
    if (cfg->host[0]) {
        connected = (stream_connect(cfg, config_dir) == 0);
    } else {
        LOGI("stream_run: empty host → settings menu");
        ui_show_status("MOONLIGHT PS4", "No PC yet", "Choose Add PC to enter its address");
    }

    /* Menu <-> stream loop until the user closes with the PS button. */
    for (;;) {
        input_reset();
        int m = ui_menu_run(cfg, connected ? &s_server : NULL, config_dir);
        LOGI("ui_menu_run => %d", m);
        if (m < 0) {
            input_shutdown();
            return -1;
        }
        if (m == UI_MENU_RECONNECT) {
            if (!cfg->host[0])
                continue;
            connected = (stream_connect(cfg, config_dir) == 0);
            continue;
        }
        if (!cfg->host[0]) {
            /* Launch without host: ask for host again. */
            continue;
        }
        if (!connected) {
            connected = (stream_connect(cfg, config_dir) == 0);
            continue;
        }

        int rc = stream_play(cfg, 0);
        LOGI("stream_play => %d", rc);
        while (rc == STREAM_LOST || rc == STREAM_RESTART)
            rc = rc == STREAM_LOST ? stream_reconnect(cfg) : stream_play(cfg, 1);
        if (rc < 0)
            connected = (stream_connect(cfg, config_dir) == 0);
    }
}
