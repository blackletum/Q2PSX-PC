/*
 * test_modelfade.c — DrawModels' two things that are not the model's own faces.
 *
 *   1. THE FADE. An object whose entity+0xFC is not 4096 (0x8006A874) is drawn
 *      additive over a subtracted silhouette of itself: PolyOpaqueSetup
 *      (0x8006DD68) turns each GT4 to code 0x3E / ABR 1 and gives it a POLY_FT4
 *      companion at ABR 2 with palette 73's CLUT; FilterInOpaquePolys
 *      (0x8006A5E0) colours the companions (a*133>>12, a*129>>12, a*133>>12);
 *      0x800B1D94 copies the XYs across and links each companion in front of
 *      its GT4 so it draws first.
 *   2. THE SURFACE BINDING. CopyFromGlobalTexture (0x80065138) overwrites a
 *      scene fragment's packets with the broken polygon's UVs, CLUT and page.
 *
 * Every expected number is derived from the disassembly, not from the port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modeldraw.h"
#include "surface.h"

static int g_failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        g_failures++;
    } else {
        printf("  ok:   %s\n", what);
    }
}

static void check_eq_i(long long got, long long want, const char *what)
{
    if (got != want) {
        printf("  FAIL: %s (got %lld, want %lld)\n", what, got, want);
        g_failures++;
    } else {
        printf("  ok:   %s\n", what);
    }
}

static void put_u16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/*
 * One part, four storage vertices, one face wound however it likes: the part's
 * force-draw mask (bit 31, the first face) bypasses the NCLIP pair, so the
 * face is emitted regardless of which way the camera sees it.
 */
#define OFS_VERTS 64
#define OFS_PARTS 112
#define OFS_FACES 128
#define IMAGE_LEN (OFS_FACES + 16)

static void build_model(u8 *bytes, q2_model *model, u8 face_flags, u8 face_tex)
{
    static const s16 xy[4][2] = {{-100,-100},{100,-100},{100,100},{-100,100}};
    int i;

    memset(bytes, 0, IMAGE_LEN);
    memset(model, 0, sizeof(*model));
    model->base = bytes;
    model->size = IMAGE_LEN;
    model->hdr.num_verts = 4;
    model->hdr.num_parts = 1;
    model->hdr.num_faces = 1;
    model->hdr.ofs_verts = OFS_VERTS;
    model->hdr.ofs_parts = OFS_PARTS;
    model->hdr.ofs_faces = OFS_FACES;
    model->scratch_size  = 4;

    for (i = 0; i < 4; i++) {
        put_u16(bytes + OFS_VERTS + i * 12 + 0, (u16)xy[i][0]);
        put_u16(bytes + OFS_VERTS + i * 12 + 2, (u16)xy[i][1]);
    }
    put_u16(bytes + OFS_PARTS, 1);          /* one face            */
    bytes[OFS_PARTS + 2] = 0;               /* scratch base        */
    bytes[OFS_PARTS + 3] = 4;               /* four vertices       */
    put_u32(bytes + OFS_PARTS + 4, 0x80000000u); /* force face 0   */

    bytes[OFS_FACES + 0] = 0;
    bytes[OFS_FACES + 1] = 1;
    bytes[OFS_FACES + 2] = 2;
    bytes[OFS_FACES + 3] = 3;
    for (i = 0; i < 4; i++) {
        bytes[OFS_FACES + 4 + i * 2 + 0] = (u8)(10 * (i + 1));
        bytes[OFS_FACES + 4 + i * 2 + 1] = (u8)(10 * (i + 1) + 1);
    }
    bytes[OFS_FACES + 12] = face_flags;
    bytes[OFS_FACES + 13] = face_tex;
}

static void setup_instance(q2_model_instance *inst, const q2_model *model)
{
    q2_model_instance_init(inst);
    inst->model = model;
    inst->origin[2] = 2000;
    inst->bucket_override = 5;              /* an absolute, known bucket */
}

/* The chain of bucket `b`, head first, as the backend draws it. */
static u32 chain(const psx_ot *ot, u32 b, const psx_prim **out, u32 cap)
{
    s32 at = ot->bucket_head[b];
    u32 n = 0;

    while (at >= 0 && n < cap) {
        out[n++] = &ot->prims[at];
        at = ot->next[at];
    }
    return n;
}

static void test_fade(void)
{
    u8 bytes[IMAGE_LEN];
    q2_model model;
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    q2_tpage_table table;
    const psx_prim *order[4];
    const psx_prim *gt4 = NULL, *ft4 = NULL;
    u16 page_word;
    int i;

    printf("\nthe fade: 0x8006A874, 0x8006DD68, 0x8006A5E0, 0x800B1D94\n");

    build_model(bytes, &model, 3 /* page 3, selector 0 */, 7);
    q2_tpage_table_init(&table);
    page_word = table.word[3];

    q2_camera_default(&cam, 512, 248);
    gte_init(&gte);
    gte_set_projection(&gte, cam.projection, 256, 124);
    if (psx_ot_init(&ot, 256, 8) != Q2_OK) {
        check(false, "the ordering table allocates");
        return;
    }

    /* Opaque: 4096 is the one value that draws the model as authored. */
    setup_instance(&inst, &model);
    inst.fade_clut = psx_make_clut(0, 240);
    psx_ot_clear(&ot);
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 1,
               "at +0xFC == 4096 one face is one primitive");
    check_eq_i(stats.fades_emitted, 0, "and no silhouette");
    check(!ot.prims[0].semi_transparent, "drawn opaque, selector 0");
    check_eq_i(ot.prims[0].tpage, q2_tpage_model(&table, 3, 0),
               "with the page table's word and the face's own selector");

    /* Fading: alpha 2048. */
    inst.alpha = 2048;
    psx_ot_clear(&ot);
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 2,
               "at +0xFC != 4096 the face is two primitives");
    check_eq_i(stats.faces_emitted, 1, "one of them is the face");
    check_eq_i(stats.fades_emitted, 1, "the other its silhouette");
    for (i = 0; i < (int)ot.prim_count; i++) {
        if (ot.prims[i].kind == PSX_PRIM_GT4) gt4 = &ot.prims[i];
        if (ot.prims[i].kind == PSX_PRIM_FT4) ft4 = &ot.prims[i];
    }
    check(gt4 && ft4, "a POLY_GT4 and a POLY_FT4 (tag 0x09000000)");
    if (!gt4 || !ft4) {
        psx_ot_free(&ot);
        return;
    }

    /* PolyOpaqueSetup on the face's own packet, 0x8006DDEC..0x8006DE04. */
    check(gt4->semi_transparent, "GT4 code becomes 0x3E (ABE)");
    check_eq_i(gt4->tpage, (page_word & 0x1F) | 0x20,
               "GT4 tpage = (tpage & 0x1F) | 0x20: ABR 1, B + F");
    check(gt4->textured_blend, "GT4 stays modulated");

    /* The companion, 0x8006DD68..0x8006DDE8. */
    for (i = 0; i < 4; i++) {
        check_eq_i(ft4->uv[i].u, gt4->uv[i].u, "companion copies the GT4's u");
        check_eq_i(ft4->uv[i].v, gt4->uv[i].v, "companion copies the GT4's v");
    }
    check_eq_i(ft4->tpage, (page_word & 0x1F) | 0x40,
               "companion tpage = (tpage & 0x1F) | 0x40: ABR 2, B - F");
    check_eq_i(ft4->clut, psx_make_clut(0, 240),
               "companion CLUT is palette 73's (0x800E3FBE)");
    check(ft4->semi_transparent, "companion code is 0x2E (ABE)");
    check(ft4->textured_blend, "companion modulates its texture");

    /* FilterInOpaquePolys' colour, 0x8006A5E8..0x8006A640, at alpha 2048:
     * 2048*129 = 264192 >> 12 = 64; 2048*133 = 272384 >> 12 = 66. */
    check_eq_i(ft4->rgb[0].r, 66, "red   = (alpha * 133) >> 12");
    check_eq_i(ft4->rgb[0].g, 64, "green = (alpha * 129) >> 12");
    check_eq_i(ft4->rgb[0].b, 66, "blue  = (alpha * 133) >> 12");

    /* 0x800B1D94: the GT4's four XYs go into the companion. */
    for (i = 0; i < 4; i++) {
        check_eq_i(ft4->xy[i].x, gt4->xy[i].x, "companion x matches");
        check_eq_i(ft4->xy[i].y, gt4->xy[i].y, "companion y matches");
    }

    /* And it is spliced in FRONT of the GT4 in the chain, so it draws first:
     * `swl t4, 2(v1)` links FT4 -> GT4, `swl t4, 2(a0)` links prev -> FT4. */
    check_eq_i(chain(&ot, 5, order, 4), 2, "the bucket holds the pair");
    check(order[0] == ft4, "the silhouette is drawn first");
    check(order[1] == gt4, "then the additive face over it");

    /* Alpha near the ends of the ramp. 4095: 4095*133 = 544635 >> 12 = 132,
     * 4095*129 = 528255 >> 12 = 128. 1: both zero. */
    inst.alpha = 4095;
    psx_ot_clear(&ot);
    q2_model_build_ot(&inst, &cam, &ot, &gte, &stats);
    for (i = 0; i < (int)ot.prim_count; i++)
        if (ot.prims[i].kind == PSX_PRIM_FT4) ft4 = &ot.prims[i];
    check_eq_i(ft4->rgb[0].r, 132, "alpha 4095: red 132");
    check_eq_i(ft4->rgb[0].g, 128, "alpha 4095: green 128");
    inst.alpha = 1;
    psx_ot_clear(&ot);
    q2_model_build_ot(&inst, &cam, &ot, &gte, &stats);
    for (i = 0; i < (int)ot.prim_count; i++)
        if (ot.prims[i].kind == PSX_PRIM_FT4) ft4 = &ot.prims[i];
    check_eq_i(ft4->rgb[0].r, 0, "alpha 1: the silhouette is black");

    /* No CLUT table to hand (an offline caller): drawn as if opaque. */
    inst.alpha = 2048;
    inst.fade_clut = 0;
    psx_ot_clear(&ot);
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 1,
               "without palette 73 the fade is not attempted");
    check(!ot.prims[0].semi_transparent, "and the face keeps its own code");

    psx_ot_free(&ot);
}

static void test_face_texture(void)
{
    u8 bytes[IMAGE_LEN];
    q2_model model;
    q2_model_instance inst;
    q2_model_draw_stats stats;
    q2_model_face_texture tex;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    const psx_prim *p;
    int i;

    printf("\nthe surface binding: 0x80065138 over every face\n");

    build_model(bytes, &model, 3, 7);
    q2_camera_default(&cam, 512, 248);
    gte_init(&gte);
    gte_set_projection(&gte, cam.projection, 256, 124);
    if (psx_ot_init(&ot, 256, 8) != Q2_OK) {
        check(false, "the ordering table allocates");
        return;
    }

    for (i = 0; i < 4; i++) {
        tex.uv[i][0] = (u8)(200 + i * 2);
        tex.uv[i][1] = (u8)(201 + i * 2);
    }
    tex.clut  = psx_make_clut(16, 300);
    tex.tpage = 0x0049;
    tex.semi  = true;

    setup_instance(&inst, &model);
    inst.face_tex = &tex;
    psx_ot_clear(&ot);
    check_eq_i(q2_model_build_ot(&inst, &cam, &ot, &gte, &stats), 1,
               "the bound face is still one primitive");
    p = &ot.prims[0];
    for (i = 0; i < 4; i++) {
        check_eq_i(p->uv[i].u, 200 + i * 2, "uv.u is the binding's (+0xC/+0x18/+0x24/+0x30)");
        check_eq_i(p->uv[i].v, 201 + i * 2, "uv.v is the binding's");
    }
    check_eq_i(p->clut, psx_make_clut(16, 300), "the CLUT is the binding's (+0xE)");
    check_eq_i(p->tpage, 0x0049, "the page is the binding's (+0x1A)");
    check(p->semi_transparent, "the code byte is the binding's (0x3E)");

    tex.semi = false;
    psx_ot_clear(&ot);
    q2_model_build_ot(&inst, &cam, &ot, &gte, &stats);
    check(!ot.prims[0].semi_transparent, "an opaque binding draws opaque (0x3C)");

    inst.face_tex = NULL;
    psx_ot_clear(&ot);
    q2_model_build_ot(&inst, &cam, &ot, &gte, &stats);
    check_eq_i(ot.prims[0].uv[0].u, 10, "without a binding the face's own UVs return");

    psx_ot_free(&ot);
}

int main(void)
{
    test_fade();
    test_face_texture();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
