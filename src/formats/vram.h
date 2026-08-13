/*
 * vram.h — SNDVRAM.DAT's first section: the compressed VRAM images.
 *
 * This was the last unsolved format on the disc and the one blocking all
 * texturing. The codec is PackBits run-length encoding, byte-oriented, with a
 * destination-bounded loop.
 *
 * Everything here was derived twice, independently: once from the raw bytes of
 * all 49 SNDVRAM.DAT files and 553 image records, and once from the MIPS
 * disassembly of img_open at 0x80068A58 in SLES_015.34. Where the two passes
 * disagreed, the disassembly won and the correction is called out inline —
 * those corrections are the load-bearing part of this comment, because a
 * decoder written to the earlier wording produced plausible garbage rather than
 * an obvious failure.
 *
 * ---------------------------------------------------------------------------
 * Section layout (all offsets relative to file offset 0x0C)
 * ---------------------------------------------------------------------------
 *     +0x00  u8   texpage_count      1..12
 *     +0x01  u8   image_count        0..12
 *     +0x02  u8   clut4_count_a      17..86
 *     +0x03  u8   clut4_count_b      1..181
 *     +0x04       record[texpage_count + image_count]   8 bytes each
 *     ...         u16 clut4[clut4_count_a + clut4_count_b][16]   32 bytes each
 *     ...         u16 clut8[image_count][256]                    512 bytes each
 *     ...         packed NUL-terminated image names, one per record
 *     ...         the compressed payloads, up to the sound bank offset
 *
 * A record is:
 *     +0x00  u32  payload offset, RELATIVE TO 0x0C — add 0x0C for a file offset
 *     +0x04  u16  width      BYTES PER ROW of the decoded buffer
 *     +0x06  u16  height     rows
 *
 * Payload i spans [record[i].offset, record[i+1].offset), and the last runs to
 * the sound bank. The offsets being section-relative rather than absolute is
 * easy to get wrong: the raw value lands twelve bytes inside the image-name
 * strings, so a decoder using it feeds ASCII into the first image.
 *
 * The name-list offset is
 *
 *     4 + 8*N + 32*(clut4_count_a + clut4_count_b) + 512*image_count
 *
 * read out of the EXE at 0x80068A8C..0x80068AE0. Walking N NUL-terminated names
 * from there lands exactly on record[0].offset on all 49 files, with every name
 * printable ASCII. That is what pins the whole layout: a wrong CLUT count would
 * shift the name list and the walk would land in the middle of a payload.
 *
 * CORRECTION — bytes +0x02 and +0x03 are NOT opaque scalars. An earlier pass
 * recorded them as unknown_0E / unknown_0F. They are counts of 16-entry 4bpp
 * CLUTs. The load-time fix-up loop at 0x800762B4 loads both, ADDS them, shifts
 * left by 4, and iterates exactly that many u16 over the CLUT array pointer
 * written at 0x80068AC0, setting the transparency bit on every non-zero entry.
 * (a+b)*16 halfwords is (a+b) sixteen-entry CLUTs. The engine only ever uses
 * their sum; why the build tool splits them is still an open question.
 *
 * CORRECTION — the 512-byte run of 0x8000 that an earlier pass called "an
 * unused 256-entry CLUT slot" is nothing of the kind. It is the first SIXTEEN
 * reserved 4bpp CLUTs of the clut4 array: exactly 16 leading all-0x8000 32-byte
 * blocks in all 49 files, and exactly 16 such blocks anywhere in the array. The
 * old reading landed on the right byte range for the wrong reason and would
 * have mis-sized every field after it.
 *
 * ---------------------------------------------------------------------------
 * The codec
 * ---------------------------------------------------------------------------
 * Read one control byte at a time until the expected OUTPUT size is reached:
 *
 *     c <  0x80          copy the next (c + 1) bytes literally
 *     c >= 0x80          repeat the next single byte (257 - c) times
 *
 * CORRECTION — 0x80 is not the Apple PackBits no-op. The run loop at
 * 0x80068C48..0x80068C6C writes 257-c bytes for every c >= 0x80 with no special
 * case, so 0x80 means 129 repeats. This cannot be falsified from the disc —
 * 0x80 occurs zero times in 2,965,034 control bytes — but it is unambiguous in
 * the machine code, and treating it as a no-op would silently truncate any
 * stream that used it.
 *
 * The loop terminates on the remaining output count (bgtz at 0x80068C7C), not
 * on input exhaustion, and the original decrements that count by the full run
 * length even when the run overruns the buffer — so the shipped code can write
 * up to 128 bytes past its allocation. No retail stream does; we clamp anyway.
 * The original never bounds-checks the source at all.
 *
 * ---------------------------------------------------------------------------
 * How big the output is
 * ---------------------------------------------------------------------------
 * width * height bytes — EXCEPT that texture-page records ignore their stored
 * dimensions entirely and are forced to 128 x 256 (0x80068B74 / 0x80068B7C).
 * On this disc the stored dimensions happen to be 128x256 on all 331 texture
 * pages anyway, so the difference is invisible here; we follow the EXE because
 * a variant build need not be so tidy.
 *
 * That the target is width*height and not something else is pinned by the data
 * alone, before the disassembly is consulted. Exact-termination counts out of
 * 553 records, sweeping both the output target and the codec variant:
 *
 *     target  w*h 553 | w*h/2 152 | w*h-1 59 | w*h+1 20 | w*h*2 0 | w*h*3/2 0
 *     codec   lit c+1 / run 257-c  553      <- this one
 *             lit c+1 / run 258-c   60
 *             inverted token sense  18
 *             lit c+2 / run 257-c    9
 *             run 256-c, or lit c    0
 *
 * Nothing else comes within a factor of 3.6. Independently, the VRAM rectangle
 * the engine hands LoadImage — (width>>1) halfwords by height rows — covers
 * exactly the decoded byte count in 553/553 records, which is a second, wholly
 * separate confirmation that width is bytes-per-row.
 *
 * After an exact decode, 518 of 553 payloads are consumed to the byte. The
 * other 35 leave 1-3 zero bytes, and all 35 are the LAST record of their file
 * (none of the 504 non-final records are padded). The residue always satisfies
 * pad == (-decoded_end) mod 4. It is alignment padding, not stream data.
 *
 * The decoded output is real image data, not merely something that
 * decompresses: median mean absolute difference between adjacent bytes is 28.6
 * horizontally and 23.4 vertically, against 74.7 for a shuffled control, with
 * all 553 records beating the control on both axes.
 *
 * ---------------------------------------------------------------------------
 * Where images go in VRAM
 * ---------------------------------------------------------------------------
 * The upload wrapper at 0x800691A8 builds RECT{x, y, width>>1, height} — the
 * srl is at 0x80069214 — and calls LoadImage at 0x80083648. Its callers index
 * two static s16 tables:
 *
 *     x @0x800A3274 = {64,128,192,...,960}  ->  64 * (slot + 1)  halfwords
 *     y @0x800A329C = {256, ...}            ->  256              rows
 *
 * CORRECTION — an earlier pass guessed x = slot*64, y = 0. That is wrong on
 * both axes. x starts at 64 rather than 0 because the VRAM texture-page cell at
 * (x 0..63, y 256) is not a texture page at all: it holds the 4bpp CLUT array,
 * uploaded at 0x80076348 as RECT{0, 256, 64, ceil(clut4_count/4)}. A port using
 * the guessed coordinates would stack every texture page one cell to the left,
 * on top of the CLUTs, in the wrong half of VRAM.
 *
 * RESOLVE TEXTURE PAGES BY NAME, NOT BY INDEX. "The texture-page record index
 * is the tpage number" holds on all 49 maps, but as a consequence rather than
 * by construction: img_open resolves images by name (strcmp walk at
 * 0x80068AE8..0x80068B14) and the VRAM slot is the position in a fixed 13-entry
 * name table inside the EXE. The identity holds only because every map's
 * texture-page name list is that table's prefix, in order. Positional indexing
 * will mis-bind silently the moment that stops being true, which is why
 * q2_vram_find_by_name() exists and the loader keeps the name list.
 *
 * ---------------------------------------------------------------------------
 * Bit depth — INFERRED, deliberately not acted on
 * ---------------------------------------------------------------------------
 * Texture pages are probably 4bpp, but no code writing a GPU colour-mode field
 * has been located, so this module hands back raw decoded bytes and lets the
 * caller decide. Two arguments an earlier pass offered are dead: the tpage
 * colour-mode bits are vacuous (tpage is a u8 holding only 0..11, so its high
 * bits are clear by construction), and the VRAM budget does not constrain depth
 * (the upload rect is 64 halfwords per page at ANY depth, so 13 pages plus the
 * CLUT block occupy 896 of 1024 halfwords regardless). What does support 4bpp
 * is that the world CLUT array is built and uploaded as 16-entry CLUTs, which
 * is a 4bpp CLUT by definition, plus a 78.8% median double-nibble rate in RLE
 * run values against a 5.88% chance level.
 *
 * A blanket "standalone images are 8bpp" does not hold either: of the 222
 * per-image 512-byte CLUT blocks, 68 have 16 or fewer live entries.
 *
 * ---------------------------------------------------------------------------
 * What is still NOT established
 * ---------------------------------------------------------------------------
 * Which CLUT pairs with which surface. MapMod.clut is neither an index into the
 * CLUT-id table the engine builds at 0x80076378 (its maximum exceeds the CLUT
 * count on all 49 maps) nor a raw VRAM CLUT id in that layout (only 2.4% of
 * polygons even reach y >= 256, where the uploaded array lives). So this module
 * decodes and places images but cannot yet colour world geometry, and that one
 * question is now the highest-value target in the whole project.
 */
#ifndef Q2PSX_VRAM_H
#define Q2PSX_VRAM_H

#include "disc.h"
#include "q2psx.h"

/* Forward-declared so this header does not drag the renderer in. */
struct psx_vram;

#define Q2_VRAM_RECORD_SIZE   8
#define Q2_VRAM_SECTION_BASE  0x0C

/* Texture-page records ignore their stored dimensions; the EXE forces these. */
#define Q2_VRAM_TEXPAGE_W   128    /* bytes per row */
#define Q2_VRAM_TEXPAGE_H   256    /* rows          */

/* A 4bpp CLUT is 16 entries; an 8bpp CLUT is 256. */
#define Q2_VRAM_CLUT4_ENTRIES  16
#define Q2_VRAM_CLUT8_ENTRIES  256

/* The engine's texture-page name table has 13 slots. */
#define Q2_VRAM_TEXPAGE_SLOTS  13

/*
 * VRAM placement, read out of the EXE's static tables rather than guessed.
 * Coordinates are in the PSX's native units: x counts 16-bit halfwords, y rows.
 */
#define Q2_VRAM_TEXPAGE_X(slot)  (64 * ((slot) + 1))
#define Q2_VRAM_TEXPAGE_Y        256
#define Q2_VRAM_CLUT4_X          0
#define Q2_VRAM_CLUT4_Y          256
#define Q2_VRAM_CLUT4_W          64   /* halfwords: four 16-entry CLUTs per row */

/* The transparency/STP bit the load-time fix-up ORs into every live CLUT entry. */
#define Q2_VRAM_STP_BIT  0x8000u

typedef struct q2_vram_image {
    u32         offset;        /* file offset of the packed payload            */
    u32         packed_size;
    u16         width;         /* bytes per row, AS STORED                     */
    u16         height;        /* rows, AS STORED                              */
    u32         decoded_size;  /* what it actually decodes to; see the header  */
    const char *name;          /* into the section's packed name list          */
} q2_vram_image;

/* A VRAM upload rectangle, in the PSX's own units. */
typedef struct q2_vram_rect {
    s16 x, y;    /* x in halfwords, y in rows */
    s16 w, h;    /* w in halfwords, h in rows */
} q2_vram_rect;

typedef struct q2_vram_section {
    q2_buf         buf;            /* owns the whole SNDVRAM.DAT             */
    q2_vram_image *images;         /* owned; texpages first, then images     */
    u32            image_count;    /* texpage_count + standalone image count */
    u32            texpage_count;

    /* The two CLUT arrays, borrowed from buf. clut4 is the world palette bank
     * shared by every surface; clut8 is one 256-entry block per standalone
     * image, in image order. */
    const u8      *clut4;
    u32            clut4_count;    /* clut4_count_a + clut4_count_b          */
    u8             clut4_count_a;  /* kept separately only because the split */
    u8             clut4_count_b;  /* is systematic and still unexplained    */
    const u8      *clut8;
    u32            clut8_count;    /* == the standalone image count          */
} q2_vram_section;

/* Load "<map>/SNDVRAM.DAT" and index its section A. */
q2_result q2_vram_load(q2_vram_section *out, const disc *d, const char *map);
void      q2_vram_free(q2_vram_section *section);

/*
 * Bytes image `index` decodes to. Returns 0 for an out-of-range index. This is
 * NOT always width*height — texture pages are forced to 128x256 — so always
 * size buffers with this rather than multiplying the record fields yourself.
 */
size_t q2_vram_decoded_size(const q2_vram_section *section, u32 index);

/*
 * Decode image `index` into `out`, which must hold at least
 * q2_vram_decoded_size() bytes. Writes the decoded size to `out_size`.
 *
 * Returns Q2_ERR_BAD_FORMAT if the payload does not decode to exactly that many
 * bytes — a hard error rather than a warning, because every payload on a good
 * disc does, and a short decode means we have mis-located the payload.
 */
q2_result q2_vram_decode(const q2_vram_section *section, u32 index,
                         u8 *out, size_t out_capacity, size_t *out_size);

/*
 * Find an image by name, case-insensitively. This is how the engine itself
 * resolves texture pages, and a port should use it in preference to indexing.
 * Returns false if absent.
 */
bool q2_vram_find_by_name(const q2_vram_section *section, const char *name,
                          u32 *index_out);

/*
 * Where image `index` belongs in VRAM. `slot` is the engine texture-page slot
 * for a texture page; it is ignored for standalone images, whose placement is
 * not established and which come back at (0,0) for the caller to position.
 */
bool q2_vram_image_rect(const q2_vram_section *section, u32 index, u32 slot,
                        q2_vram_rect *out);

/* Where the 4bpp CLUT array belongs in VRAM. */
void q2_vram_clut4_rect(const q2_vram_section *section, q2_vram_rect *out);

/*
 * The CLUT id the engine builds for 4bpp CLUT `index`, per 0x80076378: laid out
 * four per 64-halfword row starting at (0, 256).
 *
 * NOTE that MapMod.clut is NEITHER this id NOR an index into this table — both
 * readings are refuted (see the header comment). This is exposed for tooling
 * and for whoever finally cracks that mapping, not as a lookup a renderer
 * should be using today.
 */
u16 q2_vram_clut4_id(u32 index);

/*
 * Copy 4bpp CLUT `index` out, applying the load-time fix-up at 0x800762B4:
 * every non-zero entry gets the STP bit set. `out` takes 16 u16 entries.
 * Returns false if the index is out of range.
 */
bool q2_vram_get_clut4(const q2_vram_section *section, u32 index, u16 out[16]);

/*
 * Copy standalone image `image_index`'s 256-entry CLUT out. Note this is
 * indexed by position among the STANDALONE images, not by record index — add
 * texpage_count to convert. No STP fix-up is applied: the fix-up loop covers
 * only the clut4 array.
 */
bool q2_vram_get_clut8(const q2_vram_section *section, u32 image_index,
                       u16 out[256]);

/*
 * The PackBits decoder on its own, for testing and for any other packed data
 * that turns up. Stops once `target` bytes have been produced, clamps runs that
 * would overrun (the original does not), and reports how much input it read.
 */
size_t q2_packbits_decode(const u8 *src, size_t src_size,
                          u8 *dst, size_t target, size_t *consumed);

/* ------------------------------------------------------------------------- */
/* Palette binding                                                            */
/*                                                                            */
/* A MapMod polygon's u16 `clut` field is not a hardware CLUT word. It packs:  */
/*                                                                            */
/*     index = clut >> 8      index into clut4[] IN FILE ORDER                */
/*     semi  = clut & 3       semi-transparency selector                      */
/*                                                                            */
/* CONFIRMED on 49/49 maps, 115/115 zone files and all 274,936 polygons. The  */
/* index is an array index by construction, not by coincidence: the engine    */
/* uploads clut4[] as ONE flat blob (so array order is VRAM order) and then   */
/* builds its id table by walking i = 0..n-1 in that same order, which admits */
/* no permutation.                                                            */
/*                                                                            */
/* Set equality is what pins the range: {clut>>8} minus {0} equals exactly    */
/* [16, clut4_count_a) — dense, no gaps, no overshoot — on every map. The     */
/* leading 16 are the reserved all-0x8000 palettes.                           */
/*                                                                            */
/* Ordering was established separately, because set equality is invariant     */
/* under any permutation and therefore cannot establish it. A texel-adjacency */
/* coherence test ranked the claimed palette against every other palette of   */
/* the same map: mean percentile 0.099 against a chance of 0.5, below chance  */
/* on 34/34 maps — while the same test on index+1 and index+5 landed at 0.504 */
/* and 0.533, i.e. exactly chance. The controls are what make the result mean */
/* anything.                                                                  */
/*                                                                            */
/* Index 0 is real and must not be treated as "no palette": 11,255 polygons   */
/* use it, always on page 0 and always sampling the 64x64 tile at the page    */
/* origin, which holds genuine texture content.                               */
/*                                                                            */
/* The executable now says the same thing outright. The world renderer at     */
/* 0x80068288 loads the polygon's byte at +9 — the HIGH byte of this field —  */
/* doubles it, adds the id-table pointer at 0x800B2EDC and stores the halfword*/
/* it finds into POLY_GT4.clut; at 0x800682A8 it takes the byte at +8 and     */
/* tests only its low two bits, choosing primitive code 0x3E over 0x3C. The   */
/* statistical case above and the disassembly were reached independently and  */
/* agree exactly, which is the strongest position this format gets to.        */
/* ------------------------------------------------------------------------- */
#define Q2_VRAM_CLUT_RESERVED 16   /* leading all-0x8000 palettes */

Q2PSX_INLINE u32 q2_mapmod_clut_index(u16 clut) { return (u32)(clut >> 8); }
Q2PSX_INLINE u32 q2_mapmod_clut_semi(u16 clut)  { return (u32)(clut & 3); }

/*
 * Where clut4 entry `index` lands in VRAM, as halfword coordinates. The array
 * uploads to RECT{0, 256, 64, ceil(n/4)}, four 16-halfword palettes per row.
 */
Q2PSX_INLINE void q2_vram_clut_pos(u32 index, u32 *out_x, u32 *out_y)
{
    if (out_x) *out_x = 16u * (index & 3u);
    if (out_y) *out_y = 256u + (index >> 2);
}

/* The hardware CLUT word the GPU wants, built from that position. */
Q2PSX_INLINE u16 q2_vram_clut_word(u32 index)
{
    u32 x, y;
    q2_vram_clut_pos(index, &x, &y);
    return (u16)(((y & 0x1FFu) << 6) | ((x & 0x3FFu) >> 4));
}

/* Borrow the 16 BGR555 entries of clut4 palette `index`. NULL if out of range. */
const u8 *q2_vram_clut(const q2_vram_section *section, u32 index);

/*
 * Decode every texture page and palette into a psx_vram, at the addresses the
 * engine uses: pages at x = 64*(slot+1) halfwords, y = 256; palettes at
 * x = 16*(i&3), y = 256 + (i>>2).
 *
 * After this the rasteriser's ordinary 4bpp CLUT sampling just works, because
 * VRAM holds exactly what the console's VRAM held.
 */
q2_result q2_vram_upload(const q2_vram_section *section, struct psx_vram *vram);

#endif /* Q2PSX_VRAM_H */
