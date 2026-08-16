/*
 * stxenc.h — writing a `.STX`: the inverse of stx.h, all the way to sectors.
 *
 * `stx.h` reads the disc's three films. This makes one. Not because the port
 * needs to — it plays what is there — but because a format is only READ when
 * you can write it back: a decoder can be wrong in a way no picture reveals
 * (a quantiser off by a constant, a zigzag transposed, a DC scale folded into
 * the IDCT) and still produce something that looks like video. An encoder
 * built as the strict inverse cannot hide those. It either round-trips through
 * the same decoder or it does not.
 *
 * And it is what a port needs to REPLACE a cinematic: new footage, a subtitled
 * cut, a fan translation. The 320x192 25 fps BS v2 stream this writes is the
 * one the console's MDEC eats, and the sectors it writes are the ones the
 * drive delivers.
 *
 * ---------------------------------------------------------------------------
 * What is inverted, and what is chosen
 * ---------------------------------------------------------------------------
 * INVERTED, exactly, against the decoder's own tables (`q2_stx_quant_table`,
 * `q2_stx_zigzag_table`, `q2_stx_code_at`):
 *
 *     RGB -> YCbCr 4:2:0, six blocks per macroblock in the MDEC's order
 *     (Cr, Cb, then the four luma quadrants), macroblocks COLUMN-MAJOR
 *     forward DCT with the decoder's own cosine matrix
 *     DC  = round(F[0] / quant[0]),  a plain 10-bit signed value (this is v2)
 *     AC  = round(F[nat] * 8 / (qscale * quant[nat]))
 *     zigzag, run-length, MPEG-1 Table B.14, EOB `10`, escape `000001`
 *     16-bit little-endian words, most significant bit first
 *
 * CHOSEN, because they are the encoder's business and not the format's:
 *
 *     which qscale a frame gets — see the cadence below
 *     how the ADPCM shift and filter are picked for each block of audio
 *
 * ---------------------------------------------------------------------------
 * The cadence is a bit budget, not a decoration
 * ---------------------------------------------------------------------------
 * Every frame on the disc spans 6, 5, 5 or 5 video sectors keyed to
 * `(frame_number - 1) % 4`, with zero violations in 5,301 frames. That is not
 * a pattern the encoder happened to produce — it is the encoder's CONSTRAINT.
 * Video and audio share one 8-sector interleave, audio takes slot 7, and the
 * drive delivers 150 sectors a second; 6 sectors per frame is 25.000 fps and
 * 21 video sectors per 4 frames is what is left after the audio takes its
 * three. So a frame gets 6 or 5 sectors and must FIT, and the only free
 * variable is the quantiser.
 *
 * This picks the lowest qscale in 1..31 whose frame fits its budget, which is
 * the same shape of rate control the original used: the disc's own qscale runs
 * 1..20 and varies per frame.
 *
 * ---------------------------------------------------------------------------
 * Two output forms
 * ---------------------------------------------------------------------------
 * The sink is handed each sector with its FORM, because the two are not the
 * same size and flattening them is how audio gets lost. A video sector is 2048
 * bytes (Form 1) and an audio sector is 2324 (Form 2, of which 2304 are ADPCM).
 * An extraction that writes 2048 for both — which is what every `.STX` sitting
 * in a working tree is — has silently truncated every audio sector, and that is
 * exactly the warning FORMATS.md §6 opens with.
 */
#ifndef Q2PSX_FORMATS_STXENC_H
#define Q2PSX_FORMATS_STXENC_H

#include "q2psx.h"
#include "stx.h"
#include "xa.h"     /* the container's other half: one sector in eight */

#ifdef __cplusplus
extern "C" {
#endif

/* 320x192 is 20x12 macroblocks of six blocks each. */
#define Q2_STX_MAX_BLOCKS  (((Q2_STX_WIDTH + 15u) / 16u) * \
                            ((Q2_STX_HEIGHT + 15u) / 16u) * 6u)

/* One encoded frame: the BS v2 bitstream with its own 8-byte header. */
typedef struct q2_stx_encoded {
    u8  data[Q2_STX_MAX_CHUNKS * Q2_STX_VIDEO_PAYLOAD];
    u32 size;        /* valid bytes, a multiple of 4, header included   */
    u32 blocks;      /* 8x8 blocks written                              */
    u32 pairs;       /* AC (run, level) pairs, escapes counted          */
    u32 escapes;     /* ...of which took the escape                     */
    u32 num_codes;   /* the MDEC DMA length this frame will state       */
    u32 qscale;      /* what it was quantised with                      */
    u32 bits;        /* bits of bitstream, before the pad to 32         */
} q2_stx_encoded;

/*
 * The transform half, kept apart from the quantiser half on purpose.
 *
 * A frame is DCT'd ONCE and then quantised as many times as the rate control
 * needs, which is what makes trying eight qscales cost eight entropy-coding
 * passes rather than eight discrete cosine transforms. 184 KB of coefficients;
 * this is a host-side tool and the console never runs it.
 */
typedef struct q2_stx_transform {
    s16 coeff[Q2_STX_MAX_BLOCKS][64];   /* natural order, per block   */
    u32 blocks;
    u32 width, height;
} q2_stx_transform;

/*
 * RGB (top row first, `width * height * 3` bytes) to DCT coefficients.
 *
 * False if the dimensions are not something this format carries.
 */
bool q2_stx_transform_frame(q2_stx_transform *t, const u8 *rgb,
                            u32 width, u32 height);

/* Quantise and entropy-code an already-transformed frame at `qscale` (1..31). */
bool q2_stx_encode_at(const q2_stx_transform *t, u32 qscale,
                      q2_stx_encoded *out);

/*
 * ...and the rate control: the lowest qscale whose frame fits `budget` bytes.
 *
 * Returns false only if even the coarsest quantiser overflows, which for a
 * 320x192 frame in five sectors does not happen with real footage. `hint`, when
 * not zero, is where the search starts — pass the previous frame's qscale and
 * a film settles into one or two attempts per frame.
 */
bool q2_stx_encode_fit(const q2_stx_transform *t, u32 budget, u32 hint,
                       q2_stx_encoded *out);

/* ------------------------------------------------------------------------- */
/* The container                                                              */
/* ------------------------------------------------------------------------- */
typedef enum q2_stx_form {
    Q2_STX_FORM_VIDEO = 0,   /* Form 1, 2048 bytes: header + chunk       */
    Q2_STX_FORM_AUDIO = 1,   /* Form 2, 2324 bytes: 2304 ADPCM + 20 zero */
    Q2_STX_FORM_NULL  = 2    /* Form 1, 2048 zero bytes: the video tail  */
} q2_stx_form;

/*
 * Where a finished sector goes. `index` is its position in the file, which is
 * what decides whether it is an audio slot, so a sink that reorders sectors
 * breaks the film. Return false to stop the writer.
 */
typedef bool (*q2_stx_sink)(void *user, u32 index, q2_stx_form form,
                            const u8 *payload, u32 len);

typedef struct q2_stx_writer {
    q2_stx_sink sink;
    void       *user;
    u32         width, height;
    bool        audio;

    u32 sector;          /* next sector index, file-relative */
    u32 frame_number;    /* next frame's number, 1-based     */
    u32 qscale_hint;

    /* Audio waiting to be packed. One sector is 2016 stereo frames and a video
     * frame carries 1512 of them, so this never holds more than two sectors'
     * worth — four is slack for a caller that feeds unevenly. */
    s16 pcm[XA_FRAMES_PER_SECTOR * 2 * 4];
    u32 pcm_frames;

    /* Running ADPCM predictor state: XA carries history across sectors. */
    s32 adpcm_prev1[2], adpcm_prev2[2];

    /* What was written, for the caller to report. */
    u32 video_sectors, audio_sectors, null_sectors, frames;
    u32 qscale_min, qscale_max;
    u64 qscale_sum;
} q2_stx_writer;

void q2_stx_writer_init(q2_stx_writer *w, u32 width, u32 height, bool audio,
                        q2_stx_sink sink, void *user);

/*
 * Add one frame, and the audio that plays under it.
 *
 * `pcm` is interleaved stereo at 37800 Hz — 1512 frames per picture frame at
 * 25 fps, which is what makes three audio sectors cover four pictures exactly.
 * NULL (or `pcm_frames` 0) writes silence into the slots this frame reaches.
 */
bool q2_stx_writer_frame(q2_stx_writer *w, const u8 *rgb,
                         const s16 *pcm, u32 pcm_frames);

/*
 * Close the film: drain the audio that is still queued, nulling the video slots
 * it passes.
 *
 * That tail is the disc's own shape and not tidiness — the video region ends at
 * `frames * 6` sectors and the audio runs on for a few seconds behind it, with
 * the video slots zero-filled rather than the file simply stopping (0 of 556
 * null sectors on the disc contain a non-zero byte).
 */
bool q2_stx_writer_finish(q2_stx_writer *w);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_FORMATS_STXENC_H */
