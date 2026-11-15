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
            attr.dx[i]=(u32)(nx*4096/den)<<12;
            attr.dy[i]=(u32)(ny*4096/den)<<12;
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
