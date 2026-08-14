/*
 * gpu.h — the PlayStation GPU primitive model and ordering table.
 *
 * The game does not call a modern graphics API. It builds a linked list of GPU
 * primitives — exactly the structures libgpu used — and hands it to a backend.
 * Keeping that indirection is what makes faithful rendering possible: the
 * backend sees the same primitive stream the real hardware saw, so it can
 * reproduce the hardware's rasterisation rules rather than approximating them.
 *
 * The four properties that define the PlayStation look, and where they live:
 *
 *   1. Integer vertex positions      — produced by gte.c, stored here as s16.
 *   2. Affine (non-perspective) UVs  — u8 texture coordinates with no W term;
 *                                      the backend must NOT perspective-correct.
 *   3. No depth buffer               — depth is the ordering table bucket index;
 *                                      primitives draw strictly back-to-front.
 *   4. 15-bit colour with dithering  — the backend's final resolve step.
 */
#ifndef Q2PSX_GPU_H
#define Q2PSX_GPU_H

#include "fixed.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Primitive kinds                                                            */
/* ------------------------------------------------------------------------- */
typedef enum psx_prim_kind {
    PSX_PRIM_F3 = 0,   /* flat triangle                     */
    PSX_PRIM_FT3,      /* flat textured triangle            */
    PSX_PRIM_G3,       /* gouraud triangle                  */
    PSX_PRIM_GT3,      /* gouraud textured triangle         */
    PSX_PRIM_F4,       /* flat quad                         */
    PSX_PRIM_FT4,      /* flat textured quad                */
    PSX_PRIM_G4,       /* gouraud quad                      */
    PSX_PRIM_GT4,      /* gouraud textured quad             */
    PSX_PRIM_LINE_F2,
    PSX_PRIM_LINE_G2,
    PSX_PRIM_SPRT,     /* screen-space sprite               */
    PSX_PRIM_TILE,     /* untextured rectangle              */
    PSX_PRIM_TPAGE,    /* draw-mode change                  */
    PSX_PRIM_KIND_COUNT
} psx_prim_kind;

/* ------------------------------------------------------------------------- */
/* Semi-transparency modes. The PSX has exactly four; B is the framebuffer     */
/* value and F the incoming fragment.                                         */
/* ------------------------------------------------------------------------- */
typedef enum psx_blend {
    PSX_BLEND_HALF   = 0,   /* B/2 + F/2 — the usual "glass" look   */
    PSX_BLEND_ADD    = 1,   /* B   + F   — muzzle flashes, fire     */
    PSX_BLEND_SUB    = 2,   /* B   - F   — shadow, smoke            */
    PSX_BLEND_QUARTER= 3    /* B   + F/4 — faint additive           */
} psx_blend;

/* Texture colour depth within a texture page. */
typedef enum psx_tex_bpp {
    PSX_TEX_4BIT  = 0,
    PSX_TEX_8BIT  = 1,
    PSX_TEX_16BIT = 2
} psx_tex_bpp;

/* ------------------------------------------------------------------------- */
/* Common primitive fields                                                    */
/* ------------------------------------------------------------------------- */
typedef struct psx_xy   { s16 x, y; } psx_xy;    /* integer screen pixels     */
typedef struct psx_uv   { u8  u, v; } psx_uv;    /* texel within a 256x256 page */
typedef struct psx_rgb  { u8  r, g, b, pad; } psx_rgb;

/*
 * One primitive. This is a tagged union rather than the original's variable-size
 * packets: the storage cost is irrelevant on a PC and it makes the backend's job
 * a straight switch. Field meanings match the hardware exactly.
 *
 * `tpage` packs the texture page as the hardware does:
 *   bits 0-3  : page X base (page * 64 halfwords)
 *   bit  4    : page Y base (0 or 256)
 *   bits 5-6  : semi-transparency mode
 *   bits 7-8  : colour depth
 * `clut` packs the palette location:
 *   bits 0-5  : X / 16
 *   bits 6-14 : Y
 */
typedef struct psx_prim {
    psx_prim_kind kind;

    psx_xy   xy[4];        /* 3 for triangles, 4 for quads, 2 for lines  */
    psx_uv   uv[4];
    psx_rgb  rgb[4];       /* [0] only for flat primitives               */

    u16      tpage;
    u16      clut;

    bool     semi_transparent;  /* the primitive's ABE bit                */
    bool     textured_blend;    /* modulate texture by rgb (libgpu "raw") */

    u16      otz;          /* ordering-table bucket this landed in       */
} psx_prim;

/* ------------------------------------------------------------------------- */
/* Ordering table                                                             */
/*                                                                            */
/* The PlayStation has no depth buffer. Instead, each primitive is appended to */
/* a bucket indexed by its average Z, and the buckets are walked from far to   */
/* near. Two consequences the port must preserve:                             */
/*                                                                            */
/*   - Per-polygon sorting only. Intersecting polygons pop, and long polygons  */
/*     sort by their average depth, so they can incorrectly occlude. This is   */
/*     visible in the original and must remain visible.                       */
/*   - Within one bucket, order is defined by insertion (the hardware walks a  */
/*     singly-linked list built by prepending, so *last in draws first*).      */
/* ------------------------------------------------------------------------- */
typedef struct psx_ot {
    psx_prim *prims;       /* flat pool of all primitives this frame     */
    u32       prim_count;
    u32       prim_capacity;

    s32      *bucket_head; /* index of first prim in bucket, -1 if empty */
    s32      *next;        /* intrusive singly-linked list, parallel to prims */
    u32       bucket_count;

    /*
     * The slice of the table the current viewport owns.
     *
     * This is not a convenience: it is the hardware model. The console's world
     * renderer is handed a base pointer (the per-viewport slice address parked
     * in 0x800B2D60) and links primitives relative to it, which is how one
     * table and one DrawOTag produce four independently clipped viewports. A
     * window of zero length means the whole table.
     */
    u32       window_base;
    u32       window_len;
} psx_ot;

/* Constrain subsequent psx_ot_add calls to [base, base+len). len == 0 releases
 * the window. psx_ot_clear releases it too, so a window never outlives a frame
 * by accident. */
void psx_ot_set_window(psx_ot *ot, u32 base, u32 len);

/* How many buckets an emitter may address right now — the window if one is
 * installed, the whole table otherwise. */
u32  psx_ot_bucket_span(const psx_ot *ot);

q2_result psx_ot_init(psx_ot *ot, u32 bucket_count, u32 prim_capacity);
void      psx_ot_free(psx_ot *ot);
void      psx_ot_clear(psx_ot *ot);

/* Add a primitive to the bucket for `otz`. Returns NULL if the pool is full —
 * the original printed "Out of ScreenChanges on frame %d" and dropped geometry,
 * and we surface the same condition rather than growing silently. */
psx_prim *psx_ot_add(psx_ot *ot, u16 otz);

/* Iterate the table in draw order: far buckets first, and within a bucket the
 * most recently added primitive first. `fn` is called once per primitive. */
typedef void (*psx_ot_visit_fn)(const psx_prim *prim, void *user);
void psx_ot_walk(const psx_ot *ot, psx_ot_visit_fn fn, void *user);

/* ------------------------------------------------------------------------- */
/* Draw-mode helpers                                                          */
/* ------------------------------------------------------------------------- */
Q2PSX_INLINE u16 psx_make_tpage(int page_x, int page_y, psx_blend blend, psx_tex_bpp bpp)
{
    return (u16)(((page_x & 0x0F))
               | ((page_y & 0x01) << 4)
               | (((int)blend & 0x03) << 5)
               | (((int)bpp   & 0x03) << 7));
}

Q2PSX_INLINE u16 psx_make_clut(int x, int y)
{
    return (u16)(((x >> 4) & 0x3F) | ((y & 0x1FF) << 6));
}

/* ------------------------------------------------------------------------- */
/* VRAM                                                                       */
/*                                                                            */
/* 1024x512 16-bit words, exactly as on hardware. Textures, palettes and both  */
/* framebuffers all live in here and can overlap — the original relied on that */
/* layout, so the port keeps it rather than using separate texture objects.    */
/* ------------------------------------------------------------------------- */
#define PSX_VRAM_WIDTH   1024
#define PSX_VRAM_HEIGHT  512

typedef struct psx_vram {
    u16 px[PSX_VRAM_HEIGHT][PSX_VRAM_WIDTH];
} psx_vram;

/* RGB555 with the mask bit in bit 15. */
Q2PSX_INLINE u16 psx_rgb555(u8 r, u8 g, u8 b)
{
    return (u16)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
}

/*
 * The GPU's ordered dither matrix, applied when 24-bit colour is reduced to the
 * 15-bit framebuffer. Values are added to each channel before truncation. This
 * 4x4 pattern is a large part of why PlayStation gradients look the way they do,
 * so the backend applies it by default.
 */
extern const s8 psx_dither_matrix[4][4];

Q2PSX_INLINE u8 psx_dither_channel(u8 value, int x, int y)
{
    s32 v = (s32)value + psx_dither_matrix[y & 3][x & 3];
    return (u8)q2_clamp_s32(v, 0, 255);
}

#endif /* Q2PSX_GPU_H */
