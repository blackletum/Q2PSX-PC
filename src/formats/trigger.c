#include "trigger.h"

#include <string.h>

q2_result q2_triggers_parse(q2_triggers *out, const q2_common_file *common)
{
    const dat_chunk *chunk;
    u32 count, planes;
    size_t need;

    if (!out || !common)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = common->chunk[Q2_COMMON_TRIG_BOUNDS];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    count  = q2_rd_u16(chunk->data);
    planes = q2_rd_u16(chunk->data + 2);

    /* The size identity is exact on every map, so checking it turns a
     * plausible read into a verified one for this specific file. */
    need = 4 + (size_t)(count + 1) * Q2_TRIGGER_SIZE
             + (size_t)planes * Q2_TRIGGER_PLANE_SIZE;

    if (need != chunk->size) {
        Q2_ERROR("trigbounds: %u triggers + %u planes need %zu bytes, chunk has %u",
                 count, planes, need, chunk->size);
        return Q2_ERR_BAD_FORMAT;
    }

    out->data        = chunk->data + 4;
    out->count       = count;
    out->plane_count = planes;
    out->planes      = chunk->data + 4 + (size_t)(count + 1) * Q2_TRIGGER_SIZE;

    return Q2_OK;
}

bool q2_trigger_get(const q2_triggers *t, u32 index, q2_trigger *out)
{
    const u8 *rec;
    int i;

    /* index == count is the sentinel, which callers legitimately read to get
     * the last real trigger's plane range. */
    if (!t || !out || index > t->count)
        return false;

    rec = t->data + (size_t)index * Q2_TRIGGER_SIZE;

    for (i = 0; i < 3; i++) {
        out->min[i] = q2_rd_s32(rec + 0x00 + i * 4);
        out->max[i] = q2_rd_s32(rec + 0x0C + i * 4);
    }

    out->plane_start  = q2_rd_u16(rec + 0x18);
    out->event_offset = q2_rd_u16(rec + 0x1A);
    out->id           = q2_rd_u16(rec + 0x20);
    out->flags        = q2_rd_u16(rec + 0x22);

    return true;
}

bool q2_trigger_get_plane(const q2_triggers *t, u32 index, q2_trigger_plane *out)
{
    const u8 *rec;
    int i;

    if (!t || !out || index >= t->plane_count)
        return false;

    rec = t->planes + (size_t)index * Q2_TRIGGER_PLANE_SIZE;

    for (i = 0; i < 3; i++) {
        out->point[i]  = q2_rd_s16(rec + i * 2);
        out->normal[i] = q2_rd_s16(rec + 6 + i * 2);
    }
    return true;
}

u32 q2_trigger_plane_count(const q2_triggers *t, u32 index)
{
    q2_trigger here, next;

    if (!t || index >= t->count)
        return 0;
    if (!q2_trigger_get(t, index, &here) || !q2_trigger_get(t, index + 1, &next))
        return 0;

    /* Derived from the successor, never as 6*i — see the header. */
    if (next.plane_start < here.plane_start)
        return 0;

    return (u32)(next.plane_start - here.plane_start);
}

bool q2_trigger_point_in_box(const q2_trigger *trig, const s32 point[3])
{
    int i;

    if (!trig || !point)
        return false;

    for (i = 0; i < 3; i++) {
        if (point[i] < trig->min[i] || point[i] > trig->max[i])
            return false;
    }
    return true;
}

bool q2_trigger_contains(const q2_triggers *t, u32 index, const s32 point[3])
{
    q2_trigger trig;
    u32 first, n, k;

    if (!t || !point)
        return false;
    if (!q2_trigger_get(t, index, &trig))
        return false;

    /* Box first: it rejects the overwhelming majority for three comparisons. */
    if (!q2_trigger_point_in_box(&trig, point))
        return false;

    n = q2_trigger_plane_count(t, index);
    if (n == 0)
        return true;            /* a real case: box-only trigger */

    first = trig.plane_start;

    for (k = 0; k < n; k++) {
        q2_trigger_plane pl;
        s64 dot;
        s32 world[3];
        int i;

        if (!q2_trigger_get_plane(t, first + k, &pl))
            continue;

        /* Plane points are relative to this trigger's min corner — a different
         * base from collision planes, which use their node's bbox_min. */
        for (i = 0; i < 3; i++)
            world[i] = trig.min[i] + pl.point[i];

        dot = (s64)pl.normal[0] * (point[0] - world[0])
            + (s64)pl.normal[1] * (point[1] - world[1])
            + (s64)pl.normal[2] * (point[2] - world[2]);

        if (dot > 0)
            return false;       /* outside this face */
    }

    return true;
}
