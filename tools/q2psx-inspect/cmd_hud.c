#include "cmd_hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud.h"
#include "hudtables.h"
#include "ident.h"
#include "raster.h"
#include "vram.h"

/* The atlas rows the tables address, named by what is drawn on them. Used only
 * to make the dump readable. */
static const char *atlas_row(u8 v)
{
    switch (v) {
    case 0x80: return "lower case";
    case 0x98: return "words: Player/HyperBlaster/SuperShotgun";
    case 0xA0: return "words: Machine/Chain/Grenade/Launcher/Rail";
    case 0xA8: return "words: Rocket/BFG/was/Ammo/die/door/Sequence";
    case 0xB0: return "digits and punctuation";
    case 0xB8: return "'A'..'P'";
    case 0xC0: return "'Q'..'Z' and punctuation";
    case 0xF0: return "message backdrop tiles";
    case 0x00: return "blank";
    default:   return "";
    }
}

static void dump_tables(const q2_hud_tables *t)
{
    int i;
    unsigned live = 0;

    printf("Glyphs (0x%08X, %d entries, 8x8 sprites, index = c - 32)\n",
           Q2_HUD_ADDR_GLYPH_UV, Q2_HUD_GLYPH_COUNT);
    {
        u8 seen[256];
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < Q2_HUD_GLYPH_COUNT; i++)
            seen[t->glyph[i].v] = 1;
        for (i = 0; i < 256; i++)
            if (seen[i])
                printf("  v=0x%02X  %s\n", i, atlas_row((u8)i));
    }
    printf("  '0'..'9' at u=0x%02X..0x%02X v=0x%02X, 'A' at (0x%02X,0x%02X),"
           " 'a' at (0x%02X,0x%02X)\n",
           t->glyph['0' - 32].u, t->glyph['9' - 32].u, t->glyph['0' - 32].v,
           t->glyph['A' - 32].u, t->glyph['A' - 32].v,
           t->glyph['a' - 32].u, t->glyph['a' - 32].v);

    printf("\nIcons (0x%08X, %d entries) — pre-rendered words, deliberately"
           " overlapping\n", Q2_HUD_ADDR_ICON, Q2_HUD_ICON_COUNT);
    for (i = 0; i < Q2_HUD_ICON_COUNT; i++)
        printf("  %2d  u=0x%02X..0x%02X  v=0x%02X  %2dx%-2d\n", i,
               t->icon[i].u, (u8)(t->icon[i].u + t->icon[i].w), t->icon[i].v,
               t->icon[i].w, t->icon[i].h);

    printf("\nIcon escapes (jump table 0x800ACA1C, 'A'..'W')\n  ");
    for (i = 'A'; i <= 'W'; i++) {
        u8 ic[Q2_HUD_ICON_ESCAPE_MAX];
        int n = q2_hud_icon_escape((char)i, ic);
        if (n == 0)
            printf("&%c:-  ", i);
        else if (n == 1)
            printf("&%c:%d  ", i, ic[0]);
        else
            printf("&%c:%d+%d  ", i, ic[0], ic[1]);
    }
    printf("\n");

    printf("\nWeapon glyphs (0x%08X, 1-based; slot 0 is blank by design)\n",
           Q2_HUD_ADDR_WEAPON_GLYPH);
    for (i = 0; i < Q2_HUD_WEAPON_SLOTS; i++)
        printf("  %2d  \"%s\"\n", i, t->weapon_glyph[i]);

    printf("\nBackdrop tiles (0x%08X / 0x%08X)\n",
           Q2_HUD_ADDR_BOX_UV, Q2_HUD_ADDR_BOX_RGB);
    for (i = 0; i < Q2_HUD_BOX_LEVELS; i++)
        printf("  *%d  uv=(0x%02X,0x%02X)  rgb=(%3u,%3u,%3u)\n", i,
               t->box[i].u, t->box[i].v,
               t->box_rgb[i][0], t->box_rgb[i][1], t->box_rgb[i][2]);

    printf("\nNotification lines by player count (0x%08X)\n  ",
           Q2_HUD_ADDR_MSG_LINES);
    for (i = 0; i < Q2_HUD_MSG_TIERS; i++)
        printf("%d:%u  ", i, t->message_lines[i]);
    printf("\n");

    for (i = 0; i < Q2_HUD_PALETTE_MAX; i++)
        if (t->palette[i].present)
            live++;
    printf("\nBuilt-in palettes (0x%08X): %u of %u slots, plus the 256-entry"
           " block at (0,255)\n", Q2_HUD_ADDR_PALETTES, live,
           t->palette_count);
    {
        static const struct { u32 id; const char *what; } named[] = {
            { Q2_HUD_PALETTE_MASK,     "menu mask"          },
            { Q2_HUD_PALETTE_FONT,     "font `|0`, crosshair" },
            { Q2_HUD_PALETTE_FONT_ALT, "font `|1`, default" },
            { Q2_HUD_PALETTE_BOX,      "backdrop ramp"      }
        };
        size_t k;
        for (k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
            const q2_hud_palette *p = q2_hud_palette_get(t, named[k].id);
            int e;
            if (!p)
                continue;
            printf("  %3u  vram=(%3d,%3d) clut=0x%04X stp=0x%04X  %-20s ",
                   named[k].id, p->vram_x, p->vram_y, p->clut_id, p->stp_mask,
                   named[k].what);
            for (e = 0; e < 4; e++)
                printf("%04X ", p->entry[e]);
            printf("...\n");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Check the atlas is where the executable says, on every map that has one.   */
static int check_atlas(const disc *d)
{
    int i, n = disc_file_count(d);
    char current[64];
    int with = 0, without = 0, bad = 0;

    current[0] = '\0';

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_vram_section vs;
        u32 index;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;
        len = (size_t)(slash - rest);
        if (len >= sizeof(map))
            len = sizeof(map) - 1;
        memcpy(map, rest, len);
        map[len] = '\0';

        if (strcmp(map, current) == 0)
            continue;
        strncpy(current, map, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';

        if (q2_vram_load(&vs, d, map) != Q2_OK)
            continue;

        if (!q2_vram_find_by_name(&vs, Q2_HUD_ATLAS_NAME, &index)) {
            without++;
        } else {
            /* 4bpp: bytes-per-row * 2 must cover the widest u the glyph and
             * icon tables reach, and the rows must cover v 0x80..0xFF. */
            u32 texels = (u32)vs.images[index].width * 2u;
            u32 rows   = vs.images[index].height;

            if (texels < 256 || rows < 128) {
                printf("  %-10s chars.lbm is %ux%u texels — too small\n",
                       map, texels, rows);
                bad++;
            } else {
                with++;
            }
        }
        q2_vram_free(&vs);
    }

    printf("\nAtlas: %d maps carry chars.lbm at 256x128 4bpp, %d do not,"
           " %d are the wrong size\n", with, without, bad);
    printf("It uploads to VRAM (%d,%d) — page (%d,%d), v origin %d — from"
           " slot 15 at 0x8003FEA4\n",
           Q2_HUD_ATLAS_VRAM_X, Q2_HUD_ATLAS_VRAM_Y,
           Q2_HUD_ATLAS_PAGE_X, Q2_HUD_ATLAS_PAGE_Y, 128);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Draw the overlay, so the binding can be looked at rather than asserted.    */
static int render_overlay(const disc *d, const q2_hud_tables *tab,
                          const char *map, const char *out_path)
{
    const int W = Q2_HUD_SPACE_W, H = Q2_HUD_SPACE_H;
    q2_vram_section vs;
    psx_vram *vram;
    psx_ot ot;
    psx_framebuffer fb;
    psx_raster_opts opts;
    q2_hud_font font;
    q2_hud_ctx ctx;
    q2_hud hud;
    q2_result r;
    int y;

    if (q2_vram_load(&vs, d, map) != Q2_OK) {
        fprintf(stderr, "cannot load %s/SNDVRAM.DAT\n", map);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        q2_vram_free(&vs);
        return 1;
    }

    r = q2_hud_font_upload(&font, tab, &vs, vram);
    q2_vram_free(&vs);
    if (r != Q2_OK) {
        fprintf(stderr, "%s has no %s (%s)\n", map, Q2_HUD_ATLAS_NAME,
                q2_result_str(r));
        free(vram);
        return 1;
    }
    printf("\nFont resident: tpage 0x%04X, cluts |0=0x%04X |1=0x%04X"
           " box=0x%04X\n", font.tpage, font.clut_font, font.clut_alt,
           font.clut_box);

    if (psx_ot_init(&ot, 8, 4096) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(vram);
        return 1;
    }

    q2_hud_ctx_default(&ctx, W, H);
    q2_hud_init(&hud, tab, 1);

    /* Everything the overlay can show, at once. */
    q2_hud_pickup(&hud, "Rocket Launcher");
    q2_hud_weapon_selected(&hud, tab, 8);      /* &O — Rocket Launcher */
    q2_hud_need_key(&hud, "Blue Key");
    q2_hud_message(&hud, "^C8F000|0Kills 12/40    3/5 Secrets");
    q2_hud_centre(&hud, tab, &ctx, "Mission Objective Complete");
    q2_hud_track(&hud, 100, 50);
    q2_hud_track(&hud, 88, 50);                /* a health hit: red flash */

    /* A backdrop that is not flat, so transparency is visible. */
    for (y = 0; y < H; y++) {
        int x;
        for (x = 0; x < W; x++)
            fb.px[y * W + x] = psx_rgb555((u8)(x / 4), (u8)(y / 2), 60);
    }

    psx_raster_opts_default(&opts);
    q2_hud_build_ot(&hud, &font, &ctx, &ot, 0);
    psx_raster_ot(&fb, &ot, vram, &opts);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    return 0;
}

/* ------------------------------------------------------------------------- */
int cmd_hud(const disc *d, const char *map, const char *out_path)
{
    q2_build_id id;
    q2_hud_tables tab;
    q2_result r;
    int rc = 0;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    r = q2_hud_tables_load(&tab, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the HUD tables: %s\n", q2_result_str(r));
        return 1;
    }

    printf("HUD tables from %s\n\n", id.exe_name);
    printf("There is no status bar. No health, ammo or armour readout exists"
           " in this build:\n"
           "every format string the executable hands to its text layer was"
           " enumerated and\n"
           "none of them formats a player statistic. What follows is what the"
           " overlay is.\n\n");

    dump_tables(&tab);

    if (!map) {
        rc = check_atlas(d);
    } else {
        rc = render_overlay(d, &tab, map, out_path ? out_path : "hud.ppm");
    }

    q2_hud_tables_free(&tab);
    return rc;
}
