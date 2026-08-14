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
static psx_prim *ot_link(psx_ot *ot, u32 bucket, u16 otz)
{
    u32 idx;
    psx_prim *prim;

    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];

    memset(prim, 0, sizeof(*prim));
    prim->otz = otz;

    /* Prepend, matching the hardware's list construction. */
    ot->next[idx]           = ot->bucket_head[bucket];
    ot->bucket_head[bucket] = (s32)idx;

    return prim;
}

u32 q2_ot_bucket_for_depth(const psx_ot *ot, u32 depth, s32 far_z)
{
    u32 span = psx_ot_bucket_span(ot);
    u32 b;

    if (span == 0)
        span = 1;

    if (far_z > 0) {
        b = (u32)(((u64)depth * (span - 1)) / (u32)far_z);
    } else {
        b = depth >> 2;
    }

    if (b >= span)
        b = span - 1;
    return b;
}

psx_prim *psx_ot_add(psx_ot *ot, u16 otz)
{
    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    return ot_link(ot, psx_ot_depth_bucket(ot, otz), otz);
}

psx_prim *psx_ot_add_bucket(psx_ot *ot, u32 bucket)
{
    if (!ot || ot->prim_count >= ot->prim_capacity || ot->bucket_count == 0)
        return NULL;

    return ot_link(ot, bucket, (u16)bucket);
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
