#include "entity.h"

#include <ctype.h>
#include <string.h>

static int name_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

q2_result q2_start_pos_parse(q2_start_pos_list *out, const q2_common_file *common)
{
    const dat_chunk *chunk;

    if (!out || !common)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = common->chunk[Q2_COMMON_START_POS];
    if (!chunk)
        return Q2_ERR_NOT_FOUND;

    /* The chunk ends with a 12-byte NUL terminator, so the record area is
     * everything before it. Checking the remainder divides exactly is what makes
     * a misread stride impossible to miss. */
    if (chunk->size < 12 || (chunk->size - 12) % Q2_STARTPOS_SIZE != 0) {
        Q2_ERROR("startpos: %u bytes is not 12 + n*%d", chunk->size, Q2_STARTPOS_SIZE);
        return Q2_ERR_BAD_FORMAT;
    }

    out->data  = chunk->data;
    out->count = (chunk->size - 12) / Q2_STARTPOS_SIZE;
    return Q2_OK;
}

bool q2_start_pos_get(const q2_start_pos_list *list, u32 index, q2_start_pos *out)
{
    const u8 *rec;

    if (!list || !out || index >= list->count)
        return false;

    rec = list->data + (size_t)index * Q2_STARTPOS_SIZE;

    memcpy(out->name, rec, 12);
    out->name[12] = '\0';

    out->x     = q2_rd_s32(rec + 0x0C);
    out->y     = q2_rd_s32(rec + 0x10);
    out->z     = q2_rd_s32(rec + 0x14);
    out->angle = q2_rd_s16(rec + 0x18);
    out->zone  = q2_rd_s16(rec + 0x1A);

    return true;
}

bool q2_start_pos_find(const q2_start_pos_list *list, const char *name,
                       q2_start_pos *out)
{
    u32 i;

    if (!list || !name || !out)
        return false;

    for (i = 0; i < list->count; i++) {
        q2_start_pos sp;
        if (!q2_start_pos_get(list, i, &sp))
            continue;
        if (name_casecmp(sp.name, name) == 0) {
            *out = sp;
            return true;
        }
    }
    return false;
}

q2_result q2_lights_parse(q2_light_list *out, const q2_common_file *common)
{
    const dat_chunk *chunk;

    if (!out || !common)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = common->chunk[Q2_COMMON_LIGHTS];
    if (!chunk)
        return Q2_ERR_NOT_FOUND;

    if (chunk->size % Q2_LIGHT_SIZE != 0) {
        Q2_ERROR("lights: %u bytes is not a multiple of %d",
                 chunk->size, Q2_LIGHT_SIZE);
        return Q2_ERR_BAD_FORMAT;
    }

    out->data  = chunk->data;
    out->count = chunk->size / Q2_LIGHT_SIZE;
    return Q2_OK;
}

bool q2_light_get(const q2_light_list *list, u32 index, q2_light *out)
{
    const u8 *rec;

    if (!list || !out || index >= list->count)
        return false;

    rec = list->data + (size_t)index * Q2_LIGHT_SIZE;

    out->x = q2_rd_s32(rec + 0x00);
    out->y = q2_rd_s32(rec + 0x04);
    out->z = q2_rd_s32(rec + 0x08);

    out->r = rec[0x0C];
    out->g = rec[0x0D];
    out->b = rec[0x0E];
    /* rec[0x0F] is pad and is zero everywhere. */

    out->always_255 = rec[0x10];
    out->type       = rec[0x11];

    out->radius          = q2_rd_u16(rec + 0x12);
    out->inner_radius_sq = q2_rd_u32(rec + 0x14);
    out->radius_sq       = q2_rd_u32(rec + 0x18);

    return true;
}
