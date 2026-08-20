#include "gpu.h"

#include <stdlib.h>
#include <string.h>

/*
 * The GPU's 4x4 ordered dither. Applied to each colour channel before the
 * truncation to 5 bits per channel that the framebuffer forces.
 */
const s8 psx_dither_matrix[4][4] = {
    { -4,  0, -3,  1 },
    {  2, -2,  3, -1 },
    { -3,  1, -4,  0 },
    {  3, -1,  2, -2 }
};

q2_result psx_ot_init(psx_ot *ot, u32 bucket_count, u32 prim_capacity)
{
    if (!ot || bucket_count == 0 || prim_capacity == 0)
        return Q2_ERR_INVALID_ARG;

    /* The caller counts in the console's buckets; the table holds the
     * subdivided ones. See PSX_OT_SUBDIV. */
    bucket_count *= PSX_OT_SUBDIV;

    memset(ot, 0, sizeof(*ot));

    ot->prims       = (psx_prim *)calloc(prim_capacity, sizeof(psx_prim));
    ot->next        = (s32 *)malloc((size_t)prim_capacity * sizeof(s32));
    ot->bucket_head = (s32 *)malloc((size_t)bucket_count * sizeof(s32));

    if (!ot->prims || !ot->next || !ot->bucket_head) {
        psx_ot_free(ot);
        return Q2_ERR_NO_MEMORY;
    }

    ot->prim_capacity = prim_capacity;
    ot->bucket_count  = bucket_count;
    psx_ot_clear(ot);
    return Q2_OK;
}

void psx_ot_free(psx_ot *ot)
{
    if (!ot)
        return;
    free(ot->prims);
    free(ot->next);
    free(ot->bucket_head);
    memset(ot, 0, sizeof(*ot));
}

void psx_ot_clear(psx_ot *ot)
{
    u32 i;

    if (!ot)
        return;

    ot->prim_count  = 0;
    ot->window_base = 0;
    ot->window_len  = 0;
    for (i = 0; i < ot->bucket_count; i++)
        ot->bucket_head[i] = -1;
}

void psx_ot_set_window(psx_ot *ot, u32 base, u32 len)
{
    if (!ot)
        return;

    /* Console buckets in, real buckets out — the whole subdivided extent of the
     * named range, so a window still covers exactly what it named. */
    base *= PSX_OT_SUBDIV;
    len  *= PSX_OT_SUBDIV;

    if (base >= ot->bucket_count) {
        ot->window_base = 0;
        ot->window_len  = 0;
        return;
    }
    if (len > ot->bucket_count - base)
        len = ot->bucket_count - base;

    ot->window_base = base;
    ot->window_len  = len;
}

u32 psx_ot_bucket_span(const psx_ot *ot)
{
    if (!ot)
        return 0;
    return ot->window_len ? ot->window_len : ot->bucket_count;
}

u32 psx_ot_depth_bucket(const psx_ot *ot, u32 otz)
{
    u32 base, span;

    if (!ot || ot->bucket_count == 0)
        return 0;

    if (ot->window_len) {
        base = ot->window_base;
        span = ot->window_len;
    } else {
        base = 0;
        span = ot->bucket_count;
    }

    if (otz >= span)
        otz = span - 1;

    /* Depth counts down from the far end: the table is drawn bucket 0 first, so
     * the farthest primitive belongs in the lowest bucket of the span. */
    return base + (span - 1u - otz);
}

/* The shared tail of both add paths, once the bucket is decided. */
static psx_prim *ot_link(psx_ot *ot, u32 bucket, u16 otz, u32 key)
{
    u32 idx;
    psx_prim *prim;

    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];

    memset(prim, 0, sizeof(*prim));
    prim->otz      = otz;
    prim->sort_key = key;

    if (key == PSX_OT_KEY_NONE) {
        /* Prepend, matching the hardware's list construction. */
        ot->next[idx]           = ot->bucket_head[bucket];
        ot->bucket_head[bucket] = (s32)idx;
        return prim;
    }

    /*
     * Keyed: hold the bucket's list in descending key order from the head, so
     * the walk still draws the farthest thing in the bucket first. Equal keys
     * put the newcomer at the FRONT of its run of equals, which is the prepend
     * rule the hardware has and what every unkeyed caller keeps. A
     * PSX_OT_KEY_NONE entry stops the search — see the header.
     */
    {
        s32 prev = -1;
        s32 cur  = ot->bucket_head[bucket];

        while (cur >= 0 && ot->prims[cur].sort_key != PSX_OT_KEY_NONE &&
               ot->prims[cur].sort_key > key) {
            prev = cur;
            cur  = ot->next[cur];
        }

        ot->next[idx] = cur;
        if (prev < 0)
            ot->bucket_head[bucket] = (s32)idx;
        else
            ot->next[prev] = (s32)idx;
    }

    return prim;
}

u32 q2_ot_bucket_for_depth(const psx_ot *ot, u32 depth, s32 far_z)
{
    u32 span = psx_ot_bucket_span(ot);
    u32 lo, hi, b;

    if (span == 0)
        span = 1;

    /*
     * The depth-addressable part of the slice. A slice too small to hold the
     * reserve on both sides and still have somewhere to put geometry is a test
     * table rather than a viewport, and gets the whole thing.
     */
    if (span > PSX_OT_DEPTH_RESERVE * 2u + 1u) {
        lo = PSX_OT_DEPTH_RESERVE;
        hi = span - 1u - PSX_OT_DEPTH_RESERVE;
    } else {
        lo = 0;
        hi = span - 1u;
    }

    if (far_z > 0) {
        b = lo + (u32)(((u64)depth * (hi - lo)) / (u32)far_z);
    } else {
        b = lo + (depth >> 2);
    }

    if (b > hi)
        b = hi;
    return b;
}

psx_prim *psx_ot_add(psx_ot *ot, u16 otz)
{
    return psx_ot_add_depth(ot, otz, PSX_OT_KEY_NONE);
}

psx_prim *psx_ot_add_depth(psx_ot *ot, u16 otz, u32 key)
{
    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    return ot_link(ot, psx_ot_depth_bucket(ot, otz), otz, key);
}

/*
 * ALLOCATE WITHOUT LINKING, and why the model path needs it.
 *
 * A model's faces are BUILT in file order — the scratch window holds one part's
 * transformed vertices at a time, so a face can only be resolved while its own
 * part is current — but they are DRAWN in an order the model carries with it
 * (model.h, block A). The console keeps the two apart by parking packet
 * pointers in a flat array as it builds (0x800DDDCC) and chaining them into the
 * ordering table afterwards, walking that array in the stored order.
 *
 * This is that split. `psx_ot_alloc` takes a primitive out of the pool and
 * leaves it unlinked; `psx_ot_link_prim` puts it in a bucket later. A primitive
 * that is allocated and never linked simply never draws, which is what a culled
 * face wants.
 */
psx_prim *psx_ot_alloc(psx_ot *ot)
{
    u32 idx;
    psx_prim *prim;

    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];
    memset(prim, 0, sizeof(*prim));
    ot->next[idx] = -1;
    return prim;
}

bool psx_ot_link_prim(psx_ot *ot, psx_prim *prim, u32 bucket, u32 key)
{
    u32 idx;

    if (!ot || !prim || ot->bucket_count == 0)
        return false;
    if (prim < ot->prims || prim >= ot->prims + ot->prim_count)
        return false;

    idx = (u32)(prim - ot->prims);

    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    prim->otz      = (u16)bucket;
    prim->sort_key = key;

    if (key == PSX_OT_KEY_NONE) {
        ot->next[idx]           = ot->bucket_head[bucket];
        ot->bucket_head[bucket] = (s32)idx;
        return true;
    }

    {
        s32 prev = -1;
        s32 cur  = ot->bucket_head[bucket];

        while (cur >= 0 && ot->prims[cur].sort_key != PSX_OT_KEY_NONE &&
               ot->prims[cur].sort_key > key) {
            prev = cur;
            cur  = ot->next[cur];
        }

        ot->next[idx] = cur;
        if (prev < 0)
            ot->bucket_head[bucket] = (s32)idx;
        else
            ot->next[prev] = (s32)idx;
    }
    return true;
}

psx_prim *psx_ot_add_bucket(psx_ot *ot, u32 bucket)
{
    if (!ot || ot->prim_count >= ot->prim_capacity || ot->bucket_count == 0)
        return NULL;

    /*
     * REAL buckets, not console ones. Some callers name a console constant and
     * some hand back a bucket `psx_ot_depth_bucket` has already resolved, and
     * there is no way to tell the two apart here — so the scaling belongs where
     * a console constant is turned into a bucket (q2_screen_view_otz,
     * q2_screen_overlay_otz, and the flash and water buckets in screen.c)
     * rather than at this door. Scaling here instead multiplied the
     * already-resolved ones a second time, which sent the briefing's panel out
     * of its window and drew the world through it.
     */
    return ot_link(ot, bucket, (u16)bucket, PSX_OT_KEY_NONE);
}

void psx_ot_walk(const psx_ot *ot, psx_ot_visit_fn fn, void *user)
{
    u32 b;

    if (!ot || !fn)
        return;

    /* DrawOTag order: bucket 0 first, higher buckets on top of it. Depth is
     * inverted on the way in (psx_ot_depth_bucket), so this is still far to
     * near for anything that added itself by depth. */
    for (b = 0; b < ot->bucket_count; b++) {
        s32 idx = ot->bucket_head[b];
        while (idx >= 0) {
            fn(&ot->prims[idx], user);
            idx = ot->next[idx];
        }
    }
}
