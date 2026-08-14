/*
 * cmd_menu.c — the menu reconstruction, checked against the disc.
 *
 * `src/menu/pages.c` claims that a particular array of 24-byte records lives at
 * a particular address in the boot executable and says a particular thing. This
 * reads those records back off a real disc and compares them field by field, so
 * "the menu is transcribed correctly" is something the harness evaluates rather
 * than something a comment asserts. A mistyped coordinate or a label off by a
 * character fails the command.
 *
 * The record layout is the one the loader at 0x8001A474 walks:
 *
 *     +0x00 label   +0x04 x   +0x06 y   +0x08 action
 *     +0x0C toggle  +0x10 slider        +0x14 act-on-release
 *
 * Only the fields the port transcribes are compared: the pointers themselves
 * are addresses in a MIPS image and mean nothing here, but *whether* each is
 * null decides the widget, so that is checked too.
 */
#include "cmd_menu.h"

#include "exe.h"
#include "menu.h"
#include "menudraw.h"
#include "raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC 24u

static bool exe_str(const q2_exe *e, u32 addr, char *out, size_t n)
{
    size_t i;

    if (addr == 0) {
        out[0] = '\0';
        return true;
    }
    for (i = 0; i + 1 < n; i++) {
        u8 c;
        if (!q2_exe_u8(e, addr + (u32)i, &c))
            return false;
        out[i] = (char)c;
        if (c == 0)
            return true;
    }
    out[n - 1] = '\0';
    return true;
}

/* The widget the record's bindings imply, so the port's classification is
 * checked rather than assumed. */
static const char *widget_of(u32 toggle, u32 slider)
{
    if (slider) return "slider";
    if (toggle) return "toggle";
    return "text";
}

static const char *widget_name(u8 w)
{
    switch (w) {
    case Q2_WIDGET_TOGGLE: return "toggle";
    case Q2_WIDGET_SLIDER: return "slider";
    case Q2_WIDGET_CHOICE: return "choice";
    default:               return "text";
    }
}

static const char *page_name(int id)
{
    switch (id) {
    case Q2_PAGE_SCREEN_POSITION:  return "SCREEN POSITION";
    case Q2_PAGE_PAUSE_SP:         return "PAUSE (single player)";
    case Q2_PAGE_OPTIONS:          return "OPTIONS";
    case Q2_PAGE_PLAYER:           return "PLAYER";
    case Q2_PAGE_SOUND:            return "SOUND";
    case Q2_PAGE_VIDEO:            return "VIDEO";
    case Q2_PAGE_CONTROLLER:       return "CONTROLLER";
    case Q2_PAGE_RESTART_CONFIRM:  return "RESTART LEVEL?";
    case Q2_PAGE_RESTARTING:       return "RESTARTING LEVEL";
    case Q2_PAGE_RESUPPLY_CONFIRM: return "RESUPPLY?";
    case Q2_PAGE_RESUPPLYING:      return "RESUPPLYING";
    case Q2_PAGE_QUIT_CONFIRM:     return "QUIT GAME?";
    case Q2_PAGE_QUITTING:         return "QUITTING";
    case Q2_PAGE_NO_CONTROLLER:    return "NO CONTROLLER";
    case Q2_PAGE_DEATH:            return "DEATH";
    case Q2_PAGE_VARIABLES:        return "GAME VARIABLES";
    case Q2_PAGE_PAUSE_MP:         return "PAUSE (multiplayer)";
    default:                       return "?";
    }
}

/* Records in the table at `addr`; the loader stops at a null label. */
static u32 table_length(const q2_exe *e, u32 addr)
{
    u32 i;

    if (!addr)
        return 0;
    for (i = 0; i < 64; i++) {
        u32 lbl;
        if (!q2_exe_u32(e, addr + i * REC, &lbl))
            break;
        if (lbl == 0)
            break;
    }
    return i;
}

/* Where item `i` lives, given the page's one or two tables. */
static u32 record_addr(const q2_menu_page *p, u32 i)
{
    if (p->addr2 && i >= p->first)
        return p->addr2 + (i - p->first) * REC;
    return p->addr + i * REC;
}

/*
 * Compare one page against the tables it was transcribed from. Returns the
 * number of mismatches and prints each one.
 */
static int check_page(const q2_exe *e, const q2_menu_page *p, bool verbose)
{
    u32 i, bad = 0, na, nb, want_a, want_b;
    char label[64];

    na = table_length(e, p->addr);
    nb = table_length(e, p->addr2);

    /* Group A holds everything before `first` when there is a second table,
     * and the whole page when there is not. */
    want_a = (p->addr2 && p->first > 0) ? p->first : p->count;
    want_b = p->count - want_a;

    if (verbose)
        printf("  %-22s %08X  %u item%s\n", page_name(p->id), p->addr,
               p->count, p->count == 1 ? "" : "s");

    for (i = 0; i < p->count; i++) {
        u32 base = record_addr(p, i);
        u32 lbl, act, tog, sld, rel;
        s16 x, y;
        const q2_menu_item *it = &p->items[i];
        const char *want_widget;

        if (!q2_exe_u32(e, base + 0x00, &lbl) ||
            !q2_exe_s16(e, base + 0x04, &x) ||
            !q2_exe_s16(e, base + 0x06, &y) ||
            !q2_exe_u32(e, base + 0x08, &act) ||
            !q2_exe_u32(e, base + 0x0C, &tog) ||
            !q2_exe_u32(e, base + 0x10, &sld) ||
            !q2_exe_u32(e, base + 0x14, &rel)) {
            printf("    ! item %u: record escapes the segment\n", i);
            bad++;
            continue;
        }

        if (!exe_str(e, lbl, label, sizeof(label))) {
            printf("    ! item %u: label pointer %08X is not readable\n", i, lbl);
            bad++;
            continue;
        }

        if (strcmp(label, it->label) != 0) {
            printf("    ! item %u: label \"%s\" on disc, \"%s\" in the port\n",
                   i, label, it->label);
            bad++;
        }
        if (x != it->x || y != it->y) {
            printf("    ! item %u (%s): (%d,%d) on disc, (%d,%d) in the port\n",
                   i, label, x, y, it->x, it->y);
            bad++;
        }
        if ((rel & 1u) != (u32)it->on_release) {
            printf("    ! item %u (%s): fires on %s, the port says %s\n",
                   i, label, (rel & 1u) ? "release" : "press",
                   it->on_release ? "release" : "press");
            bad++;
        }
        /*
         * The death screen is the one page whose records carry no action on
         * disc: its hook installs all three once the 600-tick countdown at
         * 0x800205B0 expires, which is what makes the screen inert for a
         * moment after you die.
         */
        if (p->id != Q2_PAGE_DEATH &&
            (act != 0) != (it->action != Q2_ACT_NONE)) {
            printf("    ! item %u (%s): %s an action on disc, the port %s\n",
                   i, label, act ? "has" : "has no",
                   it->action ? "has one" : "has none");
            bad++;
        }

        /*
         * The CONTROLLER page is the documented exception: its records carry
         * no bindings because its own hook writes the configuration block
         * directly, so the port's widgets are that hook's effect, not the
         * table's. Everything else must agree.
         */
        want_widget = widget_of(tog, sld);
        if (p->id != Q2_PAGE_CONTROLLER &&
            strcmp(want_widget, widget_name(it->widget)) != 0) {
            printf("    ! item %u (%s): %s on disc, %s in the port\n",
                   i, label, want_widget, widget_name(it->widget));
            bad++;
        }

        if (verbose)
            printf("      %2u  %-26s x=%3d y=%3d  %-6s %s\n", i,
                   label[0] ? label : "(empty)", x, y,
                   widget_name(it->widget),
                   (rel & 1u) ? "on-release" : "");
    }

    /*
     * The port may transcribe fewer records than a table holds when the page
     * excludes some — the single-player pause menu drops its trailing empty
     * record with the `count -= 1` at 0x8001D6F4. Anything else is a gap.
     */
    if (na != want_a && !(p->id == Q2_PAGE_PAUSE_SP && na == want_a + 1u)) {
        printf("    ! %08X holds %u records, the port transcribes %u\n",
               p->addr, na, want_a);
        bad++;
    }
    if (p->addr2 && nb != want_b) {
        printf("    ! %08X holds %u records, the port transcribes %u\n",
               p->addr2, nb, want_b);
        bad++;
    }

    return (int)bad;
}

/* ------------------------------------------------------------------------- */

static void dump_page(const q2_menu_page *p, int cheat_level)
{
    q2_menu_settings set;
    q2_menu m;
    u32 i;

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    m.cheat_level = cheat_level;
    m.page        = p;
    m.page_id     = p->id;
    m.cursor      = p->first;
    m.open        = true;

    printf("\npage %-2u  %-22s  table %08X\n", p->id, page_name(p->id), p->addr);
    if (p->title)
        printf("  title  \"%s\"  at (256,%d), font 32\n",
               p->title, q2_menu_title_y(Q2_MENU_SCREEN_H));
    if (p->first >= p->count)
        printf("  (nothing on this page is selectable)\n");

    for (i = 0; i < p->count; i++) {
        char line[80];
        const q2_menu_item *it = &p->items[i];

        m.cursor = (int)i;
        q2_menu_item_display(&m, (int)i, line, (u32)sizeof(line));

        printf("  %c%2u  %-30s x=%3d y=%3d  %-6s%s\n",
               (i >= p->first) ? ' ' : '.', i,
               line[0] ? line : "(empty)", it->x, it->y,
               widget_name(it->widget),
               it->on_release ? "  on-release" : "");
    }

    if (p->back != Q2_ACT_NONE)
        printf("  triangle: back\n");
}

/*
 * Draw a page at the console's own resolution. The geometry pipeline was
 * brought up the same way — write a PPM, look at it — and a menu is no
 * different: a coordinate that is right in the table can still land in the
 * wrong place once it is drawn.
 */
static int shoot_page(const q2_menu_page *p, int cheat_level, const char *out,
                      int w, int h)
{
    psx_framebuffer fb;
    q2_menu_settings set;
    q2_menu_style    style;
    q2_menu m;

    if (psx_fb_init(&fb, w, h) != Q2_OK) {
        fprintf(stderr, "cannot allocate a framebuffer\n");
        return 1;
    }

    q2_menu_settings_defaults(&set);
    q2_menu_init(&m, &set, Q2_MENU_SCREEN_H);
    m.cheat_level = cheat_level;
    m.open        = true;
    q2_menu_goto(&m, p->id);
    m.page = p;                    /* honour the variant the caller picked */
    if (p->id == Q2_PAGE_DEATH) {
        q2_menu_set_resupplies(&m, 2);
        q2_menu_goto(&m, Q2_PAGE_DEATH);
        m.arm_ticks = 0;
    }
    if (p->id == Q2_PAGE_PAUSE_SP)
        q2_menu_set_stats(&m, 12, 40, 1, 3);

    q2_menu_style_default(&style);
    /* Nothing behind it here, so the backdrop is the whole picture. */
    style.backdrop_alpha = 255;
    psx_fb_clear(&fb, psx_rgb555(16, 16, 40));

    q2_menu_draw(&m, &fb, &style);

    if (psx_fb_write_ppm(&fb, out) != Q2_OK) {
        fprintf(stderr, "cannot write %s\n", out);
        psx_fb_free(&fb);
        return 1;
    }

    printf("wrote %s (%dx%d)\n", out, fb.width, fb.height);
    psx_fb_free(&fb);
    return 0;
}

int cmd_menu(const disc *d, const char *want, const char *out, const char *size)
{
    q2_exe exe;
    const q2_menu_page *pages;
    u32 count, i;
    int bad = 0, checked = 0;
    q2_result r;

    pages = q2_menu_pages(&count);

    if (out) {
        const q2_menu_page *p = want ? q2_menu_page_find(atoi(want)) : NULL;
        int w = Q2_MENU_SCREEN_W, h = Q2_MENU_SCREEN_H;

        if (!p) {
            fprintf(stderr, "a page id is needed to draw one; try 26\n");
            return 1;
        }
        /* A size, because the layout is authored for the console's 512x248 and
         * the client's surface is not that: seeing a page at the size it will
         * actually be drawn is the check worth having. */
        if (size && sscanf(size, "%dx%d", &w, &h) != 2) {
            fprintf(stderr, "size must look like 320x256\n");
            return 1;
        }
        return shoot_page(p, 0, out, w, h);
    }

    /* Dump first: the reconstruction is useful even without a disc to check
     * it against, and the check reads better after the thing it is checking. */
    for (i = 0; i < count; i++) {
        if (want && atoi(want) != (int)pages[i].id)
            continue;
        dump_page(&pages[i], 0);
    }

    /* The two pages with variants, which the id-keyed list holds only once. */
    if (!want || atoi(want) == Q2_PAGE_VARIABLES) {
        int lvl;
        for (lvl = 1; lvl <= 3; lvl++) {
            printf("\n-- GAME VARIABLES with cheats %s --",
                   q2_menu_cheat_level_name(lvl));
            dump_page(q2_menu_variables_page(lvl), lvl);
        }
    }
    if (!want || atoi(want) == Q2_PAGE_VIDEO) {
        printf("\n-- VIDEO in multiplayer --");
        dump_page(q2_menu_video_page(true), 0);
    }

    r = q2_exe_load(&exe, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "\ncannot read the boot executable: %s\n",
                q2_result_str(r));
        return 1;
    }

    printf("\nchecking the transcription against %s\n", exe.name);

    for (i = 0; i < count; i++) {
        bad += check_page(&exe, &pages[i], false);
        checked++;
    }
    for (i = 1; i <= 3; i++) {
        bad += check_page(&exe, q2_menu_variables_page((int)i), false);
        checked++;
    }
    bad += check_page(&exe, q2_menu_video_page(true), false);
    checked++;

    printf("%d page%s checked, %d mismatch%s\n",
           checked, checked == 1 ? "" : "s", bad, bad == 1 ? "" : "es");

    q2_exe_free(&exe);
    return bad == 0 ? 0 : 1;
}
