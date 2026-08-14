/*
 * flare.c — the lens flares, transcribed from 0x80075708 and 0x800752F8.
 *
 * Four routines meet here and each is named for its address:
 *
 *   0x800759F0   the per-viewport pass: every dynamic light, then every static
 *                light of the node the camera is in
 *   0x80075708   one light: cull, attenuate, pick a colour and a size
 *   0x800752F8   one flare: walk its element list, place each along the
 *                centre-to-light line and draw it
 *   0x80074E6C   the n-gon the discs are made of
 *   0x80074FF4   the eight lines the starburst is made of
 *
 * Everything is untextured and additive. There is no flare bitmap on the disc.
 */
#include "flare.h"

#include "trig.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The style table — 0x800A1FDC, 0x800A2014, 0x800A2024, 0x800A203C           */
/*                                                                            */
/* Read out of the executable's data segment; `q2psx-inspect lights` reads the */
/* same bytes back off the disc and compares them element by element.         */
/* ------------------------------------------------------------------------- */
static const q2_flare_element k_style1[] = {   /* 0x800A1FDC — six elements */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 },
    { Q2_FLARE_KIND_DISC,  0x044C, -0x0400, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0320,  0x0600, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0200,  0x1800, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0300, -0x1400, 0x0400 }
};

static const q2_flare_element k_style2[] = {   /* 0x800A2014 — the core alone */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 }
};

static const q2_flare_element k_style3[] = {   /* 0x800A2024 */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 }
};

static const q2_flare_element k_style4[] = {   /* 0x800A203C — style 1 less one */
    { Q2_FLARE_KIND_BURST, 0x1000,  0x1000, 0x1000 },
    { Q2_FLARE_KIND_DISC,  0x0800, -0x0ED8, 0x0600 },
    { Q2_FLARE_KIND_DISC,  0x044C, -0x0400, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0320,  0x0600, 0x0400 },
    { Q2_FLARE_KIND_DISC,  0x0200,  0x1800, 0x0400 }
};

static const q2_flare_style k_styles[Q2_FLARE_STYLE_COUNT] = {
    { NULL,     0 },
    { k_style1, (u32)(sizeof k_style1 / sizeof k_style1[0]) },
    { k_style2, (u32)(sizeof k_style2 / sizeof k_style2[0]) },
    { k_style3, (u32)(sizeof k_style3 / sizeof k_style3[0]) },
    { k_style4, (u32)(sizeof k_style4 / sizeof k_style4[0]) }
};

u32 q2_flare_style_of(u8 type) { return (u32)((type >> 3) & 7u); }
u32 q2_flare_size_of(u8 type)
{
    return (u32)Q2_FLARE_BASE_SIZE << ((type >> 6) & 3u);
}

const q2_flare_style *q2_flare_style_table(u32 style)
{
    static const q2_flare_style empty = { NULL, 0 };
    return style < Q2_FLARE_STYLE_COUNT ? &k_styles[style] : &empty;
}

/* ------------------------------------------------------------------------- */
/* The compiler-folded divides                                                */
/*                                                                            */
/* The ring generator's two are exact — 320*4096 in x and 240*4096 in y, so a  */
/* flare is a fraction of the VIEWPORT rather than a number of pixels. The     */
/* starburst's four are not: they are magic multiplies whose closed form was   */
/* not recovered, so they are reproduced as the arithmetic the original        */
/* actually performs. Their effect is measurable even if their source is not:  */
/* they make the axis spikes 1.637x the disc radius and the diagonals 1.170x,  */
/* consistently in both axes.                                                  */
/* ------------------------------------------------------------------------- */
#define Q2_FLARE_DIV_X ((s32)Q2_FLARE_REF_W * Q2_LIGHT_ONE)
#define Q2_FLARE_DIV_Y ((s32)Q2_FLARE_REF_H * Q2_LIGHT_ONE)

static s32 magic_div(s32 n, u32 m, int shift)
{
    s64 prod = (s64)n * (s64)(s32)m;
    s32 hi    = (s32)(prod >> 32);

    /* The `mfhi; addu` form the compiler emits when the magic has bit 31 set. */
    if (m & 0x80000000u)
        hi += n;

    return (hi >> shift) - (n >> 31);
}

/* ------------------------------------------------------------------------- */
/* 0x80074E6C — a regular n-gon around a centre, plus the centre itself        */
/*                                                                            */
/* out[0] is the centre; out[1..n] the rim; out[n+1] and out[n+2] repeat       */
/* out[1] and out[2] so the caller's fan can read three points past the last   */
/* without wrapping. The x offsets mirror and the y offsets do not, which is   */
/* what makes it symmetric about the vertical axis.                           */
/* ------------------------------------------------------------------------- */
#define Q2_FLARE_RING_MAX 16

/*
 * The two multiplies are 32-bit and are allowed to wrap, because the original's
 * are: `mult` / `mflo` twice, with no widening. A flare large enough to
 * overflow would tear the same way there.
 */
static s32 ring_offset_x(s32 angle, s32 radius, const q2_flare_view *view)
{
    s32 v = (s32)(q2_sin12(angle) * radius);
    v = v * view->extent[0];
    return v / Q2_FLARE_DIV_X;
}

static s32 ring_offset_y(s32 angle, s32 radius, const q2_flare_view *view)
{
    s32 v = (s32)(q2_cos12(angle) * radius);
    v = v * view->extent[1];
    return v / Q2_FLARE_DIV_Y;
}

static void flare_ring(psx_xy *out, int n, const q2_flare_view *view,
                       s32 radius)
{
    const int step = 4096 / n;
    int i;

    out[0].x = view->centre[0];
    out[0].y = view->centre[1];

    for (i = 0; i <= n / 2; i++) {
        s32 angle = (s32)i * step;
        s32 dx = ring_offset_x(angle, radius, view);
        s32 dy = ring_offset_y(angle, radius, view);

        out[i + 1].x = (s16)(view->centre[0] + dx);
        out[i + 1].y = (s16)(view->centre[1] + dy);

        out[n - i].x = (s16)(view->centre[0] - dx);
        out[n - i].y = (s16)(view->centre[1] + dy);
    }

    out[n + 1] = out[1];
    out[n + 2] = out[2];
}

/* ------------------------------------------------------------------------- */
/* 0x80074FF4 — the eight spikes                                              */
/*                                                                            */
/* Four iterations, each emitting a pair of Gouraud lines in opposite          */
/* directions from the centre: coloured at the centre, black at the tip. The   */
/* even iterations (0 and 90 degrees) use one pair of scale constants and the  */
/* odd ones (45 and 135) another, which is what gives the star its long axis   */
/* arms and short diagonals.                                                  */
/* ------------------------------------------------------------------------- */
static u32 flare_burst(psx_xy centre, s32 radius, const u8 rgb[3],
                       const q2_flare_view *view, psx_ot *ot,
                       q2_flare_stats *stats)
{
    const int n = 8;
    const int step = 4096 / n;
    u32 emitted = 0;
    int i;

    for (i = 0; i < n / 2; i++) {
        s32 angle = (s32)i * step;
        s32 vx = (s32)(q2_sin12(angle) * radius) * view->extent[0];
        s32 vy = (s32)(q2_cos12(angle) * radius) * view->extent[1];
        s32 dx, dy;
        int side;

        if (i & 1) {
            dx = magic_div(vx, 0x1DF5959Fu, 17);
            dy = magic_div(vy, 0x09FC8735u, 15);
        } else {
            dx = magic_div(vx, 0x14F8B589u, 16);
            dy = magic_div(vy, 0x6FD91D85u, 18);
        }

        for (side = 0; side < 2; side++) {
            psx_prim *p = psx_ot_add_bucket(ot, view->bucket);
            s32 sign = side ? 1 : -1;

            if (!p) {
                if (stats) stats->ot_overflow++;
                return emitted;
            }

            p->kind  = PSX_PRIM_LINE_G2;
            p->xy[0] = centre;
            p->xy[1].x = (s16)(centre.x + sign * dx);
            p->xy[1].y = (s16)(centre.y + sign * dy);

            p->rgb[0].r = rgb[0];
            p->rgb[0].g = rgb[1];
            p->rgb[0].b = rgb[2];
            p->rgb[1].r = p->rgb[1].g = p->rgb[1].b = 0;

            p->semi_transparent = true;
            p->textured_blend   = false;
            emitted++;
        }
    }

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* Colour helpers                                                             */
/* ------------------------------------------------------------------------- */
static u8 clamp_u8(s32 v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (u8)v;
}

/* `mult` then `sra 16`: the element's 1.12 colour factor applied to a 1.12
 * component, landing back in 0..255. */
static u8 element_colour(s16 component, s16 factor)
{
    return clamp_u8(((s32)component * factor) >> 16);
}

/* ------------------------------------------------------------------------- */
/* 0x800752F8 — one flare's element list                                      */
/* ------------------------------------------------------------------------- */
static u32 flare_emit(const q2_flare_style *style, psx_xy at, s32 scale,
                      const s16 colour[3], const q2_flare_view *view,
                      psx_ot *ot, q2_flare_stats *stats)
{
    u32 emitted = 0;
    u32 e;

    for (e = 0; e < style->count; e++) {
        const q2_flare_element *el = &style->element[e];
        psx_xy ring[Q2_FLARE_RING_MAX + 3];
        psx_xy centre;
        s32 radius;
        u8  rgb[3];
        int i, n;

        /*
         * Place the element along the line from the screen centre to the
         * light. This — not a corona around the light — is what makes the
         * effect a lens flare: pos 4096 sits on the light, 0 on the centre and
         * a negative value on the far side of it.
         */
        centre.x = (s16)(view->centre[0]
                 + (((s32)(at.x - view->centre[0]) * el->pos) >> Q2_FRAC_12));
        centre.y = (s16)(view->centre[1]
                 + (((s32)(at.y - view->centre[1]) * el->pos) >> Q2_FRAC_12));

        radius = (scale * el->size) >> Q2_FRAC_12;

        for (i = 0; i < 3; i++)
            rgb[i] = element_colour(colour[i], el->colour);

        n = (el->kind == Q2_FLARE_KIND_BURST) ? 12 : 6;

        {
            q2_flare_view local = *view;
            local.centre[0] = centre.x;
            local.centre[1] = centre.y;
            flare_ring(ring, n, &local, radius);
        }

        /*
         * The fan. Each primitive spans two sectors: corners 0 and 1 are the
         * near pair and 2 and 3 the far, which is libgpu's Z order, so a quad
         * of {rim, centre, rim, rim} draws as two triangles both touching the
         * centre.
         */
        for (i = 0; i < n; i += 2) {
            psx_prim *p = psx_ot_add_bucket(ot, view->bucket);
            int c;

            if (!p) {
                if (stats) stats->ot_overflow++;
                return emitted;
            }

            p->xy[0] = ring[i + 1];
            p->xy[1] = ring[0];
            p->xy[2] = ring[i + 2];
            p->xy[3] = ring[i + 3];

            if (el->kind == Q2_FLARE_KIND_BURST) {
                /* POLY_G4, code 0x3A: bright at the centre corner and black at
                 * the other three. The gradient IS the glow. */
                p->kind = PSX_PRIM_G4;
                for (c = 0; c < 4; c++)
                    p->rgb[c].r = p->rgb[c].g = p->rgb[c].b = 0;
                p->rgb[1].r = rgb[0];
                p->rgb[1].g = rgb[1];
                p->rgb[1].b = rgb[2];
            } else {
                /* POLY_F4, code 0x28 with ABE set at 0x80075640. */
                p->kind = PSX_PRIM_F4;
                p->rgb[0].r = rgb[0];
                p->rgb[0].g = rgb[1];
                p->rgb[0].b = rgb[2];
            }

            p->semi_transparent = true;
            p->textured_blend   = false;
            emitted++;
        }

        if (el->kind == Q2_FLARE_KIND_BURST)
            emitted += flare_burst(centre, radius, rgb, view, ot, stats);
    }

    if (stats)
        stats->prims_emitted += emitted;

    return emitted;
}

/* ------------------------------------------------------------------------- */
/* 0x80075708 — one light                                                     */
/* ------------------------------------------------------------------------- */
u32 q2_flare_draw(const q2_light *l, const q2_camera *cam,
                  const q2_flare_view *view, psx_ot *ot, gte_state *gte,
                  q2_flare_stats *stats)
{
    const q2_flare_style *style;
    s16 d[3];
    s32 size, atten;
    s16 colour[3];
    s32 scale;
    psx_xy at;
    s32 origin[3];

    if (!l || !cam || !view || !ot || !gte)
        return 0;

    if (stats)
        stats->lights_considered++;

    style = q2_flare_style_table(q2_flare_style_of(l->type));
    if (style->count == 0)
        return 0;

    if (stats)
        stats->lights_styled++;

    size = (s32)q2_flare_size_of(l->type);

    origin[0] = cam->pos[0];
    origin[1] = cam->pos[1];
    origin[2] = cam->pos[2];
    q2_light_delta(l, origin, d);

    /*
     * MVMVA against the view rotation with NO translation vector — the delta
     * is already relative to the camera. The near reject is on the resulting
     * camera-space Z, before any of the attenuation work.
     */
    gte->v[0].x = d[0];
    gte->v[0].y = d[1];
    gte->v[0].z = d[2];
    gte_mvmva(gte, 1, 0, 0, 3, 0);

    if (gte->mac[2] < Q2_FLARE_MIN_Z) {
        if (stats) stats->rejected_near++;
        return 0;
    }

    atten = q2_light_attenuation(l, d, size);
    if (atten == 0) {
        if (stats) stats->rejected_dark++;
        return 0;
    }

    /*
     * Project. The GTE still holds the delta in V0, so RTPS reuses it — and
     * because it goes through the same rotation, projection and integer
     * truncation the world does, a flare sits on exactly the pixel its light
     * would.
     */
    gte_rtps(gte, false);
    at.x = gte->sxy[2].x;
    at.y = gte->sxy[2].y;

    /*
     * The two sides of 4096. Inside the inner radius the colour is the light's
     * own and the flare GROWS; outside it the size pins to its floor and the
     * colour DIMS. (0x80075904 branches on exactly this.)
     */
    if (atten >= Q2_LIGHT_ONE) {
        colour[0] = (s16)((s32)l->r << 4);
        colour[1] = (s16)((s32)l->g << 4);
        colour[2] = (s16)((s32)l->b << 4);
        scale = (atten < 0 ? atten + 127 : atten) >> Q2_FLARE_SCALE_SHIFT;
    } else {
        const u8 rgb[3] = { l->r, l->g, l->b };
        int i;
        for (i = 0; i < 3; i++) {
            s32 v = (s32)rgb[i] * atten;
            colour[i] = (s16)((v < 0 ? v + 255 : v) >> 8);
        }
        scale = Q2_FLARE_MIN_SCALE;
    }

    if (stats)
        stats->flares_drawn++;

    return flare_emit(style, at, scale, colour, view, ot, stats);
}

/* ------------------------------------------------------------------------- */
/* 0x800759F0 — the per-viewport pass                                         */
/* ------------------------------------------------------------------------- */
u32 q2_flare_draw_all(const q2_light_world *w, const q2_camera *cam,
                      s32 coll_node, const q2_flare_view *view,
                      psx_ot *ot, gte_state *gte, q2_flare_stats *stats)
{
    u32 emitted = 0;
    u32 i;

    if (!w || !cam || !view || !ot || !gte)
        return 0;

    for (i = 0; i < w->dynamic_world_count && i < Q2_DYNLIGHT_MAX; i++)
        emitted += q2_flare_draw(&w->dynamic_world[i], cam, view, ot, gte, stats);

    if (coll_node < 0 || !w->space || !w->statics)
        return emitted;

    {
        u32 first, count;

        if (!q2_spacelights_range(w->space, (u32)coll_node, &first, &count))
            return emitted;

        for (i = 0; i < count; i++) {
            u16 index;
            q2_light l;

            if (!q2_spacelights_entry(w->space, first + i, &index))
                break;
            if (!q2_light_get(w->statics, index, &l))
                continue;
            emitted += q2_flare_draw(&l, cam, view, ot, gte, stats);
        }
    }

    return emitted;
}
