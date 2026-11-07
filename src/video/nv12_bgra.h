/* NV12 to BGRA for the display path. The stream is requested as BT.709, limited
 * range (see config.c): luma 16..235 must become 0..255, or blacks stay grey and
 * whites stay dim. */
#ifndef NV12_BGRA_H
#define NV12_BGRA_H

#include <emmintrin.h>
#include <stddef.h>
#include <stdint.h>

/* Luma gain in 1/256ths: 299 rather than the exact 298 so that 235 reaches 255. */
#define BT709_LUMA_8 299
/* Chroma gains in 1/64ths: R = 1.793 V, G = 0.213 U + 0.533 V, B = 2.112 U. */
#define BT709_RV 115
#define BT709_GU 14
#define BT709_GV 34
#define BT709_BU 135

/*
 * Store mode for the BGRA output. Non-temporal only pays off on WC_GARLIC:
 * on the ONION/WB alias the cache is what hides the coherent bus latency, and
 * bypassing it measured ~6x slower (8.3 ms vs 1.4 ms per 1080p frame).
 */
#define BGRA_ST_UNALIGNED 0
#define BGRA_ST_ALIGNED   1
#define BGRA_ST_NT        2

/* 8 px BGRA from Y (epi16) + precomputed chroma terms. */
static inline __attribute__((always_inline)) void
bgra_store8(uint8_t *dp, __m128i y16, __m128i rv, __m128i guv, __m128i bu,
            __m128i a255, const int st_mode) {
    /* (Y - 16) * 299 / 256, saturating below black. */
    __m128i luma = _mm_mulhi_epu16(_mm_slli_epi16(_mm_subs_epu16(y16, _mm_set1_epi16(16)), 8),
                                   _mm_set1_epi16(BT709_LUMA_8));
    __m128i r = _mm_add_epi16(luma, rv);
    __m128i g = _mm_sub_epi16(luma, guv);
    __m128i b = _mm_add_epi16(luma, bu);
    __m128i br = _mm_packus_epi16(b, r);
    __m128i ga = _mm_packus_epi16(g, a255);
    __m128i bg0 = _mm_unpacklo_epi8(br, ga);
    __m128i ra0 = _mm_unpackhi_epi8(br, ga);
    __m128i px0 = _mm_unpacklo_epi16(bg0, ra0);
    __m128i px1 = _mm_unpackhi_epi16(bg0, ra0);
    if (st_mode == BGRA_ST_NT) {
        /* Callers pair +0/+32 so full 64B lines are written. */
        _mm_stream_si128((__m128i *)(void *)dp, px0);
        _mm_stream_si128((__m128i *)(void *)(dp + 16), px1);
    } else if (st_mode == BGRA_ST_ALIGNED) {
        _mm_store_si128((__m128i *)(void *)dp, px0);
        _mm_store_si128((__m128i *)(void *)(dp + 16), px1);
    } else {
        _mm_storeu_si128((__m128i *)(void *)dp, px0);
        _mm_storeu_si128((__m128i *)(void *)(dp + 16), px1);
    }
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* uu and vv are centred on 0. */
static inline void nv12_to_bgra_px(uint8_t *dp, int yy, int uu, int vv) {
    int luma = yy > 16 ? ((yy - 16) * BT709_LUMA_8) >> 8 : 0;
    dp[0] = clamp_u8(luma + ((BT709_BU * uu) >> 6));
    dp[1] = clamp_u8(luma - ((BT709_GU * uu + BT709_GV * vv) >> 6));
    dp[2] = clamp_u8(luma + ((BT709_RV * vv) >> 6));
    dp[3] = 0xFF;
}

/*
 * 16 px/iter, 2 Y rows per UV row (4:2:0: chroma computed once per pair),
 * prefetch next row pair (Jaguar). Constant `st_mode` → compiler emits
 * a single store variant per instantiation.
 *
 * Chroma coefficients are >>6: |chroma|·coeff ≤ 17280 < 32768, so mullo_epi16
 * cannot wrap (a >>8 path with 359/454 once painted lilac as #ffc400).
 */
static inline __attribute__((always_inline)) void
nv12_to_bgra_impl(uint8_t *dst, int dst_pitch, int w, int h,
                  const uint8_t *y, const uint8_t *uv,
                  int pitch_y, int pitch_uv, const int st_mode) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i m00ff = _mm_set1_epi16(0x00FF);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i c_rv = _mm_set1_epi16(BT709_RV);
    const __m128i c_gu = _mm_set1_epi16(BT709_GU);
    const __m128i c_gv = _mm_set1_epi16(BT709_GV);
    const __m128i c_bu = _mm_set1_epi16(BT709_BU);
    const __m128i a255 = _mm_set1_epi16(255);

    int row = 0;
    for (; row + 1 < h; row += 2) {
        uint8_t *drow0 = dst + (size_t)row * (size_t)dst_pitch;
        uint8_t *drow1 = drow0 + dst_pitch;
        const uint8_t *yrow0 = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *yrow1 = yrow0 + pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        int x = 0;
        for (; x + 15 < w; x += 16) {
            _mm_prefetch((const char *)(yrow1 + pitch_y + x), _MM_HINT_T0);
            _mm_prefetch((const char *)(yrow1 + 2 * pitch_y + x), _MM_HINT_T0);
            _mm_prefetch((const char *)(uvrow + pitch_uv + x), _MM_HINT_T0);

            /* 16B interleaved UV = chroma for 16 px; deinterleave by mask/shift */
            __m128i uv8 = _mm_loadu_si128((const __m128i *)(const void *)(uvrow + x));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c_rv), 6);
            __m128i guv = _mm_srai_epi16(
                _mm_add_epi16(_mm_mullo_epi16(u16, c_gu),
                              _mm_mullo_epi16(v16, c_gv)), 6);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c_bu), 6);
            /* duplicate each chroma sample to its 2 px */
            __m128i rv_lo = _mm_unpacklo_epi16(rv, rv);
            __m128i rv_hi = _mm_unpackhi_epi16(rv, rv);
            __m128i guv_lo = _mm_unpacklo_epi16(guv, guv);
            __m128i guv_hi = _mm_unpackhi_epi16(guv, guv);
            __m128i bu_lo = _mm_unpacklo_epi16(bu, bu);
            __m128i bu_hi = _mm_unpackhi_epi16(bu, bu);

            __m128i y0 = _mm_loadu_si128((const __m128i *)(const void *)(yrow0 + x));
            __m128i y1 = _mm_loadu_si128((const __m128i *)(const void *)(yrow1 + x));
            bgra_store8(drow0 + (size_t)x * 4,
                        _mm_unpacklo_epi8(y0, zero), rv_lo, guv_lo, bu_lo, a255, st_mode);
            bgra_store8(drow0 + (size_t)x * 4 + 32,
                        _mm_unpackhi_epi8(y0, zero), rv_hi, guv_hi, bu_hi, a255, st_mode);
            bgra_store8(drow1 + (size_t)x * 4,
                        _mm_unpacklo_epi8(y1, zero), rv_lo, guv_lo, bu_lo, a255, st_mode);
            bgra_store8(drow1 + (size_t)x * 4 + 32,
                        _mm_unpackhi_epi8(y1, zero), rv_hi, guv_hi, bu_hi, a255, st_mode);
        }
        for (; x < w; x++) {
            int uu = (int)uvrow[(x / 2) * 2] - 128;
            int vv = (int)uvrow[(x / 2) * 2 + 1] - 128;
            nv12_to_bgra_px(drow0 + (size_t)x * 4, yrow0[x], uu, vv);
            nv12_to_bgra_px(drow1 + (size_t)x * 4, yrow1[x], uu, vv);
        }
    }
    /* fila suelta si h impar */
    for (; row < h; row++) {
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        for (int x = 0; x < w; x++) {
            int uu = (int)uvrow[(x / 2) * 2] - 128;
            int vv = (int)uvrow[(x / 2) * 2 + 1] - 128;
            nv12_to_bgra_px(drow + (size_t)x * 4, yrow[x], uu, vv);
        }
    }
}

#endif
