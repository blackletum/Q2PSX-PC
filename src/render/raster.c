#include "raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void psx_raster_opts_default(psx_raster_opts *opts)
{
    if (!opts)
        return;
    opts->dither            = true;
    opts->affine_uv         = true;
    opts->semi_transparency = true;
    opts->textures          = true;
}

q2_result psx_fb_init(psx_framebuffer *fb, int width, int height)
{
    if (!fb || width <= 0 || height <= 0)
        return Q2_ERR_INVALID_ARG;

    fb->px = (u16 *)calloc((size_t)width * (size_t)height, sizeof(u16));
    if (!fb->px)
        return Q2_ERR_NO_MEMORY;

    fb->width  = width;
    fb->height = height;
    return Q2_OK;
}

void psx_fb_free(psx_framebuffer *fb)
{
    if (!fb)
        return;
    free(fb->px);
    fb->px = NULL;
    fb->width = fb->height = 0;
}

void psx_fb_clear(psx_framebuffer *fb, u16 colour)
{
    int i, n;

    if (!fb || !fb->px)
        return;

    n = fb->width * fb->height;
    for (i = 0; i < n; i++)
        fb->px[i] = colour;
}

/* ------------------------------------------------------------------------- */
/* Blending                                                                   */
/* ------------------------------------------------------------------------- */
static void unpack555(u16 c, int *r, int *g, int *b)
{
    *r = (c & 0x1F) << 3;
    *g = ((c >> 5) & 0x1F) << 3;
    *b = ((c >> 10) & 0x1F) << 3;
}

static u16 blend_pixel(u16 back, int r, int g, int b, psx_blend mode)
{
    int br, bg, bb;

    unpack555(back, &br, &bg, &bb);

    switch (mode) {
    case PSX_BLEND_HALF:
        r = (br + r) / 2; g = (bg + g) / 2; b = (bb + b) / 2;
        break;
    case PSX_BLEND_ADD:
        r = br + r; g = bg + g; b = bb + b;
        break;
    case PSX_BLEND_SUB:
        r = br - r; g = bg - g; b = bb - b;
        break;
    case PSX_BLEND_QUARTER:
        r = br + r / 4; g = bg + g / 4; b = bb + b / 4;
        break;
    }

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return psx_rgb555((u8)r, (u8)g, (u8)b);
}

/* ------------------------------------------------------------------------- */
/* Texture sampling                                                           */
/*                                                                            */
/* Texture pages are 256x256 texels inside the 1024x512 16-bit VRAM. In 4- and */
/* 8-bit modes a texel is an index into a CLUT that also lives in VRAM. UVs    */
/* are u8 and therefore wrap within the page — a UV of 260 is 4, not clamped.  */
/* ------------------------------------------------------------------------- */
static bool sample_texture(const psx_vram *vram, u16 tpage, u16 clut,
                           u8 u, u8 v, int *r, int *g, int *b)
{
    int page_x = (tpage & 0x0F) * 64;      /* in 16-bit words */
    int page_y = ((tpage >> 4) & 1) * 256;
    int bpp    = (tpage >> 7) & 3;
    int clut_x = (clut & 0x3F) * 16;
    int clut_y = (clut >> 6) & 0x1FF;
    u16 texel;

    if (!vram)
        return false;

    if (bpp == 0) {              /* 4 bits per texel: 4 texels per word */
        int vx = page_x + (u >> 2);
        int vy = page_y + v;
        u16 word;
        int index;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;

        word  = vram->px[vy][vx];
        index = (word >> ((u & 3) * 4)) & 0x0F;

        if (clut_y >= PSX_VRAM_HEIGHT || clut_x + index >= PSX_VRAM_WIDTH)
            return false;
        texel = vram->px[clut_y][clut_x + index];

    } else if (bpp == 1) {       /* 8 bits per texel: 2 texels per word */
        int vx = page_x + (u >> 1);
        int vy = page_y + v;
        u16 word;
        int index;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;

        word  = vram->px[vy][vx];
        index = (word >> ((u & 1) * 8)) & 0xFF;

        if (clut_y >= PSX_VRAM_HEIGHT || clut_x + index >= PSX_VRAM_WIDTH)
            return false;
        texel = vram->px[clut_y][clut_x + index];

    } else {                     /* 16 bits per texel, direct colour */
        int vx = page_x + u;
        int vy = page_y + v;

        if (vx < 0 || vx >= PSX_VRAM_WIDTH || vy < 0 || vy >= PSX_VRAM_HEIGHT)
            return false;
        texel = vram->px[vy][vx];
    }

    /* An all-zero texel is fully transparent on this hardware — it is not
     * black. Skipping the pixel entirely is the correct behaviour. */
    if (texel == 0)
        return false;

    unpack555(texel, r, g, b);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Triangle rasterisation                                                     */
/*                                                                            */
/* Edge functions in integer arithmetic, matching the integer vertex positions */
/* the GTE produced. Barycentric weights drive Gouraud colour and affine UVs.  */
/* ------------------------------------------------------------------------- */
typedef struct raster_vertex {
    int x, y;
    int r, g, b;
    int u, v;
} raster_vertex;

static int edge(const raster_vertex *a, const raster_vertex *b, int px, int py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

static void raster_triangle(psx_framebuffer *fb,
                            const raster_vertex *v0,
                            const raster_vertex *v1,
                            const raster_vertex *v2,
                            const psx_prim *prim,
                            const psx_vram *vram,
                            const psx_raster_opts *opts)
{
    int min_x, max_x, min_y, max_y;
    int area;
    int px, py;
    const raster_vertex *a = v0, *b = v1, *c = v2;

    area = edge(a, b, c->x, c->y);
    if (area == 0)
        return;                     /* degenerate */

    /* The GPU draws both windings. Rather than cull, normalise so the
     * barycentric weights stay positive. Backface rejection is the game's job,
     * via NCLIP, not the rasteriser's. */
    if (area < 0) {
        const raster_vertex *t = b;
        b = c;
        c = t;
        area = -area;
    }

    min_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    max_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    min_y = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    max_y = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width)  max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;

    for (py = min_y; py <= max_y; py++) {
        for (px = min_x; px <= max_x; px++) {
            int w0 = edge(b, c, px, py);
            int w1 = edge(c, a, px, py);
            int w2 = edge(a, b, px, py);
            int r, g, bl;
            u16 *dst;

            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            /* Gouraud: barycentric interpolation of the per-vertex colours. */
            r  = (w0 * a->r + w1 * b->r + w2 * c->r) / area;
            g  = (w0 * a->g + w1 * b->g + w2 * c->g) / area;
            bl = (w0 * a->b + w1 * b->b + w2 * c->b) / area;

            if (opts->textures && vram &&
                (prim->kind == PSX_PRIM_FT3 || prim->kind == PSX_PRIM_GT3 ||
                 prim->kind == PSX_PRIM_FT4 || prim->kind == PSX_PRIM_GT4)) {
                int tu, tv, tr, tg, tb;

                /* Affine: the UVs interpolate in screen space with no 1/w term.
                 * This is the warp, and it is intentional. */
                tu = (w0 * a->u + w1 * b->u + w2 * c->u) / area;
                tv = (w0 * a->v + w1 * b->v + w2 * c->v) / area;

                if (!sample_texture(vram, prim->tpage, prim->clut,
                                    (u8)tu, (u8)tv, &tr, &tg, &tb))
                    continue;       /* transparent texel */

                if (prim->textured_blend) {
                    /* Texture modulated by the vertex colour. 0x80 is unity, so
                     * a mid-grey vertex colour leaves the texture unchanged. */
                    r  = (tr * r)  >> 7;
                    g  = (tg * g)  >> 7;
                    bl = (tb * bl) >> 7;
                } else {
                    r = tr; g = tg; bl = tb;
                }
            }

            if (opts->dither) {
                r  = psx_dither_channel((u8)(r  < 0 ? 0 : (r  > 255 ? 255 : r)),  px, py);
                g  = psx_dither_channel((u8)(g  < 0 ? 0 : (g  > 255 ? 255 : g)),  px, py);
                bl = psx_dither_channel((u8)(bl < 0 ? 0 : (bl > 255 ? 255 : bl)), px, py);
            }

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (bl < 0) bl = 0; else if (bl > 255) bl = 255;

            dst = &fb->px[py * fb->width + px];

            if (opts->semi_transparency && prim->semi_transparent) {
                *dst = blend_pixel(*dst, r, g, bl,
                                   (psx_blend)((prim->tpage >> 5) & 3));
            } else {
                *dst = psx_rgb555((u8)r, (u8)g, (u8)bl);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
static void fill_vertex(raster_vertex *rv, const psx_prim *prim, int i, int colour_index)
{
    rv->x = prim->xy[i].x;
    rv->y = prim->xy[i].y;
    rv->r = prim->rgb[colour_index].r;
    rv->g = prim->rgb[colour_index].g;
    rv->b = prim->rgb[colour_index].b;
    rv->u = prim->uv[i].u;
    rv->v = prim->uv[i].v;
}

void psx_raster_prim(psx_framebuffer *fb,
                     const psx_prim *prim,
                     const psx_vram *vram,
                     const psx_raster_opts *opts)
{
    raster_vertex v[4];
    bool gouraud;
    int i;

    if (!fb || !fb->px || !prim || !opts)
        return;

    gouraud = (prim->kind == PSX_PRIM_G3  || prim->kind == PSX_PRIM_GT3 ||
               prim->kind == PSX_PRIM_G4  || prim->kind == PSX_PRIM_GT4);

    for (i = 0; i < 4; i++)
        fill_vertex(&v[i], prim, i, gouraud ? i : 0);

    switch (prim->kind) {
    case PSX_PRIM_F3:
    case PSX_PRIM_FT3:
    case PSX_PRIM_G3:
    case PSX_PRIM_GT3:
        raster_triangle(fb, &v[0], &v[1], &v[2], prim, vram, opts);
        break;

    case PSX_PRIM_F4:
    case PSX_PRIM_FT4:
    case PSX_PRIM_G4:
    case PSX_PRIM_GT4:
        /*
         * MapMod quad corners run around the PERIMETER, so the split is a fan:
         * (0,1,2) and (0,2,3).
         *
         * A libgpu POLY_GT4 packet uses Z order, and assuming this data matched
         * it produces a bowtie per quad — walls come out as a regular lattice of
         * diamond holes that reads like a clipping bug rather than an index-order
         * one. See the correction in scene.h.
         */
        raster_triangle(fb, &v[0], &v[1], &v[2], prim, vram, opts);
        raster_triangle(fb, &v[0], &v[2], &v[3], prim, vram, opts);
        break;

    default:
        break;      /* lines, sprites and mode-setting packets: not yet drawn */
    }
}

typedef struct walk_ctx {
    psx_framebuffer       *fb;
    const psx_vram        *vram;
    const psx_raster_opts *opts;
} walk_ctx;

static void walk_visit(const psx_prim *prim, void *user)
{
    walk_ctx *ctx = (walk_ctx *)user;
    psx_raster_prim(ctx->fb, prim, ctx->vram, ctx->opts);
}

void psx_raster_ot(psx_framebuffer *fb,
                   const psx_ot *ot,
                   const psx_vram *vram,
                   const psx_raster_opts *opts)
{
    walk_ctx ctx;

    ctx.fb   = fb;
    ctx.vram = vram;
    ctx.opts = opts;

    psx_ot_walk(ot, walk_visit, &ctx);
}

/* ------------------------------------------------------------------------- */
q2_result psx_fb_write_ppm(const psx_framebuffer *fb, const char *path)
{
    FILE *f;
    int i, n;

    if (!fb || !fb->px || !path)
        return Q2_ERR_INVALID_ARG;

    f = fopen(path, "wb");
    if (!f)
        return Q2_ERR_IO;

    fprintf(f, "P6\n%d %d\n255\n", fb->width, fb->height);

    n = fb->width * fb->height;
    for (i = 0; i < n; i++) {
        int r, g, b;
        u8 rgb[3];

        unpack555(fb->px[i], &r, &g, &b);
        rgb[0] = (u8)r;
        rgb[1] = (u8)g;
        rgb[2] = (u8)b;
        fwrite(rgb, 1, 3, f);
    }

    fclose(f);
    return Q2_OK;
}
