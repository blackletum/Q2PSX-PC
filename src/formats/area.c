#include "area.h"

#include <string.h>

q2_result q2_area_parse(q2_area_graph *out, const q2_zone_file *zone)
{
    const dat_chunk *chunk;
    u32 areas;
    u32 table_bytes;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = zone->chunk[Q2_ZONE_AREA_CONX];
    if (!chunk || chunk->size < 2)
        return Q2_ERR_BAD_FORMAT;

    areas = q2_rd_u16(chunk->data);
    if (areas == 0)
        return Q2_ERR_BAD_FORMAT;

    /* The offset table lives at +0x02, immediately after the count. */
    table_bytes = 2 + areas * 2;
    if (table_bytes > chunk->size)
        return Q2_ERR_BAD_FORMAT;

    /* Area 0 never has links, so its offset is always zero. Checking that here
     * catches the classic misparse where the table is read from +0x04 --
     * without this, a wrong base still "works" on a minority of files. */
    if (q2_rd_u16(chunk->data + 2) != 0) {
        Q2_ERROR("areaconx: area 0 offset is %u, expected 0 -- wrong table base?",
                 q2_rd_u16(chunk->data + 2));
        return Q2_ERR_BAD_FORMAT;
    }

    out->base       = chunk->data;
    out->size       = chunk->size;
    out->area_count = areas;

    return Q2_OK;
}

/* Byte offset of an area's link record, or 0 when it has none. */
static u32 area_record_offset(const q2_area_graph *g, u32 area)
{
    if (!g || area >= g->area_count)
        return 0;
    return q2_rd_u16(g->base + 2 + area * 2);
}

u32 q2_area_link_count(const q2_area_graph *g, u32 area)
{
    u32 offset;

    if (!g || area >= g->area_count)
        return 0;

    offset = area_record_offset(g, area);
    if (offset == 0 || offset >= g->size)
        return 0;

    return g->base[offset];
}

bool q2_area_get_link(const q2_area_graph *g, u32 area, u32 index,
                      q2_area_link *out)
{
    u32 offset, count, payload;

    if (!g || !out)
        return false;

    offset = area_record_offset(g, area);
    if (offset == 0 || offset >= g->size)
        return false;

    count = g->base[offset];
    if (index >= count)
        return false;

    /*
     * Two arrays, not one array of structs — see area.h. The neighbours are
     * bytes, the planes are halfwords, and the halfword array is padded to an
     * even offset WITHIN THE RECORD, which is where the corpus size identity
     * `9n + 2 - (n & 1)` comes from.
     */
    {
        u32 planes = offset + 1 + count;
        int k;

        planes += (planes - offset) & 1u;

        if (offset + 1 + index >= g->size)
            return false;
        if (planes + (index + 1) * 8u > g->size)
            return false;

        payload = planes + index * 8u;

        out->neighbour = g->base[offset + 1 + index];
        out->dist      = (s16)q2_rd_u16(g->base + payload);
        for (k = 0; k < 3; k++)
            out->normal[k] = (s16)q2_rd_u16(g->base + payload + 2 + k * 2);
    }

    return true;
}

/* ------------------------------------------------------------------------- */
q2_result q2_map_names_parse(q2_map_name_table *out, const q2_zone_file *zone)
{
    const dat_chunk *chunk;

    if (!out || !zone)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    chunk = zone->chunk[Q2_ZONE_MAP_NAMES];
    if (!chunk || chunk->size < 4)
        return Q2_ERR_BAD_FORMAT;

    /* 16 bytes per entry plus a 4-byte terminator. */
    if ((chunk->size - 4) % 16 != 0) {
        Q2_ERROR("mapnames: %u bytes is not 4 + n*16", chunk->size);
        return Q2_ERR_BAD_FORMAT;
    }

    out->data  = chunk->data;
    out->count = (chunk->size - 4) / 16;
    return Q2_OK;
}

bool q2_map_name_get(const q2_map_name_table *t, u32 index, q2_map_name *out)
{
    const u8 *rec;

    if (!t || !out || index >= t->count)
        return false;

    rec = t->data + (size_t)index * 16;

    memcpy(out->name, rec, 12);
    out->name[12] = '\0';
    out->id = q2_rd_u32(rec + 12);

    return true;
}

bool q2_map_name_find_by_id(const q2_map_name_table *t, u32 id, q2_map_name *out)
{
    u32 i;

    if (!t || !out)
        return false;

    /* Linear, because the id is not the index and the table is never large
     * enough for that to matter. */
    for (i = 0; i < t->count; i++) {
        q2_map_name entry;
        if (!q2_map_name_get(t, i, &entry))
            continue;
        if (entry.id == id) {
            *out = entry;
            return true;
        }
    }
    return false;
}
