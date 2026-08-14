/*
 * test_screen.c — the screen's behaviour, not its constants.
 *
 * `q2psx-inspect screen <disc>` already reads every literal back off a real
 * executable, so nothing here re-asserts a projection distance. What this pins
 * down is the behaviour those constants produce and that no table can check:
 * that the buffers alternate, that the ordering table's slices tile it exactly
 * and do not overlap, that a viewport's primitives cannot escape into another
 * viewport's slice, that the screen shake shrinks a viewport rather than moving
 * it off its own edge, that the full-screen background clear turns the
 * per-viewport ones off, and that a long host frame is clamped rather than
 * averaged.
 */
#include "screen.h"

#include <stdio.h>
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

/* ------------------------------------------------------------------------- */
static void test_bringup(void)
{
    q2_screen s;

    CHECK(q2_screen_init(&s, Q2_VIDEO_PAL) == Q2_OK, "init failed");

    CHECK(s.disp.width == 512 && s.disp.height == 248,
          "framebuffer %ux%u", s.disp.width, s.disp.height);
    CHECK(s.disp.video_mode == 1, "video mode %u", s.disp.video_mode);
    CHECK(s.disp.field_hz == 50 && s.disp.vsync_divisor == 2, "frame lock");
    CHECK(!s.disp.height_is_inferred, "PAL height should be read, not inferred");

    /* Boot is the front end's single-buffered full-screen state. */
    CHECK(s.layout == Q2_SCREEN_LAYOUT_FULL_SINGLE, "boot layout %d", (int)s.layout);
    CHECK(s.disp.buf[0].x == s.disp.buf[1].x && s.disp.buf[0].y == s.disp.buf[1].y,
          "the boot layout is single buffered");

    CHECK(s.buf[0].px && s.buf[1].px, "two framebuffers");
    CHECK(s.buf[0].width == 512 && s.buf[0].height == 248, "buffer geometry");

    q2_screen_free(&s);

    /*
     * An NTSC build's framebuffer has not been read out of an NTSC executable,
     * and PAL's 248 already refuted the widely repeated 256 — so asking for one
     * must yield the PAL geometry *and say so*, never a guess presented as a
     * fact.
     */
    CHECK(q2_screen_init(&s, Q2_VIDEO_NTSC) == Q2_OK, "ntsc init failed");
    CHECK(s.disp.height_is_inferred, "an NTSC screen must admit it is inferred");
    CHECK(s.disp.height == 248, "and must not invent a height");
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_swap(void)
{
    q2_screen s;
    psx_ot ot;
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);

    CHECK(s.disp.draw_buffer == 0 && s.disp.disp_buffer == 1, "initial indices");

    for (i = 0; i < 6; i++) {
        u8 before = s.disp.draw_buffer;
        q2_screen_frame_begin(&s, &ot);
        CHECK(s.disp.draw_buffer == (u8)(1 - before), "swap %d flips the draw buffer", i);
        CHECK(s.disp.disp_buffer == before, "swap %d shows the previous buffer", i);

        /* Present makes what was drawn what is shown, so a host reading the
         * front buffer after present sees this frame. */
        q2_screen_present(&s);
        CHECK(q2_screen_front(&s) == &s.buf[s.disp.draw_buffer],
              "present %d exposes the buffer just drawn", i);
    }

    /* The three-slot ring rotates by one per swap and therefore returns to its
     * start every three frames (0x80018214 with the {0,1,2} from 0x8007657C). */
    q2_screen_init(&s, Q2_VIDEO_PAL);
    for (i = 0; i < 3; i++)
        q2_screen_frame_begin(&s, &ot);
    CHECK(s.disp.rotate[0] == 0 && s.disp.rotate[1] == 1 && s.disp.rotate[2] == 2,
          "the ring is back to {0,1,2} after three swaps: {%u,%u,%u}",
          s.disp.rotate[0], s.disp.rotate[1], s.disp.rotate[2]);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * The slicing is the whole mechanism behind split screen, so this checks it as
 * a partition: every bucket a viewport can reach belongs to that viewport, the
 * slices do not overlap, and nothing lands on a draw env's bucket or outside
 * the table.
 */
static void test_ot_slices(void)
{
    q2_screen s;
    int p;
    int owner[Q2_SCREEN_OT_ENTRIES];
    int i;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);

    for (i = 0; i < Q2_SCREEN_OT_ENTRIES; i++)
        owner[i] = -1;

    for (p = 0; p < 4; p++) {
        u32 env = (u32)Q2_SCREEN_OT_VIEW_BASE + (u32)p * Q2_SCREEN_OT_VIEW_STRIDE
                + Q2_SCREEN_OT_VIEW_ENV;
        u32 z;

        for (z = 0; z < 4096; z++) {
            u16 b = q2_screen_view_otz(&s, p, z);

            CHECK(b < Q2_SCREEN_OT_ENTRIES, "view %d z %u -> bucket %u is off the table",
                  p, z, b);
            if (b >= Q2_SCREEN_OT_ENTRIES)
                break;
            CHECK(b != env, "view %d z %u landed on its own draw env bucket", p, z);
            CHECK(owner[b] == -1 || owner[b] == p,
                  "bucket %u is claimed by both view %d and view %d", b, owner[b], p);
            owner[b] = p;
        }
    }

    /* The overlay owns its own slice and cannot reach a viewport's. */
    {
        u32 z;
        for (z = 0; z < 4096; z++) {
            u16 b = q2_screen_overlay_otz(&s, z);
            CHECK(b >= Q2_SCREEN_OT_OVERLAY &&
                  b < Q2_SCREEN_OT_OVERLAY + Q2_SCREEN_OT_OVERLAY_LEN,
                  "overlay z %u -> bucket %u escapes the overlay slice", z, b);
            CHECK(owner[b] == -1, "overlay bucket %u collides with view %d", b, owner[b]);
        }
    }

    /* Bucket 1 is the full-screen background env and belongs to nobody else. */
    CHECK(owner[Q2_SCREEN_OT_BACKGROUND] == -1,
          "the background env bucket was claimed by a viewport");

    q2_screen_free(&s);
}

/* The same guarantee has to hold through the ordering table itself, because
 * that is the path the world builder actually takes. */
static void test_ot_window(void)
{
    q2_screen s;
    psx_ot ot;
    u32 b;
    int p;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);

    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 256);
    q2_screen_frame_begin(&s, &ot);

    for (p = 0; p < 2; p++) {
        u32 z;
        CHECK(q2_screen_view_begin(&s, p, &ot, NULL), "view %d should be live", p);
        CHECK(psx_ot_bucket_span(&ot) == Q2_SCREEN_OT_VIEW_STRIDE - 2,
              "view %d span %u", p, psx_ot_bucket_span(&ot));

        /* Deliberately overshoot: an emitter that hands over a huge depth must
         * saturate inside its own slice, not spill into the next viewport. */
        for (z = 0; z < 64; z++)
            psx_ot_add(&ot, (u16)(z * 37));
    }

    CHECK(!q2_screen_view_begin(&s, 2, &ot, NULL), "view 2 is not live in a 2P layout");

    for (b = 0; b < Q2_SCREEN_OT_ENTRIES; b++) {
        if (ot.bucket_head[b] < 0)
            continue;
        CHECK(b >= (u32)Q2_SCREEN_OT_VIEW_BASE, "bucket %u is below the first slice", b);
        CHECK(b < (u32)Q2_SCREEN_OT_VIEW_BASE + 2u * Q2_SCREEN_OT_VIEW_STRIDE,
              "bucket %u is past the second slice", b);
    }

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_layouts(void)
{
    q2_screen s;

    q2_screen_init(&s, Q2_VIDEO_PAL);

    /* The layout the session mode would pick. */
    CHECK(q2_screen_layout_for(1, true)  == Q2_SCREEN_LAYOUT_ONE,   "1P");
    CHECK(q2_screen_layout_for(2, true)  == Q2_SCREEN_LAYOUT_TWO_H, "2P split on");
    CHECK(q2_screen_layout_for(2, false) == Q2_SCREEN_LAYOUT_TWO_V, "2P split off");
    CHECK(q2_screen_layout_for(3, true)  == Q2_SCREEN_LAYOUT_QUAD,  "3P");
    CHECK(q2_screen_layout_for(4, false) == Q2_SCREEN_LAYOUT_QUAD,  "4P");

    /* Three players use the four-quadrant layout with one quadrant unused. */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 3);
    CHECK(s.view_count == 3, "3P view count %d", s.view_count);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    CHECK(s.view_count == 4, "4P view count %d", s.view_count);

    /*
     * Every layout must give each viewport a geometry offset at its own centre,
     * because that is what makes the GTE produce viewport-local pixels that the
     * draw env's offset can then place. Getting this wrong is invisible in one
     * viewport and catastrophic in four.
     */
    {
        int l, i;
        for (l = 0; l < Q2_SCREEN_LAYOUT_COUNT; l++) {
            q2_screen_set_layout(&s, (q2_screen_layout)l, 4);
            for (i = 0; i < s.view_count; i++) {
                const q2_screen_view *v = &s.view[i];
                CHECK(v->ofs_x == v->w / 2 && v->ofs_y == v->h / 2,
                      "%s view %d offset (%d,%d) is not its centre (%d,%d)",
                      q2_screen_layout_name((q2_screen_layout)l), i,
                      v->ofs_x, v->ofs_y, v->w / 2, v->h / 2);
                CHECK(v->w > 0 && v->h > 0, "%s view %d is empty",
                      q2_screen_layout_name((q2_screen_layout)l), i);
                CHECK(v->x >= 0 && v->y >= 0 &&
                      v->x + v->w <= (s16)s.disp.width + 1 &&
                      v->y + v->h <= (s16)s.disp.height + 1,
                      "%s view %d (%d,%d %dx%d) leaves the framebuffer",
                      q2_screen_layout_name((q2_screen_layout)l), i,
                      v->x, v->y, v->w, v->h);
            }
        }
    }

    /*
     * The two full-screen layouts differ only in projection distance and in
     * whether they double buffer — which is the entire reason FULL_SINGLE is a
     * separate layout rather than a duplicate of ONE.
     */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    CHECK(s.view[0].proj == 160, "1P projection %d", s.view[0].proj);
    CHECK(s.disp.buf[1].x == 512, "1P is double buffered");
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);
    CHECK(s.view[0].proj == 320, "boot projection %d", s.view[0].proj);
    CHECK(s.disp.buf[1].x == 0, "the boot layout is single buffered");

    /* The horizontal split's 2D extent is NOT its viewport size — it is 320x160
     * against a 512x123 viewport, which is the sort of thing that only survives
     * a port if it is transcribed rather than derived. */
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_H, 2);
    CHECK(s.view[0].w == 512 && s.view[0].h == 123, "2P-H viewport %dx%d",
          s.view[0].w, s.view[0].h);
    CHECK(s.view[0].vw == 320 && s.view[0].vh == 160, "2P-H 2D extent %dx%d",
          s.view[0].vw, s.view[0].vh);
    CHECK(s.view[1].y == 121, "2P-H second viewport y %d", s.view[1].y);

    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    CHECK(s.view[0].w == 255, "2P-V width %d", s.view[0].w);
    CHECK(s.view[0].ofs_x == 127, "2P-V odd width halves toward zero: %d",
          s.view[0].ofs_x);

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * Composition. The draw envs live in the table and change the clip as the walk
 * reaches them, so a primitive built for viewport 1 must not put a pixel in
 * viewport 0 — even when its coordinates say it should.
 */
static void test_compose_clip(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    int x, y, stray = 0;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 64);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);

    /* One huge white tile in the right-hand viewport, spanning far more than
     * the viewport holds. */
    q2_screen_view_begin(&s, 1, &ot, NULL);
    p = psx_ot_add(&ot, q2_screen_view_otz(&s, 1, 0));
    CHECK(p != NULL, "no room in the primitive pool");
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = -400; p->xy[0].y = -400;
        p->xy[2].x =  900; p->xy[2].y =  900;
        p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
    }

    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);
    fb = q2_screen_front(&s);

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            bool lit = fb->px[y * fb->width + x] != 0;
            bool inside = (x >= s.view[1].x && x < s.view[1].x + s.view[1].w &&
                           y >= s.view[1].y && y < s.view[1].y + s.view[1].h);
            if (lit != inside)
                stray++;
        }
    }
    CHECK(stray == 0, "%d pixels disagree with viewport 1's clip rectangle", stray);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * The background clear. Arming the full-screen one must turn every viewport's
 * own clear off, or the split-screen gutters — the only pixels the full-screen
 * clear owns — would be the only thing it painted.
 */
static void test_background(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    int i, x, y, wrong = 0;
    u16 want;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;

    s.disp.bg_rgb[0] = 8;
    s.disp.bg_rgb[1] = 16;
    s.disp.bg_rgb[2] = 248;
    s.disp.bg_enable = 1;
    for (i = 0; i < 4; i++)
        s.view[i].bg_enable = 1;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0x7FFF);
    q2_screen_background(&s);

    for (i = 0; i < 4; i++)
        CHECK(s.view[i].bg_enable == 0,
              "view %d still clears after the full-screen clear was armed", i);

    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);

    fb   = q2_screen_front(&s);
    want = psx_rgb555(8, 16, 248);
    for (y = 0; y < fb->height; y++)
        for (x = 0; x < fb->width; x++)
            if (fb->px[y * fb->width + x] != want)
                wrong++;
    CHECK(wrong == 0, "%d pixels were not cleared to the background colour", wrong);

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/*
 * The shake. 0x80076C18 displaces the draw rectangle's origin and subtracts the
 * same amount from its size, so the far edge is pinned and the viewport gets
 * smaller — a shake that never lets a viewport bleed over its neighbour.
 */
static void test_shake(void)
{
    q2_screen s;
    psx_ot ot;
    psx_raster_opts opts;
    const psx_framebuffer *fb;
    psx_prim *p;
    int x, y, stray = 0;
    const int sx = 6, sy = 4;

    q2_screen_init(&s, Q2_VIDEO_PAL);
    q2_screen_set_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 16);
    psx_raster_opts_default(&opts);
    opts.textures = false;
    opts.dither   = false;

    s.view[0].shake_x = (s16)sx;
    s.view[0].shake_y = (s16)sy;

    q2_screen_frame_begin(&s, &ot);
    psx_fb_clear(q2_screen_back(&s), 0);
    q2_screen_view_begin(&s, 0, &ot, NULL);
    p = psx_ot_add(&ot, q2_screen_view_otz(&s, 0, 0));
    if (p) {
        p->kind = PSX_PRIM_TILE;
        p->xy[0].x = -900; p->xy[0].y = -900;
        p->xy[2].x =  900; p->xy[2].y =  900;
        p->rgb[0].r = p->rgb[0].g = p->rgb[0].b = 255;
    }
    q2_screen_compose(&s, &ot, NULL, &opts);
    q2_screen_present(&s);
    fb = q2_screen_front(&s);

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            bool lit = fb->px[y * fb->width + x] != 0;
            bool inside = (x >= s.view[0].x + sx &&
                           x <  s.view[0].x + s.view[0].w &&
                           y >= s.view[0].y + sy &&
                           y <  s.view[0].y + s.view[0].h);
            if (lit != inside)
                stray++;
        }
    }
    CHECK(stray == 0, "%d pixels disagree with the shaken clip rectangle", stray);

    /* And the far edge really is pinned. */
    CHECK(s.view[0].x + sx + (s.view[0].w - sx) == s.view[0].x + s.view[0].w,
          "the shake moved the far edge");

    psx_ot_free(&ot);
    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
static void test_dt_clamp(void)
{
    q2_screen s;

    q2_screen_init(&s, Q2_VIDEO_PAL);

    /* One VSync(2) field pair at 50 Hz is 12 units of 1/300 s. */
    CHECK(q2_screen_tick_dt(&s, 2.0 / 50.0) == Q2_SCREEN_DT_NOMINAL,
          "a nominal frame should be %d", Q2_SCREEN_DT_NOMINAL);

    /* 0x800184B8 clamps rather than averages, so a stalled frame is lost time
     * and never a lurch. */
    CHECK(q2_screen_tick_dt(&s, 1.0) == Q2_SCREEN_DT_MAX,
          "a one-second frame should clamp to %d", Q2_SCREEN_DT_MAX);
    CHECK(q2_screen_tick_dt(&s, 30.0 / 300.0) == 30, "exactly 30 is not clamped");
    CHECK(q2_screen_tick_dt(&s, 0.0) == 1, "a zero frame still advances");

    q2_screen_free(&s);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    test_bringup();
    test_swap();
    test_ot_slices();
    test_ot_window();
    test_layouts();
    test_compose_clip();
    test_background();
    test_shake();
    test_dt_clamp();

    if (g_fail == 0)
        printf("test_screen: all checks passed\n");
    else
        printf("test_screen: %d check%s failed\n", g_fail, g_fail == 1 ? "" : "s");

    return g_fail ? 1 : 0;
}
