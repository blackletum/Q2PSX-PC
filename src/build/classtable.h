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
 *     0x20  u8        class_byte  the index into classMethods[256]
 *     0x24  u32       fn_b        likewise
 *     0x28  s16       gib_health  negative; below it the body is destroyed
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
 * version of the same game published.
 *
 * ---------------------------------------------------------------------------
 * And +0x28 is GIB HEALTH, which this used to call `offset`
 * ---------------------------------------------------------------------------
 * The old name came from the shape of the column — negative, and scaling with
 * the creature's size — which is exactly what a model's vertical offset would
 * look like too. The values settle it: they are id's own `self->gib_health`,
 * creature for creature.
 *
 *     Soldier −30   Infantry −40   Parasite −50   Berserk −60   Gunner −70
 *     Hover −100    Medic −130     Gladiator −175 Boss2 −200    Boss1 −500
 *     Jorg −2000    Rider −2000    Flipper −30
 *
 * Thirteen of the table's own numbers landing on a different version of the
 * same game's, from a field nothing in the decode was tuned against. Two do
 * not — Ironmaiden reads −30 where id's chick is −70, and Flyer reads 0 where
 * id's flyer is −50 — and those are stated rather than smoothed over: this
 * disc's numbers are this disc's.
 *
 * `creworld.c` was already USING it as gib health, so the code was right and
 * only the name was wrong. The three Soldiers sharing one value while differing
 * in health is still what makes it a skin family; it is just a shared gib
 * threshold rather than a shared model offset.
 *
 * ---------------------------------------------------------------------------
 * The class byte at +0x20, which had no home at all
 * ---------------------------------------------------------------------------
 * A third numbering, distinct from `class_id` and from the module name: it is
 * the index into the engine's 256-entry `classMethods` table, and the module
 * loader copies it straight into the entity at 0x8007E660 (`lbu v0, 0x20(s4)` /
 * `sb v0, 0x23(s0)`). Reading the column out gives Arachner 64, Berserk 70,
 * Gunner 79, Infantry 81, Soldier 87/88/89, Tankcomm 91, Insane 94 — which is
 * exactly the set the disc's seven CreAI modules register for themselves, so
 * the column is checkable against code rather than merely plausible.
 *
 * It is what `M_ReactToDamage` compares when it refuses to take offence at the
 * four big monsters (ai.c), and the only way to name those four.
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
    s16  gib_health;
    u8   class_byte;     /* +0x20: the index into classMethods[256] */
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
