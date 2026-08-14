/*
 * inventory.h — the player's health, armour, ammo, weapons and keys.
 *
 * Hammerhead retuned every LENGTH and TIME constant for the console but copied
 * the unitless gameplay values from PC Quake II verbatim. The max-ammo table is
 * the proof: at vaddr 0x8009C5C8 it reads 100/200/50/50/200/50, then the
 * bandolier tier 150/250/50/50/250/75, then the pack tier 200/300/100/100/
 * 300/100 — id's numbers exactly.
 *
 * That is a useful licence. Where a constant is unitless, the PC value is
 * almost certainly right and can be used with confidence. Where a constant is a
 * length or a duration, it was retuned and the PC value is almost certainly
 * wrong. worldscale.h makes the same distinction for the physics.
 *
 * ---------------------------------------------------------------------------
 * Weapons
 * ---------------------------------------------------------------------------
 * Eleven, read from the 12-byte-stride name table at file offset 0x863A8. The
 * order is the weapon id, and it matches PC Quake II's progression.
 *
 * ---------------------------------------------------------------------------
 * Keys, and the word they share
 * ---------------------------------------------------------------------------
 * The script's ONKEYDO primitive tests a bitfield at 0x800B29D0 with four masks
 * (all-set, any-set, all-clear, any-clear), and movers test it for locked doors.
 * So keys are not items in a list — they are bits, and the whole conditional
 * layer of the scripting language is built on them.
 *
 * Where those bits come FROM is now known, and it is not a global the pickup
 * code owns. Every item effect writes to one word in the player's client block
 * at `client+0x5C`, and the non-deathmatch pickup path at 0x80059970 then does
 *
 *     0x800B29D0 = client[0].flags & 0xFFF
 *
 * — 0x800C7CBC being client 0's own +0x5C, the base 0x800C7C60 being
 * materialised at eleven sites. So the script's key word is the low twelve bits
 * of player one's flags, refreshed after every pickup, and the same word's
 * upper bits carry the armour class and the power items. That is why this
 * structure holds one `flags` word rather than a `keys` field: splitting it
 * would invent a boundary the engine does not have.
 *
 * The twelve key bits are each set by exactly one item effect, read off the
 * touch dispatch's twelve one-line handlers (0x800371C0…0x800372C8).
 */
#ifndef Q2PSX_INVENTORY_H
#define Q2PSX_INVENTORY_H

#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* Ammo                                                                       */
/* ------------------------------------------------------------------------- */
typedef enum q2_ammo {
    Q2_AMMO_SHELLS = 0,
    Q2_AMMO_BULLETS,
    Q2_AMMO_GRENADES,
    Q2_AMMO_ROCKETS,
    Q2_AMMO_CELLS,
    Q2_AMMO_SLUGS,
    Q2_AMMO_COUNT
} q2_ammo;

/* Capacity tiers, in the table's own order. A bandolier raises the first tier
 * to the second; an ammo pack raises it to the third. */
typedef enum q2_ammo_tier {
    Q2_AMMO_TIER_BASE = 0,
    Q2_AMMO_TIER_BANDOLIER,
    Q2_AMMO_TIER_PACK,
    Q2_AMMO_TIER_COUNT
} q2_ammo_tier;

/* [tier][ammo], read out of the executable rather than invented. */
extern const s16 q2_ammo_max[Q2_AMMO_TIER_COUNT][Q2_AMMO_COUNT];

/* ------------------------------------------------------------------------- */
/* Weapons                                                                    */
/* ------------------------------------------------------------------------- */
typedef enum q2_weapon {
    Q2_WEAPON_BLASTER = 0,
    Q2_WEAPON_SHOTGUN,
    Q2_WEAPON_SUPER_SHOTGUN,
    Q2_WEAPON_MACHINEGUN,
    Q2_WEAPON_CHAINGUN,
    Q2_WEAPON_HAND_GRENADE,
    Q2_WEAPON_GRENADE_LAUNCHER,
    Q2_WEAPON_ROCKET_LAUNCHER,
    Q2_WEAPON_HYPERBLASTER,
    Q2_WEAPON_RAILGUN,
    Q2_WEAPON_BFG,
    Q2_WEAPON_COUNT
} q2_weapon;

extern const char *const q2_weapon_names[Q2_WEAPON_COUNT];

/* Which ammo a weapon draws on. The blaster draws none. */
extern const s8 q2_weapon_ammo[Q2_WEAPON_COUNT];

/* ------------------------------------------------------------------------- */
/* The client flags word — client+0x5C                                        */
/*                                                                            */
/* One word, three jobs. Each key bit is named by the single touch handler     */
/* that sets it; the item that carries it is in the comment.                  */
/* ------------------------------------------------------------------------- */
enum q2_inv_flag {
    Q2_KEY_BLUE            = 0x00000001u,  /* Bluekey P     0x800371C0 */
    Q2_KEY_RED             = 0x00000002u,  /* Redkey P      0x800371D8 */
    Q2_KEY_PASS            = 0x00000004u,  /* Pass P        0x800371F0 */
    Q2_KEY_NUKE            = 0x00000008u,  /* Nuke P        0x80037208 */
    Q2_KEY_HEAD            = 0x00000010u,  /* Head P        0x80037220 */
    Q2_KEY_PYRAMID_PURPLE  = 0x00000040u,  /* Pkeypurp P    0x80037238 */
    Q2_KEY_PYRAMID_RED     = 0x00000020u,  /* Pkeyred P     0x80037250 */
    Q2_KEY_DATA_CD         = 0x00000080u,  /* Datacd P      0x80037268 */
    Q2_KEY_DATA_SPINNER    = 0x00000100u,  /* Datasp P      0x80037280 */
    Q2_KEY_GREEN           = 0x00000200u,  /* Greenkey P    0x80037298 */
    Q2_KEY_YELLOW          = 0x00000400u,  /* Yellowkey P   0x800372B0 */
    Q2_KEY_WHITE           = 0x00000800u,  /* Whitekey P    0x800372C8 */

    /* The twelve bits the script sees. 0x80059978 masks exactly this. */
    Q2_KEY_MASK            = 0x00000FFFu,

    /* Armour class held, one bit each. The armour handlers set the bit for
     * their own class only when no stronger bit is already up, which is what
     * makes the three tests `& 0x4000`, `& 0x6000` and `& 0x7000` widen as the
     * class weakens (0x80036C28 / 0x80036CA0 / 0x80036D08). */
    Q2_INV_ARMOUR_JACKET   = 0x00001000u,
    Q2_INV_ARMOUR_COMBAT   = 0x00002000u,
    Q2_INV_ARMOUR_BODY     = 0x00004000u,
    Q2_INV_ARMOUR_MASK     = 0x00007000u,

    /* The two power items. Only the shield has a touch handler on this disc —
     * the screen's effect index lands on the dispatch's failure exit — but the
     * damage path tests both bits together (Q2_POWERUP_POWER_ARMOUR). */
    Q2_INV_POWER_SHIELD    = 0x00008000u,  /* Shield P      0x80036DB4 */
    Q2_INV_POWER_SCREEN    = 0x00010000u,  /* no handler exists         */

    /* Mega health is held rather than consumed, and decays on a timer. */
    Q2_INV_MEGA_HEALTH     = 0x00020000u   /* Mega Medi P   0x80036E30 */
};

/* ------------------------------------------------------------------------- */
/* Player inventory                                                           */
/* ------------------------------------------------------------------------- */
#define Q2_HEALTH_MAX_DEFAULT 100
#define Q2_ARMOUR_MAX_DEFAULT 200

/* Armour classes, in the order of the three-record table at 0x8009C5EC. The
 * armour handlers pass these as the class index (0x80036CFC / 0x80036C94 /
 * 0x80036C1C), and the cross-conversion at 0x80037338 compares them for
 * strength — so the numeric order IS the strength order. */
typedef enum q2_armour_class {
    Q2_ARMOUR_JACKET = 0,
    Q2_ARMOUR_COMBAT = 1,
    Q2_ARMOUR_BODY   = 2,
    Q2_ARMOUR_CLASS_COUNT
} q2_armour_class;

typedef struct q2_inventory {
    s16 health;
    s16 health_max;         /* client+0x48; adrenaline raises it              */
    s16 armour;             /* client+0x4E                                   */
    u8  armour_class;       /* client+0x4C, a q2_armour_class                */

    s16 ammo[Q2_AMMO_COUNT];
    u16 weapons;            /* bit per q2_weapon                             */
    s8  current_weapon;
    u8  ammo_tier;

    /* client+0x5C. The low twelve bits are the keys the script tests; see the
     * header comment and q2_inventory_script_keys. */
    u32 flags;

    /* client+0x52. The silencer grants shots, not time (0x800370BC). */
    s16 silencer_shots;

    /* client+0x54 and +0xBC: which item was last collected and until when the
     * HUD names it. 900 ticks is three seconds on the 300 Hz level clock. */
    u8  last_item;
    s32 item_name_until;

    /*
     * Powerup expiry on the level clock, from the four words at client+0xAC.
     * Zero means not held. Each handler extends rather than replaces:
     * `t = max(now, t) + 9000`, which is 30 seconds (0x800370D0…0x80037184).
     */
    s32 quad_until;         /* client+0xAC */
    s32 invuln_until;       /* client+0xB0 */
    s32 enviro_until;       /* client+0xB4 */
    s32 breather_until;     /* client+0xB8 */

    /* client+0xC8. Mega health bleeds one point per interval back to the cap. */
    s32 mega_health_next;
} q2_inventory;

void q2_inventory_init(q2_inventory *inv);

/* Capacity for one ammo type at the current tier. */
s16 q2_inventory_ammo_max(const q2_inventory *inv, q2_ammo type);

/* Add ammo, clamped to capacity. Returns how much was actually taken, which is
 * 0 when already full — that is what decides whether a pickup disappears. */
s16 q2_inventory_add_ammo(q2_inventory *inv, q2_ammo type, s16 amount);

/* Grant a weapon. Returns false if already held. */
bool q2_inventory_add_weapon(q2_inventory *inv, q2_weapon w);
bool q2_inventory_has_weapon(const q2_inventory *inv, q2_weapon w);

/* Switch weapons. Fails if not held. */
bool q2_inventory_select(q2_inventory *inv, q2_weapon w);

/* True when the current weapon can fire: held, and ammo available. */
bool q2_inventory_can_fire(const q2_inventory *inv);

/* Consume one shot's ammo. Returns false when it could not. */
bool q2_inventory_consume(q2_inventory *inv, s16 amount);

/* Health and armour. Armour absorbs a share of incoming damage, as in the
 * original lineage; returns the damage actually applied to health. */
s16 q2_inventory_add_health(q2_inventory *inv, s16 amount, bool allow_overheal);
s16 q2_inventory_apply_damage(q2_inventory *inv, s16 damage);

/* Keys are bits, not items, and they share a word with the armour class and the
 * power items — so giving one ORs into `flags` and the script only ever sees the
 * low twelve bits. */
void q2_inventory_give_key(q2_inventory *inv, u32 mask);
bool q2_inventory_has_keys(const q2_inventory *inv, u32 mask);

/* What 0x80059978 hands the script: `flags & 0xFFF`. */
u32 q2_inventory_script_keys(const q2_inventory *inv);

/* The cap for the armour class currently held, from the table at 0x8009C5EC. */
s16 q2_inventory_armour_max(const q2_inventory *inv);

#endif /* Q2PSX_INVENTORY_H */
