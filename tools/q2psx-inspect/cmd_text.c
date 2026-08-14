#include "cmd_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "briefing.h"
#include "hud.h"
#include "hudtables.h"
#include "hudtables.h"
#include "ident.h"
#include "level.h"
#include "leveltext.h"
#include "menufont.h"
#include "panel.h"
#include "prompt.h"
#include "raster.h"
#include "vram.h"

/*
 * The `Strings` chunk, and the briefing screen it feeds.
 *
 * FORMATS.md listed `Strings` as catalogued-but-undecoded for a long time, and
 * openquestions.md #43 listed the briefing screen as "not located". They are
 * the same finding: the chunk is a name-to-text dictionary, and three of its
 * keys are the briefing's three fields.
 *
 * This command decodes every map's chunk, checks the invariants that would
 * break first if the record layout were wrong, and prints the briefing each map
 * would show — which is the check that matters, because a plausible-looking
 * misread produces a dictionary of the right size full of the wrong text.
 */

/* Map directories are upper case on the disc; a caller types what they like. */
static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

static const char *dir_of(const char *path, char *out, size_t n)
{
    const char *end = strrchr(path, '/');
    const char *start;

    if (!end)
        return path;
    start = end - 1;
    while (start > path && start[-1] != '/')
        start--;

    if ((size_t)(end - start) >= n)
        return path;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return out;
}


/*
 * Draw the briefing, because a layout is only checkable as a picture. A wrong
 * margin or a wrong box still produces a screenful of legible text; it is the
 * panel's border landing where the words are, or the objective running off the
 * right edge instead of wrapping, that gives it away.
 */
static int shoot_briefing(const disc *d, const q2_briefing *b, const char *map,
                          const char *out)
{
    const int W = 512, H = 248;
    q2_build_id id;
    q2_hud_tables tab;
    q2_vram_section vs;
    q2_menu_font mfont;
    q2_hud_font hfont;
    q2_hud_ctx ctx;
    q2_hud_pen pen;
    q2_prompt_bar bar;
    psx_framebuffer fb;
    psx_raster_opts ropts;
    psx_vram *vram;
    psx_ot ot;
    u32 prims;
    int i;

    if (q2_identify(d, &id) != Q2_OK ||
        q2_hud_tables_load(&tab, d, &id) != Q2_OK)
        return 1;
    if (q2_vram_load(&vs, d, map) != Q2_OK) {
        q2_hud_tables_free(&tab);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_vram_free(&vs);
        q2_hud_tables_free(&tab);
        return 1;
    }

    if (q2_menu_font_upload(&mfont, &tab, &vs, vram, false, 1) != Q2_OK ||
        q2_hud_font_upload(&hfont, &tab, &vs, vram) != Q2_OK) {
        fprintf(stderr, "%s carries neither atlas\n", map);
        q2_vram_free(&vs);
        free(vram);
        q2_hud_tables_free(&tab);
        return 1;
    }
    q2_vram_free(&vs);

    if (psx_ot_init(&ot, 256, 8192) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(vram);
        q2_hud_tables_free(&tab);
        return 1;
    }

    /* Something behind it, so the panel's quarter-strength body is visible as
     * a darkening rather than as a flat black rectangle. */
    for (i = 0; i < H; i++) {
        int k;
        for (k = 0; k < W; k++)
            fb.px[(size_t)i * W + k] =
                psx_rgb555((u8)(8 + k / 12), (u8)(10 + i / 8), (u8)(40 - i / 10));
    }

    q2_hud_ctx_default(&ctx, W, H);
    q2_hud_pen_default(&pen);

    /* Buckets: body furthest, then frame, then text — the console's own order
     * (panel.h), expressed as three indices into a flat table. */
    prims = q2_briefing_build_ot(b, &hfont, &mfont, &ctx, &pen, &ot, 10, 11, 12);

    /* The BACK prompt, settled where a screen would have asked for it. */
    q2_prompt_init(&bar);
    q2_prompt_show(&bar, Q2_PROMPT_BACK, 216);
    for (i = 0; i < 64; i++)
        q2_prompt_step(&bar);
    prims += q2_prompt_build_ot(&bar, &mfont, &ot, 12);

    psx_raster_opts_default(&ropts);
    psx_raster_ot(&fb, &ot, vram, &ropts);

    if (psx_fb_write_ppm(&fb, out) == Q2_OK)
        printf("\n  wrote %s (%dx%d), %u primitives\n", out, W, H, prims);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    q2_hud_tables_free(&tab);
    return 0;
}

int cmd_text(const disc *d, const char *want_map, const char *out_path)
{
    u32 n, f, maps = 0, with_chunk = 0, entries = 0, briefings = 0;
    u32 key_hist_title = 0, key_hist_orders = 0, key_hist_obj = 0;
    int bad = 0;

    printf("The `Strings` chunk — a map's text, addressed by name\n");
    printf("Records are {char name[12]; u32 offset}, ending at an all-zero"
           " record; the\n"
           "text follows, NUL-terminated. The name field is NOT terminated when"
           " it is\n"
           "full — `FoundASecret` is exactly twelve characters.\n\n");

    n = disc_file_count(d);
    for (f = 0; f < n; f++) {
        const disc_file *file = disc_file_at(d, f);
        const char *base = strrchr(file->path, '/');
        char dirbuf[64];
        const char *dir;
        q2_buf buf;
        q2_common_file cf;
        q2_leveltext tx;
        q2_result r;

        base = base ? base + 1 : file->path;
        if (strcmp(base, "COMMON.DAT") != 0)
            continue;

        dir = dir_of(file->path, dirbuf, sizeof(dirbuf));
        maps++;
        if (want_map && !same_name(dir, want_map))
            continue;

        if (disc_read_file(d, file->path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        r = q2_leveltext_open(&tx, &cf);
        if (r != Q2_OK) {
            printf("  %-10s chunk did not decode (%d)\n", dir, (int)r);
            bad++;
        } else {
            const char *title = q2_leveltext_find(&tx, "MapTitle");
            char key[Q2_LEVELTEXT_NAME_LEN + 1];
            const char *orders = NULL, *objective = NULL;
            int unit, step;

            with_chunk++;
            entries += tx.count;

            /* The briefing's keys are built at run time from the unit number.
             * Sweep the plausible range rather than guessing which unit a map
             * belongs to — the mapping is the level table's business. */
            for (unit = 1; unit <= 9 && !objective; unit++) {
                q2_leveltext_key_objective(key, unit);
                objective = q2_leveltext_find(&tx, key);
            }
            for (unit = 1; unit <= 9 && !orders; unit++)
                for (step = 0; step <= 15 && !orders; step++) {
                    q2_leveltext_key_orders(key, unit, step);
                    orders = q2_leveltext_find(&tx, key);
                }

            if (title)     key_hist_title++;
            if (orders)    key_hist_orders++;
            if (objective) key_hist_obj++;

            if (want_map) {
                u32 i;
                printf("  %s — %u entries\n", dir, tx.count);
                for (i = 0; i < tx.count; i++)
                    printf("    %-13s +0x%04X  \"%s\"\n", tx.entry[i].name,
                           tx.entry[i].offset, tx.entry[i].text);
            } else {
                printf("  %-10s %2u entries  MapTitle=%s\n", dir, tx.count,
                       title ? title : "(none)");
            }

            /* The briefing this map would show. */
            if (title || orders || objective) {
                q2_briefing b;
                char text[512];

                briefings++;
                q2_briefing_init(&b);
                q2_briefing_set_location(&b, title ? title : "");
                if (orders)    q2_briefing_set_orders(&b, orders);
                if (objective) q2_briefing_set_objective(&b, objective);

                if (want_map) {
                    printf("\n  the briefing screen (0x800215A0), box"
                           " (%d, %d, %d, %d):\n",
                           b.box.x, b.box.y, b.box.w, b.box.h);
                    printf("    Location:          %s\n", b.location);
                    printf("    Current Orders:    %s\n", b.orders);
                    printf("    Mission Objective: %s\n", b.objective);
                    if (q2_briefing_compose(&b, text, sizeof(text)))
                        printf("\n  as markup:\n    %s\n", text);
                    if (out_path)
                        shoot_briefing(d, &b, dir, out_path);
                }
            }
        }

        q2_common_close(&cf);
        q2_buf_free(&buf);
    }

    printf("\n  %u COMMON.DAT, %u decoded, %u entries in total\n",
           maps, with_chunk, entries);
    printf("  MapTitle on %u, Unit*Curr* on %u, Unit*Miss1 on %u;"
           " %u maps brief\n",
           key_hist_title, key_hist_orders, key_hist_obj, briefings);

    /* ------------------------------------------------------------------ */
    /* The two screens' chrome, which is data rather than text.           */
    /* ------------------------------------------------------------------ */
    printf("\nThe panel frame (0x8003E8D0) — frontend.lbm, eight quads\n");
    printf("  corner    (%3d,%3d) %2dx%-2d  drawn four times, mirrored by"
           " corner order\n",
           Q2_PANEL_CORNER_U, Q2_PANEL_CORNER_V,
           Q2_PANEL_CORNER_W, Q2_PANEL_CORNER_H);
    printf("  h edge    (%3d,%3d) %2dx%-2d  stretched along the top and bottom\n",
           Q2_PANEL_EDGE_H_U, Q2_PANEL_EDGE_H_V,
           Q2_PANEL_EDGE_H_W, Q2_PANEL_EDGE_H_H);
    printf("  v edge    (%3d,%3d) %2dx%-2d  stretched down both sides\n",
           Q2_PANEL_EDGE_V_U, Q2_PANEL_EDGE_V_V,
           Q2_PANEL_EDGE_V_W, Q2_PANEL_EDGE_V_H);
    printf("  body      two black TILEs at 50%%, so a quarter of the scene"
           " shows through\n");
    printf("  overhang  %d left and right, %d top and bottom\n",
           Q2_PANEL_OVERHANG_X, Q2_PANEL_OVERHANG_Y);

    printf("\nThe button prompts (0x8009B4D8) — art, not text\n");
    {
        static const char *what[Q2_PROMPT_COUNT] = {
            "X SELECT", "triangle BACK", "square RULES"
        };
        q2_prompt_bar bar;
        int i;

        for (i = 0; i < Q2_PROMPT_COUNT; i++) {
            const q2_prompt_rec *r = &q2_prompt_table[i];
            printf("  %d  uv=(%3u,%3u) %ux%-2u  x=%-3d y=%-3d centre=%-3u  %s\n",
                   i, r->u, r->v, r->w, r->h, r->x, r->y, r->centre, what[i]);
            if (r->x + Q2_PROMPT_W / 2 != (int)r->centre) {
                printf("     centre does not agree with x + w/2\n");
                bad++;
            }
        }
        printf("  they slide %d pixels a frame; a page open parks them at"
               " y = %d\n", Q2_PROMPT_SPEED, Q2_PROMPT_Y_HIDDEN);

        /* The animation has to converge, and it has to converge on the target
         * exactly rather than oscillating around it. */
        q2_prompt_init(&bar);
        q2_prompt_show(&bar, Q2_PROMPT_BACK, 208);
        for (i = 0; i < 64 && bar.rec[Q2_PROMPT_BACK].y !=
                              bar.rec[Q2_PROMPT_BACK].y_target; i++)
            q2_prompt_step(&bar);
        if (bar.rec[Q2_PROMPT_BACK].y != bar.rec[Q2_PROMPT_BACK].y_target) {
            printf("  the slide did not settle\n");
            bad++;
        } else {
            printf("  a prompt asked to 208 settles at %d after %d frames\n",
                   bar.rec[Q2_PROMPT_BACK].y, i);
        }
    }

    printf("\n%s\n", bad == 0
           ? "PASS - every chunk decoded and every invariant held."
           : "FAIL - see above.");
    return bad == 0 ? 0 : 1;
}
