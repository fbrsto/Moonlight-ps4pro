#include "ui_draw.h"
#include "ui_font.h"

#include <string.h>

void ui_clear(ui_surface_t *s, uint32_t argb) {
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)y * (size_t)s->pitch);
        for (int x = 0; x < s->w; x++)
            row[x] = argb;
    }
}

void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)(y + r) * (size_t)s->pitch);
        for (int c = 0; c < w; c++)
            row[x + c] = argb;
    }
}

static void draw_glyph(ui_surface_t *s, int x, int y, int scale, uint32_t argb,
                       const unsigned char *glyph) {
    for (int gy = 0; gy < UI_GLYPH_H; gy++) {
        unsigned bits = glyph[gy];
        if (!bits)
            continue;
        for (int gx = 0; gx < UI_GLYPH_W; gx++) {
            if (!((bits >> gx) & 1u))
                continue;
            ui_rect(s, x + gx * scale, y + gy * scale, scale, scale, argb);
        }
    }
}

void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str) {
    if (scale < 1)
        scale = 1;
    int cx = x;
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126)
            c = '?';
        draw_glyph(s, cx, y, scale, argb, ui_font8x8[c - 32]);
        cx += UI_GLYPH_W * scale;
    }
}

int ui_text_w(int scale, const char *str) {
    if (scale < 1)
        scale = 1;
    return (int)strlen(str) * UI_GLYPH_W * scale;
}

/* --- Anti-aliased drawing --- */

/* Blend argb over dst at coverage cov (0..255). */
static inline uint32_t blend_px(uint32_t dst, uint32_t argb, unsigned cov) {
    unsigned a = ((argb >> 24) * cov + 127u) / 255u;
    unsigned ia = 255u - a;
    unsigned r = (((argb >> 16) & 0xFFu) * a + ((dst >> 16) & 0xFFu) * ia + 127u) / 255u;
    unsigned g = (((argb >> 8) & 0xFFu) * a + ((dst >> 8) & 0xFFu) * ia + 127u) / 255u;
    unsigned b = ((argb & 0xFFu) * a + (dst & 0xFFu) * ia + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint32_t *px_row(ui_surface_t *s, int y) {
    return (uint32_t *)(void *)(s->fb + (size_t)y * (size_t)s->pitch);
}

void ui_blend_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *row = px_row(s, y + r);
        for (int c = 0; c < w; c++)
            row[x + c] = blend_px(row[x + c], argb, 255u);
    }
}

/* Coverage of a pixel against a circle, from 4x4 samples. Coordinates are in
 * quarter-pixels relative to the circle's center, radius r4 likewise. */
static unsigned circle_cov(int px4, int py4, int r4) {
    int inside = 0;
    long rr = (long)r4 * r4;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            long dx = (long)(px4 + sx) * 2 + 1;
            long dy = (long)(py4 + sy) * 2 + 1;
            if (dx * dx + dy * dy <= rr * 4)
                inside++;
        }
    }
    return (unsigned)(inside * 255 / 16);
}

void ui_circle(ui_surface_t *s, int cx, int cy, int radius, uint32_t argb) {
    int r4 = radius * 4;
    for (int y = -radius; y < radius; y++) {
        int py = cy + y;
        if (py < 0 || py >= s->h)
            continue;
        uint32_t *row = px_row(s, py);
        for (int x = -radius; x < radius; x++) {
            int px = cx + x;
            if (px < 0 || px >= s->w)
                continue;
            unsigned cov = circle_cov(x * 4, y * 4, r4);
            if (cov)
                row[px] = blend_px(row[px], argb, cov);
        }
    }
}

void ui_round_rect(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t argb) {
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    ui_blend_rect(s, x, y + radius, w, h - radius * 2, argb);
    ui_blend_rect(s, x + radius, y, w - radius * 2, radius, argb);
    ui_blend_rect(s, x + radius, y + h - radius, w - radius * 2, radius, argb);

    int r4 = radius * 4;
    for (int j = 0; j < radius; j++) {
        for (int i = 0; i < radius; i++) {
            /* (i, j) in the top-left corner, mirrored to the other three. */
            unsigned cov = circle_cov((i - radius) * 4, (j - radius) * 4, r4);
            if (!cov)
                continue;
            const int xs[2] = { x + i, x + w - 1 - i };
            const int ys[2] = { y + j, y + h - 1 - j };
            for (int a = 0; a < 2; a++) {
                if (ys[a] < 0 || ys[a] >= s->h)
                    continue;
                uint32_t *row = px_row(s, ys[a]);
                for (int b = 0; b < 2; b++) {
                    if (xs[b] >= 0 && xs[b] < s->w)
                        row[xs[b]] = blend_px(row[xs[b]], argb, cov);
                }
            }
        }
    }
}

void ui_triangle_down(ui_surface_t *s, int x, int y, int w, uint32_t argb) {
    for (int r = 0; r < w / 2; r++)
        ui_blend_rect(s, x + r, y + r, w - r * 2, 1, argb);
}

static void draw_mask(ui_surface_t *s, int x, int y, int w, int h, const unsigned char *mask,
                      uint32_t argb) {
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= s->h)
            continue;
        uint32_t *row = px_row(s, py);
        const unsigned char *m = mask + (size_t)r * (size_t)w;
        for (int c = 0; c < w; c++) {
            int px = x + c;
            if (m[c] && px >= 0 && px < s->w)
                row[px] = blend_px(row[px], argb, m[c]);
        }
    }
}

void ui_icon(ui_surface_t *s, int x, int y, const ui_icon_t *icon, uint32_t argb) {
    draw_mask(s, x, y, icon->w, icon->h, icon->mask, argb);
}

void ui_image(ui_surface_t *s, int x, int y, int w, int h, const uint32_t *pixels) {
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= s->h)
            continue;
        int x0 = x < 0 ? -x : 0;
        int x1 = x + w > s->w ? s->w - x : w;
        if (x1 > x0)
            memcpy(px_row(s, py) + x + x0, pixels + (size_t)r * (size_t)w + x0,
                   (size_t)(x1 - x0) * 4u);
    }
}

/* Decode one UTF-8 code point; malformed bytes come back as '?'. */
static const char *next_cp(const char *p, unsigned *cp) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) {
        *cp = c;
        return p + 1;
    }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
        *cp = '?';
        return p + 1;
    }
    unsigned v = c & (0x3Fu >> extra);
    for (int i = 1; i <= extra; i++) {
        if (((unsigned char)p[i] & 0xC0) != 0x80) {
            *cp = '?';
            return p + 1;
        }
        v = (v << 6) | ((unsigned char)p[i] & 0x3Fu);
    }
    *cp = v;
    return p + extra + 1;
}

/* Glyph table: 32..126 at 0..94, then 160..255 at 95..190. */
static const ui_glyph_t *font_glyph(const ui_font_t *font, unsigned cp) {
    /* Typographic punctuation is common in app titles and has a plain equivalent. */
    if (cp >= 0x2010 && cp <= 0x2015)
        cp = '-';
    else if (cp >= 0x2018 && cp <= 0x201B)
        cp = '\'';
    else if (cp >= 0x201C && cp <= 0x201F)
        cp = '"';
    else if (cp == 0x2026)
        cp = '.';

    if (cp >= 32 && cp <= 126)
        return &font->glyphs[cp - 32];
    if (cp >= 160 && cp <= 255)
        return &font->glyphs[95 + cp - 160];
    return &font->glyphs['?' - 32];
}

/* Width of the n bytes at str. */
static int span_w(const ui_font_t *font, const char *str, size_t n) {
    int w = 0;
    const char *end = str + n;
    for (const char *p = str; p < end && *p;) {
        unsigned cp;
        p = next_cp(p, &cp);
        w += font_glyph(font, cp)->advance;
    }
    return w;
}

int ui_font_w(const ui_font_t *font, const char *str) {
    return span_w(font, str, strlen(str));
}

static int draw_span(ui_surface_t *s, const ui_font_t *font, int x, int y, uint32_t argb,
                     const char *str, size_t n) {
    const char *end = str + n;
    for (const char *p = str; p < end && *p;) {
        unsigned cp;
        p = next_cp(p, &cp);
        const ui_glyph_t *g = font_glyph(font, cp);
        if (g->w > 0)
            draw_mask(s, x + g->xoff, y + g->yoff, g->w, g->h, font->data + g->offset, argb);
        x += g->advance;
    }
    return x;
}

void ui_font_text(ui_surface_t *s, const ui_font_t *font, int x, int y, uint32_t argb, const char *str) {
    draw_span(s, font, x, y, argb, str, strlen(str));
}

/* Bytes of str that fit in max_w, ending on a code point boundary. */
static size_t fit_bytes(const ui_font_t *font, const char *str, int max_w) {
    int w = 0;
    const char *p = str;
    while (*p) {
        unsigned cp;
        const char *q = next_cp(p, &cp);
        w += font_glyph(font, cp)->advance;
        if (w > max_w)
            break;
        p = q;
    }
    return (size_t)(p - str);
}

void ui_font_fit(ui_surface_t *s, const ui_font_t *font, int x, int y, int max_w, uint32_t argb, const char *str) {
    if (ui_font_w(font, str) <= max_w) {
        ui_font_text(s, font, x, y, argb, str);
        return;
    }
    int dots = ui_font_w(font, "...");
    size_t n = fit_bytes(font, str, max_w - dots);
    x = draw_span(s, font, x, y, argb, str, n);
    ui_font_text(s, font, x, y, argb, "...");
}

/* Bytes of the next wrapped line; breaks at the last space that fits. */
static size_t wrap_line(const ui_font_t *font, const char *str, int w) {
    size_t n = fit_bytes(font, str, w);
    if (str[n] == '\0')
        return n;
    for (size_t i = n; i > 0; i--) {
        if (str[i] == ' ')
            return i;
    }
    return n ? n : 1; /* a word wider than the line is cut, and always progresses */
}

void ui_font_wrap(ui_surface_t *s, const ui_font_t *font, int x, int y, int w, int h, int max_lines,
                  uint32_t argb, const char *str) {
    /* Count the lines first to center the block vertically. */
    int lines = 0;
    for (const char *p = str; *p && lines < max_lines; lines++) {
        while (*p == ' ')
            p++;
        p += wrap_line(font, p, w);
    }
    int ty = h > 0 ? y + (h - lines * font->line_h) / 2 : y;

    const char *p = str;
    for (int i = 0; i < lines; i++) {
        while (*p == ' ')
            p++;
        if (i == lines - 1 && span_w(font, p, strlen(p)) > w) {
            int lw = w; /* last line, more text than room: ellipsis */
            ui_font_fit(s, font, x, ty + i * font->line_h, lw, argb, p);
            return;
        }
        size_t n = wrap_line(font, p, w);
        draw_span(s, font, x + (w - span_w(font, p, n)) / 2, ty + i * font->line_h, argb, p, n);
        p += n;
    }
}

int ui_font_para(ui_surface_t *s, const ui_font_t *font, int x, int y, int w, uint32_t argb, const char *str) {
    int lines = 0;
    for (const char *p = str; *p; lines++) {
        while (*p == ' ')
            p++;
        size_t n = wrap_line(font, p, w);
        draw_span(s, font, x, y + lines * font->line_h, argb, p, n);
        p += n;
    }
    return lines * font->line_h;
}
