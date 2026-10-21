/*
 * test_save.c — capturing, writing, reading back and applying a save.
 *
 * The round trip is the point. A save format that writes without error and
 * reads back subtly different values is worse than one that fails, because the
 * damage shows up as a corrupted game rather than an error message.
 *
 * The tests below are in three groups:
 *
 *   the format    every field survives the file, including the ones a first
 *                 pass at this forgot: the level clock, the mover state, the
 *                 trigger residency, the entity set
 *   the slots     four rows, their text, and what an empty or corrupt one does
 *   the flow      the front end's three entry points, driven through both a
 *                 save and a load without a screen anywhere near it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mission.h"
#include "explosive.h"
#include "modelent.h"
#include "save.h"
#include "saveui.h"
#include "ai.h"
#include "creworld.h"
#include "random.h"

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

static const char *tmp_dir(void)
{
    static char path[512];
    const char *dir = getenv("TEMP");

    if (!dir || !*dir)
        dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = ".";
    snprintf(path, sizeof(path), "%s/q2psx_save_test", dir);
    return path;
}

/* Beside the slot directory rather than inside it: the loose-file tests run
 * before anything has created one, and q2_save_slot_write is what makes it. */
static const char *tmp_path(void)
{
    static char path[512];
    const char *dir = getenv("TEMP");

    if (!dir || !*dir)
        dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = ".";
    snprintf(path, sizeof(path), "%s/q2psx_save_test.sav", dir);
    return path;
}

/* Every slot file this run could have made. */
static void clear_slots(void)
{
    int i;
    for (i = 0; i < Q2_SAVE_SLOTS; i++)
        q2_save_slot_delete(i);
}

static void clear_settings_slots(void)
{
    int i;
    for (i = 0; i < Q2_SAVE_SLOTS; i++)
        q2_settings_slot_delete(i);
}

/* ------------------------------------------------------------------------- */
/* A sim with something in every corner of it, so the round trip has something */
/* to lose.                                                                    */
/* ------------------------------------------------------------------------- */
static void build_state(q2_sim *sim, q2_inventory *inv)
{
    s32 spawn[3] = { 1234, -5678, 9012 };

    q2_sim_init(sim, NULL, 50);
    q2_sim_spawn(sim, spawn, 700);

    sim->player[0].pitch          = -250;
    sim->player[0].roll           = 33;
    sim->player[0].vel[0]         = 17;
    sim->player[0].vel[1]         = -900;
    sim->player[0].vel[2]         = 4;
    sim->player[0].wish[0]        = -120;
    sim->player[0].frame_delta[1] = -7;
    sim->player[0].jump_hold      = 3;
    sim->player[0].view_height    = 421;
    sim->player[0].on_ground      = true;
    sim->player[0].ground_y       = -5678;
    sim->player[0].fall_value     = 61;
    sim->player[0].fall_time      = 7777;
    sim->player[0].foot           = 1;
    sim->player[0].look_scheme    = 6;
    sim->player[0].wade           = 3;
    sim->player[0].water_air      = 1234;
    sim->player[0].water_next     = 124000;
    sim->player[0].splash_time    = 123900;
    sim->player[0].water_voice    = true;
    sim->player[0].ent.flags      = 0xDEADBEEFu;
    sim->player[0].ent.node       = 42;
    sim->player[0].ent.ground_normal[1] = -4096;

    sim->level_time   = 123456;
    sim->tick_count   = 4321;
    sim->dt_accum     = 5;
    sim->gravity      = 47;
    sim->env_flags    = 0x1234u;
    sim->cheats       = 0x40u;
    sim->current_node = 42;
    sim->no_fall_damage = true;

    sim->combat.weapon_id        = 9;
    sim->combat.next_fire        = 123500;
    sim->combat.kick[0]          = -12;
    sim->combat.chaingun_bullets = 3;
    sim->combat.rng.state        = 0xABCDEF01u;
    sim->fx_rng.state            = 0x0BADF00Du;

    /* A rocket in flight is as much state as the player is. */
    sim->combat.projectiles.p[2].in_use  = true;
    sim->combat.projectiles.p[2].kind    = Q2_PROJ_ROCKET;
    sim->combat.projectiles.p[2].pos[0]  = 5000;
    sim->combat.projectiles.p[2].vel[2]  = -333;
    sim->combat.projectiles.p[2].damage  = 100;
    sim->combat.projectiles.p[2].expires = 200000;
    sim->combat.projectiles.p[2].node    = 7;
    sim->combat.projectiles.live         = 1;

    q2_inventory_init(inv);
    q2_inventory_add_weapon(inv, Q2_WEAPON_RAILGUN);
    q2_inventory_add_ammo(inv, Q2_AMMO_SLUGS, 23);
    q2_inventory_give_key(inv, 0x0005);
    inv->health         = 77;
    inv->armour         = 42;
    inv->armour_class   = Q2_ARMOUR_BODY;
    inv->silencer_shots = 12;
    inv->quad_until     = 132456;
    inv->ammo_tier      = Q2_AMMO_TIER_BANDOLIER;
    inv->last_item      = 19;
}

/* The sim owns neither triggers nor items without a map, so the pieces that
 * come from one are installed by hand — which is also what lets the test check
 * them without a disc. */
static void attach_fake_world(q2_sim *sim, u32 triggers, u32 entities)
{
    u32 i;

    sim->trigger_capacity = triggers;
    sim->trigger_inside   = (u8 *)calloc(triggers ? triggers : 1, 1);

    for (i = 0; i < entities; i++) {
        q2_entity *e = q2_entity_alloc(&sim->entities);
        if (!e)
            break;
        e->place_id = (u16)(100 + i);
        e->kind     = Q2_ENT_KIND_ITEM;
        e->scale    = 4096;
        e->pos[0]   = (s32)(i * 1000);
        e->think    = q2_item_think;
    }
    sim->entities_ready = true;
}

/* ------------------------------------------------------------------------- */
/* A three-group Population for the deferred-item save regression.            */
/* ------------------------------------------------------------------------- */
static void batch_put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void batch_put_group(u8 *buf, u32 at, const char *name, u32 places)
{
    memset(buf + at, 0, Q2_POP_GROUP_SIZE);
    memcpy(buf + at, name, strlen(name));
    batch_put_u32(buf + at + 0x10, places);
}

static void batch_put_place(u8 *buf, u32 at, s32 x, u16 id)
{
    memset(buf + at, 0, Q2_POP_PLACE_SIZE);
    batch_put_u32(buf + at, (u32)x);
    buf[at + 14] = (u8)id;
    buf[at + 15] = (u8)(id >> 8);
    batch_put_u32(buf + at + Q2_POP_PLACE_SIZE, Q2_POP_TERM_FFFF);
}

/*
 * The allocator reuses a collected item's slot. That makes this sequence the
 * hard case for a save:
 *
 *   Zone0 starts in slot 0 -> collected -> Second reuses slot 0 -> First
 *   appends at slot 1.
 *
 * Replaying a bitmap in Population order would instead build First, Second;
 * replaying the right order without stable place keys would still leave the
 * fresh Zone0 template in slot 0. The ITEM chunk has to carry both facts.
 */
static void test_deferred_item_group_restore(void)
{
    enum {
        ZONE_PLACE   = 80,
        FIRST_PLACE  = ZONE_PLACE + Q2_POP_PLACE_SIZE + 4,
        SECOND_PLACE = FIRST_PLACE + Q2_POP_PLACE_SIZE + 4,
        POP_SIZE     = SECOND_PLACE + Q2_POP_PLACE_SIZE + 4
    };
    u8 population[POP_SIZE];
    dat_chunk pop_chunk;
    q2_common_file common;
    q2_sim source, restored;
    q2_save saved, loaded;
    q2_result applied;

    printf("deferred item group restore\n");

    memset(population, 0, sizeof(population));
    batch_put_group(population, 0,  "Zone0",  ZONE_PLACE);
    batch_put_group(population, 24, "First",  FIRST_PLACE);
    batch_put_group(population, 48, "Second", SECOND_PLACE);
    batch_put_u32(population + 72, 0);       /* group-table terminator */

    /* All three use the same item id deliberately: place_id alone cannot
     * distinguish which group owns a reconstructed slot. */
    batch_put_place(population, ZONE_PLACE,   1000, 27); /* Shells P */
    batch_put_place(population, FIRST_PLACE,  2000, 27);
    batch_put_place(population, SECOND_PLACE, 3000, 27);

    memset(&pop_chunk, 0, sizeof(pop_chunk));
    pop_chunk.data = population;
    pop_chunk.size = sizeof(population);
    memset(&common, 0, sizeof(common));
    common.chunk[Q2_COMMON_POPULATION] = &pop_chunk;

    q2_sim_init(&source, NULL, 50);
    check_eq_i(q2_sim_attach_items(&source, &common, 0, NULL, NULL), Q2_OK,
               "source attaches the synthetic Population");
    check_eq_i(source.entities.count, 1, "only resident Zone0 starts live");
    check_eq_i(source.entities.ent[0].population_group, 0,
               "the startup entity is keyed to Zone0");

    q2_entity_remove(&source.entities.ent[0]);
    check(!source.entities.ent[0].in_use,
          "collecting the startup item leaves an allocator hole");

    check_eq_i(q2_sim_activate_item_group(&source, "Second"), 1,
               "the later Population row activates first");
    check_eq_i(source.entities.count, 1,
               "Second reuses the collected Zone0 slot");
    check_eq_i(source.entities.ent[0].population_group, 2,
               "the reused slot keeps Second's stable group key");
    check_eq_i(q2_sim_activate_item_group(&source, "First"), 1,
               "the earlier Population row activates second");
    check_eq_i(source.entities.count, 2, "First appends after the reused slot");
    check_eq_i(source.item_group_order_count, 3,
               "the source records startup plus both dynamic groups");
    check_eq_i(source.item_group_order[0], 0, "Zone0 is the startup prefix");
    check_eq_i(source.item_group_order[1], 2,
               "Second retains its actual activation position");
    check_eq_i(source.item_group_order[2], 1,
               "First is not sorted back into Population order");

    /* Mutable ENTS state must still land on the physical slots selected by
     * the stable keys after the roster is rebuilt. */
    source.entities.ent[0].pos[0] = 32002;
    source.entities.ent[0].hidden = true;
    source.entities.ent[0].taken[0] = true;
    source.entities.ent[1].pos[0] = 21001;
    source.entities.ent[1].scale = 1024;

    check_eq_i(q2_save_capture(&saved, &source, NULL,
                               "SLES-01534", "BASE1", 0), Q2_OK,
               "captures a dynamically activated item roster");
    check(saved.item_state_present, "capture emits the version-5 ITEM state");
    check_eq_i(saved.item_population_group_count, 3,
               "ITEM records the Population group count");
    check_eq_i(saved.item_group_order_count, 3,
               "ITEM records every first-run latch");
    check_eq_i(saved.item_group_order[1], 2,
               "capture preserves reversed activation order");
    check_eq_i(saved.item_key_count, 2,
               "ITEM carries one stable key per ENTS slot");
    check_eq_i(saved.item_keys[0].group, 2,
               "the reused physical slot is keyed to Second");
    check_eq_i(saved.item_keys[1].group, 1,
               "the appended physical slot is keyed to First");

    check_eq_i(q2_save_write(&saved, tmp_path()), Q2_OK,
               "writes the deferred-item save");
    check_eq_i(q2_save_read(&loaded, tmp_path()), Q2_OK,
               "reads the deferred-item save");
    check(loaded.item_state_present, "the ITEM chunk survives the file");
    check_eq_i(loaded.item_group_order_count, 3,
               "the activation order survives the file");
    check_eq_i(loaded.item_group_order[1], 2,
               "the reversed middle entry survives the file");
    check_eq_i(loaded.item_keys[0].group, 2,
               "stable slot identity survives the file");

    q2_sim_init(&restored, NULL, 50);
    check_eq_i(q2_sim_attach_items(&restored, &common, 0, NULL, NULL), Q2_OK,
               "load starts from a fresh startup roster");
    check_eq_i(restored.entities.count, 1,
               "the fresh roster initially contains Zone0 only");

    loaded.item_state_present = false;
    applied = q2_save_apply(&loaded, &restored, NULL,
                            "SLES-01534", "BASE1");
    check_eq_i(applied, Q2_ERR_BAD_FORMAT,
               "a Population-backed v5 save cannot omit ITEM");
    check_eq_i(restored.entities.count, 1,
               "a missing ITEM rejection leaves the roster untouched");
    loaded.item_state_present = true;

    loaded.item_keys[0].slot = 99;
    applied = q2_save_apply(&loaded, &restored, NULL,
                            "SLES-01534", "BASE1");
    check_eq_i(applied, Q2_ERR_BAD_FORMAT,
               "apply rejects a stable key absent from the Population");
    check_eq_i(restored.entities.count, 1,
               "a rejected roster rebuild leaves the entity set untouched");
    check_eq_i(restored.entities.ent[0].population_group, 0,
               "a rejected rebuild leaves the startup entity untouched");
    check_eq_i(restored.item_group_order_count, 1,
               "a rejected rebuild does not commit deferred latches");
    check(restored.item_group_run[0] && !restored.item_group_run[1] &&
              !restored.item_group_run[2],
          "a rejected rebuild leaves the startup latch bitmap untouched");

    loaded.item_keys[0].slot = 0;
    applied = q2_save_apply(&loaded, &restored, NULL,
                            "SLES-01534", "BASE1");
    check_eq_i(applied, Q2_OK,
               "apply rebuilds dynamic items without an entity-count mismatch");
    check_eq_i(restored.entities.count, 2,
               "the collected startup item stays absent after load");
    check_eq_i(restored.entities.ent[0].population_group, 2,
               "Second returns to its reused physical slot");
    check_eq_i(restored.entities.ent[1].population_group, 1,
               "First returns to its appended physical slot");
    check_eq_i(restored.entities.ent[0].pos[0], 32002,
               "Second's mutable ENTS state follows its key");
    check(restored.entities.ent[0].hidden,
          "Second's hidden state follows its key");
    check(restored.entities.ent[0].taken[0],
          "Second's per-player taken bit follows its key");
    check_eq_i(restored.entities.ent[1].pos[0], 21001,
               "First's mutable ENTS state follows its key");
    check_eq_i(restored.entities.ent[1].scale, 1024,
               "First's scale follows its key");

    check(restored.item_group_run[0] && restored.item_group_run[1] &&
              restored.item_group_run[2],
          "all startup and dynamic one-shot latches are restored");
    check_eq_i(restored.item_group_order_count, 3,
               "the restored order contains each group once");
    check_eq_i(restored.item_group_order[0], 0,
               "the restored order keeps the startup prefix");
    check_eq_i(restored.item_group_order[1], 2,
               "the restored order keeps Second before First");
    check_eq_i(restored.item_group_order[2], 1,
               "the restored order keeps First last");

    check_eq_i(q2_sim_activate_item_group(&restored, "Second"), 0,
               "Second cannot duplicate after load");
    check_eq_i(q2_sim_activate_item_group(&restored, "First"), 0,
               "First cannot duplicate after load");
    check_eq_i(restored.entities.count, 2,
               "repeated post-load CREBATCH calls leave the roster unchanged");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&source);
    q2_sim_free(&restored);
    remove(tmp_path());
}

/* ------------------------------------------------------------------------- */
static void test_game_random_restore(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved = {0}, loaded = {0}, legacy = {0};
    s32 expected[32];
    u32 i, state;
    printf("shared gameplay random state\n");
    build_state(&sim, &inv);
    q2_random_seed(0xF1234567u);
    for (i = 0; i < 17; i++) (void)q2_random_next();
    state = q2_random_state();
    check(q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 0) == Q2_OK,
          "capture the shared random stream");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "random state survives disk");
    for (i = 0; i < 32; i++) expected[i] = q2_random_next();
    q2_random_seed(42);
    check(q2_save_apply(&loaded, &sim, NULL, "SLUS-00757", "BASE1") != Q2_OK &&
          q2_random_state() == 42, "a refused save leaves the stream alone");
    check(q2_save_apply(&loaded, &sim, NULL, "SLES-01534", "BASE1") == Q2_OK &&
          q2_random_state() == state, "restore the complete state after rebuilding");
    for (i = 0; i < 32; i++)
        check_eq_i(q2_random_next(), expected[i], "resume the next gameplay choices");
    q2_random_seed(123);
    q2_save_apply_game_rng(&legacy);
    check_eq_i(q2_random_state(), 123, "legacy files leave the unspecified stream alone");
    saved.game_rng_state = 0;
    q2_save_free(&loaded);
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "zero state survives disk");
    q2_save_apply_game_rng(&loaded);
    check_eq_i(q2_random_state(), 0, "zero is restored without changing the seed");
    q2_save_free(&saved); q2_save_free(&loaded); q2_sim_free(&sim);
    remove(tmp_path());
}

static void test_projectile_runtime_restore(void)
{
    q2_sim sim;
    q2_save saved = {0}, loaded = {0};
    printf("projectile effects and BFG beam timing\n");
    q2_sim_init(&sim, NULL, 50);
    sim.combat.projectiles.p[0].in_use = true;
    sim.combat.projectiles.p[0].kind = Q2_PROJ_BOLT;
    sim.combat.projectiles.p[0].mod = 5;
    sim.combat.projectiles.p[0].flags = Q2_PROJ_FLAGS_HYPERBLASTER;
    sim.combat.projectiles.p[1].in_use = true;
    sim.combat.projectiles.p[1].kind = Q2_PROJ_BFG;
    sim.combat.projectiles.p[1].beam_time = 27;
    check(q2_save_capture(&saved, &sim, NULL, "SLES-01534", "BASE0", 0) == Q2_OK,
          "capture a bolt and a BFG near its next beam tick");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "projectile state survives disk");
    check(q2_save_apply(&loaded, &sim, NULL, "SLES-01534", "BASE0") == Q2_OK,
          "restore projectile state");
    check_eq_i(sim.combat.projectiles.p[0].flags, Q2_PROJ_FLAGS_HYPERBLASTER,
               "bolt body, light and impact flags survive");
    check_eq_i(sim.combat.projectiles.p[1].beam_time, 27,
               "BFG damage cadence keeps its accumulated phase");
    saved.projectile_runtime_present = false;
    q2_save_free(&loaded);
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK && !loaded.projectile_runtime_present,
          "read a legacy file without PRST");
    check_eq_i(loaded.proj[0].flags, Q2_PROJ_FLAGS_HYPERBLASTER,
               "legacy bolt flags are reconstructed from means of death");
    check_eq_i(loaded.proj[1].beam_time, 0, "legacy BFG has no recorded phase");
    q2_save_free(&saved); q2_save_free(&loaded); q2_sim_free(&sim);
    remove(tmp_path());
}

static void test_round_trip(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;

    printf("round trip\n");

    build_state(&sim, &inv);
    attach_fake_world(&sim, 5, 4);

    /* Something has already happened in this level. */
    sim.trigger_inside[1] = 1;
    sim.trigger_inside[4] = 1;
    sim.entities.ent[0].taken[0] = true;
    sim.entities.ent[0].hidden   = true;
    sim.entities.ent[0].respawn_at = 4000;
    sim.entities.ent[2].scale    = 1024;
    sim.entities.ent[2].think    = q2_item_shrink_think;
    q2_entity_remove(&sim.entities.ent[3]);

    /*
     * A pane that has been shot. Without this in the file, a save made after
     * breaking a window restores it whole and the shards already on the floor
     * come back as glass — the same defect as a save that shuts every door the
     * player opened, which is why the script flags are carried.
     */
    sim.breakable_count = 2;
    sim.breakable[0].scene_node = 187;
    sim.breakable[0].health     = -4;
    sim.breakable[0].broken     = true;
    sim.breakable[1].scene_node = 205;
    sim.breakable[1].health     = 30;
    sim.breakable[1].broken     = false;

    check(q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 2) == Q2_OK,
          "captures state");
    check_eq_i(saved.player.pos[0], 1234, "captured x");
    check_eq_i(saved.zone, 2, "captured zone");
    check_eq_i(saved.entity_count, 4, "captured every entity slot");
    check(!saved.item_state_present,
          "a sim without Population keeps the optional ITEM chunk absent");

    check(q2_save_write(&saved, tmp_path()) == Q2_OK, "writes to disk");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK, "reads back");
    check(!loaded.item_state_present,
          "a version-5 no-Population file may omit ITEM");

    /* --- identity ------------------------------------------------------- */
    check(strcmp(loaded.map, "BASE1") == 0, "map name survives");
    check(strcmp(loaded.serial, "SLES-01534") == 0, "serial survives");
    check_eq_i(loaded.zone, saved.zone, "zone survives");
    check(loaded.label[0] != '\0', "a label was composed");
    check(strcmp(loaded.label, saved.label) == 0, "label survives");

    /* --- the player ------------------------------------------------------ */
    check_eq_i(loaded.player.pos[0], saved.player.pos[0], "x survives");
    check_eq_i(loaded.player.pos[1], saved.player.pos[1],
               "y survives, including negatives");
    check_eq_i(loaded.player.pos[2], saved.player.pos[2], "z survives");
    check_eq_i(loaded.player.vel[1], -900, "velocity survives");
    check_eq_i(loaded.player.yaw, saved.player.yaw, "yaw survives");
    check_eq_i(loaded.player.pitch, -250, "pitch survives, including negatives");
    check_eq_i(loaded.player.roll, 33, "roll survives");
    check_eq_i(loaded.player.wish[0], -120, "the wish velocity survives");
    check_eq_i(loaded.player.frame_delta[1], -7,
               "the one-tick frame delta survives");
    check_eq_i(loaded.player.jump_hold, 3, "jump hold survives");
    check_eq_i(loaded.player.view_height, 421, "view height survives");
    check(loaded.player.on_ground, "the ground flag survives");
    check_eq_i(loaded.player.ground_y, -5678, "ground height survives");
    check_eq_i(loaded.player.fall_value, 61, "the fall kick survives");
    check_eq_i(loaded.player.fall_time, 7777, "the fall deadline survives");
    check_eq_i(loaded.player.foot, 1, "which footstep is next survives");
    check_eq_i(loaded.player.look_scheme, 6, "the control scheme survives");
    check_eq_i(loaded.player.wade, 3, "the shallow-water counter survives");
    check_eq_i(loaded.player.water_air, 1234, "the accumulated breath survives");
    check_eq_i(loaded.player.water_next, 124000,
               "the drowning deadline survives");
    check_eq_i(loaded.player.splash_time, 123900,
               "the water-splash deadline survives");
    check(loaded.player.water_voice, "the drowning voice alternation survives");

    /* The mover's own carried state. Losing this is what makes a restored
     * player fall through the floor they were standing on. */
    check_eq_i((s64)loaded.player.ent.flags, (s64)0xDEADBEEFu,
               "the mover's flags word survives");
    check_eq_i(loaded.player.ent.node, 42, "the cached collision cell survives");
    check_eq_i(loaded.player.ent.ground_normal[1], -4096,
               "the ground normal survives");

    /* --- the clock -------------------------------------------------------- */
    check_eq_i(loaded.level_time, 123456, "the level clock survives");
    check_eq_i(loaded.tick_count, 4321, "the tick count survives");
    check_eq_i(loaded.dt_accum, 5, "the leftover dt survives");
    check_eq_i(loaded.gravity, 47, "gravity survives");
    check_eq_i((s64)loaded.env_flags, 0x1234, "the environment flags survive");
    check_eq_i((s64)loaded.cheats, 0x40, "the game-variable word survives");
    check(loaded.no_fall_damage != 0, "the fall-damage rule survives");

    /* --- inventory --------------------------------------------------------- */
    check_eq_i(loaded.inventory.health, 77, "health survives");
    check_eq_i(loaded.inventory.armour, 42, "armour survives");
    check_eq_i(loaded.inventory.armour_class, Q2_ARMOUR_BODY,
               "armour class survives");
    check_eq_i(loaded.inventory.ammo[Q2_AMMO_SLUGS], 23, "ammo survives");
    check_eq_i(loaded.inventory.ammo_tier, Q2_AMMO_TIER_BANDOLIER,
               "the ammo tier survives");
    check_eq_i(loaded.inventory.silencer_shots, 12, "silencer shots survive");
    check_eq_i(loaded.inventory.last_item, 19, "the last item survives");
    /* An absolute deadline on the level clock is meaningless without the clock;
     * both are here, which is the point. */
    check_eq_i(loaded.inventory.quad_until, 132456, "the quad deadline survives");
    /* Keys are bits in the client flags word, not a field of their own — see
     * the structure note in inventory.h. */
    check(q2_inventory_has_keys(&loaded.inventory, 0x0005), "keys survive");
    check(q2_inventory_has_weapon(&loaded.inventory, Q2_WEAPON_RAILGUN),
          "weapons survive");

    /* --- combat ----------------------------------------------------------- */
    check_eq_i(loaded.weapon_id, 9, "the held weapon survives");
    check_eq_i(loaded.next_fire, 123500, "the refire gate survives");
    check_eq_i(loaded.kick[0], -12, "the view kick survives");
    check_eq_i(loaded.chaingun_bullets, 3, "the chaingun spin survives");
    check_eq_i((s64)loaded.rng_state, (s64)0xABCDEF01u,
               "the weapon generator survives");
    check_eq_i((s64)loaded.fx_rng_state, (s64)0x0BADF00Du,
               "the effect generator survives");

    check(loaded.proj[2].in_use, "a projectile in flight survives");
    check_eq_i(loaded.proj[2].kind, Q2_PROJ_ROCKET, "its kind survives");
    check_eq_i(loaded.proj[2].pos[0], 5000, "its position survives");
    check_eq_i(loaded.proj[2].vel[2], -333, "its velocity survives");
    check_eq_i(loaded.proj[2].node, 7, "its cached cell survives");
    check(!loaded.proj[0].in_use, "an empty projectile slot stays empty");

    /* --- the world --------------------------------------------------------- */
    check_eq_i(loaded.trigger_count, 5, "the trigger residency survives");
    check_eq_i(loaded.trigger_inside[1], 1, "a volume the player is in survives");
    check_eq_i(loaded.trigger_inside[0], 0, "one they are not stays clear");

    check_eq_i(loaded.entity_count, 4, "every entity slot survives");
    check_eq_i(loaded.entities[0].place_id, 100, "an entity's place id survives");
    check_eq_i(loaded.entities[0].taken, 1, "a collected item stays collected");
    check_eq_i(loaded.entities[0].hidden, 1, "a hidden item stays hidden");

    check_eq_i(loaded.breakable_count, 2, "both panes survive");
    check_eq_i(loaded.breakables[0].scene_node, 187, "a pane is keyed by node");
    check_eq_i(loaded.breakables[0].broken, 1, "a broken pane stays broken");
    check_eq_i(loaded.breakables[0].health, -4, "its hit points survive");
    check_eq_i(loaded.breakables[1].broken, 0, "an intact pane stays intact");
    check_eq_i(loaded.breakables[1].health, 30, "and keeps what it has left");

    /*
     * The movers, which the client owns: two leaves of one MOVER_C item, so the
     * item offset alone does not identify a leaf and the sequence number is
     * what separates them.
     */
    {
        q2_mover_set set;
        q2_save      out;
        q2_save      back;

        memset(&set, 0, sizeof(set));
        set.count   = 2;
        set.movers  = (q2_mover *)calloc(2, sizeof(q2_mover));
        set.movers[0].item_offset = 0x240;
        set.movers[0].state       = 3;
        set.movers[0].offset      = 700;
        set.movers[0].wait_timer  = 1200;
        set.movers[1].item_offset = 0x240;   /* the other leaf */
        set.movers[1].state       = 1;
        set.movers[1].offset      = -700;

        memset(&out, 0, sizeof(out));
        q2_save_capture_movers(&out, &set);
        check_eq_i(out.mover_count, 2, "both leaves captured");
        check_eq_i(out.movers[0].seq, 0, "the first leaf is sequence 0");
        check_eq_i(out.movers[1].seq, 1, "the second is sequence 1");

        check(q2_save_write(&out, tmp_path()) == Q2_OK, "movers write");
        memset(&back, 0, sizeof(back));
        check(q2_save_read(&back, tmp_path()) == Q2_OK, "movers read back");

        /* Applied to a set built fresh — everything shut, timers full. */
        memset(set.movers, 0, 2 * sizeof(q2_mover));
        set.movers[0].item_offset = 0x240;
        set.movers[1].item_offset = 0x240;
        q2_save_apply_movers(&back, &set);

        check_eq_i(set.movers[0].state, 3, "the open leaf reopens");
        check_eq_i(set.movers[0].offset, 700, "where it had travelled to");
        check_eq_i(set.movers[0].wait_timer, 1200, "and its countdown");
        check_eq_i(set.movers[1].state, 1, "the other leaf is its own state");
        check_eq_i(set.movers[1].offset, -700, "and travelled the other way");

        free(set.movers);
        q2_save_free(&out);
        q2_save_free(&back);
    }

    /* The creatures: who is dead survives, and a mismatched count restores
     * nothing rather than putting one creature's health on another. */
    {
        q2_monster_set cset;
        q2_save out, back;

        memset(&cset, 0, sizeof(cset));
        cset.count    = 3;
        cset.monsters = (q2_monster *)calloc(3, sizeof(q2_monster));
        cset.monsters[0].in_use = true;
        cset.monsters[0].health = 40;
        cset.monsters[0].pos[0] = 900;
        cset.monsters[1].in_use = true;
        cset.monsters[1].dead   = true;
        cset.monsters[1].health = -12;
        cset.monsters[1].frame  = 77;
        cset.monsters[2].in_use = true;
        cset.monsters[2].health = 100;

        memset(&out, 0, sizeof(out));
        q2_save_capture_creatures(&out, &cset);
        check(q2_save_write(&out, tmp_path()) == Q2_OK, "creatures write");
        memset(&back, 0, sizeof(back));
        check(q2_save_read(&back, tmp_path()) == Q2_OK, "creatures read back");
        check_eq_i(back.creature_count, 3, "all three survive");

        memset(cset.monsters, 0, 3 * sizeof(q2_monster));
        q2_save_apply_creatures(&back, &cset);
        check_eq_i(cset.monsters[1].dead, 1, "the dead one stays dead");
        check_eq_i(cset.monsters[1].frame, 77, "in the pose it died in");
        check_eq_i(cset.monsters[0].health, 40, "the hurt one stays hurt");
        check_eq_i(cset.monsters[0].pos[0], 900, "where it had walked to");

        /* A different population: restore nothing rather than by index. */
        cset.count = 2;
        memset(cset.monsters, 0, 3 * sizeof(q2_monster));
        q2_save_apply_creatures(&back, &cset);
        check_eq_i(cset.monsters[0].health, 0,
                   "a mismatched count restores nothing");

        free(cset.monsters);
        q2_save_free(&out);
        q2_save_free(&back);
    }

    /*
     * And the round trip through the sim: applied to a registry whose panes are
     * in a DIFFERENT order, the match is by node and not by index.
     */
    {
        q2_sim s2;

        memset(&s2, 0, sizeof(s2));
        s2.breakable_count = 2;
        s2.breakable[0].scene_node = 205;   /* swapped */
        s2.breakable[1].scene_node = 187;
        s2.breakable[0].health = 50;
        s2.breakable[1].health = 50;

        q2_save_apply(&loaded, &s2, NULL, "SLES-01534", "BASE1");
        check_eq_i(s2.breakable[1].broken, 1,
                   "the broken pane is found by node, not by index");
        check_eq_i(s2.breakable[0].broken, 0, "and the intact one is not");
        check_eq_i(s2.breakable[1].health, -4, "with its hit points");
    }
    check_eq_i(loaded.entities[0].respawn_at, 4000, "its respawn timer survives");
    check_eq_i(loaded.entities[2].scale, 1024, "a mid-shrink scale survives");
    check_eq_i(loaded.entities[2].think, Q2_SAVE_THINK_SHRINK,
               "which think is installed survives");
    check_eq_i(loaded.entities[3].in_use, 0, "a freed slot stays freed");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&sim);
    remove(tmp_path());
}

/* ------------------------------------------------------------------------- */
static void test_apply(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved;
    s32 spawn[3] = { 100, 200, 300 };

    printf("apply\n");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 512);
    attach_fake_world(&sim, 3, 3);
    q2_inventory_init(&inv);
    inv.health        = 55;
    sim.level_time    = 9000;
    sim.player[0].vel[1] = 640;
    sim.trigger_inside[2]        = 1;
    sim.entities.ent[1].taken[0] = true;
    sim.entities.ent[1].hidden   = true;

    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE0", 0);

    /* Move away, get hurt, let time pass, pick the last item up. */
    sim.player[0].pos[0]            = 99999;
    sim.player[0].yaw               = 3000;
    sim.player[0].vel[1]            = 12345;
    inv.health                   = 1;
    sim.level_time               = 50000;
    sim.trigger_inside[2]        = 0;
    sim.entities.ent[1].taken[0] = false;
    sim.entities.ent[1].hidden   = false;
    sim.entities.ent[2].taken[0] = true;

    check(q2_save_apply(&saved, &sim, &inv, "SLES-01534", "BASE0") == Q2_OK,
          "applies to the matching disc and map");
    check_eq_i(sim.player[0].pos[0], 100, "position restored");
    check_eq_i(sim.player[0].yaw, 512, "yaw restored");
    check_eq_i(inv.health, 55, "health restored");
    check_eq_i(sim.combat.inv.health, 55, "the sim's own inventory restored");
    check_eq_i(sim.level_time, 9000, "the level clock rewinds with the save");

    /*
     * Velocity is RESTORED, not cleared. It was cleared once, and that was
     * wrong: a save made mid-jump has a real velocity, and zeroing it drops the
     * player straight down out of an arc they were in the middle of.
     */
    check_eq_i(sim.player[0].vel[1], 640, "velocity restored, not zeroed");

    check_eq_i(sim.trigger_inside[2], 1, "trigger residency restored");
    check(sim.entities.ent[1].taken[0], "a collected item is still collected");
    check(sim.entities.ent[1].hidden, "and still hidden");
    check(!sim.entities.ent[2].taken[0],
          "an item collected after the save is back");

    /* A save from a different release must be refused: the level table and
     * script offsets differ per build, so the coordinates mean something else. */
    check(q2_save_apply(&saved, &sim, &inv, "SLUS-00658", "BASE0") != Q2_OK,
          "refuses a save from another build");

    /* And it must not be applied to the wrong map. */
    check(q2_save_apply(&saved, &sim, &inv, "SLES-01534", "JAIL2") != Q2_OK,
          "refuses to restore into the wrong map");

    q2_save_free(&saved);
    q2_sim_free(&sim);
}

/* A save whose entity set does not match the map's is a save for a different
 * population, and applying it by index would put one item's state on another. */
static void test_apply_rejects_mismatched_map(void)
{
    q2_sim a, b;
    q2_inventory inv;
    q2_save saved;
    s32 spawn[3] = { 0, 0, 0 };

    printf("apply, mismatched population\n");

    q2_sim_init(&a, NULL, 50);
    q2_sim_spawn(&a, spawn, 0);
    attach_fake_world(&a, 2, 4);
    q2_inventory_init(&inv);
    q2_save_capture(&saved, &a, &inv, "SLES-01534", "BASE0", 0);

    q2_sim_init(&b, NULL, 50);
    q2_sim_spawn(&b, spawn, 0);
    attach_fake_world(&b, 2, 6);

    check(q2_save_apply(&saved, &b, &inv, "SLES-01534", "BASE0")
              == Q2_ERR_BAD_FORMAT,
          "refuses a save whose entity count disagrees with the map");

    /* The same size but a different population: the place ids no longer line
     * up, and that is caught too. */
    q2_sim_free(&b);
    q2_sim_init(&b, NULL, 50);
    q2_sim_spawn(&b, spawn, 0);
    attach_fake_world(&b, 2, 4);
    b.entities.ent[2].place_id = 999;
    b.player[0].pos[0] = 4242;
    b.level_time    = 31337;

    check(q2_save_apply(&saved, &b, &inv, "SLES-01534", "BASE0")
              == Q2_ERR_BAD_FORMAT,
          "refuses a save whose place ids disagree with the map");

    /*
     * And refusing means refusing ENTIRELY. The mismatch is on entity 2, so a
     * restore that validated as it went would already have moved the player and
     * rewound the clock before it noticed — leaving a half-restored session,
     * which is worse than either outcome.
     */
    check_eq_i(b.player[0].pos[0], 4242, "a refused apply does not move the player");
    check_eq_i(b.level_time, 31337, "a refused apply does not touch the clock");
    check(!b.entities.ent[0].taken[0],
          "a refused apply does not touch the entities either");

    q2_save_free(&saved);
    q2_sim_free(&a);
    q2_sim_free(&b);
}

/* ------------------------------------------------------------------------- */
static void test_rejects_bad_files(void)
{
    q2_save s;
    const char *path = tmp_path();
    FILE *f;

    printf("bad files\n");

    check(q2_save_read(&s, "no_such_file_here.sav") == Q2_ERR_NOT_FOUND,
          "a missing file is not found");

    /* Wrong magic. */
    f = fopen(path, "wb");
    if (f) {
        fwrite("XXXX....................................", 1, 40, f);
        fclose(f);
        check(q2_save_read(&s, path) == Q2_ERR_BAD_FORMAT, "rejects wrong magic");
    }

    /* Right magic, truncated body. */
    f = fopen(path, "wb");
    if (f) {
        fwrite(Q2_SAVE_MAGIC, 1, 4, f);
        fclose(f);
        check(q2_save_read(&s, path) == Q2_ERR_BAD_FORMAT, "rejects a truncated file");
    }

    /* A version this build does not read. */
    {
        u8 head[16];
        memset(head, 0, sizeof(head));
        memcpy(head, Q2_SAVE_MAGIC, 4);
        head[4] = (u8)(Q2_SAVE_VERSION + 7);

        f = fopen(path, "wb");
        if (f) {
            fwrite(head, 1, sizeof(head), f);
            fclose(f);
            check(q2_save_read(&s, path) == Q2_ERR_UNSUPPORTED,
                  "rejects an unknown version rather than misreading it");
        }
    }

    remove(path);
}

/*
 * This is a checksum-valid, chunk-valid pre-batch fixture: the captured sim
 * has no Population state and therefore no ITEM chunk. Only its header is
 * relabelled as the old version, exactly as a save written before deferred
 * item rosters existed would be. Rejecting it at the header proves we cannot
 * fall through to the unsafe by-index entity or old PROJ interpretation.
 */
static void test_rejects_pre_batch_v4(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    const char *path = tmp_path();
    const u8 v4[4] = { 4, 0, 0, 0 };
    FILE *f;

    printf("pre-batch version 4\n");

    check_eq_i(Q2_SAVE_VERSION_OLDEST, 5,
               "the oldest format read is version 5, the one ITEM arrived in");
    build_state(&sim, &inv);
    check_eq_i(q2_save_capture(&saved, &sim, &inv,
                               "SLES-01534", "BASE1", 0), Q2_OK,
               "captures the pre-batch-shaped state");
    check(!saved.item_state_present, "the fixture has no ITEM history");
    check_eq_i(q2_save_write(&saved, path), Q2_OK,
               "writes a checksum-valid fixture body");

    f = fopen(path, "r+b");
    if (!f) {
        check(false, "reopens the fixture header");
    } else {
        check(fseek(f, 4, SEEK_SET) == 0 &&
              fwrite(v4, 1, sizeof(v4), f) == sizeof(v4),
              "marks the fixture as version 4");
        fclose(f);
        check_eq_i(q2_save_read(&loaded, path), Q2_ERR_UNSUPPORTED,
                   "rejects a valid v4 body before interpreting its chunks");
    }

    q2_save_free(&saved);
    q2_sim_free(&sim);
    remove(path);
}

/* ------------------------------------------------------------------------- */
/* A save image built by hand, byte by byte, the way an older build wrote it. */
/* Nothing here goes through q2_save_write, so the test cannot agree with the  */
/* writer by construction.                                                     */
/* ------------------------------------------------------------------------- */
typedef struct save_image {
    u8  b[512];
    u32 len;
} save_image;

static void img_u8(save_image *m, u8 v)
{
    if (m->len < sizeof(m->b))
        m->b[m->len++] = v;
}

static void img_u32(save_image *m, u32 v)
{
    img_u8(m, (u8)v);
    img_u8(m, (u8)(v >> 8));
    img_u8(m, (u8)(v >> 16));
    img_u8(m, (u8)(v >> 24));
}

static void img_str(save_image *m, const char *s, u32 width)
{
    u32 i, n = (u32)strlen(s);

    for (i = 0; i < width; i++)
        img_u8(m, i < n && i + 1 < width ? (u8)s[i] : 0u);
}

/* tag, size, then the payload the caller writes; returns where size sits. */
static u32 img_chunk_begin(save_image *m, const char *tag)
{
    u32 at;

    img_u8(m, (u8)tag[0]);
    img_u8(m, (u8)tag[1]);
    img_u8(m, (u8)tag[2]);
    img_u8(m, (u8)tag[3]);
    at = m->len;
    img_u32(m, 0);
    return at;
}

static void img_chunk_end(save_image *m, u32 at)
{
    u32 size = m->len - (at + 4);

    m->b[at + 0] = (u8)size;
    m->b[at + 1] = (u8)(size >> 8);
    m->b[at + 2] = (u8)(size >> 16);
    m->b[at + 3] = (u8)(size >> 24);
}

/* The standard reflected CRC-32 — the file header's checksum. */
static u32 img_crc32(const u8 *p, u32 n)
{
    u32 crc = 0xFFFFFFFFu, i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* HEAD for BASE1 on this disc, then EVNT carrying `flags`. */
static void img_body(save_image *m, const u8 *flags, u32 count)
{
    u32 at, i;

    memset(m, 0, sizeof(*m));

    at = img_chunk_begin(m, "HEAD");
    img_str(m, "SLES-01534", Q2_SAVE_SERIAL_LEN);
    img_str(m, "BASE1", Q2_SAVE_MAP_LEN);
    img_u32(m, 0);                          /* zone      */
    img_str(m, "OLD SAVE", Q2_SAVE_LABEL_LEN);
    img_u32(m, 0);                          /* timestamp */
    img_chunk_end(m, at);

    at = img_chunk_begin(m, "EVNT");
    img_u32(m, count);
    for (i = 0; i < count; i++)
        img_u8(m, flags[i]);
    img_chunk_end(m, at);
}

static bool img_write(const char *path, u32 version, const save_image *body)
{
    u8 head[16];
    u32 crc = img_crc32(body->b, body->len);
    FILE *f;
    bool ok;

    memcpy(head, "Q2PS", 4);
    head[4]  = (u8)version;
    head[5]  = (u8)(version >> 8);
    head[6]  = (u8)(version >> 16);
    head[7]  = (u8)(version >> 24);
    head[8]  = (u8)body->len;
    head[9]  = (u8)(body->len >> 8);
    head[10] = (u8)(body->len >> 16);
    head[11] = (u8)(body->len >> 24);
    head[12] = (u8)crc;
    head[13] = (u8)(crc >> 8);
    head[14] = (u8)(crc >> 16);
    head[15] = (u8)(crc >> 24);

    f = fopen(path, "wb");
    if (!f)
        return false;
    ok = fwrite(head, 1, sizeof(head), f) == sizeof(head) &&
         fwrite(body->b, 1, body->len, f) == body->len;
    fclose(f);
    return ok;
}

/*
 * A two-record Events chunk to apply a save to:
 *
 *   +8   CAT_A|ONESHOT      [CALL 1]
 *   +24  CAT_B              [CALL|ONESHOT 2]
 *
 * after the u32 count at +0 and the directory terminator at +4.
 */
#define SAVE_EV_ONESHOT_RECORD 8
#define SAVE_EV_MIXED_RECORD   24
#define SAVE_EV_MIXED_ITEM     28

static void save_events_chunk(u8 *raw, q2_events *ev)
{
    memset(raw, 0, 40);
    raw[0] = 2;                                          /* record count */

    raw[8]  = 16; raw[10] = 1;
    raw[11] = (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT);
    raw[12] = Q2_EVOP_CALL;                raw[13] = 12; raw[14] = 1;

    raw[24] = 16; raw[26] = 1;
    raw[27] = Q2_EVREC_CAT_B;
    raw[28] = (u8)(Q2_EVOP_CALL | Q2_EVOP_ONESHOT); raw[29] = 12; raw[30] = 2;

    memset(ev, 0, sizeof(*ev));
    ev->data         = raw;
    ev->size         = 40;
    ev->record_count = 2;
    ev->dir_offset   = 4;
    ev->first_record = 8;
}

static bool save_events_attach(q2_sim *sim)
{
    if (q2_event_rt_init(&sim->event_rt, &sim->events) != Q2_OK)
        return false;
    sim->events_ready = true;
    return true;
}

typedef struct saved_timer_probe {
    q2_event_rt *rt;
    u32 calls[4];
} saved_timer_probe;

static void saved_timer_call(void *user, const q2_event_item *item, u8 index)
{
    saved_timer_probe *p = user;
    (void)item;
    if (index == 0) {
        p->rt->defer_timer = true;
        p->rt->defer_fires = 2;
        p->rt->defer_window = 1;
    } else if (index < 4) p->calls[index]++;
}

static void test_event_scheduler_restore(void)
{
    u8 raw[80] = {0};
    q2_sim live, rebuilt;
    q2_save saved = {0}, loaded = {0};
    saved_timer_probe a = {0}, b = {0};
    q2_event_timer expected;
    u32 next_random;
    printf("resident TIMER, WAIT and pending-trigger restore\n");
    raw[8] = 28; raw[10] = 2; raw[11] = Q2_EVREC_CAT_B;
    raw[12] = Q2_EVOP_CALL; raw[13] = 12; raw[16] = 2; raw[18] = 10;
    raw[24] = Q2_EVOP_CALL; raw[25] = 12; raw[26] = 1;
    raw[36] = 28; raw[38] = 2; raw[39] = Q2_EVREC_CAT_B;
    raw[40] = Q2_EVOP_WAIT; raw[41] = 12; raw[48] = raw[50] = 3;
    raw[52] = Q2_EVOP_CALL; raw[53] = 12; raw[54] = 3;
    raw[64] = 16; raw[66] = 1; raw[67] = Q2_EVREC_CAT_B;
    raw[68] = Q2_EVOP_CALL; raw[69] = 12; raw[70] = 2;
    q2_sim_init(&live, NULL, 50); q2_sim_init(&rebuilt, NULL, 50);
    live.events.data = raw; live.events.size = sizeof(raw);
    live.events.first_record = 8; live.events.record_count = 3;
    rebuilt.events = live.events;
    check(save_events_attach(&live) && save_events_attach(&rebuilt), "attach scheduler fixtures");
    a.rt = &live.event_rt; b.rt = &rebuilt.event_rt;
    a.rt->on_call = b.rt->on_call = saved_timer_call;
    a.rt->on_call_user = &a; b.rt->on_call_user = &b;
    a.rt->timer_period = b.rt->timer_period = q2_event_timer_period;
    q2_random_seed(1);
    q2_event_rt_trigger(a.rt, 8); q2_event_rt_update(a.rt);
    q2_event_rt_advance(a.rt, 70);
    q2_event_rt_trigger(a.rt, 36); q2_event_rt_update(a.rt);
    q2_event_rt_trigger(a.rt, 64); /* queued, not yet dispatched */
    check_eq_i(a.rt->wait[0].count, 2, "WAIT was partially consumed");
    check(q2_save_capture(&saved, &live, NULL, "SLES-01534", "BASE0", 0) == Q2_OK,
          "capture scheduler between deadlines");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "scheduler survives disk");
    q2_event_rt_advance(a.rt, 141); q2_event_rt_update(a.rt);
    expected = a.rt->deferred[0]; next_random = q2_random_state();
    check_eq_i(expected.period, 90, "original rerolls the next period");
    check(q2_save_apply(&loaded, &rebuilt, NULL, "SLES-01534", "BASE0") == Q2_OK,
          "restore into a rebuilt scheduler");
    check_eq_i(b.rt->clock, 70, "restore the scheduler clock");
    check_eq_i(b.rt->deferred[0].due, 210, "retain remaining delay, not a full restart");
    check_eq_i(b.rt->deferred_count, 1, "rebuild the active slot count");
    check_eq_i(b.rt->wait[0].count, 2, "retain WAIT countdown");
    check_eq_i(b.rt->wait[0].stamp, 2, "retain WAIT consecutive-arrival stamp");
    check_eq_i(b.calls[1] + b.calls[2], 0, "loading dispatches nothing");
    q2_event_rt_advance(b.rt, 141); q2_event_rt_update(b.rt);
    check_eq_i(b.calls[1], 1, "saved timer runs its one-item window");
    check_eq_i(b.calls[2], 1, "queued trigger is not lost or repeated");
    check(memcmp(&expected, &b.rt->deferred[0], sizeof(expected)) == 0 &&
          q2_random_state() == next_random, "rearm and randomness match uninterrupted play");
    q2_event_rt_advance(b.rt, 91); q2_event_rt_update(b.rt);
    check_eq_i(b.calls[1], 2, "saved repeat count reaches the final deadline");
    check_eq_i(b.rt->deferred_count, 0, "finite timer frees the restored slot");
    check_eq_i(q2_random_state(), next_random, "final deadline draws no random number");
    loaded.event_runtime.timers[0].timer_index = 1;
    rebuilt.player[0].pos[0] = 987;
    check(q2_save_apply(&loaded, &rebuilt, NULL, "SLES-01534", "BASE0") == Q2_ERR_BAD_FORMAT &&
          rebuilt.player[0].pos[0] == 987, "reject an inconsistent item key before mutating the sim");
    loaded.event_runtime.timers[0].timer_index = 0;
    raw[18] = 11;
    check(q2_save_apply(&loaded, &rebuilt, NULL, "SLES-01534", "BASE0") == Q2_ERR_BAD_FORMAT,
          "reject a changed script with the same byte and record counts");
    raw[18] = 10;
    loaded.event_runtime_present = false;
    check(q2_save_apply(&loaded, &rebuilt, NULL, "SLES-01534", "BASE0") == Q2_OK,
          "old saves without a scheduler retain legacy reconstruction");
    saved.event_runtime.timers[1] = saved.event_runtime.timers[0];
    check(q2_save_write(&saved, tmp_path()) == Q2_ERR_BAD_FORMAT, "duplicate timer keys are refused");
    q2_save_free(&saved); q2_save_free(&loaded);
    q2_sim_free(&live); q2_sim_free(&rebuilt);
    remove(tmp_path());
}

/*
 * VERSION 5 IS MIGRATED. The runtime that wrote version 5 refused a record on
 * ONESHOT && HASRUN and never set DISABLED for it, so a spent one-shot record
 * is 0x49 in a v5 file. Today's gate is DISABLED alone (0x8002799C) — loaded
 * as-is, that record would fire again. The reader gives it its DISABLED bit.
 */
static void test_projectile_chunk_order(void)
{
    save_image image;
    q2_save loaded;
    u32 order, part, at, i;
    for (order = 0; order < 2; order++) {
        memset(&image, 0, sizeof(image));
        for (part = 0; part < 2; part++) {
            if (part == order) {
                at = img_chunk_begin(&image, "PRST");
                img_u32(&image, 1); img_u32(&image, Q2_PROJ_MAX);
                for (i = 0; i < Q2_PROJ_MAX; i++)
                    img_u32(&image, i == 0 ? (27u << 16) | Q2_PROJ_FLAGS_HYPERBLASTER : 0);
            } else {
                at = img_chunk_begin(&image, "PROJ");
                img_u32(&image, 1);
                img_u8(&image, 1); img_u8(&image, Q2_PROJ_BFG);
                for (i = 0; i < 43; i++) img_u8(&image, 0);
            }
            img_chunk_end(&image, at);
        }
        check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_OK &&
              loaded.proj[0].in_use && loaded.proj[0].kind == Q2_PROJ_BFG &&
              loaded.proj[0].flags == Q2_PROJ_FLAGS_HYPERBLASTER &&
              loaded.proj[0].beam_time == 27,
              "projectile runtime state is independent of chunk order");
        q2_save_free(&loaded);
    }
    remove(tmp_path());
}

static void test_migrates_v5_spent_oneshots(void)
{
    /*
     * 0x49 CAT_A|ONESHOT|HASRUN      spent under v5         -> 0xC9
     * 0xC9 ... |DISABLED             already disabled       -> 0xC9
     * 0x48 CAT_A|ONESHOT             never ran              -> 0x48
     * 0x19 CAT_A|CAT_B|HASRUN        repeatable, has run    -> 0x19
     * 0x51 CAT_B|ONESHOT|HASRUN      spent                  -> 0xD1
     * 0x5D ONESHOT|CAT_B|CAT_A|RT2|HASRUN, contact bit too  -> 0xDD
     */
    static const u8 old_bytes[6] = { 0x49, 0xC9, 0x48, 0x19, 0x51, 0x5D };
    static const u8 migrated[6]  = { 0xC9, 0xC9, 0x48, 0x19, 0xD1, 0xDD };
    save_image body;
    q2_save loaded;
    const char *path = tmp_path();

    printf("version 5 event flags\n");

    img_body(&body, old_bytes, 6);

    check(img_write(path, 5, &body) &&
              q2_save_read(&loaded, path) == Q2_OK &&
              loaded.event_count == 6 && loaded.event_flags &&
              memcmp(loaded.event_flags, migrated, 6) == 0,
          "a v5 file reads with every spent one-shot record disabled, "
          "and nothing else touched");
    q2_save_free(&loaded);

    /* The same bytes in a version-6 file mean what they say: 0x49 there is a
     * one-shot record an ENABLE re-armed (0x800278B0 clears bit 7 only), and
     * the reader must leave it armed. */
    check(img_write(path, Q2_SAVE_VERSION, &body) &&
              q2_save_read(&loaded, path) == Q2_OK &&
              loaded.event_count == 6 && loaded.event_flags &&
              memcmp(loaded.event_flags, old_bytes, 6) == 0,
          "a v6 file's 0x49 stays 0x49: re-armed, not spent");
    q2_save_free(&loaded);

    remove(path);
}

/*
 * And the point of the migration, end to end: an old save of a level where a
 * one-shot record has fired, loaded into today's runtime, must not fire it
 * again.
 */
static void test_v5_spent_record_stays_spent(void)
{
    static const u8 old_bytes[2] = {
        (u8)(Q2_EVREC_CAT_A | Q2_EVREC_ONESHOT | Q2_EVREC_HASRUN),   /* 0x49 */
        (u8)(Q2_EVREC_CAT_B | Q2_EVREC_HASRUN)                       /* 0x11 */
    };
    u8 raw[40];
    save_image body;
    q2_save loaded;
    q2_sim sim;
    const char *path = tmp_path();

    printf("a spent record in a v5 save stays spent\n");

    img_body(&body, old_bytes, 2);
    q2_sim_init(&sim, NULL, 50);
    save_events_chunk(raw, &sim.events);

    check(img_write(path, 5, &body) &&
              q2_save_read(&loaded, path) == Q2_OK &&
              save_events_attach(&sim) &&
              q2_save_apply(&loaded, &sim, NULL, "SLES-01534", "BASE1")
                  == Q2_OK &&
              q2_event_rt_trigger(&sim.event_rt, SAVE_EV_ONESHOT_RECORD),
          "a hand-built v5 save applies to a runtime over the same script");

    q2_event_rt_update(&sim.event_rt);
    check_eq_i(sim.event_rt.call_count, 0,
               "the one-shot record it saved as spent refuses to run");

    q2_save_free(&loaded);
    q2_sim_free(&sim);
    remove(path);
}

/* EVIT: a one-shot ITEM spent before the save stays spent after the load. */
static void test_item_latches_round_trip(void)
{
    u8 raw[40];
    q2_sim source, restored;
    q2_inventory inv;
    q2_save saved, loaded;
    const char *path = tmp_path();

    printf("event item latches\n");

    q2_sim_init(&source, NULL, 50);
    save_events_chunk(raw, &source.events);
    check(save_events_attach(&source), "the source runtime starts");
    q2_event_rt_trigger(&source.event_rt, SAVE_EV_MIXED_RECORD);
    q2_event_rt_update(&source.event_rt);
    q2_inventory_init(&inv);

    check(q2_save_capture(&saved, &source, &inv, "SLES-01534", "BASE1", 0)
              == Q2_OK &&
              saved.event_item_count == 40 && saved.event_item_flags &&
              saved.event_item_flags[SAVE_EV_MIXED_ITEM] == Q2_EVOP_DISABLED,
          "capture takes the one-shot item's latch");

    q2_sim_init(&restored, NULL, 50);
    save_events_chunk(raw, &restored.events);
    check(q2_save_write(&saved, path) == Q2_OK &&
              q2_save_read(&loaded, path) == Q2_OK &&
              save_events_attach(&restored) &&
              q2_save_apply(&loaded, &restored, NULL, "SLES-01534", "BASE1")
                  == Q2_OK &&
              q2_event_rt_item_flags(&restored.event_rt, SAVE_EV_MIXED_ITEM)
                  == Q2_EVOP_DISABLED,
          "the latch survives the file and the apply");

    q2_event_rt_trigger(&restored.event_rt, SAVE_EV_MIXED_RECORD);
    q2_event_rt_update(&restored.event_rt);
    check_eq_i(restored.event_rt.call_count, 0,
               "so the repeatable record runs without its spent item");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&source);
    q2_sim_free(&restored);
    remove(path);
}

/*
 * A file that is the right length and wrong in the middle. The chunk sizes
 * cannot catch this; the checksum is what exists for it, and a save system
 * without one turns a bad byte into a game that behaves strangely.
 */
static void test_permanent_object_state(void)
{
    u8 raw[40];
    q2_sim sim;
    q2_save saved, loaded;
    q2_event_carry carry;
    puts("permanent object changes without one-shot latches");
    q2_sim_init(&sim, NULL, 50);
    save_events_chunk(raw, &sim.events);
    raw[11] = Q2_EVREC_CAT_A;
    raw[28] = Q2_EVOP_CALL;
    check(save_events_attach(&sim), "object-state runtime attaches");
    sim.event_rt.object_flags[12] = 1;
    check(q2_save_capture(&saved, &sim, NULL, "SLES-01534", "SECURITY", 2) == Q2_OK,
          "capture includes permanent changes");
    check(saved.event_object_count == sizeof(raw) && saved.event_object_flags[12],
          "object flags are separate from the script's control flags");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "object state survives the file");
    sim.event_rt.object_flags[28] = 1; /* Change made after saving. */
    check(q2_save_event_carry(&loaded, &sim.event_rt, &carry) == Q2_OK &&
          carry.destroyed_items[12] && !carry.destroyed_items[28],
          "load replays only the saved object changes");
    q2_event_rt_carry_in(&sim.event_rt, &carry);
    q2_event_rt_replay(&sim.event_rt, &carry);
    check_eq_i(sim.event_rt.call_count, 1, "only the changed item is replayed");
    q2_event_rt_carry_out(&sim.event_rt, &carry);
    check(carry.destroyed_items[12] && !carry.destroyed_items[28],
          "the next zone departure retains that same object state");
    sim.event_rt.object_flags[12] = 0;
    check(q2_save_apply(&loaded, &sim, NULL, "SLES-01534", "SECURITY") == Q2_OK &&
          sim.event_rt.object_flags[12], "direct apply restores the marker too");
    loaded.event_object_count++;
    check(q2_save_write(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "mismatched object-state length is rejected");
    loaded.event_object_count--;
    free(loaded.event_object_flags); loaded.event_object_flags = NULL;
    loaded.event_object_count = 0;
    check(q2_save_event_carry(&loaded, &sim.event_rt, &carry) == Q2_OK &&
          !carry.destroyed_items[12], "legacy saves never borrow later object changes");
    q2_save_free(&saved); q2_save_free(&loaded); q2_sim_free(&sim);
    remove(tmp_path());
}

static void test_event_history_round_trip(void)
{
    u8 raw[40];
    q2_sim sim;
    q2_save saved, loaded, legacy = {0};
    q2_event_store history = {0}, restored = {0};
    q2_event_carry *block;

    printf("visited-map script history\n");
    q2_sim_init(&sim, NULL, 50);
    save_events_chunk(raw, &sim.events);
    check(save_events_attach(&sim), "history fixture attaches");
    q2_event_rt_trigger(&sim.event_rt, SAVE_EV_ONESHOT_RECORD);
    q2_event_rt_update(&sim.event_rt);
    block = q2_event_store_open(&history, "WASTE1");
    check(block != NULL, "visited map has a history block");
    if (block) q2_event_rt_carry_out(&sim.event_rt, block);
    q2_event_rt_trigger(&sim.event_rt, SAVE_EV_MIXED_RECORD);
    q2_event_rt_update(&sim.event_rt);
    block = q2_event_store_open(&history, "WASTE2");
    if (block) {
        q2_event_rt_carry_out(&sim.event_rt, block);
        block->destroyed_items[12] = 1;
    }

    check(q2_save_capture(&saved, &sim, NULL, "SLES-01534", "WASTE3", 0)
              == Q2_OK, "history snapshot captures");
    check(q2_save_capture_event_history(&saved, &history) == Q2_OK,
          "history is captured with the save");
    q2_event_store_free(&history);
    check(q2_save_write(&saved, tmp_path()) == Q2_OK,
          "history snapshot writes independently of the live store");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK,
          "history snapshot reads");
    q2_event_store_open(&restored, "UNSAVED");
    check(q2_save_restore_event_history(&loaded, &restored) == Q2_OK,
          "restore replaces the abandoned session's store");
    check_eq_i(restored.count, 2, "both visited maps, no abandoned map");
    check(q2_event_store_find(&restored, "UNSAVED") == NULL,
          "later progression does not leak into the save");
    block = q2_event_store_find(&restored, "waste1");
    check(block && (block->record_flags[0] & Q2_EVREC_DISABLED) &&
              block->item_flags[SAVE_EV_MIXED_ITEM] == 0,
          "the first map retains its own latches");
    block = q2_event_store_find(&restored, "WASTE2");
    check(block && block->item_flags[SAVE_EV_MIXED_ITEM] == Q2_EVOP_DISABLED,
          "the second map retains its different item history");
    check(block && block->destroyed_items[12],
          "non-one-shot destruction survives with the visited map");
    q2_save_free(&loaded);
    check(block && block->size == sizeof(raw), "restore owns its blocks");

    check(q2_save_restore_event_history(&legacy, &restored) == Q2_OK &&
              restored.count == 0,
          "older saves clear unrelated history rather than borrowing it");
    q2_event_store_free(&restored);
    q2_save_free(&saved);
    q2_sim_free(&sim);
    remove(tmp_path());
}

static void test_saved_script_replay(void)
{
    u8 raw[40];
    q2_sim sim;
    q2_save saved;
    q2_event_carry carry;
    u32 calls;

    printf("replay uses saved script state\n");
    q2_sim_init(&sim, NULL, 50);
    save_events_chunk(raw, &sim.events);
    check(save_events_attach(&sim), "replay fixture attaches");
    check(q2_save_capture(&saved, &sim, NULL, "SLES-01534", "BASE1", 0)
              == Q2_OK, "save before the switch is used");
    q2_event_rt_trigger(&sim.event_rt, SAVE_EV_ONESHOT_RECORD);
    q2_event_rt_update(&sim.event_rt);
    calls = sim.event_rt.call_count;
    check(q2_save_event_carry(&saved, &sim.event_rt, &carry) == Q2_OK,
          "prepare saved latches before rebuilding world objects");
    q2_event_rt_carry_in(&sim.event_rt, &carry);
    q2_event_rt_replay(&sim.event_rt, &carry);
    check_eq_i(sim.event_rt.call_count, calls,
               "a switch used after the save is not replayed");
    q2_save_free(&saved);

    q2_event_rt_trigger(&sim.event_rt, SAVE_EV_ONESHOT_RECORD);
    q2_event_rt_update(&sim.event_rt);
    check(q2_save_capture(&saved, &sim, NULL, "SLES-01534", "BASE1", 0)
              == Q2_OK, "save after the switch is used");
    q2_event_rt_free(&sim.event_rt);
    check(save_events_attach(&sim), "a new process starts with fresh script state");
    check(q2_save_event_carry(&saved, &sim.event_rt, &carry) == Q2_OK,
          "the cold load builds its replay from the save");
    q2_event_rt_carry_in(&sim.event_rt, &carry);
    q2_event_rt_replay(&sim.event_rt, &carry);
    check_eq_i(sim.event_rt.call_count, 1,
               "the switch's world effect is reconstructed on cold load");
    saved.event_count++;
    check(q2_save_event_carry(&saved, &sim.event_rt, &carry) == Q2_ERR_BAD_FORMAT
              && carry.size == 0,
          "mismatched saved script counts cannot be replayed");
    saved.event_count--;
    saved.event_item_count++;
    check(q2_save_event_carry(&saved, &sim.event_rt, &carry) == Q2_ERR_BAD_FORMAT,
          "mismatched item latches cannot be replayed");
    saved.event_item_count--;
    q2_save_free(&saved);
    q2_sim_free(&sim);
}

static void test_explosive_save_and_transient_models(void)
{
    u8 population[120] = {0};
    dat_chunk chunk = {0};
    q2_common_file common = {0};
    q2_sim source, restored;
    q2_save saved, loaded;
    q2_entity *effect;
    q2_explosive group = {0};
    q2_explosive_set groups = {0};

    printf("destroyable groups and transient models\n");
    batch_put_group(population, 0, "Zone0", 32);
    batch_put_place(population, 32, 1000, 27);
    chunk.data = population;
    chunk.size = sizeof(population);
    common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(&source, NULL, 50);
    q2_sim_init(&restored, NULL, 50);
    check(q2_sim_attach_items(&source, &common, 0, NULL, NULL) == Q2_OK &&
              q2_sim_attach_items(&restored, &common, 0, NULL, NULL) == Q2_OK,
          "both worlds start with the same pickup");
    effect = q2_entity_alloc(&source.entities);
    check(effect != NULL, "a live explosion takes the next slot");
    if (effect) {
        effect->kind = Q2_ENT_KIND_MODEL;
        effect->think = q2_model_ent_think;
    }
    effect = q2_entity_alloc(&source.entities);
    if (effect) {
        effect->kind = Q2_GIB_BOSS_CLASS;
        effect->think = q2_gib_think;
    }
    group.item_offset = 24;
    group.health = 3;
    groups.items = &group;
    groups.count = 1;
    source.explosives = &groups;
    check(q2_save_capture(&saved, &source, NULL, "SLES-01534", "BASE0", 0)
              == Q2_OK, "save during an explosion captures");
    check(saved.entity_count == 3 && !saved.entities[1].in_use &&
              !saved.entities[2].in_use,
          "explosion and boss-gib slots are empty in the save");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
              q2_save_read(&loaded, tmp_path()) == Q2_OK,
          "the explosive group survives the file");
    group.health = 10;
    restored.explosives = &groups;
    restored.breakable_count = 2;
    restored.breakable[0].kind = restored.breakable[1].kind = Q2_BREAKABLE_FXGROUP;
    restored.breakable[0].item_offset = restored.breakable[1].item_offset = 24;
    check(q2_save_apply(&loaded, &restored, NULL, "SLES-01534", "BASE0") == Q2_OK,
          "a mid-explosion save restores without requiring transient models");
    check(restored.entities.count == 3 && restored.entities.ent[0].in_use &&
              !restored.entities.ent[1].in_use && !restored.entities.ent[2].in_use,
          "the real pickup survives and the effect slots remain reusable");
    check(group.health == 3 && restored.breakable[0].health == 3 &&
              restored.breakable[1].health == 3,
          "group health, shared by all hitboxes, is restored exactly");
    loaded.explosives[0].destroyed = 1;
    check(q2_save_apply(&loaded, &restored, NULL, "SLES-01534", "BASE0") == Q2_OK
              && group.destroyed && restored.breakable[0].broken &&
              restored.breakable[1].broken,
          "a destroyed group retires every restored hitbox");
    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&source);
    q2_sim_free(&restored);
    remove(tmp_path());
}

static void test_history_rejects_bad_counts(void)
{
    save_image image;
    q2_save loaded;
    u32 at;
    u32 i;
    static const char *const new_tags[] = { "VWPN", "ERTS", "PRST" };

    for (i = 0; i < sizeof(new_tags) / sizeof(new_tags[0]); i++) {
        memset(&image, 0, sizeof(image));
        at = img_chunk_begin(&image, new_tags[i]);
        img_u32(&image, 1); /* Valid version, missing state. */
        img_chunk_end(&image, at);
        check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
              "truncated live-state chunk is rejected");
        memset(&image, 0, sizeof(image));
        at = img_chunk_begin(&image, new_tags[i]);
        img_u32(&image, 99);
        img_chunk_end(&image, at);
        check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
              "unknown live-state chunk version is rejected");
    }
    printf("malformed added save chunks\n");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "EVHS");
    img_u32(&image, 65);
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "an oversized visited-map count is rejected");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "EVHS");
    img_u32(&image, 1);
    img_str(&image, "BASE0", Q2_EVENT_BLOCK_NAME_MAX);
    img_u32(&image, Q2_EVENT_CARRY_MAX + 1);
    img_u32(&image, 1);
    img_u32(&image, 0);
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "an oversized map script cannot overrun the carry arrays");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "EXPL");
    img_u32(&image, 1); /* No entry follows. */
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
              q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "a truncated explosive entry is rejected");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "ROTR");
    img_u32(&image, 1); img_u32(&image, 1); /* Missing rotator. */
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
          q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "a truncated rotator cannot allocate or restore partial motion");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "CAIS");
    img_u32(&image, 1); img_u32(&image, 65536); /* No roster follows. */
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
          q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "an AI roster count is bounded by its available payload");
    memset(&image, 0, sizeof(image));
    at = img_chunk_begin(&image, "SKIL"); img_u32(&image, 4);
    img_chunk_end(&image, at);
    check(img_write(tmp_path(), Q2_SAVE_VERSION, &image) &&
          q2_save_read(&loaded, tmp_path()) == Q2_ERR_BAD_FORMAT,
          "invalid difficulty is rejected before spawning");
    remove(tmp_path());
}

static void test_rotator_resume(void)
{
    q2_rotator_set live = {0}, rebuilt = {0};
    q2_save saved = {0}, loaded = {0}, legacy = {0};
    u32 i, tick;
    printf("rotators resume between endpoints\n");
    for (i = 0; i < 3; i++) {
        q2_rotator *r = q2_rotators_add(&live, (q2_rot_kind)i,
                                         (s16)(20 + i), 1, 40);
        q2_rotator *fresh = q2_rotators_add(&rebuilt, (q2_rot_kind)i,
                                             (s16)(20 + i), 1, 40);
        r->target = fresh->target = 1200;
        r->hold_reset = fresh->hold_reset = 300;
        q2_rotator_trigger(&live, i);
    }
    for (i = 0; i < 3; i++) q2_rotators_tick(&live, 24);
    q2_rotator_trigger(&live, 0); /* Preserve the unconsumed SIMROT request. */
    check(live.rotators[1].angle > 0 && live.rotators[1].angle < 1200,
          "the fixture hatch is moving, not at an endpoint");
    check(q2_save_capture_rotators(&saved, &live) == Q2_OK, "capture rotators");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "rotators survive disk");
    rebuilt.rotators[2].node++;
    check(q2_save_apply_rotators(&loaded, &rebuilt) == Q2_ERR_BAD_FORMAT &&
          rebuilt.rotators[1].angle == 0, "roster mismatch changes no rotator");
    rebuilt.rotators[2].node--;
    check(q2_save_apply_rotators(&loaded, &rebuilt) == Q2_OK, "restore motion");
    check_eq_i(q2_rotator_take_loop(&rebuilt, 1), Q2_ROTLOOP_START,
               "a running hatch restarts its motor on a new voice");
    check(rebuilt.rotators[1].loop_voice == NULL &&
          q2_rotator_take_sound(&rebuilt, 1) == Q2_ROTSND_NONE,
          "no stale mixer handle or repeated opening sound");
    for (tick = 0; tick < 12; tick++) {
        q2_rotators_tick(&live, 24);
        q2_rotators_tick(&rebuilt, 24);
        for (i = 0; i < 3; i++) {
            const q2_rotator *a = &live.rotators[i], *b = &rebuilt.rotators[i];
            check(a->angle == b->angle && a->accum == b->accum &&
                  a->state == b->state && a->delay == b->delay &&
                  a->hold == b->hold && a->step_pending == b->step_pending,
                  "loaded motion follows the uninterrupted timeline");
        }
    }
    rebuilt.rotators[0].angle = 123;
    check(q2_save_apply_rotators(&legacy, &rebuilt) == Q2_OK &&
          rebuilt.rotators[0].angle == 123, "legacy saves retain replay fallback");
    q2_rotators_free(&live); q2_rotators_free(&rebuilt);
    q2_save_free(&saved); q2_save_free(&loaded);
    remove(tmp_path());
}

static q2_creature_world *save_ai_fixture(void)
{
    q2_creature_world *w = calloc(1, sizeof(*w));
    q2_creature_module *mod;
    u32 i;
    if (!w) exit(2);
    w->set.count = w->set.capacity = 3;
    w->set.monsters = calloc(3, sizeof(*w->set.monsters));
    w->corner_count = 2;
    w->corner = calloc(2, sizeof(*w->corner));
    if (!w->set.monsters || !w->corner) exit(2);
    w->mod_count = 1;
    mod = &w->mod[0];
    snprintf(mod->name, sizeof(mod->name), "Test");
    mod->cre.base = 0x80100000;
    mod->cre.move_count = mod->bind.move_count = 1;
    mod->cre.move[0].addr = mod->cre.base + 0x100;
    mod->bind.cre = &mod->cre;
    mod->bind.move[0].first_frame = 10;
    mod->bind.move[0].last_frame = 12;
    mod->bind.move[0].frames = mod->cre.frames;
    for (i = 0; i < 3; i++) {
        q2_monster_init(&w->set.monsters[i]);
        w->set.monsters[i].group = (u16)i;
        w->set.monsters[i].pop_class_id = 18;
        w->set.monsters[i].stand = q2_cre_generic_stand;
    }
    q2_monster_init(&w->sight);
    w->sight.health = 70;
    return w;
}

static void test_ai_resume(void)
{
    q2_creature_world *live = save_ai_fixture(), *rebuilt = save_ai_fixture();
    q2_save saved = {0}, loaded = {0}, legacy = {0};
    q2_monster stranger = {0}, *m = &live->set.monsters[0], *body;
    q2_trail_state trail;
    s32 crumb[3] = { 100, -300, 900 }, skill;
    u32 phase = 0;
    printf("creature behavior resumes through stable links\n");
    q2_level_reset(); q2_trail_init();
    q2_level_state.time = q2_level_state.framenum = 200;
    q2_level_state.total_monsters = 3; q2_level_state.killed_monsters = 1;
    q2_level_state.sight_client = &live->sight;
    q2_level_state.sight_entity = m;
    q2_level_state.sound_entity = &live->sight;
    q2_level_state.sound_entity_framenum = 199;
    live->sight.teleport_time = 198;
    q2_trail_add(crumb);
    m->in_use = true; m->spawnflags |= Q2_SVFLAG_INUSE;
    m->enemy = &live->sight; m->oldenemy = &live->set.monsters[1];
    m->goalentity = &live->corner[1]; m->movetarget = &live->corner[0];
    m->owner = &live->set.monsters[2];
    m->currentmove = &live->mod[0].bind.move[0];
    m->frame = 10; m->think = q2_M_MoveFrame; m->next_think = 201;
    m->attack_finished = 220; m->pain_debounce = 205; m->idle_time = 230;
    m->aiflags = Q2_AI_LOST_SIGHT; m->last_sighting[2] = 1200;
    m->saved_goal[0] = 600; m->trail_time = 199;
    m->velocity[0] = 40; m->maxs[1] = 100; m->on_ground = true;
    body = &live->set.monsters[1];
    body->in_use = body->dead = body->corpse = true;
    body->class_id = Q2_CLASS_CORPSE; body->corpse_was_class = 87;
    body->dissolve = 12; body->effect[2] = 3; body->hit_height = 150;
    saved.skill_present = true; saved.skill = 2;
    check(q2_save_capture_ai(&saved, live, 33333) == Q2_OK, "capture AI state");
    m->enemy = &stranger;
    check(q2_save_capture_ai(&saved, live, 0) == Q2_ERR_BAD_FORMAT,
          "unknown references fail capture without replacing the snapshot");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK &&
          q2_save_read(&loaded, tmp_path()) == Q2_OK, "AI state survives disk");
    check(q2_save_get_skill(&loaded, &skill) && skill == 2,
          "difficulty survives independently of menu settings");
    q2_level_reset(); q2_trail_init();
    rebuilt->set.monsters[2].group = 99;
    check(q2_save_apply_ai(&loaded, rebuilt, &phase) == Q2_ERR_BAD_FORMAT &&
          q2_level_state.time == 0 && !rebuilt->set.monsters[0].enemy,
          "validate the whole roster before applying behavior");
    rebuilt->set.monsters[2].group = 2;
    check(q2_save_apply_ai(&loaded, rebuilt, &phase) == Q2_OK, "restore AI");
    m = &rebuilt->set.monsters[0]; body = &rebuilt->set.monsters[1];
    check(m->enemy == &rebuilt->sight && m->oldenemy == body &&
          m->goalentity == &rebuilt->corner[1] &&
          m->movetarget == &rebuilt->corner[0] &&
          m->owner == &rebuilt->set.monsters[2], "all five links use new objects");
    check(m->currentmove == &rebuilt->mod[0].bind.move[0] && m->frame == 10 &&
          m->think == q2_M_MoveFrame && m->stand == q2_cre_generic_stand,
          "resume the exact animation with rebuilt callbacks");
    check(m->next_think == 201 && m->attack_finished == 220 &&
          m->pain_debounce == 205 && m->idle_time == 230 &&
          m->trail_time == 199, "absolute AI deadlines survive");
    check(m->velocity[0] == 40 && m->maxs[1] == 100 && m->on_ground &&
          m->aiflags == Q2_AI_LOST_SIGHT && m->last_sighting[2] == 1200 &&
          m->saved_goal[0] == 600, "movement and pursuit state survive");
    check(body->corpse && body->dead && body->class_id == Q2_CLASS_CORPSE &&
          body->dissolve == 12 && body->effect[2] == 3 && body->hit_height == 150 &&
          body->stand == NULL, "a detached corpse stays detached at its size");
    check(q2_level_state.time == 200 && q2_level_state.framenum == 200 &&
          q2_level_state.sight_entity == m &&
          q2_level_state.sound_entity == &rebuilt->sight &&
          q2_level_state.sound_entity_framenum == 199 &&
          rebuilt->sight.teleport_time == 198 && phase == 33333,
          "AI clock, pending noise and fractional tick survive");
    q2_trail_capture(&trail);
    check(trail.active && trail.head == 1 && trail.spot[0].timestamp == 200 &&
          trail.spot[0].origin[2] == 900, "pursuit breadcrumbs survive");
    q2_M_MoveFrame(m);
    check(m->frame == 11, "the next frame continues instead of restarting stand");
    check(q2_save_apply_ai(&legacy, rebuilt, &phase) == Q2_OK && m->frame == 11,
          "a legacy file retains its basic creature restore");
    trail.head = Q2_TRAIL_LENGTH;
    check(!q2_trail_restore(&trail), "an invalid trail head is refused");
    q2_save_free(&saved); q2_save_free(&loaded);
    free(live->set.monsters); free(live->corner); free(live);
    free(rebuilt->set.monsters); free(rebuilt->corner); free(rebuilt);
    q2_level_reset(); q2_trail_init();
    remove(tmp_path());
}

static void test_detects_corruption(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    const char *path = tmp_path();
    FILE *f;
    long size;
    u8 *bytes;

    printf("corruption\n");

    build_state(&sim, &inv);
    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 0);
    check(q2_save_write(&saved, path) == Q2_OK, "writes a good file");

    f = fopen(path, "rb");
    if (!f) {
        check(false, "reopens what it just wrote");
        goto done;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = (u8 *)malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(bytes);
        check(false, "reads it back for corrupting");
        goto done;
    }
    fclose(f);

    /* Flip one bit well inside the body, leaving the length untouched. */
    bytes[size / 2] ^= 0x40;

    f = fopen(path, "wb");
    if (f) {
        fwrite(bytes, 1, (size_t)size, f);
        fclose(f);
        check(q2_save_read(&loaded, path) == Q2_ERR_BAD_FORMAT,
              "a single flipped bit is caught rather than loaded");
    }
    free(bytes);

done:
    q2_save_free(&saved);
    q2_sim_free(&sim);
    remove(path);
}

/* ------------------------------------------------------------------------- */
static void test_mission_and_settings(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    q2_mission mission, restored;
    s16 settings[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    s16 back[8];
    int secrets, secrets_total, kills, kills_total;

    printf("mission tallies and settings\n");

    build_state(&sim, &inv);
    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 0);

    q2_mission_init(&mission);
    mission.unit = 2;
    q2_mission_set_row(&mission, 0, "OUTER BASE", 2, 3, 14, 20);
    q2_mission_set_row(&mission, 1, "INSTALLATION", 1, 1, 9, 9);
    q2_save_capture_mission(&saved, &mission);
    q2_save_set_settings(&saved, settings, 8);

    check(q2_save_write(&saved, tmp_path()) == Q2_OK, "writes");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK, "reads back");

    q2_mission_init(&restored);
    q2_save_apply_mission(&loaded, &restored);

    check_eq_i(restored.unit, 2, "the mission number survives");
    check(strcmp(restored.row[0].name, "OUTER BASE") == 0,
          "a level name survives");
    check_eq_i(restored.row[0].kills, 14, "a kill count survives");
    check_eq_i(restored.row[1].secrets_total, 1, "a secret total survives");

    q2_mission_totals(&restored, &secrets, &secrets_total, &kills, &kills_total);
    check_eq_i(kills, 23, "the totals still add up after a round trip");

    /*
     * 0x80022210 — the counters back OUT of a row, which is what the console's
     * save restore does and what the port had no way to do. Reading them is
     * the fallback for a file written before LVLC existed.
     */
    secrets = secrets_total = kills = kills_total = -1;
    check(q2_mission_get_counts(&restored, 0, &secrets, &secrets_total,
                                &kills, &kills_total),
          "a row hands its counters back");
    check_eq_i(secrets, 2, "...the secrets found");
    check_eq_i(secrets_total, 3, "...the secrets there are");
    check_eq_i(kills, 14, "...the kills made");
    check_eq_i(kills_total, 20, "...and the kills there are");
    check(!q2_mission_get_counts(&restored, 6, &secrets, NULL, NULL, NULL),
          "a seventh row has nothing to hand back");
    check_eq_i(secrets, 2, "...and writes nothing when it refuses");

    /* The file this save came from has no LVLC chunk yet. */
    {
        q2_save_level_counters lc;

        check(!q2_save_get_level_counters(&loaded, &lc),
              "a save with no live counters says so");
    }

    memset(back, 0, sizeof(back));
    check_eq_i(q2_save_get_settings(&loaded, back, 8), 8, "eight settings back");
    check_eq_i(back[7], 8, "the last setting survives");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&sim);
    remove(tmp_path());
}

/*
 * LVLC — the counters the mission row is a clamped copy of.
 *
 * The row alone cannot resume a level. It holds one byte per counter and says
 * nothing about WHICH secrets have already counted or WHERE the kills were
 * made, and without those two a restored game either re-counts a secret it has
 * already banked or hands the level's kill tally back to the resident zone's.
 */
static void test_level_counters_round_trip(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    q2_save_level_counters lc, back;

    printf("the level's live counters\n");

    build_state(&sim, &inv);
    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 1);

    memset(&lc, 0, sizeof(lc));
    lc.secrets_found     = 3;
    lc.secret_seen_count = 3;
    lc.secret_seen[0]    = 0x40;
    lc.secret_seen[1]    = 0x118;
    lc.secret_seen[2]    = 0x2A0;
    lc.zone_dead[0]      = 5;   /* a zone left behind */
    lc.zone_placed[0]    = 8;
    lc.zone_dead[1]      = 2;   /* the resident one   */
    lc.zone_placed[1]    = 10;
    q2_save_set_level_counters(&saved, &lc);

    check(q2_save_write(&saved, tmp_path()) == Q2_OK, "writes");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK, "reads back");

    memset(&back, 0, sizeof(back));
    check(q2_save_get_level_counters(&loaded, &back),
          "the chunk comes back present");
    check_eq_i(back.secrets_found, 3, "the found count survives");
    check_eq_i(back.secret_seen_count, 3, "so does the length of the list");
    check_eq_i(back.secret_seen[1], 0x118, "and the offsets in it");
    check_eq_i(back.zone_dead[0], 5, "a departed zone's kills survive");
    check_eq_i(back.zone_placed[0], 8, "...with what it placed");
    check_eq_i(back.zone_dead[1] + back.zone_dead[0], 7,
               "and the level's tally is the sum over its zones");
    check_eq_i(back.zone_dead[2], 0, "a zone never visited stays empty");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    q2_sim_free(&sim);
    remove(tmp_path());
}

/*
 * HELP — the help computer's current message pair, as the two Strings keys.
 *
 * `HelpComputer_GetMessages` (0x800218A8) copies `LastOrders` and
 * `LastObjective` into the unit's save block on the save arm of every
 * `LoadSaveHub`, so the pair the player last read is part of a saved game. The
 * port carried neither, and a reload showed the shipped defaults.
 *
 * Both halves are pinned: that a written file brings the keys back, and that a
 * file with no chunk reports absent rather than empty — the caller has to be
 * able to tell "this unit has had no HELPCOMPUTER" from "this file predates the
 * question", because only the second keeps the session's pair.
 */
static void test_help_keys_round_trip(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded, bare, bare_back;
    q2_save_help hk;

    printf("the help computer's message pair\n");

    build_state(&sim, &inv);
    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 1);

    q2_save_set_help_keys(&saved, "Base1Ord2", "Base1Obj2");
    check(q2_save_write(&saved, tmp_path()) == Q2_OK, "writes");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK, "reads back");

    memset(&hk, 0, sizeof(hk));
    check(q2_save_get_help_keys(&loaded, &hk), "the chunk comes back present");
    check(strcmp(hk.orders_key, "Base1Ord2") == 0, "the orders key survives");
    check(strcmp(hk.objective_key, "Base1Obj2") == 0,
          "the objective key survives");

    q2_save_free(&saved);
    q2_save_free(&loaded);
    remove(tmp_path());

    /* And a capture that never set them: absent, not empty-and-present. */
    q2_save_capture(&bare, &sim, &inv, "SLES-01534", "BASE1", 1);
    check(q2_save_write(&bare, tmp_path()) == Q2_OK, "writes a file with none");
    check(q2_save_read(&bare_back, tmp_path()) == Q2_OK, "reads it back");
    memset(&hk, 0, sizeof(hk));
    check(!q2_save_get_help_keys(&bare_back, &hk),
          "a file with no HELP reports absent");

    /* Twelve zero bytes is what HelpComputer_Reset writes, and it IS an
     * answer: present, with both keys empty. */
    q2_save_set_help_keys(&bare, "", "");
    check(q2_save_get_help_keys(&bare, &hk),
          "a reset pair is present rather than absent");
    check(hk.orders_key[0] == '\0' && hk.objective_key[0] == '\0',
          "...and both keys are empty");

    q2_save_free(&bare);
    q2_save_free(&bare_back);
    q2_sim_free(&sim);
    remove(tmp_path());
}

/* ------------------------------------------------------------------------- */
static void test_slots(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    q2_save_info info[Q2_SAVE_SLOTS];
    char row[64];

    printf("slots\n");

    q2_save_set_dir(tmp_dir());
    check(strcmp(q2_save_dir(), tmp_dir()) == 0, "the directory can be steered");
    clear_slots();

    build_state(&sim, &inv);
    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 2);

    check_eq_i(q2_save_slots_scan(info, Q2_SAVE_SLOTS), 0,
               "no slots are in use to begin with");
    check(!info[0].used, "an empty slot reports unused");

    check(q2_save_slot_write(&saved, 1) == Q2_OK, "writes slot 2");
    check(q2_save_slot_write(&saved, 6) == Q2_ERR_RANGE,
          "refuses a slot that does not exist");

    check_eq_i(q2_save_slots_scan(info, Q2_SAVE_SLOTS), 1, "one slot in use");
    check(info[1].used, "slot 2 is the one");
    check(strcmp(info[1].map, "BASE1") == 0, "the header names the map");
    check_eq_i(info[1].zone, 2, "and the zone");
    check_eq_i(info[1].health, 77, "and the player's condition");
    check_eq_i(info[1].level_time, 123456, "and how long they have played");
    check_eq_i(info[1].weapon_id, 9, "and what they are holding");

    /* The row text. An empty slot must be the EMPTY STRING: that is what makes
     * the screen draw nothing and give it no selection bar. */
    check(q2_save_slot_row(&info[0], 0, row, (u32)sizeof(row))[0] == '\0',
          "an empty row is the empty string");
    check(q2_save_slot_row(&info[1], 1, row, (u32)sizeof(row))[0] != '\0',
          "a used row has text");
    check(strstr(row, "BASE1") != NULL, "and the text names the map");

    check(q2_save_slot_read(&loaded, 1) == Q2_OK, "reads the slot back");
    check_eq_i(loaded.player.pos[0], 1234, "with the state intact");
    q2_save_free(&loaded);

    check(q2_save_slot_delete(1) == Q2_OK, "deletes a slot");
    check_eq_i(q2_save_slots_scan(info, Q2_SAVE_SLOTS), 0, "and it is gone");
    check(q2_save_slot_delete(1) != Q2_OK, "deleting it twice fails");

    q2_save_free(&saved);
    q2_sim_free(&sim);
    clear_slots();
}

/* A slot whose file is nonsense must list as unused rather than as an error, so
 * the screen still draws four rows and none of them is selectable. */
static void test_slot_scan_survives_rubbish(void)
{
    q2_save_info info[Q2_SAVE_SLOTS];
    char path[512];
    FILE *f;

    printf("slots, rubbish\n");

    q2_save_set_dir(tmp_dir());
    clear_slots();

    check(q2_save_slot_path(2, path, (u32)sizeof(path)) == Q2_OK,
          "resolves a slot path");
    f = fopen(path, "wb");
    if (f) {
        fwrite("not a save file at all", 1, 22, f);
        fclose(f);
    }

    check_eq_i(q2_save_slots_scan(info, Q2_SAVE_SLOTS), 0,
               "a rubbish file does not count as a save");
    check(!info[2].used, "and its row is not selectable");

    clear_slots();
}

/* QFRONT distinguishes multiplayer settings from game saves with its file
 * type flag. The host backend does the same with a separate four-slot format. */
static void test_settings_slots_and_ui(void)
{
    q2_settings_blob source, loaded;
    q2_save_ui ui;
    bool used[Q2_SAVE_SLOTS];
    char path[512];
    FILE *f;
    int i;

    printf("multiplayer settings slots\n");
    q2_save_set_dir(tmp_dir());
    clear_settings_slots();

    memset(&source, 0, sizeof(source));
    source.count = 9;
    for (i = 0; i < (int)source.count; i++)
        source.value[i] = (s16)(i == 7 ? -1 : i * 17 - 40);

    check_eq_i(q2_settings_slots_scan(used, Q2_SAVE_SLOTS), 0,
               "no settings slots begin in use");
    check(q2_settings_slot_write(&source, 2) == Q2_OK,
          "writes settings slot 3");
    check(q2_settings_slot_write(&source, Q2_SAVE_SLOTS) == Q2_ERR_RANGE,
          "rejects an out-of-range settings slot");
    check_eq_i(q2_settings_slots_scan(used, Q2_SAVE_SLOTS), 1,
               "one settings slot is in use");
    check(used[2] && !used[0], "the settings scan preserves positions");
    check(q2_settings_slot_read(&loaded, 2) == Q2_OK,
          "reads settings back");
    check_eq_i(loaded.count, source.count, "the settings count survives");
    check_eq_i(loaded.value[7], -1, "signed settings survive");

    /* The settings mode drives the same deferred card flow and does not list
     * game-save files as settings files. */
    q2_save_ui_init(&ui);
    q2_save_ui_open_settings_load(&ui);
    check(q2_save_ui_row(&ui, 0)[0] == '\0',
          "a game-save position is empty in the settings listing");
    check(strstr(q2_save_ui_row(&ui, 2), "MULTIPLAYER") != NULL,
          "a settings row identifies its file type");
    q2_save_ui_choose(&ui, 2);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_BUSY,
               "settings load enters the busy state");
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_LOADED,
               "settings load completes");
    check(q2_save_ui_take_settings(&ui, &loaded),
          "the loaded settings can be collected");
    check_eq_i(loaded.value[8], source.value[8],
               "the UI returns the complete payload");
    q2_save_ui_acknowledge(&ui);
    q2_save_ui_free(&ui);

    /* A save to a different empty position is deferred, then immediately
     * appears in a fresh settings scan. */
    q2_save_ui_init(&ui);
    q2_save_ui_open_settings_save(&ui, &source);
    q2_save_ui_choose(&ui, 1);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_SAVED,
               "settings save completes");
    check(strstr(ui.message, "SETTINGS") != NULL,
          "the report names settings, not a game");
    check(q2_save_ui_row(&ui, 1)[0] != '\0',
          "the written settings row appears");
    q2_save_ui_acknowledge(&ui);
    q2_save_ui_free(&ui);

    /* CRC validation applies to the small format too. */
    check(q2_settings_slot_path(2, path, (u32)sizeof(path)) == Q2_OK,
          "resolves a settings path");
    f = fopen(path, "r+b");
    if (f) {
        int byte;
        fseek(f, 20, SEEK_SET);
        byte = fgetc(f);
        fseek(f, 20, SEEK_SET);
        fputc(byte ^ 0x40, f);
        fclose(f);
        check(q2_settings_slot_read(&loaded, 2) == Q2_ERR_BAD_FORMAT,
              "a corrupt settings payload fails its checksum");
    } else {
        check(false, "opens a settings slot for corruption");
    }

    clear_settings_slots();
}

/* ------------------------------------------------------------------------- */
/* The front end's three entry points, without a screen                        */
/* ------------------------------------------------------------------------- */
static void test_ui_save_flow(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save snapshot;
    q2_save_ui ui;

    printf("front end: saving\n");

    q2_save_set_dir(tmp_dir());
    clear_slots();

    build_state(&sim, &inv);
    q2_save_capture(&snapshot, &sim, &inv, "SLES-01534", "BASE1", 2);

    q2_save_ui_init(&ui);
    q2_save_ui_open_save(&ui, &snapshot);

    check(ui.open, "the front end is open");
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_LIST,
               "it starts on the list");
    check(q2_save_ui_row(&ui, 0)[0] == '\0', "with four empty rows");

    /* Row 0, into an empty slot: no overwrite question, straight to the work. */
    q2_save_ui_choose(&ui, 0);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_BUSY,
               "an empty slot goes straight to the busy state");

    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_SAVED, "the write succeeds");
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_REPORT,
               "and it reports the outcome");
    check(ui.message[0] != '\0', "with something to say");
    check(q2_save_ui_row(&ui, 0)[0] != '\0', "the row now shows the save");

    q2_save_ui_acknowledge(&ui);
    check(!ui.open, "acknowledging closes the front end");
    check_eq_i(ui.status, Q2_SAVE_UI_SAVED, "with the save recorded");

    /* Now the same slot again — this time the question is asked. */
    q2_save_ui_open_save(&ui, &snapshot);
    q2_save_ui_choose(&ui, 0);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_CHOICE,
               "an occupied slot asks before overwriting");
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_RUNNING,
               "and does nothing while it waits");

    /* The choice's NO arm: back to the list, nothing written. */
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_START);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_LIST,
               "declining returns to the list");

    /* And the YES arm. */
    q2_save_ui_choose(&ui, 0);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_BUSY);
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_SAVED, "accepting overwrites");

    q2_save_ui_acknowledge(&ui);
    q2_save_ui_free(&ui);
    q2_save_free(&snapshot);
    q2_sim_free(&sim);
    clear_slots();
}

static void test_ui_load_flow(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save snapshot, taken;
    q2_save_ui ui;

    printf("front end: loading\n");

    q2_save_set_dir(tmp_dir());
    clear_slots();

    build_state(&sim, &inv);
    q2_save_capture(&snapshot, &sim, &inv, "SLES-01534", "BASE1", 2);
    q2_save_slot_write(&snapshot, 3);

    q2_save_ui_init(&ui);
    q2_save_ui_open_load(&ui);

    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_LIST,
               "loading starts on the list too");
    check(q2_save_ui_row(&ui, 3)[0] != '\0', "the written slot shows");

    /* An empty row must not start a load. */
    q2_save_ui_choose(&ui, 0);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_LIST,
               "an empty row does not start a load");

    q2_save_ui_choose(&ui, 3);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_BUSY,
               "a used row does");
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_LOADED, "the read succeeds");

    check(q2_save_ui_take_loaded(&ui, &taken), "the snapshot can be taken");
    check_eq_i(taken.player.pos[0], 1234, "and it is the right one");
    check(!q2_save_ui_take_loaded(&ui, &taken), "taking it twice does not");
    q2_save_free(&taken);

    q2_save_ui_acknowledge(&ui);
    check(!ui.open, "and the front end closes");

    /* Cancelling out of an open session leaves nothing behind. */
    q2_save_ui_open_load(&ui);
    q2_save_ui_close(&ui);
    check(!ui.open, "cancelling closes it");
    check_eq_i(ui.status, Q2_SAVE_UI_CANCELLED, "and says so");

    q2_save_ui_free(&ui);
    q2_save_free(&snapshot);
    q2_sim_free(&sim);
    clear_slots();
}

/* A load whose slot has been deleted between the listing and the choice: the
 * front end has to report rather than pretend. */
static void test_ui_reports_failure(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save snapshot;
    q2_save_ui ui;

    printf("front end: failure\n");

    q2_save_set_dir(tmp_dir());
    clear_slots();

    build_state(&sim, &inv);
    q2_save_capture(&snapshot, &sim, &inv, "SLES-01534", "BASE1", 2);
    q2_save_slot_write(&snapshot, 2);

    q2_save_ui_init(&ui);
    q2_save_ui_open_load(&ui);
    q2_save_slot_delete(2);          /* it goes away under the front end */

    q2_save_ui_choose(&ui, 2);
    q2_save_ui_request(&ui, Q2_SAVEUI_STATE_LIST);
    check_eq_i(q2_save_ui_update(&ui), Q2_SAVE_UI_FAILED,
               "a vanished slot fails rather than loading rubbish");
    check_eq_i(q2_save_ui_poll(&ui), Q2_SAVEUI_STATE_REPORT,
               "and the failure is reported");
    check(strstr(ui.message, "FAIL") != NULL, "in so many words");

    q2_save_ui_acknowledge(&ui);
    check_eq_i(ui.status, Q2_SAVE_UI_FAILED, "the outcome survives the close");

    q2_save_ui_free(&ui);
    q2_save_free(&snapshot);
    q2_sim_free(&sim);
    clear_slots();
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC save tests\n\n");

    test_deferred_item_group_restore();
    test_game_random_restore();
    test_event_scheduler_restore();
    test_projectile_runtime_restore();
    test_projectile_chunk_order();
    test_round_trip();
    test_apply();
    test_apply_rejects_mismatched_map();
    test_rejects_bad_files();
    test_rejects_pre_batch_v4();
    test_migrates_v5_spent_oneshots();
    test_v5_spent_record_stays_spent();
    test_item_latches_round_trip();
    test_event_history_round_trip();
    test_permanent_object_state();
    test_saved_script_replay();
    test_explosive_save_and_transient_models();
    test_history_rejects_bad_counts();
    test_rotator_resume();
    test_ai_resume();
    test_detects_corruption();
    test_mission_and_settings();
    test_level_counters_round_trip();
    test_help_keys_round_trip();
    test_slots();
    test_slot_scan_survives_rubbish();
    test_settings_slots_and_ui();
    test_ui_save_flow();
    test_ui_load_flow();
    test_ui_reports_failure();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
