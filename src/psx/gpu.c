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

    ot->prim_count = 0;
    for (i = 0; i < ot->bucket_count; i++)
        ot->bucket_head[i] = -1;
}

psx_prim *psx_ot_add(psx_ot *ot, u16 otz)
{
    u32 bucket;
    u32 idx;
    psx_prim *prim;

    if (!ot || ot->prim_count >= ot->prim_capacity)
        return NULL;

    bucket = otz;
    if (bucket >= ot->bucket_count)
        bucket = ot->bucket_count - 1;

    idx  = ot->prim_count++;
    prim = &ot->prims[idx];

    memset(prim, 0, sizeof(*prim));
    prim->otz = otz;

    /* Prepend, matching the hardware's list construction. */
    ot->next[idx]          = ot->bucket_head[bucket];
    ot->bucket_head[bucket] = (s32)idx;

    return prim;
}

void psx_ot_walk(const psx_ot *ot, psx_ot_visit_fn fn, void *user)
{
    u32 b;

    if (!ot || !fn)
        return;

    /* Far to near: the highest bucket index is the most distant. */
    for (b = ot->bucket_count; b-- > 0; ) {
        s32 idx = ot->bucket_head[b];
        while (idx >= 0) {
            fn(&ot->prims[idx], user);
            idx = ot->next[idx];
        }
    }
}
