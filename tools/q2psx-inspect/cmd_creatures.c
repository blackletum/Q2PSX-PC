#include "cmd_creatures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crebind.h"
#include "creature.h"
#include "dat.h"
#include "level.h"
#include "reloc.h"

/* Where a module image is relocated for inspection. Any address works; this
 * one is far from the executable's own so a stray pointer is obvious. */
#define CRE_BASE 0x80100000u

/* The disc's level directories. Kept here rather than read from the level
 * table so the command works on a disc whose table location is unknown. */
static const char *const g_maps[] = {
    "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2",
    "CITY1", "CITY2", "CITY3", "COMMAND", "COMPLEX", "CORE", "FACT1",
    "FACT2", "FACT3", "HANGAR1", "HANGAR2", "JAIL2", "JAIL3", "JAIL4",
    "JAIL5", "LAB", "MAGDEMO", "MINE1", "MINE2", "MINE3", "MINE4",
    "MINTRO", "POWER1", "POWER2", "SECURITY", "SEWER1", "SPACE",
    "STRIKE", "TRAIN", "WARE1", "WARE2", "WASTE1", "WASTE2", "WASTE3",
    "WASTE4", NULL
};

static int g_modules;
static int g_distinct;
static int g_covered;
static int g_reloc_fail;
static int g_actions_total;
static int g_actions_done;
static const u8 *g_img;
static size_t g_img_size;
static bool g_last_full;

static void report(const q2_creature *c, const q2_cre_impl *impl)
{
    u8  idx[64];
    u32 n, i, missing = 0;

    printf("\n  %-10s  classes", c->name);
    for (i = 0; i < c->class_count; i++)
        printf(" %u", c->class_byte[i]);
    printf("   scale %d   mass %d\n", c->speed_scale, c->mass);

    printf("    callbacks :");
    for (i = 0; i < 13; i++)
        if (c->callback[i])
            printf(" %s", q2_cre_callback_names[i]);
    printf("\n");

    printf("    moves %2u, frames %3u, methods %2u\n",
           c->move_count, c->frame_count, c->method_count);

    /*
     * Every move's frame RANGE, which is the number a port needs and the one
     * the census never printed. A move is named by its first frame (crebind.h),
     * and the highest last_frame a module reaches is what a drawing side has to
     * be able to show — so the two together say how a creature's animation
     * numbering relates to the CastList clips its model actually carries.
     */
    {
        s32 hi = -1;
        printf("    ranges    :");
        for (i = 0; i < c->move_count; i++) {
            printf(" %d-%d", c->move[i].first_frame, c->move[i].last_frame);
            if (c->move[i].last_frame > hi)
                hi = c->move[i].last_frame;
        }
        printf("\n    highest frame : %d\n", hi);
    }

    n = q2_creature_think_indices(c, idx, sizeof(idx));
    printf("    think     :");
    for (i = 0; i < n; i++) {
        bool have = impl && impl->method[idx[i]] != NULL;
        printf(" %u%s", idx[i], have ? "" : "*");
        if (!have)
            missing++;
    }
    if (n == 0)
        printf(" (none)");
    printf("\n");

    /* The addresses, so an index the port does not cover can be gone and
     * read rather than guessed at. */
    printf("    at        :");
    for (i = 0; i < n; i++)
        printf(" [%u]=%08X", idx[i], c->method[idx[i]]);
    printf("\n");

    /* What each think index actually does, decoded from the module. */
    {
        static q2_cre_think th[Q2_CLASS_METHOD_COUNT];
        u32 got = q2_creature_decode_thinks(c, g_img, g_img_size, CRE_BASE,
                                            th, Q2_CLASS_METHOD_COUNT);
        printf("    actions   : %u of %u think indices decode to an action\n",
               got, n);
        for (i = 0; i < n; i++) {
            const q2_cre_think *t = &th[idx[i]];
            u32 k;
            if (!t->step_count)
                continue;
            printf("      [%2u]", idx[i]);
            for (k = 0; k < t->step_count; k++) {
                const q2_cre_step *st = &t->step[k];
                switch (st->op) {
                case Q2_CRE_OP_SOUND:
                    printf(" sound(%08X)%s", st->addr, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_MELEE:
                    printf(" melee(aim %d,%d,%d dmg %d+r%%%d kick %d)",
                           st->aim[0], st->aim[1], st->aim[2],
                           st->damage_base, st->damage_rand, st->kick);
                    break;
                case Q2_CRE_OP_NEXTFRAME:
                    printf(" nextframe(%d)%s", st->frame,
                           st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_MOVE:
                    printf(" move(%08X)%s", st->addr, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_AIFLAG:
                    printf(" aiflags(+%X-%X)%s", st->flag_set,
                           st->flag_clear & 0xFFFFu, st->gated ? "?" : "");
                    break;
                case Q2_CRE_OP_CALL:
                    printf(" call(+%02X)%s", st->import_ofs,
                           st->gated ? "?" : "");
                    break;
                default:
                    break;
                }
            }
            printf("\n");
        }
        g_actions_total += n;
        g_actions_done  += got;
        g_last_full = (n > 0 && got == n);
    }

    if (impl && impl->name) {
        printf("    port      : %s — transcribed by hand, %u of %u indices\n",
               impl->name, n - missing, n);
        if (missing == 0)
            g_covered++;
    } else {
        printf("    port      : decoded actions — %s\n",
               g_last_full ? "every index acts"
                           : "some indices do not decode");
        if (g_last_full)
            g_covered++;
    }
}

int cmd_creatures(const disc *d)
{
    int mi;
    char seen[32][13];
    int nseen = 0;

    printf("Creature modules, decoded from their own code\n");

    for (mi = 0; g_maps[mi]; mi++) {
        char path[160];
        q2_buf b;
        q2_common_file cf;
        const dat_chunk *bin, *rel;
        u32 boff = 0, roff = 0;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", g_maps[mi]);
        if (disc_read_file(d, path, &b) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &b) != Q2_OK) {
            q2_buf_free(&b);
            continue;
        }

        bin = cf.chunk[q2_common_chunk_index("CreAIBin")];
        rel = cf.chunk[q2_common_chunk_index("CreAIRel")];

        while (bin && rel && bin->size > 4 && boff + 16 < bin->size) {
            char nm[13];
            u32 bnext, rnext;
            size_t body;
            u8 *img;
            q2_reloc_stats st;
            int k, dup = 0;

            memcpy(nm, bin->data + boff, 12);
            nm[12] = 0;
            bnext = q2_rd_u32(bin->data + boff + 12);
            rnext = q2_rd_u32(rel->data + roff + 12);

            g_modules++;

            for (k = 0; k < nseen; k++)
                if (strcmp(seen[k], nm) == 0)
                    dup = 1;

            if (!dup) {
                body = (bnext > boff ? bnext : bin->size) - boff - 16;
                img  = (u8 *)malloc(body);
                if (img) {
                    memcpy(img, bin->data + boff + 16, body);
                    if (q2_reloc_apply(img, body, rel->data + roff + 16,
                                       (rnext > roff ? rnext : rel->size)
                                           - roff - 16,
                                       CRE_BASE, &st) == Q2_OK) {
                        q2_creature c;
                        g_img = img; g_img_size = body;
                        if (q2_creature_decode(&c, img, body, CRE_BASE, nm)) {
                            const q2_cre_impl *impl = q2_cre_impl_find(nm);
                            g_distinct++;
                            report(&c, impl);
                        }
                    } else {
                        printf("\n  %-10s  RELOCATION FAILED\n", nm);
                        g_reloc_fail++;
                    }
                    free(img);
                }
                if (nseen < 32) {
                    memcpy(seen[nseen], nm, 13);
                    nseen++;
                }
            }

            if (bnext <= boff || bnext >= bin->size)
                break;
            boff = bnext;
            roff = rnext;
        }

        q2_common_close(&cf);
    }

    printf("\n  %d modules over the disc, %d distinct, %d relocation failures\n",
           g_modules, g_distinct, g_reloc_fail);
    printf("  %d of %d creatures act: animations off the disc, and every think"
           " index they use resolves to a real action\n",
           g_covered, g_distinct);
    printf("  %d of %d think indices across the disc decode to an action;"
           " a ? marks one behind a branch\n", g_actions_done, g_actions_total);
    printf("  a * marks an index with no hand transcription — it runs from the"
           " decoded action instead\n");

    return g_reloc_fail ? 1 : 0;
}
