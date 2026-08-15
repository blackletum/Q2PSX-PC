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
                b->dry = true;
                return v << 1;
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
    /* 2..7 bits â€” the common codes, and the ones the data confirms. */
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
    { 0x05,  7,  8,  1 },   /* 0000101  */
    { 0x06,  7,  0,  4 },   /* 0000110  */
    { 0x07,  7,  9,  1 },   /* 0000111  */

    /*
     * 8 bits, prefix `00100` â€” and only that prefix. An earlier pass had a
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
     * `000001`, which is the ESCAPE â€” every one of them was unreachable, and
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

    /*
     * 12 bits, and these two prefixes are DERIVED rather than recalled.
     *
     * Every lookahead the table could not match was bucketed by its run of
     * leading zeros and its tail collected: the buckets are 7 zeros (845 of
     * 1316) and 8 zeros (297), and their tails fill exactly 4 and 3 bits with
     * the next bit taking both values â€” which is the signature of a fixed-width
     * code followed by a sign. 7 zeros + a 1 + 4 bits and 8 zeros + a 1 + 3
     * bits are both twelve. The remaining buckets (9, 10 and 11 zeros, 174
     * samples) each hold ONE distinct pattern at every width, which is what a
     * reader that has already lost sync sees in a run of padding, not a group.
     *
     * The run/level assignments below are B.14's and are NOT derived â€” sync
     * depends only on the lengths, so a wrong assignment here is a wrong
     * picture and not a failed decode. Which of those two the port is looking
     * at is exactly what the frame counter now distinguishes.
     */
    { 0x010, 12,  0, 11 },  /* 000000010000 */
    { 0x011, 12,  0, 12 },
    { 0x012, 12,  0, 13 },
    { 0x013, 12,  0, 14 },
    { 0x014, 12,  1,  5 },
    { 0x015, 12,  1,  6 },
    { 0x016, 12,  1,  7 },
    { 0x017, 12,  2,  4 },
    { 0x018, 12,  3,  3 },
    { 0x019, 12,  5,  2 },
    { 0x01A, 12,  6,  2 },
    { 0x01B, 12,  7,  2 },
    { 0x01C, 12,  8,  2 },
    { 0x01D, 12,  9,  2 },
    { 0x01E, 12, 17,  1 },
    { 0x01F, 12, 18,  1 },

    { 0x008, 12, 19,  1 },  /* 000000001000 */
    { 0x009, 12, 20,  1 },
    { 0x00A, 12, 21,  1 },
    { 0x00B, 12, 22,  1 },
    { 0x00C, 12, 23,  1 },
    { 0x00D, 12, 24,  1 },
    { 0x00E, 12, 25,  1 },
    { 0x00F, 12, 26,  1 },

    /*
     * 14, 15 and 16 bits, all three DERIVED the same way as the twelves.
     *
     * With the twelves in, every remaining first-failure fell into the 9-, 10-
     * and 11-zero buckets, and each one's tail fills exactly FOUR bits with the
     * fifth taking both values â€” a fixed-width code and its sign. So the groups
     * are `<9 zeros> 1 <4>`, `<10 zeros> 1 <4>` and `<11 zeros> 1 <4>`:
     * fourteen, fifteen and sixteen bits, sixteen codes each.
     *
     * The run/level assignments are B.14's and are NOT derived. Synchronisation
     * depends only on the lengths, so an assignment error here shows up as a
     * wrong PICTURE from a frame that decoded, which the frame counter and the
     * eye can tell apart â€” a length error shows up as a frame that does not
     * decode at all.
     */
    { 0x0010, 14, 10,  2 },   /* 00000000010000 */
    { 0x0011, 14, 11,  2 },
    { 0x0012, 14, 12,  2 },
    { 0x0013, 14, 13,  2 },
    { 0x0014, 14, 14,  2 },
    { 0x0015, 14, 15,  2 },
    { 0x0016, 14, 16,  2 },
    { 0x0017, 14, 27,  1 },
    { 0x0018, 14, 28,  1 },
    { 0x0019, 14, 29,  1 },
    { 0x001A, 14, 30,  1 },
    { 0x001B, 14, 31,  1 },
    { 0x001C, 14,  0, 15 },
    { 0x001D, 14,  0, 16 },
    { 0x001E, 14,  0, 17 },
    { 0x001F, 14,  0, 18 },

    { 0x0010, 15,  0, 19 },   /* 000000000010000 */
    { 0x0011, 15,  0, 20 },
    { 0x0012, 15,  0, 21 },
    { 0x0013, 15,  0, 22 },
    { 0x0014, 15,  0, 23 },
    { 0x0015, 15,  0, 24 },
    { 0x0016, 15,  0, 25 },
    { 0x0017, 15,  0, 26 },
    { 0x0018, 15,  1,  8 },
    { 0x0019, 15,  1,  9 },
    { 0x001A, 15,  1, 10 },
    { 0x001B, 15,  1, 11 },
    { 0x001C, 15,  1, 12 },
    { 0x001D, 15,  1, 13 },
    { 0x001E, 15,  1, 14 },
    { 0x001F, 15,  1, 15 },

    { 0x0010, 16,  1, 16 },   /* 0000000000010000 */
    { 0x0011, 16,  1, 17 },
    { 0x0012, 16,  1, 18 },
    { 0x0013, 16,  6,  3 },
    { 0x0014, 16, 16,  2 },
    { 0x0015, 16, 17,  2 },
    { 0x0016, 16, 18,  2 },
    { 0x0017, 16, 19,  2 },
    { 0x0018, 16, 20,  2 },
    { 0x0019, 16, 21,  2 },
    { 0x001A, 16, 22,  2 },
    { 0x001B, 16, 23,  2 },
    { 0x001C, 16, 24,  2 },
    { 0x001D, 16, 25,  2 },
    { 0x001E, 16, 26,  2 },
    { 0x001F, 16, 27,  2 }
};


#define AC_COUNT ((u32)(sizeof(k_ac) / sizeof(k_ac[0])))

/* EOB is `10` and ESCAPE is `000001`, both distinct from every AC code. */
#define AC_EOB_CODE    0x02u
#define AC_EOB_LEN     2u
#define AC_ESCAPE_CODE 0x01u
#define AC_ESCAPE_LEN  6u

/* ------------------------------------------------------------------------- */
/* What the table cannot match yet                                            */
/* ------------------------------------------------------------------------- */
/*
 * A decoder that desynchronises is only useful if it says WHERE. Every
 * unmatched 17-bit lookahead is recorded by its leading-zero count, which is
 * what identifies a Huffman group: MPEG-1's Table B.14 is organised by run of
 * leading zeros, so "everything unmatched has 11 leading zeros" names the
 * missing group exactly, and a scatter across many counts would mean the
 * problem is elsewhere.
 */
#define Q2_STX_LZ_MAX 18
static u32 g_stx_unmatched[Q2_STX_LZ_MAX];
static u32 g_stx_unmatched_total;

/*
 * And the PATTERNS themselves, deduplicated.
 *
 * The leading-zero histogram says which GROUPS are missing; it does not say how
 * long their codes are, and length is the only thing synchronisation depends
 * on. But the data says that too: a Huffman code is prefix-free, so for each
 * group the bits AFTER the leading `1` take every value of some fixed width and
 * no more. Collect the distinct tails and count them — sixteen distinct 4-bit
 * tails and nothing longer means the group is `<n zeros> 1 <4 bits>`, and the
 * code length falls out as n + 5.
 *
 * That is a derivation from the disc rather than a recollection of a table,
 * which matters here: two of the groups transcribed from memory were wrong in
 * ways that only a prefix check caught.
 */
#define Q2_STX_PAT_MAX 256
static u32 g_stx_pat[Q2_STX_PAT_MAX];
static u32 g_stx_pat_lz[Q2_STX_PAT_MAX];
static u32 g_stx_pat_n;

u32 q2_stx_last_look;
u32 q2_stx_last_bits;

/*
 * WHY a block gave up, counted separately, because the three reasons are three
 * different faults and they had all been reported as "desynchronised".
 *
 *   unmatched  no code has this prefix        -> a LENGTH is missing
 *   overrun    the coefficient index passed 63 -> a RUN value is wrong
 *   dry        the bits ran out                -> either, downstream
 */
u32 q2_stx_fail_unmatched;
u32 q2_stx_fail_overrun;
u32 q2_stx_fail_dry;

/*
 * WHICH code overran, by its length — and by whether it was the ESCAPE, whose
 * run is six raw bits rather than a table entry. "A run value is wrong" is a
 * whole column; this says which row.
 */
u32 q2_stx_overrun_by_len[20];
u32 q2_stx_overrun_escape;
u32 q2_stx_overrun_run_max;

static void q2_stx_unmatched(u32 look)
{
    u32 lz = 0, i;

    q2_stx_last_look = look;

    while (lz < 17 && ((look >> (16 - lz)) & 1u) == 0)
        lz++;

    if (lz < Q2_STX_LZ_MAX)
        g_stx_unmatched[lz]++;
    g_stx_unmatched_total++;

    /*
     * The bits after the leading `1`, left-aligned into eight.
     *
     * This used to shift by `17 - (lz+1) - 8`, which goes NEGATIVE as soon as
     * lz reaches 8 — so every deep bucket reported one constant tail and looked
     * like padding rather than a group. It was an artefact of the analysis, not
     * a fact about the data, and it sent a whole pass looking in the wrong
     * place. Aligning to the top of the byte instead is defined for every lz.
     */
    if (lz + 1 < 17) {
        u32 avail = 17u - lz - 1u;
        u32 tail  = look & ((1u << avail) - 1u);

        tail = (avail >= 8u) ? (tail >> (avail - 8u)) : (tail << (8u - avail));

        for (i = 0; i < g_stx_pat_n; i++)
            if (g_stx_pat[i] == tail && g_stx_pat_lz[i] == lz)
                return;
        if (g_stx_pat_n < Q2_STX_PAT_MAX) {
            g_stx_pat[g_stx_pat_n]    = tail;
            g_stx_pat_lz[g_stx_pat_n] = lz;
            g_stx_pat_n++;
        }
    }
}

u32 q2_stx_unmatched_report(u32 *by_leading_zeros, u32 max)
{
    u32 i;

    for (i = 0; i < max && i < Q2_STX_LZ_MAX; i++)
        by_leading_zeros[i] = g_stx_unmatched[i];
    return g_stx_unmatched_total;
}

u32 q2_stx_unmatched_tails(u32 lz, u32 width, u32 *out, u32 max)
{
    u32 i, n = 0, shift = 8 - (width > 8 ? 8 : width);

    for (i = 0; i < g_stx_pat_n; i++) {
        u32 t, k;
        bool seen = false;

        if (g_stx_pat_lz[i] != lz)
            continue;
        t = g_stx_pat[i] >> shift;
        for (k = 0; k < n; k++)
            if (out[k] == t) { seen = true; break; }
        if (seen)
            continue;
        if (n < max)
            out[n] = t;
        n++;
    }

    return n;
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
            run = br_get(b, 6);
            v   = (s32)br_get(b, 10);
            if (v & 0x200)
                v -= 0x400;
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
                q2_stx_unmatched(look);
                q2_stx_last_bits = (b->bytes - b->at) * 8u + b->bits;
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

        n += run + 1;
        if (n >= 64) {
            q2_stx_fail_overrun++;
            if (len < 20)
                q2_stx_overrun_by_len[len]++;
            if (len == 0)
                q2_stx_overrun_escape++;
            if (run > q2_stx_overrun_run_max)
                q2_stx_overrun_run_max = run;
            return false;
        }

        {
            u32 nat = k_zigzag[n];
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
    br_init(&br, f->data + 8, f->size - 8);

    mbw = (f->width  + 15) / 16;
    mbh = (f->height + 15) / 16;

    for (mb_y = 0; mb_y < mbh; mb_y++) {
        for (mb_x = 0; mb_x < mbw; mb_x++) {
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
