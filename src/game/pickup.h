/*
 * pickup.h — items lying in the world, and collecting them.
 *
 * Population's place records give the position and an id; this turns those into
 * live items the player can walk over.
 *
 * ---------------------------------------------------------------------------
 * What is known, and what is guessed
 * ---------------------------------------------------------------------------
 * CONFIRMED: place records are 16 bytes — s32 x, y, z then a u16 of flags and a
 * u16 id — and there are 1,013 of them across the disc, with 41 distinct ids in
 * the range 0..56.
 *
 * NOT CONFIRMED: what an id means. The range matches the EXE's class-table id
 * byte, so it is very likely an index into the global pickup table, but that
 * table has not been decoded. Until it is, this module cannot say that id 17 is
 * a rocket launcher.
 *
 * So the design keeps the two apart. `q2_pickup` carries the raw id and a
 * `kind` that is resolved through a table the caller supplies. Ship a null
 * table and items still spawn, get collected and disappear — they simply have
 * no effect. That is a usable, honest intermediate state, and it means the
 * decoded table drops in later without touching this code.
 *
 * ---------------------------------------------------------------------------
 * Collection radius
 * ---------------------------------------------------------------------------
 * The original's touch distance has not been read out of the executable. The
 * value here is derived from the player's own dimensions in worldscale.h rather
 * than picked arbitrarily, and it is marked INFERRED so nobody mistakes it for
 * a recovered constant.
 */
#ifndef Q2PSX_PICKUP_H
#define Q2PSX_PICKUP_H

#include "inventory.h"
#include "population.h"
#include "q2psx.h"
#include "worldscale.h"

/* INFERRED: roughly the player's own half-width. */
#define Q2_PICKUP_RADIUS (Q2_SWEEP_HALF_EXTENT)

/* What collecting an item does. Resolved from its id via a caller-supplied
 * table, because the id-to-effect mapping is not decoded yet. */
typedef enum q2_pickup_kind {
    Q2_PICKUP_NONE = 0,
    Q2_PICKUP_HEALTH,
    Q2_PICKUP_ARMOUR,
    Q2_PICKUP_AMMO,
    Q2_PICKUP_WEAPON,
    Q2_PICKUP_KEY
} q2_pickup_kind;

typedef struct q2_pickup_def {
    u16            id;        /* the value stored in the place record */
    q2_pickup_kind kind;
    s16            amount;    /* health points, ammo rounds, armour   */
    s16            subtype;   /* q2_ammo, q2_weapon, or a key bit     */
    bool           overheal;  /* health may exceed the normal cap     */
} q2_pickup_def;

typedef struct q2_pickup {
    s32  pos[3];
    u16  id;
    u16  flags;      /* the record's u16; not a plain angle, meaning unknown */
    bool taken;
} q2_pickup;

typedef struct q2_pickup_set {
    q2_pickup *items;
    u32        count;
    u32        capacity;

    /* Optional. NULL means items spawn and can be collected but do nothing. */
    const q2_pickup_def *defs;
    u32                  def_count;
} q2_pickup_set;

/* Gather every place record in the map into a live item set. */
q2_result q2_pickups_build(q2_pickup_set *out, const q2_population *pop);
void      q2_pickups_free(q2_pickup_set *set);

/* Attach the id-to-effect table once it is known. Safe to leave unset. */
void q2_pickups_set_defs(q2_pickup_set *set, const q2_pickup_def *defs, u32 count);

/*
 * Collect anything within reach of `pos`, applying its effect to `inv`.
 *
 * Returns how many were taken. An item whose effect cannot apply — full health,
 * full ammo, a weapon already held — is NOT taken, which is the behaviour that
 * lets a player come back for it later.
 */
u32 q2_pickups_collect(q2_pickup_set *set, const s32 pos[3], q2_inventory *inv);

/* Look up a definition by id. NULL when absent or no table is attached. */
const q2_pickup_def *q2_pickup_find_def(const q2_pickup_set *set, u16 id);

#endif /* Q2PSX_PICKUP_H */
