/*
 * itemtable.h — what a `Population` place record's id actually means.
 *
 * `Population`'s place list gives every item in a map a position, a halfword of
 * flags-and-angle, and an id in 0…56. For three passes that id resolved to
 * nothing: FORMATS.md §9.5 could describe the 24-byte table it indexes but not
 * what any column meant, so the port carried a pickup module built to take a
 * caller-supplied id table that nobody could supply. That module is gone; this
 * is the table it was waiting for.
 *
 * The table is at `0x8009F5CC` and the reader is the item spawner at
 * `0x800599DC`, which is reached from the relocated level modules through the
 * import table (slot 11). It does exactly this:
 *
 *     id  = place->id                       // lh at place + 0x0E
 *     rec = scan the table for rec->place_id == id, stride 24, 0xFF terminates
 *     if (!rec) return NULL                 // nothing spawns
 *     ent = spawn(place, rec->model, -1, (rec->flags & 0x100) ? 0 : 0x400)
 *     ent->think  = 0x80059330              // the item think, see item.h
 *     ent->flags  = rec->flags
 *     ent->effect = rec->effect             // the touch dispatch index
 *     ent->scale  = (rec->flags & 2) ? 0 : 4096
 *     ent->model->clips = &rec->extra[0]
 *
 * So the columns are: which place id selects this record, which MODEL it wears,
 * how it BEHAVES (flags), and what COLLECTING it does (effect). The last is an
 * index into a 55-entry jump table at `0x800AC30C`, taken as `effect - 2` and
 * bounds-checked `(unsigned)(effect - 2) < 55` — which is why effect 0 means
 * "cannot be collected at all" rather than "collect for no gain".
 *
 * ---------------------------------------------------------------------------
 * Why the flag bits are CONFIRMED and not a plausible guess
 * ---------------------------------------------------------------------------
 * Bits 4/5/6 light a dynamic light in the item's own colour, one bit per
 * channel (`0x80059648`…`0x8005973C` writes r, g and b from separate tests).
 * Decoded that way, the twelve key items name their own colours:
 *
 *     Redkey    0x0019  ->  R          Greenkey  0x0029  ->  G
 *     Bluekey   0x004B  ->  B          Yellowkey 0x0039  ->  R+G  = yellow
 *     Pkeypurp  0x0059  ->  R+B = purple    Whitekey  0x0079  ->  R+G+B
 *
 * Nothing in the decode was tuned to produce that. It is the same class of
 * argument the class table's health column carries (classtable.h) and the
 * armour table's protection column (weapontables.h): the field landing on
 * values the rest of the game already published under those names.
 *
 * ---------------------------------------------------------------------------
 * Eight items in the table are inert, and that is a finding
 * ---------------------------------------------------------------------------
 * Twelve of the 55 jump-table slots point at the dispatch's own failure exit
 * (`0x800372E8` with the "took something" flag still clear, which returns 2 =
 * nothing happened). Eight of them are named by real records:
 *
 *     Ionripper P, Plasmagun P, Flame P, Tesla P   the Xatrix/Rogue weapons
 *     Discharge P, Flame Fuel P                    and their ammo
 *     Screen P                                     power screen
 *     Stimpack P                                   the 2-point health
 *
 * The power screen is the interesting one: the damage path still tests its
 * powerup bit (`Q2_POWERUP_POWER_ARMOUR` is 0x18000, two bits, combat.h) but
 * only the shield's bit can ever be set, because only the shield has a handler.
 * A port that invented an effect for these would be *more* complete than the
 * game and wrong.
 *
 * ---------------------------------------------------------------------------
 * `place->unk` is an angle, not flags
 * ---------------------------------------------------------------------------
 * The spawner at `0x80058930` reads `lhu place[+0x0C]`, masks `0xFFF` and
 * stores it as the entity's yaw. So the low twelve bits of the place record's
 * halfword are a heading on the 4096-step circle and bits 12…15 are something
 * else — which is why the observed values 4096 / 32768 / 36864 / 53248 looked
 * like "flags OR'd with one" to a reader treating the whole halfword as a unit.
 */
#ifndef Q2PSX_ITEMTABLE_H
#define Q2PSX_ITEMTABLE_H

#include "disc.h"
#include "ident.h"
#include "q2psx.h"

#define Q2_ITEM_RECORD_SIZE 24
#define Q2_ITEM_MODEL_LEN   12
#define Q2_ITEM_EXTRA_MAX    4

/* PAL build. Per-build data, and refusing an uncatalogued executable is the
 * point: every one of these columns would decode into *something* at the wrong
 * address, and a rocket launcher that grants a key is worse than a failed load. */
#define Q2_ITEMTABLE_ADDR_SLES01534   0x8009F5CCu
#define Q2_ITEMTABLE_END_SLES01534    0x8009FBCCu   /* the 0xFF terminator */
#define Q2_ITEM_DISPATCH_SLES01534    0x800AC30Cu   /* the 55-entry jump table */
#define Q2_ITEM_SOUNDNAMES_SLES01534  0x800AC240u   /* 11 x 12-byte names */

/* The dispatch is indexed `effect - 2` and bounds-checked against 55, so this
 * is the whole usable range. Effect 0 is the table's own "no effect" value. */
#define Q2_ITEM_EFFECT_FIRST  2
#define Q2_ITEM_EFFECT_COUNT 55
#define Q2_ITEM_EFFECT_LAST  (Q2_ITEM_EFFECT_FIRST + Q2_ITEM_EFFECT_COUNT - 1)

/* ------------------------------------------------------------------------- */
/* Flags — all bits observed on this disc are accounted for                    */
/* ------------------------------------------------------------------------- */
enum q2_item_flag {
    /* 0x80059458: spin the model, yaw -= 3 per tick, then rebuild its matrix. */
    Q2_ITEM_SPIN        = 0x0001,
    /* 0x80059488: materialise — scale 0 -> 4096 at +2/tick, "msc_tele1", and
     * fifteen sparkle offsets. Set on disc only by Bluekey P. */
    Q2_ITEM_MATERIALISE = 0x0002,
    /* 0x800597C8: a countdown at +0xF4, then shrink away and free. Never set on
     * disc — the runtime sets it on items a dying actor drops. */
    Q2_ITEM_TIMED       = 0x0004,
    /* Set on exactly the twelve key/objective items and on nothing else. No
     * reader was located in the executable, so the NAME is inferred from that
     * partition; the bit itself is CONFIRMED to be those twelve records. */
    Q2_ITEM_OBJECTIVE   = 0x0008,
    /* 0x80059648..0x8005973C: one dynamic-light channel each. */
    Q2_ITEM_GLOW_R      = 0x0010,
    Q2_ITEM_GLOW_G      = 0x0020,
    Q2_ITEM_GLOW_B      = 0x0040,
    Q2_ITEM_GLOW        = 0x0070,
    /* 0x800593F4: suppresses the per-tick frame advance, so the model holds
     * frame 0. Set on disc only by Blackhole P. */
    Q2_ITEM_NO_ANIM     = 0x0080,
    /*
     * 0x80059A44: clears the 0x400 argument the spawner is otherwise given.
     *
     * AND THE 0x400 IS NOW IDENTIFIED. It is the DROP DISTANCE the shared
     * placement routine sweeps downward at spawn — `sll a3, a3, 10` at
     * 0x80059A50 turns it into 1024 — so this flag is Quake II's own no-drop
     * flag and the name says so. The creature spawner passes the same 1024 out
     * of 0x800AE894. See q2_entity_drop_to_floor.
     */
    Q2_ITEM_NO_DROP = 0x0100
};

/* One 24-byte record. */
typedef struct q2_item_def {
    s8   place_id;                        /* +0x00 matched against place->id  */
    u8   effect;                          /* +0x01 the touch dispatch index   */
    u16  flags;                           /* +0x02 see q2_item_flag           */
    char model[Q2_ITEM_MODEL_LEN + 1];    /* +0x04 CastList name, NOT always
                                           *       NUL-terminated on disc     */
    u16  extra[Q2_ITEM_EXTRA_MAX];        /* +0x10 0xFFFF-terminated list     */
    u8   extra_count;                     /* entries before the terminator    */
} q2_item_def;

/* 64 real records plus the terminator. */
#define Q2_ITEM_COUNT 64

typedef struct q2_item_table {
    q2_item_def def[Q2_ITEM_COUNT];
    u32         count;

    /* Which of the 55 dispatch slots are real, read from the jump table rather
     * than assumed: slot i (effect i + 2) is inert when it points at the
     * dispatch's failure exit. */
    u32  dispatch[Q2_ITEM_EFFECT_COUNT];
    u32  dispatch_default;      /* the failure exit every inert slot shares */

    /* The eleven item sound names at 0x800AC240, in table order. */
    char sound[11][Q2_ITEM_MODEL_LEN + 1];
} q2_item_table;

q2_result q2_item_table_load(q2_item_table *out, const disc *d,
                             const q2_build_id *id);

/* The same table as transcribed constants for the catalogued PAL build, so the
 * game side works without the disc in hand — and so `q2psx-inspect items` can
 * compare the two and keep both honest. */
const q2_item_table *q2_item_table_builtin(void);

/*
 * The record a place id selects, or NULL.
 *
 * This is the engine's own scan: first match wins, and one place id (6,
 * Bandol P) genuinely appears twice, so "first" is behaviour rather than an
 * accident to be tidied up.
 */
const q2_item_def *q2_item_find(const q2_item_table *t, s32 place_id);

/* True when this effect index reaches a real handler. Reads the loaded
 * dispatch when one is present and falls back to the built-in list. */
bool q2_item_effect_is_live(const q2_item_table *t, u32 effect);

/* Compare a loaded table against the built-in one, field by field. */
typedef void (*q2_item_report_fn)(void *user, const char *what,
                                  long expected, long got);
u32 q2_item_table_diff(const q2_item_table *disc_side,
                       const q2_item_table *builtin,
                       q2_item_report_fn report, void *user);

#endif /* Q2PSX_ITEMTABLE_H */
