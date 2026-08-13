/*
 * classtable.h — the entity class table: what a spawn's class id actually is.
 *
 * `Population` gives every actor a `class_id` in 0…37, and for two passes that
 * number resolved to nothing. It is not an index into `ModelNames`, and the
 * executable's Population globals have no reader at all — which is what sent
 * the search into the relocatable modules.
 *
 * The answer was one step further back. A CreAI module's 16-byte preamble
 * begins with a 12-byte NAME, and the loader at 0x8007D990 looks that name up
 * through 0x80057A18 in a 48-byte-stride table at 0x800A3368. Each record in
 * that table carries the class id, the name, and the class's own constants. So
 * the mapping is name-keyed, and `class_id` is simply the value stored beside
 * the name.
 *
 * ---------------------------------------------------------------------------
 * Record layout, 48 bytes
 * ---------------------------------------------------------------------------
 *     0x00  u32       class_id
 *     0x04  char[12]  name        matches a CastList model name
 *     0x14  u32       fn_a        non-zero only on the player classes
 *     0x24  u32       fn_b        likewise
 *     0x28  s16       offset      negative, scales with the creature's size
 *     0x2A  s16       health
 *     0x2C  u32       zero
 *
 * ---------------------------------------------------------------------------
 * Why the health field is not a guess
 * ---------------------------------------------------------------------------
 * The values are PC Quake II's own, creature for creature: three Soldier
 * records at 30, 20 and 40 — the shotgun, light and machinegun guards — plus
 * Infantry 100, Flyer 50, Gladiator 400, Jorg 3000. Nothing about the decode
 * was tuned to produce that; it is the field landing on values a different
 * version of the same game published. The three Soldiers also share one
 * `offset` of −30 while differing in health, which is what a skin family looks
 * like and what a mis-parse does not.
 *
 * The class ids are 1…37 for creatures and 39…44 for the deathmatch player
 * skins, so the range Population uses is covered exactly.
 */
#ifndef Q2PSX_CLASSTABLE_H
#define Q2PSX_CLASSTABLE_H

#include "disc.h"
#include "ident.h"
#include "q2psx.h"

#define Q2_CLASS_RECORD_SIZE 48
#define Q2_CLASS_NAME_LEN    12

/* PAL build. Like the level table, this is per-build data and refusing an
 * uncatalogued executable is the point. */
#define Q2_CLASSTABLE_ADDR_SLES01534  0x800A3368u
#define Q2_CLASSTABLE_END_SLES01534   0x800A3A40u

typedef struct q2_class_entry {
    u32  id;
    char name[Q2_CLASS_NAME_LEN + 1];
    u32  fn_a;
    u32  fn_b;
    s16  offset;
    s16  health;
    bool is_player;      /* the deathmatch skins carry both function pointers */
} q2_class_entry;

typedef struct q2_class_table {
    q2_class_entry *entries;
    u32             count;
} q2_class_table;

q2_result q2_class_table_load(q2_class_table *out, const disc *d,
                              const q2_build_id *id);
void      q2_class_table_free(q2_class_table *t);

/*
 * The class with this id, or NULL.
 *
 * Ids are NOT unique: the four player skins share ids 41…44 across two model
 * families, and looking one up by id alone returns the first. That is the
 * engine's own behaviour — its lookup is by name, and the id is what it writes
 * down afterwards.
 */
const q2_class_entry *q2_class_find(const q2_class_table *t, u32 id);

/* The class with this name, which is the direction the engine looks. */
const q2_class_entry *q2_class_find_name(const q2_class_table *t,
                                         const char *name);

#endif /* Q2PSX_CLASSTABLE_H */
