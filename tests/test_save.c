/*
 * test_save.c — capturing, writing, reading back and applying a save.
 *
 * The round trip is the point. A save format that writes without error and
 * reads back subtly different values is worse than one that fails, because the
 * damage shows up as a corrupted game rather than an error message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save.h"

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

static const char *tmp_path(void)
{
    static char path[512];
    const char *dir = getenv("TEMP");

    if (!dir || !*dir)
        dir = ".";
    snprintf(path, sizeof(path), "%s/q2psx_save_test.sav", dir);
    return path;
}

/* ------------------------------------------------------------------------- */
static void test_round_trip(void)
{
    q2_sim sim;
    q2_inventory inv;
    q2_save saved, loaded;
    s32 spawn[3] = { 1234, -5678, 9012 };

    printf("round trip\n");

    q2_sim_init(&sim, NULL, 50);
    q2_sim_spawn(&sim, spawn, 700);
    sim.player.pitch = -250;

    q2_inventory_init(&inv);
    q2_inventory_add_weapon(&inv, Q2_WEAPON_RAILGUN);
    q2_inventory_add_ammo(&inv, Q2_AMMO_SLUGS, 23);
    q2_inventory_give_key(&inv, 0x0005);
    inv.health = 77;
    inv.armour = 42;

    check(q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE1", 2) == Q2_OK,
          "captures state");
    check_eq_i(saved.pos[0], 1234, "captured x");
    check_eq_i(saved.zone, 2, "captured zone");

    check(q2_save_write(&saved, tmp_path()) == Q2_OK, "writes to disk");
    check(q2_save_read(&loaded, tmp_path()) == Q2_OK, "reads back");

    /* Every field must survive the trip byte for byte. */
    check_eq_i(loaded.pos[0], saved.pos[0], "x survives");
    check_eq_i(loaded.pos[1], saved.pos[1], "y survives, including negatives");
    check_eq_i(loaded.pos[2], saved.pos[2], "z survives");
    check_eq_i(loaded.yaw, saved.yaw, "yaw survives");
    check_eq_i(loaded.pitch, saved.pitch, "pitch survives, including negatives");
    check_eq_i(loaded.zone, saved.zone, "zone survives");
    check(strcmp(loaded.map, "BASE1") == 0, "map name survives");
    check(strcmp(loaded.serial, "SLES-01534") == 0, "serial survives");

    check_eq_i(loaded.inventory.health, 77, "health survives");
    check_eq_i(loaded.inventory.armour, 42, "armour survives");
    check_eq_i(loaded.inventory.ammo[Q2_AMMO_SLUGS], 23, "ammo survives");
    check_eq_i(loaded.inventory.keys, 0x0005, "keys survive");
    check(q2_inventory_has_weapon(&loaded.inventory, Q2_WEAPON_RAILGUN),
          "weapons survive");

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
    q2_inventory_init(&inv);
    inv.health = 55;

    q2_save_capture(&saved, &sim, &inv, "SLES-01534", "BASE0", 0);

    /* Move away and get hurt, then restore. */
    sim.player.pos[0] = 99999;
    sim.player.yaw    = 3000;
    inv.health        = 1;

    check(q2_save_apply(&saved, &sim, &inv, "SLES-01534", "BASE0") == Q2_OK,
          "applies to the matching disc and map");
    check_eq_i(sim.player.pos[0], 100, "position restored");
    check_eq_i(sim.player.yaw, 512, "yaw restored");
    check_eq_i(inv.health, 55, "health restored");

    /* Velocity must be cleared, or the player resumes mid-fall. */
    check_eq_i(sim.player.vel[0], 0, "velocity cleared on restore");

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

    remove(path);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    printf("Q2PSX-PC save tests\n\n");

    test_round_trip();
    test_apply();
    test_rejects_bad_files();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

    return g_failures ? 1 : 0;
}
