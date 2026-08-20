/*
 * test_raster.c — which diagonal a quad is cut along, and why it is not free.
 *
 * The backend holds a quad's four corners in the FILE's perimeter order. The
 * console does not: both of its linkers convert perimeter to the hardware's Z
 * order on the way into the packet, and they do it by exchanging corners 2 and
 * 3 — vertices and UVs together, so the pairing survives.
 *
 *     model linker  0x800B2410  SXY0,SXY1,SXY2 <= file v0,v1,v3, then SXY0 <=
 *                              file v2 for the second NCLIP; stored at packet
 *                              +8,+20,+32,+44. The shift counts on the packed
 *                              index word are 3, 5, 21, 13 — that is 0,1,3,2.
 *     world linker  0x800AF844  the identical four shifts, same order.
 *     model emitter 0x8006A3CC..0x8006A3F8  POLY uv0..uv3 <= file uv0,uv1,uv3,
 *                              uv2, stored to +12,+24,+36,+48.
 *
 * The GPU then splits a four-point polygon on the edge between its SECOND and
 * THIRD corners. Undo the conversion and that edge, in perimeter indices, is
 * 1—3: the two triangles are (0,1,3) and (1,2,3), NOT the fan (0,1,2)+(0,2,3).
 *
 * Both cover a convex quad, so nothing about a wall's silhouette catches this.
 * What it changes is everything that interpolates: UVs are affine per triangle,
 * so the diagonal decides which way a texture creases, and a folded quad's
 * COVERAGE differs outright. Those are the two things pinned here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "raster.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static u16 pixel_at(const psx_framebuffer *fb, int x, int y)
{
    return fb->px[(size_t)y * (size_t)fb->width + (size_t)x];
}

/* Every option off but the one under test: no dither, no blending, so a
 * sampled texel reaches the buffer unchanged and a comparison is exact. */
static void opts_exact(psx_raster_opts *o)
{
    psx_raster_opts_default(o);
    o->dither            = false;
    o->semi_transparency = false;
}

/* ------------------------------------------------------------------------- */
/* 1. The affine warp — which pair of corners the interpolation is exact on   */
/* ------------------------------------------------------------------------- */
/*
 * A 64x64 square, perimeter order, carrying UVs that are EQUAL on the opposite
 * pairs: corners 0 and 2 both sample u=0, corners 1 and 3 both sample u=2.
 *
 * Along whichever diagonal the quad is actually split, both triangles share
 * that edge and therefore agree exactly along it — so the texel on the shared
 * diagonal is the one its two endpoints name, with no interpolation left in it.
 *
 * Probe (48,16), which lies on the 1—3 diagonal (x + y == 64):
 *
 *     split 1—3 (the hardware's)  u interpolates 2 -> 2, so u = 2  GREEN
 *     split 0—2 (a fan)           (48,16) falls in triangle (0,1,2) with
 *                                 weights 1/4, 1/2, 1/4 on u = 0, 2, 0,
 *                                 so u = 1                        BLUE
 *
 * Three distinct texels rather than two, so a failure says which rule ran
 * instead of only that the expected one did not.
 */
static void test_uv_diagonal(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram       *vram;
    psx_prim        prim;
    const u16       RED   = psx_rgb555(248, 0, 0);
    const u16       BLUE  = psx_rgb555(0, 0, 248);
    const u16       GREEN = psx_rgb555(0, 248, 0);
    int i;

    puts("\nthe affine warp runs along the hardware's diagonal");

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (!vram || psx_fb_init(&fb, 128, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        free(vram);
        return;
    }
    psx_fb_clear(&fb, 0);
    opts_exact(&opts);

    /* Three texels in a row of a 16-bit page: u = 0, 1, 2. */
    vram->px[0][0] = RED;
    vram->px[0][1] = BLUE;
    vram->px[0][2] = GREEN;

    memset(&prim, 0, sizeof(prim));
    prim.kind  = PSX_PRIM_GT4;
    prim.tpage = 2 << 7;          /* 16 bits per texel, page (0,0) */

    prim.xy[0].x = 0;   prim.xy[0].y = 0;
    prim.xy[1].x = 64;  prim.xy[1].y = 0;
    prim.xy[2].x = 64;  prim.xy[2].y = 64;
    prim.xy[3].x = 0;   prim.xy[3].y = 64;

    prim.uv[0].u = 0;   prim.uv[0].v = 0;
    prim.uv[1].u = 2;   prim.uv[1].v = 0;
    prim.uv[2].u = 0;   prim.uv[2].v = 0;
    prim.uv[3].u = 2;   prim.uv[3].v = 0;

    /* Modulation is (texel * rgb) >> 7, so 128 passes the texel through. */
    for (i = 0; i < 4; i++) {
        prim.rgb[i].r = prim.rgb[i].g = prim.rgb[i].b = 128;
    }
    prim.textured_blend = true;

    psx_raster_prim(&fb, &prim, vram, &opts);

    check(pixel_at(&fb, 48, 16) == GREEN,
          "a point on the 1-3 diagonal samples the texel that diagonal names");
    check(pixel_at(&fb, 48, 16) != BLUE,
          "...and not the one the 0-2 fan would have interpolated");

    /* The mirrored probe on the other diagonal must NOT be pinned to u=0: if
     * it were, the split would be 0—2 after all. (16,16) lies on y == x. */
    check(pixel_at(&fb, 16, 16) != RED,
          "a point on the 0-2 diagonal is interpolated, not pinned");

    /* The quad is still fully covered — the corners themselves prove the two
     * triangles between them left no gap. */
    check(pixel_at(&fb, 2, 2)   != 0, "the 0 corner is covered");
    check(pixel_at(&fb, 61, 2)  != 0, "the 1 corner is covered");
    check(pixel_at(&fb, 61, 61) != 0, "the 2 corner is covered");
    check(pixel_at(&fb, 2, 61)  != 0, "the 3 corner is covered");

    psx_fb_free(&fb);
    free(vram);
}

/* ------------------------------------------------------------------------- */
/* 2. Coverage — a folded quad, where the two rules disagree outright         */
/* ------------------------------------------------------------------------- */
/*
 * A dart: the perimeter walks (0,0) -> (64,24) -> (128,0) -> (64,96), so corner
 * 1 is REFLEX and the polygon has a notch bitten out of its top edge.
 *
 * The hardware's triangles (0,1,3) and (1,2,3) meet along the vertical segment
 * from (64,24) to (64,96) — inside the shape — and cover the dart and nothing
 * else. The fan's diagonal 0—2 runs along y = 0, which lies OUTSIDE the
 * polygon, so its two triangles overlap and between them fill the whole
 * triangle (0,0),(128,0),(64,96) — notch included.
 *
 * A quad that folds in screen space is not hypothetical here: the second NCLIP
 * in both linkers exists precisely because a quad can fold, and test_world
 * pins that behaviour.
 */
static void test_folded_quad_coverage(void)
{
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_prim        prim;
    int i;

    puts("\na folded quad covers the dart, not its convex hull");

    if (psx_fb_init(&fb, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        return;
    }
    psx_fb_clear(&fb, 0);
    opts_exact(&opts);

    memset(&prim, 0, sizeof(prim));
    prim.kind = PSX_PRIM_G4;

    prim.xy[0].x = 0;   prim.xy[0].y = 0;
    prim.xy[1].x = 64;  prim.xy[1].y = 24;
    prim.xy[2].x = 128; prim.xy[2].y = 0;
    prim.xy[3].x = 64;  prim.xy[3].y = 96;

    for (i = 0; i < 4; i++) {
        prim.rgb[i].r = prim.rgb[i].g = prim.rgb[i].b = 248;
    }

    psx_raster_prim(&fb, &prim, NULL, &opts);

    /* Inside the notch — above the V, below the hull's top edge. The fan draws
     * here; the hardware does not. */
    check(pixel_at(&fb, 64, 8)  == 0,
          "the notch above the reflex corner is left empty");
    check(pixel_at(&fb, 64, 16) == 0,
          "...all the way down to the reflex corner");

    /* Inside the dart proper — drawn under either rule, so this is the control
     * that says the primitive was rasterised at all. */
    check(pixel_at(&fb, 64, 60) != 0, "the body of the dart is drawn");
    check(pixel_at(&fb, 40, 40) != 0, "and its left half");
    check(pixel_at(&fb, 88, 40) != 0, "and its right half");

    psx_fb_free(&fb);
}

/* ------------------------------------------------------------------------- */
/* 3. The flare path still gets the console's own order                       */
/* ------------------------------------------------------------------------- */
/*
 * `quad_zorder` marks a packet that is ALREADY in hardware order because it was
 * built as a console packet (flare.c). Those must keep the literal
 * (0,1,2)+(1,3,2), and the fix to the perimeter rule must not have quietly
 * become one rule for both.
 *
 * The same dart in Z order is corners 0,1,3,2 — so feeding the perimeter dart's
 * corners through the exchange gives a packet that must rasterise IDENTICALLY
 * to the perimeter one above.
 */
static void test_zorder_unchanged(void)
{
    psx_framebuffer perimeter, zorder;
    psx_raster_opts opts;
    psx_prim        a, b;
    int i, x, y, differing = 0;

    puts("\na Z-order packet still describes the same shape");

    if (psx_fb_init(&perimeter, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        return;
    }
    if (psx_fb_init(&zorder, 160, 128) != Q2_OK) {
        printf("  FAIL  out of memory\n");
        g_failures++;
        psx_fb_free(&perimeter);
        return;
    }
    psx_fb_clear(&perimeter, 0);
    psx_fb_clear(&zorder, 0);
    opts_exact(&opts);

    memset(&a, 0, sizeof(a));
    a.kind = PSX_PRIM_G4;
    a.xy[0].x = 0;   a.xy[0].y = 0;
    a.xy[1].x = 64;  a.xy[1].y = 24;
    a.xy[2].x = 128; a.xy[2].y = 0;
    a.xy[3].x = 64;  a.xy[3].y = 96;
    for (i = 0; i < 4; i++) {
        a.rgb[i].r = a.rgb[i].g = a.rgb[i].b = 248;
    }

    /* The console's own conversion: corners 2 and 3 exchanged. */
    b = a;
    b.quad_zorder = true;
    b.xy[2] = a.xy[3];
    b.xy[3] = a.xy[2];
    b.rgb[2] = a.rgb[3];
    b.rgb[3] = a.rgb[2];

    psx_raster_prim(&perimeter, &a, NULL, &opts);
    psx_raster_prim(&zorder, &b, NULL, &opts);

    for (y = 0; y < perimeter.height; y++) {
        for (x = 0; x < perimeter.width; x++) {
            if (pixel_at(&perimeter, x, y) != pixel_at(&zorder, x, y))
                differing++;
        }
    }
    check(differing == 0,
          "the same quad in either convention rasterises to the same pixels");

    psx_fb_free(&perimeter);
    psx_fb_free(&zorder);
}

int main(void)
{
    puts("quad split");
    puts("==========");

    test_uv_diagonal();
    test_folded_quad_coverage();
    test_zorder_unchanged();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
