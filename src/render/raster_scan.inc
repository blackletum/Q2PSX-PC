/* Fixed-point scan conversion adapted from Mednafen's PS1 gpu_polygon.cpp
 * and gpu_line.cpp, Copyright (C) 2011-2017 Mednafen Team.
 * https://github.com/libretro-mirrors/mednafen-git/tree/master/src/psx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any later version.
 * It is distributed WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See LICENSE.
 *
 * Edges have 32 fractional bits with directed division and a subpixel bias.
 * UV/RGB gradients are truncated to 12 fractional bits. Unsigned 8.24
 * accumulators make wrapping explicit; the low 12 padding bits stay zero.
 * Attribute origin and scan direction depend on the ORIGINAL leftmost
 * vertex, including tie order. Winding normalization would lose that detail.
 */
typedef struct raster_attr {
    u32 base[5], dx[5], dy[5];  /* r, g, b, u, v */
} raster_attr;

static s64 edge_start(int x)
{
    return (s64)x * 4294967296LL + 4294965248LL;
}

static s64 edge_step(int dx, int dy)
{
    s64 n = (s64)dx * 4294967296LL;
    if (n < 0) n -= dy - 1;
    if (n > 0) n += dy - 1;
    return n / dy;
}

/*
 * Everything a span needs that does not change between the spans of one
 * triangle: the clip, the texture page and palette decode that sample_texture
 * derives from tpage/clut/E2 for every texel, and write_pixel's blend mode and
 * mask bits. A BASE3 frame draws ~17,000 spans averaging seven pixels, so the
 * per-pixel and per-span setup both matter. The arithmetic below is
 * sample_texture's and write_pixel's, term for term; only the point at which
 * each term is evaluated moved.
 */
typedef struct raster_span_ctx {
    int cx0, cy0, cx1, cy1;
    const psx_vram *vram;
    psx_texture_cache *cache;
    const u16 *clut_tab;      /* cache->clut, or NULL for a direct VRAM lookup */
    int page_x, page_y, bpp, clut_x, clut_y;
    u32 umask, vmask, ufill, vfill;   /* E2: (u & ~umask) | ufill */
    psx_blend mode;
    u16 mask_force;           /* 0x8000 when E6 bit 0 forces the mask bit */
    bool mask_check;          /* E6 bit 1: skip pixels whose mask bit is set */
    bool semi_prim;           /* opts->semi_transparency && prim->semi_transparent */
    bool modulate;            /* prim->textured_blend */
    bool ui;                  /* opts->ui || prim->ui */
    bool textured, gouraud, dither;
    bool fast4;               /* raster_span_t4_fast's conditions, per triangle */
} raster_span_ctx;

static void raster_span_setup(raster_span_ctx *c, const psx_framebuffer *fb,
                              const psx_prim *prim, const psx_vram *vram,
                              const psx_raster_opts *opts, bool textured,
                              bool gouraud, bool dither)
{
    u32 window = opts->texture_window;
    raster_clip(fb, opts, &c->cx0, &c->cy0, &c->cx1, &c->cy1);
    c->vram = vram;
    c->cache = opts->cache;
    c->clut_tab = opts->cache ? opts->cache->clut : NULL;
    c->page_x = (prim->tpage & 0x0F) * 64;
    c->page_y = ((prim->tpage >> 4) & 1) * 256;
    c->bpp    = (prim->tpage >> 7) & 3;
    c->clut_x = (prim->clut & 0x3F) * 16;
    c->clut_y = (prim->clut >> 6) & 0x1FF;
    c->umask = (window & 31u) << 3;
    c->vmask = ((window >> 5) & 31u) << 3;
    c->ufill = ((window >> 10) << 3) & c->umask;
    c->vfill = ((window >> 15) << 3) & c->vmask;
    c->mode = (psx_blend)((prim->tpage >> 5) & 3);
    c->mask_force = (opts->mask_mode & 1) ? 0x8000u : 0;
    c->mask_check = (opts->mask_mode & 2) != 0;
    c->semi_prim = opts->semi_transparency && prim->semi_transparent;
    c->modulate = prim->textured_blend;
    c->ui = opts->ui || prim->ui;
    c->textured = textured;
    c->gouraud = gouraud;
    c->dither = dither;
    c->fast4 = textured && c->bpp == 0 && dither && c->cache && !c->mask_check &&
               !psx_raster_general_only;
}

/*
 * The pixels of one span. `tex` is -1 for an untextured primitive, else the
 * texture depth (0 = 4-bit, 1 = 8-bit, 2 = direct colour); with `gouraud` and
 * `dither` it is a compile-time constant at every call, so each instance
 * carries only the work its primitive kind needs.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define RASTER_FORCE_INLINE static __forceinline
#else
#define RASTER_FORCE_INLINE static inline __attribute__((always_inline))
#endif
RASTER_FORCE_INLINE void raster_span_pixels(psx_framebuffer *fb, const raster_span_ctx *c,
                                     const raster_attr *a, u32 cur[5],
                                     u16 *row, u16 *vrow, u16 *scene_row,
                                     u8 *ui_row, const s8 *drow, bool row_skip,
                                     int left, int right,
                                     const int tex, const bool gouraud,
                                     const bool dither)
{
    int x;
    const int r0 = (int)(cur[0] >> 24), g0 = (int)(cur[1] >> 24), b0 = (int)(cur[2] >> 24);
    for (x = left; x < right; ++x) {
        int r = gouraud ? (int)(cur[0] >> 24) : r0;
        int g = gouraud ? (int)(cur[1] >> 24) : g0;
        int b = gouraud ? (int)(cur[2] >> 24) : b0;
        bool stp = false, semi;
        u16 colour, mask;
        if (tex >= 0) {
            /* sample_texture, with the page, palette and E2 terms above. */
            u8 u = (u8)(((u8)(cur[3] >> 24) & ~c->umask) | c->ufill);
            u8 v = (u8)(((u8)(cur[4] >> 24) & ~c->vmask) | c->vfill);
            int vy = c->page_y + v;
            u16 texel;
            if (tex == 0) {
                int vx = (c->page_x + (u >> 2)) & 1023;
                u16 word = texture_word(c->vram, c->cache, (unsigned)vx, (unsigned)vy, 0);
                int index = (word >> ((u & 3) * 4)) & 0x0F;
                texel = c->clut_tab ? c->clut_tab[index]
                                    : c->vram->px[c->clut_y][(c->clut_x + index) & 1023];
            } else if (tex == 1) {
                int vx = (c->page_x + (u >> 1)) & 1023;
                u16 word = texture_word(c->vram, c->cache, (unsigned)vx, (unsigned)vy, 1);
                int index = (word >> ((u & 1) * 8)) & 0xFF;
                texel = c->clut_tab ? c->clut_tab[index]
                                    : c->vram->px[c->clut_y][(c->clut_x + index) & 1023];
            } else {
                int vx = (c->page_x + u) & 1023;
                texel = texture_word(c->vram, c->cache, (unsigned)vx, (unsigned)vy, 2);
            }
            if (texel == 0) goto next_pixel;   /* transparent, not black */
            stp = (texel & 0x8000u) != 0;
            if (c->modulate) {
                r = (((texel & 0x1F) << 3) * r) >> 7;
                g = ((((texel >> 5) & 0x1F) << 3) * g) >> 7;
                b = ((((texel >> 10) & 0x1F) << 3) * b) >> 7;
            } else {
                r = (texel & 0x1F) << 3;
                g = ((texel >> 5) & 0x1F) << 3;
                b = ((texel >> 10) & 0x1F) << 3;
            }
        }
        /* Dither precedes saturation, including modulated values above 255. */
        if (dither) {
            int d = drow[(x+fb->vram_x) & 3];
            r += d; g += d; b += d;
        }
        r = q2_clamp_s32(r, 0, 255);
        g = q2_clamp_s32(g, 0, 255);
        b = q2_clamp_s32(b, 0, 255);
        /* write_pixel */
        if (row_skip) goto next_pixel;
        semi = c->semi_prim && (tex < 0 || stp);
        colour = psx_rgb555((u8)r, (u8)g, (u8)b);
        mask = stp ? 0x8000u : c->mask_force;
        if (c->mask_check && (row[x] & 0x8000u)) goto next_pixel;
        row[x] = (u16)((semi ? blend_pixel(row[x], r, g, b, c->mode) : colour) | mask);
        if (vrow) vrow[x] = row[x];
        if (scene_row) {
            if (c->ui) {
                ui_row[x] = 255;
            } else {
                scene_row[x] = (u16)((semi ? blend_pixel(scene_row[x], r, g, b, c->mode) : colour) | mask);
                if (!semi) ui_row[x] = 0;
            }
        }
next_pixel:
        if (gouraud) {
            cur[0] += a->dx[0]; cur[1] += a->dx[1]; cur[2] += a->dx[2];
        }
        if (tex >= 0) {
            cur[3] += a->dx[3]; cur[4] += a->dx[4];
        }
    }
}

/* Saturate a dithered channel to 0..255 and keep its top five bits, for
 * v in -64..511: a table rather than compares, because a modulated channel
 * goes past 255 and a dithered dark one below 0 often enough that the
 * branches the compiler makes of min/max mispredict. A modulated channel is
 * at most (31 << 3) * 255 >> 7 = 494, plus a dither of at most 3; the lowest
 * is -4. */
#define RASTER_SAT5(v) ((v) < 0 ? 0 : (v) > 255 ? 31 : (v) >> 3)
#define RASTER_SAT5_4(b)  RASTER_SAT5(b), RASTER_SAT5((b) + 1), RASTER_SAT5((b) + 2), RASTER_SAT5((b) + 3)
#define RASTER_SAT5_16(b) RASTER_SAT5_4(b), RASTER_SAT5_4((b) + 4), RASTER_SAT5_4((b) + 8), RASTER_SAT5_4((b) + 12)
#define RASTER_SAT5_64(b) RASTER_SAT5_16(b), RASTER_SAT5_16((b) + 16), RASTER_SAT5_16((b) + 32), RASTER_SAT5_16((b) + 48)
static const u8 raster_sat5_table[576] = {
    RASTER_SAT5_64(-64), RASTER_SAT5_64(0),   RASTER_SAT5_64(64),
    RASTER_SAT5_64(128), RASTER_SAT5_64(192), RASTER_SAT5_64(256),
    RASTER_SAT5_64(320), RASTER_SAT5_64(384), RASTER_SAT5_64(448)
};
#undef RASTER_SAT5_64
#undef RASTER_SAT5_16
#undef RASTER_SAT5_4
#undef RASTER_SAT5
#define RASTER_SAT5_AT(v) raster_sat5_table[(v) + 64]

/* blend_pixel after its own >> 3, for five-bit foreground channels. */
static inline u16 raster_blend5(u16 back, int r, int g, int b, psx_blend mode)
{
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    switch (mode) {
    case PSX_BLEND_HALF:    r = (br + r) / 2; g = (bg + g) / 2; b = (bb + b) / 2; break;
    case PSX_BLEND_ADD:     r = br + r; g = bg + g; b = bb + b; break;
    case PSX_BLEND_SUB:     r = br - r; g = bg - g; b = bb - b; break;
    case PSX_BLEND_QUARTER: r = br + r / 4; g = bg + g / 4; b = bb + b / 4; break;
    }
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 31) g = 31;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (u16)(r | (g << 5) | (b << 10));
}

/*
 * The span every gameplay frame is made of: a 4-bit textured, modulated,
 * dithered polygon (world and models alike), through the texture cache, with
 * no mask test and no bloom scene copy. In captured BASE3, WASTE2, JAIL2 and
 * four-player MATRIX5 frames it is every textured pixel, ~150 000 a frame.
 * The arithmetic is raster_span_pixels' for (0, gouraud, true), reordered for
 * the register file:
 *
 * - Two 8.24 accumulators share a 64-bit register. Their low 12 bits are
 *   always zero (gradients are shifted left 12 and the bases are built from
 *   them), so the carry out of the low half adds at most one per pixel to the
 *   high half's zero bits and, for a span under 4096 pixels, never reaches
 *   bit 12. Both halves' top bytes are therefore exactly the separate sums.
 * - A 4-bit page column is page_x + u/4 <= 960 + 63, so the & 1023 wrap never
 *   applies and the VRAM word address is the page base plus v * 1024 + u/4.
 * - Dithered textured polygons are modulated by construction
 *   (raster_triangle: dither needs textured_blend).
 * - The dither row is a rotating byte pattern instead of an indexed load.
 *
 * Texel fetches, cache fills and framebuffer stores happen in the same order
 * as in raster_span_pixels, so a polygon sampling the page it draws into
 * reads the same words.
 */
RASTER_FORCE_INLINE void raster_span_t4_fast(const raster_span_ctx *c, const raster_attr *a,
                                      const u32 cur[5], u16 *row, u16 *vrow,
                                      u32 dither_pattern, int left, int right,
                                      const bool gouraud)
{
    psx_texture_cache *cache = c->cache;
    const u16 *clut = c->clut_tab;
    const u16 *vram_words = &c->vram->px[0][0];
    const u32 umask = ~c->umask & 255u, ufill = c->ufill;
    const u32 vmask = ~c->vmask & 255u, vfill = c->vfill;
    const u32 page = (u32)c->page_y * 1024u + (u32)c->page_x;
    const u16 mask_force = c->mask_force;
    const bool semi_prim = c->semi_prim;
    const psx_blend mode = c->mode;
    /* (r, g) and (b, u) pairs; v alone. */
    u64 rg = ((u64)cur[0] << 32) | cur[1];
    u64 bu = ((u64)cur[2] << 32) | cur[3];
    u32 cv = cur[4];
    const u64 drg = ((u64)a->dx[0] << 32) | a->dx[1];
    const u64 dbu = ((u64)a->dx[2] << 32) | a->dx[3];
    const u32 dv = a->dx[4];
    int x;
    for (x = left; x < right; ++x) {
        u32 u = ((((u32)bu) >> 24) & umask) | ufill;
        u32 v = ((cv >> 24) & vmask) | vfill;
        u32 address = page + v * 1024u + (u >> 2);
        u32 line = ((address >> 2) & 3u) | ((address >> 8) & 252u);
        u32 tag = (address & ~3u) + 1u;
        u16 texel;
        if (cache->tag[line] != tag) {
            memcpy(cache->word[line], vram_words + (address & ~3u), 8);
            cache->tag[line] = tag;
        }
        texel = clut[(cache->word[line][address & 3u] >> ((u & 3u) * 4u)) & 0x0Fu];
        if (texel) {
            int d = (s8)(u8)dither_pattern;
            int r = (int)((((u32)texel & 0x1Fu) << 3) * (u32)(rg >> 56)) >> 7;
            int g = (int)(((((u32)texel >> 5) & 0x1Fu) << 3) * (((u32)rg) >> 24)) >> 7;
            int b = (int)(((((u32)texel >> 10) & 0x1Fu) << 3) * (u32)(bu >> 56)) >> 7;
            bool stp = (texel & 0x8000u) != 0;
            u16 mask = stp ? 0x8000u : mask_force;
            u16 out;
            r = RASTER_SAT5_AT(r + d);
            g = RASTER_SAT5_AT(g + d);
            b = RASTER_SAT5_AT(b + d);
            if (semi_prim && stp)   /* blend_pixel, after its own >> 3 */
                out = (u16)(raster_blend5(row[x], r, g, b, mode) | mask);
            else
                out = (u16)((u32)r | ((u32)g << 5) | ((u32)b << 10) | mask);
            row[x] = out;
            if (vrow) vrow[x] = out;
        }
        if (gouraud) rg += drg;   /* flat: zero colour gradients, r g b fixed */
        bu += dbu;
        cv += dv;
        dither_pattern = (dither_pattern >> 8) | (dither_pattern << 24);
    }
}

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RASTER_HAVE_SSE2 1
/* Spans this long or longer take the two-pass form; shorter ones are mostly
 * set-up and stay with raster_span_t4_fast. The upper bound is the texel
 * buffer, kept small enough to need no stack probe. */
#define RASTER_SSE2_MIN_SPAN 16
#define RASTER_SSE2_MAX_SPAN 1024

/*
 * raster_span_t4_fast for a span of 16 pixels or more, in two passes:
 * every texel of the span through the texture cache first, in pixel
 * order, then the colour arithmetic eight pixels at a time. The caller only
 * comes here when the span's VRAM row cannot be inside the polygon's texture
 * page, so no fetch can read a word this span writes and taking the fetches
 * ahead of the stores changes nothing; the cache sees the same addresses in
 * the same order.
 *
 * The SIMD lanes compute what the scalar loop does: 8-bit accumulators from
 * the same 8.24 sums (lane i of block k is the start plus (8k + i) steps, mod
 * 2^32), modulation as an unsigned 16-bit product (at most 248 * 255 = 63240)
 * shifted by 7, the dither added before saturation to 0..255, the top five
 * bits packed with the mask bit, transparent texels left as they were, and
 * on a semi-transparent polygon the STP texels blended per blend_pixel with
 * the word already in the framebuffer.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#define RASTER_NOINLINE static __declspec(noinline)
#else
#define RASTER_NOINLINE static __attribute__((noinline))
#endif
/* Not inlined: its vector registers and texel buffer would otherwise be set
 * up (and on Windows x64, xmm6-15 saved) by every raster_span call, which in
 * a four-player frame of eight-pixel spans cost more than the vectors saved. */
RASTER_NOINLINE void raster_span_t4_sse2(const raster_span_ctx *c, const raster_attr *a,
                                const u32 cur[5], u16 *row, u16 *vrow,
                                u32 dither_pattern, int left, int right,
                                bool gouraud)
{
    u16 texels[RASTER_SSE2_MAX_SPAN];
    const bool semi = c->semi_prim;
    const psx_blend mode = c->mode;
    psx_texture_cache *cache = c->cache;
    const u16 *clut = c->clut_tab;
    const u16 *vram_words = &c->vram->px[0][0];
    const u32 umask = ~c->umask & 255u, ufill = c->ufill;
    const u32 vmask = ~c->vmask & 255u, vfill = c->vfill;
    const u32 page = (u32)c->page_y * 1024u + (u32)c->page_x;
    u32 cu = cur[3], cv = cur[4];
    const u32 du = a->dx[3], dv = a->dx[4];
    int n = right - left, i;

    /* Pass 1: the fetches, exactly raster_span_t4_fast's. */
    for (i = 0; i < n; ++i) {
        u32 u = ((cu >> 24) & umask) | ufill;
        u32 v = ((cv >> 24) & vmask) | vfill;
        u32 address = page + v * 1024u + (u >> 2);
        u32 line = ((address >> 2) & 3u) | ((address >> 8) & 252u);
        u32 tag = (address & ~3u) + 1u;
        if (cache->tag[line] != tag) {
            memcpy(cache->word[line], vram_words + (address & ~3u), 8);
            cache->tag[line] = tag;
        }
        texels[i] = clut[(cache->word[line][address & 3u] >> ((u & 3u) * 4u)) & 0x0Fu];
        cu += du; cv += dv;
    }

    /* Pass 2: eight pixels at a time. */
    {
        const __m128i zero = _mm_setzero_si128();
        const __m128i c255 = _mm_set1_epi16(255);
        const __m128i c31 = _mm_set1_epi16(31);
        const __m128i hi = _mm_set1_epi16(-32768);   /* 0x8000, the mask bit */
        const __m128i force = _mm_set1_epi16((short)c->mask_force);
        /* Lane i dithers with pattern byte (i & 3); eight is a whole number
         * of dither periods, so the vector is the same for every block. */
        const __m128i dith = _mm_setr_epi16((s8)(u8)dither_pattern, (s8)(u8)(dither_pattern >> 8),
                                            (s8)(u8)(dither_pattern >> 16), (s8)(u8)(dither_pattern >> 24),
                                            (s8)(u8)dither_pattern, (s8)(u8)(dither_pattern >> 8),
                                            (s8)(u8)(dither_pattern >> 16), (s8)(u8)(dither_pattern >> 24));
        __m128i acc[3][2], step[3];
        int ch, x = 0;
        for (ch = 0; ch < 3; ++ch) {
            u32 b0 = cur[ch], d = gouraud ? a->dx[ch] : 0u;
            acc[ch][0] = _mm_setr_epi32((int)b0, (int)(b0 + d), (int)(b0 + 2u * d), (int)(b0 + 3u * d));
            acc[ch][1] = _mm_setr_epi32((int)(b0 + 4u * d), (int)(b0 + 5u * d), (int)(b0 + 6u * d), (int)(b0 + 7u * d));
            step[ch] = _mm_set1_epi32((int)(8u * d));
        }
        for (; x + 8 <= n; x += 8) {
            __m128i t = _mm_loadu_si128((const __m128i *)(texels + x));
            __m128i keep = _mm_cmpeq_epi16(t, zero);
            __m128i out, old;
            __m128i comp[3];
            /* Channel CH's five texel bits sit at bit 5 * CH (an immediate,
             * hence the macro rather than a loop). */
#define RASTER_SSE2_CHANNEL(CH)                                                                      \
            {                                                                                        \
                __m128i lo8 = _mm_srli_epi32(acc[CH][0], 24), hi8 = _mm_srli_epi32(acc[CH][1], 24);  \
                __m128i intensity = _mm_packs_epi32(lo8, hi8);   /* 0..255 */                        \
                __m128i texel8 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(t, 5 * (CH)), c31), 3); \
                __m128i v = _mm_srli_epi16(_mm_mullo_epi16(texel8, intensity), 7);                   \
                v = _mm_add_epi16(v, dith);                                                          \
                v = _mm_min_epi16(_mm_max_epi16(v, zero), c255);                                     \
                comp[CH] = _mm_srli_epi16(v, 3);                                                     \
                acc[CH][0] = _mm_add_epi32(acc[CH][0], step[CH]);                                    \
                acc[CH][1] = _mm_add_epi32(acc[CH][1], step[CH]);                                    \
            }
            RASTER_SSE2_CHANNEL(0)
            RASTER_SSE2_CHANNEL(1)
            RASTER_SSE2_CHANNEL(2)
#undef RASTER_SSE2_CHANNEL
            out = _mm_or_si128(_mm_or_si128(comp[0], _mm_slli_epi16(comp[1], 5)),
                               _mm_or_si128(_mm_slli_epi16(comp[2], 10),
                                            _mm_or_si128(_mm_and_si128(t, hi), force)));
            old = _mm_loadu_si128((const __m128i *)(row + left + x));
            if (semi) {
                /* A semi-transparent polygon blends its STP texels with the
                 * framebuffer word (blend_pixel after its >> 3); the rest are
                 * opaque. */
                __m128i stp = _mm_cmpeq_epi16(_mm_and_si128(t, hi), hi);
                if (_mm_movemask_epi8(stp)) {
                    __m128i br = _mm_and_si128(old, c31);
                    __m128i bg = _mm_and_si128(_mm_srli_epi16(old, 5), c31);
                    __m128i bb = _mm_and_si128(_mm_srli_epi16(old, 10), c31);
                    __m128i nr, ng, nb, blended;
                    switch (mode) {
                    case PSX_BLEND_HALF:
                        nr = _mm_srli_epi16(_mm_add_epi16(br, comp[0]), 1);
                        ng = _mm_srli_epi16(_mm_add_epi16(bg, comp[1]), 1);
                        nb = _mm_srli_epi16(_mm_add_epi16(bb, comp[2]), 1);
                        break;
                    case PSX_BLEND_ADD:
                        nr = _mm_min_epi16(_mm_add_epi16(br, comp[0]), c31);
                        ng = _mm_min_epi16(_mm_add_epi16(bg, comp[1]), c31);
                        nb = _mm_min_epi16(_mm_add_epi16(bb, comp[2]), c31);
                        break;
                    case PSX_BLEND_SUB:
                        nr = _mm_max_epi16(_mm_sub_epi16(br, comp[0]), zero);
                        ng = _mm_max_epi16(_mm_sub_epi16(bg, comp[1]), zero);
                        nb = _mm_max_epi16(_mm_sub_epi16(bb, comp[2]), zero);
                        break;
                    default:   /* PSX_BLEND_QUARTER */
                        nr = _mm_min_epi16(_mm_add_epi16(br, _mm_srli_epi16(comp[0], 2)), c31);
                        ng = _mm_min_epi16(_mm_add_epi16(bg, _mm_srli_epi16(comp[1], 2)), c31);
                        nb = _mm_min_epi16(_mm_add_epi16(bb, _mm_srli_epi16(comp[2], 2)), c31);
                        break;
                    }
                    blended = _mm_or_si128(_mm_or_si128(nr, _mm_slli_epi16(ng, 5)),
                                           _mm_or_si128(_mm_slli_epi16(nb, 10), hi));
                    out = _mm_or_si128(_mm_and_si128(stp, blended), _mm_andnot_si128(stp, out));
                }
            }
            _mm_storeu_si128((__m128i *)(row + left + x),
                             _mm_or_si128(_mm_and_si128(keep, old), _mm_andnot_si128(keep, out)));
            if (vrow) {
                old = _mm_loadu_si128((const __m128i *)(vrow + left + x));
                _mm_storeu_si128((__m128i *)(vrow + left + x),
                                 _mm_or_si128(_mm_and_si128(keep, old), _mm_andnot_si128(keep, out)));
            }
        }
        /* The last few pixels, as raster_span_t4_fast does them. */
        if (x < n) {
            u32 rr = cur[0] + (gouraud ? (u32)x * a->dx[0] : 0u);
            u32 gg = cur[1] + (gouraud ? (u32)x * a->dx[1] : 0u);
            u32 bb = cur[2] + (gouraud ? (u32)x * a->dx[2] : 0u);
            u32 pattern = dither_pattern;   /* x is a multiple of 8, so the phase is unchanged */
            for (; x < n; ++x) {
                u16 texel = texels[x];
                if (texel) {
                    int d = (s8)(u8)pattern;
                    int r = (int)((((u32)texel & 0x1Fu) << 3) * (rr >> 24)) >> 7;
                    int g = (int)(((((u32)texel >> 5) & 0x1Fu) << 3) * (gg >> 24)) >> 7;
                    int b = (int)(((((u32)texel >> 10) & 0x1Fu) << 3) * (bb >> 24)) >> 7;
                    u16 out;
                    r = RASTER_SAT5_AT(r + d);
                    g = RASTER_SAT5_AT(g + d);
                    b = RASTER_SAT5_AT(b + d);
                    if (semi && (texel & 0x8000u))
                        out = (u16)(raster_blend5(row[left + x], r, g, b, mode) | 0x8000u);
                    else
                        out = (u16)((u32)r | ((u32)g << 5) | ((u32)b << 10) |
                                    ((texel & 0x8000u) ? 0x8000u : c->mask_force));
                    row[left + x] = out;
                    if (vrow) vrow[left + x] = out;
                }
                if (gouraud) { rr += a->dx[0]; gg += a->dx[1]; bb += a->dx[2]; }
                pattern = (pattern >> 8) | (pattern << 24);
            }
        }
    }
}
#endif

static void raster_span(psx_framebuffer *fb, const psx_raster_opts *opts,
                         const raster_span_ctx *c, const raster_attr *a,
                         int y, int left, int right)
{
    int i, attr_x=left, attr_y=y;
    u32 cur[5];
    u16 *row, *vrow, *scene_row;
    u8 *ui_row;
    const s8 *drow;
    bool row_skip;
    if (opts->native_coords) {
        int width=right-left;
        left=signed11(left+fb->vram_x)-fb->vram_x;
        right=left+width;
        y=signed11(y+fb->vram_y)-fb->vram_y;
    }
    if (y < c->cy0 || y >= c->cy1) return;
    if (left < c->cx0) { attr_x+=c->cx0-left; left = c->cx0; }
    if (right > c->cx1) right = c->cx1;
    if (left >= right) return;   /* no pixel, so no fetch and no store either */
    for (i = 0; i < 5; ++i)
        cur[i] = a->base[i] + a->dx[i] * (u32)attr_x + a->dy[i] * (u32)attr_y;

    /* write_pixel's row terms. The row is inside the clip, so at / width is y
     * for every pixel of the span and the field test decides the whole row;
     * a skipped row still samples its texels, since the texture cache fills
     * either way. */
    row = fb->px + y * fb->width;
    vrow = fb->vram ? &fb->vram->px[fb->vram_y + y][fb->vram_x] : NULL;
    scene_row = opts->scene ? opts->scene + y * fb->width : NULL;
    ui_row = opts->scene ? opts->ui_mask + y * fb->width : NULL;
    drow = psx_dither_matrix[(y + fb->vram_y) & 3];
    row_skip = opts->skip_field && (((y + fb->vram_y) & 1) == opts->field);

    if (c->fast4 && !row_skip && !scene_row && right - left < 4096) {
        int ph = (left + fb->vram_x) & 3;
        u32 pattern = (u32)(u8)drow[ph] | ((u32)(u8)drow[(ph + 1) & 3] << 8) |
                      ((u32)(u8)drow[(ph + 2) & 3] << 16) | ((u32)(u8)drow[(ph + 3) & 3] << 24);
#ifdef RASTER_HAVE_SSE2
        /* The span's VRAM row against the 4-bit page (64 words by 256 rows):
         * only when they cannot meet may the fetches run ahead. */
        if (right - left >= RASTER_SSE2_MIN_SPAN &&
            right - left <= RASTER_SSE2_MAX_SPAN &&
            (!vrow || (u32)(fb->vram_y + y - c->page_y) >= 256u ||
             fb->vram_x + right <= c->page_x || fb->vram_x + left >= c->page_x + 64)) {
            raster_span_t4_sse2(c, a, cur, row, vrow, pattern, left, right, c->gouraud);
            return;
        }
#endif
        if (c->gouraud) raster_span_t4_fast(c, a, cur, row, vrow, pattern, left, right, true);
        else            raster_span_t4_fast(c, a, cur, row, vrow, pattern, left, right, false);
        return;
    }

    /* One instance of the pixel loop per (texture depth, Gouraud, dither)
     * so that none of those is a test inside it. A flat primitive's colour
     * accumulators have zero gradients (raster_triangle leaves dx/dy at 0), so
     * reading them once is the same as stepping them. */
#define RASTER_SPAN_PIXELS(T, G, D) \
    raster_span_pixels(fb, c, a, cur, row, vrow, scene_row, ui_row, drow, \
                       row_skip, left, right, (T), (G), (D))
    {
        int tex = c->textured ? (c->bpp >= 2 ? 2 : c->bpp) : -1;
        if (c->gouraud) {
            if (c->dither) {
                switch (tex) {
                case -1: RASTER_SPAN_PIXELS(-1, true, true); break;
                case 0:  RASTER_SPAN_PIXELS(0, true, true); break;
                case 1:  RASTER_SPAN_PIXELS(1, true, true); break;
                default: RASTER_SPAN_PIXELS(2, true, true); break;
                }
            } else {
                switch (tex) {
                case -1: RASTER_SPAN_PIXELS(-1, true, false); break;
                case 0:  RASTER_SPAN_PIXELS(0, true, false); break;
                case 1:  RASTER_SPAN_PIXELS(1, true, false); break;
                default: RASTER_SPAN_PIXELS(2, true, false); break;
                }
            }
        } else {
            if (c->dither) {
                switch (tex) {
                case -1: RASTER_SPAN_PIXELS(-1, false, true); break;
                case 0:  RASTER_SPAN_PIXELS(0, false, true); break;
                case 1:  RASTER_SPAN_PIXELS(1, false, true); break;
                default: RASTER_SPAN_PIXELS(2, false, true); break;
                }
            } else {
                switch (tex) {
                case -1: RASTER_SPAN_PIXELS(-1, false, false); break;
                case 0:  RASTER_SPAN_PIXELS(0, false, false); break;
                case 1:  RASTER_SPAN_PIXELS(1, false, false); break;
                default: RASTER_SPAN_PIXELS(2, false, false); break;
                }
            }
        }
    }
#undef RASTER_SPAN_PIXELS
}

static void raster_triangle(psx_framebuffer *fb,
                            const raster_vertex *v0,
                            const raster_vertex *v1,
                            const raster_vertex *v2,
                            const psx_prim *prim,
                            const psx_vram *vram,
                            const psx_raster_opts *opts)
{
    const raster_vertex *v[3] = {v0,v1,v2}, *core, *tmp;
    raster_attr attr = {0};
    raster_span_ctx span;
    s64 base, slope, upper = 0, lower = 0;
    int den, cv, i, j, vo, vp;
    bool right, textured, gouraud, dither;
    struct {
        s64 x[2], step[2];
        int y, end;
        bool up;
    } part[2];

    /* Hardware rejects each triangle separately, including halves of a quad. */
    for (i=0;i<3;++i) for (j=i+1;j<3;++j)
        if (abs(v[i]->x-v[j]->x)>PSX_PRIM_MAX_SPAN_X ||
            abs(v[i]->y-v[j]->y)>PSX_PRIM_MAX_SPAN_Y) return;

    if (v[1]->x <= v[0]->x) core = v[2]->x <= v[1]->x ? v[2] : v[1];
    else core = v[2]->x < v[0]->x ? v[2] : v[0];
    if (v[2]->y < v[1]->y) { tmp=v[2]; v[2]=v[1]; v[1]=tmp; }
    if (v[1]->y < v[0]->y) { tmp=v[1]; v[1]=v[0]; v[0]=tmp; }
    if (v[2]->y < v[1]->y) { tmp=v[2]; v[2]=v[1]; v[1]=tmp; }
    cv = core == v[0] ? 0 : core == v[1] ? 1 : 2;
    den = (v[1]->x-v[0]->x)*(v[2]->y-v[1]->y) -
          (v[2]->x-v[1]->x)*(v[1]->y-v[0]->y);
    if (!den) return;
    textured = opts->textures && vram &&
        (prim->kind == PSX_PRIM_FT3 || prim->kind == PSX_PRIM_GT3 ||
         prim->kind == PSX_PRIM_FT4 || prim->kind == PSX_PRIM_GT4);
    gouraud = prim->kind == PSX_PRIM_G3 || prim->kind == PSX_PRIM_G4 ||
              prim->kind == PSX_PRIM_GT3 || prim->kind == PSX_PRIM_GT4;
    if (textured && opts->missing_texture_bank) return;
    dither = opts->dither && (textured ? prim->textured_blend : gouraud);

    for (i=0;i<5;++i) {
        int av[3];
        for (j=0;j<3;++j) {
            switch (i) {
            case 0: av[j]=v[j]->r; break;
            case 1: av[j]=v[j]->g; break;
            case 2: av[j]=v[j]->b; break;
            case 3: av[j]=v[j]->u; break;
            default:av[j]=v[j]->v; break;
            }
        }
        if (i<3 ? gouraud : textured) {
            s64 nx=(av[1]-av[0])*(v[2]->y-v[1]->y)-
                   (av[2]-av[1])*(v[1]->y-v[0]->y);
            s64 ny=(v[1]->x-v[0]->x)*(av[2]-av[1])-
                   (v[2]->x-v[1]->x)*(av[1]-av[0]);
            /* A 32-bit divide when the product fits, which for a polygon
             * inside the 1023 x 511 limit it always does (|ny| <= 2 * 1023
             * * 255); C truncates toward zero at either width. */
            if (nx >= -524287 && nx <= 524287 && ny >= -524287 && ny <= 524287 &&
                !psx_raster_general_only) {
                attr.dx[i]=(u32)((s32)nx*4096/den)<<12;
                attr.dy[i]=(u32)((s32)ny*4096/den)<<12;
            } else {
                attr.dx[i]=(u32)(nx*4096/den)<<12;
                attr.dy[i]=(u32)(ny*4096/den)<<12;
            }
        }
        attr.base[i]=((u32)av[cv]<<24)+0x800000u -
                     attr.dx[i]*(u32)core->x-attr.dy[i]*(u32)core->y;
    }

    base=edge_start(v[0]->x);
    slope=edge_step(v[2]->x-v[0]->x,v[2]->y-v[0]->y);
    if (v[1]->y==v[0]->y) right=v[1]->x>v[0]->x;
    else {
        upper=edge_step(v[1]->x-v[0]->x,v[1]->y-v[0]->y);
        right=upper>slope;
    }
    if (v[2]->y!=v[1]->y)
        lower=edge_step(v[2]->x-v[1]->x,v[2]->y-v[1]->y);
    vo = cv ? 1 : 0;
    vp = cv == 2 ? 3 : 0;
    part[vo].y=v[vo]->y;
    part[vo].end=v[1^vo]->y;
    part[vo].x[right]=edge_start(v[vo]->x);
    part[vo].step[right]=upper;
    part[vo].x[!right]=base+(v[vo]->y-v[0]->y)*slope;
    part[vo].step[!right]=slope;
    part[vo].up=vo!=0;
    part[vo^1].y=v[1^vp]->y;
    part[vo^1].end=v[2^vp]->y;
    part[vo^1].x[right]=edge_start(v[1^vp]->x);
    part[vo^1].step[right]=lower;
    part[vo^1].x[!right]=base+(v[1^vp]->y-v[0]->y)*slope;
    part[vo^1].step[!right]=slope;
    part[vo^1].up=vp!=0;
    raster_span_setup(&span,fb,prim,vram,opts,textured,gouraud,dither);
    for (i=0;i<2;++i) {
        int y=part[i].y;
        s64 left=part[i].x[0], right_x=part[i].x[1];
        while (y!=part[i].end) {
            if (part[i].up) {
                --y; left-=part[i].step[0]; right_x-=part[i].step[1];
            }
            raster_span(fb,opts,&span,&attr,y,(int)(left>>32),
                        (int)(right_x>>32));
            if (!part[i].up) {
                ++y; left+=part[i].step[0]; right_x+=part[i].step[1];
            }
        }
    }
}

static void raster_line(psx_framebuffer *fb, const psx_prim *prim,
                        const psx_raster_opts *opts)
{
    int first=0, last=1, k, i, c, x0, y0, x1, y1;
    int cx0, cy0, cx1, cy1;
    s64 x,y,dx=0,dy=0;
    u32 rgb[3], step[3]={0};
    bool gouraud=prim->kind==PSX_PRIM_LINE_G2;
    x0=prim->xy[0].x+opts->ofs_x; y0=prim->xy[0].y+opts->ofs_y;
    x1=prim->xy[1].x+opts->ofs_x; y1=prim->xy[1].y+opts->ofs_y;
    if (abs(x1-x0)>PSX_PRIM_MAX_SPAN_X || abs(y1-y0)>PSX_PRIM_MAX_SPAN_Y) return;
    k=abs(x1-x0)>abs(y1-y0)?abs(x1-x0):abs(y1-y0);
    if (x0>=x1 && k) {
        int t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t;
        first=1; last=0;
    }
    if (k) { dx=edge_step(x1-x0,k); dy=edge_step(y1-y0,k); }
    x=(s64)x0*4294967296LL+2147483648LL-1024;
    y=(s64)y0*4294967296LL+2147483648LL-(dy<0?1024:0);
    for (c=0;c<3;++c) {
        int a=c==0?prim->rgb[first].r:c==1?prim->rgb[first].g:prim->rgb[first].b;
        int b=c==0?prim->rgb[last].r:c==1?prim->rgb[last].g:prim->rgb[last].b;
        if (!gouraud) a=c==0?prim->rgb[0].r:c==1?prim->rgb[0].g:prim->rgb[0].b;
        rgb[c]=((u32)a<<12)+2048;
        if (gouraud && k) step[c]=(u32)((b-a)*4096/k);
    }
    raster_clip(fb,opts,&cx0,&cy0,&cx1,&cy1);
    for (i=0;i<=k;++i) {
        int px=(int)(x>>32), py=(int)(y>>32), colour[3];
        if (opts->native_coords) {
            px=signed11(px+fb->vram_x)-fb->vram_x;
            py=signed11(py+fb->vram_y)-fb->vram_y;
        }
        if (px>=cx0 && px<cx1 && py>=cy0 && py<cy1) {
            for (c=0;c<3;++c) {
                int value=(u8)(rgb[c]>>12);
                if (opts->dither)
                    value+=psx_dither_matrix[(py+fb->vram_y)&3][(px+fb->vram_x)&3];
                colour[c]=q2_clamp_s32(value,0,255);
            }
            write_pixel(fb,opts,prim,py*fb->width+px,colour[0],colour[1],colour[2],
                        opts->semi_transparency&&prim->semi_transparent,false);
        }
        x+=dx; y+=dy;
        for (c=0;c<3;++c) rgb[c]+=step[c];
    }
}
