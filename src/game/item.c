#include "item.h"

#include "trig.h"
#include "weapontables.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The dispatch, transcribed                                                  */
/*                                                                            */
/* One row per effect index. Empty rows are the twelve slots that point at the  */
/* dispatch's failure exit plus indices 0 and 1, which the bounds check         */
/* `(unsigned)(effect - 2) < 55` rejects before the jump.                      */
/* ------------------------------------------------------------------------- */
#define FX_LAST Q2_ITEM_EFFECT_LAST

static const q2_item_fx k_fx[FX_LAST + 1] = {
    /* 0, 1 — below the dispatch's range (0x8003638C bounds check). */
    { 0 }, { 0 },

    /* --- weapons. bit = 1 << (id - 1); the ammo is the weapon's own type. --- */
    /*  2 */ { Q2_ITEMFX_WEAPON, 2,  10,  20, Q2_SND_WEAPON, 0, 0x800363BCu },
    /*  3 */ { Q2_ITEMFX_WEAPON, 3,  10,  20, Q2_SND_WEAPON, 0, 0x8003645Cu },
    /*  4 */ { Q2_ITEMFX_WEAPON, 4,  50, 100, Q2_SND_WEAPON, 0, 0x800364FCu },
    /*  5 */ { Q2_ITEMFX_WEAPON, 5,  50, 100, Q2_SND_WEAPON, 0, 0x8003659Cu },
    /*  6 */ { 0 },                       /* inert: no record names it       */
    /*  7 */ { Q2_ITEMFX_WEAPON, 7,   5,  10, Q2_SND_WEAPON, 0, 0x8003663Cu },
    /*  8 */ { Q2_ITEMFX_WEAPON, 8,   5,  10, Q2_SND_WEAPON, 0, 0x800366ECu },
    /*  9 */ { Q2_ITEMFX_WEAPON, 9,  50, 100, Q2_SND_WEAPON, 0, 0x8003678Cu },
    /* 10 */ { Q2_ITEMFX_WEAPON, 10, 10,  20, Q2_SND_WEAPON, 0, 0x800368CCu },
    /* 11 */ { Q2_ITEMFX_WEAPON, 11, 50, 100, Q2_SND_WEAPON, 0, 0x8003682Cu },

    /* 12..16 — inert. Ionripper P, Plasmagun P, Discharge P, Flame P and
     * Tesla P name these and get nothing; the Xatrix/Rogue weapons were never
     * implemented on this disc. */
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 },

    /* 17 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_NUKE, 0x80037208u },

    /* --- ammo boxes --- */
    /* 18 */ { Q2_ITEMFX_AMMO, Q2_AMMO_SHELLS,   10,  20, Q2_SND_AMMO, 0, 0x8003696Cu },
    /* 19 */ { Q2_ITEMFX_AMMO, Q2_AMMO_BULLETS,  50, 100, Q2_SND_AMMO, 0, 0x800369D4u },
    /* 20 */ { Q2_ITEMFX_AMMO, Q2_AMMO_GRENADES,  5,  10, Q2_SND_AMMO, 0, 0x80036A3Cu },
    /* 21 */ { Q2_ITEMFX_AMMO, Q2_AMMO_ROCKETS,   5,  10, Q2_SND_AMMO, 0, 0x80036AACu },
    /* 22 */ { Q2_ITEMFX_AMMO, Q2_AMMO_CELLS,    50, 100, Q2_SND_AMMO, 0, 0x80036B14u },
    /* 23 */ { Q2_ITEMFX_AMMO, Q2_AMMO_SLUGS,    10,  20, Q2_SND_AMMO, 0, 0x80036BACu },

    /* 24, 25 — inert. Flame Fuel P names 24. */
    { 0 }, { 0 },

    /* --- armour --- */
    /* 26 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_BODY,   0, 0, Q2_SND_ARMOUR, 0, 0x80036C14u },
    /* 27 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_COMBAT, 0, 0, Q2_SND_ARMOUR, 0, 0x80036C8Cu },
    /* 28 */ { Q2_ITEMFX_ARMOUR, Q2_ARMOUR_JACKET, 0, 0, Q2_SND_ARMOUR, 0, 0x80036CF4u },
    /* 29 */ { Q2_ITEMFX_ARMOUR_SHARD, -1, 2, 2, Q2_SND_ARMOUR_SHARD, 0, 0x80036D6Cu },
    /* 30 */ { Q2_ITEMFX_POWER_SHIELD, Q2_AMMO_CELLS, 50, 100, Q2_SND_POWER, 0,
               0x80036DA8u },

    /* 31 — inert. Screen P names it, and the damage path still tests the power
     * screen's bit, so the bit can never be set. */
    { 0 },

    /* --- health --- */
    /* 32 */ { Q2_ITEMFX_MEGA_HEALTH, -1, 100, 100, Q2_SND_MEGA_HEALTH, 0,
               0x80036E20u },
    /* 33 */ { Q2_ITEMFX_ADRENALINE,  -1,   1,   1, Q2_SND_ITEM, 0, 0x80036E64u },
    /* 34 */ { Q2_ITEMFX_HEALTH,      -1,  10,  10, Q2_SND_MEDKIT, 0, 0x80036EA8u },
    /* 35 */ { Q2_ITEMFX_HEALTH,      -1,  25,  25, Q2_SND_LARGE_MEDKIT, 0,
               0x80036EDCu },

    /* 36 — inert. Stimpack P names it: the 2-point health has no handler. */
    { 0 },

    /* --- carry capacity --- */
    /* 37 */ { Q2_ITEMFX_AMMO_PACK,  Q2_AMMO_TIER_PACK,      0, 0, Q2_SND_ITEM, 0,
               0x80036F10u },
    /* 38 */ { Q2_ITEMFX_BANDOLIER,  Q2_AMMO_TIER_BANDOLIER, 0, 0, Q2_SND_ITEM, 0,
               0x8003701Cu },
    /* 39 */ { Q2_ITEMFX_SILENCER,   -1, 30, 30, Q2_SND_ITEM, 0, 0x800370B4u },

    /* --- powerups --- */
    /* 40 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_QUAD,     0, 0, Q2_SND_QUAD,    0,
               0x800370D0u },
    /* 41 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_INVULN,   0, 0, Q2_SND_PROTECT, 0,
               0x8003710Cu },
    /* 42 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_ENVIRO,   0, 0, Q2_SND_ITEM,    0,
               0x80037148u },
    /* 43 */ { Q2_ITEMFX_POWERUP, Q2_POWERUP_BREATHER, 0, 0, Q2_SND_ITEM,    0,
               0x80037184u },

    /* --- keys and objectives. One bit each, one handler each. --- */
    /* 44 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_BLUE,           0x800371C0u },
    /* 45 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_RED,            0x800371D8u },
    /* 46 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PASS,           0x800371F0u },
    /* 47 */ { 0 },
    /* 48 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_HEAD,           0x80037220u },
    /* 49 */ { 0 },
    /* 50 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PYRAMID_PURPLE, 0x80037238u },
    /* 51 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_PYRAMID_RED,    0x80037250u },
    /* 52 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_DATA_CD,        0x80037268u },
    /* 53 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_DATA_SPINNER,   0x80037280u },
    /* 54 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_GREEN,          0x80037298u },
    /* 55 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_YELLOW,         0x800372B0u },
    /* 56 */ { Q2_ITEMFX_KEY, -1, 0, 0, Q2_SND_ITEM, Q2_KEY_WHITE,          0x800372C8u }
};

const q2_item_fx *q2_item_effect_def(u32 effect)
{
    if (effect > FX_LAST)
        return &k_fx[0];
    return &k_fx[effect];
}

const char *q2_item_effect_name(u32 effect)
{
    static const char *const kind_name[Q2_ITEMFX_KIND_COUNT] = {
        "inert", "weapon", "ammo", "armour", "armour shard", "power shield",
        "health", "mega health", "adrenaline", "bandolier", "ammo pack",
        "silencer", "powerup", "key"
    };
    const q2_item_fx *d = q2_item_effect_def(effect);

    if (d->kind >= Q2_ITEMFX_KIND_COUNT)
        return "?";
    return kind_name[d->kind];
}

/* ------------------------------------------------------------------------- */
/* Helpers the dispatch shares                                                */
/* ------------------------------------------------------------------------- */

/* 0x80037DFC: min(max, cur + add), all in 16 bits. */
static s16 clamp_add(s16 max, s16 cur, s16 add)
{
    s16 sum = (s16)(cur + add);
    return sum > max ? max : sum;
}

/*
 * 0x80037E28. Returns 0 only when the weapon is already held AND deathmatch AND
 * weapons-stay — the one case where a second pickup must be refused outright.
 * `selects` is the weapon the engine switches to when the blaster is out.
 */
static bool give_weapon(q2_inventory *inv, int weapon_id, int selects,
                        const q2_entity_world *w)
{
    u32 bit;
    bool had;

    if (!inv || weapon_id < 1 || weapon_id > Q2_WEAPON_COUNT)
        return false;

    /* The engine's owned bit is 1 << (id - 1) and its ids are 1-based
     * (weapontables.h); q2_inventory's enum is 0-based, so the shift is the
     * same number and the enum value is id - 1. */
    bit = 1u << (weapon_id - 1);
    had = (inv->weapons & bit) != 0;

    if (had) {
        /* 0x80037E4C: only this combination refuses. */
        if (w && w->deathmatch && w->weapons_stay)
            return false;
        return true;
    }

    inv->weapons |= (u16)bit;

    /* 0x80037E84: the switch happens only when the blaster is out. */
    if (inv->current_weapon == Q2_WEAPON_BLASTER &&
        selects >= 1 && selects <= Q2_WEAPON_COUNT)
        inv->current_weapon = (s8)(selects - 1);

    return true;
}

/* The ammo type a weapon id draws on, 1-based in, q2_ammo out. */
static int weapon_ammo_of(int weapon_id)
{
    if (weapon_id < 1 || weapon_id > Q2_WEAPON_COUNT)
        return -1;
    return q2_weapon_ammo[weapon_id - 1];
}

s16 q2_item_armour_amount(q2_inventory *inv, u8 cls)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    const q2_wt_armour *nw, *cw;
    s16 cur_type, keep;
    s32 ratio, add;

    if (!inv || !wt)
        return 0;
    if (cls >= Q2_WT_ARMOUR_CLASSES)
        cls = Q2_WT_ARMOUR_CLASSES - 1;

    /* 0x8003733C: with no armour held, the class and its flag bits are reset
     * first. The mask is the literal the engine ANDs with — it clears the three
     * class bits and everything above bit 18. */
    if (inv->armour == 0) {
        inv->armour_class = 0;
        inv->flags &= 0x00078FFFu;
        if (cls > 0)
            inv->armour_class = cls;
    }

    cur_type = (s16)inv->armour_class;
    if (cur_type >= Q2_WT_ARMOUR_CLASSES)
        cur_type = Q2_WT_ARMOUR_CLASSES - 1;

    nw = &wt->armour[cls];
    cw = &wt->armour[cur_type];

    if (cls < (u8)cur_type) {
        /*
         * 0x8003738C: weaker armour on top of stronger. The ratio is formed in
         * 2-bit fixed point — `(new.normal << 2) / cur.normal` — multiplied by
         * the new class's base count and shifted back. That is PC Quake II's
         * `salvage = new->normal_protection / old->normal_protection` with a
         * float replaced by two bits, which is why the numbers land where id's
         * do without anything being tuned.
         */
        ratio = ((s32)nw->normal_protection << 2) / (s32)cw->normal_protection;
        add   = ratio * (s32)nw->base_count;
        keep  = inv->armour;
        inv->armour = 0;
        return (s16)(keep + (add >> 2));
    }

    if (cls == (u8)cur_type) {
        /* 0x80037490: same class, just the base count. */
        return (s16)nw->base_count;
    }

    /* 0x80037404: stronger armour. The old value converts down and the class
     * changes. */
    ratio = ((s32)cw->normal_protection << 2) / (s32)nw->normal_protection;
    add   = ratio * (s32)inv->armour;
    inv->armour       = 0;
    inv->armour_class = cls;
    return (s16)((s16)nw->base_count + (add >> 2));
}

static s16 armour_cap(const q2_inventory *inv)
{
    const q2_weapon_tables *wt = q2_weapon_tables_builtin();
    u8 cls;

    if (!inv || !wt)
        return 0;
    cls = inv->armour_class;
    if (cls >= Q2_WT_ARMOUR_CLASSES)
        cls = Q2_WT_ARMOUR_CLASSES - 1;
    return (s16)wt->armour[cls].max_count;
}

static s32 *powerup_slot(q2_inventory *inv, int slot)
{
    switch (slot) {
    case Q2_POWERUP_QUAD:     return &inv->quad_until;
    case Q2_POWERUP_INVULN:   return &inv->invuln_until;
    case Q2_POWERUP_ENVIRO:   return &inv->enviro_until;
    case Q2_POWERUP_BREATHER: return &inv->breather_until;
    default:                  return NULL;
    }
}

static s32 powerup_expiry(const q2_inventory *inv, int slot)
{
    switch (slot) {
    case Q2_POWERUP_QUAD:     return inv->quad_until;
    case Q2_POWERUP_INVULN:   return inv->invuln_until;
    case Q2_POWERUP_ENVIRO:   return inv->enviro_until;
    case Q2_POWERUP_BREATHER: return inv->breather_until;
    default:                  return 0;
    }
}

bool q2_item_powerup_active(const q2_inventory *inv, q2_powerup_slot slot,
                            s32 level_time)
{
    s32 until;

    if (!inv)
        return false;
    until = powerup_expiry(inv, (int)slot);
    if (until == 0)
        return false;

    /* The handlers compare unsigned (`sltu` at 0x800370E0), so the clock is
     * treated as unsigned and a wrap is not special-cased. */
    return (u32)level_time < (u32)until;
}

void q2_item_mega_health_tick(q2_inventory *inv, s32 level_time)
{
    if (!inv)
        return;
    if (!(inv->flags & Q2_INV_MEGA_HEALTH))
        return;

    if (inv->mega_health_next == 0) {
        inv->mega_health_next = level_time + Q2_ITEM_MEGA_DECAY_TICKS;
        return;
    }
    if ((u32)level_time < (u32)inv->mega_health_next)
        return;

    inv->mega_health_next = level_time + Q2_ITEM_MEGA_DECAY_TICKS;

    if (inv->health > inv->health_max) {
        inv->health--;
    } else {
        inv->flags &= ~Q2_INV_MEGA_HEALTH;
        inv->mega_health_next = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* The touch dispatch                                                         */
/* ------------------------------------------------------------------------- */
q2_touch_result q2_item_touch(u32 effect, q2_inventory *inv, const s32 pos[3],
                             q2_entity_world *w)
{
    const q2_item_fx *d;
    bool took = false;       /* s3 */
    bool weaponish = false;  /* !s6 */
    bool infinite_ammo;
    s16 amount;

    if (!inv || !w)
        return Q2_TOUCH_NOTHING;

    /* 0x8003638C: `(unsigned)(effect - 2) < 55`, so 0 and 1 fall out here. */
    if (effect < Q2_ITEM_EFFECT_FIRST || effect > FX_LAST)
        return Q2_TOUCH_NOTHING;

    /* An effect index whose dispatch slot is the failure exit does nothing, and
     * the table says which those are rather than this code assuming it. */
    if (!q2_item_effect_is_live(w->items, effect))
        return Q2_TOUCH_NOTHING;

    d = q2_item_effect_def(effect);
    if (d->kind == Q2_ITEMFX_NONE)
        return Q2_TOUCH_NOTHING;

    infinite_ammo = (w->cheats & Q2_CHEAT_INFINITE_AMMO) != 0;
    amount = w->deathmatch ? d->amount_dm : d->amount;

    switch (d->kind) {

    case Q2_ITEMFX_WEAPON: {
        int ammo = weapon_ammo_of(d->subject);
        s16 cap  = ammo >= 0 ? q2_inventory_ammo_max(inv, (q2_ammo)ammo) : 0;
        int bit_weapon = d->subject;

        /* 0x800363BC: with INFINITE AMMO on, the weapon grants capacity. */
        if (infinite_ammo)
            amount = cap;

        /*
         * The grenade launcher grants the HAND GRENADE first (bit 0x20, i.e.
         * weapon 6) and the launcher second (0x8003668C / 0x800366D0), and both
         * calls name weapon 7 as the one to switch to. So one pickup yields two
         * weapons — reproduced rather than simplified, because a player who
         * picks up a launcher in the original can throw grenades.
         */
        if (d->subject == Q2_WEAPON_GRENADE_LAUNCHER + 1)
            bit_weapon = Q2_WEAPON_HAND_GRENADE + 1;

        if (!give_weapon(inv, bit_weapon, d->subject, w))
            return Q2_TOUCH_NOTHING;

        took = true;

        if (ammo >= 0)
            inv->ammo[ammo] = clamp_add(cap, inv->ammo[ammo], amount);

        weaponish = true;

        if (d->subject == Q2_WEAPON_GRENADE_LAUNCHER + 1)
            give_weapon(inv, d->subject, d->subject, w);
        break;
    }

    case Q2_ITEMFX_AMMO: {
        q2_ammo type = (q2_ammo)d->subject;
        s16 cap = q2_inventory_ammo_max(inv, type);

        /*
         * 0x80036B14: cells while a power item is held and the player has none
         * plays the power sound as well — the shield coming back to life.
         */
        if (type == Q2_AMMO_CELLS &&
            (inv->flags & Q2_INV_POWER_SHIELD) && inv->ammo[type] == 0)
            q2_ent_sound_at(&w->events, Q2_SND_POWER, pos);

        /* 0x8003698C: full means the box stays where it is. */
        if (inv->ammo[type] >= cap)
            return Q2_TOUCH_NOTHING;

        took = true;
        inv->ammo[type] = clamp_add(cap, inv->ammo[type], amount);

        /* 0x80036A90: the grenade box is also the hand grenade. */
        if (type == Q2_AMMO_GRENADES)
            give_weapon(inv, Q2_WEAPON_HAND_GRENADE + 1,
                        Q2_WEAPON_HAND_GRENADE + 1, w);
        break;
    }

    case Q2_ITEMFX_ARMOUR: {
        u8  cls = (u8)d->subject;
        s16 add, cap;
        u32 test;

        add = q2_item_armour_amount(inv, cls);

        /* 0x80036C28 / 0x80036CA0 / 0x80036D08: the test widens as the class
         * weakens, so a stronger bit already up suppresses the weaker flag. */
        test = cls == Q2_ARMOUR_BODY   ? Q2_INV_ARMOUR_BODY
             : cls == Q2_ARMOUR_COMBAT ? (Q2_INV_ARMOUR_BODY | Q2_INV_ARMOUR_COMBAT)
                                       : Q2_INV_ARMOUR_MASK;
        if ((inv->flags & test) == 0)
            inv->flags |= cls == Q2_ARMOUR_BODY   ? Q2_INV_ARMOUR_BODY
                        : cls == Q2_ARMOUR_COMBAT ? Q2_INV_ARMOUR_COMBAT
                                                  : Q2_INV_ARMOUR_JACKET;

        /* The cap is read AFTER the conversion, because the conversion may have
         * changed the class. */
        cap = armour_cap(inv);

        /* Body and jacket refuse a full player; combat does not. */
        if (cls != Q2_ARMOUR_COMBAT && inv->armour >= cap)
            return Q2_TOUCH_NOTHING;

        took = true;
        inv->armour = clamp_add(cap, inv->armour, add);
        break;
    }

    case Q2_ITEMFX_ARMOUR_SHARD:
        /* 0x80036D74: a shard with no armour held makes it jacket armour, and
         * its own cap is the literal 999 rather than the class maximum. */
        if ((inv->flags & Q2_INV_ARMOUR_MASK) == 0)
            inv->flags |= Q2_INV_ARMOUR_JACKET;
        took = true;
        inv->armour = clamp_add(999, inv->armour, amount);
        break;

    case Q2_ITEMFX_POWER_SHIELD: {
        s16 cap = q2_inventory_ammo_max(inv, Q2_AMMO_CELLS);

        /* 0x80036DB4: the bit goes up whether or not the cells fit. */
        inv->flags |= Q2_INV_POWER_SHIELD;
        took = true;
        if (inv->ammo[Q2_AMMO_CELLS] < cap)
            inv->ammo[Q2_AMMO_CELLS] =
                clamp_add(cap, inv->ammo[Q2_AMMO_CELLS], amount);
        break;
    }

    case Q2_ITEMFX_HEALTH:
        /* 0x80036EB4: the cap is the player's own max, and being at it refuses
         * the pickup so it can be come back for. */
        if (inv->health >= inv->health_max)
            return Q2_TOUCH_NOTHING;
        took = true;
        inv->health = clamp_add(inv->health_max, inv->health, amount);
        break;

    case Q2_ITEMFX_MEGA_HEALTH:
        /* 0x80036E20: 200 and 100 are immediates, not the player's max — so
         * mega health always applies and always overheals to 200. */
        inv->flags |= Q2_INV_MEGA_HEALTH;
        took = true;
        inv->health = clamp_add(200, inv->health, amount);
        inv->mega_health_next = w->level_time + Q2_ITEM_MEGA_DECAY_TICKS;
        break;

    case Q2_ITEMFX_ADRENALINE:
        /* 0x80036E64: max health rises by one and health is topped up to it. */
        inv->health_max = (s16)(inv->health_max + amount);
        took = true;
        if (inv->health < inv->health_max)
            inv->health = clamp_add(inv->health_max, inv->health,
                                    inv->health_max);
        break;

    case Q2_ITEMFX_BANDOLIER:
        /* 0x8003701C: a pack already held is not downgraded. Then shells and
         * bullets top up at the NEW tier's capacity. */
        if (inv->ammo_tier != Q2_AMMO_TIER_PACK)
            inv->ammo_tier = Q2_AMMO_TIER_BANDOLIER;
        took = true;
        inv->ammo[Q2_AMMO_SHELLS] =
            clamp_add(q2_inventory_ammo_max(inv, Q2_AMMO_SHELLS),
                      inv->ammo[Q2_AMMO_SHELLS], w->deathmatch ? 20 : 10);
        inv->ammo[Q2_AMMO_BULLETS] =
            clamp_add(q2_inventory_ammo_max(inv, Q2_AMMO_BULLETS),
                      inv->ammo[Q2_AMMO_BULLETS], w->deathmatch ? 100 : 50);
        break;

    case Q2_ITEMFX_AMMO_PACK: {
        /* 0x80036F10: the tier is set to the pack unconditionally, and then all
         * six types top up — each by its own amount, at the pack capacities. */
        static const s16 sp[Q2_AMMO_COUNT] = { 10, 50,  5,  5, 50, 10 };
        static const s16 dm[Q2_AMMO_COUNT] = { 20, 100, 10, 10, 100, 20 };
        int i;

        inv->ammo_tier = Q2_AMMO_TIER_PACK;
        took = true;
        for (i = 0; i < Q2_AMMO_COUNT; i++)
            inv->ammo[i] = clamp_add(q2_inventory_ammo_max(inv, (q2_ammo)i),
                                     inv->ammo[i],
                                     w->deathmatch ? dm[i] : sp[i]);
        break;
    }

    case Q2_ITEMFX_SILENCER:
        /* 0x800370BC: shots, not seconds, and no cap. */
        inv->silencer_shots = (s16)(inv->silencer_shots + amount);
        took = true;
        break;

    case Q2_ITEMFX_POWERUP: {
        s32 *slot = powerup_slot(inv, d->subject);

        if (!slot)
            return Q2_TOUCH_NOTHING;

        /* 0x800370D0: extend from whichever is later, then add 30 seconds. */
        if ((u32)w->level_time < (u32)*slot)
            *slot = *slot + Q2_ITEM_POWERUP_TICKS;
        else
            *slot = w->level_time + Q2_ITEM_POWERUP_TICKS;
        took = true;
        break;
    }

    case Q2_ITEMFX_KEY:
        /* 0x800371C0 and the eleven like it: one OR, never refused. */
        inv->flags |= d->key_bit;
        took = true;
        break;

    case Q2_ITEMFX_NONE:
    default:
        return Q2_TOUCH_NOTHING;
    }

    /* 0x800372E0: the sound plays at the PLAYER's position, not the item's. */
    q2_ent_sound_at(&w->events, (q2_ent_sound)d->sound, pos);

    if (!took)
        return Q2_TOUCH_NOTHING;

    /* 0x800372F0: the HUD's caption. */
    inv->last_item       = (u8)effect;
    inv->item_name_until = w->level_time + Q2_ITEM_CAPTION_TICKS;

    return weaponish ? Q2_TOUCH_WEAPON : Q2_TOUCH_COLLECTED;
}

/* ------------------------------------------------------------------------- */
/* Spawning                                                                   */
/* ------------------------------------------------------------------------- */
q2_entity *q2_item_spawn(q2_entity_set *set, const q2_pop_place *place,
                         const q2_item_table *table, s16 model_offset)
{
    const q2_item_def *def;
    q2_entity *e;
    u32 i;

    if (!set || !place)
        return NULL;

    if (!table)
        table = q2_item_table_builtin();

    /* 0x800599F0: the scan, and a place id it does not name spawns nothing. */
    def = q2_item_find(table, (s32)place->id);
    if (!def)
        return NULL;

    e = q2_entity_alloc(set);
    if (!e)
        return NULL;

    e->think    = q2_item_think;
    e->kind     = Q2_ENT_KIND_ITEM;      /* 0x80059A60 writes 46 */
    e->place_id = place->id;
    e->flags    = def->flags;
    e->effect   = def->effect;
    e->def      = def;

    memcpy(e->model, def->model, sizeof(e->model));
    for (i = 0; i < Q2_ITEM_EXTRA_MAX; i++)
        e->clip[i] = def->extra[i];
    e->clip_count = def->extra_count;

    /*
     * 0x80051054 copies the place record straight in, then 0x80051068 and
     * 0x800510A4 raise y by 286 and 30. +Y is down, so both subtract. The
     * engine follows this with a hull query that drops the entity onto the
     * surface below (0x80044F54 against the SecondaryCol context); that part is
     * not reproduced yet, so an item on a slope sits where the record put it
     * rather than on the floor.
     */
    e->pos[0] = place->x;
    e->pos[1] = place->y - Q2_ITEM_SPAWN_LIFT;
    e->pos[2] = place->z;

    /* 0x80058930: the low twelve bits of the place record's halfword are the
     * heading. Bits 12..15 are something else and are left alone. */
    e->angles[1] = (s32)(place->unk & 0x0FFFu);

    /*
     * 0x800588F0: the draw origin is the position lowered by 286 and raised
     * again by the model's own bias. Net against the place record it is
     * `place.y - 30 - model_offset`, the 286s cancelling.
     */
    e->model_offset = model_offset;
    e->origin[0]    = e->pos[0];
    e->origin[1]    = e->pos[1] + Q2_EYE_BASE - model_offset;
    e->origin[2]    = e->pos[2];

    /* 0x80058908: the spawn origin keeps the same lowering without the bias. */
    e->spawn_origin[0] = e->pos[0];
    e->spawn_origin[1] = e->pos[1] + Q2_EYE_BASE;
    e->spawn_origin[2] = e->pos[2];

    /* The box is about the POSITION, not the draw origin (0x8005A9DC). */
    q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);

    /* 0x80059A98: a materialising item starts at nothing. */
    e->scale = (def->flags & Q2_ITEM_MATERIALISE) ? 0 : (s16)Q2_ONE_12;

    /* 0x80059AB4: the countdown a dropped item would use. Set at spawn on every
     * item, but only read when Q2_ITEM_TIMED is up. */
    e->remove_in = Q2_ITEM_DROP_LIFE;

    /* 0x80059AC4: bit 1 of the render word comes off unless the item spins. */
    if (!(def->flags & Q2_ITEM_SPIN))
        e->render_flags &= ~2u;

    /* The engine gets the clip length from the CastList node. Without the bank
     * open there is nothing to wrap against, and the frame simply holds — which
     * is what Q2_ITEM_NO_ANIM does anyway. */
    e->clip_length = 0;

    return e;
}

q2_result q2_item_spawn_zone(q2_entity_set *set, const q2_population *pop,
                             int zone, const q2_item_table *table,
                             q2_item_spawn_stats *stats)
{
    q2_item_spawn_stats local;
    u32 gi;

    if (!set || !pop)
        return Q2_ERR_INVALID_ARG;

    memset(&local, 0, sizeof(local));

    if (!table)
        table = q2_item_table_builtin();

    for (gi = 0; gi < pop->group_count; gi++) {
        q2_pop_group g;
        int claims;
        u32 slot;

        if (!q2_pop_get_group(pop, gi, &g))
            continue;

        /* The one case the disc settles by itself — see item.h. */
        claims = q2_pop_group_zone(&g);
        if (zone >= 0 && claims >= 0 && claims != zone) {
            local.other_zone++;
            continue;
        }

        /* Every group can carry a place list, the path group included: the
         * spawn and place lists are independent. */
        for (slot = 0; ; slot++) {
            q2_pop_place place;
            q2_entity *e;

            if (!q2_pop_get_place(pop, &g, slot, &place))
                break;

            local.places++;

            e = q2_item_spawn(set, &place, table, 0);
            if (!e) {
                if (q2_item_find(table, (s32)place.id))
                    local.no_memory++;
                else
                    local.no_def++;
                continue;
            }

            local.spawned++;
            if (e->effect == 0)
                local.scenery++;
            else if (!q2_item_effect_is_live(table, e->effect))
                local.inert++;
        }
    }

    if (stats)
        *stats = local;

    return local.no_memory ? Q2_ERR_NO_MEMORY : Q2_OK;
}

q2_result q2_item_spawn_all(q2_entity_set *set, const q2_population *pop,
                            const q2_item_table *table,
                            q2_item_spawn_stats *stats)
{
    return q2_item_spawn_zone(set, pop, -1, table, stats);
}

/* ------------------------------------------------------------------------- */
/* The think                                                                  */
/* ------------------------------------------------------------------------- */
s32 q2_item_glow_pulse(s32 level_time)
{
    /*
     * 0x80059550: `table[(time << 6) & 0x3FC0]` reads a 256-entry signed table
     * with a 2-byte stride, i.e. entry `time & 0xFF`, then `(v + 4096) >> 4`.
     * The port's sine is indexed on the 4096-step circle, so the same 256 steps
     * are `(time & 0xFF) * 16`.
     */
    s32 v = q2_sin12((level_time & 0xFF) * 16);
    return (v + Q2_ONE_12) >> 4;
}

void q2_item_shrink_think(q2_entity *e, q2_entity_world *w)
{
    if (!e || !w)
        return;

    /* 0x8005B368: scale falls by 16 per tick and the entity goes when it hits
     * zero. */
    e->scale = (s16)(e->scale - Q2_ITEM_SHRINK_RATE * w->dt);
    if (e->scale <= 0)
        q2_entity_remove(e);
}

/* The collected path: 0x800598CC (freed or respawned) and 0x8005998C (kept). */
static void item_collected(q2_entity *e, q2_entity_world *w, u32 player,
                           bool keep)
{
    u32 i;

    /* 0x8005B6C0: the pickup particle burst, in both branches. */
    q2_ent_burst_at(&w->events, e->origin, e->glow, e->model_index);

    e->hidden = true;

    if (keep)
        return;

    if (w->deathmatch && !(e->flags & Q2_ITEM_TIMED)) {
        /* 0x800598FC: mega health waits a great deal longer than anything
         * else, and it is selected by the effect index rather than a flag. */
        e->respawn_at = (e->effect == 32) ? Q2_ITEM_RESPAWN_MEGA
                                          : Q2_ITEM_RESPAWN_TICKS;
        for (i = 0; i < Q2_MAX_PLAYERS; i++)
            e->taken[i] = true;
        return;
    }

    (void)player;
    q2_entity_remove(e);
}

void q2_item_think(q2_entity *e, q2_entity_world *w)
{
    u32 flags, p;

    if (!e || !w)
        return;

    flags = e->flags;

    /*
     * 0x80059360: in deathmatch a taken item counts its respawn down and does
     * nothing else. Outside deathmatch this block is skipped entirely, which is
     * why a collected item in single player is freed rather than hidden.
     */
    if (w->deathmatch && e->taken[0]) {
        e->respawn_at -= w->dt;
        if (e->respawn_at > 0)
            return;
        if (w->player_count == 0)
            return;
        for (p = 0; p < Q2_MAX_PLAYERS; p++)
            e->taken[p] = false;
        e->hidden = false;
        return;
    }

    if (flags == 0)
        goto touch_sweep;      /* 0x800593EC */

    /* 0x800593F4: the frame advances unless the record says not to. */
    if (!(flags & Q2_ITEM_NO_ANIM) && e->clip_length > 0) {
        e->frame += w->dt;
        while (e->frame >= e->clip_length)
            e->frame -= e->clip_length;
    }

    /* 0x8005945C: the spin, and the matrix rebuild it implies. */
    if (flags & Q2_ITEM_SPIN) {
        e->angles[1] = (s32)((s16)(e->angles[1] - Q2_ITEM_SPIN_RATE * w->dt));
    }

    /* 0x80059488: materialising. */
    if (flags & Q2_ITEM_MATERIALISE) {
        if (e->scale != (s16)Q2_ONE_12) {
            if (e->scale == 0)
                q2_ent_sound_at(&w->events, Q2_SND_TELEPORT, e->origin);

            e->scale = (s16)(e->scale + Q2_ITEM_SCALE_RATE * w->dt);
            if (e->scale > (s16)Q2_ONE_12) {
                e->scale = (s16)Q2_ONE_12;
                /* 0x800595DC: full size resets the tint to neutral. */
                e->glow[0] = e->glow[1] = e->glow[2] = 127;
            }

            /* 0x80059608: fifteen offsets from the engine's own rand, drawn
             * whether or not anything consumes them, because drawing them is
             * what advances the shared generator. */
            {
                int i;
                for (i = 0; i < Q2_ITEM_SPARKLES * 3; i++)
                    (void)q2_rng_next(&w->rng);
            }
        }
    }

    /* 0x800596B8: the glow light. */
    if (flags & Q2_ITEM_GLOW) {
        s32 pulse = q2_item_glow_pulse(w->level_time);
        s32 lit[3];

        e->glow[0] = e->glow[1] = e->glow[2] = 0;
        if (flags & Q2_ITEM_GLOW_R) e->glow[0] = (u8)(pulse >> 2);
        if (flags & Q2_ITEM_GLOW_G) e->glow[1] = (u8)(pulse >> 2);
        if (flags & Q2_ITEM_GLOW_B) e->glow[2] = (u8)(pulse >> 2);

        lit[0] = e->origin[0] + Q2_ITEM_LIGHT_OFFSET_X;
        lit[1] = e->origin[1] + Q2_ITEM_LIGHT_OFFSET_Y;
        lit[2] = e->origin[2];
        q2_ent_light_at(&w->events, lit, e->glow,
                        pulse + Q2_ITEM_GLOW_RADIUS_BIAS);
    }

    /* 0x800597C8: a dropped item's life. */
    if (flags & Q2_ITEM_TIMED) {
        e->remove_in -= w->dt;
        if (e->remove_in <= 0) {
            e->remove_in = Q2_ITEM_SHRINK_RESET;
            e->think     = q2_item_shrink_think;
            e->scale     = (s16)Q2_ONE_12;
        }
    }

touch_sweep:
    /* 0x80059810: every player, in slot order. */
    for (p = 0; p < w->player_count && p < Q2_MAX_PLAYERS; p++) {
        q2_entity_player *pl = &w->player[p];
        q2_touch_result r;
        bool keep;

        if (!pl->present || !pl->inv)
            continue;
        if (pl->inv->health <= 0)              /* 0x8005983C */
            continue;
        if (!q2_entity_bounds_overlap(e, &pl->ent))
            continue;

        r = q2_item_touch(e->effect, pl->inv, pl->pos, w);

        /*
         * 0x80059878: a weapon-style pickup survives only when the item is not
         * a dropped one, weapons-stay is on and this is deathmatch. Everything
         * else folds back to an ordinary collection.
         */
        keep = false;
        if (r == Q2_TOUCH_WEAPON) {
            if (!(e->flags & Q2_ITEM_TIMED) && w->weapons_stay && w->deathmatch)
                keep = true;
        } else if (r == Q2_TOUCH_NOTHING) {
            continue;                          /* try the next player */
        }

        item_collected(e, w, p, keep);
        return;
    }
}
