/*
 * model.h — the CastList chunk: the map's model / mesh bank.
 *
 * A CastList is a back-to-back chain of models. Walk it by reading a 64-byte
 * header at p and stepping to p + ofs_end; an ofs_end of zero marks the last
 * model, whose blocks run to the end of the chunk. Both COMMON.DAT and
 * ZONE*.DAT carry one.
 *
 * Verified over the whole disc: 1,723 models (965 in COMMON.DAT, 758 in zones),
 * 192,798 vertices, 138,290 faces, 553,160 face indices, 12,871 parts. Every
 * size identity below holds on all 1,723 with zero exceptions.
 *
 * ---------------------------------------------------------------------------
 * Header — 64 bytes, all sub-block offsets model-relative and sorted
 * ---------------------------------------------------------------------------
 *   0x00  u8     magic         always 6
 *   0x01  u8[3]  unk           24-bit LE, 261..333367, meaning unknown
 *   0x04  u32    always3       always 3, presumably a version
 *   0x08  char   name[12]      appears in this map's ModelNames on all 49 maps;
 *                              NOT unique — 11 maps contain duplicate names
 *   0x14  u16    num_faces
 *   0x16  u16    num_parts
 *   0x18  s16    ext0, ext1    unknown extents
 *   0x1C  s16    ext2, ext3    probable top/bottom of bounds; see below
 *   0x20  u32    ofs_faces     ofs_block_a - ofs_faces == 16 * num_faces
 *   0x24  u32    ofs_verts     ALWAYS 0x40; num_verts = (ofs_parts - 0x40) / 12
 *   0x28  u32    ofs_parts     ofs_faces - ofs_parts == 8 * num_parts
 *   0x2C  u32    ofs_block_a   8 x {u16 count; u16 offset; u32 0} directory
 *   0x30  u32    ofs_block_b   16 zero bytes on 821/965; larger on 144
 *   0x34  u32    ofs_block_c
 *   0x38  u32    ofs_block_d
 *   0x3C  u32    ofs_end       total model size; 0 == last model
 *
 * ext2 / ext3 are interesting for a reason that has nothing to do with bounds:
 * ext2 equals the raw vertex max-Y on 72.5% of single-part models and 50.9% of
 * multi-part STATIC ones, but on 0 of 399 ARTICULATED models. ext3 likewise:
 * 86.5% / 85.4% / 1.3%. That is one of the strongest pieces of evidence that
 * articulated model vertices are stored PART-LOCAL rather than model-local.
 *
 * ---------------------------------------------------------------------------
 * The vertex-index base — the thing that blocked this format for two passes
 * ---------------------------------------------------------------------------
 * Face vertex indices are u8 and were long thought to be either part-relative
 * or model-absolute. Both readings fail: part-relative leaves 31.5% of indices
 * out of range disc-wide and is unsolvable for 399 models, while model-absolute
 * is trivially in range but leaves most vertices of large models unreferenced.
 *
 * The answer is that the byte at part+2, previously documented as "flags", is
 * the part's BASE INDEX INTO A SHARED PER-MODEL SCRATCH WINDOW. Parts write
 * their transformed vertices into that window at vert_base, and faces index the
 * WINDOW, not the model's vertex array. Windows may overlap: on articulated
 * models 53% of scratch slots are written by more than one part.
 *
 * Elimination supports it as much as fit does — the u32 at part+4 is zero in
 * all 12,871 parts on the disc, so +2 is the only field in the record free to
 * carry any data at all.
 *
 * Verified over all 1,723 models / 553,160 indices:
 *   - indices at or past the window size:              0
 *   - reads of a slot no earlier part had written:     0
 *   - storage-vertex coverage:                         100.0000%
 *   - models at full coverage:                         1,723 / 1,723
 *   - reversing part order:                            exactly 18,995 failures
 *
 * The geometric evidence is decisive where it matters. On the 399 articulated
 * models — the only place the competing readings differ at all — mean agreement
 * between the stored normal and the computed face normal, over intra-part
 * faces, with sign agreement in brackets:
 *
 *     this reading      +0.7117 [98.13%]
 *     part-relative     +0.3274 [73.56%]   (22,237 of 47,620 unresolvable)
 *     model-absolute    +0.0855 [55.76%]
 *     vert_base + 1     +0.0062 [50.67%]   <- control
 *     vert_base - 1     +0.2125 [64.53%]   <- control
 *     bases shuffled    +0.1763 [62.14%]   <- control
 *
 * The +/-1 controls collapsing is what rules out a near-miss.
 *
 * Note carefully WHERE the new information lives, because it is easy to
 * accidentally re-test the old hypothesis: on all 656 multi-part models whose
 * parts all carry vert_base == 0, this reading and the old part-relative one
 * are BIT-IDENTICAL. The entire novel content is the 399 articulated models. In
 * particular the 745-vertex, 22-part model that earlier notes cited as the
 * counter-example to model-absolute has vert_base == 0 in every part and proves
 * nothing about the new mechanism.
 *
 * ---------------------------------------------------------------------------
 * What is INFERRED rather than confirmed
 * ---------------------------------------------------------------------------
 * Cross-part resolution. 21,217 faces — 15.3% of the disc, all inside the
 * articulated models — carry an index outside their own part's window. Two
 * rules both fit: LAST WRITER WINS in the shared buffer, and a pure arithmetic
 * offset with no buffer at all. Both resolve 100% of indices in range, they
 * differ on 18,283 articulated faces, and the normal test cannot separate them
 * (+0.0264 vs +0.0936 on exactly those faces, both noise).
 *
 * We take last-writer-wins because it is the only rule reaching 100.0000%
 * storage coverage on 1,723/1,723 models, where the rival orphans 746 vertices
 * across 261 models. First-writer-wins IS ruled out (+0.1341 / 58.33%).
 *
 * That choice is deliberately confined to q2_model_bake_indices() so it can be
 * flipped in one place: the two rules will draw those seam quads differently
 * once the bone matrices are known, because the rival transforms borrowed
 * vertices with the READING part's matrix while last-writer-wins uses the
 * WRITING part's.
 *
 * The per-part transform matrices themselves are still undecoded, so the 399
 * articulated models cannot yet be posed. The 1,324 static models are complete.
 * Block C is NOT where the matrices live — a direct search of all 8 byte lanes
 * and all 7 halfword lanes of its per-part body finds nothing on any of the 399
 * models that actually have non-zero bases.
 *
 * ---------------------------------------------------------------------------
 * Normals: the component order is z, x, y
 * ---------------------------------------------------------------------------
 * NOT x, y, z. This was established hypothesis-free on the 668 single-part
 * models, 58,580 samples, all six permutations and both quad windings, before
 * the index question was touched:
 *
 *     (s4,s5,s3) -> +0.7526 [99.0%]     <- +0x08 = nx, +0x0A = ny, +0x06 = nz
 *     (s5,s4,s3) -> +0.2806
 *     (s3,s5,s4) -> +0.2515
 *     (s4,s3,s5) -> +0.1995
 *     (s5,s3,s4) -> -0.0136
 *     (s3,s4,s5) -> -0.0073 [44.9%]     <- as previously documented
 *
 * Same ranking under both windings. Read in the previously documented order the
 * vector is uncorrelated with the surface and lighting is pure noise, which is
 * why this correction matters more than it looks.
 *
 * Magnitudes are unit vectors in 1.3.12 fixed point, |n| in 4094..4096.
 *
 * ---------------------------------------------------------------------------
 * A caution about the EXE
 * ---------------------------------------------------------------------------
 * No code that reads the on-disc part record has been located: across all
 * 174,592 instructions in the text segment, none of the 60 sites that advance a
 * pointer by 8 read bytes at both +2 and +3 off it. The field name vert_base
 * rests on statistical fit, not on a decompiled reader. That is consistent with
 * the loader relocating the structure before use, but it is worth knowing.
 *
 * The fixed-size effect renderer at 0x80064780 is a useful analogy and NOT
 * corroboration — its face stride is four bytes, not sixteen, and it never
 * touches CastList data. What it does show is the shape of the machinery: index
 * times eight into a scratch array, and a 96-entry transform buffer. 96 is
 * comfortably above the largest window any model on the disc needs, which is 91.
 *
 * ---------------------------------------------------------------------------
 * What the loader does to a model before anything reads it
 * ---------------------------------------------------------------------------
 * The relocation pass at 0x8006D124 walks the bank as a LINKED LIST — ofsEnd is
 * turned into an absolute next-pointer — and rewrites the header in place:
 *
 *   +0x04  ofsFaces..ofsBlockA, ofsBlockC, ofsBlockD and ofsEnd all become
 *          absolute pointers. ofsBlockB (+0x30) is deliberately NOT relocated
 *          and stays an offset, so its consumers compute model + [+0x30].
 *   +0x04  the field documented as `always3` is OVERWRITTEN with a pointer to a
 *          shared object at gp+0x588. It is a runtime slot, not model data.
 *
 * Then 0x8006C214 rescales three of the undecoded blocks. The vertex array is
 * NOT touched — model vertices are already at world scale — but:
 *
 *   block B  is an 8-entry table of u16 chain heads (which is why 821 of 965
 *            models have "16 zero bytes" there: eight empty chains). Each chain
 *            node is {u16 countAndFlags; u16 nextOffset; entry[count & 0x7F]},
 *            with each entry two u16 — and BOTH are multiplied by 10 at load.
 *   block C  is a chain of records whose s16 at +0 is multiplied by 10 and
 *            whose word at +4 is the byte delta to the next record.
 *   block D  is a run of 20-byte records ending at a zero word, with three u16
 *            at +12, +14 and +16 each multiplied by 5.
 *
 * The scale factors are the interesting part. Ten is the world's own lattice
 * step, so block B and block C hold spatial quantities stored at a tenth of
 * runtime scale — which is what a per-part translation would look like, and
 * block B is per-instance and present only on articulated models. That is now
 * the best lead for the missing transforms; it is not yet a decode.
 */
#ifndef Q2PSX_MODEL_H
#define Q2PSX_MODEL_H

#include "level.h"
#include "q2psx.h"

#define Q2_MODEL_HEADER_SIZE  64
#define Q2_MODEL_VERT_SIZE    12
#define Q2_MODEL_PART_SIZE     8
#define Q2_MODEL_FACE_SIZE    16

/* The engine's own scratch buffer is 96 entries; the largest window any model
 * on the disc needs is 91. Anything above 96 is a malformed file. */
#define Q2_MODEL_SCRATCH_MAX  96

typedef struct q2_model_header {
    char name[13];
    u16  num_faces;
    u16  num_parts;
    u32  num_verts;      /* derived: (ofs_parts - 0x40) / 12 */
    s16  ext0, ext1, ext2, ext3;
    u32  ofs_faces;
    u32  ofs_verts;
    u32  ofs_parts;
    u32  ofs_block_a;
    u32  ofs_block_b;
    u32  ofs_block_c;
    u32  ofs_block_d;
    u32  ofs_end;        /* 0 means this is the last model in the chunk */
} q2_model_header;

typedef struct q2_model_vertex {
    s16 x, y, z;         /* model-local on static models, PART-local otherwise */
    s16 nx, ny, nz;      /* stored on disc as z, x, y — see the header comment */
} q2_model_vertex;

typedef struct q2_model_part {
    u16 num_faces;       /* may legitimately be 0                              */
    u8  vert_base;       /* base slot in the shared scratch window             */
    u8  num_verts;
} q2_model_part;

typedef struct q2_model_face {
    u8 v[4];             /* indices into the SCRATCH window, not the vertex array */
    u8 uv[4][2];
    u8 flags;            /* bits 0-4 texture page, bits 5-7 blend selector      */
    u8 texture;          /* CLUT index within the map's SECOND palette section  */
} q2_model_face;

/*
 * What `flags` and `texture` mean, from the model emitter at 0x8006A2C4 — the
 * same shape as the world renderer's, with one difference that matters.
 *
 *   POLY.tpage = tpageTable[flags & 0x1F] | blendTable[flags >> 5]
 *   POLY.code  = codeTable[flags >> 5] | 0x3C
 *   POLY.clut  = clutIdTable[clut4_count_a + texture]      <-- the difference
 *
 * The world indexes the CLUT array from zero; a model face is offset by the
 * map's `clut4_count_a`. That is what the previously unexplained split of the
 * CLUT count into two bytes has always been: **section A is the world's
 * palettes, section B is the models'**. The engine only ever uses their sum to
 * size the upload, which is why the split looked vestigial from the data side.
 *
 * Checked disc-wide by `q2psx-inspect cluts` over all 138,290 faces: not one
 * has `texture >= clut4_count_b`, not one sets a blend bit — every model face
 * on this disc is opaque — and not one names a texture page the map does not
 * upload. The highest `texture` seen is 180 against a largest `clut4_count_b`
 * of 181, so the fit is tight rather than permissive.
 */
Q2PSX_INLINE u32 q2_model_face_page(const q2_model_face *f)
{
    return (u32)(f->flags & 0x1F);
}

Q2PSX_INLINE u32 q2_model_face_blend(const q2_model_face *f)
{
    return (u32)(f->flags >> 5);
}

/* `clut4_count_a` comes from the map's VRAM section; see vram.h. */
Q2PSX_INLINE u32 q2_model_face_clut_index(const q2_model_face *f,
                                          u32 clut4_count_a)
{
    return clut4_count_a + (u32)f->texture;
}

/* One model located inside a chunk. Borrows the chunk's buffer. */
typedef struct q2_model {
    const u8        *base;         /* start of this model's 64-byte header */
    u32              size;         /* ofs_end, or the remainder of the chunk */
    q2_model_header  hdr;
    u32              scratch_size; /* max over parts of vert_base + num_verts */
} q2_model;

/* The whole bank. Borrows the level file's buffer, so it must outlive this. */
typedef struct q2_model_bank {
    const u8 *data;
    u32       size;
    u32       count;
} q2_model_bank;

/*
 * Locate the CastList chunk of an already-opened file and count its models. A
 * zone's CastList may be zero length — 17 zones have one — which parses to a
 * bank with count 0 rather than an error.
 */
q2_result q2_model_bank_from_common(q2_model_bank *out, const q2_common_file *f);
q2_result q2_model_bank_from_zone(q2_model_bank *out, const q2_zone_file *f);

/*
 * Decode model `index` of the bank, validating every size identity. Returns
 * Q2_ERR_BAD_FORMAT rather than a best effort if anything fails to add up,
 * because on retail data nothing does.
 */
q2_result q2_model_get(const q2_model_bank *bank, u32 index, q2_model *out);

/* Decode one vertex, part or face. Return false if the index is out of range. */
bool q2_model_get_vertex(const q2_model *m, u32 index, q2_model_vertex *out);
bool q2_model_get_part(const q2_model *m, u32 index, q2_model_part *out);
bool q2_model_get_face(const q2_model *m, u32 index, q2_model_face *out);

/*
 * Resolve every face index to an index into the model's own vertex array, so a
 * static model can be uploaded as a flat mesh. `out` takes num_faces * 4 u16.
 *
 * Valid as a flat mesh ONLY when q2_model_is_static() is true. For an
 * articulated model the resolved vertices sit in different bone-local frames
 * and still need the per-part transforms, which are not yet decoded.
 *
 * This function is where the last-writer-wins choice lives; see the header
 * comment for why it is only INFERRED and what the alternative is.
 */
q2_result q2_model_bake_indices(const q2_model *m, u16 *out);

/*
 * True when every part carries vert_base == 0, i.e. the model is a plain flat
 * mesh needing no per-part transform. 1,324 of the disc's 1,723 models.
 */
bool q2_model_is_static(const q2_model *m);

#endif /* Q2PSX_MODEL_H */
