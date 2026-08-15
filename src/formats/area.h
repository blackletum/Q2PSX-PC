/*
 * area.h — the AreaConx portal graph and the MapNames table.
 *
 * ---------------------------------------------------------------------------
 * AreaConx — how areas of a zone connect
 * ---------------------------------------------------------------------------
 *     u16  num_areas
 *     u16  area_offset[num_areas]     <-- at +0x02, NOT +0x04
 *     ...  link records
 *
 * THE OFFSET TABLE STARTS AT +0x02. This is worth stating loudly because
 * getting it wrong is silent: an earlier analysis pass assumed a u16 pad at
 * +0x02 and put the table at +0x04, which yields out-of-bounds area offsets on
 * 100 of 115 files while still "parsing" the other 15. What looked like padding
 * is area 0's offset, which is always 0 because area 0 never has links.
 *
 * With the correct base, every offset is in bounds on all 115 files, the first
 * non-zero offset equals 2 + 2*num_areas, and 1,725 records recover cleanly.
 *
 * A link record is NOT an array of 9-byte structs. It is a count, a byte array
 * of NEIGHBOURS, and then a halfword array of PLANES:
 *
 *     u8   num_links
 *     u8   neighbour[num_links]        the adjacent area
 *     u8   pad                         only when (1 + num_links) is odd
 *     struct { s16 dist; s16 n[3]; }   plane[num_links]
 *
 * and that layout is exactly the record size the corpus shows:
 * `1 + n + pad + 8n` is `9n + 2 - (n & 1)`, which holds on all 1,675 interior
 * records. `num_links` is observed 1..7 and 9 — never 8.
 *
 * ---------------------------------------------------------------------------
 * Why this took three passes, and it was never the data
 * ---------------------------------------------------------------------------
 * The payload was read as "n interleaved 9-byte records" and every attempt to
 * find a normal inside one failed — no byte offset gave a 1.3.12 unit vector in
 * more than 39% of links, and byte histograms showed 0x10/0xF0 clustering that
 * looked like s16 values sliced at the wrong parity. All of that is true and
 * all of it followed from the premise. The arrays are SEPARATE, so a "byte +3"
 * belongs to a different link's field depending on n.
 *
 * The premise fell to a test the interleaved reading could not pass: an
 * adjacency graph must be SYMMETRIC. Reading the first n bytes as neighbours
 * gives **3,494 of 3,494 edges with their reverse present — 100%** — against
 * 25.6% for the best interleaved candidate. With that pinning the array
 * boundary, the plane array falls out: **3,494 of 3,494 normals are unit length
 * in 1.3.12**, 3,150 of them axis-aligned with a component of exactly 4096, and
 * not one is zero or anything else.
 *
 * `dist` is non-negative, a multiple of 256, and runs 0..9984.
 *
 * ---------------------------------------------------------------------------
 * MapNames — a name/id table
 * ---------------------------------------------------------------------------
 *     { char name[12]; u32 id; }[n]    then a u32 0 terminator
 *
 * with n = (size - 4) / 16, exact on all 115 files; 671 entries in total.
 *
 * `id` is a real identifier, not a redundant index: it equals the entry's
 * position in 428 entries but differs in 243, reaching 274. Code must look up
 * by id rather than assuming the array index is the id.
 */
#ifndef Q2PSX_AREA_H
#define Q2PSX_AREA_H

#include "level.h"
#include "q2psx.h"

/* Bytes per link across the two arrays: one neighbour and four halfwords. */
#define Q2_AREA_LINK_SIZE 9

typedef struct q2_area_link {
    u8  neighbour;        /* the area on the other side                      */
    s16 dist;             /* multiples of 256, 0..9984                       */
    s16 normal[3];        /* 1.3.12, unit length on 3494/3494                */
} q2_area_link;

typedef struct q2_area_graph {
    const u8 *base;       /* start of the chunk, offsets are relative to it */
    u32       size;
    u32       area_count;
} q2_area_graph;

q2_result q2_area_parse(q2_area_graph *out, const q2_zone_file *zone);

/* Number of links leaving `area`. Returns 0 for an area with no record. */
u32 q2_area_link_count(const q2_area_graph *g, u32 area);

/* Borrow link `index` of `area`. Returns false if out of range. */
bool q2_area_get_link(const q2_area_graph *g, u32 area, u32 index,
                      q2_area_link *out);

/* ------------------------------------------------------------------------- */
typedef struct q2_map_name {
    char name[13];
    u32  id;
} q2_map_name;

typedef struct q2_map_name_table {
    const u8 *data;
    u32       count;
} q2_map_name_table;

q2_result q2_map_names_parse(q2_map_name_table *out, const q2_zone_file *zone);
bool      q2_map_name_get(const q2_map_name_table *t, u32 index, q2_map_name *out);

/* Look up by id, which is NOT the same as the array index. */
bool q2_map_name_find_by_id(const q2_map_name_table *t, u32 id, q2_map_name *out);

#endif /* Q2PSX_AREA_H */
