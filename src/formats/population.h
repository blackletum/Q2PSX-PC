/*
 * population.h — Population: where the actors and pickups go.
 *
 * The last piece of level data a game needs. Geometry says what the world looks
 * like, collision says where you can walk, triggers say what fires — this says
 * what is standing in the room when you arrive.
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 *     group[]                     24 bytes each, terminated by u32 0
 *     ... then the per-group lists, located by chunk-relative offset
 *
 * A group is:
 *     0x00  char[12]  name
 *     0x0C  u32       spawn_offset   chunk-relative; 0 means the group has none
 *     0x10  u32       place_offset   chunk-relative; 0 means none
 *     0x14  u32       zero
 *
 * 76 distinct group names exist game-wide: zone names, script batch names, item
 * categories, and one special path group. An empty Population chunk is 8 bytes.
 *
 * ---------------------------------------------------------------------------
 * Three list layouts, and three DIFFERENT terminators
 * ---------------------------------------------------------------------------
 * This is the part that punishes a careless reader. The lists do not share a
 * terminator convention:
 *
 *     spawn list  (most groups)   24 bytes/record, terminated by u32 0
 *     path list   (path group)    24 bytes/record, terminated by 0xFFFFFFFF
 *     place list  (all groups)    16 bytes/record, terminated by 0xFFFFFFFF
 *
 * Reading a place list expecting a zero terminator runs off the end; reading a
 * path list expecting one stops at the first node whose coordinates happen to
 * begin with a zero word.
 *
 * The spawn list also changes SHAPE for the path group — same 24-byte stride,
 * entirely different fields. Which layout applies is selected by the group's
 * name, not by anything in the record. That is why q2_pop_group_is_path exists.
 *
 * Spawn record (non-path groups), 24 bytes:
 *     0x00  u32  class_id   25 distinct values 0..37. NOT a ModelNames index:
 *                           15 of 673 exceed the map's name count and name
 *                           resolution yields nonsense. Meaning UNKNOWN.
 *     0x04  s32  x
 *     0x08  s32  y
 *     0x0C  s32  z
 *     0x10  u16  angle      0..3958; 0/1015/2041/3068 dominate (INFERRED)
 *     0x12  u16  link       0xFFFF means none, in 482 of 673
 *     0x14  u16  flags      47 distinct values, UNKNOWN
 *     0x16  u16  index      slot 0..323, not strictly monotonic, UNKNOWN
 *
 * Path record (path group only), 24 bytes:
 *     0x00  s32  x, y, z    xyz is at +0x00 here, NOT +0x04 as in a spawn
 *     0x0C  u16  unk0
 *     0x0E  u16  zero
 *     0x10  u32  link0      neighbour index; 0xFFFFFFFF means none (INFERRED)
 *     0x14  u32  link1
 *
 * Place record, 16 bytes:
 *     0x00  s32  x, y, z
 *     0x0C  u16  unk        NOT a plain angle — values include 4096, 32768,
 *                           36864, 53248, so it reads as flags OR'd with one
 *     0x0E  u16  id         0..56, 41 distinct. Same numeric range as the EXE's
 *                           class-table id byte, so most likely the link to the
 *                           global pickup table (INFERRED)
 *
 * Region between a list's terminator and the next declared offset is slack, not
 * data: two maps leave a 4-byte gap after the group table, as do 18 spawn lists
 * and 34 place lists.
 */
#ifndef Q2PSX_POPULATION_H
#define Q2PSX_POPULATION_H

#include "level.h"
#include "q2psx.h"

#define Q2_POP_GROUP_SIZE  24
#define Q2_POP_SPAWN_SIZE  24
#define Q2_POP_PATH_SIZE   24
#define Q2_POP_PLACE_SIZE  16

#define Q2_POP_LINK_NONE   0xFFFFu
#define Q2_POP_TERM_FFFF   0xFFFFFFFFu

typedef struct q2_pop_group {
    char name[13];
    u32  spawn_offset;   /* 0 when absent */
    u32  place_offset;
} q2_pop_group;

typedef struct q2_pop_spawn {
    u32 class_id;
    s32 x, y, z;
    u16 angle;
    u16 link;
    u16 flags;
    u16 index;
} q2_pop_spawn;

typedef struct q2_pop_path {
    s32 x, y, z;
    u16 unk0;
    u32 link0, link1;
} q2_pop_path;

typedef struct q2_pop_place {
    s32 x, y, z;
    u16 unk;
    u16 id;
} q2_pop_place;

typedef struct q2_population {
    const u8 *data;
    u32       size;
    u32       group_count;
} q2_population;

q2_result q2_population_parse(q2_population *out, const q2_common_file *common);

bool q2_pop_get_group(const q2_population *p, u32 index, q2_pop_group *out);

/* True when this group's spawn list uses the path-node layout. Selected by
 * name, because nothing in the records distinguishes them. */
bool q2_pop_group_is_path(const q2_pop_group *g);

/*
 * Walk a group's lists. `slot` counts from 0; each returns false at the list's
 * terminator, at the end of the chunk, or when the group has no such list.
 *
 * Each honours its own terminator — they genuinely differ, see the header.
 */
bool q2_pop_get_spawn(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_spawn *out);
bool q2_pop_get_path(const q2_population *p, const q2_pop_group *g,
                     u32 slot, q2_pop_path *out);
bool q2_pop_get_place(const q2_population *p, const q2_pop_group *g,
                      u32 slot, q2_pop_place *out);

#endif /* Q2PSX_POPULATION_H */
