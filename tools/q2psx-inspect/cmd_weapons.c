#include "cmd_weapons.h"

#include <stdio.h>
#include <string.h>

#include "combat.h"
#include "ident.h"
#include "weapon.h"
#include "weapontables.h"

static void report(void *user, const char *what, long expected, long got)
{
    (void)user;
    printf("  MISMATCH  %-24s builtin %ld, disc %ld\n", what, expected, got);
}

static const char *ammo_name(s32 t)
{
    static const char *const k[] = { "shells", "bullets", "grenades",
                                     "rockets", "cells", "slugs" };
    if (t < 0 || t >= (s32)(sizeof(k) / sizeof(k[0])))
        return "?";
    return k[t];
}

static const char *kind_name(q2_fire_kind k)
{
    switch (k) {
    case Q2_FK_BULLET:       return "hitscan";
    case Q2_FK_BOLT:         return "bolt";
    case Q2_FK_RAIL:         return "rail";
    case Q2_FK_GRENADE:      return "grenade";
    case Q2_FK_HAND_GRENADE: return "thrown";
    case Q2_FK_ROCKET:       return "rocket";
    case Q2_FK_BFG:          return "bfg";
    default:                 return "-";
    }
}

int cmd_weapons(const disc *d)
{
    q2_build_id id;
    q2_weapon_tables disc_side;
    const q2_weapon_tables *builtin = q2_weapon_tables_builtin();
    q2_result r;
    u32 bad;
    int i;

    if (!d)
        return 1;

    r = q2_identify(d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot identify the disc\n");
        return 1;
    }

    r = q2_weapon_tables_load(&disc_side, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the weapon tables: %s\n",
                q2_result_str(r));
        return 1;
    }

    printf("Weapon tables, read from %s\n\n", id.exe_name);
    printf("  id  name          ammo      per  bit    fire fn     kind"
           "     dmg  quad  pel  kick  muzzle\n");

    for (i = 1; i <= Q2_WT_WEAPON_COUNT; i++) {
        const q2_weapon_behaviour *b = &q2_weapon_behaviour_table[i];
        char dmg[8], quad[8];

        if (b->damage)
            snprintf(dmg, sizeof(dmg), "%d", b->damage);
        else
            snprintf(dmg, sizeof(dmg), "calc");
        if (b->damage_quad)
            snprintf(quad, sizeof(quad), "%d", b->damage_quad);
        else
            snprintf(quad, sizeof(quad), "x4");

        printf("  %2d  %-12s  %-8s  %3d  0x%03X  0x%08X  %-7s  %4s  %4s  %3d  "
               "%4s  %d,%d,%d\n",
               i, disc_side.name[i],
               disc_side.ammo_per_shot[i] ? ammo_name(disc_side.ammo_type[i])
                                          : "-",
               (int)disc_side.ammo_per_shot[i],
               (unsigned)disc_side.owned_bit[i],
               (unsigned)disc_side.fire_fn[i],
               kind_name(b->kind), dmg, quad,
               b->pellets ? b->pellets : 0,
               b->kick_random ? "rand" : "",
               disc_side.muzzle[i][0], disc_side.muzzle[i][1],
               disc_side.muzzle[i][2]);
    }

    printf("\n  auto-switch order:");
    for (i = 0; i < (int)disc_side.autoswitch_count; i++)
        printf(" %s", disc_side.name[disc_side.autoswitch[i]]);
    printf("\n");

    printf("\n  armour classes\n");
    for (i = 0; i < Q2_WT_ARMOUR_CLASSES; i++) {
        static const char *const cls[] = { "jacket", "combat", "body" };
        const q2_wt_armour *a = &disc_side.armour[i];
        printf("    %-7s base %3d  max %3d  normal %5d/4096 (%.2f)  "
               "energy %5d/4096 (%.2f)\n",
               cls[i], a->base_count, a->max_count,
               a->normal_protection, a->normal_protection / 4096.0,
               a->energy_protection, a->energy_protection / 4096.0);
    }

    printf("\n  weapon sounds (%d)\n   ", Q2_WT_SOUND_COUNT);
    for (i = 0; i < Q2_WT_SOUND_COUNT; i++) {
        printf(" %s", disc_side.sound[i]);
        if ((i % 5) == 4)
            printf("\n   ");
    }
    printf("\n");

    printf("\n  bolt hull (0x8009DB1C), eight corners:");
    for (i = 0; i < Q2_WT_BOLT_POINTS; i++)
        printf(" (%d,%d,%d)", disc_side.bolt_shape[i][0],
               disc_side.bolt_shape[i][1], disc_side.bolt_shape[i][2]);
    printf("\n");

    printf("\nchecking the port's built-in copy against the disc\n");
    bad = q2_weapon_tables_diff(&disc_side, builtin, report, NULL);

    /*
     * The adjacency check. Each array is eleven elements with its 1-based base
     * one element earlier, so the words either side of the storage belong to
     * the neighbouring tables. Reading exactly the values below is what says
     * the arrays are eleven long and where they start — a 12- or 13-element
     * reading would swallow one of them.
     */
    {
        struct { const char *what; s32 got, want; } adj[] = {
            { "ammo_per_shot[0] is the bolt hull's terminator",
              disc_side.ammo_per_shot[0],  0x7FFF },
            { "ammo_per_shot[12] is the auto-switch head",
              disc_side.ammo_per_shot[12], 10 },
            { "ammo_type[0] is the owned-bit array's padding",
              disc_side.ammo_type[0],      0 },
            { "owned_bit[12] is the ammo-type base, and is zero",
              (s32)disc_side.owned_bit[12], 0 }
        };
        size_t k;

        printf("\n  table adjacency\n");
        for (k = 0; k < sizeof(adj) / sizeof(adj[0]); k++) {
            bool ok = adj[k].got == adj[k].want;
            printf("    %-48s %s (%d)\n", adj[k].what, ok ? "ok" : "FAILED",
                   adj[k].got);
            if (!ok)
                bad++;
        }
    }

    /* The five call sites the behaviour table claims are fire functions have to
     * be exactly what the pointer array holds, or the transcription belongs to
     * some other function. */
    for (i = 0; i <= Q2_WT_WEAPON_COUNT; i++) {
        if (q2_weapon_behaviour_table[i].addr != disc_side.fire_fn[i]) {
            printf("  MISMATCH  behaviour[%d].addr 0x%08X, table 0x%08X\n",
                   i, (unsigned)q2_weapon_behaviour_table[i].addr,
                   (unsigned)disc_side.fire_fn[i]);
            bad++;
        }
    }

    if (bad == 0)
        printf("\nPASS - %d weapons, %d armour classes, %d sounds, "
               "0 mismatches\n",
               Q2_WT_WEAPON_COUNT, Q2_WT_ARMOUR_CLASSES, Q2_WT_SOUND_COUNT);
    else
        printf("\nFAIL - %u mismatches\n", bad);

    q2_weapon_tables_free(&disc_side);
    return bad ? 1 : 0;
}
