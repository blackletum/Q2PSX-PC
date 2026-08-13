#include "points.h"

#include <stdlib.h>
#include <string.h>

q2_result q2_points_parse(q2_points *out, const q2_zone_file *zone)
{
    const dat_chunk *chunk;
    const u8 *p;
    u32 group_count, header_size, running = 0;
    u32 i;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = zone->chunk[Q2_ZONE_POINTS];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    p = chunk->data;
    group_count = q2_rd_u32(p);

    if (group_count == 0)
        return Q2_ERR_BAD_FORMAT;

    /* Guard the multiply before it can wrap. */
    if (group_count > (chunk->size - 4) / 8)
        return Q2_ERR_BAD_FORMAT;

    header_size = 4 + group_count * 8;

    out->groups = (q2_point_group *)calloc(group_count, sizeof(q2_point_group));
    if (!out->groups)
        return Q2_ERR_NO_MEMORY;

    for (i = 0; i < group_count; i++) {
        u32 byte_offset = q2_rd_u32(p + 4 + i * 8);
        u32 count       = q2_rd_u32(p + 4 + i * 8 + 4);

        /* Groups are contiguous and ordered; anything else means we have
         * misread the chunk and should say so rather than guess. */
        if (byte_offset != running * Q2_POINT_SIZE) {
            Q2_ERROR("points: group %u starts at 0x%X, expected 0x%X",
                     i, byte_offset, running * Q2_POINT_SIZE);
            free(out->groups);
            memset(out, 0, sizeof(*out));
            return Q2_ERR_BAD_FORMAT;
        }

        out->groups[i].first = running;
        out->groups[i].count = count;
        running += count;
    }

    if ((u64)running * Q2_POINT_SIZE != (u64)(chunk->size - header_size)) {
        Q2_ERROR("points: %u points need %llu bytes but the chunk has %u",
                 running, (unsigned long long)running * Q2_POINT_SIZE,
                 chunk->size - header_size);
        free(out->groups);
        memset(out, 0, sizeof(*out));
        return Q2_ERR_BAD_FORMAT;
    }

    out->data        = p + header_size;
    out->count       = running;
    out->group_count = group_count;

    return Q2_OK;
}

void q2_points_free(q2_points *p)
{
    if (!p)
        return;
    free(p->groups);
    memset(p, 0, sizeof(*p));
}

bool q2_points_get(const q2_points *p, u32 index, q2_point *out)
{
    const u8 *rec;

    if (!p || !out || index >= p->count)
        return false;

    rec = p->data + (size_t)index * Q2_POINT_SIZE;

    out->x = q2_rd_s16(rec + 0);
    out->y = q2_rd_s16(rec + 2);
    out->z = q2_rd_s16(rec + 4);
    memcpy(out->slot, rec + 6, 6);

    return true;
}

void q2_points_local_bounds(const q2_points *p, s16 min_out[3], s16 max_out[3])
{
    u32 i;
    s32 mn[3] = { INT16_MAX, INT16_MAX, INT16_MAX };
    s32 mx[3] = { INT16_MIN, INT16_MIN, INT16_MIN };

    if (!p || p->count == 0) {
        if (min_out) min_out[0] = min_out[1] = min_out[2] = 0;
        if (max_out) max_out[0] = max_out[1] = max_out[2] = 0;
        return;
    }

    for (i = 0; i < p->count; i++) {
        const u8 *rec = p->data + (size_t)i * Q2_POINT_SIZE;
        int c;

        for (c = 0; c < 3; c++) {
            s32 v = q2_rd_s16(rec + c * 2);
            if (v < mn[c]) mn[c] = v;
            if (v > mx[c]) mx[c] = v;
        }
    }

    for (i = 0; i < 3; i++) {
        if (min_out) min_out[i] = (s16)mn[i];
        if (max_out) max_out[i] = (s16)mx[i];
    }
}
