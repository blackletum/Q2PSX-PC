/*
 * test_panel.c — the UI panel frame and the briefing screen that sits in it.
 *
 * The frame is eight quads cut from three pieces of art, and four of them are
 * mirrored by writing their screen corners backwards. That is the part a
 * plausible-looking mistake survives: an unmirrored corner still draws a
 * corner, just the wrong one, and the border comes out with two rounded ends
 * and two square ones. So the checks below are about ORIENTATION and COVERAGE,
 * not about whether a primitive appeared.
 */
#include "panel.h"
#include "briefing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static psx_ot *make_ot(void)
{
    psx_ot *ot = (psx_ot *)calloc(1, sizeof(*ot));

    psx_ot_init(ot, 217, 4096);
    return ot;
}

/* Every primitive in one bucket, in the order they were added. */
static u32 gather(psx_ot *ot, u32 bucket, psx_prim **out, u32 max)
{
    u32 n = 0, i;

    for (i = 0; i < ot->prim_count && n < max; i++)
        if (ot->prims[i].otz == bucket)
            out[n++] = &ot->prims[i];
    return n;
}

static void test_frame_shape(void)
{
    q2_menu_font font;
    q2_panel_rect r = { 96, 32, 336, 100 };
    psx_ot *ot = make_ot();
    psx_prim *p[16];
    u32 n, i;

    memset(&font, 0, sizeof(font));
    font.tpage_item = 0x1234;
    font.clut_text  = 0x5678;

    n = q2_panel_frame_ot(&font, &r, ot, 40);
    CHECK(n == Q2_PANEL_FRAME_PRIMS, "the frame is %d quads, got %u",
          Q2_PANEL_FRAME_PRIMS, n);

    n = gather(ot, 40, p, 16);
    CHECK(n == Q2_PANEL_FRAME_PRIMS, "all of them landed in the bucket");

    for (i = 0; i < n; i++) {
        CHECK(p[i]->kind == PSX_PRIM_FT4, "quad %u is a textured quad", i);
        /* code 0x2D — raw, so the texture is not modulated, and no ABE bit. */
        CHECK(!p[i]->textured_blend, "quad %u is raw (code 0x2D)", i);
        CHECK(!p[i]->semi_transparent, "quad %u is opaque", i);
        CHECK(p[i]->tpage == 0x1234 && p[i]->clut == 0x5678,
              "quad %u draws from frontend.lbm with the menu's palette", i);
    }

    /*
     * The two top corners must be mirror images: same texture span, opposite
     * screen direction. This is the check that fails if the mirroring is lost.
     */
    {
        int tl_dir = p[0]->xy[1].x - p[0]->xy[0].x;
        int tr_dir = p[1]->xy[1].x - p[1]->xy[0].x;

        CHECK(tl_dir > 0, "the top-left corner runs left to right");
        CHECK(tr_dir < 0, "the top-right corner runs right to left");
        CHECK(tl_dir == -tr_dir, "and they are the same width, %d vs %d",
              tl_dir, -tr_dir);
        CHECK(p[0]->uv[0].u == p[1]->uv[0].u && p[0]->uv[1].u == p[1]->uv[1].u,
              "both sample the same texels");
    }

    /* And the two left/right corners of the bottom pair are mirrored in y. */
    {
        CHECK(p[2]->xy[3].y - p[2]->xy[0].y < 0,
              "the bottom-right corner runs upwards");
        CHECK(p[3]->xy[3].y - p[3]->xy[0].y < 0,
              "the bottom-left corner runs upwards");
        CHECK(p[0]->xy[3].y - p[0]->xy[0].y > 0,
              "while the top-left runs downwards");
    }

    /* The corner patch is 46 x 29 on screen and 46 x 29 in the sheet: 1:1. */
    {
        int w = abs(p[0]->xy[1].x - p[0]->xy[0].x) + 1;
        int h = abs(p[0]->xy[3].y - p[0]->xy[0].y) + 1;
        int uw = abs((int)p[0]->uv[1].u - (int)p[0]->uv[0].u) + 1;
        int vh = abs((int)p[0]->uv[3].v - (int)p[0]->uv[0].v) + 1;

        CHECK(w == Q2_PANEL_CORNER_W && uw == Q2_PANEL_CORNER_W,
              "the corner is %d wide from %d texels, want %d",
              w, uw, Q2_PANEL_CORNER_W);
        CHECK(h == Q2_PANEL_CORNER_H && vh == Q2_PANEL_CORNER_H,
              "and %d tall from %d texels, want %d",
              h, vh, Q2_PANEL_CORNER_H);
    }

    /* The horizontal edges STRETCH: 19 texels across a much wider span. That
     * is the difference between an edge and a corner, and getting it backwards
     * gives a border with a 19-pixel gap in the middle of every side. */
    {
        int span = p[4]->xy[1].x - p[4]->xy[0].x + 1;
        int texels = (int)p[4]->uv[1].u - (int)p[4]->uv[0].u + 1;

        CHECK(texels == Q2_PANEL_EDGE_H_W,
              "the top edge samples %d texels, want %d",
              texels, Q2_PANEL_EDGE_H_W);
        CHECK(span > texels * 4, "and stretches them across %d pixels", span);
        CHECK(p[4]->xy[0].x == r.x + 36 && p[4]->xy[1].x == r.x + r.w - 35,
              "meeting the corners at both ends");
    }

    free(ot);
}

static void test_frame_overhang(void)
{
    q2_menu_font font;
    q2_panel_rect r = { 100, 50, 200, 80 }, b;
    psx_ot *ot = make_ot();
    psx_prim *p[16];
    u32 n, i;
    int minx = 30000, maxx = -30000, miny = 30000, maxy = -30000;

    memset(&font, 0, sizeof(font));
    q2_panel_frame_ot(&font, &r, ot, 12);
    n = gather(ot, 12, p, 16);

    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; k < 4; k++) {
            if (p[i]->xy[k].x < minx) minx = p[i]->xy[k].x;
            if (p[i]->xy[k].x > maxx) maxx = p[i]->xy[k].x;
            if (p[i]->xy[k].y < miny) miny = p[i]->xy[k].y;
            if (p[i]->xy[k].y > maxy) maxy = p[i]->xy[k].y;
        }
    }

    /* 10 either side, 6 top and bottom — the numbers a caller has to leave
     * room for, and the reason the briefing's box is inset from 512 to 336. */
    CHECK(minx == r.x - Q2_PANEL_OVERHANG_X, "the frame reaches %d left of x",
          r.x - minx);
    CHECK(miny == r.y - Q2_PANEL_OVERHANG_Y, "and %d above y", r.y - miny);

    q2_panel_bounds(&r, &b);
    CHECK(b.x == minx && b.y == miny, "q2_panel_bounds agrees with the quads");
    CHECK(b.w == r.w + 2 * Q2_PANEL_OVERHANG_X, "and with the width");

    free(ot);
}

static void test_body_is_black_and_doubled(void)
{
    q2_panel_rect r = { 96, 32, 336, 100 };
    psx_ot *ot = make_ot();
    psx_prim *p[8];
    u32 n, i;

    n = q2_panel_body_ot(&r, ot, 30);
    CHECK(n == Q2_PANEL_BODY_PRIMS, "the body is %d tiles, got %u",
          Q2_PANEL_BODY_PRIMS, n);

    n = gather(ot, 30, p, 8);
    for (i = 0; i < n; i++) {
        CHECK(p[i]->kind == PSX_PRIM_TILE, "tile %u is a TILE", i);
        CHECK(p[i]->semi_transparent, "tile %u is semi-transparent", i);
        /* Black at B/2 + F/2 darkens; twice gets to a quarter. A single tile
         * at some pre-darkened colour would look right alone and wrong the
         * moment anything else translucent overlapped it. */
        CHECK(p[i]->rgb[0].r == 0 && p[i]->rgb[0].g == 0 && p[i]->rgb[0].b == 0,
              "tile %u is black", i);
        CHECK(p[i]->xy[0].x == r.x && p[i]->xy[0].y == r.y &&
              p[i]->xy[1].x == r.w && p[i]->xy[1].y == r.h,
              "tile %u covers the rectangle exactly", i);
    }
    CHECK(n >= 2 && p[0]->xy[0].x == p[1]->xy[0].x,
          "both tiles are the same rectangle");

    free(ot);
}

static void test_briefing_string(void)
{
    q2_briefing b;
    char out[1024];
    u32 len;

    q2_briefing_init(&b);

    CHECK(b.box.x == Q2_BRIEFING_BOX_X && b.box.y == Q2_BRIEFING_BOX_Y &&
          b.box.w == Q2_BRIEFING_BOX_W && b.box.h == Q2_BRIEFING_BOX_H,
          "the box is gp+300..306's (96, 32, 336, 100)");

    /* Untouched, the screen shows the two shipped strings. */
    CHECK(strcmp(b.orders, "Awaiting Orders.") == 0, "orders default");
    CHECK(strcmp(b.objective, "Awaiting Mission Objective.") == 0,
          "objective default");

    q2_briefing_set_location(&b, "Outer Base");
    q2_briefing_set_orders(&b, "Secure the perimeter.");

    len = q2_briefing_compose(&b, out, sizeof(out));
    CHECK(len > 0, "it composes");

    /*
     * The margin escape is the whole point of this screen: it is the only
     * place in the game that turns word wrap on, so if it stops being emitted
     * the objective silently runs off the panel instead of wrapping.
     */
    CHECK(strstr(out, "#06A196") != NULL, "the margins are set to 106..406");
    CHECK(strstr(out, "#000000") != NULL, "and cleared again at the end");
    CHECK(strstr(out, "#000000") > strstr(out, "#06A196"),
          "in that order");

    CHECK(strstr(out, "~4Location:") != NULL, "the Location label, ~4 and all");
    CHECK(strstr(out, "Current Orders:") != NULL, "the orders label");
    CHECK(strstr(out, "Mission Objective:") != NULL, "the objective label");
    CHECK(strstr(out, "Outer Base") != NULL, "the location value");
    CHECK(strstr(out, "Secure the perimeter.") != NULL, "the orders value");

    /* Labels green, values olive, alternating. */
    CHECK(strstr(out, "^508C78") != NULL, "the label colour");
    CHECK(strstr(out, "^788C50") != NULL, "the value colour");
    CHECK(strstr(out, "^508C78") < strstr(out, "^788C50"),
          "a label comes before the first value");

    /* A field longer than the buffer must not produce a half-written escape —
     * that would be interpreted as text and corrupt everything after it. */
    {
        char tiny[16];
        CHECK(q2_briefing_compose(&b, tiny, sizeof(tiny)) == 0,
              "a buffer too small refuses rather than truncating");
    }
}

static void test_briefing_wrap_is_live(void)
{
    /* The margins the escape carries have to be the ones the pen ships with,
     * because hud.c reads 0x800AE8E0/0x800AE8E4 for its defaults and the
     * briefing writes the same two numbers back. If these ever disagree, one
     * of the two was misread. */
    q2_hud_pen pen;

    q2_hud_pen_default(&pen);
    CHECK(pen.left == Q2_BRIEFING_MARGIN_L,
          "the shipped left margin is 0x%03X, the briefing sets 0x%03X",
          (unsigned)pen.left, Q2_BRIEFING_MARGIN_L);
    CHECK(pen.right == Q2_BRIEFING_MARGIN_R,
          "the shipped right margin is 0x%03X, the briefing sets 0x%03X",
          (unsigned)pen.right, Q2_BRIEFING_MARGIN_R);
    CHECK(!pen.wrap, "but wrap ships OFF — the escape is what turns it on");
}

int main(void)
{
    test_frame_shape();
    test_frame_overhang();
    test_body_is_black_and_doubled();
    test_briefing_string();
    test_briefing_wrap_is_live();

    if (g_fail) {
        printf("\n%d panel check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("panel: all checks passed\n");
    return 0;
}
