/*
 * test_item.c — the item table, the item entity and the touch dispatch.
 *
 * Every number asserted here came out of the executable, so these tests pin the
 * VALUES as well as the behaviour: if a future pass re-transcribes a handler
 * wrongly, a medkit that gives 12 health fails here rather than quietly
 * changing how the game plays.
 *
 * The addresses in the comments are the instructions each expectation was read
 * from, so a failure can be taken straight back to the disassembly.
 */
#include <stdio.h>
#include <string.h>

#include "entity.h"
#include "item.h"
#include "itemtable.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_i(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
static const q2_item_def *find(const char *model)
{
    const q2_item_table *t = q2_item_table_builtin();
    u32 i;

    for (i = 0; i < t->count; i++)
        if (strcmp(t->def[i].model, model) == 0)
            return &t->def[i];
    return NULL;
}

static void test_table(void)
{
    const q2_item_table *t = q2_item_table_builtin();
    const q2_item_def *e;

    printf("item table (0x8009F5CC)\n");

    check_eq_i(t->count, 64, "64 records before the 0xFF terminator");

    /* The first-match scan, and the one duplicate. Place id 6 appears twice and
     * the engine takes the first, so the second record is dead. */
    e = q2_item_find(t, 6);
    check(e != NULL && e == &t->def[14], "place id 6 resolves to the FIRST Bandol P");

    /* Two names use all twelve bytes with no NUL. */
    e = find("Large Medi P");
    check(e != NULL, "Large Medi P present");
    check(e && strlen(e->model) == 12, "a 12-byte name is not truncated");

    /* Weapons, by the effect index each names. */
    check_eq_i(find("Shotgun P")->effect,   2, "Shotgun P  -> effect 2");
    check_eq_i(find("Sshotgun P")->effect,  3, "Sshotgun P -> effect 3");
    check_eq_i(find("Machgun P")->effect,   4, "Machgun P  -> effect 4");
    check_eq_i(find("Chaingun P")->effect,  5, "Chaingun P -> effect 5");
    check_eq_i(find("Glaunch P")->effect,   7, "Glaunch P  -> effect 7");
    check_eq_i(find("Rocketl P")->effect,   8, "Rocketl P  -> effect 8");
    check_eq_i(find("Hyperbl P")->effect,   9, "Hyperbl P  -> effect 9");
    check_eq_i(find("Railgun P")->effect,  10, "Railgun P  -> effect 10");
    check_eq_i(find("Bfg P")->effect,      11, "Bfg P      -> effect 11");

    /*
     * The eight items whose effect index lands on the dispatch's failure exit.
     * This is a load-bearing negative: a port that invented an effect for them
     * would be more complete than the game.
     */
    check(!q2_item_effect_is_live(t, find("Ionripper P")->effect),
          "Ionripper P is inert");
    check(!q2_item_effect_is_live(t, find("Plasmagun P")->effect),
          "Plasmagun P is inert");
    check(!q2_item_effect_is_live(t, find("Flame P")->effect),
          "Flame P is inert");
    check(!q2_item_effect_is_live(t, find("Tesla P")->effect),
          "Tesla P is inert");
    check(!q2_item_effect_is_live(t, find("Discharge P")->effect),
          "Discharge P is inert");
    check(!q2_item_effect_is_live(t, find("Flame Fuel P")->effect),
          "Flame Fuel P is inert");
    check(!q2_item_effect_is_live(t, find("Screen P")->effect),
          "Screen P (power screen) is inert");
    check(!q2_item_effect_is_live(t, find("Stimpack P")->effect),
          "Stimpack P is inert");

    /* And the ones that are not. */
    check(q2_item_effect_is_live(t, find("Shield P")->effect),
          "Shield P (power shield) is live");
    check(q2_item_effect_is_live(t, find("Medi P")->effect), "Medi P is live");

    /* Effect 0 means "cannot be collected", not "collect for nothing". */
    check_eq_i(find("Barrel P")->effect, 0, "Barrel P is scenery");
    check_eq_i(find("Q2LOGO")->effect,   0, "Q2LOGO is scenery");
}

static void test_glow_colours(void)
{
    printf("\nthe glow bits name the keys' own colours\n");

    /*
     * This is the argument that makes bits 4/5/6 CONFIRMED rather than
     * plausible: decoded as one dynamic-light channel each, the twelve key
     * records spell out the colours their names claim.
     */
    check_eq_i(find("Redkey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_R,
               "Redkey glows red");
    check_eq_i(find("Greenkey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_G,
               "Greenkey glows green");
    check_eq_i(find("Bluekey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW_B,
               "Bluekey glows blue");
    check_eq_i(find("Yellowkey P")->flags & Q2_ITEM_GLOW,
               Q2_ITEM_GLOW_R | Q2_ITEM_GLOW_G, "Yellowkey glows red+green");
    check_eq_i(find("Pkeypurp P")->flags & Q2_ITEM_GLOW,
               Q2_ITEM_GLOW_R | Q2_ITEM_GLOW_B, "Pkeypurp glows red+blue");
    check_eq_i(find("Whitekey P")->flags & Q2_ITEM_GLOW, Q2_ITEM_GLOW,
               "Whitekey glows all three");

    /* Bit 3 partitions exactly the twelve key/objective records. */
    {
        const q2_item_table *t = q2_item_table_builtin();
        u32 i, objective = 0, keyfx = 0;

        for (i = 0; i < t->count; i++) {
            if (t->def[i].flags & Q2_ITEM_OBJECTIVE)
                objective++;
            if (q2_item_effect_def(t->def[i].effect)->kind == Q2_ITEMFX_KEY)
                keyfx++;
        }
        check_eq_i(objective, 12, "twelve records carry bit 3");
        check_eq_i(keyfx, 12, "twelve records carry a key effect");
    }

    /* Only Bluekey materialises, only Blackhole suppresses its animation. */
    check(find("Bluekey P")->flags & Q2_ITEM_MATERIALISE,
          "Bluekey materialises");
    check(find("Blackhole P")->flags & Q2_ITEM_NO_ANIM,
          "Blackhole holds frame 0");
    check(find("Blackhole P")->flags & Q2_ITEM_NO_DROP,
          "Blackhole clears the spawner's 0x400 argument");
}

/* ------------------------------------------------------------------------- */
static void world_with_player(q2_entity_world *w, q2_inventory *inv)
{
    static const s32 origin[3] = { 0, 0, 0 };

    q2_entity_world_init(w);
    q2_inventory_init(inv);
    q2_entity_world_add_player(w, 0, inv, origin);
    w->dt = 12;                 /* one nominal logic step */
}

static void test_ammo(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nammo boxes (0x8003696C onward)\n");

    world_with_player(&w, &inv);

    r = q2_item_touch(18, &inv, inv.health ? w.player[0].pos : NULL, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED, "shells box is collected");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "shells box gives 10 in SP");

    inv.ammo[Q2_AMMO_SHELLS] = 100;      /* the base tier's cap */
    r = q2_item_touch(18, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full player leaves the shells box");

    /* Deathmatch doubles the ammo boxes. */
    world_with_player(&w, &inv);
    w.deathmatch = true;
    q2_item_touch(18, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 20, "shells box gives 20 in DM");

    world_with_player(&w, &inv);
    q2_item_touch(19, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 50, "bullets box gives 50");
    q2_item_touch(21, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_ROCKETS], 5, "rockets box gives 5");
    q2_item_touch(22, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 50, "cells box gives 50");
    q2_item_touch(23, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS], 10, "slugs box gives 10");

    /* 0x80036A90: the grenade box is also the hand grenade weapon. */
    q2_item_touch(20, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_GRENADES], 5, "grenade box gives 5");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_HAND_GRENADE),
          "the grenade box also grants the hand grenade");
}

static void test_weapons(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nweapon pickups (0x800363BC onward)\n");

    world_with_player(&w, &inv);
    r = q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_WEAPON, "a weapon returns 1, not 0");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_SHOTGUN), "shotgun granted");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "shotgun brings 10 shells");
    check_eq_i(inv.current_weapon, Q2_WEAPON_SHOTGUN,
               "the blaster switches away (0x80037E84)");

    /* Picking it up again with a better weapon out must NOT switch back. */
    q2_item_touch(11, &inv, w.player[0].pos, &w);      /* the BFG */
    check_eq_i(inv.current_weapon, Q2_WEAPON_SHOTGUN,
               "a second pickup does not switch while off the blaster");

    /* 0x8003668C / 0x800366D0: the launcher grants hand grenades too. */
    world_with_player(&w, &inv);
    q2_item_touch(7, &inv, w.player[0].pos, &w);
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_GRENADE_LAUNCHER),
          "launcher granted");
    check(q2_inventory_has_weapon(&inv, Q2_WEAPON_HAND_GRENADE),
          "and the hand grenade with it");
    check_eq_i(inv.current_weapon, Q2_WEAPON_GRENADE_LAUNCHER,
               "and the launcher is selected");

    /* 0x800363C8: INFINITE AMMO grants the type's capacity instead. */
    world_with_player(&w, &inv);
    w.cheats = Q2_CHEAT_INFINITE_AMMO;
    q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 100,
               "INFINITE AMMO fills the shotgun's shells to capacity");

    /* 0x80037E4C: only deathmatch AND weapons-stay refuses a second pickup. */
    world_with_player(&w, &inv);
    w.deathmatch = true;
    w.weapons_stay = true;
    q2_item_touch(2, &inv, w.player[0].pos, &w);
    r = q2_item_touch(2, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING,
               "weapons-stay refuses a weapon already held");
}

static void test_armour(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\narmour (0x80036C14 / 0x80037338)\n");

    /* Jacket first: base 25, cap 50. */
    world_with_player(&w, &inv);
    q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 25, "jacket armour gives its base 25");
    check_eq_i(inv.armour_class, Q2_ARMOUR_JACKET, "and sets the class");
    check(inv.flags & Q2_INV_ARMOUR_JACKET, "and its flag bit");

    /* A second jacket tops up to the class cap and then refuses. */
    q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 50, "a second jacket reaches the 50 cap");
    r = q2_item_touch(28, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full jacket refuses (0x80036D40)");

    /*
     * Body armour over a full jacket: the old 50 converts down by
     * jacket.normal / body.normal = 1229/3277, in two-bit fixed point, and the
     * body base is added. (1229<<2)/3277 == 1, so 50 * 1 >> 2 == 12.
     */
    q2_item_touch(26, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour_class, Q2_ARMOUR_BODY, "body armour upgrades the class");
    check_eq_i(inv.armour, 112, "and salvages the jacket into 100 + 12");

    /* 0x80036CB0: combat armour has no full test at all. */
    world_with_player(&w, &inv);
    inv.armour_class = Q2_ARMOUR_COMBAT;
    inv.armour = 100;                    /* the combat cap */
    inv.flags |= Q2_INV_ARMOUR_COMBAT;
    r = q2_item_touch(27, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED,
               "combat armour is collected even at the cap");
    check_eq_i(inv.armour, 100, "and clamps rather than overflowing");

    /* Shards: +2 with a 999 cap, and they establish jacket armour. */
    world_with_player(&w, &inv);
    q2_item_touch(29, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 2, "a shard gives 2");
    check(inv.flags & Q2_INV_ARMOUR_JACKET,
          "a shard with nothing held becomes jacket armour");
    inv.armour = 998;
    q2_item_touch(29, &inv, w.player[0].pos, &w);
    check_eq_i(inv.armour, 999, "and clamps at the literal 999");
}

static void test_health(void)
{
    q2_entity_world w;
    q2_inventory inv;
    q2_touch_result r;

    printf("\nhealth (0x80036E20 onward)\n");

    world_with_player(&w, &inv);
    inv.health = 50;
    q2_item_touch(34, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 60, "a medkit gives 10");
    q2_item_touch(35, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 85, "a large medkit gives 25");
    q2_item_touch(35, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health, 100, "and clamps at the player's max");
    r = q2_item_touch(34, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_NOTHING, "a full player leaves the medkit");

    /* 0x80036E20: 200 and 100 are immediates, so mega health always applies. */
    r = q2_item_touch(32, &inv, w.player[0].pos, &w);
    check_eq_i(r, Q2_TOUCH_COLLECTED, "mega health is never refused");
    check_eq_i(inv.health, 200, "and overheals to the literal 200");
    check(inv.flags & Q2_INV_MEGA_HEALTH, "and raises its flag");

    /* And it bleeds back down, one point per 1500 ticks. */
    {
        s32 t = w.level_time;
        inv.mega_health_next = t + Q2_ITEM_MEGA_DECAY_TICKS;
        q2_item_mega_health_tick(&inv, t + Q2_ITEM_MEGA_DECAY_TICKS);
        check_eq_i(inv.health, 199, "mega health decays a point");
        inv.health = 100;
        q2_item_mega_health_tick(&inv, t + 2 * Q2_ITEM_MEGA_DECAY_TICKS);
        check(!(inv.flags & Q2_INV_MEGA_HEALTH),
              "and the flag drops once back at the cap");
    }

    /* 0x80036E64: adrenaline raises the max by one and tops health up to it. */
    world_with_player(&w, &inv);
    inv.health = 40;
    q2_item_touch(33, &inv, w.player[0].pos, &w);
    check_eq_i(inv.health_max, 101, "adrenaline raises max health to 101");
    check_eq_i(inv.health, 101, "and fills health to the new max");
}

static void test_capacity_and_powerups(void)
{
    q2_entity_world w;
    q2_inventory inv;

    printf("\ncapacity and powerups\n");

    /* 0x8003701C: bandolier, and a pack is not downgraded by one. */
    world_with_player(&w, &inv);
    q2_item_touch(38, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_BANDOLIER, "bandolier raises the tier");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "and brings 10 shells");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS], 50, "and 50 bullets");
    check_eq_i(q2_inventory_ammo_max(&inv, Q2_AMMO_SHELLS), 150,
               "the shells cap rises to 150");

    q2_item_touch(37, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_PACK, "the pack raises it again");
    check_eq_i(q2_inventory_ammo_max(&inv, Q2_AMMO_SHELLS), 200,
               "the shells cap rises to 200");
    q2_item_touch(38, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo_tier, Q2_AMMO_TIER_PACK,
               "a bandolier does not downgrade a pack (0x80037028)");

    /* 0x80036F10: the pack tops up all six types. */
    world_with_player(&w, &inv);
    q2_item_touch(37, &inv, w.player[0].pos, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS],   10, "pack: 10 shells");
    check_eq_i(inv.ammo[Q2_AMMO_BULLETS],  50, "pack: 50 bullets");
    check_eq_i(inv.ammo[Q2_AMMO_GRENADES],  5, "pack: 5 grenades");
    check_eq_i(inv.ammo[Q2_AMMO_ROCKETS],   5, "pack: 5 rockets");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS],    50, "pack: 50 cells");
    check_eq_i(inv.ammo[Q2_AMMO_SLUGS],    10, "pack: 10 slugs");

    /* 0x800370D0: 30 seconds, extended from whichever is later. */
    world_with_player(&w, &inv);
    w.level_time = 1000;
    q2_item_touch(40, &inv, w.player[0].pos, &w);
    check_eq_i(inv.quad_until, 1000 + 9000, "quad runs 9000 ticks");
    check(q2_item_powerup_active(&inv, Q2_POWERUP_QUAD, 5000),
          "and is active in the middle");
    check(!q2_item_powerup_active(&inv, Q2_POWERUP_QUAD, 10001),
          "and lapses at the end");
    q2_item_touch(40, &inv, w.player[0].pos, &w);
    check_eq_i(inv.quad_until, 1000 + 18000, "a second quad stacks");

    q2_item_touch(41, &inv, w.player[0].pos, &w);
    check_eq_i(inv.invuln_until, 1000 + 9000, "invulnerability runs 9000");
    q2_item_touch(42, &inv, w.player[0].pos, &w);
    check_eq_i(inv.enviro_until, 1000 + 9000, "the enviro suit runs 9000");
    q2_item_touch(43, &inv, w.player[0].pos, &w);
    check_eq_i(inv.breather_until, 1000 + 9000, "the rebreather runs 9000");

    /* 0x800370BC: the silencer gives shots, not time. */
    q2_item_touch(39, &inv, w.player[0].pos, &w);
    check_eq_i(inv.silencer_shots, 30, "the silencer gives 30 shots");

    /* 0x80036DB4: the shield's bit goes up and cells come with it. */
    world_with_player(&w, &inv);
    q2_item_touch(30, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_INV_POWER_SHIELD, "the power shield raises its bit");
    check_eq_i(inv.ammo[Q2_AMMO_CELLS], 50, "and brings 50 cells");
    /* 0x00018000 is the pair combat.h's damage path tests; only the shield's
     * half can ever be set, because the screen has no handler. */
    check_eq_i(inv.flags & 0x00018000u, Q2_INV_POWER_SHIELD,
               "which is one of the two bits the damage path tests");
}

static void test_keys(void)
{
    q2_entity_world w;
    q2_inventory inv;
    u32 all;

    printf("\nkeys (0x800371C0 onward)\n");

    world_with_player(&w, &inv);

    q2_item_touch(44, &inv, w.player[0].pos, &w);
    check_eq_i(inv.flags & Q2_KEY_MASK, Q2_KEY_BLUE, "effect 44 is the blue key");
    q2_item_touch(56, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_KEY_WHITE, "effect 56 is the white key");

    /* 0x80037238 / 0x80037250: 50 is the PURPLE pyramid key and 51 the RED —
     * the pair is crossed relative to the table order, which is exactly the
     * sort of thing that has to be read rather than assumed. */
    world_with_player(&w, &inv);
    q2_item_touch(50, &inv, w.player[0].pos, &w);
    check_eq_i(inv.flags & Q2_KEY_MASK, Q2_KEY_PYRAMID_PURPLE,
               "effect 50 is the purple pyramid key");
    q2_item_touch(51, &inv, w.player[0].pos, &w);
    check(inv.flags & Q2_KEY_PYRAMID_RED,
          "effect 51 is the red pyramid key");

    /* All twelve fit the mask the script sees, and nothing else does. */
    world_with_player(&w, &inv);
    {
        u32 fx;
        for (fx = Q2_ITEM_EFFECT_FIRST; fx <= (u32)Q2_ITEM_EFFECT_LAST; fx++)
            if (q2_item_effect_def(fx)->kind == Q2_ITEMFX_KEY)
                q2_item_touch(fx, &inv, w.player[0].pos, &w);
    }
    all = q2_inventory_script_keys(&inv);
    check_eq_i(all, Q2_KEY_MASK, "all twelve keys fill the script's mask");
    check_eq_i(inv.flags & ~Q2_KEY_MASK, 0u,
               "and nothing outside it is touched");
}

/* ------------------------------------------------------------------------- */
static void test_spawn_and_think(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;

    printf("\nspawn (0x800599DC) and think (0x80059330)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);

    memset(&place, 0, sizeof(place));
    place.x = 1000;
    place.y = 2000;
    place.z = 3000;
    place.unk = 0x9400;          /* bits 12..15 set, plus a heading of 0x400 */
    place.id = 39;               /* Shotgun P */

    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check(e != NULL, "a known place id spawns");
    check_eq_i(e->effect, 2, "and carries the table's effect index");
    check_eq_i(e->kind, Q2_ENT_KIND_ITEM, "and the item kind, 46");
    check(strcmp(e->model, "Shotgun P") == 0, "and the table's model name");

    /* 0x80058930: the heading is the low twelve bits only. */
    check_eq_i(e->angles[1], 0x400, "the yaw is place->unk & 0xFFF");

    /*
     * 0x80051068 / 0x800510A4 raise the record by 286 and then 30 before the
     * hull query; 0x800588F8 lowers the draw origin by 286 again. So the stored
     * position is 316 above the record and the draw origin is 30 above it.
     */
    check_eq_i(e->pos[1], 2000 - 316, "the position is the record raised by 316");
    check_eq_i(e->origin[1], 2000 - 30, "and the draw origin 30 above it");

    /* And the touch box is a 286-cube about the POSITION, not the origin. */
    check_eq_i(e->bounds_min[1], e->pos[1] - 286, "the box is about the position");
    check_eq_i(e->bounds_max[1], e->pos[1] + 286, "and is 572 tall");

    check_eq_i(e->scale, 4096, "a non-materialising item spawns full size");

    /* An id no record names spawns nothing at all. */
    place.id = 22;               /* the one gap inside the used range */
    check(q2_item_spawn(&set, &place, NULL, 0, NULL) == NULL,
          "an unnamed place id spawns nothing");

    /* 0x8005947C: the spin is -3 per tick of the level clock. */
    {
        s32 before = e->angles[1];
        w.dt = 6;
        e->think(e, &w);
        check_eq_i(e->angles[1], before - 3 * 6, "the spin is -3 * dt");
    }

    /* 0x80059488: a materialising item ramps up and announces itself. */
    {
        q2_entity *m;

        place.id = 45;           /* Bluekey P — the only record with the bit */
        m = q2_item_spawn(&set, &place, NULL, 0, NULL);
        check(m != NULL, "Bluekey P spawns");
        check_eq_i(m->scale, 0, "and starts at nothing");

        q2_ent_events_clear(&w.events);
        w.dt = 12;
        m->think(m, &w);
        check_eq_i(m->scale, 2 * 12, "and grows by 2 * dt");
        check(w.events.count > 0, "and plays a sound on the first tick");
        check_eq_i(w.events.e[0].sound, Q2_SND_TELEPORT,
                   "which is msc_tele1");

        /* Its glow is blue, and the light block writes only that channel. */
        check_eq_i(m->glow[0], 0, "no red");
        check_eq_i(m->glow[1], 0, "no green");
        check(m->glow[2] > 0, "and a blue channel that is lit");
    }

    q2_entity_set_free(&set);
}

static void test_touch_sweep(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;
    s32 pos[3] = { 0, 0, 0 };

    printf("\nthe touch sweep (0x80059810)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);

    memset(&place, 0, sizeof(place));
    place.id = 27;               /* Shells P */
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check(e != NULL, "Shells P spawns");

    /* Out of reach: nothing happens. */
    pos[0] = 100000;
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check(e->in_use, "an item out of reach survives");
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 0, "and gives nothing");

    /* In reach, single player: collected and freed. */
    pos[0] = 0;
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 10, "walking over it collects it");
    check(!e->in_use, "and in single player the entity is freed");

    /* A dead player collects nothing (0x8005983C). */
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    inv.health = 0;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check_eq_i(inv.ammo[Q2_AMMO_SHELLS], 0, "a dead player collects nothing");
    check(e->in_use, "and the item stays");

    /* Deathmatch: hidden, then back after 9000 ticks (0x800598FC). */
    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    w.deathmatch = true;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    q2_entity_world_move_player(&w, 0, pos);
    e->think(e, &w);
    check(e->in_use, "in deathmatch the entity survives");
    check(e->taken[0], "but is taken");
    check_eq_i(e->respawn_at, Q2_ITEM_RESPAWN_TICKS, "with a 9000-tick timer");

    /* Run the clock down, with the player stepped off it — otherwise it is
     * collected again the tick after it returns, which is also correct and
     * would hide the thing being tested. */
    {
        int i;
        pos[0] = 100000;
        q2_entity_world_move_player(&w, 0, pos);
        w.dt = 300;
        for (i = 0; i < 31; i++)
            e->think(e, &w);
        check(!e->taken[0], "and comes back when the timer expires");
        check(!e->hidden, "and is visible again");
    }

    q2_entity_set_free(&set);
}

static void test_timed_removal(void)
{
    q2_entity_set set;
    q2_entity_world w;
    q2_inventory inv;
    q2_pop_place place;
    q2_entity *e;
    s32 away[3] = { 100000, 0, 0 };
    int i;

    printf("\ntimed removal (0x800597C8 / 0x8005B358)\n");

    memset(&set, 0, sizeof(set));
    world_with_player(&w, &inv);
    q2_entity_world_move_player(&w, 0, away);   /* nobody standing on it */

    memset(&place, 0, sizeof(place));
    place.id = 27;
    e = q2_item_spawn(&set, &place, NULL, 0, NULL);
    check_eq_i(e->remove_in, Q2_ITEM_DROP_LIFE,
               "an item spawns with a 1500-tick life");

    /* The flag is never set on disc; the runtime sets it on a dropped item. */
    e->flags |= Q2_ITEM_TIMED;
    w.dt = 300;
    for (i = 0; i < 20 && e->think == q2_item_think; i++)
        e->think(e, &w);
    check_eq_i(i, 5, "the 1500-tick life expires after five 300-tick steps");
    check(e->think == q2_item_shrink_think,
          "the countdown switches the think to the shrink");
    check_eq_i(e->scale, 4096, "and resets the scale to full");

    /* 4096 / 16 == 256 ticks of shrink, so a 64-tick step takes four. */
    w.dt = 64;
    for (i = 0; i < 8 && e->in_use; i++)
        e->think(e, &w);
    check(!e->in_use, "which then shrinks it away");
    check_eq_i(i, 4, "in 4096 / (16 * 64) steps");

    q2_entity_set_free(&set);
}

static void test_entity_set(void)
{
    q2_entity_set set;
    q2_entity *a, *b, *c;

    printf("\nthe entity set\n");

    memset(&set, 0, sizeof(set));

    a = q2_entity_alloc(&set);
    b = q2_entity_alloc(&set);
    check(a && b && a != b, "two allocations are distinct");
    check_eq_i(set.count, 2, "and both are live");

    q2_entity_remove(a);
    c = q2_entity_alloc(&set);
    check(c == a, "a freed slot is reused (0x8006C098)");
    check_eq_i(set.count, 2, "without growing the set");

    /* An entity with no think is skipped rather than crashing the sweep. */
    {
        q2_entity_world w;
        q2_inventory inv;
        world_with_player(&w, &inv);
        check_eq_i(q2_entity_run(&set, &w), 0, "thinkless entities run nothing");
        check_eq_i(w.level_time, w.dt, "but the clock still advances");
    }

    q2_entity_set_free(&set);
}

/* ------------------------------------------------------------------------- */
/*
 * Which groups a zone load spawns.
 *
 * A group is spawned because a script SELECTED it by name (0x80056C60 sets bit
 * 0 of its flags word; the pass at 0x80056D68 runs only what has bit 0 set and
 * bit 1 clear). Those scripts live in the level modules, which this port does
 * not run — so the port's rule is the one thing the disc settles by itself: a
 * group whose NAME claims a zone belongs to that zone and to no other.
 *
 * The population is built by hand rather than read off a disc, so this runs in
 * the self-contained suite and pins the rule rather than one map's data.
 */
static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void put_group(u8 *buf, u32 at, const char *name, u32 place_offset)
{
    memset(buf + at, 0, Q2_POP_GROUP_SIZE);
    memcpy(buf + at, name, strlen(name));
    put_u32(buf + at + 0x0C, 0);              /* no spawn list */
    put_u32(buf + at + 0x10, place_offset);
}

/* `count` places then the list's own 0xFFFFFFFF terminator. */
static void put_places(u8 *buf, u32 at, u32 count, u16 id)
{
    u32 i;
    for (i = 0; i < count; i++) {
        u8 *p = buf + at + i * Q2_POP_PLACE_SIZE;
        memset(p, 0, Q2_POP_PLACE_SIZE);
        p[14] = (u8)id;
        p[15] = (u8)(id >> 8);
    }
    put_u32(buf + at + count * Q2_POP_PLACE_SIZE, Q2_POP_TERM_FFFF);
}

static u32 spawn_count(const q2_population *pop, int zone, u32 *other_zone)
{
    q2_entity_set set;
    q2_item_spawn_stats st;
    u32 n;

    memset(&set, 0, sizeof(set));
    q2_item_spawn_zone(&set, pop, zone, NULL, NULL, &st);
    n = set.count;
    if (other_zone)
        *other_zone = st.other_zone;
    q2_entity_set_free(&set);
    return n;
}

static void test_zone_groups(void)
{
    /* Three groups: two named after zones, one named after a category. */
    enum { P0 = 96, P1 = P0 + 2 * 16 + 4, P2 = P1 + 2 * 16 + 4,
           SIZE = P2 + 1 * 16 + 4 };
    u8 buf[SIZE];
    q2_population pop;
    q2_pop_group g;
    u32 skipped;

    printf("zone groups\n");

    memset(buf, 0, sizeof(buf));
    put_group(buf,  0, "Zone0",   P0);
    put_group(buf, 24, "Zone1",   P1);
    put_group(buf, 48, "Weapons", P2);
    put_u32(buf + 72, 0);                     /* the group table's terminator */
    put_places(buf, P0, 2, 27);               /* Shells P */
    put_places(buf, P1, 2, 27);
    put_places(buf, P2, 1, 27);

    pop.data        = buf;
    pop.size        = SIZE;
    pop.group_count = 3;

    /* The name rule itself. */
    memset(&g, 0, sizeof(g));
    snprintf(g.name, sizeof(g.name), "%s", "Zone0");
    check_eq_i(q2_pop_group_zone(&g), 0, "'Zone0' names zone 0");
    snprintf(g.name, sizeof(g.name), "%s", "Zone4");
    check_eq_i(q2_pop_group_zone(&g), 4, "'Zone4' names zone 4");
    /* A suffix is a batch WITHIN that zone, so the digits still decide. */
    snprintf(g.name, sizeof(g.name), "%s", "Zone1bob");
    check_eq_i(q2_pop_group_zone(&g), 1, "'Zone1bob' names zone 1");
    snprintf(g.name, sizeof(g.name), "%s", "Zone2_1");
    check_eq_i(q2_pop_group_zone(&g), 2, "'Zone2_1' names zone 2");
    snprintf(g.name, sizeof(g.name), "%s", "zone3");
    check_eq_i(q2_pop_group_zone(&g), 3, "the match is case-insensitive");
    /* The deathmatch maps' groups, which partition by category instead. */
    snprintf(g.name, sizeof(g.name), "%s", "Weapons");
    check_eq_i(q2_pop_group_zone(&g), -1, "'Weapons' names no zone");
    snprintf(g.name, sizeof(g.name), "%s", "PathCorner");
    check_eq_i(q2_pop_group_zone(&g), -1, "'PathCorner' names no zone");
    snprintf(g.name, sizeof(g.name), "%s", "Zone");
    check_eq_i(q2_pop_group_zone(&g), -1, "'Zone' with no digits names none");

    /* And what that does to a spawn. A zone gets its own group and every group
     * that claims none — never another zone's. */
    check_eq_i(spawn_count(&pop, 0, &skipped), 3, "zone 0 spawns its 2 plus the 1");
    check_eq_i(skipped, 1, "zone 0 skips one group");
    check_eq_i(spawn_count(&pop, 1, &skipped), 3, "zone 1 spawns its 2 plus the 1");
    check_eq_i(skipped, 1, "zone 1 skips one group");
    check_eq_i(spawn_count(&pop, 2, &skipped), 1,
               "a zone with no group of its own still gets the unclaimed one");
    check_eq_i(skipped, 2, "zone 2 skips both zone-named groups");

    /* -1 is the census: every group, whichever zone it names. */
    check_eq_i(spawn_count(&pop, -1, &skipped), 5, "zone -1 spawns every group");
    check_eq_i(skipped, 0, "zone -1 skips nothing");
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    /* Unbuffered, so a crash mid-suite still shows which section reached the
     * console. Cheap, and it has already paid for itself once. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_item\n\n");

    test_table();
    test_glow_colours();
    test_ammo();
    test_weapons();
    test_armour();
    test_health();
    test_capacity_and_powerups();
    test_keys();
    test_spawn_and_think();
    test_touch_sweep();
    test_timed_removal();
    test_entity_set();
    test_zone_groups();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
