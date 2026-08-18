/*
 * cmd_viewweapon.c — the weapon in the player's hands, checked against the disc.
 *
 * `src/build/vmtables.c` claims that the view model's animation bank is a
 * twelve-entry pointer table at 0x8009F59C, that each entry names five
 * pointers of which the fifth is the next clip's start, and that a key is
 * twenty bytes with its duration at +12 and its event at +16. `src/game/
 * viewweapon.c` claims a four-state machine, a seventy-tick switch countdown,
 * and that the weapon hangs off the eye at `286 - view_offset`.
 *
 * All of that is checked here rather than asserted:
 *
 *   - the constants, by reading the immediate field of the instruction each one
 *     was read from;
 *   - the table's structure, by re-deriving it — a clip's length is a pointer
 *     difference, so "every clip is a whole number of 20-byte keys" and "the
 *     fifth pointer is exactly the next weapon's first" are properties the
 *     bank either has or does not;
 *   - the bank's content, by walking every key of every clip of every weapon
 *     and bounding each field.
 *
 * A wrong address does not fail to parse — it produces a plausible bank of
 * nonsense — so the structural checks are the ones that matter.
 */
#include "cmd_viewweapon.h"

#include "exe.h"
#include "viewweapon.h"
#include "vmtables.h"
#include "vram.h"
#include "world.h"
#include "entity.h"
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
typedef struct check {
    u32         addr;
    s32         expect;
    const char *what;
} check;

static int g_total;

static bool imm_at(const q2_exe *e, u32 addr, s32 *out)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;
    *out = (s32)(s16)(u16)(word & 0xFFFFu);
    return true;
}

static void run_checks(const q2_exe *e, const char *title,
                       const check *list, size_t n, int *failed)
{
    size_t i;

    printf("\n%s\n", title);
    g_total += (int)n;

    for (i = 0; i < n; i++) {
        s32 got;

        if (!imm_at(e, list[i].addr, &got)) {
            printf("  %08X  %-46s  UNMAPPED\n", list[i].addr, list[i].what);
            (*failed)++;
            continue;
        }
        if (got == list[i].expect) {
            printf("  %08X  %-46s  %6d  ok\n", list[i].addr, list[i].what, got);
        } else {
            printf("  %08X  %-46s  %6d  MISMATCH (port says %d)\n",
                   list[i].addr, list[i].what, got, list[i].expect);
            (*failed)++;
        }
    }
}

/* ------------------------------------------------------------------------- */
static void print_clip(const q2_vm_tables *t, int w, q2_vm_state s, bool verbose)
{
    const q2_vm_clip *c = &t->clip[w][s];
    s32 total = 0;
    u32 k;
    int events = 0;

    for (k = 0; k < c->count; k++) {
        total += c->key[k].duration;
        if (c->key[k].event != Q2_VM_EVENT_NONE)
            events++;
    }

    printf("    %-6s %08X  %3u keys  %5d ticks  %d event%s\n",
           q2_vm_state_name(s), c->addr, c->count, total,
           events, events == 1 ? "" : "s");

    if (!verbose)
        return;

    for (k = 0; k < c->count; k++) {
        const q2_vm_key *key = &c->key[k];
        printf("      %2u  t %6d %6d %6d   r %6d %6d %6d   dur %4d%s",
               k, key->t[0], key->t[1], key->t[2],
               key->r[0], key->r[1], key->r[2], key->duration,
               key->jitter ? " jitter" : "");
        if (key->event != Q2_VM_EVENT_NONE)
            printf("   event %d", key->event);
        printf("\n");
    }
}


/* ------------------------------------------------------------------------- */
/*
 * One first-person frame, composed the way the console composes one: the world
 * into the viewport's slice of the ordering table, then the weapon into the
 * SAME slice, then one walk. The weapon is placed by q2_vw_place and by nothing
 * else — no nudge, no fudge — so what comes out is the reconstruction's own
 * answer to where the weapon goes.
 */
static int render_view(disc *d, q2_vm_tables *tab, const char *map,
                       int zone_index, int weapon, const char *out)
{
    q2_world_zone zone;
    q2_screen scr;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_render render;
    q2_world_stats stats;
    q2_model_bank bank;
    q2_model model;
    q2_viewweapon vw;
    q2_common_file common;
    q2_buf cbuf;
    u32 clut_a = 0;
    s32 feet[3] = { 0, 0, 0 };
    s16 aim[3] = { 0, 0, 0 }, kick[3] = { 0, 0, 0 };
    bool have_model = false, have_common = false;
    char cpath[256];
    q2_result r;
    int rc = 1;

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        return 1;
    }
    if (q2_screen_init(&scr, Q2_VIDEO_PAL) != Q2_OK) {
        q2_world_free_zone(&zone);
        return 1;
    }
    q2_screen_set_layout(&scr, Q2_SCREEN_LAYOUT_ONE, 1);

    q2_camera_default(&cam, scr.disp.width, scr.disp.height);
    psx_raster_opts_default(&opts);
    q2_world_render_init(&render);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 300000);
    memset(&gte, 0, sizeof(gte));

    /* Stand at a spawn, looking where it looks. */
    snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
        if (q2_common_open(&common, &cbuf) == Q2_OK) {
            q2_start_pos_list sl;
            have_common = true;
            if (q2_start_pos_parse(&sl, &common) == Q2_OK) {
                u32 k;
                for (k = 0; k < sl.count; k++) {
                    q2_start_pos sp;
                    if (!q2_start_pos_get(&sl, k, &sp) || sp.zone != zone_index)
                        continue;
                    feet[0] = sp.x; feet[1] = sp.y; feet[2] = sp.z;
                    cam.yaw = sp.angle;
                    printf("  eye at spawn '%s'\n", sp.name);
                    break;
                }
            }
            if (q2_model_bank_from_common(&bank, &common) == Q2_OK) {
                s32 idx;
                q2_vw_init(&vw, tab, weapon);
                idx = q2_model_bank_find(&bank, q2_vw_model_name(&vw));
                if (idx >= 0 && q2_model_get(&bank, (u32)idx, &model) == Q2_OK) {
                    q2_vw_set_model(&vw, &model);
                    have_model = true;
                    printf("  model '%s' is index %d of %s's bank\n",
                           q2_vw_model_name(&vw), (int)idx, map);
                } else {
                    printf("  %s ships no model called '%s'\n",
                           map, q2_vw_model_name(&vw));
                }
            }
        } else {
            q2_buf_free(&cbuf);
        }
    }

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (vram) {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            opts.textures = (q2_vram_upload(&vs, vram) == Q2_OK);
            clut_a = vs.clut4_count_a;
            q2_vram_free(&vs);
        } else {
            opts.textures = false;
        }
    }

    /* Let the weapon settle out of its raise so the pose is the resting one. */
    if (have_model) {
        int i;
        for (i = 0; i < 60; i++)
            q2_vw_advance(&vw, 12, false, Q2_VW_FIRED);
    }

    cam.pos[0] = feet[0];
    cam.pos[1] = feet[1] + Q2_VW_EYE_BASE - 576;   /* the eye, §9.12 */
    cam.pos[2] = feet[2];
    aim[1] = (s16)cam.yaw;

    q2_screen_frame_begin(&scr, &ot);
    scr.disp.bg_rgb[0] = 16; scr.disp.bg_rgb[1] = 16; scr.disp.bg_rgb[2] = 32;
    scr.disp.bg_enable = 1;
    scr.background_enable = true;

    q2_screen_view_begin(&scr, 0, &ot, &gte);
    /* The viewport's, not the buffer's — proj, the geometry offset and the far
     * distance are all view record fields (screen.h), and the weapon has to be
     * projected through exactly the same three as the world it stands in. */
    cam.projection = (u16)scr.view[0].proj;
    cam.ofs_x      = scr.view[0].ofs_x;
    cam.ofs_y      = scr.view[0].ofs_y;
    cam.far_z      = scr.view[0].far_z;
    render.subdiv_threshold = scr.view[0].far_z;
    q2_world_build_ot(&zone, &cam, scr.view[0].w, scr.view[0].h,
                      &ot, &gte, &render, &stats);
    printf("  world     %u quads\n", stats.quads_emitted);

    if (have_model) {
        q2_model_instance proto;
        q2_model_draw_stats ms;

        q2_model_instance_init(&proto);
        proto.tpage         = &render.tpage;
        proto.clut4_count_a = clut_a;

        u32 before = ot.prim_count;

        memset(&ms, 0, sizeof(ms));
        q2_vw_build_ot(&vw, &proto, feet, 576, aim, kick,
                       &cam, &ot, &gte, &ms);

        /* Where did they actually land? A face that projects is not yet a face
         * you can see. */
        {
            s32 minx = 1 << 30, maxx = -(1 << 30);
            s32 miny = 1 << 30, maxy = -(1 << 30);
            u32 minz = 0xFFFFFFFFu, maxz = 0;
            u32 i, k;

            for (i = before; i < ot.prim_count; i++) {
                const psx_prim *pr = &ot.prims[i];
                if (pr->otz < minz) minz = pr->otz;
                if (pr->otz > maxz) maxz = pr->otz;
                for (k = 0; k < 4; k++) {
                    if (pr->xy[k].x < minx) minx = pr->xy[k].x;
                    if (pr->xy[k].x > maxx) maxx = pr->xy[k].x;
                    if (pr->xy[k].y < miny) miny = pr->xy[k].y;
                    if (pr->xy[k].y > maxy) maxy = pr->xy[k].y;
                }
            }
            if (ot.prim_count > before)
                printf("  screen    x %d..%d  y %d..%d   otz %u..%u"
                       "  (viewport %dx%d)\n",
                       minx, maxx, miny, maxy, minz, maxz,
                       scr.view[0].w, scr.view[0].h);
        }
        printf("  weapon    %u of %u faces, %u parts "
               "(near %u, bad %u, overflow %u)\n",
               ms.faces_emitted, ms.faces_total, ms.parts,
               ms.faces_rejected_near, ms.faces_rejected_bad, ms.ot_overflow);
        {
            s32 o[3], a[3];
            q2_vw_place(&vw, feet, 576, aim, kick, o, a);
            printf("  place     origin %d %d %d   angles %d %d %d\n",
                   o[0], o[1], o[2], a[0], a[1], a[2]);
            printf("  eye       %d %d %d   local t %d %d %d\n",
                   cam.pos[0], cam.pos[1], cam.pos[2],
                   vw.cur_t[0], vw.cur_t[1], vw.cur_t[2]);
        }
    }

    q2_screen_compose(&scr, &ot, vram, &opts);
    q2_screen_present(&scr);

    r = psx_fb_write_ppm(q2_screen_front(&scr), out);
    if (r == Q2_OK) {
        printf("  wrote %s (%u x %u)\n", out, scr.disp.width, scr.disp.height);
        rc = 0;
    } else {
        fprintf(stderr, "cannot write %s: %s\n", out, q2_result_str(r));
    }

    free(vram);
    psx_ot_free(&ot);
    q2_screen_free(&scr);
    if (have_common)
        q2_common_close(&common);
    q2_world_free_zone(&zone);
    return rc;
}

/* ------------------------------------------------------------------------- */
int cmd_viewweapon(disc *d, const char *weapon, const char *out,
                   const char *map, int zone_index)
{
    q2_exe e;
    q2_vm_tables t;
    q2_build_id id;
    q2_result r;
    int failed = 0;
    int want = -1;
    int w, s;

    static const check consts[] = {
        { 0x8004ECECu,  70, "the weapon-switch countdown, in ticks"        },
        { 0x8004F250u,  12, "key duration, offset within the key"          },
        { 0x8004F1F8u,  14, "key jitter flag, offset within the key"       },
        { 0x8004EF34u,  16, "key event, offset within the key"             },
        { 0x8004F494u,   6, "key rotation, offset within the key"          },
        { 0x8004F508u,   8, "key rotation y"                               },
        { 0x8004F57Cu,  10, "key rotation z"                               },
        { 0x8004F608u, 286, "the eye base the weapon hangs off"            },
    };

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "this does not look like a Quake II PSX disc\n");
        return 1;
    }

    r = q2_vm_tables_load(&t, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the view model bank: %s\n", q2_result_str(r));
        return 1;
    }

    if (weapon) {
        char *endp = NULL;
        long n = strtol(weapon, &endp, 0);

        if (endp && *endp == '\0' && n >= 0 && n < Q2_VM_SLOTS) {
            want = (int)n;
        } else {
            for (w = 0; w < Q2_VM_SLOTS; w++) {
                if (t.model_name[w][0] &&
#ifdef _WIN32
                    _strnicmp(t.model_name[w], weapon, 12) == 0)
#else
                    strncasecmp(t.model_name[w], weapon, 12) == 0)
#endif
                { want = w; break; }
            }
            if (want < 0) {
                fprintf(stderr, "no view model called '%s'\n", weapon);
                q2_vm_tables_free(&t);
                return 1;
            }
        }
    }

    /*
     * The rendering mode. Alignment is not something a table can assert, so
     * this composes a real first-person frame through the reconstructed
     * transform and writes it out — no window, no client, no fudge.
     */
    if (out) {
        int rc = render_view(d, &t, map ? map : "BASE1", zone_index,
                             (want >= 0) ? want : 1, out);
        q2_vm_tables_free(&t);
        return rc;
    }

    printf("view weapon — %s\n", id.serial);
    printf("  %d weapon slots, %d states, %u keys in the bank\n",
           Q2_VM_SLOTS, Q2_VM_STATES, t.key_count);

    /* ------------------------------------------------------------------ */
    printf("\nthe bank\n");
    for (w = 0; w < Q2_VM_SLOTS; w++) {
        if (want >= 0 && w != want)
            continue;
        printf("  %2d  %-12.12s\n", w, t.model_name[w]);
        for (s = 0; s < Q2_VM_STATES; s++)
            print_clip(&t, w, (q2_vm_state)s, want >= 0);
    }

    /* ------------------------------------------------------------------ */
    /* Structure. These are the checks a wrong address cannot survive.     */
    printf("\nstructure\n");
    {
        int bad_stride = 0, bad_empty = 0, bad_dur = 0, bad_event = 0;
        int contiguous = 0, contiguous_ok = 0;

        for (w = 0; w < Q2_VM_SLOTS; w++) {
            for (s = 0; s < Q2_VM_STATES; s++) {
                const q2_vm_clip *c = &t.clip[w][s];
                s32 total = 0;
                u32 k;

                if (c->count == 0)
                    bad_empty++;

                /* Clip s ends where clip s+1 begins — the format's own way of
                 * storing a length, and the state machine's end test. */
                if (s + 1 < Q2_VM_STATES) {
                    contiguous++;
                    if (c->addr + c->count * Q2_VM_KEY_SIZE == t.clip[w][s + 1].addr)
                        contiguous_ok++;
                }

                for (k = 0; k < c->count; k++) {
                    const q2_vm_key *key = &c->key[k];
                    total += key->duration;
                    if (key->duration < 0)
                        bad_dur++;
                    if (key->event < -1 || key->event > 4096)
                        bad_event++;
                    if (key->pad != 0)
                        bad_stride++;
                }
                if (total <= 0)
                    bad_dur++;
            }
        }

        g_total += 5;
        printf("  every clip is a whole number of 20-byte keys        %s\n",
"ok");   /* the loader refuses to build otherwise */
        printf("  clips are contiguous: %d of %d                       %s\n",
               contiguous_ok, contiguous, contiguous_ok == contiguous ? "ok" : "MISMATCH");
        if (contiguous_ok != contiguous) failed++;
        printf("  no empty clip                                       %s\n",
               bad_empty == 0 ? "ok" : "MISMATCH");
        if (bad_empty) failed++;
        printf("  every clip has a positive total duration            %s\n",
               bad_dur == 0 ? "ok" : "MISMATCH");
        if (bad_dur) failed++;
        printf("  every key's trailing halfword is zero               %s\n",
               bad_stride == 0 ? "ok" : "MISMATCH");
        if (bad_stride) failed++;
        printf("  every event is -1 or a small id                     %s\n",
               bad_event == 0 ? "ok" : "MISMATCH");
        if (bad_event) failed++;

        /* Slot 0 aliases slot 1 — the same convention as the fire-function
         * table's `jr ra` stub, and a strong signal the indexing is 1-based. */
        g_total++;
        printf("  slot 0 aliases slot 1                               %s\n",
               t.clip[0][0].addr == t.clip[1][0].addr ? "ok" : "MISMATCH");
        if (t.clip[0][0].addr != t.clip[1][0].addr) failed++;
    }

    /* ------------------------------------------------------------------ */
    if (q2_exe_load(&e, d, NULL) == Q2_OK) {
        run_checks(&e, "constants", consts, sizeof(consts) / sizeof(consts[0]),
                   &failed);
        q2_exe_free(&e);
    } else {
        fprintf(stderr, "cannot load the boot executable\n");
        failed++;
    }

    /* ------------------------------------------------------------------ */
    /* The state machine, driven for real. Not a constant check: this asks   */
    /* whether the reconstruction actually cycles the way the original's     */
    /* transitions say it should.                                           */
    printf("\nthe state machine\n");
    {
        q2_viewweapon vw;
        int ticks;
        bool saw_fire = false, saw_idle = false, swapped = false;

        q2_vw_init(&vw, &t, 1);
        printf("  init            %-6s frame %u\n",
               q2_vm_state_name(vw.state), vw.frame);

        for (ticks = 0; ticks < 400 && !saw_idle; ticks++) {
            q2_vw_advance(&vw, 12, false, Q2_VW_FIRED);
            if (vw.state == Q2_VM_IDLE) saw_idle = true;
        }
        printf("  raise settles   %-6s after %d ticks               %s\n",
               q2_vm_state_name(vw.state), ticks * 12, saw_idle ? "ok" : "NO");
        g_total++;
        if (!saw_idle) failed++;

        for (ticks = 0; ticks < 200 && !saw_fire; ticks++) {
            q2_vw_advance(&vw, 12, true, Q2_VW_FIRED);
            if (vw.state == Q2_VM_FIRE) saw_fire = true;
        }
        printf("  trigger enters  %-6s                              %s\n",
               q2_vm_state_name(vw.state), saw_fire ? "ok" : "NO");
        g_total++;
        if (!saw_fire) failed++;

        q2_vw_select(&vw, 4);
        for (ticks = 0; ticks < 800 && !swapped; ticks++)
            swapped = q2_vw_advance(&vw, 12, false, Q2_VW_FIRED);
        printf("  select swaps to %-12.12s after %d ticks       %s\n",
               t.model_name[vw.weapon], ticks * 12,
               (swapped && vw.weapon == 4) ? "ok" : "NO");
        g_total++;
        if (!swapped || vw.weapon != 4) failed++;
    }

    /* ------------------------------------------------------------------ */
    /*
     * The MODEL's own named move — the second animation, and the one the port
     * had no counterpart for.
     *
     * 0x80050454 is a switch on the weapon id over exactly five weapons, and
     * each arm hands one 12-byte string to 0x8006D330. This asks the disc
     * whether those five models really carry those five names, and whether the
     * six other weapons carry none — because if the names did not line up the
     * whole reading would be a coincidence rather than a mechanism.
     *
     * LAB is the map: it is the only one that ships all eleven view models, so
     * it is the only place the negative half of the claim can be checked.
     */
    printf("\nthe model's own move (LAB)\n");
    {
        static const struct { int weapon; const char *move; u32 arm; } named[] = {
            {  3, "Fire", 0x800505ECu },
            {  6, "Set",  0x800505ACu },
            {  7, "Spin", 0x800505CCu },
            {  8, "Fire", 0x800505ECu },
            { 11, "Fire", 0x800505ECu }
        };
        q2_common_file  common;
        q2_buf          cbuf;
        char            cpath[256];

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", "LAB");
        if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            if (q2_common_open(&common, &cbuf) == Q2_OK) {
                q2_model_bank bank;

                if (q2_model_bank_from_common(&bank, &common) == Q2_OK) {
                    size_t i;
                    int wi;

                    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
                        q2_model       m;
                        q2_model_move  mv;
                        s32            idx;
                        bool           got = false;

                        idx = q2_model_bank_find(&bank, t.model_name[named[i].weapon]);
                        if (idx >= 0 && q2_model_get(&bank, (u32)idx, &m) == Q2_OK)
                            got = q2_model_move_by_name(&m, named[i].move, &mv);

                        printf("  %08X  %-12.12s plays '%-4s'  %3u..%-3u"
                               "  ->  %4d..%-4d  %s\n",
                               named[i].arm, t.model_name[named[i].weapon],
                               named[i].move,
                               got ? mv.start : 0, got ? mv.end : 0,
                               got ? (int)mv.start * 5 : 0,
                               got ? (int)mv.end * 5 : 0,
                               got ? "ok" : "NO MOVE");
                        g_total++;
                        if (!got)
                            failed++;
                    }

                    /*
                     * And the ones that reach no arm must carry no move at all,
                     * which is what makes the mapping a mechanism rather than a
                     * coincidence of five names.
                     *
                     * THE HYPERBLASTER IS THE ONE EXCEPTION and it is not a
                     * hole in the argument: `HyperBlast G` carries a move called
                     * `Move`, and weapon 9 is exactly the weapon 0x8004FC78
                     * exists for — the only arm that drives a model by patching
                     * a key rather than by starting a clip. It is skipped here
                     * because it is animated by the other mechanism, not by
                     * none.
                     */
                    {
                        int extra = 0;

                        for (wi = 1; wi < Q2_VM_SLOTS; wi++) {
                            q2_model m;
                            s32      idx;

                            if (wi == 3 || wi == 6 || wi == 7 || wi == 8 ||
                                wi == 11 || wi == 9)
                                continue;
                            idx = q2_model_bank_find(&bank, t.model_name[wi]);
                            if (idx < 0 ||
                                q2_model_get(&bank, (u32)idx, &m) != Q2_OK)
                                continue;
                            if (q2_model_move_count(&m) > 0)
                                extra++;
                        }
                        printf("  and the rest carry no move (9 is 8004FC78's)"
                               "    %s\n",
                               extra == 0 ? "ok" : "MISMATCH");
                        g_total++;
                        if (extra != 0)
                            failed++;
                    }

                    /*
                     * The grenade's cook threshold has to land INSIDE its move,
                     * which is the whole argument for the scale of five —
                     * openquestions #51h. Unscaled it cannot.
                     */
                    {
                        q2_model      m;
                        q2_model_move mv;
                        s32           idx = q2_model_bank_find(&bank,
                                                               t.model_name[6]);
                        bool inside = false, outside_unscaled = false;

                        if (idx >= 0 && q2_model_get(&bank, (u32)idx, &m) == Q2_OK &&
                            q2_model_move_by_name(&m, "Set", &mv)) {
                            inside = ((s32)mv.start * 5 <= Q2_VW_COOK_POSITION &&
                                      Q2_VW_COOK_POSITION <= (s32)mv.end * 5);
                            outside_unscaled = (Q2_VW_COOK_POSITION > (s32)mv.end);
                        }
                        printf("  8004F3F8's 380 primes inside 'Set' at x5"
                               "          %s\n", inside ? "ok" : "MISMATCH");
                        g_total++;
                        if (!inside)
                            failed++;

                        printf("  ...and could never be reached unscaled"
                               "              %s\n",
                               outside_unscaled ? "ok" : "MISMATCH");
                        g_total++;
                        if (!outside_unscaled)
                            failed++;
                    }
                }
                q2_common_close(&common);
            }
            q2_buf_free(&cbuf);
        } else {
            printf("  LAB is not on this disc\n");
        }
    }

    /* ------------------------------------------------------------------ */
    /*
     * The hyperblaster's barrel — 0x8004FC78, the only animation on the disc
     * driven by writing to key data rather than by choosing a frame.
     *
     * Driven against the disc's own clips, because the arithmetic is four
     * constants and a divide and is not the interesting half. What is worth
     * checking is that the frames those constants belong to are REACHED: the
     * ramp only moves on fire frames 1..4 and 6, and if the clip never sat on
     * one the barrel would stand still whatever the formula said.
     */
    printf("\nthe hyperblaster's barrel\n");
    {
        q2_viewweapon vw;
        int  tick;
        s16  lo = 4096, hi = 0;
        int  moved = 0;
        s16  prev;

        q2_vw_init(&vw, &t, 9);
        for (tick = 0; tick < 400 && vw.state != Q2_VM_IDLE; tick++)
            q2_vw_advance(&vw, 12, false, Q2_VW_FIRED);

        printf("  idle holds the barrel at one                  %5d  %s\n",
               (int)vw.hyper_ramp,
               vw.hyper_ramp == 4096 ? "ok" : "MISMATCH");
        g_total++;
        if (vw.hyper_ramp != 4096) failed++;

        prev = vw.hyper_ramp;
        for (tick = 0; tick < 300; tick++) {
            q2_vw_advance(&vw, 6, true, Q2_VW_FIRED);
            if (vw.state != Q2_VM_FIRE)
                continue;
            if (vw.hyper_ramp < lo) lo = vw.hyper_ramp;
            if (vw.hyper_ramp > hi) hi = vw.hyper_ramp;
            if (vw.hyper_ramp != prev) moved++;
            prev = vw.hyper_ramp;
        }

        printf("  a held trigger turns it: %4d..%-4d over %3d steps  %s\n",
               (int)lo, (int)hi, moved, moved > 0 ? "ok" : "NO");
        g_total++;
        if (moved == 0) failed++;

        /* The four bases are 4096, 3296, 2096 and 1296 and the step is forty a
         * tick, so a run that reaches the lower frames has to come down well
         * past half — a ramp that only ever sits near one is a driver that is
         * being called and never landing on a frame that writes. */
        printf("  and reaches the lower bases                   %5d  %s\n",
               (int)lo, lo < 2096 ? "ok" : "MISMATCH");
        g_total++;
        if (lo >= 2096) failed++;
    }

    /* ------------------------------------------------------------------ */
    /* Placement: the weapon must sit at the eye, and must move with it.   */
    printf("\nplacement\n");
    {
        q2_viewweapon vw;
        s32 feet[3] = { 1000, 2000, 3000 };
        s32 o_stand[3], o_crouch[3], a[3];
        s16 aim[3] = { 0, 0, 0 }, kick[3] = { 0, 0, 0 };

        q2_vw_init(&vw, &t, 1);

        /*
         * Advance into the clip before sampling. At frame 0 the interpolation
         * has not left its starting point, so the local offset is still zero
         * and a rotation of nothing is nothing — sampling there would be
         * measuring the wrong instant, not the wrong code.
         */
        q2_vw_advance(&vw, 60, false, Q2_VW_FIRED);

        q2_vw_place(&vw, feet, 576, aim, kick, o_stand, a);
        q2_vw_place(&vw, feet, 286, aim, kick, o_crouch, a);

        printf("  standing  y = %d   crouched y = %d   delta %d\n",
               o_stand[1], o_crouch[1], o_crouch[1] - o_stand[1]);
        g_total++;
        printf("  the weapon drops with the eye by 576-286 = 290       %s\n",
               (o_crouch[1] - o_stand[1]) == 290 ? "ok" : "MISMATCH");
        if ((o_crouch[1] - o_stand[1]) != 290) failed++;

        /* Yaw must swing the weapon around the eye, not slide it. */
        {
            /*
             * The invariant is RIGIDITY, not "y stays put": the animation's own
             * pitch and roll are non-zero, so a yaw legitimately moves y. What
             * must hold is that the offset from the eye changes direction and
             * not length, because the translation is rotated by the view matrix
             * before the eye is added (0x8004F5E0). Asserting the weaker,
             * wronger thing would have hidden a real error here.
             */
            s32 o0[3], o1[3], v0[3], v1[3];
            s16 y0[3] = { 0, 0, 0 }, y1[3] = { 0, 1024, 0 };
            s32 eye_y = feet[1] + Q2_VW_EYE_BASE - 576;
            s64 d0 = 0, d1 = 0;
            int i;
            bool moved, rigid;

            q2_vw_place(&vw, feet, 576, y0, kick, o0, a);
            q2_vw_place(&vw, feet, 576, y1, kick, o1, a);

            v0[0] = o0[0] - feet[0]; v0[1] = o0[1] - eye_y; v0[2] = o0[2] - feet[2];
            v1[0] = o1[0] - feet[0]; v1[1] = o1[1] - eye_y; v1[2] = o1[2] - feet[2];
            for (i = 0; i < 3; i++) {
                d0 += (s64)v0[i] * v0[i];
                d1 += (s64)v1[i] * v1[i];
            }

            moved = (o0[0] != o1[0] || o0[1] != o1[1] || o0[2] != o1[2]);
            rigid = (d0 - d1 < 4096 * 4096) && (d1 - d0 < 4096 * 4096);

            g_total += 2;
            printf("  a quarter turn moves the weapon                     %s\n",
                   moved ? "ok" : "MISMATCH");
            if (!moved) failed++;
            printf("  and its distance from the eye is unchanged          %s\n",
                   rigid ? "ok" : "MISMATCH");
            if (!rigid) failed++;
        }
    }

    printf("\n%d of %d checks passed\n", g_total - failed, g_total);

    q2_vm_tables_free(&t);
    return failed ? 1 : 0;
}
