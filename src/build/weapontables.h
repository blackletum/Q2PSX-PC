/*
 * weapontables.h — the weapon and armour data tables, read out of the boot
 * executable.
 *
 * ---------------------------------------------------------------------------
 * What is data and what is code
 * ---------------------------------------------------------------------------
 * The combat system splits cleanly in two, and the split matters because the
 * two halves have different evidence.
 *
 * The *data* half is arrays in the executable's rodata: how much ammo a shot
 * costs, which ammo type a weapon draws on, which bit marks it owned, the order
 * the game auto-switches through, where each weapon's muzzle sits relative to
 * the eye, and how much of a hit each armour class absorbs. Those are read from
 * the disc by this module, and `q2psx-inspect weapons` reads them back and
 * compares — so "we understand this table" is something the build evaluates.
 *
 * The *behaviour* half — damage, pellet counts, spread, view kick, refire — is
 * not in any table. Every weapon has its own fire function and the numbers are
 * immediate operands inside it. Those live in `src/game/weapon.c`, transcribed
 * with the address of the instruction each came from. This module does not
 * pretend to read them.
 *
 * ---------------------------------------------------------------------------
 * The arrays are consumed ONE-BASED
 * ---------------------------------------------------------------------------
 * Every weapon array in this build is materialised as *base minus one element*
 * and indexed by the live weapon id, which runs 1…11 with 0 meaning "no
 * weapon". FORMATS.md §9.6 proves it from the auto-switch list. This module
 * therefore stores slot 0 as well, so callers index by the id the engine uses
 * and never have to subtract.
 *
 * `q2_weapon_cycle` (game side) walks 1…12, not 1…11 — the original's own
 * off-by-one. Slot 12 exists here for that reason: its owned-bit is 0, so the
 * scan always rejects it. Reproducing the bound rather than "fixing" it is the
 * point.
 *
 * ---------------------------------------------------------------------------
 * Where each array is
 * ---------------------------------------------------------------------------
 *   0x8009DB1C   bolt geometry     8 points x 3 s16, the blaster bolt's shape
 *   0x8009DB4C   ammo per shot     u32[12], 1-based; {-,0,1,2,1,1,1,1,1,1,1,50}
 *   0x8009DB7C   auto-switch list  u32, 0-terminated: 10 9 8 5 4 3 2 1 0
 *   0x8009DB9C   names             char[12][12], 1-based
 *   0x8009DC2C   owned bit         u32[12], 1-based; 1<<(id-1)
 *   0x8009DC5C   ammo type         u32[12], 1-based
 *   0x8009DC8C   HUD glyph         char[3][12], 0-based (see hudtables.h)
 *   0x8009D704   fire function     void(*[12])(edict*), 1-based
 *   0x800AE9C4   muzzle offsets    s16[3] each, one per weapon that has one
 *   0x8009C5EC   armour classes    {u8 base, u8 max, s16 normal, s16 energy}
 *   0x800ACBC8   weapon sounds     char[12][22]
 *
 * The ammo-per-shot column is the one that proves the 1-based reading from the
 * data side alone: read 1-based it is id's own {0,1,2,1,1,1,1,1,1,1,50} — the
 * BFG's fifty cells landing on the BFG. Read 0-based the fifty lands on the
 * railgun and the blaster costs a shell.
 *
 * ---------------------------------------------------------------------------
 * The bases OVERLAP their neighbours, and that is the second proof
 * ---------------------------------------------------------------------------
 * Each array is exactly eleven elements, and the 1-based base is one element
 * BEFORE the first of them — which means the base word belongs to whatever
 * table sits above it, and the word after element eleven belongs to whatever
 * sits below. Both are readable and both are somebody else's data:
 *
 *     ammo_per_shot[0]  = 0x7FFF, the last halfword pair of the bolt hull
 *     ammo_per_shot[12] = 10,     the head of the auto-switch list
 *     ammo_type[0]      = 0,      the tail padding of the owned-bit array
 *     owned_bit[12]     = 0,      the ammo-type array's own base word
 *
 * That last one is load-bearing rather than incidental: it is the zero the
 * cycle scan tests when it walks into slot 12, so the phantom slot is rejected
 * by an accident of layout rather than by a bounds check. The port keeps zero
 * there for exactly that reason.
 *
 * A 12- or 13-element reading of any of these arrays would put a neighbouring
 * table's first word inside a weapon's row, and this is where it would show.
 * `q2psx-inspect weapons` therefore compares ids 1..11 and checks the overlap
 * words separately, so the adjacency is evidence rather than noise.
 *
 * ---------------------------------------------------------------------------
 * Armour, and why its table is not a guess
 * ---------------------------------------------------------------------------
 * Three records of six bytes at 0x8009C5EC, immediately after the ammo-capacity
 * tiers and read by the absorption routine at 0x80057BE4 (`+2` for ordinary
 * damage, `+4` for energy). Decoded they are:
 *
 *     jacket   base  25  max  50   normal 1229/4096   energy    0/4096
 *     combat   base  50  max 100   normal 2458/4096   energy 1229/4096
 *     body     base 100  max 200   normal 3277/4096   energy 2458/4096
 *
 * 1229/4096 is 0.300, 2458/4096 is 0.600, 3277/4096 is 0.800. Those are PC
 * Quake II's armour constants exactly, on all six values, including the energy
 * column that is 0.00/0.30/0.60 there too — and the counts 25/50, 50/100,
 * 100/200 land with them. Nothing in the decode was tuned to produce that; it
 * is the same argument the class table's health column carries (classtable.h),
 * and it is why the record layout is CONFIRMED rather than plausible.
 *
 * ---------------------------------------------------------------------------
 * Muzzle offsets
 * ---------------------------------------------------------------------------
 * Each fire function loads three halfwords, negates the middle one, and hands
 * them to the local-to-world rotation at 0x8006FC1C.
 *
 * THE ENGINE NEGATES IT TWICE. This block used to stop at the first negation
 * and conclude "the stored triple is (right, down, forward) and the engine
 * flips the middle to get up". The second one is at 0x8004C04C, a `subu` on
 * the ROTATED Y as it is added to the origin, and it cancels the first — so
 * the value reaches world space with its sign unchanged and the stored triple
 * is (right, DOWN, forward).
 *
 * The values here are the disc's, unmodified. The consumer
 * (q2_weapon_muzzle_origin) takes them as (right, down, forward) and rotates
 * them by the camera matrix's transpose, whose row 1 is already the downward
 * axis, so nothing negates anything.
 *
 * What the misreading cost: q2_weapon_muzzle_origin built an "up" vector by
 * negating one component of the view matrix's row 1. That put every muzzle on
 * the wrong side of the eye — 112 world units for the blaster, 280 for the
 * hyperblaster — and, because only one of three components was flipped, left
 * the basis non-orthonormal, so at a 45-degree pitch the vertical part of the
 * offset was applied along FORWARD instead. The test pinned the inverted sign
 * and only ever exercised pitch 0.
 *
 * At the established world scale of 10, the blaster's (80, 56, 250) is (8.0,
 * 5.6, 25.0) PC units against id's own (8, 8, viewheight-8) — the horizontal
 * offset and the forward reach match to the unit, which is a good independent
 * check on both the scale and the table's identity.
 */
#ifndef Q2PSX_WEAPONTABLES_H
#define Q2PSX_WEAPONTABLES_H

#include "exe.h"
#include "ident.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Where the tables are in the catalogued PAL build                           */
/* ------------------------------------------------------------------------- */
#define Q2_WT_ADDR_BOLT_SHAPE    0x8009DB1Cu
#define Q2_WT_ADDR_AMMO_PER_SHOT 0x8009DB4Cu   /* 1-based base */
#define Q2_WT_ADDR_AUTOSWITCH    0x8009DB7Cu
#define Q2_WT_ADDR_NAMES         0x8009DB9Cu   /* 1-based base */
#define Q2_WT_ADDR_OWNED_BIT     0x8009DC2Cu   /* 1-based base */
#define Q2_WT_ADDR_AMMO_TYPE     0x8009DC5Cu   /* 1-based base */
#define Q2_WT_ADDR_FIRE_FN       0x8009D704u   /* 1-based base */
#define Q2_WT_ADDR_MUZZLE        0x800AE9C4u
#define Q2_WT_ADDR_ARMOUR        0x8009C5ECu
#define Q2_WT_ADDR_SOUNDS        0x800ACBC8u

/* Slots, not weapons: index 0 is "no weapon" and index 12 is the phantom the
 * original's cycle loop walks into and always rejects. */
#define Q2_WT_SLOTS        13
#define Q2_WT_WEAPON_COUNT 11
#define Q2_WT_NAME_LEN     12
#define Q2_WT_AUTOSWITCH_MAX 12
#define Q2_WT_ARMOUR_CLASSES  3
#define Q2_WT_SOUND_COUNT    22
#define Q2_WT_BOLT_POINTS     8

/* The 22 weapon sound names, in table order. Named so a caller can ask for one
 * without counting, and so the order is checkable against the disc. */
typedef enum q2_wt_sound {
    Q2_WSND_RAILGUN = 0,     /* wep_railgf1a */
    Q2_WSND_NO_AMMO,         /* wep_noammo   */
    Q2_WSND_GRENADE_BOUNCE,  /* wep_grenlb1b */
    Q2_WSND_HANDGREN_PRIME,  /* wep_hgrenc1b */
    Q2_WSND_HANDGREN_THROW,  /* wep_hgrent1a */
    Q2_WSND_SHOTGUN_RELOAD,  /* wep_shotgr1b */
    Q2_WSND_SSHOT_RELOAD,    /* wep_sshotr1b */
    Q2_WSND_GRENADE_EXPLODE, /* wep_grenlx1a */
    Q2_WSND_BLASTER,         /* wep_blastf1a */
    Q2_WSND_SHOTGUN,         /* wep_shotgf1b */
    Q2_WSND_SUPER_SHOTGUN,   /* wep_sshotf1b */
    Q2_WSND_MACHINEGUN,      /* wep_machgf1b */
    Q2_WSND_CHAINGUN_UP,     /* wep_chngnu1a */
    Q2_WSND_CHAINGUN_LOOP,   /* wep_chngnl1a */
    Q2_WSND_CHAINGUN_DOWN,   /* wep_chngnd1a */
    Q2_WSND_HYPERBLASTER,    /* wep_hyprbf1a */
    Q2_WSND_HYPERBLAST_DOWN, /* wep_hyprbd1a */
    Q2_WSND_ROCKET,          /* wep_rocklf1a */
    Q2_WSND_GRENADE_LAUNCH,  /* wep_grenlf1a */
    Q2_WSND_BURN,            /* pla_burn1    */
    Q2_WSND_SPARK,           /* amb_spark5   */
    Q2_WSND_BFG              /* wep_bfg__f1y */
} q2_wt_sound;

/* One armour class, 6 bytes on disc. Protection is 1.0.12: 4096 would be total. */
typedef struct q2_wt_armour {
    u8  base_count;
    u8  max_count;
    s16 normal_protection;
    s16 energy_protection;
} q2_wt_armour;

typedef struct q2_weapon_tables {
    q2_exe exe;                 /* owns the executable image */

    /* All 1-based: index by the live weapon id. */
    s32  ammo_per_shot[Q2_WT_SLOTS];
    s32  ammo_type[Q2_WT_SLOTS];
    u32  owned_bit[Q2_WT_SLOTS];
    u32  fire_fn[Q2_WT_SLOTS];              /* address, for the record  */
    char name[Q2_WT_SLOTS][Q2_WT_NAME_LEN + 1];

    /* Preference order, 0-terminated on disc; `autoswitch_count` excludes the
     * terminator. Ids, so already 1-based. */
    s32  autoswitch[Q2_WT_AUTOSWITCH_MAX];
    u32  autoswitch_count;

    /* (right, up, forward), already sign-corrected. Indexed by weapon id;
     * weapons whose fire function loads no offset read zero. */
    s16  muzzle[Q2_WT_SLOTS][3];

    q2_wt_armour armour[Q2_WT_ARMOUR_CLASSES];

    char sound[Q2_WT_SOUND_COUNT][Q2_WT_NAME_LEN + 1];

    /* The bolt's eight local points, in world units. */
    s16  bolt_shape[Q2_WT_BOLT_POINTS][3];
} q2_weapon_tables;

/*
 * Read the tables out of the disc's boot executable.
 *
 * Refuses an uncatalogued build with Q2_ERR_UNSUPPORTED rather than reading a
 * plausible-looking wrong address: every one of these arrays would decode into
 * *something* at the wrong offset, and a shotgun that costs three rockets is a
 * harder bug to see than a failed load.
 */
q2_result q2_weapon_tables_load(q2_weapon_tables *out, const disc *d,
                                const q2_build_id *id);
void      q2_weapon_tables_free(q2_weapon_tables *t);

/*
 * The same tables as transcribed constants for the catalogued PAL build.
 *
 * The game side uses this so combat works without the disc in hand — tests
 * especially — and `q2psx-inspect weapons` compares it field by field against
 * what `q2_weapon_tables_load` reads, which is what keeps the two honest.
 */
const q2_weapon_tables *q2_weapon_tables_builtin(void);

/*
 * Compare a loaded set against the built-in one. Returns the number of
 * mismatching fields and, when `report` is non-NULL, calls it for each.
 */
typedef void (*q2_wt_report_fn)(void *user, const char *what,
                                long expected, long got);
u32 q2_weapon_tables_diff(const q2_weapon_tables *disc_side,
                          const q2_weapon_tables *builtin,
                          q2_wt_report_fn report, void *user);

#endif /* Q2PSX_WEAPONTABLES_H */
