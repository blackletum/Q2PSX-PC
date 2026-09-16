/*
 * simcombat.c — where the reconstructed combat meets the reconstructed world.
 *
 * Everything here is glue, and deliberately so: weapon.c holds what the fire
 * functions do, combat.c holds what the damage function does, projectile.c
 * holds what a rocket is, and this file is the only place that knows all three
 * plus the collision hull. Keeping it separate from sim.c also keeps the
 * movement code readable, which is the harder of the two to follow.
 *
 * The one decision made here rather than read: a shot is traced against the
 * SecondaryCol hull the player moves in, because that is the only hull the port
 * can trace a segment through today. The console traces bullets against
 * PrimaryColl (0x80053974 takes the primary context) and only movement against
 * the secondary. The difference is the player's own 286-unit erosion, so a
 * bullet fired flat along a wall stops 286 units early. It is called out here
 * rather than hidden because it is a real divergence, not a rounding one.
 */
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "explosive.h"
#include "lighting.h"     /* q2_light_glow_fade — 0x80075E14 */
#include "modelent.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */
void q2_sim_attach_effects(q2_sim *sim, const q2_fx_tables *tab, u32 seed)
{
    if (!sim)
        return;

    q2_fx_world_init(&sim->fx, tab);
    q2_rng_seed(&sim->fx_rng, seed);
    sim->fx_ready = (tab != NULL && tab->loaded);
}

bool q2_sim_attach_glint(q2_sim *sim, const q2_common_file *common)
{
    const dat_chunk *chunk;

    if (!sim)
        return false;

    memset(&sim->glint, 0, sizeof(sim->glint));

    if (!common)
        return false;

    chunk = common->chunk[Q2_COMMON_GLINT_MOD];
    if (!chunk || !chunk->data)
        return false;

    sim->glint.ready = q2_fx_glint_mesh_decode(&sim->glint.mesh,
                                               chunk->data, chunk->size);
    if (!sim->glint.ready)
        return false;

    /*
     * ASK THE LEVEL SCRIPT whether it turns a glint on, and take its numbers.
     *
     * The flag, the band count and the phase are all written by `LevelBin`, and
     * this port does not execute one — but it can read it, which is enough. A
     * map whose script raises no glint gets none; a map whose script uses
     * different numbers gets those rather than BIGGUN's.
     */
    {
        const dat_chunk *lb = common->chunk[Q2_COMMON_LEVEL_BIN];
        q2_fx_glint_script script;
        u32 i;

        if (!lb || !lb->data ||
            !q2_fx_glint_scan(&script, lb->data, lb->size)) {
            /* The mesh is loaded and drawable, but nothing turns it on. */
            return true;
        }

        sim->glint.raised     = true;
        sim->glint.band_count = script.band_count ? script.band_count
                                                  : Q2_FX_GLINT_BANDS;
        sim->glint.phase      = script.phase ? script.phase
                                             : Q2_FX_GLINT_PHASE_START;

        /*
         * The band records themselves are the one thing not readable: the
         * script writes them through its import table (effect.h), into memory
         * rather than into any chunk. The port lays them out evenly and says
         * so — it is the only invented quantity left in the effect system, and
         * it moves where the highlights sit, not whether or how they sweep.
         */
        sim->glint.tint[0] = 255;
        sim->glint.tint[1] = 220;
        sim->glint.tint[2] = 160;

        for (i = 0; i < sim->glint.band_count &&
                    i < Q2_FX_GLINT_BANDS_MAX; i++) {
            sim->glint.band[i].angle[1] =
                (s16)((s32)i * Q2_ONE_12 / (s32)sim->glint.band_count);
            sim->glint.band[i].phase  = (u8)(sim->glint.phase - (i & 3u));
            sim->glint.band[i].colour = 0x3AA0DCFFu;
        }
    }

    return true;
}

/*
 * WHICH AREA A BURST BELONGS TO — 0x800686C4, transcribed.
 *
 *     800686D0  bne  a0, zero, 0x80068720   ; a non-zero byte is taken as is
 *     800686E8  jal  0x80044F54             ; else find the cell for the point
 *     80068710  lbu  v0, 32(v0)             ;   in PrimaryColl (0x800C8E90)
 *
 * The third group spawner (0x8002FDFC) runs this on its own area argument at
 * 0x8002FED0, which is why the console's own call sites can pass a literal 0
 * and still land in a live area chain. The other two spawners rely on their
 * CALLERS supplying a resolved byte — the hitscan's is the trace's own area
 * record, `lbu s1, 32(v1)` at 0x80048854.
 *
 * The port stores whatever byte it is given (effect.c) and the group draw culls
 * a group whose area has no screen-change record to drain (effect.c, the
 * `psx_ot_area_bucket` test). Since no collision cell on the disc carries area
 * 0 — `q2psx-inspect coll` reports contents != 0 on 22767 of 22773 nodes —
 * every burst this file raised with a literal 0 was culled in every zone that
 * ships a SortData stream, which is most of them. So the resolve arm lives
 * here, at the call sites, rather than inside effect.c: that module has no
 * collision dependency and the console only runs the arm in one of its three
 * spawners.
 *
 * PrimaryColl, as 0x800686D8 loads, and not the movement hull.
 */
static u8 fx_area_resolve(q2_sim *sim, u8 area, const s32 at[3])
{
    q2_collision *hull;
    q2_coll_node  cell;
    s32           node;

    if (area)                       /* 0x800686D0 */
        return area;
    if (!sim || !at)
        return 0;

    hull = sim->coll_primary_ready ? &sim->coll_primary
         : (sim->coll_ready ? &sim->coll : NULL);
    if (!hull)
        return 0;

    node = q2_coll_find_node(hull, at, -1, true);
    if (node < 0 || !q2_collision_get_node(hull, (u32)node, &cell))
        /* 0x800686F8 branches to 0x8006871C, which returns zero: a point in no
         * cell has no area record, and the caller is left with the byte the
         * draw culls. There is no better answer to invent. */
        return 0;
    /* The console returns the raw byte (0x80068710); the mask is the port's,
     * applied here rather than at every reader, and matches what world.c and
     * the entity draws register. */
    return (u8)(cell.contents & 0x7F);
}

/* A spawn that costs nothing when no tables are attached. `area` is the byte
 * the console's caller would have had in hand; 0 asks for the 0x800686C4
 * lookup above. */
static void fx_at_area(q2_sim *sim, q2_fx_preset_id id, const s32 at[3],
                       u8 area)
{
    if (!sim->fx_ready || !at)
        return;
    q2_fx_spawn(&sim->fx, &sim->fx_rng, id, at,
                fx_area_resolve(sim, area, at));
}

static void fx_at(q2_sim *sim, q2_fx_preset_id id, const s32 at[3])
{
    fx_at_area(sim, id, at, 0);
}

/*
 * Where a hitscan shot leaves its mark.
 *
 * The port DOES have a contact point: `world_fraction_for` already runs the
 * pellet through the hull and hands back the 1.0.12 fraction at which the world
 * stopped it, and the direction carries the range, so `origin + dir * frac` is
 * the impact. The original's own hitscan (0x8004874C) does the same thing — it
 * forms the trace end as origin + dir, takes the clipped point back, and either
 * sprays blood there (0x80048980 -> 0x80048B64) or throws a spark (0x800486EC).
 *
 * A pellet that hit nothing and was stopped by nothing marks nothing: a frac of
 * 4096 with no victim means the shot ran out of range in open air.
 *
 * CORRECTION — the world arm is NOT a spark, and this was the loud half of
 * "all effects are broken". It used to raise Q2_FX_SPARK, whose site
 * (0x8003E0C0) sits inside 0x8003E014, and `xrefs 0x8003E014` returns exactly
 * one caller: 0x8003D4C4, inside the player's per-frame STATE think. No weapon
 * in the executable reaches it. Its ramp is record 0 — (64,64,255), pure blue,
 * additive — so every pellet that struck a wall painted a blue disc, fifteen
 * quads at a time, and a burst of buckshot painted a screenful of them.
 *
 * The real world arm is 0x80048990 -> 0x800489D8, which spawns through the
 * SECOND group spawner (0x8003004C) with grey and dark-red ramps: a smoke
 * puff. See q2_fx_bullet_puff. The address the old note cited for the spark,
 * 0x800486EC, is the EXPLOSION site and is what the missile tick's world arm
 * uses — not the bullet's.
 */
static void fx_hitscan_impact(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], s32 frac, u8 area, s32 victim,
                              const q2_damage_result *dr, s16 damage)
{
    s32 at[3];
    int k;

    if (!sim->fx_ready)
        return;

    for (k = 0; k < 3; k++)
        at[k] = origin[k] + (s32)(((s64)dir[k] * frac) >> Q2_FRAC_12);

    if (victim >= 0) {
        /* Flesh only: armour taking the whole hit is the case the HUD's damage
         * flash also distinguishes. */
        if (!dr || dr->taken > 0)
            fx_at_area(sim, Q2_FX_BLOOD, at, area);
        /*
         * THE SECOND BURST IS GONE, and that is a correction rather than a trim.
         *
         * 0x800596B0 is not a gib burst. It lives inside the ITEM think
         * 0x80059330 — 0x800595AC advances the materialise scale, 0x800595C8
         * clamps it at 4096 — and its ramp is chosen from the ITEM's glow bits
         * at entity+0x44, which is the same word 0x800596B8 tests for the glow
         * light one instruction later. Raising it on a killing blow painted an
         * item's ramp-1 glow at the victim's feet.
         *
         * A kill raises nothing of its own. What the console raises when a
         * body COMES APART is the destruction dispatcher 0x8007CEB4, whose
         * default arm calls ThrowGibs 0x8005A3D4 (0x8007D138), which opens with
         * 0x8005B320 (0x8005A440): the MESH blood spray on ramps 2 and 3 at
         * size 6144, q2_fx_gib_spray in effect.h. That needs the victim's posed
         * model, which this file has no access to. Nothing extra is raised
         * here rather than raising the wrong thing.
         */
        return;
    }

    if (frac < 4096) {
        /*
         * The area byte the console forwards here is `lbu s1, 32(v1)` at
         * 0x80048854 — the TRACE's own area record, not a fresh lookup of the
         * contact point. `world_fraction_for` already ran that trace and now
         * hands the byte back with the fraction, so this is the console's own
         * value at no extra hull cost. A trace that ended on a door or an
         * intact pane has no cell and therefore no area record (q2_trace.node
         * is -1 there); `fx_area_resolve` falls back to the contact point's own
         * cell in that case rather than storing a 0 the group draw would cull.
         */
        if (sim->fx_ready)
            q2_fx_bullet_puff(&sim->fx, &sim->fx_rng, at,
                              fx_area_resolve(sim, area, at));

        /*
         * And the BREAKABLE, tested along the whole shot rather than at its
         * end: the console's sweep runs INSIDE the trace, so a pane in front of
         * the wall is what the shot hits, not the wall behind it.
         */
        q2_sim_breakable_shot(sim, origin, at, damage);
    }
}

/*
 * Which burst a projectile leaves behind.
 *
 * The BFG has a burst of its own, four times the size of any other
 * (0x8004BDBC). Everything else detonates as the missile tick's world arm
 * does: 0x800486EC, ramp 9, two groups.
 *
 * A BOLT USED TO BE MAPPED TO THE BLUE SPARK, on the strength of a comment
 * saying "0x8004D74C reaches the small blue spark rather than the fireball".
 * That is false, and the address does not say what it was read as saying:
 * 0x8004D74C is `jal 0x80048AFC` inside the bolt SPAWNER at 0x8004D70C — the
 * entity allocator, whose result is tested `beq s2, zero` two instructions
 * later and then filled in as a 104-byte missile record. It is not a burst
 * site at all, and the spark's only reachable caller is in the player's state
 * think (see fx_hitscan_impact).
 *
 * SO WHAT DOES A BOLT DO? Measured, not chosen. The missile tick (0x80047C6C)
 * has exactly two burst arms — 0x80048B64 blood when a victim was hit, and the
 * world arm at 0x8004866C which loads ramp 9 and loops `slti v0,s7,2` — and
 * BOTH are gated on bit 3 of the missile record's flag halfword at +0x22
 * (`lhu v0,-54(s3); andi v0,8; beq -> 0x80048700`, at 0x80048624 and
 * 0x80048658). The spawner writes that halfword from its FIFTH argument:
 * `lhu s1, 128(sp)` at 0x8004D714 against its own `addiu sp, sp, -112`, stored
 * by `sh s1, 34(s2)` at 0x8004D7BC.
 *
 * Reading the fifth argument at every call site settles it. `xrefs 0x8004D70C`
 * gives three:
 *
 *     0x8004C134   sw 11, 16(sp)          0b1011 — bit 3 set
 *     0x8004D400   sw 14, 16(sp)          0b1110 — bit 3 set
 *     0x800620EC   sw 11 or 14, 16(sp)    both arms, bit 3 set
 *
 * Every bolt on the disc carries the bit, so every bolt detonates through the
 * world arm — the same orange two-group burst every other missile gets. The
 * blue spark was never any part of it.
 *
 * STILL OWED: the gate ITSELF. This port raises a detonation burst for every
 * kind unconditionally, where the original tests bit 3 per record. It happens
 * to agree for the bolt because the bit is always set there; it will not agree
 * for whatever kind is spawned without it, and that kind has not been looked
 * for.
 */
static q2_fx_preset_id fx_for_projectile(q2_proj_kind kind)
{
    switch (kind) {
    case Q2_PROJ_BFG:  return Q2_FX_BFG_BURST;
    default:           return Q2_FX_EXPLOSION;
    }
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_init(q2_sim *sim)
{
    if (!sim)
        return;

    memset(&sim->combat, 0, sizeof(sim->combat));

    q2_inventory_init(&sim->combat.inv);
    q2_combat_rules_default(&sim->combat.rules);
    q2_projectiles_init(&sim->combat.projectiles);
    q2_rng_seed(&sim->combat.rng, 0x51ED2701u);

    /* A fresh player has the blaster and nothing else, which is what the
     * spawn path at 0x8003D4FC leaves in the weapon fields. */
    sim->combat.weapon_id        = Q2_WID_BLASTER;
    sim->combat.inv.weapons      =
        (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_BLASTER];
    sim->combat.chaingun_bullets = 1;

    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    /*
     * THE KILLER BYTE A PLAYER IS PLACED WITH. 0x8003DDF8 builds the body on a
     * freshly cleared entity (0x8006C18C's 768-byte memset) and then stores 4:
     * 0x8003DE24 `addiu v0, zero, 4` / 0x8003DE34 `sb v0, 222(s1)`. The memset
     * above leaves 0, which is player 0's index, and the refresh carries the
     * byte rather than re-seeding it (combat.h), so without this a deathmatch
     * kill by a creature nobody has hurt (the 0x80057E5C arm, no store) was
     * scored as player 0's suicide.
     *
     * Seeded here rather than through q2_actor_init, which would also move
     * `owner` from the memset's 0 to -1 and leave player 0 unnamed for every
     * caller that does not set it. The other fields the refresh carries (+223,
     * the effect bytes, client+0x94) are already the memset's 0, which is what
     * the cleared entity and the cleared client record (0x8003B2BC) hold.
     */
    sim->combat.self.last_attacker = (s8)Q2_MP_NOT_A_PLAYER;
}

/* ------------------------------------------------------------------------- */
void q2_sim_aim(const q2_sim *sim, s16 out[3])
{
    s32 sy, cy, sp, cp;

    if (!out)
        return;
    if (!sim) {
        out[0] = out[1] = out[2] = 0;
        return;
    }

    sy = q2_sin12(sim->player[sim->cur_player].yaw);   cy = q2_cos12(sim->player[sim->cur_player].yaw);
    sp = q2_sin12(sim->player[sim->cur_player].pitch); cp = q2_cos12(sim->player[sim->cur_player].pitch);

    /* The engine keeps this triple at player+0x3C..0x40 as a 1.3.12 unit
     * vector; every fire function then scales it for itself — >> 6 for a
     * blaster bolt, << 2 for a bullet — so the port has to hand over the same
     * magnitude or every weapon's range and spread come out wrong together. */
    out[0] = (s16)(((s64)cp * sy) >> 12);
    out[1] = (s16)(-sp);          /* world Y grows downward */
    out[2] = (s16)(((s64)cp * cy) >> 12);
}

void q2_sim_set_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->combat.targets      = targets;
    sim->combat.target_count = targets ? count : 0;
}

void q2_sim_set_bodies(q2_sim *sim, const q2_move_body *bodies, u32 count)
{
    if (!sim)
        return;
    sim->extra_bodies      = bodies;
    sim->extra_body_count  = bodies ? count : 0;
}

int q2_sim_weapon_after_pickup(const q2_sim *sim, const q2_inventory *inv,
                               int held, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();
    u32 i;
    int rank_new = -1, rank_held = -1;

    if (!sim || !inv || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return held;

    /*
     * AUTOSWITCH, and it is a DEVIATION from the console rather than a fix to
     * it — stated here so the next reader does not "correct" it back.
     *
     * 0x80037E78 switches to the weapon just picked up only when the BLASTER is
     * the one in hand: take a shotgun while holding a railgun and the railgun
     * stays. That is the disc's behaviour and it is what `autoswitch == false`
     * still does, exactly — 0x80037E84's store, and nothing else.
     *
     * With it on — the default, at the owner's request — a pickup that ranks
     * ABOVE the held weapon is taken up instead. The ranking is not invented:
     * it is the console's own preference list at 0x8009DB7C, the same order
     * `q2_weapon_autoselect` walks after a shot empties something, so the gun
     * a pickup promotes you to is the gun the engine would have chosen for you
     * anyway. Explosives are absent from that list by design (weapontables.c),
     * which is what keeps a grenade pickup from arming a grenade in your hand.
     *
     * A weapon with no ammo does not win: `q2_weapon_usable` gates the walk, so
     * picking up a railgun you cannot feed leaves you holding what you had.
     */
    if (!sim->autoswitch)
        return held == Q2_WID_BLASTER ? weapon_id : held;

    for (i = 0; i < t->autoswitch_count; i++) {
        if (t->autoswitch[i] == weapon_id && rank_new < 0)
            rank_new = (int)i;
        if (t->autoswitch[i] == held && rank_held < 0)
            rank_held = (int)i;
    }

    /* Off the list entirely — an explosive — is never promoted to. */
    if (rank_new < 0)
        return held;

    /* Holding something the list does not rank (the blaster is on it, so this
     * is an explosive in hand) means anything ranked wins. */
    if (rank_held < 0 || rank_new < rank_held) {
        if (q2_weapon_usable(inv, weapon_id))
            return weapon_id;
    }

    return held;
}

bool q2_sim_give_weapon(q2_sim *sim, int weapon_id)
{
    const q2_weapon_tables *t = q2_weapon_tables_builtin();

    if (!sim || weapon_id <= 0 || weapon_id > Q2_WID_COUNT)
        return false;
    if (sim->combat.inv.weapons & t->owned_bit[weapon_id])
        return false;

    sim->combat.inv.weapons |= t->owned_bit[weapon_id];
    sim->combat.weapon_id   = q2_sim_weapon_after_pickup(
        sim, &sim->combat.inv, sim->combat.weapon_id, weapon_id);

    return true;
}

bool q2_sim_cycle_weapon(q2_sim *sim, int dir)
{
    int next;

    if (!sim)
        return false;

    next = q2_weapon_cycle(&sim->combat.inv, sim->combat.weapon_id, dir);
    if (next == Q2_WID_NONE)
        return false;

    sim->combat.weapon_id = next;
    return true;
}

bool q2_sim_autoselect_weapon(q2_sim *sim)
{
    int best;

    if (!sim)
        return false;

    /*
     * 0x800506C4, the refire pass's own selection: walk the fixed preference
     * list and take the first entry that is both owned and fed. Idempotent —
     * a player already holding the best affordable weapon re-picks it and
     * nothing changes, which is exactly why the console can afford to run it
     * after every shot.
     *
     * This is NOT q2_sim_cycle_weapon. That one transcribes 0x80050758, the
     * +/-1 neighbour scan the console runs twice per pass only to refill its
     * next/previous caches, and it never writes the held weapon. The refire
     * pass used to call it, so every shot advanced the carousel by one.
     */
    best = q2_weapon_autoselect(&sim->combat.inv);
    if (best == Q2_WID_NONE || best == sim->combat.weapon_id)
        return false;

    sim->combat.weapon_id = best;
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * How far along a shot's direction the world lets it travel, as a 1.0.12
 * fraction. The direction carries the range — 0x80048790 forms the trace end as
 * origin + dir — so the fraction the hull returns is exactly what the entity
 * pass needs to bound itself with.
 */
/*
 * THE COMBAT RULES ARE SESSION STATE, and until now only the clock in them was.
 *
 * `q2_combat_rules_default` memsets the struct and writes skill 1, and the two
 * call sites that touched it afterwards refreshed `level_time` and nothing
 * else. So the other two fields were frozen at their defaults for the whole run
 * and every rule that reads them was dead:
 *
 *   - `skill` never left 1, so 0x800582C8's "a monster hits you at skill 0 for
 *     half" never fired, even though the front end already knows the skill and
 *     hands it to the creature AI through `q2_cre_set_skill`. Easy was as
 *     dangerous as medium. Armour's rounding bias hangs off the same halfword
 *     (0x80057C10 `lh 0x800B334A`, then 0x80057C18 `bne v0, zero` to the 2048
 *     in its delay slot), so easy's 4095 was never used either.
 *   - `deathmatch` never left false, so the railgun's 150 (0x8004D5D8) was
 *     unreachable and the -3072 rocket-jump ceiling was applied inside
 *     deathmatch, where 0x800580E8 does not apply it. The armour bias does not
 *     read it: the deathmatch word 0x800AEBCC is loaded at 0x80057DA8 for the
 *     killer byte and at 0x800580D0 for that ceiling (tested at 0x800580E0),
 *     while the bias's own load, 0x80057C10, is the skill halfword above.
 *
 * Both are read from where the rest of the port already keeps them rather than
 * from a new copy: the skill from the AI's own global, deathmatch from the
 * `sim->multiplayer` flag that `sim->ent_world.deathmatch` is already taken
 * from, so the item half and the combat half of deathmatch cannot disagree.
 *
 * And the third frozen field was `cheats`. The damage function reads the GAME
 * VARIABLES word straight out of its global — 0x80058398 `lhu` of 0x800B29EC —
 * for ONE SHOT KILL, and `q2_combat_rules_default` zeroes it. The client
 * already writes the menu's word into `sim->cheats` (0x8001C698's hand-off),
 * so without this line the cheat reached the sim and stopped one struct short
 * of the only instruction that reads it: a zero word, forever. Copied from the
 * same field the item dispatch already reads bit 0 of, so there is still one
 * 0x800B29EC in the port.
 *
 * And the fourth was `knockback_mass`, BLAST FORCE. 0x80057F84 reads
 * 0x800B3358 on every hit and scales the impulse by 125*(v+64)>>6, or
 * 25*(v+64)>>2 when a player hurt themselves, so a zero there is not a
 * disabled rule but a halved one: every rocket, grenade, rail slug and bullet
 * pushed at half the disc's severity, and gibs were thrown half as far.
 * Carried from `sim->blast_force`, which q2_sim_init seeds with the reset
 * routine's 64 and the menu overwrites with the slider.
 */
static void sync_rules(q2_sim *sim)
{
    sim->combat.rules.level_time = sim->level_time;
    sim->combat.rules.skill      = (s16)q2_cre_skill();
    sim->combat.rules.deathmatch = sim->multiplayer;
    sim->combat.rules.cheats     = sim->cheats;   /* 0x80058398 lhu 0x800B29EC */
    sim->combat.rules.knockback_mass = (s16)sim->blast_force;  /* 0x800B3358 */
}

/*
 * How far the world let the shot go, and WHICH AREA it stopped in.
 *
 * The console's hitscan keeps both: 0x80048854 `lbu s1, 32(v1)` reads the
 * area record straight off the trace's own cell and carries it into the
 * impact burst. The port formed the same trace and threw the cell away, so the
 * puff and the blood had nothing to pass but a literal 0 — which the group
 * draw culls. `area` is optional; it is 0 when the trace ended on a runtime
 * entity box (a door or an intact pane, q2_trace.node == -1), which is not a
 * cell and has no area record at all.
 */
static s32 world_fraction_for(q2_sim *sim, const s32 origin[3],
                              const s32 dir[3], u8 *area)
{
    q2_trace tr;
    s32 end[3];
    int k;

    if (area)
        *area = 0;
    if (!sim->coll_ready)
        return 4096;

    for (k = 0; k < 3; k++)
        end[k] = origin[k] + dir[k];

    q2_sim_trace(sim, origin, end, &tr);
    if (area && tr.node >= 0)
        *area = (u8)(tr.contents & 0x7F);
    return tr.hit ? tr.fraction : 4096;
}

/*
 * Is the SIM'S OWN PLAYER behind this origin pointer? combat.c hands the clear
 * test `t->origin` itself (q2_combat_radius_damage_traced), so a pointer
 * compare against the sim's player actors is exact, and a creature — whose
 * actor the client owns — can never compare equal.
 */
static bool is_player_origin(const q2_sim *sim, const s32 *origin)
{
    int k;

    if (origin == sim->combat.self.origin)
        return true;
    for (k = 0; k < Q2_SIM_MAX_PLAYERS; k++)
        if (origin == sim->pcombat[k].self.origin)
            return true;
    return false;
}

/*
 * "Can the blast see this candidate?" — 0x80050810's two gates, which sit
 * between the falloff and the damage call and skip the candidate on a zero:
 *
 *   0x80050A24  jal 0x80044C44   a0 = 0x800C8E90 (PrimaryColl), a1 = the
 *                                blast, a2 = candidate+0x54, a3 = the
 *                                projectile's own cell (the caller's 2nd arg)
 *   0x80050A3C  jal 0x80053974   a0 = the blast, a1 = candidate+0x54, a2 = 0
 *
 * 0x80044C44 is the hull's swept move — collision.h names it q2_coll_move —
 * and 0x80053974 is the 48-slot entity-box clip at 0x800CAE10, which returns
 * 0 when a box stops the segment. So the pair is "the hull lets the segment
 * through, and then no door, lift or intact pane does", which is exactly the
 * two passes q2_sim_trace makes, in the same order, through the same hull.
 *
 * THE ZONE/NODE QUERY IS STILL NOT MODELLED AS A SEPARATE GATE. combat.h
 * describes 0x80050A24 as a zone/node visibility query and leaves it out;
 * nothing here adds one. Read at the call, that `jal` is the same swept move
 * this trace already runs, so modelling it again would trace the hull twice.
 * The one argument q2_sim_trace does not pass is the cell HINT in a3: it
 * starts from -1, a brute-force sweep (0x80044C74). That only changes which
 * cell a boundary point is placed in, and q2_coll_point_in_node is inclusive,
 * so a blast sitting on the face it struck is still inside that cell.
 *
 * Two port-side adjustments, both because the port's inputs differ from the
 * console's rather than because the rule does:
 *
 *   - A PLAYER'S actor carries the FEET in `origin` (q2_actor_from_player is
 *     handed `player.pos`), where retail's candidate+0x54 is the entity
 *     origin, Q2_EYE_BASE above them. The segment is aimed at the origin, so
 *     a floor lip between a blast and a player's feet hides nothing the
 *     console's segment would not also meet.
 *   - A blast point in NO CELL (tr.node < 0) is let through. The console's
 *     blast is where its projectile's own move left it, always inside a cell;
 *     the port detonates a direct hit at the step's unclipped end, which can
 *     lie inside a wall behind the victim. There the question has no console
 *     answer, so it keeps the distance-only one it had before occlusion, and
 *     `q2_sim_proj_scan.splash_unplaced` counts every such blast.
 *
 * Behind `coll_ready`, exactly as world_fraction_for is: no movement hull, no
 * world, and everything is visible.
 */
static bool splash_clear(void *ctx, const s32 from[3], const s32 to[3])
{
    q2_sim *sim = (q2_sim *)ctx;
    q2_trace tr;
    s32 aim[3];

    if (!sim || !sim->coll_ready || !from || !to)
        return true;

    aim[0] = to[0];
    aim[1] = is_player_origin(sim, to) ? q2_sim_origin_y(to[1]) : to[1];
    aim[2] = to[2];

    q2_sim_proj_scan.splash_asked++;
    q2_sim_trace(sim, from, aim, &tr);

    if (tr.hit && tr.node < 0) {
        q2_sim_proj_scan.splash_unplaced++;
        return true;
    }
    if (tr.hit)
        q2_sim_proj_scan.splash_occluded++;
    return !tr.hit;
}

/* Player inventories own pickups and ammunition; actors own damage during a
 * trace. Synchronise at the damage boundary, before another owner can be
 * selected or an item can mutate an inventory. Health-only write-back loses
 * armour and shield cells, and waiting until after a swap loses hits on player 0.
 * Preserve actor effects/attribution rather than reinitialising a target. */
static void player_damage_begin(q2_sim *sim)
{
    int pi;
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        q2_actor *a = pi == sim->cur_player ? &sim->combat.self
                                           : &sim->pcombat[pi].self;
        const q2_inventory *inv = pi == sim->cur_player ? &sim->combat.inv
                                                       : &sim->pcombat[pi].inv;
        a->health = inv->health;
        a->armour = inv->armour;
        a->armour_class = inv->armour_class;
        a->cells = inv->ammo[Q2_AMMO_CELLS];
        a->powerups = inv->flags;
        a->invuln_until = inv->invuln_until;
        a->protect_until = inv->enviro_until;
        a->origin[0] = sim->player[pi].pos[0];
        a->origin[1] = q2_sim_origin_y(sim->player[pi].pos[1]);
        a->origin[2] = sim->player[pi].pos[2];
    }
}

/*
 * Integer square root, for the one place in this file that has to turn a
 * separation into a unit direction (the hit point q2_sim_hurt_player rebuilds).
 * combat.c keeps its own for apply_knockback's normalise; this is the same
 * binary search rather than a second formula, and it truncates the same way.
 */
static s32 hurt_isqrt(s64 v)
{
    s64 lo = 0, hi = 0x7FFFFFFF, best = 0;

    if (v <= 0)
        return 0;
    while (lo <= hi) {
        s64 mid = lo + (hi - lo) / 2;
        if (mid * mid <= v) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (s32)best;
}

static void player_damage_impulse(q2_sim *sim, int pi, q2_actor *actor)
{
    int axis;
    if (!actor->knocked) return;
    for (axis = 0; axis < 3; axis++) {
        sim->player[pi].impulse[axis] =
            (s16)(sim->player[pi].impulse[axis] + actor->knockback[axis]);
        actor->knockback[axis] = 0;
    }
    sim->player[pi].impulse_armed = true;
    actor->knocked = false;
}

static void player_damage_end(q2_sim *sim)
{
    int pi;
    for (pi = 0; pi < sim->player_count && pi < Q2_SIM_MAX_PLAYERS; pi++) {
        q2_actor *a = pi == sim->cur_player ? &sim->combat.self
                                          : &sim->pcombat[pi].self;
        q2_inventory *inv = pi == sim->cur_player ? &sim->combat.inv
                                                 : &sim->pcombat[pi].inv;
        q2_actor_to_player(a, inv);
        player_damage_impulse(sim, pi, a);
    }
}

q2_fire_result_v2 q2_sim_fire(q2_sim *sim)
{
    q2_fire_result_v2 r;
    s32 eye[3];
    s16 aim[3];
    s32 prev_next_fire;
    u32 i;

    memset(&r, 0, sizeof(r));
    r.sound = -1;
    if (!sim)
        return r;

    prev_next_fire = sim->combat.next_fire;

    q2_sim_eye(sim, eye);
    q2_sim_aim(sim, aim);

    sync_rules(sim);
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);

    r = q2_weapon_fire(&sim->combat.inv, &sim->combat.rng, NULL,
                       sim->combat.weapon_id, eye,
                       sim->player[sim->cur_player].yaw,
                       sim->player[sim->cur_player].pitch,
                       sim->player[sim->cur_player].roll, aim,
                       sim->level_time, sim->combat.next_fire,
                       /*
                        * QUAD, which was a hardcoded `false` — so the powerup
                        * was picked up, counted down on the HUD and played
                        * `itm_damage3` on every shot while multiplying nothing.
                        *
                        * Every fire function opens by comparing the level clock
                        * against the player's own expiry word (client+0xAC,
                        * which is `quad_until`) and picks the second immediate
                        * on the near side of it — 8 or 32, 6 or 24, 120 or 480.
                        * That comparison is this argument, and the view model
                        * already forms exactly it for the sound
                        * (`q2_vw.quad_active`).
                        */
                       sim->level_time < sim->combat.inv.quad_until,
                       sim->combat.rules.deathmatch,
                       sim->combat.chaingun_bullets);

    sim->combat.last_shot = r;
    sim->combat.shot_serial++;
    if (!r.fired) {
        /* A dry trigger still takes the gate — see Q2_WEAPON_DRY_REFIRE. A
         * shot blocked by the clock does not, and must not: overwriting the
         * deadline from a blocked tick would push it forward forever and the
         * weapon would never fire again. */
        if (r.dry)
            sim->combat.next_fire = r.next_fire;
        return r;
    }

    /*
     * The muzzle flash. Only the machinegun and the chaingun carry one, and its
     * radii come from a single rand draw the way 0x8004C978 computes them --
     * see weapon.h. Drawn AFTER the shot so the flash does not perturb the
     * spread sequence, which is what the fire function's own ordering does.
     */
    if (q2_weapon_has_muzzle_light((u8)sim->combat.weapon_id)) {
        static const u8 flash[3] = { Q2_MUZZLE_LIGHT_R, Q2_MUZZLE_LIGHT_G,
                                     Q2_MUZZLE_LIGHT_B };
        s32 inner, outer;

        q2_weapon_muzzle_light(q2_rng_next(&sim->combat.rng), &inner, &outer);
        /*
         * At the player ENTITY ORIGIN, not at the eye and not at the feet.
         * The console passes `addiu a0, s3, 84` — player + 0x54.
         *
         * `sim->player[].pos` is the FEET (sim.h says so at the top of the
         * file, and sim.c converts with q2_sim_origin_y everywhere it wants
         * the entity). Passing it raw moved the flash from 290 above the
         * origin to 286 below it — a correction in the right direction that
         * overshot by the whole body.
         */
        s32 lit[3];

        lit[0] = sim->player[sim->cur_player].pos[0];
        lit[1] = q2_sim_origin_y(sim->player[sim->cur_player].pos[1]);
        lit[2] = sim->player[sim->cur_player].pos[2];
        q2_ent_light_at(&sim->ent_world.events, lit, flash, inner, outer);
    }

    sim->combat.next_fire = r.next_fire;
    sim->combat.kick[0] = r.kick[0];
    sim->combat.kick[1] = r.kick[1];
    sim->combat.kick[2] = r.kick[2];

    /*
     * And on to the view, which is where a kick was always going: the weapon
     * table's figure has had a home in `combat.kick` for a while and nothing
     * read it. `q2_sim_view_angles` composes it over 30 ticks (FORMATS.md
     * §9.12.11a), so the deadline is what turns one number into recoil.
     */
    sim->player[sim->cur_player].kick[0]  = r.kick[0];
    sim->player[sim->cur_player].kick[1]  = r.kick[1];
    sim->player[sim->cur_player].kick[2]  = r.kick[2];
    sim->player[sim->cur_player].kick_time = sim->level_time + Q2_VIEW_KICK_FIRE;

    player_damage_begin(sim); /* after the fire function spends ammunition */
    switch (r.kind) {
    case Q2_FK_BULLET:
        /* Every pellet is its own trace, which is why a shotgun can catch two
         * creatures and a machinegun cannot. */
        for (i = 0; i < r.shot_count; i++) {
            const q2_shot *s = &r.shot[i];
            u8  frac_area;
            s32 frac = world_fraction_for(sim, s->origin, s->dir,
                                          &frac_area);
            q2_damage_result dr;
            s32 victim;

            victim = q2_combat_fire_bullet(&sim->combat.self, s->origin,
                                           s->dir, s->damage, frac,
                                           Q2_HITSCAN_RADIUS,
                                           sim->combat.targets,
                                           sim->combat.target_count,
                                           &sim->combat.rules, &dr);
            fx_hitscan_impact(sim, s->origin, s->dir, frac, frac_area,
                              victim, &dr, s->damage);
        }
        break;

    case Q2_FK_RAIL: {
        const q2_shot *s = &r.shot[0];
        u8  frac_area;
        s32 frac = world_fraction_for(sim, s->origin, s->dir, &frac_area);
        u32 hits = q2_combat_fire_rail(&sim->combat.self, s->origin, s->dir,
                                       s->damage, frac, Q2_HITSCAN_RADIUS,
                                       sim->combat.targets,
                                       sim->combat.target_count,
                                       &sim->combat.rules);

        /* The rail does not stop at the first target, so it always marks the
         * world where the beam ends and blood is left to the per-target pass
         * this port does not get back from `fire_rail`. */
        fx_hitscan_impact(sim, s->origin, s->dir, frac, frac_area, -1, NULL,
                          s->damage);
        (void)hits;
        break;
    }

    default:
        /* WHO fired it. The -1 here meant "the world", so a bolt could not say
         * who to credit and a kill by one had no killer. */
        q2_sim_proj_scan.launched++;
        if (q2_projectile_launch(&sim->combat.projectiles, &r,
                                 sim->cur_player, sim->level_time) < 0) {
            /*
             * THE POOL WAS FULL, and the shot has already been paid for:
             * q2_weapon_fire decrements the ammo and advances the refire gate
             * before this line is reached, so swallowing the launch charges the
             * player for nothing. Refund both and report the shot as not fired,
             * which is also what stops the view model playing a fire clip for a
             * projectile that does not exist.
             *
             * This used to be unreachable in practice only because projectiles
             * never terminated (they moved at a twentieth of their speed and
             * filled the pool); with the step fixed it is rare, and it is still
             * wrong to lose a rocket to it.
            */
            q2_sim_proj_scan.dropped_full++;
            /* Grenade3 has only been primed here; its ammo is not charged
             * until the 411 release crossing, so there is nothing to refund. */
            if (r.kind != Q2_FK_HAND_GRENADE)
                q2_weapon_refund(&sim->combat.inv, sim->combat.weapon_id);
            sim->combat.next_fire = prev_next_fire;
            r.fired = false;
            /* The same attempt amended, not a new one, so the serial stays
             * where the write above left it. */
            sim->combat.last_shot = r;
        } else if (r.kind == Q2_FK_HAND_GRENADE && sim->fire_from_input) {
            /* A harness with no view-model machine has no 261/380/411
             * timeline to drive Grenade3. Preserve that API's historical
             * "attack fires" contract by releasing at minimum charge in the
             * same call. The playable client sets fire_from_input false and
             * follows the retail held path below. */
            if (q2_sim_hand_grenade_update(sim, eye, 0, true) ==
                Q2_HAND_GRENADE_RELEASED) {
                r.sound = Q2_WSND_HANDGREN_THROW;
                sim->combat.last_shot = r;
            }
        }
        break;
    }

    /* Projectile allocation failure may refund cells; no trace has run on
     * that arm, so keep its inventory as the source. */
    if (r.kind == Q2_FK_BULLET || r.kind == Q2_FK_RAIL)
        player_damage_end(sim);
    return r;
}

q2_damage_result q2_sim_hurt_player(q2_sim *sim, q2_actor *attacker,
                                    s16 damage, s16 mod, const s32 point[3])
{
    q2_damage_result out;
    /* The rebuilt hit point, when the shot arm below has to rebuild one.
     * It outlives that block because `point` is read again for the blood
     * and the flinch, so it lives here rather than inside the branch. */
    s32 aimed[3] = { 0, 0, 0 };

    memset(&out, 0, sizeof(out));
    if (!sim)
        return out;

    /*
     * A CAPTURE AID, off unless a harness asks for it: the player takes no
     * damage. It exists because several things worth photographing — a
     * creature's death animation, a corpse settling, a long fight — outlast
     * the player in any engagement dense enough to produce them, and every
     * frame captured of one was a death-cam view instead. Returning the empty
     * result rather than clamping health keeps the whole chain quiet: no
     * flinch, no kick, no blood, no death.
     */
    if (sim->invulnerable)
        return out;

    /* Health and armour live in the inventory, everything else in the actor, so
     * the two are synchronised around the call rather than duplicated. */
    q2_actor_from_player(&sim->combat.self, &sim->combat.inv, sim->player[sim->cur_player].pos);
    sync_rules(sim);

    /*
     * A CONTACT HIT LANDS AT THE ATTACKER, and that is not the caller's choice
     * to make. `0x800612F0` passes the creature's own origin as the damage
     * point — `q2_combat_melee` is that one line — and everything downstream of
     * `point` is DIRECTIONAL: the knockback, where the blood sprays, and the
     * flinch's roll, which is the hit point measured against the view's own
     * right vector.
     *
     * The client used to hand this the PLAYER's own position for a melee, which
     * makes that difference the zero vector. `side` came out 0 on every claw
     * that ever landed, so a hit from the left rolled the view exactly as far
     * as a hit from the right — which is to say not at all — and the blood
     * sprayed from the player's own feet. Overriding the point here rather than
     * trusting the caller is what stops that being reintroduced: a melee is the
     * one mod whose point is not free.
     */
    if (mod == Q2_MOD_MELEE && attacker) {
        out   = q2_combat_melee(attacker, &sim->combat.self, damage,
                                &sim->combat.rules);
        point = attacker->origin;
    } else {
        /*
         * A SHOT ALSO ARRIVES FROM SOMEWHERE, and the creature hooks hand this
         * the SIGHT CLIENT's position — which `sight_place` pins to the player
         * every tick, so the "hit point" of every creature bullet, rail, rocket
         * and grenade was the player's own origin. That is the failure the melee
         * override above exists to stop, one mod family along, and unlike a claw
         * these four ARE in the knockback set (0x80057ED0..0x80057EE8: mods 3,
         * 13, 15 and 18).
         *
         * What that did was not "no knockback". `apply_knockback` forms
         * `target->origin - point`; the player's actor carries the FEET in
         * `origin` and the supplied point is the entity origin 286 above them,
         * so the difference was (0, +286, 0) on every hit — straight DOWN, since
         * +Y is down. Every creature bullet shoved the player into the floor
         * and, through `player_damage_impulse` into 0x800460A4, cleared
         * ON_GROUND and the ground normal with it.
         *
         * The console's point is where the trace landed on the victim:
         * 0x8004874C sweeps the ray (0x8004891C `jal 0x800544EC`) and hands
         * T_Damage the swept endpoint at sp+24 (0x80048974, with `sw s0,
         * 16(sp)`), which is on the target's own hull facing the shooter. That
         * is the same point this port's OWN hitscan already computes for the
         * player's guns (q2_combat_fire_bullet, combat.c), so rebuild it in that
         * shape rather than invent a third convention: keep the point ON the
         * player and displace it one hull half-extent (Q2_EYE_BASE, 286) back
         * along the shot's own direction.
         *
         * NOT `point = attacker->origin`: that is mod 7's rule and mod 7's only
         * (0x800612F0), and moving a hitscan's point onto the shooter would put
         * the blood on the creature at `fx_at` below.
         *
         * Only when the caller's point really is horizontally coincident with
         * the player, so a caller that computed a real impact point — the
         * player's own splash, a traced shot — keeps the one it computed.
         */
        s32  dx = 0, dz = 0, len = 0;
        bool blind = attacker && attacker != &sim->combat.self && point &&
                     point[0] == sim->combat.self.origin[0] &&
                     point[2] == sim->combat.self.origin[2];

        if (blind) {
            dx  = point[0] - attacker->origin[0];
            dz  = point[2] - attacker->origin[2];
            len = hurt_isqrt((s64)dx * dx + (s64)dz * dz);
        }

        if (len > 0) {
            aimed[0] = point[0] - (s32)(((s64)dx * Q2_EYE_BASE) / len);
            aimed[2] = point[2] - (s32)(((s64)dz * Q2_EYE_BASE) / len);
            /*
             * THE VERTICAL IS THE PORT'S, and it is a deviation worth naming.
             * `q2_actor_from_player` puts the FEET in the actor's origin where
             * the console's entity+0x54 is mid-body, so a knockback point left
             * at the console's impact height would leave the 286-unit
             * feet-to-origin offset inside `target - point` and keep half of the
             * old downward shove. The damage call therefore gets the actor's own
             * Y, which makes that difference purely horizontal — what the
             * console computes for a level shot — while the blood and the flinch
             * below keep the impact height they already had.
             */
            aimed[1] = sim->combat.self.origin[1];
            out = q2_combat_damage(attacker, &sim->combat.self, damage, mod,
                                   aimed, &sim->combat.rules);
            aimed[1] = point[1];
            point    = aimed;
        } else {
            out = q2_combat_damage(attacker, &sim->combat.self, damage, mod,
                                   point, &sim->combat.rules);
        }
    }

    q2_actor_to_player(&sim->combat.self, &sim->combat.inv);
    player_damage_impulse(sim, sim->cur_player, &sim->combat.self);

    /* Blood only when flesh actually took some of it: armour absorbing the
     * whole hit is the case the HUD's damage flash also distinguishes. */
    if (out.taken > 0)
        fx_at(sim, Q2_FX_BLOOD, point);

    /*
     * The flinch. Amplitude scaled by how much got through, capped at the same
     * 40 degrees the fall kick is capped at, and pitched UP and rolled with the
     * sign of the knockback so a hit from the left throws the view right.
     *
     * The amplitude is the port's: 0x80038334 reads client+0x9C and +0x9E and
     * this is the only path that could write them, but the write itself sits
     * behind the damage callback rather than in T_Damage, so what the console
     * puts there is not established. The DECAY is the console's — 150 ticks
     * against the pain deadline — and that is the part that is felt.
     */
    if (out.taken > 0) {
        s32 amp = out.taken * 8;
        s32 side = 0;

        if (amp > Q2_FALL_KICK_MAX)
            amp = Q2_FALL_KICK_MAX;

        /* Which side it came from: the hit point against the view's own right
         * vector, so a shot from the left rolls the view right. */
        if (point) {
            s32 sy = q2_sin12(sim->player[sim->cur_player].yaw), cy = q2_cos12(sim->player[sim->cur_player].yaw);
            s32 dx = point[0] - sim->player[sim->cur_player].pos[0];
            s32 dz = point[2] - sim->player[sim->cur_player].pos[2];

            side = (cy * dx - sy * dz) >> Q2_FRAC_12;
        }

        sim->player[sim->cur_player].hurt_kick[0] = (s16)(-amp);
        sim->player[sim->cur_player].hurt_kick[1] = (s16)((side >= 0) ? -amp / 2 : amp / 2);
    }

    return out;
}

/*
 * The actor that fired a projectile, from the owner index the launch recorded.
 *
 * The step runs on player 0's tick — projectiles are the world's — so
 * `combat.self` at step time is player 0 whoever fired. Passing that as the
 * attacker credited every bolt in the air to player 0 and gave a
 * player-versus-player kill the wrong killer, or the shooter their own bolt.
 */
/* Where a projectile got to, per the note on q2_combat_scan: "it missed" has
 * several causes and a total cannot tell them apart. */
q2_sim_proj_stats q2_sim_proj_scan;

void q2_sim_set_world_targets(q2_sim *sim, q2_actor **targets, u32 count)
{
    if (!sim)
        return;
    sim->world_targets      = targets;
    sim->world_target_count = count;
}

static q2_actor *attacker_for(q2_sim *sim, s32 owner)
{
    if (owner < 0 || owner >= Q2_SIM_MAX_PLAYERS)
        return &sim->combat.self;
    if (owner == sim->cur_player)
        return &sim->combat.self;
    return &sim->pcombat[owner].self;
}

/* The caller's target list normally excludes the shooter so a bolt cannot hit
 * its own muzzle. Radius damage is different: the console deliberately lets a
 * projectile hurt its owner (including the 3.2x self-knockback path). Add that
 * one actor when the published list does not already contain it. */
static void projectile_owner_splash(q2_sim *sim, const q2_projectile *p,
                                    const s32 point[3],
                                    q2_actor **targets, u32 count)
{
    q2_actor *owner;
    q2_actor *one[1];
    u32 i;

    if (!sim || !p || p->owner < 0 || p->owner >= Q2_SIM_MAX_PLAYERS ||
        p->splash_radius <= 0 || p->damage <= 0)
        return;

    owner = attacker_for(sim, p->owner);
    for (i = 0; i < count; i++)
        if (targets && targets[i] == owner)
            return;

    if (!owner->takedamage ||
        (owner == &sim->combat.self && sim->invulnerable))
        return;

    /* The owner is one more candidate of the same 0x80050810 sweep, so it is
     * occluded by the same trace: a rocket that bursts on the far side of a
     * wall does not reach the player who fired it. */
    one[0] = owner;
    q2_combat_radius_damage_traced(owner, NULL, point ? point : p->pos,
                                   p->damage, p->splash_radius, p->mod,
                                   one, 1, &sim->combat.rules,
                                   splash_clear, sim);

    if (owner == &sim->combat.self)
        q2_actor_to_player(owner, &sim->combat.inv);
}

q2_hand_grenade_update q2_sim_hand_grenade_update(
    q2_sim *sim, const s32 attached_pos[3], s32 cook_dt, bool release)
{
    static const s16 release_offset[3] = {
        Q2_HAND_GRENADE_RELEASE_RIGHT,
        Q2_HAND_GRENADE_RELEASE_DOWN,
        Q2_HAND_GRENADE_RELEASE_FORWARD
    };
    s32 index;
    q2_projectile *p;

    if (!sim || !attached_pos)
        return Q2_HAND_GRENADE_NONE;

    index = q2_projectile_hand_held_index(&sim->combat.projectiles,
                                          sim->cur_player);
    if (index < 0)
        return Q2_HAND_GRENADE_NONE;

    q2_projectile_hand_update(&sim->combat.projectiles, sim->cur_player,
                              attached_pos, cook_dt);
    p = &sim->combat.projectiles.p[index];

    /* The fuse is decremented before Grenade3's state dispatch. A held expiry
     * therefore wins over a 411 release reached on the same tick. */
    if (p->expires && sim->level_time >= p->expires) {
        q2_actor **targets = sim->world_targets ? sim->world_targets
                                                : sim->combat.targets;
        u32 count = sim->world_targets ? sim->world_target_count
                                       : sim->combat.target_count;
        s32 where[3];

        player_damage_begin(sim);
        memcpy(where, p->pos, sizeof(where));
        projectile_owner_splash(sim, p, where, targets, count);
        q2_projectile_detonate_traced(&sim->combat.projectiles, (u32)index,
                                      attacker_for(sim, p->owner), targets,
                                      count, &sim->combat.rules,
                                      splash_clear, sim);
        fx_at(sim, Q2_FX_EXPLOSION, where);
        player_damage_end(sim);
        return Q2_HAND_GRENADE_EXPIRED;
    }

    if (release) {
        s16 local_dir[3];
        s32 eye[3], origin[3], raw_dir[3];
        static const s32 zero[3] = { 0, 0, 0 };
        s32 charge = q2_projectile_hand_charge(&sim->combat.projectiles,
                                                sim->cur_player);
        q2_player *pl = &sim->player[sim->cur_player];

        q2_sim_eye(sim, eye);
        q2_weapon_muzzle_origin(release_offset, eye,
                                pl->yaw, pl->pitch, pl->roll, origin);

        local_dir[0] = 0;
        local_dir[1] = -Q2_HAND_GRENADE_RELEASE_UP;
        local_dir[2] = (s16)charge;
        q2_weapon_muzzle_origin(local_dir, zero,
                                pl->yaw, pl->pitch, pl->roll, raw_dir);

        if (q2_projectile_hand_release(&sim->combat.projectiles,
                                       sim->cur_player, origin, raw_dir)) {
            /* 0x8004A7C0: after reveal, velocity and throw sound. */
            (void)q2_weapon_consume(&sim->combat.inv,
                                    Q2_WID_HAND_GRENADE);
            return Q2_HAND_GRENADE_RELEASED;
        }
    }

    return Q2_HAND_GRENADE_HELD;
}

/* ------------------------------------------------------------------------- */
/*
 * THE CLIENT HALF OF ONE PRESENTATION: which viewport the actor's bursts hide
 * from, and whose quad deadline its shell reads.
 *
 * The console takes both off the entity it is presenting. Every drawer writes
 * the view nibble for ANY entity whose +0x0C client is non-null (0x800590C0
 * `lw v1, 12(s5)` / 0x800590C8 `beq`, and the same at 0x80058B70, 0x80058D38,
 * 0x80058EFC and 0x80059288), from that client's own index; and the shell
 * reads that entity's own client+0xAC (0x8005B7EC, 0x8005B7FC). So the live
 * player is not special. A PARKED deathmatch player in the world list is a
 * client entity too, and it gets its own index and its own deadline; only an
 * actor with no client gets -1 and 0.
 *
 * This used to pass -1 and 0 for everything in the world list and -1 for the
 * live player. Once a mesh arrives that would have drawn a parked player's
 * crackle in that player's own viewport, which the console hides, and would
 * never have shown a quad shell on anyone but the live player.
 *
 * The client is found by POINTER, because the sim owns every player actor it
 * can name: `combat.self` is the live player's, `pcombat[k].self` player k's
 * while it is parked. An actor that claims a client but is none of those has
 * no client block this sim can read, so it keeps -1 and 0 (no nibble, no
 * shell). `has_client` gates both, as the console's client pointer does.
 */
static void present_client_of(const q2_sim *sim, const q2_actor *a,
                              s32 *view_skip, s32 *quad_until)
{
    int k;

    *view_skip  = -1;
    *quad_until = 0;
    if (!a->has_client)
        return;

    if (a == &sim->combat.self) {
        *view_skip  = sim->cur_player;
        *quad_until = sim->combat.inv.quad_until;
        return;
    }
    for (k = 0; k < Q2_SIM_MAX_PLAYERS; k++) {
        if (k != sim->cur_player && a == &sim->pcombat[k].self) {
            *view_skip  = k;
            *quad_until = sim->pcombat[k].inv.quad_until;
            return;
        }
    }
}

/*
 * One actor through 0x8005B880, and the energy light it reports.
 *
 * THE MESH COMES FROM THE OWNER'S HOOK. This file has no posed model for an
 * actor; the client does, so it installs `sim->fx_mesh` and the hook hands back
 * a q2_fx_mesh_src whose vertex callback yields the WORLD-space posed vertex
 * 0x8006CC44 returns. With no hook, or a hook that answers false (the local
 * player's own actor, which has no body in first person), the source is NULL
 * and the drawers take the console's model-less path: 0x8006D6AC returns zero
 * for a null model, every walk finds fewer vertices than it needs and leaves,
 * and the spark leaves at 0x800589E0 before its 45 velocity draws. That last
 * part is exact only for an entity that really has no model. A MODELLED
 * creature the hook cannot pose costs the console those 45 draws per spark
 * firing and the port none (effect.h). The timers and the light reports are
 * exact either way. The hook decides only what `src` is, never whether the
 * call happens, so it cannot make a countdown run twice.
 *
 * The source is built immediately before the call and dies after it, so
 * whatever `ctx` the hook fills in only has to outlive this function, which is
 * the lifetime the hook's contract promises.
 *
 * `frame` is `tick_count`, the port's [0x800B2DE4]: a FRAME COUNTER, +1 a
 * world tick (0x800705C4 `addiu a1, a1, 1`, stored at 0x80070610), and counted
 * before this pass runs, as the console counts it before its thinks (sim.h).
 * It picks which vertices a drawer samples: the crackle starts on vertex
 * `frame & 1`, so the two halves of the mesh alternate, and the spark on
 * `frame & 7`. level_time used to stand in for it. The clock steps by dt, 12 a
 * tick at the nominal rate, so at any even dt its low bit never changed, the
 * crackle sampled the same half of the body forever, and the spark reached two
 * of its eight phases.
 *
 * The energy light, one per lit actor: 0x80058638 raises it on effect[1] >= 3
 * with the colour and radii at 0x800AEAAC and 0x800AEAB0 (combat.h). It follows
 * the report rather than a re-read of the slot, which puts it on the right side
 * of the decrement: 0x800586D0 raises it from the value the tick STARTED with,
 * and the subtraction at 0x80058768 comes afterwards. `q2_actor_energy_lit`
 * still states the same predicate for anyone who wants it without running a
 * presentation pass; combat.c owns it.
 *
 * NOT RAISED HERE: `rep.energy_pulse`, the `effect[1] == 2` arm's own light
 * at 0x80058758. It goes through 0x80075D14, a different function from the
 * `>= 3` arm's 0x80075C34, and which list entry that appends is lighting.c's
 * to name (effect.h). It is reported so it stops being invisible, and not
 * raised on a guess.
 */
static void present_actor(q2_sim *sim, q2_actor *a)
{
    static const u8 energy[3] = { Q2_ENERGY_LIGHT_R, Q2_ENERGY_LIGHT_G,
                                  Q2_ENERGY_LIGHT_B };
    static const u8 ambient_target[3] = { Q2_ACTOR_AMBIENT_DEFAULT,
                                          Q2_ACTOR_AMBIENT_DEFAULT,
                                          Q2_ACTOR_AMBIENT_DEFAULT };
    q2_fx_present_report rep;
    q2_fx_mesh_src src;
    const q2_fx_mesh_src *sp;
    s32 view_skip, quad_until;

    sp = (sim->fx_mesh && sim->fx_mesh(sim->fx_mesh_user, a, &src))
             ? &src : NULL;
    present_client_of(sim, a, &view_skip, &quad_until);

    /*
     * FIRST IN THE CHAIN, before anything this pass spawns: 0x8005B88C loads 7
     * and 0x8005B894 jumps to the ambient fade 0x80075E14, which walks
     * entity+0x2AC one seventh of the way toward +0x2B0 per call. lighting.c
     * has owned that routine since it was reconstructed and nothing called it,
     * so an actor's own back colour never moved; the creature and body draws
     * fed q2_light_env_build a literal instead.
     *
     * DEVIATION, stated: the target is the port's own 0x30 rather than the
     * entity's +0x2B0, which nothing in the image is known to write after a
     * spawn (combat.h, q2_actor.ambient). Seeded equal, so this changes only
     * what happens AFTER a hit.
     */
    q2_light_glow_fade(a->ambient, ambient_target, Q2_ACTOR_AMBIENT_STEPS);

    /*
     * THE AREA EVERY GROUP IN THIS CHAIN IS FILED UNDER. The console passes
     * entity+0x9E — the byte movement caches on the entity from the cell it
     * occupies, `lbu` of the collision record's +32 at 0x80046B08. Nothing in
     * this port caches a cell on an actor (q2_actor has no such field, and
     * q2_actor_from_monster rebuilds the struct every frame), so the byte is
     * resolved from the actor's origin here instead. DEVIATION, stated: the
     * console reads a cached byte and this does one hull descent per actor per
     * world tick. The value is the same cell movement would have recorded; what
     * differs is when it is looked up, and the previous 0 put the whole chain —
     * the damage crackle, the two sparks and the quad shell — into an area the
     * draw always culls.
     */
    q2_fx_actor_present(&sim->fx, &sim->fx_rng, a, sp, sim->tick_count,
                        fx_area_resolve(sim, 0, a->origin),
                        view_skip, sim->level_time, quad_until, &rep);

    if (rep.energy_light)
        q2_ent_light_at(&sim->ent_world.events, a->origin, energy,
                        Q2_ENERGY_LIGHT_INNER, Q2_ENERGY_LIGHT_OUTER);

    /*
     * And the ambient write the report has been carrying with no reader:
     * 0x800586E8 copies the four bytes at 0x800AEAAC into entity+0x2AC on the
     * same effect[1] >= 3 arm that raises the light, so the body itself takes
     * the energy colour for that tick and the fade above eases it back out over
     * the following ones. The green is the light's own preset, which is why it
     * is the same triplet.
     */
    if (rep.set_ambient) {
        a->ambient[0] = Q2_ENERGY_LIGHT_R;
        a->ambient[1] = Q2_ENERGY_LIGHT_G;
        a->ambient[2] = Q2_ENERGY_LIGHT_B;
    }
}

/* ------------------------------------------------------------------------- */
void q2_sim_combat_tick(q2_sim *sim)
{
    u32 i;

    if (!sim)
        return;

    /*
     * The projectiles in flight are the WORLD's, not a player's — one list,
     * shared, exactly like the entity sweep and the effects. So they step once
     * a frame, not once a player: with four players a bolt was advancing four
     * times per frame and a rocket crossed an arena at four times its speed.
     *
     * This is the same class of bug the world-half gate in `q2_sim_tick`
     * exists for, and it was missed because it lives in another file.
     */
    if (sim->cur_player != 0)
        return;

    player_damage_begin(sim);

    /*
     * This tick contains damage sites of its own — the detonations below and,
     * now, the BFG's beam pass — and the console reads the rule globals AT each
     * site rather than from a copy. `sim->combat.rules` was last refreshed by
     * whichever fire or hurt ran most recently, which for a projectile that has
     * been in the air for a second is a long time ago, so refresh it here for
     * the same reason q2_sim_fire and q2_sim_hurt_player do.
     */
    sync_rules(sim);

    /*
     * THE PER-ACTOR PRESENTATION PASS, 0x8005B880, once per actor per world
     * tick: the live player first, then every other actor in the world.
     *
     * THE COUNTDOWN IS NO LONGER THIS LOOP'S, AND THE GATES WERE WRONG.
     *
     * What used to sit here was `for every slot: if (slot) slot--`, over all
     * six bytes, for self and every world target, once per world tick. Its
     * comment said "nothing did", which was true and was the right thing to
     * notice — the first energy hit an actor took left `effect[1]` at full
     * strength for the rest of the level and parked a 1300-unit pure-green
     * light on them, which is the fault it was written to fix.
     *
     * But it decremented the wrong things on the wrong clock:
     *
     *   - effect[1] is run down INSIDE 0x80058638 (0x80058768), by the
     *     caller's dt, and only when it was non-zero on entry.
     *   - effect[0] and effect[2] are run down by (health > 0) (0x8005B8AC
     *     feeds `slt s1, zero, s1` to those two tickers alone), so on a
     *     corpse they hold and keep emitting. What ends that on the console
     *     is the corpse think's gate 0x8005B2A8, which swaps such a body to a
     *     dissolve handler that frees it a few frames later (effect.h). That
     *     gate is the corpse owner's to model, not this pass's.
     *   - effect[4] and effect[5] are run down by a literal 1 whatever the
     *     health (0x8005B8D0, 0x8005B8E4).
     *   - effect[3]: 0x8005B880 never touches +0x2F3, and `q2psx-inspect
     *     access 0x2F3` finds no immediate-offset load or store of it in the
     *     main executable (it does not scan the relocated modules), so
     *     decrementing it was inventing a rule.
     *
     * All five countdowns belong to the presentation pass, which is now
     * q2_fx_actor_present (effect.h). Calling it from here keeps the fix and
     * takes the rules from the executable.
     *
     * WHO IS PRESENTED. The console presents every entity every frame: the
     * creature think calls 0x8005B880 at 0x8007EC0C for every monster, alive
     * or dead (0x8007EBEC..0x8007EC08 only sets a flag when health <= 0), and
     * the player think calls it at 0x8003B004. Here that is `combat.self`
     * and then the world's list — `world_targets`, or `combat.targets` when
     * nobody published one, the same fallback every projectile path below
     * takes. This pass used to walk `world_targets` alone, so a caller that
     * registered its creatures only through q2_sim_set_targets had none of
     * them presented: the effect[1] = 3 an energy bolt armed on a monster was
     * never counted down by this pass and never raised its light. The client
     * now also publishes its single-player creature list as the world list
     * (main.c, after its q2_sim_set_targets); for this pass that call is
     * redundant, since both lists are the same one. The pass no longer
     * depends on what the caller publishes.
     *
     * ONCE PER ACTOR. The live player was presented first, and it can appear in
     * the list under two names, both skipped: `&combat.self`, which a
     * deathmatch world list names for the player who was live when it was
     * built (main.c client_targets_for); and `pcombat[cur_player].self`, that
     * player's PARKED slot. It is stale while the player is live, and
     * combat_swap_to overwrites it on the next swap. Presenting either would
     * run every countdown in effect[] twice a tick. A list is taken to name
     * each other actor once, as both of the client's lists do.
     */
    {
        q2_actor **list  = sim->world_targets ? sim->world_targets
                                              : sim->combat.targets;
        u32        count = sim->world_targets ? sim->world_target_count
                                              : sim->combat.target_count;
        const q2_actor *parked_self = &sim->pcombat[sim->cur_player].self;
        u32 t;

        present_actor(sim, &sim->combat.self);

        for (t = 0; t < count; t++) {
            q2_actor *a = list ? list[t] : NULL;

            if (!a || a == &sim->combat.self || a == parked_self)
                continue;
            present_actor(sim, a);
        }
    }

    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *p = &sim->combat.projectiles.p[i];
        q2_proj_step step;
        q2_actor **hit_list;
        u32 hit_count;
        s32 hit_index;
        s32 dir[3];
        bool held;
        int k;

        if (!p->in_use)
            continue;

        q2_sim_proj_scan.stepped++;
        held = (p->kind == Q2_PROJ_HAND_GRENADE &&
                p->node == Q2_PROJ_NODE_HELD);

        /*
         * The light, from the preset the sweep at 0x80047C6C reads out of
         * 0x800AE954 -- warm orange, outer radius 800. Raised before the step
         * so it sits where the projectile was drawn this frame rather than
         * where it is about to be.
         *
         * GATED, for a bolt, on bit 0x2 of its flags (0x800481C0). Both bolt
         * values carry it, so nothing changes today; the gate is here because
         * the same halfword decides the body and the trail and reading one bit
         * of it in one place and not the others is how they drift apart.
         */
        if (!held && (p->kind != Q2_PROJ_BOLT ||
                      (p->flags & Q2_PROJ_FLAG_LIGHT))) {
            static const u8 glow[3]     = { Q2_PROJ_LIGHT_R, Q2_PROJ_LIGHT_G,
                                            Q2_PROJ_LIGHT_B };
            static const u8 bfg_glow[3] = { Q2_PROJ_BFG_LIGHT_R,
                                            Q2_PROJ_BFG_LIGHT_G,
                                            Q2_PROJ_BFG_LIGHT_B };
            bool bfg = (p->kind == Q2_PROJ_BFG);

            q2_ent_light_at(&sim->ent_world.events, p->pos,
                            bfg ? bfg_glow : glow,
                            bfg ? Q2_PROJ_BFG_LIGHT_INNER
                                : Q2_PROJ_LIGHT_INNER,
                            bfg ? Q2_PROJ_BFG_LIGHT_OUTER
                                : Q2_PROJ_LIGHT_OUTER);
        }

        /*
         * AND THE TRAIL, which is the blaster bolt's only body.
         *
         * AFTER THE MOVE, NOT BEFORE IT. This used to run above the step,
         * under a note saying 0x80048334 takes `pos - disp/2` with pos "still
         * the old one". It is not: the sweep commits the new position to the
         * record at 0x80047EE8 and 0x80047F30 — the contact arm and the clear
         * arm — and BOTH of those are above the flags dispatch at 0x80047F44,
         * so by the time the bit-0x1 arm loads 0(s6)/4(s6)/8(s6) at
         * 0x800482E8 the record already holds where the bolt ended up. The
         * group therefore spans `[newpos - disp, newpos]`, which is exactly
         * the segment just travelled; raising it first spanned
         * `[oldpos - disp, oldpos]`, the segment travelled on the PREVIOUS
         * tick, and left a whole step of clear air between a bolt and the
         * trail behind it.
         */
        q2_projectile_step(&sim->combat.projectiles, i, sim->gravity,
                           sim->cur_dt, sim->level_time, &step);

        if (!held && (p->flags & Q2_PROJ_FLAG_TRAIL) && sim->fx_ready) {
            s16 disp[3];
            int d;

            for (d = 0; d < 3; d++) {
                /* The sweep's own `vel * dt`, 0x80047D50 against
                 * [0x800B2DB4], truncated to a halfword by the `sh` at
                 * 0x80047D58, in the velocity's raw halfword units. */
                s32 raw_vel = q2_projectile_raw_velocity(p, d);

                disp[d] = (s16)(raw_vel * sim->cur_dt);
            }
            q2_fx_bolt_trail(&sim->fx, &sim->combat.rng, p->pos, disp,
                             fx_area_resolve(sim, 0, p->pos));
        }

        /* State 1 has no mover, collision body, light or visible model. The
         * owner update after the view-model step attaches it and resolves an
         * elapsed fuse at the current hand position. */
        if (held)
            continue;

        if (step.expired) {
            q2_sim_proj_scan.expired++;
            /* Grenade2's +0xF4 is a fuse and calls its explosion. Rocket and
             * BFGBlast use the same-looking field only as a safety lifetime:
             * 0x8004ADB0 / 0x8004B8E4 free them without damage or an effect.
             * The bolt lifetime is quiet for the same reason. */
            if (p->kind == Q2_PROJ_GRENADE ||
                p->kind == Q2_PROJ_HAND_GRENADE) {
                q2_fx_preset_id fx = fx_for_projectile(p->kind);
                s32 where[3];
                q2_actor **targets = sim->world_targets
                                          ? sim->world_targets
                                          : sim->combat.targets;
                u32 count = sim->world_targets ? sim->world_target_count
                                               : sim->combat.target_count;

                memcpy(where, p->pos, sizeof(where));
                projectile_owner_splash(sim, p, where, targets, count);
                q2_projectile_detonate_traced(&sim->combat.projectiles, i,
                                              attacker_for(sim, p->owner),
                                              targets, count,
                                              &sim->combat.rules,
                                              splash_clear, sim);
                fx_at(sim, fx, where);
            } else {
                q2_projectile_expire(&sim->combat.projectiles, i);
            }
            continue;
        }

        /*
         * The BFG's beams — the game's weapon trail, and MOST OF ITS DAMAGE.
         *
         * 0x8004BD04 calls the beam maintainer every tick while the ball flies,
         * and it holds a green beam on every target it can see, refreshing each
         * one rather than adding a second (effect.h). The beams outlive the
         * ball's passage by their own timer, which is what makes the BFG leave
         * a lattice behind it rather than a single line.
         *
         * THE SAME PASS HURTS WHAT IT BEAMS, and that half was missing: this
         * arm was one q2_fx_beam_timed call, so the lattice was decoration and
         * the weapon was worth its contact hit plus one 1300-radius blast. On
         * the disc a candidate that clears the filters takes 10 points (5 in
         * deathmatch) every tenth of a second while the ball is in the air —
         * 0x80049DE4..0x80049E38, `a3 = 1` so mod 1, `sw zero, 16(sp)` so no
         * point and therefore no knockback.
         *
         * The beam and the damage are SEPARATELY gated: 0x80049DBC `beq v1,
         * zero` falls through to the damage when the 12-slot beam table is
         * full, so the damage cannot live under `fx_ready` the way the beam
         * does.
         *
         * The port's visibility test is the same segment sweep the projectile
         * itself uses, because it has no separate line-of-sight query; the
         * original calls 0x80051874 at 0x80049CE0, and failing it branches past
         * BOTH halves. Called out as the one substitution — and until now the
         * test was only described here, never performed, so beams were held
         * through walls.
         */
        if (p->kind == Q2_PROJ_BFG) {
            q2_actor **list  = sim->world_targets ? sim->world_targets
                                                  : sim->combat.targets;
            u32        count = sim->world_targets ? sim->world_target_count
                                                  : sim->combat.target_count;
            const q2_actor *parked_self = &sim->pcombat[sim->cur_player].self;
            q2_actor *owner = attacker_for(sim, p->owner);
            /* 0x80049DE8 `lw 0x800AEBCC` then 0x80049E10 `addiu a2, zero, 5`
             * against 0x80049E28's 10 — read from the port's own stand-in for
             * that global rather than from the rules copy, as the console reads
             * the word itself at the site. */
            s16  beam_damage = sim->multiplayer ? 5 : 10;
            bool hurt;
            u32  t;

            /*
             * 0x8004BCF0..0x8004BD08: the frame delta at 0x800B2DB4 is added
             * into the ball's halfword at +0x4C and stored back in the
             * maintainer's own delay slot, so the pass below already sees this
             * tick's dt in it.
             */
            p->beam_time = (u16)(p->beam_time + (u16)sim->cur_dt);

            /* 0x80049E00 / 0x80049E1C `slti v0, v0, 30` on the SIGNED halfword:
             * below 30 the branch skips the damage but not the beam. */
            hurt = (s16)p->beam_time >= 30;

            for (t = 0; t < count; t++) {
                q2_actor *a = list ? list[t] : NULL;
                s64 dx, dy, dz;

                /* 0x80049C54 `beq a1, s3, 0x80049E3C`: the ball never beams or
                 * hurts its own owner. The world list names the live player in
                 * deathmatch, so without this the shooter would scythe himself
                 * for the length of his own shot. */
                if (!a || a == owner)
                    continue;

                /*
                 * The live player can appear in a world list under two names,
                 * and the parked `pcombat[cur_player].self` is the stale one:
                 * player_damage_end maps the current player to `combat.self`,
                 * so damage landed on the parked copy is never written back,
                 * and damage landed on both is applied twice. The presentation
                 * pass above skips the same pair for the same reason.
                 */
                if (a == parked_self)
                    continue;

                /*
                 * 0x80049C5C..0x80049CB4: squared origin distance against
                 * `lui v0, 0x90` = 0x900000 = 3072^2. The console ALSO box-
                 * tests the candidate's own bounds first (0x80049C3C, the
                 * 0x800552D8 overlap against entity+0x78) against a +/-3072 box
                 * around the ball; port actors carry no bounds, so only the
                 * sphere is reproduced. It is the tighter of the two for a
                 * point, which is what an actor here is. In s64 because the sum
                 * of three squared world spans does not fit an s32.
                 */
                dx = (s64)p->pos[0] - a->origin[0];
                dy = (s64)p->pos[1] - a->origin[1];
                dz = (s64)p->pos[2] - a->origin[2];
                if (dx * dx + dy * dy + dz * dz > (s64)3072 * 3072)
                    continue;

                /*
                 * THE ONE PLACE THE CONSOLE READS `takedamage` OUTSIDE
                 * T_Damage, so this filter can be the original's rather than a
                 * stand-in: the beam maintainer at 0x80049B9C does
                 * `lw v0, 748(a1)` / `lw v0, 28(v0)` / `and 0xC0000000` /
                 * `beq v0, zero` at 0x80049CBC..0x80049CD8. It asks whether the
                 * thing can be hurt, not whether it is alive.
                 */
                if (!a->takedamage)
                    continue;

                /* 0x80049CE0 `jal 0x80051874`, the visibility test. */
                if (!splash_clear(sim, p->pos, a->origin))
                    continue;

                /*
                 * The area comes from the BALL, which is what 0x80048D24
                 * resolves on the console: the point-clip helper 0x8004E920
                 * runs the owner entity's position through PrimaryColl and
                 * hands the cell's area record to the beam queue as its fourth
                 * argument. Resolved from the ball's position here, on each
                 * refresh, rather than per submit — see q2_fx_timed_beam.area.
                 */
                if (sim->fx_ready)
                    q2_fx_beam_timed(&sim->fx, (s32)i, (s32)t,
                                     p->pos, a->origin,
                                     Q2_FX_TIMED_BEAM_RADIUS,
                                     Q2_FX_TIMED_BEAM_STYLE,
                                     Q2_FX_TIMED_BEAM_LIFE,
                                     fx_area_resolve(sim, 0, p->pos));

                /* 0x80049E34, the damage the beam does when it lands. */
                if (hurt)
                    q2_combat_damage(owner, a, beam_damage,
                                     Q2_MOD_ENERGY_BOLT, NULL,
                                     &sim->combat.rules);
            }

            /*
             * 0x8004BD0C..0x8004BD3C. Two things the obvious reading gets
             * wrong: the drain is a LOOP (0x8004BD3C branches back to
             * 0x8004BD20), and its threshold is 31, ONE MORE than the damage
             * gate's 30. So an accumulator that lands exactly on 30 is not
             * drained and fires again on the very next tick before resetting,
             * which makes the beams hurt somewhat oftener than once per 30
             * ticks — at the PAL frame delta of 12, three ticks in every five.
             * That is the disc's arithmetic, not a rounding choice here.
             */
            while ((s16)p->beam_time >= 31)
                p->beam_time = (u16)(p->beam_time - 30);
        }

        /* A creature in the way takes it before the world does. */
        for (k = 0; k < 3; k++)
            dir[k] = step.to[k] - step.from[k];

        hit_list  = sim->world_targets ? sim->world_targets
                                       : sim->combat.targets;
        hit_count = sim->world_targets ? sim->world_target_count
                                       : sim->combat.target_count;

        hit_index = q2_combat_nearest_on_segment(step.from, dir,
                                                 Q2_HITSCAN_RADIUS,
                                                 hit_list, hit_count);

        /* Never its own shooter: the world list holds everybody, including the
         * player who fired this. */
        if (hit_index >= 0 && hit_list[hit_index] == attacker_for(sim, p->owner))
            hit_index = -1;

        /*
         * How close it came, whether or not it counted. Measured against the
         * segment's LINE, so "near but past the end" separates a bolt that is
         * badly aimed from one that is aimed correctly and simply has not
         * arrived yet — which a short per-tick step makes the common case.
         */
        {
            u32 hi;
            s64 dl = (s64)dir[0] * dir[0] + (s64)dir[1] * dir[1] +
                     (s64)dir[2] * dir[2];

            /* The square is enough to compare; the length is only printed. */
            q2_sim_proj_scan.seg_len = (s32)dl;

            for (hi = 0; hi < hit_count; hi++) {
                q2_actor *t = hit_list[hi];
                s64 along = 0, d2;
                s64 reach;

                /* Same rule as the sweep in combat.c: a corpse is a target
                 * until its `takedamage` says otherwise, which is what lets a
                 * rocket finish the job on a body already down. */
                if (!t || t == attacker_for(sim, p->owner) || !t->takedamage)
                    continue;

                d2 = q2_combat_ray_dist_sq(step.from, dir, t->origin, &along);
                if (q2_sim_proj_scan.closest_sq == 0 ||
                    d2 < q2_sim_proj_scan.closest_sq) {
                    q2_sim_proj_scan.closest_sq        = d2;
                    q2_sim_proj_scan.closest_origin[0] = t->origin[0];
                    q2_sim_proj_scan.closest_origin[1] = t->origin[1];
                    q2_sim_proj_scan.closest_origin[2] = t->origin[2];
                    q2_sim_proj_scan.closest_from[0]   = step.from[0];
                    q2_sim_proj_scan.closest_from[1]   = step.from[1];
                    q2_sim_proj_scan.closest_from[2]   = step.from[2];
                    q2_sim_proj_scan.closest_owner     = p->owner;
                }

                reach = (s64)Q2_HITSCAN_RADIUS + t->radius;
                if (d2 <= reach * reach) {
                    q2_sim_proj_scan.near_miss++;
                    if (along > 4096)
                        q2_sim_proj_scan.past_end++;
                }
            }
        }

        if (hit_index >= 0) {
            q2_actor *victim = hit_list[hit_index];

            q2_sim_proj_scan.hit++;
            q2_fx_preset_id fx = fx_for_projectile(p->kind);

            projectile_owner_splash(sim, p, step.to, hit_list, hit_count);
            q2_projectile_impact_traced(&sim->combat.projectiles, i, step.to,
                                        NULL, attacker_for(sim, p->owner),
                                        victim, hit_list, hit_count,
                                        &sim->combat.rules,
                                        splash_clear, sim);

            /*
             * Two bursts, not three. The projectile's own and the victim's
             * blood are both real sites; the third used to be 0x800596B0 on the
             * killing blow, and that address is the ITEM MATERIALISE burst, not
             * a gib — see the note at fx_hitscan_impact and effect.h. What the
             * console raises when a body comes apart is the mesh blood spray
             * (q2_fx_gib_spray) at the head of ThrowGibs, behind 0x8007CEB4,
             * and that needs a posed model this file does not carry.
             */
            fx_at(sim, fx, step.to);
            if (victim)
                fx_at(sim, Q2_FX_BLOOD, step.to);
            continue;
        }

        if (sim->coll_ready || sim->coll_primary_ready) {
            s32 end[3], node = p->node;
            bool complete;
            q2_collision *phull = sim->coll_primary_ready
                                      ? &sim->coll_primary : &sim->coll;

            /*
             * Traced from the PROJECTILE's own cell, not the player's. A rocket
             * that has crossed the map is nowhere near the shooter, and asking
             * the hull to clip a segment starting in the shooter's cell answers
             * a different question — one whose answer is usually "no obstacle",
             * which is how a projectile ends up flying through walls.
             *
             * Through PrimaryColl, for the reason q2_sim_trace now states: a
             * bolt is a point and the eroded hull stopped it 286 units short of
             * every surface, leaving rockets to burst in mid-air short of the
             * wall and grenades to stop above the floor. `p->node` is cached
             * against this hull throughout, so the hint stays meaningful.
             */
            complete = q2_coll_move(phull, step.from, step.to, node,
                                    end, &node);
            p->node = node;

            /*
             * AND THE ENTITY BOXES. Doors, lifts and intact panes are not hull,
             * so the walk above otherwise flies a projectile through all of
             * them. Clipped from the step's start to wherever the hull left it,
             * so the nearer pass wins; `complete` goes false because a bolt
             * that met any solid entity has met something.
             */
            if (sim->move_world.count) {
                q2_move_seg_hit mh;
                const s32 *stop = complete ? step.to : end;

                if (q2_move_clip_segment(&sim->move_world, step.from, stop,
                                         NULL, &mh)) {
                    end[0]   = mh.pos[0];
                    end[1]   = mh.pos[1];
                    end[2]   = mh.pos[2];
                    complete = false;
                    q2_sim_proj_scan.stopped_on_entity++;
                }
            }

            /*
             * A pane in the way, tested over the step the projectile just took.
             * The console routes to a breakable from five weapon call sites and
             * a bolt's is one of them (0x80049B18), so this is not confined to
             * hitscan — and a blaster is the weapon a player has in hand when
             * they first meet a window.
             *
             * Tested against the step rather than the impact point, because a
             * pane is thinner than a step: at the projectile's speed the shot
             * would otherwise pass through it between one tick and the next.
             */
            if (sim->breakable_count) {
                u32 pieces = q2_sim_breakable_shot(sim, step.from,
                                                   complete ? step.to : end,
                                                   (s16)p->damage);
                if (pieces)
                    fx_at(sim, fx_for_projectile(p->kind), step.to);
            }

            if (!complete) {
                /* The hull gives back the clipped point; the port has no
                 * surface normal here, so a grenade landing on geometry stops
                 * rather than bouncing. Called out because the bounce sound is
                 * one of the twenty-two and the behaviour certainly exists. */
                bool consumed;
                q2_fx_preset_id fx = fx_for_projectile(p->kind);
                q2_actor **targets = sim->world_targets
                                          ? sim->world_targets
                                          : sim->combat.targets;
                u32 count = sim->world_targets ? sim->world_target_count
                                               : sim->combat.target_count;

                projectile_owner_splash(sim, p, end, targets, count);
                consumed = q2_projectile_impact_traced(
                    &sim->combat.projectiles, i, end, NULL,
                    attacker_for(sim, p->owner), NULL, targets, count,
                    &sim->combat.rules, splash_clear, sim);
                /* A grenade that only bounced has not gone off, so it must not
                 * leave a fireball behind. */
                if (consumed)
                    fx_at(sim, fx, end);
                continue;
            }
        }

        q2_projectile_commit(&sim->combat.projectiles, i, step.to);
    }

    /*
     * Debris, through PRIMARY collision.
     *
     * 0x80046DA0 installs 0x800C8E90 as the hull and the entity's +0xA0 as its
     * cell, where the player's path (0x80046DDC) installs SecondaryCol and
     * +0xA2. Tracing shards against the player's eroded hull would leave every
     * one of them floating 286 units off the floor.
     */
    for (i = 0; i < Q2_FX_DEBRIS_MAX; i++) {
        q2_fx_debris *d = &sim->fx.debris[i];
        q2_fx_debris_step step;

        if (!d->in_use)
            continue;

        q2_fx_debris_step_one(&sim->fx, i, sim->gravity, &step);

        if (step.expired) {
            d->in_use = false;
            continue;
        }

        if (sim->coll_primary_ready) {
            s32 end[3], node = d->node;

            if (!q2_coll_move(&sim->coll_primary, step.from, step.to, node,
                              end, &node)) {
                d->node = node;
                q2_fx_debris_impact(&sim->fx, i, end);
                continue;
            }
            d->node = node;
        }

        q2_fx_debris_commit(&sim->fx, i, step.to);
    }
    player_damage_end(sim);
}

/* ------------------------------------------------------------------------- */
u32 q2_sim_debris_burst(q2_sim *sim, const s32 bmin[3], const s32 bmax[3],
                        const s32 *at, u32 count, u8 area)
{
    if (!sim || !sim->fx_ready)
        return 0;
    return q2_fx_debris_burst(&sim->fx, &sim->fx_rng, bmin, bmax, at, count,
                              area);
}

u32 q2_sim_breakable_call(q2_sim *sim, const q2_scene *scene,
                          const q2_uf_operands *ops,
                          const q2_event_item *item, u8 call_index)
{
    const u8 *p;
    q2_scene_node node;
    s16 slot;
    u32 count_a, count_b, made = 0;

    if (!sim || !scene || !item || !item->payload || !sim->userfuncs_ready)
        return 0;

    /* Only GLASS. SHOOTTHEN's handler returns at 0x8002E840 when the damage
     * argument is zero, and a script CALL always passes zero, so running it
     * here would be inventing behaviour rather than reproducing it. */
    if (q2_userfuncs_prim(&sim->userfuncs, call_index) != Q2_UF_GLASS)
        return 0;

    /* The item is 16 bytes and the operands live at +4, +6, +10 and +12; the
     * payload points past the two header bytes, so a documented +N is
     * payload[N - 2] — the same convention q2_rotators_call uses. */
    if (item->len < 16)
        return 0;

    p = q2_uf_operand_at(ops, item->payload - 2, 16);

    slot = q2_rd_s16(p + 4);
    if (slot < 0 || !q2_scene_get_node(scene, (u32)slot, &node))
        return 0;

    count_a = q2_rd_u8(p + 10);
    count_b = q2_rd_u8(p + 12);

    /*
     * The node's own box, WITHOUT q2_scene_node_bounds' slop. That margin
     * exists so a node is not culled a pixel before its geometry reaches the
     * edge of it; the console's burst reads +16..+36 raw (0x800645A8), and
     * padding the box here would throw pieces from just outside the pane.
     */

    /*
     * The hit burst first, out of a point, and then the shatter across the
     * whole box — the two calls at 0x8002A384 and 0x8002A3DC in that order.
     * A script call carries no damage, so 0x8002A390 falls straight through
     * between them and both run.
     *
     * The point the hit burst comes out of is the runtime OBJECT, which this
     * port does not allocate; the node's centre is the nearest thing it has
     * and is where the destruction sound is placed (0x8002A4A4), so the two
     * agree. Stated because it is the one invented quantity here.
     */
    {
        s32 centre[3];
        int k;

        for (k = 0; k < 3; k++)
            centre[k] = (node.bbox_min[k] + node.bbox_max[k]) / 2;

        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                    centre, count_a, 0);

        /*
         * 0x8002A3C4 `lh v0, 16948(gp)` / 0x8002A3CC `bne` to 0x8002A4AC: while
         * the EVE_ replay runs, the shatter and the sound are skipped. The hit
         * burst above is not — it is 0x8002A384, before the test — so a spent
         * pane replayed across a zone seam puffs once and does not come apart
         * a second time. The console's flag is the global gp+16948; the port's
         * is the runtime's own copy of it (events_rt.h, `initial_pass`).
         */
        if (!sim->event_rt.initial_pass)
            made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max,
                                        NULL, count_b, 0);
    }

    return made;
}

/* ------------------------------------------------------------------------- */
/* Shooting a breakable — the port's 0x80053AA4 sweep and 0x8002EF1C router    */
/* ------------------------------------------------------------------------- */
static void breakable_solids_drop(q2_sim *sim)
{
    u32 first, count, i;

    if (!sim)
        return;

    first = sim->mover_count;
    count = sim->breakable_solid_count;
    if (count && sim->volumes && first <= sim->volume_count &&
        count <= sim->volume_count - first) {
        memmove(sim->volumes + first, sim->volumes + first + count,
                (sim->volume_count - first - count) * sizeof(*sim->volumes));
        sim->volume_count -= count;
    }

    for (i = 0; i < sim->breakable_count; i++)
        sim->breakable[i].solid_target = -1;
    sim->breakable_solid_count = 0;
    sim->move_world.targets = sim->volumes;
    sim->move_world.count   = sim->volume_count;
}

/*
 * GLASS owns an ordinary entry in retail's 48-slot entity-box table.
 * Insert those boxes after mover parts and before trigger volumes, preserving
 * the same entity-then-volume walk that q2_move_sweep_world implements.
 */
static bool breakable_solids_add(q2_sim *sim)
{
    q2_move_target *grown;
    u32 count = 0, first, out = 0, i;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_GLASS)
            count++;
    if (!count)
        return true;

    first = sim->mover_count;
    if (first > sim->volume_count)
        return false;

    grown = (q2_move_target *)calloc(sim->volume_count + count,
                                     sizeof(*grown));
    if (!grown)
        return false;

    if (sim->volumes && first)
        memcpy(grown, sim->volumes, first * sizeof(*grown));
    if (sim->volumes && sim->volume_count > first)
        memcpy(grown + first + count, sim->volumes + first,
               (sim->volume_count - first) * sizeof(*grown));

    for (i = 0; i < sim->breakable_count; i++) {
        q2_breakable *b = &sim->breakable[i];
        q2_move_target *t;
        int k;

        if (b->kind != Q2_BREAKABLE_GLASS)
            continue;

        t = &grown[first + out];
        for (k = 0; k < 3; k++) {
            t->min[k] = t->env_min[k] = b->bmin[k];
            t->max[k] = t->env_max[k] = b->bmax[k];
        }
        t->dy           = 0;
        t->mask         = 0;
        t->kind         = Q2_MOVE_KIND_ENTITY;
        t->id           = (s32)i;
        t->active       = !b->broken;
        b->solid_target = (s32)(first + out);
        out++;
    }

    free(sim->volumes);
    sim->volumes                = grown;
    sim->volume_count          += out;
    sim->breakable_solid_count  = out;
    sim->move_world.targets     = sim->volumes;
    sim->move_world.count       = sim->volume_count;
    return true;
}

void q2_sim_breakables_sync_solidity(q2_sim *sim)
{
    u32 i;

    if (!sim || !sim->volumes)
        return;

    for (i = 0; i < sim->breakable_count; i++) {
        q2_breakable *b = &sim->breakable[i];

        if (b->kind != Q2_BREAKABLE_GLASS || b->solid_target < 0 ||
            (u32)b->solid_target >= sim->volume_count)
            continue;
        sim->volumes[b->solid_target].active = !b->broken;
    }
}

u32 q2_sim_attach_breakables(q2_sim *sim, const q2_scene *scene,
                             const q2_uf_operands *ops)
{
    q2_event_record rec, prev;
    bool more;
    u32 bi;

    if (!sim)
        return 0;

    breakable_solids_drop(sim);
    sim->breakable_count  = 0;
    sim->breakable_hits   = 0;
    sim->breakable_pieces = 0;
    sim->breakable_fired  = 0;
    sim->breakable_scene  = scene;

    /* The explosive set is rebuilt per zone by the owner, so anything the last
     * zone left pointing at it is stale. */
    sim->explosives           = NULL;
    sim->node_vis_count       = 0;
    sim->blast_count          = 0;
    sim->explosive_destroyed  = 0;
    sim->explosive_blasts     = 0;

    for (bi = 0; bi < Q2_SIM_MAX_BREAKABLES; bi++)
        sim->breakable[bi].solid_target = -1;

    if (!scene || !sim->events_ready || !sim->userfuncs_ready)
        return 0;

    for (more = q2_events_first_record(&sim->events, &rec);
         more;
         more = q2_events_next_record(&sim->events, &prev, &rec)) {
        u32 i;

        prev = rec;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            q2_scene_node node;
            q2_breakable *b;
            const u8 *p;
            q2_uf_prim prim;
            u8  call_index;
            u32 need;
            s16 slot;
            int k;

            if (!q2_events_get_item(&sim->events, &rec, i, &item))
                break;
            if (!item.payload)
                continue;

            /*
             * A SHOOTABLE DOOR OR BUTTON IS A MOVER_A, not a CALL.
             *
             * 0x80025E98 tests the item's s16 at +20 for > 0 and, when it is,
             * installs the damage callback at object+0x24 and flags the box
             * 0x4 — and bit 0x4 is the only thing a weapon impact gates on. So
             * a panel you shoot to open a door is an ordinary MOVER_A with hit
             * points, and this loop never looked at one. Fourteen of them, in
             * ten maps.
             *
             * The hit points come from `p`, the WALKED copy: 0x80025E98 is
             * `lh v0, 20(s0)` with s0 the item in the record being walked, and
             * only the object slots are rebased (mover.h). The node slot is
             * therefore read from the rebased pointer and the health is not.
             */
            if (item.opcode == Q2_EVOP_MOVER_A) {
                const u8 *mp = item.payload - 2;
                const u8 *mq;
                s16 hp, mslot;

                if (item.len < 24)
                    continue;
                hp = q2_rd_s16(mp + 20);
                if (hp <= 0)
                    continue;               /* an ordinary door */
                if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                    break;

                mq    = q2_uf_operand_at(ops, mp, item.len);
                mslot = q2_rd_s16(mq + 8);   /* the first node slot */
                if (mslot < 0 || !q2_scene_get_node(scene, (u32)mslot, &node))
                    continue;

                b = &sim->breakable[sim->breakable_count];
                memset(b, 0, sizeof(*b));
                b->solid_target = -1;
                b->scene_node = mslot;
                for (k = 0; k < 3; k++) {
                    b->bmin[k] = node.bbox_min[k];
                    b->bmax[k] = node.bbox_max[k];
                }
                b->health       = hp;
                b->kind         = (u8)Q2_BREAKABLE_MOVER;
                b->item_offset  = item.offset;
                b->record_offset = rec.offset;
                sim->breakable_count++;
                continue;
            }

            if (item.opcode != Q2_EVOP_CALL)
                continue;
            if (!q2_events_get_call_index(&item, &call_index))
                continue;
            prim = q2_userfuncs_prim(&sim->userfuncs, call_index);
            if (prim != Q2_UF_GLASS && prim != Q2_UF_SHOOTTHEN)
                continue;
            need = (prim == Q2_UF_GLASS) ? 16u : 8u;
            if (item.len < need)
                continue;
            if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                break;

            /* The same rebase the scripted call uses: four of the disc's ten
             * object slots read -1 in COMMON's copy (#66). */
            p = q2_uf_operand_at(ops, item.payload - 2, need);

            slot = q2_rd_s16(p + 4);
            if (slot < 0 || !q2_scene_get_node(scene, (u32)slot, &node))
                continue;

            /*
             * The box, straight off the Scene node and WITHOUT the culling
             * slop q2_scene_node_bounds adds — 0x800555D8 copies the node
             * record's own numbers, and a padded box would be shootable from
             * just outside the pane.
             */
            b = &sim->breakable[sim->breakable_count];
            memset(b, 0, sizeof(*b));
            b->solid_target = -1;
            b->scene_node = slot;
            for (k = 0; k < 3; k++) {
                b->bmin[k] = node.bbox_min[k];
                b->bmax[k] = node.bbox_max[k];
            }
            b->health = q2_rd_s16(p + 6);
            b->kind   = (prim == Q2_UF_GLASS) ? (u8)Q2_BREAKABLE_GLASS
                                              : (u8)Q2_BREAKABLE_SHOOTTHEN;
            if (prim == Q2_UF_GLASS) {
                b->count_a = q2_rd_u8(p + 10);
                b->count_b = q2_rd_u8(p + 12);
            } else {
                /*
                 * SHOOTTHEN's constructor caches the record it is being built
                 * in (gp+16936) in obj+0x40, and its exec hands that back to
                 * the record dispatcher. Here the walk already knows which
                 * record the item came out of, so the cache is the loop's own
                 * variable.
                 */
                b->record_offset = rec.offset;
            }
            sim->breakable_count++;
        }
    }

    /* Allocation failure leaves the pane shootable and visible, but cannot
     * manufacture a valid solid target. The registry count is still the
     * function's documented return value. */
    (void)breakable_solids_add(sim);
    return sim->breakable_count;
}

/* ------------------------------------------------------------------------- */
/* The `func_explosive` groups — opcode 0x08                                  */
/* ------------------------------------------------------------------------- */
u32 q2_sim_attach_explosives(q2_sim *sim, q2_explosive_set *set,
                             const q2_scene *scene)
{
    u32 i, added = 0;

    if (!sim)
        return 0;

    sim->explosives = set;
    if (!set || !scene)
        return 0;

    if (!sim->breakable_scene)
        sim->breakable_scene = scene;

    for (i = 0; i < set->count; i++) {
        const q2_explosive *e = &set->items[i];
        int k;

        /*
         * A group with no hit points has no damage callback — 0x80026B10
         * branches past the arm that installs one — so a shot must find no box
         * to hit. Registering it anyway would make every scripted explosive
         * shootable, which is exactly the distinction the constructor draws.
         */
        if (!e->damageable)
            continue;

        for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
            q2_scene_node node;
            q2_breakable *b;
            int c;

            if (e->node[k] < 0)
                continue;
            if (sim->breakable_count >= Q2_SIM_MAX_BREAKABLES)
                return added;
            if (!q2_scene_get_node(scene, (u32)e->node[k], &node))
                continue;

            b = &sim->breakable[sim->breakable_count];
            memset(b, 0, sizeof(*b));
            b->solid_target = -1;
            b->scene_node = e->node[k];
            for (c = 0; c < 3; c++) {
                /* 0x800555D8 copies the node record's own six s32 from +16, so
                 * NOT q2_scene_node_bounds, which inflates for culling. */
                b->bmin[c] = node.bbox_min[c];
                b->bmax[c] = node.bbox_max[c];
            }
            b->health       = e->health;
            b->kind         = (u8)Q2_BREAKABLE_FXGROUP;
            b->part         = (u8)k;
            b->owner        = (s16)i;
            b->item_offset  = e->item_offset;
            b->record_offset = e->record_offset;
            sim->breakable_count++;
            added++;
        }
    }

    return added;
}

static void node_vis_push(q2_sim *sim, s16 node, u8 hidden)
{
    u32 cap = sizeof(sim->node_vis) / sizeof(sim->node_vis[0]);

    if (node < 0 || sim->node_vis_count >= cap)
        return;
    sim->node_vis[sim->node_vis_count].node   = node;
    sim->node_vis[sim->node_vis_count].hidden = hidden;
    sim->node_vis_count++;
}

bool q2_sim_next_node_vis(q2_sim *sim, s16 *node, u8 *hidden)
{
    u32 i;

    if (!sim || !sim->node_vis_count)
        return false;

    if (node)   *node   = sim->node_vis[0].node;
    if (hidden) *hidden = sim->node_vis[0].hidden;

    /* A queue rather than a stack: the console applies the hide and the show in
     * the order the handler emits them, and a group that reveals the node it
     * also hides would otherwise settle the wrong way. */
    for (i = 1; i < sim->node_vis_count; i++)
        sim->node_vis[i - 1] = sim->node_vis[i];
    sim->node_vis_count--;
    return true;
}

bool q2_sim_next_blast(q2_sim *sim, s32 out[3])
{
    u32 i;

    if (!sim || !sim->blast_count)
        return false;

    if (out) {
        out[0] = sim->blast_at[0][0];
        out[1] = sim->blast_at[0][1];
        out[2] = sim->blast_at[0][2];
    }
    for (i = 1; i < sim->blast_count; i++) {
        sim->blast_at[i - 1][0] = sim->blast_at[i][0];
        sim->blast_at[i - 1][1] = sim->blast_at[i][1];
        sim->blast_at[i - 1][2] = sim->blast_at[i][2];
    }
    sim->blast_count--;
    return true;
}

/*
 * Turn one destruction into effects and visibility changes.
 *
 * The port's stand-in for `0x8005A778` is the PARTICLE explosion — see
 * explosive.h for why, and for what changes here when the model-entity
 * explosion exists.
 */
static void explosive_apply(q2_sim *sim, const q2_explosive_result *res)
{
    u32 i;

    for (i = 0; i < res->burst_count; i++) {
        const q2_explosive_burst *b = &res->burst[i];
        q2_scene_node node;

        if (b->explode) {
            /*
             * THE MODEL ENTITY, AND NOTHING ELSE.
             *
             * 0x800267C4 makes exactly five calls and this is the whole list:
             * 0x80064558 twice (the hit burst and the destruction debris),
             * 0x8005A778 once, 0x80073704 for the report, and 0x80068818 to
             * hide the node. A PARTICLE BURST IS NOT AMONG THEM.
             *
             * `fx_at(sim, Q2_FX_EXPLOSION, ...)` used to run here. It was a
             * deliberate stand-in from when this port had no model entities and
             * explosive.h said so out loud — and then it survived the arrival
             * of the real thing, so a detonation drew the ROCKET's burst
             * (0x800486EC, a different site entirely) on top of the model the
             * console actually spawns. That is a bright cyan ball the console
             * never puts there, and it swamped the fireball behind it.
             *
             * A map whose CastList carries no `Explosion` gets no entity and no
             * substitute, which is the console's own behaviour: 0x8005A894
             * abandons the spawn.
             */
            if (sim->entities_ready &&
                q2_model_ent_spawn(&sim->entities, sim->model_bank,
                                   Q2_MODEL_ENT_EXPLOSION, b->at,
                                   (u8)b->area))
                sim->explosive_models++;

            sim->explosive_blasts++;

            /* And the report, at the same point (0x8002695C). Queued for the
             * owner, which is the half that has a mixer and a sound bank. */
            if (sim->blast_count <
                    sizeof(sim->blast_at) / sizeof(sim->blast_at[0])) {
                int k;

                for (k = 0; k < 3; k++)
                    sim->blast_at[sim->blast_count][k] = b->at[k];
                sim->blast_count++;
            }
        }

        /* The origin argument is zero at 0x80026970, so the pieces scatter
         * through the node's whole box rather than out of a point. */
        if (sim->breakable_scene && b->pieces &&
            q2_scene_get_node(sim->breakable_scene, (u32)b->node, &node))
            sim->breakable_pieces +=
                q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, NULL,
                                    b->pieces, 0);
    }

    for (i = 0; i < res->hide_count; i++)
        node_vis_push(sim, res->hide[i], 1);
    for (i = 0; i < res->show_count; i++)
        node_vis_push(sim, res->show[i], 0);

    if (res->destroyed)
        sim->explosive_destroyed++;
}

/* Mark every registered box of a destroyed group dead, so a second shot finds
 * nothing — 0x80068818 frees each node's box as it hides it. */
static void explosive_free_boxes(q2_sim *sim, u32 item_offset)
{
    u32 i;

    for (i = 0; i < sim->breakable_count; i++)
        if (sim->breakable[i].kind == Q2_BREAKABLE_FXGROUP &&
            sim->breakable[i].item_offset == item_offset)
            sim->breakable[i].broken = true;
}

bool q2_sim_explosive_trigger_item(q2_sim *sim, u32 item_offset)
{
    q2_explosive_result res;

    if (!sim || !sim->explosives)
        return false;

    if (!q2_explosive_trigger_item(sim->explosives, item_offset, false,
                                   sim->breakable_scene, &res))
        return false;

    explosive_apply(sim, &res);
    explosive_free_boxes(sim, item_offset);
    return true;
}

/*
 * Segment against an axis-aligned box: the standard slab test, in the fixed
 * point everything here is in.
 *
 * The console's own test is 0x80052078 and is not transcribed. What matters for
 * the behaviour is which box the shot crosses first, and that is a property of
 * the geometry rather than of the arithmetic; where the two could differ is a
 * shot that grazes an edge.
 */
static bool segment_hits_box(const s32 from[3], const s32 to[3],
                             const s32 bmin[3], const s32 bmax[3],
                             s32 out[3])
{
    /* Parametrised in 1.0.12 along the segment, as every other fraction in
     * this file is. */
    s32 t0 = 0, t1 = 4096;
    int k;

    for (k = 0; k < 3; k++) {
        s32 d = to[k] - from[k];

        if (d == 0) {
            if (from[k] < bmin[k] || from[k] > bmax[k])
                return false;
            continue;
        }
        {
            s64 a = ((s64)(bmin[k] - from[k]) << 12) / d;
            s64 b = ((s64)(bmax[k] - from[k]) << 12) / d;
            s32 lo = (s32)(a < b ? a : b);
            s32 hi = (s32)(a < b ? b : a);

            if (lo > t0) t0 = lo;
            if (hi < t1) t1 = hi;
            if (t0 > t1)
                return false;
        }
    }

    if (t0 < 0 || t0 > 4096)
        return false;

    for (k = 0; k < 3; k++)
        out[k] = from[k] + (s32)((((s64)(to[k] - from[k])) * t0) >> 12);
    return true;
}

u32 q2_sim_breakable_shot(q2_sim *sim, const s32 from[3], const s32 to[3],
                          s16 damage)
{
    q2_scene_node node;
    s32 best_at[3] = { 0, 0, 0 };
    s32 at[3];
    u32 i, made = 0;
    int best = -1;
    s64 best_d2 = 0;

    if (!sim || !sim->breakable_scene || !sim->breakable_count || !from || !to)
        return 0;

    /* The sweep: every in-use slot, nearest crossing wins — 0x80053AA4 walks
     * all 48 and keeps the closest for the same reason. */
    for (i = 0; i < sim->breakable_count; i++) {
        const q2_breakable *b = &sim->breakable[i];
        s64 d2;
        int k;

        if (b->broken)
            continue;
        if (!segment_hits_box(from, to, b->bmin, b->bmax, at))
            continue;

        d2 = 0;
        for (k = 0; k < 3; k++) {
            s64 d = (s64)at[k] - from[k];
            d2 += d * d;
        }
        if (best < 0 || d2 < best_d2) {
            best    = (int)i;
            best_d2 = d2;
            best_at[0] = at[0]; best_at[1] = at[1]; best_at[2] = at[2];
        }
    }

    if (best < 0)
        return 0;

    {
        q2_breakable *b = &sim->breakable[best];

        if (!q2_scene_get_node(sim->breakable_scene, (u32)b->scene_node, &node))
            return 0;

        sim->breakable_hits++;

        /*
         * SHOOTTHEN is the other primitive with a box, and it throws nothing:
         * `0x8002E81C` subtracts the damage from the item's hit points and, at
         * zero, frees the box and hands the record it was constructed in to the
         * record dispatcher. Shoot the panel, and whatever the rest of that
         * record does happens.
         */
        /*
         * A SHOOTABLE LEAF. 0x8002F050 subtracts the amount from the item's
         * own hit points and opens the door when they reach zero.
         *
         * The box is NOT freed and `broken` is not set: the console leaves the
         * counter at or below zero in the item, so every later shot re-opens
         * the leaf. That is the behaviour, and it is what makes a shoot-to-open
         * door work twice.
         *
         * The open itself is queued rather than done here — the mover set is
         * the caller's, not the sim's.
         */
        if (b->kind == Q2_BREAKABLE_MOVER) {
            if (damage == 0)
                return 0;
            b->health = (s16)(b->health - damage);
            if (b->health > 0)
                return 0;

            if (sim->breakable_open_count <
                    sizeof(sim->breakable_open) / sizeof(sim->breakable_open[0]))
                sim->breakable_open[sim->breakable_open_count++] =
                    b->item_offset;

            /*
             * AND ITS RECORD RUNS ON TOO. 0x8002F050 is the fourth damage
             * callback with 0x800267C4's contract: 0x8002F084 returns 0 while
             * the leaf stands, and the fatal hit falls through to 0x8002F144
             * `jr ra` / 0x8002F148 `addiu v0, a0, 24` — item + 24, the next
             * item — so 0x8002EFA8 lets it through to the executor exactly as
             * it does a crate's. A shoot-to-open door whose record carries a
             * STRING or a second mover behind it runs those as well.
             */
            if (sim->events_ready)
                (void)q2_event_rt_resume_after_item(&sim->event_rt,
                                                    b->record_offset,
                                                    b->item_offset, NULL);
            return 0;
        }

        /*
         * A `func_explosive`. The router (0x8002EF1C) hands the callback the
         * ITEM, the node whose box was hit and the amount, and everything the
         * exec does with them lives in explosive.c — including the hit burst,
         * which comes out of the node's whole box rather than the impact point
         * the way GLASS's does.
         */
        if (b->kind == Q2_BREAKABLE_FXGROUP) {
            q2_explosive_result res;
            q2_scene_node hit;

            if (!sim->explosives || b->owner < 0)
                return 0;

            if (!q2_explosive_damage(sim->explosives, (u32)b->owner,
                                     (int)b->part, damage, false,
                                     sim->breakable_scene, &res)) {
                /* Survived. The per-hit burst still ran — 0x80026820 precedes
                 * the survival test. */
                if (res.hit_pieces && res.hit_node >= 0 &&
                    sim->breakable_scene &&
                    q2_scene_get_node(sim->breakable_scene,
                                      (u32)res.hit_node, &hit))
                    made += q2_sim_debris_burst(sim, hit.bbox_min, hit.bbox_max,
                                                NULL, res.hit_pieces, 0);
                b->health = sim->explosives->items[b->owner].health;
                sim->breakable_pieces += made;
                return made;
            }

            if (res.hit_pieces && res.hit_node >= 0 &&
                sim->breakable_scene &&
                q2_scene_get_node(sim->breakable_scene,
                                  (u32)res.hit_node, &hit))
                made += q2_sim_debris_burst(sim, hit.bbox_min, hit.bbox_max,
                                            NULL, res.hit_pieces, 0);

            explosive_apply(sim, &res);
            explosive_free_boxes(sim, b->item_offset);
            sim->breakable_pieces += made;

            /*
             * AND THE REST OF THE RECORD RUNS, because a weapon killed it.
             *
             * 0x800267C4 returns `item + 28` rather than 0 once the
             * destruction loop has run (0x800269FC), and the weapon-impact
             * router at 0x8002EFA8 takes that as a cue: it reads the record
             * offset out of obj+0x40 and hands the record back to the
             * executor at the NEXT item (0x8002EFC8, a2 = item + len). The
             * visibility swap and the freed boxes happen first, exactly as
             * they do above, because the console's exec does them itself and
             * only then returns to the router.
             *
             * This is what spawns BASE0's jacket armour: record +508 is
             * [FXGROUP, CALL CREBATCH "Amour", CALL INSECRET] and nothing on
             * the map can reach it except a shot. Ten records on the disc
             * have a tail behind a shootable 0x08 item.
             */
            if (sim->events_ready &&
                q2_event_rt_resume_after_item(&sim->event_rt,
                                              b->record_offset,
                                              b->item_offset, NULL))
                sim->breakable_fired++;
            return made;
        }

        if (b->kind == Q2_BREAKABLE_SHOOTTHEN) {
            if (damage == 0)
                return 0;                    /* 0x8002E840: a script call */
            b->health = (s16)(b->health - damage);
            if (b->health > 0)
                return 0;

            b->broken = true;                /* the box is freed, not reused */
            if (sim->events_ready &&
                q2_event_rt_trigger(&sim->event_rt, b->record_offset))
                sim->breakable_fired++;
            return 0;
        }

        /*
         * The hit burst runs on EVERY call — 0x8002A384 is before the branch
         * that tests the damage — and comes out of the crossing point. Then the
         * hit points, which the console subtracts in the ITEM rather than in
         * the object, so a pane that has taken two shots remembers it.
         */
        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, best_at,
                                    b->count_a, 0);

        if (damage != 0) {
            b->health = (s16)(b->health - damage);
            if (b->health > 0) {
                /* The hit burst this call made still counts. Returning
                 * straight out skipped the accumulator at the foot of the
                 * function, so every non-fatal hit on a pane threw its piece
                 * and reported none. */
                sim->breakable_pieces += made;
                return made;
            }
        }

        /* The shatter, across the whole box: 0x8002A3DC passes zero for the
         * origin and the burst then scatters uniformly through the node. */
        made += q2_sim_debris_burst(sim, node.bbox_min, node.bbox_max, NULL,
                                    b->count_b, 0);
        b->broken = true;
        q2_sim_breakables_sync_solidity(sim);
    }

    sim->breakable_pieces += made;
    return made;
}
