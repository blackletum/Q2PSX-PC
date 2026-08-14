/*
 * cmd_zonescript — which script does a trigger volume fire, and what names does
 * a zone's own script carry?
 *
 * Two scripts exist per map. COMMON.DAT has an Events chunk and so does every
 * ZONE*.DAT, and only COMMON has the trigger volumes. A zone's carries 2959
 * CALL items, 805 movers and 619 zone gates across the disc, and this port ran
 * none of them — which looked like the largest piece of level behaviour still
 * missing.
 *
 * ** THE PARAGRAPH THAT USED TO BE HERE WAS WRONG. ** It said "the engine never
 * loads a zone's Events chunk", and listed the zone loader's chunks as AreaConx,
 * CastList, CreAIBin, CreAIRel, MapMod, MapNames, Points, Scene, SortData,
 * SpaceLights and the two hulls, concluding `Events` was not among them.
 *
 * `Events` IS among them. The zone loader's own name run is:
 *
 *     CastList  Events  CreAIBin  CreAIRel  SecondaryCol  SecondaryRem
 *     MapNames  SpaceLights  SortData  Scene  MapMod  Points  AreaConx
 *
 * There are two Events LOADERS, not one. COMMON's at 0x8007AC30 stores into
 * gp+372 (0x800AE774); the ZONE's at 0x8007C14C looks the same string up with
 * base *(gp+18856) — the zone file — and stores into gp+376 (0x8007C234). The
 * old note was right that the string has two references and wrong about what the
 * second one does, and that error cost this port most of its rotating geometry:
 * a rotation CALL reads its object slots from gp+376 while STAMPING -1 into
 * gp+372 as it consumes them (0x800285F4 / 0x8002861C), so parsing COMMON alone
 * sees an empty call every time. See openquestions #56.
 *
 * Two counting tests were run before that and BOTH decided nothing, which is
 * worth keeping so neither is repeated: all 834 trigger offsets start a record
 * in COMMON's script *and* in a zone's, and none runs past the end of either.
 * An offset is just a number, and record starts are dense.
 *
 *
 * What is left for this command to do is measure the script that does run:
 * COMMON's, fired by every trigger volume, with the rotators built from the
 * same chunk the engine's global points at.
 */
#include "cmd_zonescript.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "events_rt.h"
#include "level.h"
#include "rotator.h"
#include "trigger.h"
#include "userfuncs.h"

static const char *const g_maps[] = {
    "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2",
    "CITY1", "CITY2", "CITY3", "COMMAND", "COMPLEX", "CORE", "FACT1",
    "FACT2", "FACT3", "HANGAR1", "HANGAR2", "JAIL2", "JAIL3", "JAIL4",
    "JAIL5", "LAB", "MAGDEMO", "MINE1", "MINE2", "MINE3", "MINE4",
    "MINTRO", "POWER1", "POWER2", "SECURITY", "SEWER1", "SPACE",
    "STRIKE", "TRAIN", "WARE1", "WARE2", "WASTE1", "WASTE2", "WASTE3",
    "WASTE4", NULL
};

/* The client's hook, run here over COMMON's script instead of live play. */
typedef struct live_rot_ctx {
    const q2_userfuncs *uf;
    q2_rotator_set     *set;
    u32                 steps;
    u32                 rot_fired;   /* rotation CALLs the script actually ran */
    u32                 rot_barren;  /* ...of those, ones that turned nothing  */

    /* Which item offsets turned something, so a call barren under THIS zone can
     * be told apart from one barren under every zone the map ships. */
    const u8           *chunk;
    u8                 *turned;      /* one byte per chunk offset */
    u8                 *seen;
} live_rot_ctx;

static void live_rot_call(void *user, const q2_event_item *item, u8 call_index)
{
    live_rot_ctx *ctx = (live_rot_ctx *)user;

    u32 hit = q2_rotators_call(ctx->set, ctx->uf, item, call_index);
    q2_uf_prim prim = q2_userfuncs_prim(ctx->uf, call_index);

    ctx->steps += hit;

    /*
     * A call that names no object is only a gap if the script ever RUNS it.
     * Count the rotation primitives the trigger sweep actually reaches, and how
     * many of those turned nothing, so "54 calls are empty" can be separated
     * from "54 calls are dead script".
     */
    if (prim == Q2_UF_SIMROT || prim == Q2_UF_SIMROT2 ||
        prim == Q2_UF_ROTHATCH || prim == Q2_UF_ROTBUTTON) {
        ctx->rot_fired++;
        if (!hit)
            ctx->rot_barren++;

        if (ctx->turned && ctx->chunk && item->payload) {
            size_t off = (size_t)(item->payload - ctx->chunk);
            ctx->seen[off] = 1;
            if (hit)
                ctx->turned[off] = 1;
        }
    }
}

/* Does `offset` name the start of a record in this script? */
static bool offset_is_record(const q2_events *ev, u32 offset)
{
    q2_event_record rec;

    if (!q2_events_first_record(ev, &rec))
        return false;

    do {
        if (rec.offset == offset)
            return true;
    } while (q2_events_next_record(ev, &rec, &rec));

    return false;
}

int cmd_zonescript(const disc *d, const char *only_map)
{
    int mi;
    u32 t_total = 0, t_common = 0, t_zone = 0, t_both = 0, t_neither = 0;
    u32 zone_dirs = 0, zone_records = 0, common_records = 0;
    u32 past_common = 0, past_zone = 0;
    u32 zone_same = 0, zone_diff = 0;
    u32 rot_prim_calls = 0, rot_too_short = 0, rot_no_object = 0,
        rot_usable = 0, rot_zone_rescue = 0, rot_zone_inrange = 0,
        rot_zone_slots = 0, rot_zone_nonneg = 0;
    u32 live_rot_fired = 0, live_rot_barren = 0;
    u32 rot_any_zone = 0, rot_no_zone = 0;
    u32 live_built = 0, live_calls = 0, live_steps = 0, live_moved = 0,
        live_turned = 0;
    bool verbose = (only_map != NULL);

    printf("Which Events chunk does a trigger volume fire?\n\n");

    for (mi = 0; g_maps[mi]; mi++) {
        char path[160];
        q2_buf cbuf;
        q2_common_file cf;
        q2_events cev;
        q2_triggers tg;
        q2_events zev[8];
        q2_buf zbuf[8];
        q2_zone_file zf[8];
        u32 zcount = 0, zi, k;
        bool have_common = false, have_trig = false;

        if (only_map && strcmp(only_map, g_maps[mi]) != 0)
            continue;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", g_maps[mi]);
        if (disc_read_file(d, path, &cbuf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &cbuf) != Q2_OK) {
            q2_buf_free(&cbuf);
            continue;
        }

        have_common = (q2_events_parse_common(&cev, &cf) == Q2_OK);
        have_trig   = (q2_triggers_parse(&tg, &cf) == Q2_OK);

        /* Every zone of this map, so an offset can be tried against each. */
        for (zi = 0; zi < 8; zi++) {
            snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/ZONE%u.DAT",
                     g_maps[mi], zi);
            if (disc_read_file(d, path, &zbuf[zcount]) != Q2_OK)
                continue;
            if (q2_zone_open(&zf[zcount], &zbuf[zcount]) != Q2_OK) {
                q2_buf_free(&zbuf[zcount]);
                continue;
            }
            if (q2_events_parse_zone(&zev[zcount], &zf[zcount]) != Q2_OK) {
                q2_zone_close(&zf[zcount]);
                continue;
            }
            zcount++;
        }

        if (have_common)
            common_records += cev.record_count;

        /*
         * The stronger test: an offset PAST THE END of a chunk cannot be an
         * offset into it, whatever records happen to start where. Record-start
         * membership proved nothing — every offset on the disc starts a record
         * in both scripts — but a chunk's size is not a coincidence.
         */
        if (have_common && have_trig) {
            u32 hi = 0;

            for (k = 0; k < tg.count; k++) {
                q2_trigger tr;

                if (!q2_trigger_get(&tg, k, &tr))
                    continue;
                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                    continue;
                if (tr.event_offset > hi)
                    hi = tr.event_offset;
            }

            if (hi >= cev.size)
                past_common++;
            for (zi = 0; zi < zcount; zi++)
                if (hi >= zev[zi].size) {
                    past_zone++;
                    break;
                }

            if (verbose)
                printf("  highest trigger offset %u; COMMON events %u bytes, "
                       "zone0 events %u bytes\n",
                       hi, cev.size, zcount ? zev[0].size : 0);
        }

        if (verbose)
            printf("%s: COMMON %u records, %u named; %u zones\n",
                   g_maps[mi], have_common ? cev.record_count : 0,
                   have_common ? cev.dir_count : 0, zcount);

        for (zi = 0; zi < zcount; zi++) {
            zone_records += zev[zi].record_count;
            zone_dirs    += zev[zi].dir_count;

            /*
             * Is a zone's script a COPY of COMMON's? BASE0's is the same 604
             * bytes, the same 23 records and the same two named entries, which
             * would explain why every offset resolves in both at once.
             */
            if (have_common) {
                if (zev[zi].size == cev.size && cev.size &&
                    memcmp(zev[zi].data, cev.data, cev.size) == 0)
                    zone_same++;
                else
                    zone_diff++;
            }

            if (verbose) {
                printf("  ZONE%u: %u records, %u named\n",
                       zi, zev[zi].record_count, zev[zi].dir_count);
                for (k = 0; k < zev[zi].dir_count && k < 24; k++) {
                    q2_event_dir_entry e;
                    if (q2_events_get_dir_entry(&zev[zi], k, &e))
                        printf("    %-13s +%u\n", e.name, e.offset);
                }
            }
        }

        if (verbose && have_common) {
            printf("  COMMON named:\n");
            for (k = 0; k < cev.dir_count && k < 24; k++) {
                q2_event_dir_entry e;
                if (q2_events_get_dir_entry(&cev, k, &e))
                    printf("    %-13s +%u\n", e.name, e.offset);
            }
        }

        /*
         * What the console actually runs: COMMON's script, fired by the
         * trigger volumes. Every volume with an event is fired once, which is
         * a player who has walked the whole map, and the rotators are built
         * from the same chunk the engine's global points at.
         */
        if (have_common && have_trig) {
            q2_userfuncs   uf;
            q2_rotator_set rs;
            q2_event_rt    rt;

            /*
             * Coverage, stated rather than left to be inferred: how many CALL
             * items in this map's script name a rotation primitive, against how
             * many rotators get built from them. A SIMROT names up to four
             * objects and builds one rotator each, so `built` is normally the
             * larger; what matters is that no rotation call is skipped.
             */
            {
                q2_event_record rec;

                if (q2_userfuncs_parse(&uf, &cf) == Q2_OK &&
                    q2_events_first_record(&cev, &rec)) {
                    do {
                        u32 it;

                        for (it = 0; it < rec.n_items; it++) {
                            q2_event_item item;
                            q2_uf_call call;

                            if (!q2_events_get_item(&cev, &rec, it, &item))
                                break;
                            if (!item.payload ||
                                (item.opcode & Q2_EVOP_MASK) != Q2_EVOP_CALL)
                                continue;
                            if (q2_uf_decode_call(&call, &uf, &item) != Q2_OK)
                                continue;

                            {
                                const u8 *pp = item.payload - 2;
                                u32 need = 0;
                                s16 first_obj = -1;

                                switch (call.prim) {
                                case Q2_UF_SIMROT:
                                case Q2_UF_SIMROT2:
                                    need = 24;
                                    /* ANY of the four, since a negative slot is
                                     * skipped rather than terminating the loop
                                     * (0x80028628 branches to the increment). */
                                    if (item.len >= need) {
                                        u32 sl;

                                        for (sl = 0; sl < 4; sl++) {
                                            s16 nd = q2_rd_s16(pp + 12 + 2 * (s32)sl);

                                            if (nd >= 0) {
                                                first_obj = nd;
                                                break;
                                            }
                                        }

                                        /*
                                         * If COMMON's copy has nothing, ask the
                                         * ZONE's copy at the same offset.
                                         *
                                         * 0x800285CC..0x800285F4 sets up TWO
                                         * cursors: s1 = item + 12 into the chunk
                                         * at gp+372, and s2 = that same offset
                                         * rebased into the chunk at gp+376. The
                                         * loop READS the slot from s2 and stamps
                                         * -1 into s1. So the buffer we parse need
                                         * not be the buffer the operand lives in.
                                         */
                                        if (first_obj < 0) {
                                            u32 off = (u32)(pp - cev.data);
                                            u32 zj;

                                            bool any_in_range = false;

                                            for (zj = 0; zj < zcount; zj++) {
                                                if (off + need > zev[zj].size)
                                                    continue;
                                                any_in_range = true;
                                                for (sl = 0; sl < 4; sl++) {
                                                    s16 q = q2_rd_s16(
                                                        zev[zj].data + off +
                                                        12 + 2 * (s32)sl);
                                                    rot_zone_slots++;
                                                    if (q >= 0)
                                                        rot_zone_nonneg++;
                                                }
                                            }
                                            if (any_in_range)
                                                rot_zone_inrange++;

                                            for (zj = 0; zj < zcount &&
                                                         first_obj < 0; zj++) {
                                                if (off + need > zev[zj].size)
                                                    continue;
                                                for (sl = 0; sl < 4; sl++) {
                                                    s16 nd = q2_rd_s16(
                                                        zev[zj].data + off +
                                                        12 + 2 * (s32)sl);
                                                    if (nd >= 0) {
                                                        first_obj = nd;
                                                        rot_zone_rescue++;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    break;
                                case Q2_UF_ROTHATCH:
                                    need = 20;
                                    if (item.len >= need)
                                        first_obj = q2_rd_s16(pp + 18);
                                    break;
                                case Q2_UF_ROTBUTTON:
                                    need = 12;
                                    if (item.len >= need)
                                        first_obj = q2_rd_s16(pp + 10);
                                    break;
                                default:
                                    break;
                                }

                                if (!need)
                                    continue;

                                rot_prim_calls++;
                                if (item.len < need)
                                    rot_too_short++;
                                else if (first_obj < 0)
                                    rot_no_object++;
                                else
                                    rot_usable++;
                            }
                        }
                    } while (q2_events_next_record(&cev, &rec, &rec));
                }
            }

            memset(&rs, 0, sizeof(rs));
            /*
             * Operands come from the ZONE's Events chunk, at the same offset —
             * `gp+376`, which the zone loader fills at 0x8007C234 after looking
             * "Events" up at 0x8007C14C. Set it before building so the build
             * reads the slots the engine reads. See #56.
             */
            /*
             * Per ZONE, not per map: the engine loads one zone at a time, so the
             * same COMMON script drives different geometry depending on which
             * zone is resident. Take the zone that yields the most rotators —
             * for a map whose zones are byte-identical to COMMON that is any of
             * them, and the count is unchanged.
             */
            /*
             * Sweep EVERY zone, not just the best one. A call that turns nothing
             * with zone 0 resident may turn something with zone 3, and only a
             * call barren under every zone the map ships is genuinely missing.
             */
            if (zcount && cev.size) {
                u8 *any = (u8 *)calloc(cev.size, 1);
                u8 *ran = (u8 *)calloc(cev.size, 1);
                u32 best = 0, bz = 0, zq;

                for (zq = 0; zq < zcount; zq++) {
                    q2_rotator_set probe;
                    q2_userfuncs puf;
                    q2_event_rt prt;

                    memset(&probe, 0, sizeof(probe));
                    q2_rotators_set_operand_source(&probe, cev.data,
                                                   zev[zq].data, zev[zq].size);
                    if (q2_userfuncs_parse(&puf, &cf) == Q2_OK &&
                        q2_rotators_build(&probe, &cev, &puf) == Q2_OK) {
                        if (probe.count > best) {
                            best = probe.count;
                            bz   = zq;
                        }
                        if (any && ran && have_trig &&
                            q2_event_rt_init(&prt, &cev) == Q2_OK) {
                            live_rot_ctx pc;

                            memset(&pc, 0, sizeof(pc));
                            pc.uf     = &puf;
                            pc.set    = &probe;
                            pc.chunk  = cev.data;
                            pc.turned = any;
                            pc.seen   = ran;
                            prt.on_call      = live_rot_call;
                            prt.on_call_user = &pc;

                            for (k = 0; k < tg.count; k++) {
                                q2_trigger tr;
                                if (!q2_trigger_get(&tg, k, &tr))
                                    continue;
                                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                                    continue;
                                q2_event_rt_trigger(&prt, tr.event_offset);
                            }
                            q2_event_rt_update(&prt);
                        }
                    }
                    q2_rotators_free(&probe);
                }

                if (any && ran) {
                    u32 o;
                    for (o = 0; o < cev.size; o++) {
                        if (!ran[o])
                            continue;
                        if (any[o]) {
                            rot_any_zone++;
                        } else {
                            u32 zq2;

                            rot_no_zone++;
                            printf("  %s: rotation CALL at Events+%u turns "
                                   "nothing under any of its %u zones\n",
                                   g_maps[mi], o, zcount);
                            printf("    COMMON (%u bytes) slots:", cev.size);
                            if (o >= 2 && o + 22 <= cev.size) {
                                const u8 *qq = cev.data + o - 2;
                                int sl2;
                                for (sl2 = 0; sl2 < 4; sl2++)
                                    printf(" %d",
                                           (int)q2_rd_s16(qq + 12 + 2 * sl2));
                            } else {
                                printf(" (offset past the end)");
                            }
                            printf("\n");
                            for (zq2 = 0; zq2 < zcount; zq2++) {
                                printf("    ZONE%u (%u bytes) slots:", zq2,
                                       zev[zq2].size);
                                if (o >= 2 && o + 22 <= zev[zq2].size) {
                                    const u8 *qq = zev[zq2].data + o - 2;
                                    int sl2;
                                    for (sl2 = 0; sl2 < 4; sl2++)
                                        printf(" %d",
                                               (int)q2_rd_s16(qq + 12 + 2 * sl2));
                                } else {
                                    printf(" (offset past the end)");
                                }
                                printf("\n");
                            }
                        }
                    }
                }
                free(any);
                free(ran);

                q2_rotators_set_operand_source(&rs, cev.data,
                                               zev[bz].data, zev[bz].size);
            }
            if (q2_userfuncs_parse(&uf, &cf) == Q2_OK &&
                q2_rotators_build(&rs, &cev, &uf) == Q2_OK &&
                q2_event_rt_init(&rt, &cev) == Q2_OK) {
                live_rot_ctx ctx;
                u32 t;

                ctx.uf = &uf;
                ctx.set = &rs;
                ctx.steps = 0;
                ctx.rot_fired = 0;
                ctx.rot_barren = 0;
                rt.on_call      = live_rot_call;
                rt.on_call_user = &ctx;

                live_built += rs.count;

                for (k = 0; k < tg.count; k++) {
                    q2_trigger tr;

                    if (!q2_trigger_get(&tg, k, &tr))
                        continue;
                    if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                        continue;
                    q2_event_rt_trigger(&rt, tr.event_offset);
                }
                q2_event_rt_update(&rt);

                live_calls += rt.call_count;
                live_steps += ctx.steps;
                live_rot_fired  += ctx.rot_fired;
                live_rot_barren += ctx.rot_barren;
                for (t = 0; t < 400; t++)
                    live_moved += q2_rotators_tick(&rs, 12);
                for (zi = 0; zi < rs.count; zi++)
                    if (rs.rotators[zi].angle != 0)
                        live_turned++;

                q2_event_rt_free(&rt);
            }
            q2_rotators_free(&rs);
        }

        if (have_common && have_trig) {
            for (k = 0; k < tg.count; k++) {
                q2_trigger tr;
                bool in_c, in_z = false;

                if (!q2_trigger_get(&tg, k, &tr))
                    continue;
                if (tr.event_offset == Q2_TRIGGER_NO_EVENT)
                    continue;

                t_total++;
                in_c = offset_is_record(&cev, tr.event_offset);
                for (zi = 0; zi < zcount; zi++)
                    if (offset_is_record(&zev[zi], tr.event_offset))
                        in_z = true;

                if (in_c && in_z) t_both++;
                else if (in_c)    t_common++;
                else if (in_z)    t_zone++;
                else              t_neither++;
            }
        }

        for (zi = 0; zi < zcount; zi++)
            q2_zone_close(&zf[zi]);
        q2_common_close(&cf);
    }

    printf("\n  COMMON records    : %u\n", common_records);
    printf("  zone records      : %u\n", zone_records);
    printf("  zone Events byte-identical to COMMON's: %u of %u\n",
           zone_same, zone_same + zone_diff);
    printf("  zone named entries: %u\n", zone_dirs);
    printf("\n  trigger volumes naming an event : %u\n", t_total);
    printf("    resolves in COMMON only       : %u\n", t_common);
    printf("    resolves in a ZONE only       : %u\n", t_zone);
    printf("    resolves in both              : %u  (says nothing either way)\n",
           t_both);
    printf("    resolves in neither           : %u\n", t_neither);

    printf("\n  maps whose highest trigger offset runs PAST the end of\n");
    printf("    COMMON's Events chunk         : %u\n", past_common);
    printf("    a ZONE's Events chunk         : %u\n", past_zone);

    printf("\n  COMMON's script, fired by every trigger volume — what the\n"
           "  console runs, since the zone loader never looks up \"Events\":\n");
    printf("    rotation CALLs  : %u  in COMMON's scripts, disc-wide\n",
           rot_prim_calls);
    printf("      too short     : %u  (the item cannot hold the operands)\n",
           rot_too_short);
    printf("      no object     : %u  (every slot the call has is -1)\n",
           rot_no_object);
    printf("      usable        : %u\n", rot_usable);
    printf("      empty in COMMON, a ZONE reaches that offset : %u\n",
           rot_zone_inrange);
    printf("      ...and the ZONE has a non-negative slot     : %u\n",
           rot_zone_rescue);
    printf("      zone slots examined %u, non-negative %u (%.1f%%)\n",
           rot_zone_slots, rot_zone_nonneg,
           rot_zone_slots ? 100.0 * rot_zone_nonneg / rot_zone_slots : 0.0);
    printf("    rotation CALLs the script RUNS : %u, of which turn nothing : %u\n",
           live_rot_fired, live_rot_barren);
    printf("    distinct rotation CALL sites the script reaches : %u\n",
           rot_any_zone + rot_no_zone);
    printf("      turn something under SOME zone : %u\n", rot_any_zone);
    printf("      barren under EVERY zone        : %u\n", rot_no_zone);
    printf("    rotators built  : %u  (one per object slot each call names)\n",
           live_built);
    printf("    CALL items run  : %u\n", live_calls);
    printf("    rotation steps  : %u\n", live_steps);
    printf("    tick-moves      : %u\n", live_moved);
    printf("    rotators turned : %u\n", live_turned);
        printf("\n  Record-start membership decides NOTHING: every offset"
               " starts a record in both.\n");

    return 0;
}
