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
 * A link record is a count byte followed by that many 9-byte payloads:
 *
 *     u8   num_links
 *     u8   payload[num_links][9]
 *
 * and the record size satisfies 9*n + 2 - (n & 1) exactly on all 1,675 interior
 * records. The final record in a chunk cannot be checked that way because the
 * chunk is padded to 4 bytes. `num_links` is observed 1..7 and 9 — never 8.
 *
 * The 9-byte payload is UNDECODED. No fixed byte offset yields a 1.3.12 unit
 * normal in more than 39% of 3,494 links. Byte histograms show 0x10/0xF0
 * clustering consistent with unaligned s16 values of 0x1000/0xF000, but links
 * begin at record + 1 + 9*L so their alignment alternates and no single struct
 * can express it. Byte +3 is zero in 2,132 of 3,494 links and is the best
 * neighbour-index candidate. The payload is therefore exposed as raw bytes
 * rather than a struct that would encode a guess.
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

#define Q2_AREA_LINK_SIZE 9

typedef struct q2_area_link {
    const u8 *payload;    /* 9 undecoded bytes */
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
