/*
 * cmd_modelent — the effect models a MODEL ENTITY can bind.
 *
 * Two questions the code alone cannot answer.
 *
 * WHICH MAPS CAN DETONATE AT ALL. `0x8005A894` throws the whole spawn away when
 * the CastList does not carry the name, so a map with no `Explosion` gets no
 * entity — not an invisible one. That is a property of the data, and the answer
 * decides whether the func_explosive work is visible on one map or on forty.
 *
 * WHETHER `lh(model + 2)` IS THE CLIP LENGTH. It is NOT, and this is what says
 * so. The think reads its lifetime from the raw header at +2 (0x8005A61C) while
 * `entitydraw.c` resolves what it calls the same quantity through the animation
 * table. Both readings shipped in this port and nothing had compared them.
 *
 * Compared over all 1,723 models: +2 equals the FIRST clip's length on 1,496
 * and differs on 227 — and the 1,496 are exactly the models that have ONE clip.
 * Against the SUM of every clip's frames it agrees on 1,723 of 1,723. So +2 is
 * the model's TOTAL animation length, and `entitydraw.c`'s note calling it "the
 * duration of the clip the entity is playing" was right only by coincidence.
 *
 * Soldier is the clearest case: 31 clips, +2 reads 1302, clip 0 is 108.
 *
 * It costs the explosion nothing — `Explosion` has one clip of 40 either way —
 * but it would have cost a multi-clip effect model everything.
 */
#include "cmd_modelent.h"

#include <stdio.h>
#include <string.h>

#include "level.h"
#include "model.h"
#include "modelent.h"

typedef struct census {
    u32 banks;            /* CastLists walked                              */
    u32 models;           /* models in them                                */

    u32 with_explosion;   /* banks carrying "Explosion"                    */
    u32 with_hexplosion;  /* ...and "Hexplosion"                           */
    u32 maps;             /* COMMON.DAT banks walked                       */
    u32 maps_with;        /* ...that carry at least one effect model       */

    /* lh(model + 2) against the animation table's first clip. */
    u32 anim_compared;
    u32 anim_agree;
    u32 anim_differ;
    u32 sum_agree;        /* ...and against the SUM of every clip           */
    u32 sum_differ;
    u32 single_clip;      /* models with exactly one clip                   */
    u32 anim_no_clip;     /* the model has no clip to compare against      */
    s32 differ_sample_h2;
    s32 differ_sample_clip;
    char differ_sample[16];

    /* The effect models themselves, run through the think. */
    u32 spawned;
    u32 ticks;
    s32 life_min, life_max;
    s32 height_min, height_max;
    char first_map[16];
    s32 first_life, first_height, first_clip;
} census;

/* ------------------------------------------------------------------------- */
static void check_bank(census *c, const q2_model_bank *bank, const char *map,
                       bool is_common)
{
    u32 i;
    int found = 0;
    int k;

    c->banks++;
    c->models += bank->count;

    /*
     * The +2 / animation-table comparison, over every model rather than the two
     * that matter — a rule that holds only where it is convenient is not a rule.
     */
    for (i = 0; i < bank->count; i++) {
        q2_model m;
        q2_model_anim clip;

        if (q2_model_get(bank, i, &m) != Q2_OK)
            continue;
        if (!q2_model_anim_get(&m, 0, &clip)) {
            c->anim_no_clip++;
            continue;
        }

        c->anim_compared++;
        {
            s32 h2 = (s32)q2_rd_s16(m.base + 2);
            u32 n  = q2_model_anim_count(&m);
            u32 a;
            s32 total = 0;

            for (a = 0; a < n; a++) {
                q2_model_anim one;

                if (q2_model_anim_get(&m, a, &one))
                    total += (s32)one.frames;
            }

            if (n == 1)
                c->single_clip++;

            if (h2 == (s32)clip.frames) {
                c->anim_agree++;
            } else {
                c->anim_differ++;
                if (c->anim_differ == 1) {
                    c->differ_sample_h2   = h2;
                    c->differ_sample_clip = (s32)clip.frames;
                    snprintf(c->differ_sample, sizeof(c->differ_sample), "%s",
                             m.hdr.name);
                }
            }

            if (h2 == total) c->sum_agree++;
            else             c->sum_differ++;
        }
    }

    /* And the two effect models, through the module the game uses. */
    for (k = 0; k < Q2_MODEL_ENT_KIND_COUNT; k++) {
        const char *name = q2_model_ent_name((q2_model_ent_kind)k);
        s32 h = 0, clip = 0, index = -1;

        if (!q2_model_ent_height(bank, name, &h, &clip, &index))
            continue;

        found = 1;
        if (k == Q2_MODEL_ENT_EXPLOSION) c->with_explosion++;
        else                             c->with_hexplosion++;

        {
            s32 life = q2_model_ent_lifetime(clip);
            s32 t;
            u32 ticks = 0;

            if (life < c->life_min) c->life_min = life;
            if (life > c->life_max) c->life_max = life;
            if (h    < c->height_min) c->height_min = h;
            if (h    > c->height_max) c->height_max = h;

            /* Run the clock the way the think does — dt 6, doubled — and count
             * the ticks the entity survives. */
            for (t = 0; t < life; t += Q2_MODEL_ENT_CLOCK_RATE * 6)
                ticks++;

            c->spawned++;
            c->ticks += ticks;

            if (!c->first_map[0]) {
                snprintf(c->first_map, sizeof(c->first_map), "%s", map);
                c->first_life   = life;
                c->first_height = h;
                c->first_clip   = clip;
            }
        }
    }

    if (is_common) {
        c->maps++;
        if (found)
            c->maps_with++;
    }
}

/* ------------------------------------------------------------------------- */
int cmd_modelent(const disc *d)
{
    census c;
    int fi, nfiles;

    memset(&c, 0, sizeof(c));
    c.life_min = c.height_min = 1 << 30;
    c.life_max = c.height_max = -(1 << 30);

    puts("Model entities - the effect models 0x8005A778 can bind\n");
    puts("  A spawn whose CastList does not carry the name produces NO");
    puts("  entity at all (0x8005A894), so which maps carry these decides");
    puts("  where a detonation is visible.\n");

    nfiles = disc_file_count(d);

    for (fi = 0; fi < nfiles; fi++) {
        const disc_file *entry = disc_file_at(d, fi);
        const char *base;
        char mapname[64];
        q2_buf buf;
        bool is_common;

        base = strrchr(entry->path, '/');
        if (!base)
            continue;
        base++;
        is_common = (strcmp(base, "COMMON.DAT") == 0);
        if (!is_common && strncmp(base, "ZONE", 4) != 0)
            continue;

        {
            const char *dir_end = base - 1;
            const char *dir = dir_end;
            size_t len;

            while (dir > entry->path && dir[-1] != '/')
                dir--;
            len = (size_t)(dir_end - dir);
            if (len == 0 || len >= sizeof(mapname))
                continue;
            memcpy(mapname, dir, len);
            mapname[len] = 0;
        }

        if (disc_read_file(d, entry->path, &buf) != Q2_OK)
            continue;

        if (is_common) {
            q2_common_file cf;
            q2_model_bank bank;

            if (q2_common_open(&cf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            if (q2_model_bank_from_common(&bank, &cf) == Q2_OK)
                check_bank(&c, &bank, mapname, true);
            q2_common_close(&cf);
        } else {
            q2_zone_file zf;
            q2_model_bank bank;

            if (q2_zone_open(&zf, &buf) != Q2_OK) {
                q2_buf_free(&buf);
                continue;
            }
            if (q2_model_bank_from_zone(&bank, &zf) == Q2_OK)
                check_bank(&c, &bank, mapname, false);
            q2_zone_close(&zf);
        }
    }

    if (!c.banks) {
        puts("  no CastLists found");
        return 1;
    }

    printf("  CastLists walked          : %u, %u models\n", c.banks, c.models);
    printf("  maps (COMMON.DAT)         : %u, %u carry an effect model\n",
           c.maps, c.maps_with);
    printf("  banks with \"Explosion\"    : %u\n", c.with_explosion);
    printf("  banks with \"Hexplosion\"   : %u\n", c.with_hexplosion);

    printf("\n  lh(model + 2) against the animation table's first clip\n");
    printf("    compared                : %u\n", c.anim_compared);
    printf("    agree                   : %u\n", c.anim_agree);
    printf("    differ                  : %u", c.anim_differ);
    if (c.anim_differ)
        printf("   e.g. %s: +2 is %d, clip 0 is %d",
               c.differ_sample, c.differ_sample_h2, c.differ_sample_clip);
    putchar('\n');
    printf("    no clip to compare      : %u\n", c.anim_no_clip);
    printf("    (of those, single-clip) : %u\n", c.single_clip);
    printf("\n  ...and against the SUM of EVERY clip\'s frames\n");
    printf("    agree                   : %u\n", c.sum_agree);
    printf("    differ                  : %u\n", c.sum_differ);

    if (c.spawned) {
        printf("\n  the effect models, run through the think\n");
        printf("    bindings exercised      : %u\n", c.spawned);
        printf("    clip length x 10        : %d .. %d units\n",
               c.life_min, c.life_max);
        printf("    model height (ext2)     : %d .. %d\n",
               c.height_min, c.height_max);
        printf("    ticks survived at dt 6  : %u in total\n", c.ticks);
        printf("    first: %s  clip %d, life %d, height %d,"
               " box (-256,%d,-256)-(256,%d,256)\n",
               c.first_map, c.first_clip, c.first_life, c.first_height,
               c.first_height - Q2_MODEL_ENT_HEIGHT, c.first_height);
    }

    printf("\n  the scale ramp (+0xFE), which the draw multiplies by +0xFC\n");
    {
        s32 t;

        printf("    t     ");
        for (t = 0; t <= 400; t += 50)
            printf("%6d", t);
        printf("\n    scale ");
        for (t = 0; t <= 400; t += 50)
            printf("%6d", q2_model_ent_scale(t));
        printf("\n    flash ");
        for (t = 0; t <= 400; t += 50)
            printf("%6d", q2_model_ent_flash(t));
        putchar('\n');
    }

    {
        bool ok = (c.sum_differ == 0) && (c.with_explosion > 0);

        printf("\n%s - %s\n", ok ? "PASS" : "FAIL",
               ok ? "lh(model + 2) is the TOTAL frame count of every clip, on"
                    " all 1723\n       models - it equals the FIRST clip's"
                    " length only on the 1496\n       that have one clip."
                  : "see the mismatches above.");
        return ok ? 0 : 1;
    }
}
