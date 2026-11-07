// Software drawing onto a BGRA framebuffer (uint32 0xAARRGGBB little-endian).
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t *fb;
    int pitch; /* bytes per row */
    int w, h;  /* pixels */
} ui_surface_t;

void ui_clear(ui_surface_t *s, uint32_t argb);
void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb);
/* ASCII text 32..126 with scaled 8x8 font. */
void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str);
int ui_text_w(int scale, const char *str);

/* --- Anti-aliased drawing for the menu. Colors may carry alpha. --- */

typedef struct {
    short w, h;       /* bitmap size */
    short xoff, yoff; /* bitmap position relative to the pen and the line top */
    short advance;
    unsigned offset;  /* into the font's data */
} ui_glyph_t;

typedef struct {
    int line_h;
    const ui_glyph_t *glyphs; /* see ui_font_glyph() for the index */
    const unsigned char *data;
} ui_font_t;

typedef struct {
    int w, h;
    const unsigned char *mask; /* 8-bit coverage, w*h */
} ui_icon_t;

void ui_blend_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb);
void ui_round_rect(ui_surface_t *s, int x, int y, int w, int h, int radius, uint32_t argb);
void ui_circle(ui_surface_t *s, int cx, int cy, int radius, uint32_t argb);
void ui_triangle_down(ui_surface_t *s, int x, int y, int w, uint32_t argb);
void ui_icon(ui_surface_t *s, int x, int y, const ui_icon_t *icon, uint32_t argb);
/* Opaque image: w*h pixels of 0xFFRRGGBB, tightly packed. */
void ui_image(ui_surface_t *s, int x, int y, int w, int h, const uint32_t *pixels);

/* Text is UTF-8; code points outside Latin-1 draw as '?'. y is the line top. */
int ui_font_w(const ui_font_t *font, const char *str);
void ui_font_text(ui_surface_t *s, const ui_font_t *font, int x, int y, uint32_t argb, const char *str);
/* Ends in "..." when str is wider than max_w. */
void ui_font_fit(ui_surface_t *s, const ui_font_t *font, int x, int y, int max_w, uint32_t argb, const char *str);
/* Word-wrapped and centered horizontally in [x, x+w), vertically in [y, y+h). */
void ui_font_wrap(ui_surface_t *s, const ui_font_t *font, int x, int y, int w, int h, int max_lines,
                  uint32_t argb, const char *str);
/* Word-wrapped, left aligned, from the line top y. Returns the height used. */
int ui_font_para(ui_surface_t *s, const ui_font_t *font, int x, int y, int w, uint32_t argb, const char *str);

