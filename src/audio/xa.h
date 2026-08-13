/*
 * xa.h — CD-XA ADPCM: the streamed music in Q2DATA/AUD/QUAKE_[A-E].XAI.
 *
 * This is a completely different codec from the SPU-ADPCM in vag.h. Both are
 * 4-bit ADPCM with a four-tap predictor, and there the similarity ends: XA uses
 * 128-byte "sound groups" carrying eight interleaved blocks, decodes stereo, and
 * runs at 37800 Hz off the CD rather than out of sound RAM.
 *
 * ---------------------------------------------------------------------------
 * How the five files are laid out
 * ---------------------------------------------------------------------------
 * Each .XAI holds FOUR independent stereo streams multiplexed by CD-XA channel
 * number, in strict one-sector round-robin:
 *
 *     sector_index % 4 == channel_num
 *
 * with zero violations across 66,608 audio sectors. So a track is every fourth
 * sector, and playing one means demultiplexing rather than reading a range.
 *
 * That gives 20 (file, channel) slots but only 19 distinct tracks: two channels
 * of one file are byte-identical.
 *
 * One audio sector holds 2016 stereo frames = 53.333 ms, and a channel receives
 * one sector in four at 75 sectors/s, i.e. 18.75/s — exactly real time at 1x
 * drive speed. That is not a coincidence; it is why the interleave is 4.
 *
 * ---------------------------------------------------------------------------
 * Two traps that produce plausible garbage
 * ---------------------------------------------------------------------------
 * 1. A Form 2 payload is 2324 bytes but only 2304 of them are ADPCM (18 groups
 *    x 128). The trailing 20 bytes are unused and are zero in all 70,663 audio
 *    sectors. A decoder that consumes 2324 will drift.
 *
 * 2. Null sectors are NOT zero-filled. After a channel emits its EOF sector its
 *    remaining slots carry stale mastering filler — on one file all 1,154 have
 *    non-zero user data and 479 duplicate the payload four sectors earlier.
 *    Decoding them produces noise. A player MUST gate on the submode and channel
 *    and never decode a sector it did not ask for.
 *
 * ---------------------------------------------------------------------------
 * Sound group — 128 bytes, 18 per sector
 * ---------------------------------------------------------------------------
 *     0x00  u8[4]   parameter copy (low)
 *     0x04  u8[4]   parameters for blocks 0..3
 *     0x08  u8[4]   parameter copy (high)
 *     0x0C  u8[4]   parameters for blocks 4..7
 *     0x10  u32[28] sample nibbles; block b sample n is (word[n] >> (4*b)) & 0xF
 *
 * A parameter byte is a shift in its low nibble (0..12) and a filter index in
 * its high nibble (0..3). Bytes 4..7 and 12..15 are the authoritative copies —
 * that is the CD-XA standard's rule, not a finding from this disc, though the
 * copies do agree in 98.2% of sampled groups and the disagreements fail every
 * other sanity check.
 *
 * Even-numbered blocks form one output channel and odd-numbered the other. The
 * separation is confirmed by signal analysis; which one is physically *left* is
 * a standard fact rather than something these bytes reveal.
 */
#ifndef Q2PSX_XA_H
#define Q2PSX_XA_H

#include "disc.h"
#include "q2psx.h"

#define XA_SECTOR_ADPCM_BYTES 2304   /* 18 * 128; NOT the full 2324 payload */
#define XA_SOUND_GROUP_SIZE   128
#define XA_GROUPS_PER_SECTOR  18
#define XA_BLOCKS_PER_GROUP   8
#define XA_SAMPLES_PER_BLOCK  28

/* Every audio sector on this disc reports stereo / 37800 Hz / 4-bit. */
#define XA_SAMPLE_RATE        37800
#define XA_CHANNELS           2

/* Frames (stereo sample pairs) produced by one sector. */
#define XA_FRAMES_PER_SECTOR  (XA_GROUPS_PER_SECTOR * XA_BLOCKS_PER_GROUP * XA_SAMPLES_PER_BLOCK / 2)

#define XAI_CHANNEL_COUNT     4

/* Decoder state. XA is stride-dependent: each output channel carries its own
 * two-sample history across sectors, so this must persist for a whole track. */
typedef struct q2_xa_decoder {
    s32 prev1[XA_CHANNELS];
    s32 prev2[XA_CHANNELS];
} q2_xa_decoder;

void q2_xa_decoder_reset(q2_xa_decoder *dec);

/*
 * Decode one sector's 2304 ADPCM bytes into interleaved stereo PCM16.
 *
 * `out` must hold at least XA_FRAMES_PER_SECTOR * 2 samples. Returns the number
 * of samples written (frames * 2).
 */
u32 q2_xa_decode_sector(q2_xa_decoder *dec, const u8 *adpcm, s16 *out, u32 out_capacity);

/* Validate a sector's sound-group parameters without decoding. Returns the
 * count of structurally invalid blocks; zero is expected for real audio. */
u32 q2_xa_validate_sector(const u8 *adpcm);

/* ------------------------------------------------------------------------- */
/* Track access                                                               */
/* ------------------------------------------------------------------------- */

/* One demultiplexed music track: a (file, channel) pair. */
typedef struct q2_xa_track {
    const disc *disc;
    u32   first_lba;
    u32   sector_count;   /* total sectors in the file, all channels */
    u8    channel;
} q2_xa_track;

/* Open QUAKE_<letter>.XAI and select one of its four channels.
 * `letter` is 'A'..'E'; `channel` is 0..3. */
q2_result q2_xa_track_open(q2_xa_track *out, const disc *d, char letter, u8 channel);

/*
 * Read and decode the next sector belonging to this track's channel, starting
 * the search at `*cursor` (a sector index within the file) and advancing it.
 *
 * Returns the number of samples written, or 0 at end of stream. Sectors that
 * are not audio, not this channel, or are post-EOF filler are skipped rather
 * than decoded — see the null-sector trap above.
 */
u32 q2_xa_track_read(q2_xa_track *track, q2_xa_decoder *dec,
                     u32 *cursor, s16 *out, u32 out_capacity);

#endif /* Q2PSX_XA_H */
