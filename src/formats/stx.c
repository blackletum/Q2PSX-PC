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
    /* 2..7 bits — the common codes, and the ones the data confirms. */
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
    { 0x00F, 10,  4,  2 }   /* 0000001111 */
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

static void q2_stx_unmatched(u32 look)
{
    u32 lz = 0;

    while (lz < 17 && ((look >> (16 - lz)) & 1u) == 0)
        lz++;

    if (lz < Q2_STX_LZ_MAX)
        g_stx_unmatched[lz]++;
    g_stx_unmatched_total++;
}

u32 q2_stx_unmatched_report(u32 *by_leading_zeros, u32 max)
{
    u32 i;

    for (i = 0; i < max && i < Q2_STX_LZ_MAX; i++)
        by_leading_zeros[i] = g_stx_unmatched[i];
    return g_stx_unmatched_total;
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
                return false;
            }

            br_get(b, len);
            if (br_get(b, 1))
                level = -level;
        }

        if (b->dry)
            return false;

        n += run + 1;
        if (n >= 64)
            return false;

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
