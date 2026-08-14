/*
 * test_surface.c — surface flags, blend selection, and the SortData stream.
 *
 * The disc-wide census lives in `q2psx-inspect surfaces`. This pins the parts
 * no census can reach: the exact contents of the two blend tables, the four
 * fields packed into Scene.flags08, the subdivision policy's decision table,
 * and the SortData bit reader — which is tested by encoding a stream with an
 * independent encoder written from the same specification and decoding it back,
 * including the cases the disc may or may not contain (a field straddling a
 * word boundary, a multi-word skip, both encoding modes).
 */
#include <stdio.h>
#include <string.h>

#include "sortdata.h"
#include "surface.h"

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

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* The two tables, exactly as the executable holds them                       */
/* ------------------------------------------------------------------------- */
static void test_tables(void)
{
    static const u8  want_code[8]  = { 0, 2, 2, 2, 2, 0, 0, 0 };
    static const u16 want_blend[8] = { 32, 0, 32, 64, 96, 0, 0, 0 };
    int i;

    puts("codeTable 0x800AE614 / blendTable 0x800B36D8");

    for (i = 0; i < 8; i++) {
        check_eq(q2_surf_code_table[i],  want_code[i],  "codeTable entry");
        check_eq(q2_surf_blend_table[i], want_blend[i], "blendTable entry");
    }

    /* Selector 0 is opaque and 1..4 are the four hardware modes in order. This
     * is the whole of transparency on both the world and the model paths. */
    check(!q2_surf_selector_semi(0), "selector 0 is opaque");
    check(q2_surf_selector_semi(1),  "selector 1 is transparent");
    check(q2_surf_selector_semi(2),  "selector 2 is transparent");
    check(q2_surf_selector_semi(3),  "selector 3 is transparent");
    check(q2_surf_selector_semi(4),  "selector 4 is transparent");
    check(!q2_surf_selector_semi(5), "selector 5 is opaque");
    check(!q2_surf_selector_semi(6), "selector 6 is opaque");
    check(!q2_surf_selector_semi(7), "selector 7 is opaque");

    check_eq(q2_surf_selector_blend(1), PSX_BLEND_HALF,    "selector 1 is B/2+F/2");
    check_eq(q2_surf_selector_blend(2), PSX_BLEND_ADD,     "selector 2 is B+F");
    check_eq(q2_surf_selector_blend(3), PSX_BLEND_SUB,     "selector 3 is B-F");
    check_eq(q2_surf_selector_blend(4), PSX_BLEND_QUARTER, "selector 4 is B+F/4");

    /* An out-of-range selector must not index off the end. The engine masks to
     * three bits before it gets here, so this can only be a port's own bug. */
    check(!q2_surf_selector_semi(8),   "selector 8 is treated as opaque");
    check(!q2_surf_selector_semi(999), "a wild selector is treated as opaque");
}

/* ------------------------------------------------------------------------- */
/* The texture-page table and its write-back                                  */
/* ------------------------------------------------------------------------- */
static void test_tpage(void)
{
    q2_tpage_table t;
    u16 before, after, semi_before, semi_after;

    puts("texture-page table (0x80077FE8) and the opaque write-back (0x80068320)");

    q2_tpage_table_init(&t);

    /* GetTPage(0, 0, 64*(i+1), 256): 4bpp, page cell i+1, y 256, ABR 0. */
    check_eq(q2_tpage_blend(t.word[0]), PSX_BLEND_HALF, "a fresh page is ABR 0");
    check_eq(t.word[0] & 0x0F, 1, "page 0 lives in cell 1");
    check_eq((t.word[0] >> 4) & 1, 1, "pages sit on the second cell row");
    check_eq((t.word[0] >> 7) & 3, PSX_TEX_4BIT, "pages are 4bpp");

    /*
     * The behaviour that makes world transparency additive, and the reason the
     * table is state rather than a constant: a transparent polygon on a page no
     * opaque polygon has touched yet blends at half, and the same polygon
     * blends additively once one has.
     */
    semi_before = q2_tpage_world_semi(&t, 3);
    check_eq(q2_tpage_blend(semi_before), PSX_BLEND_HALF,
             "transparent-before-opaque is B/2+F/2");

    before = t.word[3];
    after  = q2_tpage_world_opaque(&t, 3);
    check(after != before, "the opaque path changes the entry");
    check_eq(q2_tpage_blend(after), PSX_BLEND_ADD, "an opaque page becomes ABR 1");
    check_eq(t.word[3], after, "and the change is written back");

    semi_after = q2_tpage_world_semi(&t, 3);
    check_eq(q2_tpage_blend(semi_after), PSX_BLEND_ADD,
             "transparent-after-opaque is B+F");

    /* Only the page that was drawn is promoted. */
    check_eq(q2_tpage_blend(q2_tpage_world_semi(&t, 4)), PSX_BLEND_HALF,
             "a neighbouring page is untouched");

    /* The model path ORs its own selector on and never writes back. */
    {
        u16 saved = t.word[7];
        u16 m     = q2_tpage_model(&t, 7, 3);

        check_eq(q2_tpage_blend(m), PSX_BLEND_SUB, "a model selector picks the mode");
        check_eq(t.word[7], saved, "the model path does not write the table back");
    }
}

/* ------------------------------------------------------------------------- */
/* Scene.flags08                                                              */
/* ------------------------------------------------------------------------- */
static void test_flags(void)
{
    static const u16 on_disc[] = {
        0x0000, 0x0400, 0x0800, 0x1000, 0x1400, 0x4000, 0x4400, 0x4800
    };
    size_t i;

    puts("Scene.flags08 (0x80067714, 0x80066524, 0x80066740)");

    /* Bits 0-9 are the runtime object slot, 1-based, and are zero on disc. */
    check_eq(q2_scene_flags_object(0x0000), -1, "no object when the field is 0");
    check_eq(q2_scene_flags_object(0x0001),  0, "field 1 is slot 0");
    check_eq(q2_scene_flags_object(0x0030), 47, "field 48 is slot 47");
    check_eq(q2_scene_flags_object(0x4C01),  0, "the other fields do not leak in");

    check(q2_scene_flags_nodraw(0x8000),  "bit 15 hides the node");
    check(!q2_scene_flags_nodraw(0x7FFF), "and nothing else does");
    check(q2_scene_flags_deferred(0x4000),  "bit 14 selects the deferred path");
    check(!q2_scene_flags_deferred(0xBFFF), "and nothing else does");

    check_eq(q2_scene_flags_variant(0x0000), Q2_SURF_VARIANT_SUBDIVIDE, "variant 0");
    check_eq(q2_scene_flags_variant(0x0400), Q2_SURF_VARIANT_FLAT,      "variant 1");
    check_eq(q2_scene_flags_variant(0x0800), Q2_SURF_VARIANT_TAGGED,    "variant 2");
    check_eq(q2_scene_flags_variant(0x0C00), Q2_SURF_VARIANT_HIDDEN,    "variant 3");

    /* Bit 12 is authored on disc and nothing reads it, so it must not disturb
     * the two bits that are read. 0x1000 and 0x1400 are both real values. */
    check_eq(q2_scene_flags_variant(0x1000), Q2_SURF_VARIANT_SUBDIVIDE,
             "bit 12 alone still selects variant 0");
    check_eq(q2_scene_flags_variant(0x1400), Q2_SURF_VARIANT_FLAT,
             "bit 12 with bit 10 still selects variant 1");

    /* Every value the disc actually carries decodes to a drawable node bound to
     * no object — which is what makes the low ten bits' emptiness on disc a
     * statement about the format rather than about one map. */
    for (i = 0; i < sizeof(on_disc) / sizeof(on_disc[0]); i++) {
        u16 f = on_disc[i];
        check(!q2_scene_flags_nodraw(f), "an on-disc value is never hidden");
        check_eq(q2_scene_flags_object(f), -1, "an on-disc value binds no object");
        check(q2_scene_flags_variant(f) != Q2_SURF_VARIANT_HIDDEN,
              "an on-disc value never selects variant 3");
    }

    /* SETWIBBLE replaces bits 10-13 and touches nothing else (0x8002E7CC). */
    check_eq(q2_scene_flags_set_wibble(0xC3FF, 0xF), 0xFFFF, "SETWIBBLE sets its field");
    check_eq(q2_scene_flags_set_wibble(0xFFFF, 0x0), 0xC3FF, "SETWIBBLE clears its field");
    check_eq(q2_scene_flags_set_wibble(0x0021, 0x3), 0x0C21, "SETWIBBLE preserves the slot");
    /* Values above 3 alias, because the renderer masks to two bits. */
    check_eq(q2_scene_flags_variant((u16)q2_scene_flags_set_wibble(0, 4)),
             q2_scene_flags_variant((u16)q2_scene_flags_set_wibble(0, 0)),
             "SETWIBBLE 4 draws as SETWIBBLE 0");
}

/* ------------------------------------------------------------------------- */
/* The subdivision policy                                                     */
/* ------------------------------------------------------------------------- */
static void test_subdivision(void)
{
    const s32 threshold = 6400;
    const u16 tagged    = 0x0004;   /* a bit inside 0x3C */
    const u16 untagged  = 0x0003;   /* only the semi bits */

    puts("subdivision policy (0x80066740, 0x800AFBCC, 0x800698A0)");

    /* Variant 0 subdivides anything nearer than the threshold. */
    check(q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                   100, threshold, Q2_SURF_POOL_FREE),
          "variant 0 subdivides a near quad");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                    threshold, threshold, Q2_SURF_POOL_FREE),
          "the threshold is exclusive");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                    99999, threshold, Q2_SURF_POOL_FREE),
          "variant 0 leaves a far quad alone");

    /* Variant 1 never subdivides — it is the one that has no callback at all. */
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_FLAT, tagged,
                                    1, threshold, Q2_SURF_POOL_FREE),
          "variant 1 never subdivides");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_HIDDEN, tagged,
                                    1, threshold, Q2_SURF_POOL_FREE),
          "variant 3 never subdivides");

    /* Variant 2 needs the polygon's own permission — the clut low-byte bits
     * that were documented as residue. */
    check(q2_surf_should_subdivide(Q2_SURF_VARIANT_TAGGED, tagged,
                                   100, threshold, Q2_SURF_POOL_FREE),
          "variant 2 subdivides a tagged polygon");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_TAGGED, untagged,
                                    100, threshold, Q2_SURF_POOL_FREE),
          "variant 2 leaves an untagged polygon flat");
    check(q2_surf_poly_may_subdivide(0x0020), "bit 5 grants permission");
    check(!q2_surf_poly_may_subdivide(0x0043), "bits 0-1 and 6 do not");

    /* Pool pressure tightens then closes the gate. */
    check(q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                   threshold / 4, threshold, Q2_SURF_POOL_TIGHT),
          "a very near quad still subdivides under pressure");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                    threshold / 2, threshold, Q2_SURF_POOL_TIGHT),
          "a merely near quad does not");
    check(!q2_surf_should_subdivide(Q2_SURF_VARIANT_SUBDIVIDE, untagged,
                                    1, threshold, Q2_SURF_POOL_FULL),
          "a full pool subdivides nothing");
}

/* ------------------------------------------------------------------------- */
/* SortData: an independent encoder, then the real decoder                    */
/* ------------------------------------------------------------------------- */
typedef struct bitwriter {
    u8  buf[512];
    u32 bit;
} bitwriter;

/* LSB-first within little-endian 32-bit words — the mirror of read_bits. */
static void put_bits(bitwriter *w, u32 value, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u32 b     = (value >> i) & 1u;
        u32 pos   = w->bit + i;
        u32 word  = pos >> 5;
        u32 shift = pos & 31;
        u32 byte  = word * 4u + (shift >> 3);

        if (b)
            w->buf[byte] |= (u8)(1u << (shift & 7));
    }
    w->bit += n;
}

static void test_sortdata(void)
{
    bitwriter w;
    q2_sortdata sd;
    q2_sort_reader r;
    q2_sort_item it;

    /* Deliberately awkward widths: a 9-bit opcode guarantees fields straddle
     * word boundaries, which is the case a naive reader gets wrong. */
    const u32 w_base = 12, w_op_short = 5, w_op_long = 9;
    const u32 w_f1 = 7, w_f3 = 4, w_f4 = 6, w_f2 = 11;
    const u32 base = 1000;

    puts("SortData stream (0x80066B70 - 0x800676D8)");

    memset(&w, 0, sizeof(w));

    /* Header: seven widths, each stored as width - 1, then the base. */
    put_bits(&w, w_base     - 1, 4);
    put_bits(&w, w_op_short - 1, 3);
    put_bits(&w, w_op_long  - 1, 4);
    put_bits(&w, w_f1       - 1, 3);
    put_bits(&w, w_f3       - 1, 3);
    put_bits(&w, w_f4       - 1, 3);
    put_bits(&w, w_f2       - 1, 4);
    put_bits(&w, base,           w_base);

    /* Windowed node: index = op - 3 + base. */
    put_bits(&w, 3 + 5, w_op_short);          /* node base + 5 */

    /* An entity whose payload is declined and must be skipped. Make the payload
     * long enough to cross more than one word, which the engine's own skip
     * handles with a shift rather than a loop. */
    put_bits(&w, 1, w_op_short);
    put_bits(&w, 125, w_f1);
    put_bits(&w, 70, w_f2);                                /* payload bits */
    put_bits(&w, 9,  w_f3);
    put_bits(&w, 33, w_f4);
    put_bits(&w, 0xABCD, 32);                              /* the payload,   */
    put_bits(&w, 0xEF12, 32);                              /* 70 bits of it  */
    put_bits(&w, 0x3, 6);

    /* Another windowed node, to prove the skip landed exactly. */
    put_bits(&w, 3 + 7, w_op_short);

    /* Switch to absolute mode: opcode 2, then the replacement at the LONG
     * width, and no base is added. */
    put_bits(&w, 2, w_op_short);
    put_bits(&w, 3 + 300, w_op_long);

    /* And back again. */
    put_bits(&w, 2, w_op_long);
    put_bits(&w, 3 + 1, w_op_short);

    put_bits(&w, 0, w_op_short);              /* end */

    sd.data = w.buf;
    sd.size = sizeof(w.buf);

    check(q2_sort_begin(&r, &sd, 0, Q2_SORT_BUCKET_START), "the header decodes");
    check_eq(r.hdr.w_base,     (s64)w_base,     "w_base");
    check_eq(r.hdr.w_op_short, (s64)w_op_short, "w_op_short");
    check_eq(r.hdr.w_op_long,  (s64)w_op_long,  "w_op_long");
    check_eq(r.hdr.w_f1,       (s64)w_f1,       "w_f1");
    check_eq(r.hdr.w_f2,       (s64)w_f2,       "w_f2");
    check_eq(r.hdr.w_f3,       (s64)w_f3,       "w_f3");
    check_eq(r.hdr.w_f4,       (s64)w_f4,       "w_f4");
    check_eq(r.hdr.base,       (s64)base,       "base");

    check(q2_sort_next(&r, &it), "first item");
    check_eq(it.kind, Q2_SORT_NODE, "it is a node");
    check_eq(it.node, base + 5, "the base is added in windowed mode");
    check_eq(it.bucket, Q2_SORT_BUCKET_START, "nodes take the current bucket");

    check(q2_sort_next(&r, &it), "second item");
    check_eq(it.kind, Q2_SORT_ENTITY, "it is an entity");
    /*
     * f1 is sign-extended from BIT 15, not from the field width: 0x800674B4 is
     * `sll a0, s2, 16; sra a0, a0, 16`. So a 7-bit field can never be negative,
     * however its top bit is set — a decoder that sign-extends from w_f1 would
     * turn 125 into -3 here and hand the entity draw the wrong argument.
     */
    check_eq(it.f1, 125, "f1 is not sign-extended from the field width");
    check_eq(it.f2, 70, "f2 is the payload length in bits");
    check_eq(it.f3, 9,  "f3");
    check_eq(it.f4, 33, "f4");
    check_eq(it.bucket, Q2_SORT_BUCKET_START, "the entity shares the bucket");

    /* Nothing may be pulled while an entity is unresolved. */
    check(!q2_sort_next(&r, &it), "the stream blocks until the entity resolves");

    q2_sort_entity_resolve(&r, false);   /* not drawn: skip the payload */

    check(q2_sort_next(&r, &it), "third item");
    check_eq(it.kind, Q2_SORT_NODE, "the skip landed on a node");
    check_eq(it.node, base + 7, "and on the right one");
    check_eq(it.bucket, Q2_SORT_BUCKET_START - 1,
             "the bucket stepped down once, at the entity");

    check(q2_sort_next(&r, &it), "fourth item");
    check_eq(it.kind, Q2_SORT_NODE, "mode switch yields a node");
    check_eq(it.node, 300, "absolute mode does not add the base");

    check(q2_sort_next(&r, &it), "fifth item");
    check_eq(it.kind, Q2_SORT_NODE, "switching back yields a node");
    check_eq(it.node, base + 1, "windowed mode adds the base again");

    check(!q2_sort_next(&r, &it), "the stream ends");
    check(r.ended, "and says so");
    check(!r.overrun, "without running off the chunk");

    /* The other resolution: a drawn entity with a non-zero f2 supplies a new
     * base instead of skipping. */
    {
        bitwriter w2;
        q2_sortdata sd2;
        q2_sort_reader r2;

        memset(&w2, 0, sizeof(w2));
        put_bits(&w2, w_base - 1, 4);
        put_bits(&w2, w_op_short - 1, 3);
        put_bits(&w2, w_op_long - 1, 4);
        put_bits(&w2, w_f1 - 1, 3);
        put_bits(&w2, w_f3 - 1, 3);
        put_bits(&w2, w_f4 - 1, 3);
        put_bits(&w2, w_f2 - 1, 4);
        put_bits(&w2, base, w_base);

        put_bits(&w2, 1, w_op_short);
        put_bits(&w2, 2, w_f1);
        put_bits(&w2, 1, w_f2);       /* non-zero: a new base follows */
        put_bits(&w2, 0, w_f3);
        put_bits(&w2, 0, w_f4);
        put_bits(&w2, 77, w_base);    /* the new base */
        put_bits(&w2, 3 + 4, w_op_short);
        put_bits(&w2, 0, w_op_short);

        sd2.data = w2.buf;
        sd2.size = sizeof(w2.buf);

        check(q2_sort_begin(&r2, &sd2, 0, Q2_SORT_BUCKET_START), "second header");
        check(q2_sort_next(&r2, &it), "entity item");
        check_eq(it.kind, Q2_SORT_ENTITY, "it is an entity");
        q2_sort_entity_resolve(&r2, true);    /* drawn: read a new base */
        check(q2_sort_next(&r2, &it), "node after the base update");
        check_eq(it.node, 77 + 4, "the new base is in force");
    }

    /* A byte offset that is not word-aligned is a real case: the viewport's
     * record carries a byte offset and the reader consumes the remainder as
     * bits rather than padding. */
    {
        bitwriter w3;
        q2_sortdata sd3;
        q2_sort_reader r3;

        memset(&w3, 0, sizeof(w3));
        put_bits(&w3, 0, 24);          /* three bytes of filler */
        put_bits(&w3, w_base - 1, 4);
        put_bits(&w3, w_op_short - 1, 3);
        put_bits(&w3, w_op_long - 1, 4);
        put_bits(&w3, w_f1 - 1, 3);
        put_bits(&w3, w_f3 - 1, 3);
        put_bits(&w3, w_f4 - 1, 3);
        put_bits(&w3, w_f2 - 1, 4);
        put_bits(&w3, base, w_base);
        put_bits(&w3, 3 + 2, w_op_short);
        put_bits(&w3, 0, w_op_short);

        sd3.data = w3.buf;
        sd3.size = sizeof(w3.buf);

        check(q2_sort_begin(&r3, &sd3, 3, Q2_SORT_BUCKET_START),
              "an unaligned start decodes");
        check(q2_sort_next(&r3, &it), "and yields its node");
        check_eq(it.node, base + 2, "at the right index");
    }

    /* A truncated chunk must report an overrun rather than read past it. */
    {
        q2_sortdata tiny;
        q2_sort_reader rt;

        tiny.data = w.buf;
        tiny.size = 8;

        if (q2_sort_begin(&rt, &tiny, 0, Q2_SORT_BUCKET_START)) {
            while (q2_sort_next(&rt, &it)) {
                if (it.kind == Q2_SORT_ENTITY)
                    q2_sort_entity_resolve(&rt, false);
            }
        }
        check(rt.overrun || rt.ended, "a truncated chunk stops rather than reads on");
    }
}

int main(void)
{
    puts("surface flags, blend modes and draw order");
    puts("=========================================");

    test_tables();
    test_tpage();
    test_flags();
    test_subdivision();
    test_sortdata();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
