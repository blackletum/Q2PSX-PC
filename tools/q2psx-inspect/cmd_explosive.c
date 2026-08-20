/*
 * cmd_explosive — the destroyable brush groups, checked against the disc.
 *
 * The layout in explosive.h comes from the disassembly of 0x800267C4 and
 * 0x80026A20, and the warning userfuncs.h gives about corpus evidence applies
 * here too: a scalar field that decodes "cleanly" would decode cleanly at the
 * wrong offset as well. So this command reports two kinds of thing and keeps
 * them apart.
 *
 * DISCRIMINATING — the nine s16 node fields. A Scene node index is checked
 * against the zone's own node count, and a wrong table fails that immediately.
 *
 * NOT DISCRIMINATING — health and the two debris counts. Any byte is a
 * plausible byte. They are printed as ranges so a reader can see what the
 * authors used, not as evidence that the offsets are right.
 *
 * ---------------------------------------------------------------------------
 * Counting, and the trap in it
 * ---------------------------------------------------------------------------
 * Every zone carries its own copy of the map's script, so the same authored
 * item exists once in COMMON.DAT and once in each ZONE*.DAT. Walking COMMON
 * inside the zone loop — which is what the rebase tempts you into — reports one
 * map's items up to eight times over. Chunks are counted once here, and the
 * total is meant to agree with `q2psx-inspect events`.
 *
 * The SLOTS still need a zone, because they are OBJSLOTs and the operand a
 * COMMON item carries is usually -1 (userfuncs.h #56). Each COMMON item is
 * therefore tried against every zone the map ships and scored on its BEST
 * resolution, so the slot figures below are per authored item rather than per
 * (item, zone) pair.
 */
#include "cmd_explosive.h"

#include <stdio.h>
#include <string.h>

#include "events.h"
#include "explosive.h"
#include "level.h"
#include "scene.h"
#include "userfuncs.h"

/*
 * The maps are enumerated from the DISC, not from a list.
 *
 * The hardcoded table the other commands carry is retail Quake II's — CITY1,
 * COMPLEX, HANGAR1, MINE1 and the rest — and this disc has none of those. It
 * ships FRAGTOWE, PODCITY, THEVAT, TIMS, MATRIX1..9 and the Q* front-end maps,
 * 49 COMMON.DAT in all. Walking the file table cannot go stale against a
 * release nobody has dumped for us yet, which is the same argument build/ makes
 * about keying data tables off the executable hash.
 */

#define MAX_ZONES     8
#define MAX_PER_CHUNK 128

typedef struct census {
    u32 chunks;         /* Events chunks carrying at least one item   */
    u32 items;          /* opcode 0x08 items, ONE count per chunk     */
    u32 items_common;   /* ...of those, in a COMMON.DAT               */
    u32 items_zone;     /* ...and in a ZONE*.DAT                      */
    u32 len_ok;         /* ...of length 28                            */
    u32 maps;

    /* Scalars — read without the rebase, because they are not OBJSLOTs. */
    u32 shootable;      /* health != 0: the constructor installs a callback */
    u32 script_only;    /* health == 0: destroyable only by a script       */
    u32 explodes;       /* item[+25] < 0                                   */

    s32 hp_min, hp_max;
    s32 hit_min, hit_max;
    s32 des_min, des_max;
    u32 hit_hist[4];    /* +24: 0, 1..15, 16..63, 64+                      */

    /* +2..+5, which neither handler reads. Characterised rather than called
     * padding, because it is non-zero on every item on the disc. */
    u32 unread_seen, unread_nonzero;
    u32 unread_distinct[8], unread_distinct_n;
    u32 unread_overflow;

    /* Slots — per authored item, scored on its best resolution. */
    u32 resolved;       /* items that name at least one intact node   */
    u32 unresolved;
    u32 slot_a, slot_b, slot_reveal;
    u32 slot_a_bad, slot_b_bad, slot_reveal_bad;
    u32 parts[Q2_EXPLOSIVE_MAX_PARTS + 1];
    u32 rubble_any;

    /* The exercise: destroy every scored group and see what moves. */
    u32 destroyed, vis_hidden, vis_shown, bursts, pieces, blasts;
} census;

/* ------------------------------------------------------------------------- */
/* One pass over a chunk, counting only what needs no zone                    */
/* ------------------------------------------------------------------------- */
static void scalars_chunk(census *c, const q2_events *ev, bool is_zone)
{
    q2_event_record rec, prev;
    bool more;
    u32 here = 0;

    for (more = q2_events_first_record(ev, &rec);
         more;
         more = q2_events_next_record(ev, &prev, &rec)) {
        u32 i;

        prev = rec;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            const u8 *p;
            s16 hp;
            u8  hit;
            s8  des;
            u32 unread;

            if (!q2_events_get_item(ev, &rec, i, &item))
                break;
            if (item.opcode != Q2_EVOP_FXGROUP || !item.payload)
                continue;

            c->items++;
            if (is_zone) c->items_zone++; else c->items_common++;
            here++;
            if (item.len != Q2_EXPLOSIVE_ITEM_LEN)
                continue;
            c->len_ok++;

            p      = item.payload - 2;
            hp     = q2_rd_s16(p + 22);
            hit    = q2_rd_u8(p + 24);
            des    = (s8)q2_rd_u8(p + 25);
            unread = q2_rd_u32(p + 2);

            if (hp != 0) c->shootable++; else c->script_only++;
            if (des < 0) c->explodes++;

            if (hp       < c->hp_min)  c->hp_min  = hp;
            if (hp       > c->hp_max)  c->hp_max  = hp;
            if ((s32)hit < c->hit_min) c->hit_min = hit;
            if ((s32)hit > c->hit_max) c->hit_max = hit;
            if ((s32)des < c->des_min) c->des_min = des;
            if ((s32)des > c->des_max) c->des_max = des;

            if (!hit)          c->hit_hist[0]++;
            else if (hit < 16) c->hit_hist[1]++;
            else if (hit < 64) c->hit_hist[2]++;
            else               c->hit_hist[3]++;

            c->unread_seen++;
            if (unread)
                c->unread_nonzero++;
            {
                u32 u, seen = 0;

                for (u = 0; u < c->unread_distinct_n; u++)
                    if (c->unread_distinct[u] == unread) { seen = 1; break; }
                if (!seen) {
                    if (c->unread_distinct_n <
                            sizeof(c->unread_distinct) /
                            sizeof(c->unread_distinct[0]))
                        c->unread_distinct[c->unread_distinct_n++] = unread;
                    else
                        c->unread_overflow++;
                }
            }
        }
    }

    if (here)
        c->chunks++;
}

/* ------------------------------------------------------------------------- */
/* The slots, which need a zone                                               */
/* ------------------------------------------------------------------------- */
/*
 * How well one (chunk, zone) pairing resolves an item: how many of its nine
 * slots name a node. The best pairing across the map's zones is the one
 * scored, because an item's operands live in the zone that owns its geometry
 * and read -1 everywhere else.
 */
typedef struct best {
    u32          score;
    q2_explosive e;
    bool         have;
} best;

static void score_pair(best *slots, const q2_events *ev,
                       const q2_uf_operands *ops, const q2_scene *scene)
{
    q2_explosive_set set;
    u32 i;

    if (q2_explosives_build(&set, ev, ops, scene) != Q2_OK)
        return;

    for (i = 0; i < set.count && i < MAX_PER_CHUNK; i++) {
        const q2_explosive *e = &set.items[i];
        u32 score = e->part_count;
        int k;

        for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++)
            if (e->rubble[k] >= 0)
                score++;
        if (e->reveal >= 0)
            score++;

        if (!slots[i].have || score > slots[i].score) {
            slots[i].have  = true;
            slots[i].score = score;
            slots[i].e     = *e;
        }
    }

    q2_explosives_free(&set);
}

/*
 * The RAW slot read, which is where an out-of-range index would show up: the
 * module drops one before a caller can see it, so the discriminating test has
 * to happen on the bytes.
 */
static void raw_slots(census *c, const q2_events *ev,
                      const q2_uf_operands *ops, const q2_scene *scene)
{
    q2_event_record rec, prev;
    bool more;

    if (!scene)
        return;

    for (more = q2_events_first_record(ev, &rec);
         more;
         more = q2_events_next_record(ev, &prev, &rec)) {
        u32 i;

        prev = rec;

        for (i = 0; i < rec.n_items; i++) {
            q2_event_item item;
            const u8 *p;
            s16 r;
            int k;

            if (!q2_events_get_item(ev, &rec, i, &item))
                break;
            if (item.opcode != Q2_EVOP_FXGROUP || !item.payload)
                continue;
            if (item.len != Q2_EXPLOSIVE_ITEM_LEN)
                continue;

            p = q2_uf_operand_at(ops, item.payload - 2,
                                 Q2_EXPLOSIVE_ITEM_LEN);

            for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++) {
                s16 a = q2_rd_s16(p + 6 + 2 * k);
                s16 b = q2_rd_s16(p + 14 + 2 * k);

                if (a >= 0 && (u32)a >= scene->node_count) c->slot_a_bad++;
                if (b >= 0 && (u32)b >= scene->node_count) c->slot_b_bad++;
            }
            r = q2_rd_s16(p + 26);
            if (r >= 0 && (u32)r >= scene->node_count) c->slot_reveal_bad++;
        }
    }
}

/* ------------------------------------------------------------------------- */
static void fold_best(census *c, best *slots, u32 n, const q2_scene *scene,
                      const char *map, u32 *printed)
{
    u32 i;

    for (i = 0; i < n; i++) {
        q2_explosive_set    one;
        q2_explosive        copy;
        q2_explosive_result res;
        int k;

        if (!slots[i].have)
            continue;

        copy = slots[i].e;

        if (copy.part_count) c->resolved++;
        else                 c->unresolved++;

        c->parts[copy.part_count]++;
        c->slot_a += copy.part_count;
        for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++)
            if (copy.rubble[k] >= 0)
                c->slot_b++;
        for (k = 0; k < Q2_EXPLOSIVE_MAX_PARTS; k++)
            if (copy.rubble[k] >= 0) { c->rubble_any++; break; }
        if (copy.reveal >= 0)
            c->slot_reveal++;

        if (*printed < 10 && copy.part_count) {
            q2_scene_node n0;
            s32 at[3] = { 0, 0, 0 };
            s32 lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };

            /* Where it stands, so a reader can go and look at one. The centre
             * is the same one the destruction puts its explosion at. */
            if (scene && q2_scene_get_node(scene, (u32)copy.node[0], &n0)) {
                int a;

                for (a = 0; a < 3; a++) {
                    lo[a] = n0.bbox_min[a];
                    hi[a] = n0.bbox_max[a];
                    at[a] = (lo[a] + hi[a]) / 2;
                }
            }

            printf("    %-9s off %5u  hp %4d %-11s  hit %3u  destroy %4d %s\n"
                   "                intact [%4d %4d %4d %4d]"
                   "  rubble [%4d %4d %4d %4d]  reveal %4d\n"
                   "                at (%d, %d, %d), box"
                   " (%d,%d,%d)-(%d,%d,%d)\n",
                   map, copy.item_offset, copy.health,
                   copy.damageable ? "(shootable)" : "(script)",
                   copy.hit_pieces, copy.destroy,
                   copy.destroy < 0 ? "BOOM" : "    ",
                   copy.node[0], copy.node[1], copy.node[2], copy.node[3],
                   copy.rubble[0], copy.rubble[1], copy.rubble[2],
                   copy.rubble[3], copy.reveal,
                   at[0], at[1], at[2],
                   lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            (*printed)++;
        }

        /*
         * Destroy it through the module, so this measures what the port does
         * rather than what the note above says it should. A one-entry set is
         * enough: `q2_explosive_damage` takes an index into whatever it is
         * given.
         */
        one.items    = &copy;
        one.count    = 1;
        one.capacity = 1;

        q2_explosive_initial_vis(&one, 0, &res);

        if (q2_explosive_damage(&one, 0, 0, 0, false, scene, &res)) {
            u32 b;

            c->destroyed++;
            c->vis_hidden += res.hide_count;
            c->vis_shown  += res.show_count;
            c->bursts     += res.burst_count;
            for (b = 0; b < res.burst_count; b++) {
                c->pieces += res.burst[b].pieces;
                if (res.burst[b].explode)
                    c->blasts++;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
int cmd_explosive(const disc *d, const char *map)
{
    census c;
    int fi, nfiles;
    u32 printed = 0;

    memset(&c, 0, sizeof(c));
    c.hp_min = c.hit_min = c.des_min = 1 << 30;
    c.hp_max = c.hit_max = c.des_max = -(1 << 30);

    puts("Opcode 0x08 - the destroyable brush groups\n");
    puts("  exec 0x800267C4, constructor 0x80026A20, item length 28.");
    puts("  Array A (+6) is the intact geometry and array B (+14) the");
    puts("  wreckage; destroying the group hides one and shows the other.\n");

    nfiles = disc_file_count(d);

    for (fi = 0; fi < nfiles; fi++) {
        const disc_file *entry = disc_file_at(d, fi);
        const char *base;
        char mapname[64];
        char path[280];
        q2_buf cbuf;
        q2_common_file cf;
        q2_events cev;
        bool have_cev;
        u32 before = c.items;
        int zi, zcount = 0;

        q2_buf       zbuf[MAX_ZONES];
        q2_zone_file zf[MAX_ZONES];
        q2_events    zev[MAX_ZONES];
        q2_scene     zsc[MAX_ZONES];
        bool         zhas_sc[MAX_ZONES];

        /* One pass per COMMON.DAT; its directory names the map. */
        base = strrchr(entry->path, '/');
        if (!base || strcmp(base + 1, "COMMON.DAT") != 0)
            continue;
        {
            const char *dir_end = base;
            const char *dir = dir_end;
            size_t len;

            while (dir > entry->path && dir[-1] != '/')
                dir--;
            len = (size_t)(dir_end - dir);
            if (len == 0 || len >= sizeof(mapname))
                continue;
            memcpy(mapname, dir, len);
            mapname[len] = '\0';
        }

        if (map && strcmp(map, mapname) != 0)
            continue;

        if (disc_read_file(d, entry->path, &cbuf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &cbuf) != Q2_OK) {
            q2_buf_free(&cbuf);
            continue;
        }
        have_cev = (q2_events_parse_common(&cev, &cf) == Q2_OK);

        for (zi = 0; zi < MAX_ZONES; zi++) {
            snprintf(path, sizeof(path), "%.*sZONE%d.DAT",
                     (int)(base + 1 - entry->path), entry->path, zi);
            if (disc_read_file(d, path, &zbuf[zcount]) != Q2_OK)
                continue;
            if (q2_zone_open(&zf[zcount], &zbuf[zcount]) != Q2_OK) {
                q2_buf_free(&zbuf[zcount]);
                continue;
            }
            zhas_sc[zcount] = (q2_scene_parse(&zsc[zcount],
                                              &zf[zcount]) == Q2_OK);
            if (q2_events_parse_zone(&zev[zcount], &zf[zcount]) != Q2_OK) {
                q2_zone_close(&zf[zcount]);
                continue;
            }
            zcount++;
        }

        /* The item count and every field that needs no zone — once per chunk. */
        if (have_cev)
            scalars_chunk(&c, &cev, false);
        for (zi = 0; zi < zcount; zi++)
            scalars_chunk(&c, &zev[zi], true);

        /* The slots: COMMON's items scored on their best zone. */
        if (have_cev && zcount) {
            best slots[MAX_PER_CHUNK];
            u32  n;
            int  bestzi = -1;

            memset(slots, 0, sizeof(slots));

            for (zi = 0; zi < zcount; zi++) {
                q2_uf_operands ops;

                ops.base_a = cev.data;
                ops.base_b = zev[zi].data;
                ops.b_size = zev[zi].size;

                score_pair(slots, &cev, &ops,
                           zhas_sc[zi] ? &zsc[zi] : NULL);
                raw_slots(&c, &cev, &ops, zhas_sc[zi] ? &zsc[zi] : NULL);
                if (bestzi < 0 && zhas_sc[zi])
                    bestzi = zi;
            }

            for (n = 0; n < MAX_PER_CHUNK && slots[n].have; n++)
                ;
            fold_best(&c, slots, n,
                      (bestzi >= 0) ? &zsc[bestzi] : NULL,
                      mapname, &printed);
        }

        for (zi = 0; zi < zcount; zi++)
            q2_zone_close(&zf[zi]);

        if (c.items != before)
            c.maps++;

        q2_common_close(&cf);
    }

    if (!c.items) {
        puts("  no opcode 0x08 items found");
        return 1;
    }

    printf("\n  items                      : %u in %u chunks, %u maps\n",
           c.items, c.chunks, c.maps);
    printf("    in a ZONE*.DAT           : %u\n", c.items_zone);
    printf("    in a COMMON.DAT          : %u\n", c.items_common);
    printf("  length 28                  : %u  (%s)\n", c.len_ok,
           c.len_ok == c.items ? "all of them" : "MISMATCH");
    printf("  shootable   (health != 0)  : %u\n", c.shootable);
    printf("  script-only (health == 0)  : %u\n", c.script_only);
    printf("  detonate    (+25 negative) : %u\n", c.explodes);

    printf("\n  the discriminating fields - Scene node slots\n");
    printf("    authored items scored    : %u resolve a node, %u never do\n",
           c.resolved, c.unresolved);
    printf("    array A  (+6)            : %u named, %u past the zone's nodes\n",
           c.slot_a, c.slot_a_bad);
    printf("    array B  (+14)           : %u named, %u past the zone's nodes\n",
           c.slot_b, c.slot_b_bad);
    printf("    reveal   (+26)           : %u named, %u past the zone's nodes\n",
           c.slot_reveal, c.slot_reveal_bad);
    printf("    items naming wreckage    : %u\n", c.rubble_any);
    printf("    intact parts per item    : 0:%u  1:%u  2:%u  3:%u  4:%u\n",
           c.parts[0], c.parts[1], c.parts[2], c.parts[3], c.parts[4]);

    printf("\n  the non-discriminating ones - any byte would decode\n");
    printf("    health     (+22)         : %d .. %d\n", c.hp_min, c.hp_max);
    printf("    hit debris (+24)         : %d .. %d"
           "   (0:%u  1-15:%u  16-63:%u  64+:%u)\n",
           c.hit_min, c.hit_max, c.hit_hist[0], c.hit_hist[1],
           c.hit_hist[2], c.hit_hist[3]);
    printf("    destroy    (+25)         : %d .. %d\n", c.des_min, c.des_max);

    printf("\n  +2..+5, which NEITHER handler reads\n");
    printf("    non-zero                 : %u of %u\n",
           c.unread_nonzero, c.unread_seen);
    if (c.unread_distinct_n) {
        u32 k;

        printf("    distinct values          : %u%s  -",
               c.unread_distinct_n + c.unread_overflow,
               c.unread_overflow ? "+" : "");
        for (k = 0; k < c.unread_distinct_n; k++)
            printf(" %08X", c.unread_distinct[k]);
        putchar('\n');
        puts("    Neither 0x800267C4 nor 0x80026A20 contains a load at +2..+5,");
        puts("    so this is authored data the engine ignores - recorded here,");
        puts("    not acted on.");
    }

    printf("\n  destroying every scored group\n");
    printf("    groups destroyed         : %u\n", c.destroyed);
    printf("    nodes hidden / shown     : %u / %u\n",
           c.vis_hidden, c.vis_shown);
    printf("    debris bursts            : %u, %u pieces\n",
           c.bursts, c.pieces);
    printf("    explosions               : %u\n", c.blasts);

    {
        bool ok = (c.len_ok == c.items) &&
                  (c.slot_a_bad == 0) && (c.slot_b_bad == 0) &&
                  (c.slot_reveal_bad == 0) &&
                  (c.destroyed == c.resolved + c.unresolved) &&
                  (c.resolved > 0);

        printf("\n%s - %s\n", ok ? "PASS" : "FAIL",
               ok ? "every item is 28 bytes, every node slot names a node its"
                    " zone has,\n       and every group can be destroyed."
                  : "see the mismatches above.");
        return ok ? 0 : 1;
    }
}
