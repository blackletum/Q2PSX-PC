/*
 * stx.c — see stx.h.
 */
#include "stx.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* The container                                                              */
/* ------------------------------------------------------------------------- */
bool q2_stx_header_read(const u8 *sector, q2_stx_header *out)
{
    q2_stx_header h;

    if (!sector || !out)
        return false;

    h.magic            = q2_rd_u16(sector + 0x00);
    h.sub_type         = q2_rd_u16(sector + 0x02);
    h.chunk_index      = q2_rd_u16(sector + 0x04);
    h.chunk_count      = q2_rd_u16(sector + 0x06);
    h.frame_number     = q2_rd_u32(sector + 0x08);
    h.frame_size_bytes = q2_rd_u32(sector + 0x0C);
    h.width            = q2_rd_u16(sector + 0x10);
    h.height           = q2_rd_u16(sector + 0x12);
    h.bs_num_codes     = q2_rd_u16(sector + 0x14);
    h.bs_magic         = q2_rd_u16(sector + 0x16);
    h.bs_qscale        = q2_rd_u16(sector + 0x18);
    h.bs_version       = q2_rd_u16(sector + 0x1A);
    h.reserved         = q2_rd_u32(sector + 0x1C);

    if (h.magic != Q2_STX_MAGIC || h.sub_type != Q2_STX_SUBTYPE_MDEC)
        return false;
    if (h.bs_magic != Q2_STX_BS_MAGIC)
        return false;
    if (h.chunk_count == 0 || h.chunk_count > Q2_STX_MAX_CHUNKS)
        return false;
    if (h.chunk_index >= h.chunk_count)
        return false;
    if (h.frame_size_bytes > (u32)h.chunk_count * Q2_STX_VIDEO_PAYLOAD)
        return false;

    *out = h;
    return true;
}

bool q2_stx_frame_next(const u8 *buf, size_t size, size_t *cursor,
                       q2_stx_frame *out)
{
    q2_stx_header h;
    u32 got = 0;

    if (!buf || !cursor || !out)
        return false;

    /* Find the next sector that opens a frame. Audio sits at slot 7 and the
     * tail's video slots are zero-filled, so both are simply skipped. */
    for (;;) {
        size_t at = *cursor * Q2_STX_SECTOR_SIZE;

        if (at + Q2_STX_SECTOR_SIZE > size)
            return false;
        if (!q2_stx_sector_is_audio((u32)*cursor) &&
            q2_stx_header_read(buf + at, &h) && h.chunk_index == 0)
            break;
        (*cursor)++;
    }

    memset(out, 0, sizeof(*out));
    out->number = h.frame_number;
    out->size   = h.frame_size_bytes;
    out->qscale = h.bs_qscale;
    out->num_codes = h.bs_num_codes;
    out->width  = h.width;
    out->height = h.height;

    while (got < h.chunk_count) {
        size_t at = *cursor * Q2_STX_SECTOR_SIZE;
        q2_stx_header c;

        if (at + Q2_STX_SECTOR_SIZE > size)
            return false;

        if (q2_stx_sector_is_audio((u32)*cursor)) {
            (*cursor)++;
            continue;
        }
        if (!q2_stx_header_read(buf + at, &c) ||
            c.frame_number != h.frame_number || c.chunk_index != got)
            return false;

        memcpy(out->data + (size_t)got * Q2_STX_VIDEO_PAYLOAD,
               buf + at + Q2_STX_HEADER_SIZE, Q2_STX_VIDEO_PAYLOAD);
        got++;
        (*cursor)++;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* The BS v2 bitstream                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The bits arrive as 16-bit LITTLE-endian words, consumed most-significant bit
 * first within each word. That pairing is the one thing about this format that
 * cannot be guessed from the data — get either half wrong and the first
 * Huffman code is already nonsense — and it is the PSX MDEC's own convention.
 */
typedef struct bitreader {
    const u8 *p;
    u32       bytes;
    u32       at;        /* byte offset of the NEXT word            */
    u32       bits;      /* how many of `word` are still unread     */
    u32       word;
    u32       consumed;  /* bits handed out, for the exactness check */
    bool      dry;
} bitreader;

static void br_init(bitreader *b, const u8 *p, u32 bytes)
{
    memset(b, 0, sizeof(*b));
    b->p     = p;
    b->bytes = bytes;
}

static u32 br_get(bitreader *b, u32 n)
{
    u32 v = 0;

    while (n--) {
        if (b->bits == 0) {
            if (b->at + 2 > b->bytes) {
                /*
                 * Out of data: hand back what there was, ZERO-PADDED to the
                 * width that was asked for. `v << 1` was the old answer and it
                 * is wrong by however many bits are still owed, which matters
                 * for exactly one caller and one place: the 17-bit LOOKAHEAD at
                 * the end of a frame. Every frame carries 8 to 47 bits of
                 * padding after its last block, so the last block of every
                 * frame peeks past the end — and got a misaligned lookahead, so
                 * its EOB never matched. That, and not the table, is why 640 of
                 * the disc's 5301 frames used to stop exactly ONE block short.
                 */
                b->dry = true;
                return v << (n + 1);
            }
            b->word = q2_rd_u16(b->p + b->at);
            b->at  += 2;
            b->bits = 16;
        }
        v = (v << 1) | ((b->word >> (b->bits - 1)) & 1u);
        b->bits--;
        b->consumed++;
    }

    return v;
}

static u32 br_peek(const bitreader *b, u32 n)
{
    bitreader t = *b;
    return br_get(&t, n);
}

/* The default intra quantisation table, in NATURAL (raster) order. */
static const u8 k_quant[64] = {
     2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

/* Zigzag: scan position -> natural index. */
static const u8 k_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * The AC coefficient table — MPEG-1's Table B.14, which is what the MDEC uses.
 *
 * Each entry is {code, length, run, level}; the code is right-aligned in
 * `code` and `length` says how many bits of it are real. A sign bit follows
 * every one of them. Longest first is not required because the reader peeks a
 * fixed 17 bits and compares prefixes, but they are kept in length order so a
 * shorter code can never shadow a longer one that starts with it.
 */
typedef struct ac_code {
    u16 code;
    u8  len;
    u8  run;
    u8  level;
} ac_code;

static const ac_code k_ac[] = {
    /*
     * 2..7 bits.  Leading-zero count is what separates the groups, and it is
     * unique per group from six zeros down: 0 -> the 2-bit code (EOB shares the
     * count and is matched first), 1 -> 3 and 4 bits, 2 -> 5 and 8 bits,
     * 3 -> 6 bits, 4 -> 7 bits, 5 -> the ESCAPE alone, 6 -> 10 bits, and then
     * one group per count up to eleven.
     */
    { 0x03,  2,  0,  1 },   /* 11       */
    { 0x03,  3,  1,  1 },   /* 011      */
    { 0x04,  4,  0,  2 },   /* 0100     */
    { 0x05,  4,  2,  1 },   /* 0101     */
    { 0x05,  5,  0,  3 },   /* 00101    */
    { 0x06,  5,  4,  1 },   /* 00110    */
    { 0x07,  5,  3,  1 },   /* 00111    */
    { 0x04,  6,  7,  1 },   /* 000100   */
    { 0x05,  6,  6,  1 },   /* 000101   */
    { 0x06,  6,  1,  2 },   /* 000110   */
    { 0x07,  6,  5,  1 },   /* 000111   */
    { 0x04,  7,  2,  2 },   /* 0000100  */
    { 0x05,  7,  9,  1 },   /* 0000101  */
    { 0x06,  7,  0,  4 },   /* 0000110  */
    { 0x07,  7,  8,  1 },   /* 0000111  */

    /*
     * 8 bits, prefix `00100` — and only that prefix. An earlier pass had a
     * second group at `00101xxx`, which is a PREFIX COLLISION with the 5-bit
     * `00101` above: a mechanical check of every pair caught it, and it is the
     * kind of error that produces a decoder which works on most blocks and
     * desynchronises on the rest.
     */
    { 0x20,  8, 13,  1 },   /* 00100000 */
    { 0x21,  8,  0,  6 },   /* 00100001 */
    { 0x22,  8, 12,  1 },   /* 00100010 */
    { 0x23,  8, 11,  1 },   /* 00100011 */
    { 0x24,  8,  3,  2 },   /* 00100100 */
    { 0x25,  8,  1,  3 },   /* 00100101 */
    { 0x26,  8,  0,  5 },   /* 00100110 */
    { 0x27,  8, 10,  1 },   /* 00100111 */

    /*
     * 10 bits, prefix `0000001`. The same check caught these sitting under
     * `000001`, which is the ESCAPE — every one of them was unreachable, and
     * the escape was swallowing bits that belonged to a coefficient.
     */
    { 0x008, 10, 16,  1 },  /* 0000001000 */
    { 0x009, 10,  5,  2 },  /* 0000001001 */
    { 0x00A, 10,  0,  7 },  /* 0000001010 */
    { 0x00B, 10,  2,  3 },  /* 0000001011 */
    { 0x00C, 10,  1,  4 },  /* 0000001100 */
    { 0x00D, 10, 15,  1 },  /* 0000001101 */
    { 0x00E, 10, 14,  1 },  /* 0000001110 */
    { 0x00F, 10,  4,  2 },  /* 0000001111 */

    /* 12 bits, prefix `00000001` — seven leading zeros, a `1`, four bits. */
    { 0x010, 12,  0, 11 },  /* 000000010000 */
    { 0x011, 12,  8,  2 },
    { 0x012, 12,  4,  3 },
    { 0x013, 12,  0, 10 },
    { 0x014, 12,  2,  4 },
    { 0x015, 12,  7,  2 },
    { 0x016, 12, 21,  1 },
    { 0x017, 12, 20,  1 },
    { 0x018, 12,  0,  9 },
    { 0x019, 12, 19,  1 },
    { 0x01A, 12, 18,  1 },
    { 0x01B, 12,  1,  5 },
    { 0x01C, 12,  3,  3 },
    { 0x01D, 12,  0,  8 },
    { 0x01E, 12,  6,  2 },
    { 0x01F, 12, 17,  1 },

    /*
     * 13 bits, prefix `000000001` — and this group is the whole repair.
     *
     * It had been transcribed as EIGHT codes of TWELVE bits (`000000001` and
     * three more), which is one bit short of every code in it. The
     * leading-zero derivation could not tell the two apart: a group of
     * sixteen 4-bit tails each followed by a sign shows exactly the same
     * "N distinct at width w, 2N at w+1" signature as eight 3-bit tails each
     * followed by a sign, and the pass that derived it picked the shorter
     * reading. Every block reaching one of these codes then read one bit too
     * few and carried on decoding rubbish that still looked like valid codes,
     * which is why 248 frames "decoded exactly" while showing nothing.
     */
    { 0x010, 13, 10,  2 },  /* 0000000010000 */
    { 0x011, 13,  9,  2 },
    { 0x012, 13,  5,  3 },
    { 0x013, 13,  3,  4 },
    { 0x014, 13,  2,  5 },
    { 0x015, 13,  1,  7 },
    { 0x016, 13,  1,  6 },
    { 0x017, 13,  0, 15 },
    { 0x018, 13,  0, 14 },
    { 0x019, 13,  0, 13 },
    { 0x01A, 13,  0, 12 },
    { 0x01B, 13, 26,  1 },
    { 0x01C, 13, 25,  1 },
    { 0x01D, 13, 24,  1 },
    { 0x01E, 13, 23,  1 },
    { 0x01F, 13, 22,  1 },

    /* 14 bits, prefix `0000000001` — run 0, levels 31 down to 16. */
    { 0x010, 14,  0, 31 },  /* 00000000010000 */
    { 0x011, 14,  0, 30 },
    { 0x012, 14,  0, 29 },
    { 0x013, 14,  0, 28 },
    { 0x014, 14,  0, 27 },
    { 0x015, 14,  0, 26 },
    { 0x016, 14,  0, 25 },
    { 0x017, 14,  0, 24 },
    { 0x018, 14,  0, 23 },
    { 0x019, 14,  0, 22 },
    { 0x01A, 14,  0, 21 },
    { 0x01B, 14,  0, 20 },
    { 0x01C, 14,  0, 19 },
    { 0x01D, 14,  0, 18 },
    { 0x01E, 14,  0, 17 },
    { 0x01F, 14,  0, 16 },

    /* 15 bits, prefix `00000000001`. */
    { 0x010, 15,  0, 40 },  /* 000000000010000 */
    { 0x011, 15,  0, 39 },
    { 0x012, 15,  0, 38 },
    { 0x013, 15,  0, 37 },
    { 0x014, 15,  0, 36 },
    { 0x015, 15,  0, 35 },
    { 0x016, 15,  0, 34 },
    { 0x017, 15,  0, 33 },
    { 0x018, 15,  0, 32 },
    { 0x019, 15,  1, 14 },
    { 0x01A, 15,  1, 13 },
    { 0x01B, 15,  1, 12 },
    { 0x01C, 15,  1, 11 },
    { 0x01D, 15,  1, 10 },
    { 0x01E, 15,  1,  9 },
    { 0x01F, 15,  1,  8 },

    /* 16 bits, prefix `000000000001` — the longest, and the last group. */
    { 0x010, 16,  1, 18 },  /* 0000000000010000 */
    { 0x011, 16,  1, 17 },
    { 0x012, 16,  1, 16 },
    { 0x013, 16,  1, 15 },
    { 0x014, 16,  6,  3 },
    { 0x015, 16, 16,  2 },
    { 0x016, 16, 15,  2 },
    { 0x017, 16, 14,  2 },
    { 0x018, 16, 13,  2 },
    { 0x019, 16, 12,  2 },
    { 0x01A, 16, 11,  2 },
    { 0x01B, 16, 31,  1 },
    { 0x01C, 16, 30,  1 },
    { 0x01D, 16, 29,  1 },
    { 0x01E, 16, 28,  1 },
    { 0x01F, 16, 27,  1 }
};


#define AC_COUNT ((u32)(sizeof(k_ac) / sizeof(k_ac[0])))

/* EOB is `10` and ESCAPE is `000001`, both distinct from every AC code. */
#define AC_EOB_CODE    0x02u
#define AC_EOB_LEN     2u
#define AC_ESCAPE_CODE 0x01u
#define AC_ESCAPE_LEN  6u

/*
 * The escape's two field widths, SETTABLE so they can be swept.
 *
 * After `000001` come a run and a level, and nothing in the bitstream announces
 * how wide either is. Rather than assert a layout, the widths are parameters
 * and the disc scores them: a wrong pair desynchronises almost every frame and
 * the right pair should stand out by a wide margin. That is the same argument
 * the code LENGTHS were derived by (#108), applied to the one field the
 * leading-zero method cannot reach — an escape has no Huffman code after it to
 * measure.
 */
static u32 g_esc_run_bits   = 6;
static u32 g_esc_level_bits = 10;

void q2_stx_set_escape_layout(u32 run_bits, u32 level_bits)
{
    g_esc_run_bits   = run_bits ? run_bits : 6;
    g_esc_level_bits = level_bits ? level_bits : 10;
}

/* ------------------------------------------------------------------------- */
/* Why a block gave up                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Three reasons, counted apart, because they are three different repairs:
 *
 *   unmatched  no code has this prefix         -> a LENGTH is wrong
 *   overrun    the coefficient index passed 63 -> a RUN is wrong
 *   dry        the bits ran out                -> either, downstream
 *
 * All three read zero across all 5301 frames of all three films, which is the
 * statement this decoder is judged on. They are kept, rather than deleted with
 * the rest of the hunt that needed them, because a table edit that breaks one
 * of them should say which.
 *
 * What HAS been deleted is the machinery that found the fault: a leading-zero
 * histogram of unmatched lookaheads and a collector for the distinct bit tails
 * in each bucket. It derived four of the five long code groups correctly and
 * the fifth wrong, and what finally settled the table was `bs_num_codes` — the
 * frame's own MDEC DMA length, which is a per-frame check needing no reference
 * at all. The derivation is recorded in openquestions #108 and #115; the code
 * for it is not worth carrying.
 */
u32 q2_stx_fail_unmatched;
u32 q2_stx_fail_overrun;
u32 q2_stx_fail_dry;

/*
 * How many AC (run, level) pairs the last frame produced.
 *
 * The check that settled this format. The frame header's `bs_num_codes` is the
 * length of the DMA that feeds the MDEC: one 16-bit word per block for the DC,
 * one per pair and one for each block's EOB, moved as 32-bit words and padded
 * to a multiple of 32 of them. So
 *
 *     bs_num_codes  ==  round_up_32(ceil((2 * blocks + pairs) / 2))
 *
 * and a frame therefore states its own pair count. That depends on the code
 * LENGTHS and on nothing else — not the run column, not the level column, not
 * the quantiser, not the transform — so it scores a candidate table without a
 * reference, a capture, or a second decoder.
 */
u32 q2_stx_last_pairs;

void q2_stx_reset_stats(void)
{
    q2_stx_fail_unmatched = 0;
    q2_stx_fail_overrun   = 0;
    q2_stx_fail_dry       = 0;
}


u32 q2_stx_code_count(void)
{
    return AC_COUNT;
}

u32 q2_stx_code_bits(u32 i)
{
    return (i < AC_COUNT) ? k_ac[i].code : 0u;
}

bool q2_stx_code_at(u32 i, u32 *len, u32 *run, u32 *level)
{
    if (i >= AC_COUNT)
        return false;
    if (len)   *len   = k_ac[i].len;
    if (run)   *run   = k_ac[i].run;
    if (level) *level = k_ac[i].level;
    return true;
}

/* ------------------------------------------------------------------------- */
static void idct8x8(const s32 in[64], s32 out[64])
{
    /* A plain separable float IDCT. The console's is the MDEC's fixed-point
     * one and differs in the last bit or two; this is a decoder for looking at
     * the movies, not a bit-exact reproduction of the chip, and saying so is
     * better than implying otherwise. */
    static double cs[8][8];
    static bool built = false;
    int u, x, y, v;
    double tmp[64];

    if (!built) {
        for (x = 0; x < 8; x++)
            for (u = 0; u < 8; u++)
                cs[x][u] = ((u == 0) ? 0.353553390593273762 : 0.5) *
                           cos((2.0 * x + 1.0) * u * 3.14159265358979323846
                               / 16.0);
        built = true;
    }

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            double s = 0.0;
            for (u = 0; u < 8; u++)
                s += cs[x][u] * (double)in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    }
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            double s = 0.0;
            for (v = 0; v < 8; v++)
                s += cs[y][v] * tmp[v * 8 + x];
            out[y * 8 + x] = (s32)(s < 0 ? s - 0.5 : s + 0.5);
        }
    }
}

static s32 clamp255(s32 v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return v;
}

/* Decode one 8x8 block into `out` (natural order, already dequantised and
 * inverse-transformed). Returns false when the bitstream desynchronises. */
static bool decode_block(bitreader *b, u32 qscale, s32 out[64])
{
    s32 coeff[64];
    s32 dc;
    u32 n = 0;
    u32 i;

    memset(coeff, 0, sizeof(coeff));

    /* BS v2's DC is a plain signed 10-bit value, not a difference. */
    dc = (s32)br_get(b, 10);
    if (dc & 0x200)
        dc -= 0x400;
    if (b->dry)
        return false;

    coeff[0] = dc * (s32)k_quant[0];

    for (;;) {
        u32 look = br_peek(b, 17);
        u32 run = 0, len = 0;
        s32 level = 0;
        bool got = false;

        if ((look >> 15) == AC_EOB_CODE) {
            br_get(b, AC_EOB_LEN);
            break;
        }

        if ((look >> 11) == AC_ESCAPE_CODE) {
            s32 v;

            br_get(b, AC_ESCAPE_LEN);
            run = br_get(b, g_esc_run_bits);
            v   = (s32)br_get(b, g_esc_level_bits);
            if (v & (1 << (g_esc_level_bits - 1)))
                v -= (1 << g_esc_level_bits);
            level = v;
            got   = true;
            len   = 0;
        } else {
            for (i = 0; i < AC_COUNT; i++) {
                u32 c = k_ac[i].code;
                u32 l = k_ac[i].len;

                if (k_ac[i].level == 0)
                    continue;
                if ((look >> (17 - l)) == c) {
                    run   = k_ac[i].run;
                    level = k_ac[i].level;
                    len   = l;
                    got   = true;
                    break;
                }
            }
            if (!got) {
                q2_stx_fail_unmatched++;
                return false;
            }

            br_get(b, len);
            if (br_get(b, 1))
                level = -level;
        }

        if (b->dry) {
            q2_stx_fail_dry++;
            return false;
        }

        q2_stx_last_pairs++;

        n += run + 1;
        if (n >= 64) {
            q2_stx_fail_overrun++;
            return false;
        }

        {
            u32 nat = k_zigzag[n];
            /*
             * The MDEC's own dequantisation. Removing the `/ 8` was tried and
             * is NOT applied: it changes the decoded frames' roughness by about
             * two rather than the eight the arithmetic predicts, which means
             * the evidence does not support it, and replacing a sourced formula
             * with an unexplained one is worse than leaving it alone.
             */
            coeff[nat] = (level * (s32)qscale * (s32)k_quant[nat]) / 8;
        }
    }

    idct8x8(coeff, out);
    return true;
}

bool q2_stx_frame_decode(const q2_stx_frame *f, u8 *out,
                         u32 *out_blocks, u32 *out_bits)
{
    bitreader br;
    u32 mb_x, mb_y, mbw, mbh, blocks = 0;
    s32 blk[6][64];

    if (out_blocks) *out_blocks = 0;
    if (out_bits)   *out_bits   = 0;

    if (!f || !out || f->width == 0 || f->height == 0)
        return false;
    if (f->size <= 8 || f->size > sizeof(f->data))
        return false;

    /* The eight-byte header at the front is the mirror the container already
     * carries — skip it rather than parsing it twice. */
    q2_stx_last_pairs = 0;
    br_init(&br, f->data + 8, f->size - 8);

    mbw = (f->width  + 15) / 16;
    mbh = (f->height + 15) / 16;

    /*
     * COLUMN-MAJOR macroblocks: the whole left-hand column top to bottom, then
     * the next column. That is the MDEC's own order — the chip is fed a vertical
     * strip at a time — and it is not guessable from the bitstream, because a
     * frame decoded row-major consumes exactly the same bits and produces
     * exactly the same blocks. It shows up only in the picture, as content that
     * is plainly real sitting in the wrong 16x16 cell.
     */
    for (mb_x = 0; mb_x < mbw; mb_x++) {
        for (mb_y = 0; mb_y < mbh; mb_y++) {
            u32 k, px, py;

            /* Cr, Cb, then the four luma quadrants — the MDEC's own order. */
            for (k = 0; k < 6; k++) {
                if (!decode_block(&br, f->qscale, blk[k])) {
                    if (out_blocks) *out_blocks = blocks;
                    if (out_bits)   *out_bits   = br.consumed;
                    return false;
                }
                blocks++;
            }

            for (py = 0; py < 16; py++) {
                for (px = 0; px < 16; px++) {
                    u32 x = mb_x * 16 + px;
                    u32 y = mb_y * 16 + py;
                    s32 yy, cb, cr, r, g, bl;
                    u32 li;

                    if (x >= f->width || y >= f->height)
                        continue;

                    li = (py / 8) * 2 + (px / 8);
                    yy = blk[2 + li][(py % 8) * 8 + (px % 8)] + 128;
                    /* Cr then Cb. Swapping them was tried and changes the
                     * COLOURS of the artefacts without changing their
                     * structure, so it is not the fault and the documented
                     * order stands. */
                    cr = blk[0][(py / 2) * 8 + (px / 2)];
                    cb = blk[1][(py / 2) * 8 + (px / 2)];

                    r  = yy + (s32)(1.402 * cr);
                    g  = yy - (s32)(0.344136 * cb + 0.714136 * cr);
                    bl = yy + (s32)(1.772 * cb);

                    out[(y * f->width + x) * 3 + 0] = (u8)clamp255(r);
                    out[(y * f->width + x) * 3 + 1] = (u8)clamp255(g);
                    out[(y * f->width + x) * 3 + 2] = (u8)clamp255(bl);
                }
            }
        }
    }

    if (out_blocks) *out_blocks = blocks;
    if (out_bits)   *out_bits   = br.consumed;
    return true;
}
