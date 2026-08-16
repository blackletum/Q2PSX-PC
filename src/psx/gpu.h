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
    PSX_PRIM_MOVE,     /* VRAM -> VRAM rectangle copy       */
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

    /*
     * Which corner order this quad's four slots are in.
     *
     * false — PERIMETER, the default and what MapMod stores: the four corners
     *         walk round the quad, so it fans as (0,1,2)+(0,2,3).
     * true  — libgpu Z ORDER, which is what the hardware's own POLY_x4 packets
     *         use: 0 1 / 2 3 across two rows, fanning as (0,1,2)+(1,3,2).
     *
     * The flare pass builds real console packets and therefore hands over Z
     * order; splitting those on the perimeter rule drew half of every sector as
     * a black sliver, turning the 12-gon glow into a six-spoke pinwheel and the
     * ghosts into bowties. Rather than reorder the packets — which would stop
     * them matching 0x800754D8..0x80075554 field for field — the backend is
     * told which convention it is looking at.
     */
    bool     quad_zorder;

    u16      otz;          /* ordering-table bucket this landed in       */
} psx_prim;

/* ------------------------------------------------------------------------- */
/* PSX_PRIM_MOVE — the one primitive that reads the framebuffer               */
/*                                                                            */
/* libgpu's `DR_MOVE`: GP0(0x80), copy a rectangle of VRAM to somewhere else  */
/* in VRAM. The game builds these by hand rather than through `SetDrawMove` — */
/* the pool at `0x80062D3C` pre-fills each packet with tag `0x05000000`,      */
/* `code[0] = 0x01000000` (flush the texture cache) and `code[1] = 0x80000000`*/
/* (the copy), leaving only the two coordinate pairs and the size to be       */
/* written per use. That pool is called **"Water Moves"** in the executable's  */
/* own descriptor string at `0x800ACF7C`, which is what names the effect.     */
/*                                                                            */
/* The fields it uses, matching `code[2]`, `code[3]` and `code[4]`:           */
/*                                                                            */
/*     xy[0]   source, in VRAM coordinates                                    */
/*     xy[1]   destination                                                    */
/*     xy[2]   width and height, as x and y                                   */
/*                                                                            */
/* TWO RULES THAT ARE NOT THE ONES A POLYGON FOLLOWS, and both matter:        */
/*                                                                            */
/*   - The copy is NOT clipped by the drawing area and NOT displaced by the   */
/*     drawing offset. Those are rasteriser state; this is a blit, and its    */
/*     coordinates are absolute. The water warp relies on it: it computes its */
/*     own `view.x + buffer origin` and would land twice as far off if the    */
/*     draw env's offset were applied on top.                                 */
/*   - Overlapping source and destination are allowed and are the normal      */
/*     case, so the copy must behave as though the source were read whole     */
/*     before the destination is written.                                     */
/* ------------------------------------------------------------------------- */
#define PSX_MOVE_SRC  0
#define PSX_MOVE_DST  1
#define PSX_MOVE_SIZE 2

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
/*                                                                            */
/* WHICH WAY THE TABLE IS WALKED — read out of the executable, not assumed.    */
/* The game builds its table with `ClearOTag` (`0x800837C0`, called at         */
/* `0x80018398`), whose loop writes into each entry the address of the *next*  */
/* one, and hands `DrawOTag` the address of entry 0. So the hardware walks     */
/* bucket 0 first and every later bucket paints on top of it. Three separate   */
/* things in the frame depend on that direction and would be nonsense reversed:*/
/* the full-screen background clear sits at OT[1] and must precede all four    */
/* viewports; each viewport's draw-env packet sits at slice bucket 1 and must  */
/* precede that viewport's geometry; and the damage flash sits at slice bucket */
/* 50 and must land on top of it.                                             */
/*                                                                            */
/* A HIGHER BUCKET IS THEREFORE NEARER. To keep that from leaking into every   */
/* emitter, `otz` here is a DEPTH — larger is farther, exactly as it arrives   */
/* from the GTE — and the table inverts it. `psx_ot_add_bucket` is the escape  */
/* hatch for the few packets that know which bucket they want.                 */
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

/*
 * Map a GTE depth into the slice the current window owns, scaled by the
 * viewport's far distance.
 *
 * The port used to shift the depth right by a fixed amount, which was fine
 * against a table of thousands of buckets and is not fine against the console's
 * real one: a viewport slice is 51 entries, so a fixed shift saturates
 * everything past a few hundred units onto the slice's far end and the sort
 * stops distinguishing anything — including a weapon held inches from the eye
 * from the wall behind it.
 *
 * `far_z` is the viewport's own far distance (view+264, parked at 0x800B2CCC),
 * so the scale is the console's number rather than a tuning constant. A far_z
 * of zero falls back to the old shift, which is what the offline tools that
 * build their own camera still get.
 */
u32  q2_ot_bucket_for_depth(const psx_ot *ot, u32 depth, s32 far_z);

q2_result psx_ot_init(psx_ot *ot, u32 bucket_count, u32 prim_capacity);
void      psx_ot_free(psx_ot *ot);
void      psx_ot_clear(psx_ot *ot);

/*
 * Add a primitive at depth `otz` — larger is farther, so it lands in a LOWER
 * bucket and is drawn earlier. The depth is clamped to the addressable span and
 * mapped into the window if one is installed. Returns NULL if the pool is full;
 * the original printed "Out of ScreenChanges on frame %d" and dropped geometry,
 * and we surface the same condition rather than growing silently.
 */
psx_prim *psx_ot_add(psx_ot *ot, u16 otz);

/*
 * Add a primitive at an ABSOLUTE bucket index, ignoring the window and the
 * depth inversion. This is what a packet whose place in the table is structural
 * rather than depth-derived uses — the draw envs, the damage flash and the
 * performance meter all name their bucket outright.
 */
psx_prim *psx_ot_add_bucket(psx_ot *ot, u32 bucket);

/* Map a depth to the absolute bucket `psx_ot_add` would choose. */
u32 psx_ot_depth_bucket(const psx_ot *ot, u32 otz);

/* Iterate the table in DrawOTag order: bucket 0 first, so far geometry is drawn
 * before near geometry, and within a bucket the most recently added primitive
 * first. `fn` is called once per primitive. */
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
