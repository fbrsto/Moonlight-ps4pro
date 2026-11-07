#include "ui_boxart.h"
#include "ui_assets.h"
#include "../log.h"
#include "../gamestream/gs_errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define MAX_ART 64
#define ART_TIMEOUT_MS 3000

typedef struct {
    int id;      /* 0 = free slot */
    uint32_t *pixels; /* NULL: use the no-art image */
} art_t;

static art_t s_art[MAX_ART];
static char s_host[128];
static uint32_t *s_no_art;
static int s_no_art_tried;

/* Bilinear scale of RGBA8 to UI_BOXART_W x UI_BOXART_H, 0xFFRRGGBB. */
static uint32_t *scale_art(const unsigned char *src, int w, int h) {
    uint32_t *out = (uint32_t *)malloc((size_t)UI_BOXART_W * UI_BOXART_H * 4u);
    if (!out)
        return NULL;
    for (int y = 0; y < UI_BOXART_H; y++) {
        /* 8 fractional bits. */
        int sy = (int)(((int64_t)y * (h - 1) * 256) / (UI_BOXART_H > 1 ? UI_BOXART_H - 1 : 1));
        int y0 = sy >> 8, fy = sy & 255;
        int y1 = y0 + 1 < h ? y0 + 1 : y0;
        for (int x = 0; x < UI_BOXART_W; x++) {
            int sx = (int)(((int64_t)x * (w - 1) * 256) / (UI_BOXART_W > 1 ? UI_BOXART_W - 1 : 1));
            int x0 = sx >> 8, fx = sx & 255;
            int x1 = x0 + 1 < w ? x0 + 1 : x0;
            const unsigned char *a = src + ((size_t)y0 * w + x0) * 4;
            const unsigned char *b = src + ((size_t)y0 * w + x1) * 4;
            const unsigned char *c = src + ((size_t)y1 * w + x0) * 4;
            const unsigned char *d = src + ((size_t)y1 * w + x1) * 4;
            uint32_t px = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                int top = a[ch] * (256 - fx) + b[ch] * fx;
                int bot = c[ch] * (256 - fx) + d[ch] * fx;
                unsigned v = (unsigned)((top * (256 - fy) + bot * fy) >> 16);
                px |= v << (16 - ch * 8);
            }
            out[(size_t)y * UI_BOXART_W + x] = px;
        }
    }
    return out;
}

static const uint32_t *no_art_image(void) {
    if (!s_no_art_tried) {
        s_no_art_tried = 1;
        int w, h, comp;
        unsigned char *img = stbi_load_from_memory(ui_no_app_png, UI_NO_APP_PNG_SIZE, &w, &h, &comp, 4);
        if (img) {
            s_no_art = scale_art(img, w, h);
            stbi_image_free(img);
        } else {
            LOGE("boxart: built-in image: %s", stbi_failure_reason());
        }
    }
    return s_no_art;
}

void ui_boxart_set_host(const char *host) {
    if (!strcmp(s_host, host))
        return;
    snprintf(s_host, sizeof(s_host), "%s", host);
    for (int i = 0; i < MAX_ART; i++) {
        free(s_art[i].pixels);
        s_art[i].pixels = NULL;
        s_art[i].id = 0;
    }
}

static art_t *find_art(int app_id) {
    for (int i = 0; i < MAX_ART; i++)
        if (s_art[i].id == app_id)
            return &s_art[i];
    return NULL;
}

const uint32_t *ui_boxart_get(int app_id, int *is_placeholder) {
    art_t *a = find_art(app_id);
    if (a && a->pixels) {
        *is_placeholder = 0;
        return a->pixels;
    }
    *is_placeholder = 1;
    return no_art_image();
}

/* Hosts send these sizes for apps with no real art (same list as the official
 * client), so the name must be shown over them. */
static int is_placeholder_size(int w, int h) {
    return (w == 130 && h == 180) || (w == 628 && h == 888) || (w == 200 && h == 266);
}

int ui_boxart_load_next(gs_server_t *server, const int *app_ids, int count) {
    for (int i = 0; i < count; i++) {
        if (find_art(app_ids[i]))
            continue;
        art_t *slot = find_art(0);
        if (!slot)
            return 0;
        slot->id = app_ids[i]; /* one attempt per app, even when it fails */

        http_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        http_set_timeout_ms(ART_TIMEOUT_MS);
        int rc = gs_app_asset(server, app_ids[i], &resp);
        http_set_timeout_ms(0);
        if (rc != GS_OK || !resp.body || resp.len == 0) {
            LOGW("boxart: app %d: fetch failed (%d)", app_ids[i], rc);
            http_resp_free(&resp);
            return 1;
        }

        int w, h, comp;
        unsigned char *img = stbi_load_from_memory((const unsigned char *)resp.body, (int)resp.len,
                                                   &w, &h, &comp, 4);
        http_resp_free(&resp);
        if (!img) {
            LOGW("boxart: app %d: %s", app_ids[i], stbi_failure_reason());
            return 1;
        }
        if (!is_placeholder_size(w, h))
            slot->pixels = scale_art(img, w, h);
        stbi_image_free(img);
        return 1;
    }
    return 0;
}
