/*
 * stxenc.c — see stxenc.h.
 */
#include "stxenc.h"

#include "xa.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Bits out                                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The mirror of stx.c's reader: 16-bit LITTLE-endian words, filled most
 * significant bit first. Get either half of that wrong and the file decodes to
 * nothing at all, which is at least a loud failure.
 */
typedef struct bitwriter {
    u8 *p;
    u32 cap;        /* bytes available                     */
    u32 at;         /* bytes written                       */
    u32 acc;        /* pending bits, right-aligned         */
    u32 nbits;      /* how many of them                    */
    u32 total;      /* bits handed in, for the DMA length  */
    bool full;
} bitwriter;

static void bw_init(bitwriter *b, u8 *p, u32 cap)
{
    memset(b, 0, sizeof(*b));
    b->p   = p;
    b->cap = cap;
}

static void bw_put(bitwriter *b, u32 value, u32 n)
{
    while (n--) {
        b->acc = (b->acc << 1) | ((value >> n) & 1u);
        b->nbits++;
        b->total++;

        if (b->nbits == 16) {
            if (b->at + 2 > b->cap) {
                b->full = true;
            } else {
                q2_wr_u16(b->p + b->at, (u16)b->acc);
                b->at += 2;
            }
            b->acc   = 0;
            b->nbits = 0;
        }
    }
}

/* Pad to a whole word with zeros. The decoder reads a 17-bit lookahead past the
 * last block, so the padding is READ — it just is not interpreted, because the
 * block count has already been met. */
static void bw_flush(bitwriter *b)
{
    while (b->nbits)
        bw_put(b, 0, 1);
}

/* ------------------------------------------------------------------------- */
/* The AC table, indexed the other way round                                  */
/* ------------------------------------------------------------------------- */
/*
 * The decoder walks the table looking for a code; the encoder needs to walk it
 * looking for a (run, level). Built once from `q2_stx_code_at`, so there is
 * exactly one copy of MPEG-1's Table B.14 in this project and it is the one
 * that was verified against 5,301 frames.
 *
 * run 0..31 and level 1..40 is what B.14 carries; everything outside that, and
 * everything inside it the table happens not to name, takes the escape.
 */
#define ENC_MAX_RUN    32
#define ENC_MAX_LEVEL  41

typedef struct enc_code { u16 bits; u8 len; } enc_code;

static enc_code g_enc[ENC_MAX_RUN][ENC_MAX_LEVEL];
static bool     g_enc_built;

static void enc_build(void)
{
    u32 n = q2_stx_code_count();
    u32 i;

    if (g_enc_built)
        return;

    memset(g_enc, 0, sizeof(g_enc));
    for (i = 0; i < n; i++) {
        u32 len = 0, run = 0, level = 0;

        if (!q2_stx_code_at(i, &len, &run, &level))
            continue;
        if (run >= ENC_MAX_RUN || level >= ENC_MAX_LEVEL || level == 0)
            continue;
        /* First writer wins, which matches the decoder taking the first match:
         * if a (run, level) appeared twice the decoder could only ever emit the
         * earlier one, so the encoder must not choose the later. */
        if (g_enc[run][level].len == 0) {
            g_enc[run][level].bits = (u16)q2_stx_code_bits(i);
            g_enc[run][level].len  = (u8)len;
        }
    }
    g_enc_built = true;
}

/* ------------------------------------------------------------------------- */
/* Colour and the transform                                                   */
/* ------------------------------------------------------------------------- */
/*
 * The decoder's conversion, solved for the other side:
 *
 *     r = y + 1.402 cr
 *     g = y - 0.344136 cb - 0.714136 cr
 *     b = y + 1.772 cb
 *
 * is BT.601 full-range, so the forward is BT.601's forward. Y comes back with
 * the 128 the decoder adds already taken off, because a block's DC is signed
 * and the level shift lives in the decoder.
 */
static void rgb_to_ycc(const u8 *px, double *y, double *cb, double *cr)
{
    double r = (double)px[0], g = (double)px[1], b = (double)px[2];

    *y  =  0.299    * r + 0.587    * g + 0.114    * b - 128.0;
    *cb = -0.168736 * r - 0.331264 * g + 0.5      * b;
    *cr =  0.5      * r - 0.418688 * g - 0.081312 * b;
}

/* The decoder's cosine matrix, and the forward transform is its transpose. */
static const double *dct_matrix(void)
{
    static double cs[8][8];
    static bool built = false;

    if (!built) {
        int x, u;

        for (x = 0; x < 8; x++)
            for (u = 0; u < 8; u++)
                cs[x][u] = ((u == 0) ? 0.353553390593273762 : 0.5) *
                           cos((2.0 * x + 1.0) * u * 3.14159265358979323846
                               / 16.0);
        built = true;
    }
    return &cs[0][0];
}

/*
 * f (8x8 spatial) -> F (8x8 frequency), natural order.
 *
 * The decoder computes out[y][x] = SUM_u SUM_v cs[x][u] cs[y][v] F[v][u], so
 * the forward is F[v][u] = SUM_x SUM_y cs[x][u] cs[y][v] f[y][x] — the same
 * matrix, applied the other way round. Writing it as its transpose rather than
 * from a textbook is the point: any scale factor the decoder folded in is
 * folded in here too, automatically.
 */
static void fdct8x8(const double in[64], s16 out[64])
{
    const double *cs = dct_matrix();
    double tmp[64];
    int u, v, x, y;

    for (y = 0; y < 8; y++) {
        for (u = 0; u < 8; u++) {
            double s = 0.0;

            for (x = 0; x < 8; x++)
                s += cs[x * 8 + u] * in[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    }
    for (u = 0; u < 8; u++) {
        for (v = 0; v < 8; v++) {
            double s = 0.0;
            long   r;

            for (y = 0; y < 8; y++)
                s += cs[y * 8 + v] * tmp[y * 8 + u];
            r = (long)(s < 0 ? s - 0.5 : s + 0.5);
            if (r >  32767) r =  32767;
            if (r < -32768) r = -32768;
            out[v * 8 + u] = (s16)r;
        }
    }
}

bool q2_stx_transform_frame(q2_stx_transform *t, const u8 *rgb,
                            u32 width, u32 height)
{
    u32 mbw, mbh, mb_x, mb_y;
    u32 blocks = 0;

    if (!t || !rgb || !width || !height)
        return false;
    if (width > Q2_STX_WIDTH || height > Q2_STX_HEIGHT)
        return false;

    mbw = (width  + 15u) / 16u;
    mbh = (height + 15u) / 16u;
    if (mbw * mbh * 6u > Q2_STX_MAX_BLOCKS)
        return false;

    t->width  = width;
    t->height = height;

    /* COLUMN-MAJOR, the same order the decoder reads them in: the whole
     * left-hand column of macroblocks top to bottom, then the next. A
     * row-major encoder produces a file that decodes to real content in the
     * wrong cells, which is the exact bug this order was found by. */
    for (mb_x = 0; mb_x < mbw; mb_x++) {
        for (mb_y = 0; mb_y < mbh; mb_y++) {
            double luma[4][64], chb[64], chr[64];
            u32 px, py, k;

            for (py = 0; py < 16; py++) {
                for (px = 0; px < 16; px++) {
                    u32 x = mb_x * 16u + px;
                    u32 y = mb_y * 16u + py;
                    const u8 *p;
                    double yy, cb, cr;
                    u32 li;

                    /* A macroblock past the edge repeats the edge rather than
                     * sampling black, which would put a hard step into the
                     * transform and cost bits for something nobody sees. */
                    if (x >= width)  x = width - 1;
                    if (y >= height) y = height - 1;

                    p = rgb + ((size_t)y * width + x) * 3;
                    rgb_to_ycc(p, &yy, &cb, &cr);

                    li = (py / 8) * 2 + (px / 8);
                    luma[li][(py % 8) * 8 + (px % 8)] = yy;

                    /*
                     * Chroma is 4:2:0 and the decoder reads it at [py/2][px/2],
                     * so the four pixels of each 2x2 are averaged into that one
                     * sample — the plain box filter, which is what the decoder's
                     * nearest-neighbour read is the inverse of.
                     */
                    if ((px & 1) == 0 && (py & 1) == 0) {
                        chb[(py / 2) * 8 + (px / 2)] = 0.0;
                        chr[(py / 2) * 8 + (px / 2)] = 0.0;
                    }
                    chb[(py / 2) * 8 + (px / 2)] += cb * 0.25;
                    chr[(py / 2) * 8 + (px / 2)] += cr * 0.25;
                }
            }

            /* Cr, Cb, then the four luma quadrants — the MDEC's own order. */
            fdct8x8(chr, t->coeff[blocks++]);
            fdct8x8(chb, t->coeff[blocks++]);
            for (k = 0; k < 4; k++)
                fdct8x8(luma[k], t->coeff[blocks++]);
        }
    }

    t->blocks = blocks;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Quantise and code                                                          */
/* ------------------------------------------------------------------------- */
static s32 divide_round(s32 num, s32 den)
{
    if (den <= 0)
        return 0;
    return (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
}

bool q2_stx_encode_at(const q2_stx_transform *t, u32 qscale,
                      q2_stx_encoded *out)
{
    const u8 *quant  = q2_stx_quant_table();
    const u8 *zigzag = q2_stx_zigzag_table();
    u32 esc_run_bits = 6, esc_level_bits = 10;
    bitwriter bw;
    u32 blk;
    u32 words, half;

    if (!t || !out || !t->blocks || qscale == 0)
        return false;

    enc_build();
    q2_stx_get_escape_layout(&esc_run_bits, &esc_level_bits);

    memset(out, 0, sizeof(*out));
    out->qscale = qscale;
    out->blocks = t->blocks;

    /* Eight bytes of header first; it is filled in once the counts are known,
     * and it is the SAME eight bytes the sector header mirrors. */
    bw_init(&bw, out->data + 8, (u32)sizeof(out->data) - 8u);

    for (blk = 0; blk < t->blocks; blk++) {
        const s16 *F = t->coeff[blk];
        s32 level[64];
        s32 dc;
        u32 n, last = 0, run = 0;

        /*
         * The DC is a plain signed 10-bit value — no difference from the last
         * block, which is what makes this BS v2 rather than v3 — and the
         * decoder scales it by quant[0] and nothing else.
         */
        dc = divide_round((s32)F[0], (s32)quant[0]);
        if (dc >  511) dc =  511;
        if (dc < -512) dc = -512;
        bw_put(&bw, (u32)dc & 0x3FFu, 10);

        /*
         * The ACs, in scan order. `level = round(F * 8 / (qscale * quant))` is
         * the inverse of the decoder's `(level * qscale * quant) / 8`, and the
         * `/ 8` is carried rather than cancelled for the reason stx.c gives:
         * the formula is sourced, and dropping it makes the frames measurably
         * no better.
         */
        for (n = 1; n < 64; n++) {
            u32 nat = zigzag[n];
            s32 den = (s32)qscale * (s32)quant[nat];

            level[n] = divide_round((s32)F[nat] * 8, den);
            if (level[n])
                last = n;
        }

        for (n = 1; n <= last; n++) {
            s32 v = level[n];
            u32 mag;

            if (v == 0) {
                run++;
                continue;
            }

            mag = (u32)(v < 0 ? -v : v);
            out->pairs++;

            if (run < ENC_MAX_RUN && mag < ENC_MAX_LEVEL &&
                g_enc[run][mag].len) {
                bw_put(&bw, g_enc[run][mag].bits, g_enc[run][mag].len);
                bw_put(&bw, (v < 0) ? 1u : 0u, 1);
            } else {
                /*
                 * The escape: `000001`, then a raw run and a raw signed level
                 * with no sign bit of their own. The level field is ten bits,
                 * so a coefficient past its range is CLAMPED rather than
                 * written wrapped — a wrapped level is a bright wrong pixel and
                 * a clamped one is a slightly flat one.
                 */
                s32 lim = (s32)1 << (esc_level_bits - 1);
                s32 cl  = v;

                if (cl >  lim - 1) cl =  lim - 1;
                if (cl < -lim)     cl = -lim;

                bw_put(&bw, Q2_STX_ESCAPE_BITS, Q2_STX_ESCAPE_LEN);
                bw_put(&bw, run & (((u32)1 << esc_run_bits) - 1u),
                       esc_run_bits);
                bw_put(&bw, (u32)cl & (((u32)1 << esc_level_bits) - 1u),
                       esc_level_bits);
                out->escapes++;
            }
            run = 0;
        }

        bw_put(&bw, Q2_STX_EOB_BITS, Q2_STX_EOB_LEN);

        if (bw.full)
            return false;
    }

    bw_flush(&bw);
    if (bw.full)
        return false;

    /*
     * `bs_num_codes` — the MDEC's DMA length, and the one field a frame can be
     * checked against with no reference at all (stx.h). One 16-bit word per
     * block for the DC, one per pair and one per block's EOB; the DMA moves
     * longwords and its length is padded to a multiple of 32 of them.
     */
    words = 2u * out->blocks + out->pairs;
    half  = (words + 1u) / 2u;
    out->num_codes = ((half + 31u) / 32u) * 32u;

    /* The eight-byte bitstream header, which the sector header then mirrors.
     * Do NOT also prepend one at demux time: the disc's chunk 0 already
     * carries this, in 5,301 frames out of 5,301. */
    q2_wr_u16(out->data + 0, (u16)out->num_codes);
    q2_wr_u16(out->data + 2, Q2_STX_BS_MAGIC);
    q2_wr_u16(out->data + 4, (u16)qscale);
    q2_wr_u16(out->data + 6, Q2_STX_BS_VERSION);

    out->bits = bw.total;
    out->size = 8u + bw.at;
    /* Always a multiple of four on the disc, and the decoder is handed the
     * size as a byte count, so round up into the zeros already there. */
    out->size = (out->size + 3u) & ~3u;
    if (out->size > sizeof(out->data))
        return false;

    return true;
}

bool q2_stx_encode_fit(const q2_stx_transform *t, u32 budget, u32 hint,
                       q2_stx_encoded *out)
{
    u32 q;

    if (!t || !out || !budget)
        return false;

    /*
     * Walk UP from the hint. A lower qscale is a finer quantiser and more bits,
     * so the first one that fits is the best one that fits — and starting at
     * the previous frame's answer costs one or two attempts a frame instead of
     * a scan from 1 every time, because a film's complexity moves slowly.
     */
    if (hint == 0 || hint > 31u)
        hint = 1u;

    for (q = hint; q <= 31u; q++)
        if (q2_stx_encode_at(t, q, out) && out->size <= budget)
            return true;

    /* The hint was too high and something simpler came along: try below it. */
    for (q = 1u; q < hint; q++)
        if (q2_stx_encode_at(t, q, out) && out->size <= budget)
            return true;

    return false;
}

/* ------------------------------------------------------------------------- */
/* The container                                                              */
/* ------------------------------------------------------------------------- */
void q2_stx_writer_init(q2_stx_writer *w, u32 width, u32 height, bool audio,
                        q2_stx_sink sink, void *user)
{
    if (!w)
        return;

    memset(w, 0, sizeof(*w));
    w->sink         = sink;
    w->user         = user;
    w->width        = width;
    w->height       = height;
    w->audio        = audio;
    w->frame_number = 1;
    w->qscale_hint  = 1;
    w->qscale_min   = 0xFFFFFFFFu;
}

/* Hand one audio slot to the sink, taking 2016 stereo frames off the queue. */
static bool writer_emit_audio(q2_stx_writer *w)
{
    u8  payload[CD_SECTOR_FORM2];
    u32 take = w->pcm_frames;

    if (take > XA_FRAMES_PER_SECTOR)
        take = XA_FRAMES_PER_SECTOR;

    memset(payload, 0, sizeof(payload));
    {
        /* The writer keeps the predictor rather than the encoder, so that a
         * sector is packed with the history the previous one left — the whole
         * point of XA's stride. */
        q2_xa_encoder enc;

        memcpy(enc.prev1, w->adpcm_prev1, sizeof(enc.prev1));
        memcpy(enc.prev2, w->adpcm_prev2, sizeof(enc.prev2));
        q2_xa_encode_sector(&enc, w->pcm, take, payload);
        memcpy(w->adpcm_prev1, enc.prev1, sizeof(enc.prev1));
        memcpy(w->adpcm_prev2, enc.prev2, sizeof(enc.prev2));
    }

    /* The 20 bytes past the ADPCM are unused and are zero in all 70,663 audio
     * sectors on the disc; `payload` was cleared, so they already are. */
    memmove(w->pcm, w->pcm + (size_t)take * 2,
            (size_t)(w->pcm_frames - take) * 2 * sizeof(w->pcm[0]));
    w->pcm_frames -= take;
    w->audio_sectors++;

    if (w->sink && !w->sink(w->user, w->sector, Q2_STX_FORM_AUDIO,
                            payload, CD_SECTOR_FORM2))
        return false;
    w->sector++;
    return true;
}

static bool writer_emit_null(q2_stx_writer *w)
{
    u8 payload[Q2_STX_SECTOR_SIZE];

    memset(payload, 0, sizeof(payload));
    w->null_sectors++;
    if (w->sink && !w->sink(w->user, w->sector, Q2_STX_FORM_NULL,
                            payload, Q2_STX_SECTOR_SIZE))
        return false;
    w->sector++;
    return true;
}

static bool writer_emit_chunk(q2_stx_writer *w, const q2_stx_encoded *f,
                              u32 chunk, u32 chunks)
{
    u8  payload[Q2_STX_SECTOR_SIZE];
    u32 off = chunk * Q2_STX_VIDEO_PAYLOAD;
    u32 n   = 0;

    memset(payload, 0, sizeof(payload));

    q2_wr_u16(payload + 0x00, Q2_STX_MAGIC);
    q2_wr_u16(payload + 0x02, Q2_STX_SUBTYPE_MDEC);
    q2_wr_u16(payload + 0x04, (u16)chunk);
    q2_wr_u16(payload + 0x06, (u16)chunks);
    q2_wr_u32(payload + 0x08, w->frame_number);
    q2_wr_u32(payload + 0x0C, f->size);
    q2_wr_u16(payload + 0x10, (u16)w->width);
    q2_wr_u16(payload + 0x12, (u16)w->height);
    /* The mirror: bytes 0x14..0x1B are the bitstream's own first eight, in
     * every sector of the frame and not only the first. */
    memcpy(payload + 0x14, f->data, 8);
    q2_wr_u32(payload + 0x1C, 0);

    if (off < f->size) {
        n = f->size - off;
        if (n > Q2_STX_VIDEO_PAYLOAD)
            n = Q2_STX_VIDEO_PAYLOAD;
        memcpy(payload + Q2_STX_HEADER_SIZE, f->data + off, n);
    }

    w->video_sectors++;
    if (w->sink && !w->sink(w->user, w->sector, Q2_STX_FORM_VIDEO,
                            payload, Q2_STX_SECTOR_SIZE))
        return false;
    w->sector++;
    return true;
}

bool q2_stx_writer_frame(q2_stx_writer *w, const u8 *rgb,
                         const s16 *pcm, u32 pcm_frames)
{
    static q2_stx_transform t;      /* 184 KB: not a stack frame */
    static q2_stx_encoded   f;
    u32 chunks, budget, chunk;

    if (!w || !rgb)
        return false;

    /*
     * THE CADENCE IS THE BUDGET. 6, 5, 5, 5 keyed to (frame_number - 1) % 4,
     * with zero violations in 5,301 frames — six sectors per frame overall,
     * which with audio taking one slot in eight is exactly 25.000 fps off a
     * 150-sector-per-second drive. See stxenc.h.
     */
    chunks = (((w->frame_number - 1u) % 4u) == 0u) ? 6u : 5u;
    budget = chunks * Q2_STX_VIDEO_PAYLOAD;

    if (!q2_stx_transform_frame(&t, rgb, w->width, w->height))
        return false;
    if (!q2_stx_encode_fit(&t, budget, w->qscale_hint, &f))
        return false;
    w->qscale_hint = f.qscale;

    if (f.qscale < w->qscale_min) w->qscale_min = f.qscale;
    if (f.qscale > w->qscale_max) w->qscale_max = f.qscale;
    w->qscale_sum += f.qscale;

    /* Queue the audio that plays under this frame. Overflow is dropped rather
     * than overrunning: a caller feeding more than two sectors ahead is not
     * feeding a 25 fps film. */
    if (pcm && pcm_frames && w->audio) {
        u32 room = (u32)(sizeof(w->pcm) / (2 * sizeof(w->pcm[0]))) -
                   w->pcm_frames;
        u32 take = pcm_frames < room ? pcm_frames : room;

        memcpy(w->pcm + (size_t)w->pcm_frames * 2, pcm,
               (size_t)take * 2 * sizeof(w->pcm[0]));
        w->pcm_frames += take;
    }

    for (chunk = 0; chunk < chunks; chunk++) {
        /* Slot 7 of every 8 belongs to the audio, whatever the video wanted. */
        while (q2_stx_sector_is_audio(w->sector)) {
            if (w->audio) {
                if (!writer_emit_audio(w))
                    return false;
            } else if (!writer_emit_null(w)) {
                return false;
            }
        }
        if (!writer_emit_chunk(w, &f, chunk, chunks))
            return false;
    }

    w->frame_number++;
    w->frames++;
    return true;
}

bool q2_stx_writer_finish(q2_stx_writer *w)
{
    if (!w)
        return false;

    /*
     * The tail. The video region ends where the last frame ended and the audio
     * keeps going, so the video slots behind it are NULLED — which is the
     * disc's own shape, not a courtesy: `q2_stx_frame_next` stops at the first
     * video slot that is not a frame header, and 0 of the disc's 556 null
     * sectors contain a non-zero byte.
     */
    while (w->audio && w->pcm_frames) {
        if (q2_stx_sector_is_audio(w->sector)) {
            if (!writer_emit_audio(w))
                return false;
        } else if (!writer_emit_null(w)) {
            return false;
        }
    }

    /* And finish on a whole interleave period, so the file is a round number
     * of 8-sector groups the way all three of the disc's are. */
    while ((w->sector % Q2_STX_INTERLEAVE) != 0) {
        if (q2_stx_sector_is_audio(w->sector) && w->audio) {
            if (!writer_emit_audio(w))
                return false;
        } else if (!writer_emit_null(w)) {
            return false;
        }
    }

    return true;
}
