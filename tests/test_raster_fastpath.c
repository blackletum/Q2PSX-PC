/* The rasteriser's specialised loops against its general ones.
 *
 * raster.c draws the common cases through dedicated code: the 4-bit
 * modulated, dithered polygon span (scalar, and on x86 an SSE2 two-pass form
 * when the span cannot overlap its texture page), and untextured rectangle
 * rows. psx_raster_general_only routes everything through the general loops
 * instead. Every packet stream here is drawn both ways from identical
 * framebuffer, VRAM and GPU state, and the results (framebuffer, VRAM, the
 * texture cache) must match byte for byte. The streams are random but cover
 * what the fast paths decide on: texture depth, modulation, dither, Gouraud,
 * semi-transparency and blend mode, the mask bits, texture windows, field
 * skipping, spans long and short, pages that overlap the draw buffer in VRAM
 * and pages that do not, transparent and set-STP texels, and channels that
 * saturate at both ends. */
#include "raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static u32 seed = 0x2468ACE1u;

static u32 rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static int range(int lo, int hi) { return lo + (int)(rnd() % (u32)(hi - lo + 1)); }

typedef struct scene {
    psx_framebuffer fb;
    psx_vram *vram;
    psx_draw_state state;
} scene;

static void scene_init(scene *s, const psx_vram *source, int w, int h, int bx, int by, bool bind)
{
    memset(s, 0, sizeof(*s));
    s->vram = (psx_vram *)malloc(sizeof(psx_vram));
    memcpy(s->vram, source, sizeof(psx_vram));
    psx_fb_init(&s->fb, w, h);
    if (bind) psx_fb_bind_vram(&s->fb, s->vram, bx, by);
    s->state.mode = 0x0200u;   /* dither on, as SetDefDrawEnv leaves it */
}

static void scene_free(scene *s)
{
    psx_fb_free(&s->fb);
    free(s->vram);
}

static psx_prim random_prim(int w, int h)
{
    static const psx_prim_kind kinds[] = {
        PSX_PRIM_GT4, PSX_PRIM_GT4, PSX_PRIM_GT3, PSX_PRIM_GT3, PSX_PRIM_FT4, PSX_PRIM_FT3,
        PSX_PRIM_G3, PSX_PRIM_F4, PSX_PRIM_TILE, PSX_PRIM_TILE, PSX_PRIM_SPRT,
        PSX_PRIM_LINE_G2, PSX_PRIM_TWINDOW, PSX_PRIM_MASK, PSX_PRIM_TPAGE
    };
    psx_prim p;
    int i, cx = range(-40, w + 40), cy = range(-40, h + 40);
    int size = (rnd() & 3) == 0 ? range(100, 520) : range(1, 60);
    memset(&p, 0, sizeof(p));
    p.kind = kinds[rnd() % (sizeof(kinds) / sizeof(kinds[0]))];
    for (i = 0; i < 4; ++i) {
        p.xy[i].x = (s16)(cx + range(-size, size));
        p.xy[i].y = (s16)(cy + range(-size / 2, size / 2));
        p.uv[i].u = (u8)rnd();
        p.uv[i].v = (u8)rnd();
        /* Over 128 modulates past 255; dark values dither below 0. */
        p.rgb[i].r = (u8)((rnd() & 3) ? range(96, 255) : range(0, 12));
        p.rgb[i].g = (u8)range(0, 255);
        p.rgb[i].b = (u8)((rnd() & 1) ? 255 : range(0, 255));
    }
    if (p.kind == PSX_PRIM_TILE || p.kind == PSX_PRIM_SPRT) {
        p.xy[2].x = (s16)(p.xy[0].x + range(0, (rnd() & 1) ? w : 40));
        p.xy[2].y = (s16)(p.xy[0].y + range(0, (rnd() & 1) ? h : 40));
    }
    /* Mostly 4-bit pages, the fast path's own; some below x = 512, where
     * the draw buffer lives, so the overlap test has to refuse. */
    p.tpage = psx_make_tpage(range(0, 15), (int)(rnd() & 1), (psx_blend)(rnd() & 3),
                             (rnd() % 5) < 3 ? PSX_TEX_4BIT : (psx_tex_bpp)range(1, 2));
    if (p.kind == PSX_PRIM_TPAGE) p.tpage |= (u16)((rnd() & 1) ? 0x0200u : 0);
    p.clut = psx_make_clut(range(0, 63) * 16, range(0, 511));
    p.semi_transparent = (rnd() % 4) == 0;
    p.textured_blend = (rnd() % 8) != 0;
    p.quad_zorder = (rnd() & 1) != 0;
    p.texture_window = (rnd() % 6) == 0 ? (rnd() & 0xFFFFFu) : 0;
    p.mask_mode = (u8)((rnd() % 5) == 0 ? (rnd() & 3) : 0);
    return p;
}

static bool same(const scene *a, const scene *b)
{
    return !memcmp(a->fb.px, b->fb.px, (size_t)a->fb.width * a->fb.height * sizeof(u16)) &&
           !memcmp(a->vram->px, b->vram->px, sizeof(a->vram->px)) &&
           !memcmp(&a->state.cache.tag, &b->state.cache.tag, sizeof(a->state.cache.tag)) &&
           !memcmp(&a->state.cache.word, &b->state.cache.word, sizeof(a->state.cache.word));
}

/* One stream of `count` packets into a w x h buffer, drawn both ways. */
static void stream(const char *name, const psx_vram *source, int w, int h, int bx, int by,
                   bool bind, bool native, bool interlaced, int count)
{
    scene fast, general;
    psx_raster_opts opts;
    int i, first_bad = -1;
    scene_init(&fast, source, w, h, bx, by, bind);
    scene_init(&general, source, w, h, bx, by, bind);
    psx_raster_opts_default(&opts);
    opts.native_coords = native;
    if (interlaced) {
        psx_draw_state_gp1(&fast.state, 0x08000024u);
        psx_draw_state_gp1(&general.state, 0x08000024u);
    }
    for (i = 0; i < count; ++i) {
        psx_prim p = random_prim(w, h);
        if ((i & 63) == 0) {   /* a fresh clip and offset now and then */
            opts.clip_x = (s16)range(0, w / 4); opts.clip_y = (s16)range(0, h / 4);
            opts.clip_w = (s16)range(w / 2, w); opts.clip_h = (s16)range(h / 2, h);
            opts.ofs_x = (s16)range(-8, 8); opts.ofs_y = (s16)range(-8, 8);
            fast.state.field = general.state.field = (u8)(rnd() & 1);
        }
        psx_raster_general_only = false;
        psx_raster_command(&fast.fb, &p, fast.vram, &opts, &fast.state);
        psx_raster_general_only = true;
        psx_raster_command(&general.fb, &p, general.vram, &opts, &general.state);
        psx_raster_general_only = false;
        /* Compared every fourth packet: a full VRAM compare after each one
         * doubles the run, and a wrong pixel is rarely redrawn exactly. */
        if (first_bad < 0 && ((i & 3) == 3 || i == count - 1) && !same(&fast, &general))
            first_bad = i;
    }
    printf("%-34s %d packets: %s", name, count, first_bad < 0 ? "identical\n" : "DIFFER");
    if (first_bad >= 0) { printf(" by packet %d\n", first_bad); ++failures; }
    scene_free(&fast);
    scene_free(&general);
}

int main(void)
{
    psx_vram *source = (psx_vram *)malloc(sizeof(psx_vram));
    int x, y;
    /* Texels and palettes: one word in eight zero (transparent), a quarter
     * with the STP bit, the rest anything. */
    for (y = 0; y < PSX_VRAM_HEIGHT; ++y)
        for (x = 0; x < PSX_VRAM_WIDTH; ++x) {
            u32 r = rnd();
            source->px[y][x] = (r & 7) == 0 ? 0 : (u16)((r >> 3) & ((r & 0x18) ? 0x7FFFu : 0xFFFFu));
        }
    source->transfer_serial = 1;

    stream("bound to VRAM (0,0), native", source, 512, 248, 0, 0, true, true, false, 6000);
    stream("bound to VRAM (0,256), native", source, 512, 248, 0, 256, true, true, false, 6000);
    stream("bound to VRAM (512,0)", source, 320, 240, 512, 0, true, false, false, 4000);
    stream("unbound host buffer", source, 640, 480, 0, 0, false, false, false, 3000);
    stream("480i field skipping", source, 512, 480, 0, 0, true, true, true, 3000);
    free(source);
    printf("Raster fast paths: %d failures\n", failures);
    return failures ? 1 : 0;
}
