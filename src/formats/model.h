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
 * The per-part transform is a keyframed translation and quaternion, decoded
 * from block C — see the animation section below. There is no matrix stored
 * anywhere, which is why searching block C's per-part body for one found
 * nothing: it holds packed angles, not matrix elements.
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
 *   block C  is the ANIMATION BANK; its per-clip u16 at +0 is a duration and
 *            the multiply by 10 is what makes the runtime clock tick ten times
 *            per animation frame. See the animation section below.
 *   block D  is a run of 20-byte records ending at a zero word. The claim that
 *            its u16 at +12, +14 and +16 are multiplied by 5 at load is
 *            UNSUPPORTED -- no such site exists; the x5 shape in this EXE is a
 *            20-byte record stride. See the move table below and #51f.
 *
 * The ten in block C is TIME, not distance. An earlier note here guessed it was
 * spatial on the grounds that ten is also the world's lattice step; the pose
 * selector shows it is a subtick rate. Whether block B's tens are times or
 * distances is still open — its entries are pairs, which fits neither a 3-vector
 * nor a duration obviously.
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

/* ---------------------------------------------------------------------------
 * Animation — block C
 * ---------------------------------------------------------------------------
 * Block C is the animation bank, and it is what poses the articulated models.
 * The pose selector at 0x8006B924 walks it as a chain of clips, subtracting
 * each clip's duration from the entity's tick counter until one contains the
 * current time; then it decodes one keyframe per part.
 *
 *   clip:  u16 frames      duration; the runtime clock ticks TEN TIMES per
 *                          animation frame -- see 0x8006B5D8, which divides the
 *                          position by 10 (magic 0x66666667, sra 2) for a frame
 *                          index and keeps the remainder as the lerp fraction.
 *                          That division is the proof of the unit; the claim
 *                          that a LOADER multiplies durations is unverified.
 *          u16 flags       bit 0 selects the variable-rate path (below)
 *          u32 next        BYTE DELTA to the next clip; 0 ends the chain
 *          then per frame f:  u16 rateOfs; u16 keyOfs
 *                          keys are at (that entry's address) + keyOfs
 *   key:   two u32 per part, in part order
 *
 * Both key words are packed three-field:
 *
 *   word 0   translation   bits 0-10 (x2), 11-20 (x4), 21-31 (x2), each signed
 *   word 1   rotation      three unsigned half-angles: bits 0-10, 11-20 (x2),
 *                          21-31, in the PlayStation's 4096-step circle
 *
 * The rotation is a QUATERNION, not a matrix. 0x800699E8 builds it with the
 * textbook Euler-to-quaternion products against a {sin, cos} table at
 * 0x800A5430 — x = SA*CB*CC - CA*SB*SC and the other three in the same shape —
 * which is also why the angle fields only span half a circle: they are half
 * angles. The output is 1.3.12, the same format as everything else here.
 *
 * The size law the earlier pass measured falls straight out of this: a
 * single-frame clip is 8 bytes of header, one 4-byte frame entry and
 * 8*numParts of keys, i.e. `12 + 8*numParts` — which held on 659 of 965 models
 * for reasons nobody could state at the time. The same pass noted that
 * single-frame models have `u16@0 == 1`, `u16@2 == 0` and `u16@10 == 4`: one
 * frame, no flags, and a key offset of 4.
 *
 * A clip with flags bit 0 set is laid out differently, and the difference is
 * easy to miss: its four-byte entries are PER PART rather than per frame, and
 * each names two things � a stream of 4-bit durations in frames, eight to a
 * word, and that part's own list of keys. A part holds a key for as many frames
 * as its nibble says, and poses between keys are interpolated: translations
 * linearly, rotations by the spherical interpolator at 0x80069C64, which flips
 * the second quaternion when the dot product is negative and falls back to a
 * straight lerp below its angle threshold. 3,241 of the disc's 4,535 clips are
 * this kind, so it is the common case rather than the exception.
 */
/*
 * How a pose reaches the vertices, from the hand-written GTE routine at
 * 0x800B1F90 — worth knowing before drawing anything, because the port has to
 * reproduce the order, not just the arithmetic:
 *
 *   1. Every part's TRANSLATION is transformed first, in one pass, and kept in
 *      a parallel array. The rotation that pass uses is the model's, not the
 *      part's, so translations are model-space offsets.
 *   2. Then, per part: the quaternion becomes a 3x3 with the standard form
 *      (m00 = 1 - 2y^2 - 2z^2 and so on, in 1.3.12), and that matrix is
 *      composed with two matrices held by the caller — one landing in the GTE's
 *      LIGHT matrix, for normals, and one in its ROTATION matrix, for
 *      positions. Neither is another part's matrix: parts do not inherit.
 *   3. The part's translation goes into the GTE translation registers, and its
 *      vertices go through RTPT three at a time, landing in the shared scratch
 *      window at vert_base * 8. Normals go through the lighting op into the
 *      same slots' colour halves.
 *
 * So a part's transform is v' = R(q) * v + t, applied per part with no
 * hierarchy — which is what q2_quat_to_matrix and the pose translation give
 * you. What is NOT yet established is what the caller's two matrices hold for a
 * given entity; see openquestions #2d.
 */
#define Q2_MODEL_TICKS_PER_FRAME 10

typedef struct q2_model_anim {
    u32 offset;      /* byte offset of the clip from the model base */
    u16 frames;      /* duration in animation frames                */
    u16 flags;       /* bit 0: per-part variable-rate keys          */
    u32 next;        /* byte delta to the next clip, 0 if last      */
} q2_model_anim;

/* One part's pose: a translation in world units and a unit quaternion in
 * 1.3.12, ordered x, y, z, w as the engine builds it. */
typedef struct q2_model_pose {
    s16 t[3];
    s16 q[4];
} q2_model_pose;

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

/*
 * The index of the model with this name, or -1.
 *
 * Case-insensitive, and FIRST MATCH WINS — names are not unique (11 maps carry
 * duplicates), and the engine's own lookup at 0x8006D008 is a linear walk of the
 * loaded list that stops at the first hit, so taking the first is behaviour
 * rather than a shortcut. Every table that names a model — the entity class
 * table, the item table — resolves through this.
 */
s32 q2_model_bank_find(const q2_model_bank *bank, const char *name);

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

/* Walk the animation chain. `index` counts from the first clip; returns false
 * past the end, on a malformed chain, or when the model has no block C. */
bool q2_model_anim_get(const q2_model *m, u32 index, q2_model_anim *out);

/*
 * The clip a TIME lands in, and how far into it — which is how the engine picks
 * one, and it does not pick by index at all.
 *
 * `0x8006B924` holds the position in a halfword at `entity+0x100` and the
 * current clip pointer at `model+0x34`, and while the position is past the
 * clip's length it advances the pointer by the clip's own `next` byte delta
 * (through `0x80070188`, which is `*p += d`) and subtracts that clip's
 * `frames`. So a model's clips are ONE CONTINUOUS TIMELINE and the animation
 * position is an offset into it; a clip boundary is wherever the subtractions
 * happen to fall.
 *
 * That matters to anything driving a model from a creature, because it means
 * there is no clip index to find: a move's frames map onto this timeline and
 * the walk lands in the right clip on its own. See openquestions #47.
 *
 * Returns false when the timeline is shorter than `tick`, which is the walk
 * running off the end rather than an error.
 */
bool q2_model_anim_at(const q2_model *m, u32 tick, q2_model_anim *out,
                      u32 *within);

/*
 * The same walk, holding the last frame instead of failing off the end.
 *
 * `q2_model_anim_at` is the engine's loop exactly — `position < clip->frames`,
 * then subtract, then advance — and the engine has NO end-of-chain test at all
 * (0x8006B924's loop, with 0x80070188 as the advance, is four instructions and
 * checks nothing). It relies on the position always landing inside.
 *
 * It does not always land inside here, and the reason is not arithmetic. An AI
 * move's length and its animation's extent are different quantities authored
 * independently (#63): the Enforcer's `Duck` is a 30-frame AI move whose
 * animation begins at model frame 1287 of a 1302-frame timeline, so it has
 * five AI frames of animation and twenty-five frames of nothing after it. At
 * `into = 5` the position is frame 1302 — one past the last — and the walk
 * fails.
 *
 * On the console the move would have ended by then: the animation's end
 * callback fires and the creature is in a different move. This port's frame
 * counter is not driven by that callback, so it can sit past the end, and the
 * choice is between holding the last pose and falling back to matching a clip
 * by LENGTH — something the disc never does. Holding is the smaller lie, and
 * it is the one the caller can see: `*clamped` says it happened.
 */
bool q2_model_anim_at_held(const q2_model *m, u32 tick, q2_model_anim *out,
                           u32 *within, bool *clamped);


/*
 * The `skip`-th clip whose length is exactly `frames`, or false if there is no
 * such clip.
 *
 * This is how a MOVE finds its clip, and it is a different question from the
 * one `q2_model_anim_at` answers. That function walks the chain subtracting
 * lengths, which is right for a position on a running timeline — but the engine
 * keeps a CURRENT clip pointer at `model+0x34` and only advances it when the
 * position overruns, so the clip is selected elsewhere and the position is
 * relative to it.
 *
 * What selects it is not in the module's data, so it is reconstructed here from
 * the one thing that does correspond: a move's frame count times
 * Q2_CRE_TICKS_PER_FRAME is exactly some clip's length. The Soldier's death
 * move runs 308-342 — 35 frames, 105 ticks — and clip 11 is 105 frames long and
 * is the death animation, standing to fallen. Treating the AI frame as a
 * position on one continuous timeline instead puts frame 308 at tick 924, which
 * lands in clip 12, and the creature stands up again halfway through dying.
 *
 * `skip` disambiguates lengths that repeat: the k-th move of a given length
 * takes the k-th clip of the matching length, in each list's own order.
 */
/*
 * Matching a clip by LENGTH — a substitute for something the disc never does,
 * and now known to be needed for exactly one move per creature.
 *
 * A move is named by a record matched on frame range, and seven moves on the
 * disc — one in each of the seven creature modules — have no such record. The
 * obvious way to recover them was POSITION: block D tiles the timeline and the
 * AI ranges tile too, so a move between two named ones would have a determined
 * record. Measured, the two orderings are independent — the Soldier's `Run` is
 * its first AI move and the LAST stretch of its model's timeline, while
 * `Death1` is late in the AI's order and sits at block-D zero. There is nothing
 * to interpolate. See openquestions #99.
 */
bool q2_model_anim_by_length(const q2_model *m, u32 frames, u32 skip,
                             q2_model_anim *out);

/* How many clips the chain holds. Zero for a model with no animation. */
u32 q2_model_anim_count(const q2_model *m);

/*
 * Block D — the NAMED MOVE table.
 *
 * Read off the bytes, not off a guess. BASE1 model 15 (31 clips, 31 moves):
 *
 *   44 65 61 74 68 31 00 00 00 00 00 00 00 00 D6 00 00 00 01 00  |Death1......|
 *   50 61 69 6E 31 00 00 00 00 00 00 00 D8 00 F4 00 D8 00 01 00  |Pain1.......|
 *   50 61 69 6E 32 00 00 00 00 00 00 00 F6 00 1E 01 F6 00 01 00  |Pain2.......|
 *   41 74 74 61 63 6B 34 00 00 00 00 00 F2 01 14 02 14 02 01 00  |Attack4.....|
 *
 * So a record is `{char name[12]; u16 start; u16 end; u16 rest; u16 one}`, and
 * the ranges TILE A CONTINUOUS TIMELINE with a two-unit gap between moves:
 * 0..214, 216..244, 246..286, 288..394, 396..496, 498..532, 534..850, ...
 * Across 92 moves on 14 zones: every span is EVEN, every gap is exactly 2, and
 * `rest` is `end` on 77 and `start` on 15. `one` is 1 on 90 and 0 on exactly
 * two — both a mover's move named "Move" — so it is a flag, not a constant.
 *
 * THE UNIT IS TWO PER ANIMATION FRAME, and move `i` IS clip `i`.
 *
 * Not inferred — compared. Model 15 of BASE1 carries 31 clips and 31 moves, and
 * laying the two lists side by side gives, in order and with no exceptions:
 *
 *   clip lengths : 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 ...
 *   span / 2 + 1 : 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 ...
 *
 * Censused over 25 zones, every model carrying both: **34 models, 0
 * mismatches.** So a move of span S is S/2 + 1 frames, the two-unit gap between
 * moves is exactly one frame, and the k-th move drives the k-th clip.
 *
 * That also recovers the factor of 5 arithmetically, without needing the load
 * site that does not exist: block D counts 2 per frame, the animation position
 * counts 10 per frame (`0x8006B5D8` divides by ten), and 2 * 5 = 10. The
 * earlier claim of a load-time multiply was right about the RATIO and wrong
 * about the mechanism, which is why it survived so long — see #51f and #51g.
 *
 * WHAT THIS TABLE IS NOT: the table `0x8007E9DC` walks. That one is reached
 * through the entity's linked object (`entity+0x2EC`, then its `+0x28`) and is
 * searched by containment —
 *
 *     find the record with  record[0] <= frame <= record[2]
 *     position = record[18] + 30 * (frame - record[0])
 *
 * — reading a SIGNED halfword at +0 as the terminator. Block D's +0 is ASCII,
 * so it can never terminate that walk. The runtime table is therefore built
 * from block D at load rather than being block D, and the note at the top of
 * this file claims the build multiplies `+12/+14/+16` by 5. **That
 * transformation is unverified**, so nothing here feeds the animation path yet
 * — see openquestions #51e.
 *
 * The surrounding facts are settled: the position is in tenths of an animation
 * frame (`0x8006B5D8` divides by 10, magic 0x66666667, keeping the remainder
 * as the lerp fraction), and the pose selector at `0x8006B924` consumes it out
 * of `entity+0x100`, the halfword `0x8007E9DC` writes. See #51b, #51c, #51d.
 */
typedef struct q2_model_move {
    char name[13];       /* 12 bytes on disc, NUL-padded; "Pain1", "Death4"   */
    u16  start;          /* +12 — first position of the move on the timeline  */
    u16  end;            /* +14 — last position, inclusive                    */
    u16  rest;           /* +16 — the record's CURRENT POSITION, a cursor.
                          * 0x8003CBF4 writes `start` here to rewind a record,
                          * so the 15 records sitting at `start` are rewound and
                          * the 77 at `end` have played out. Not a flag. */
    u16  one;            /* +18 — 1 on every record seen so far               */
} q2_model_move;

/* Read move `index` from block D. False past the end of the table. */
bool q2_model_move_get(const q2_model *m, u32 index, q2_model_move *out);

/* How many moves block D holds. Zero if the model has none. */
u32 q2_model_move_count(const q2_model *m);

/*
 * Find a move by name — and this is the ENGINE'S OWN mechanism, not a
 * convenience. `0x8006D330` walks block D comparing each record's 12-byte name
 * field against a name the caller passes by value in a1/a2/a3, three words at a
 * time, and returns the match. The engine never indexes block D and never
 * matches by clip length.
 *
 * So this function is the right way to reach a move, and
 * `q2_model_anim_by_length()` is a substitute for something the disc does not do.
 */
/* Find a move by name, case-sensitive as stored. False if there is none. */
bool q2_model_move_by_name(const q2_model *m, const char *name,
                           q2_model_move *out);

/*
 * The clip a move drives, and the frame count they agree on.
 *
 * Move `index` drives clip `index` — verified on every model on the disc that
 * carries both, 34 for 34, by `span / 2 + 1 == clip.frames` in list order.
 *
 * `index` here is a BLOCK D move index, which is not a creature module's move
 * index: the Soldier's model has 31 moves and its module has 18, in a different
 * frame numbering (see #51h). So this does NOT yet replace
 * `q2_model_anim_by_length()` on the AI path — that still matches on length
 * with a `skip` to break ties. It replaces it only once a module's move can be
 * named or ordered into this table.
 */
bool q2_model_clip_for_move(const q2_model *m, u32 index, q2_model_anim *out);

/* A move's length in animation frames: span/2 + 1. */
u32 q2_model_move_frames(const q2_model_move *mv);

/* Block D counts 2 units per animation frame; the position counts 10. */
#define Q2_MODEL_BLOCKD_PER_FRAME 2

/*
 * The animation POSITION for an AI frame — the engine's own path, end to end.
 *
 *     position = block_d[name].start * 5 + 30 * (ai_frame - move_first)
 *
 * Each term is read off the disc:
 *
 *   name lookup   0x8006D330 walks block D comparing 12-byte names
 *   * 5           block D counts 2 units per animation frame, the position 10
 *   30 per frame  0x8007EA44, which is 3 model frames at 10 units each
 *   the walk      0x8006B924 subtracts clip durations until the position lands
 *
 * `ai_frame` and `move_first` come from the CREATURE, not the model: they are a
 * duration, how long the creature spends in the move. The block-D span is the
 * animation's extent. Nothing requires them to agree — a move shorter than its
 * animation stops partway, and a longer one runs on into whatever follows on the
 * continuous timeline, which is legal because a clip boundary is only where the
 * subtractions fall.
 *
 * Feed the result to `q2_model_anim_at()`, which already walks a position to a
 * clip. That pair replaces `q2_model_anim_by_length()`, which matches a clip by
 * length — something the disc never does.
 */
bool q2_model_position_for_move(const q2_model *m, const char *move_name,
                                s32 ai_frame, s32 move_first, u32 *out_pos);

/* Position units per AI frame within a move, from `0x8007EA44`. */
#define Q2_MODEL_POS_PER_MOVE_FRAME 30

/*
 * Decode every part's pose at `frame` of `clip` into `out`, which must hold
 * num_parts entries.
 *
 * Returns Q2_ERR_UNSUPPORTED for a variable-rate clip rather than decoding it
 * wrongly — those need the interpolating path, which is not implemented.
 */
q2_result q2_model_pose_at(const q2_model *m, const q2_model_anim *clip,
                           u32 frame, q2_model_pose *out);

#endif /* Q2PSX_MODEL_H */
