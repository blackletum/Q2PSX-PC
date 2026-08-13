#include "xa.h"

#include <stdio.h>
#include <string.h>

/* XA's predictor. Only four filters, unlike SPU-ADPCM's five. */
static const s32 XA_K0[4] = { 0, 60, 115,  98 };
static const s32 XA_K1[4] = { 0,  0, -52, -55 };

void q2_xa_decoder_reset(q2_xa_decoder *dec)
{
    if (dec)
        memset(dec, 0, sizeof(*dec));
}

u32 q2_xa_validate_sector(const u8 *adpcm)
{
    u32 bad = 0;
    int g, b;

    if (!adpcm)
        return 0;

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const u8 *group = adpcm + (size_t)g * XA_SOUND_GROUP_SIZE;

        for (b = 0; b < XA_BLOCKS_PER_GROUP; b++) {
            /* Blocks 0..3 take their parameter from bytes 4..7, blocks 4..7
             * from bytes 12..15. Those are the authoritative copies. */
            u8 param  = (b < 4) ? group[4 + b] : group[12 + (b - 4)];
            u8 shift  = param & 0x0F;
            u8 filter = param >> 4;

            if (shift > 12 || filter > 3)
                bad++;
        }
    }
    return bad;
}

u32 q2_xa_decode_sector(q2_xa_decoder *dec, const u8 *adpcm, s16 *out, u32 out_capacity)
{
    u32 written = 0;
    int g, b, n;

    if (!dec || !adpcm || !out)
        return 0;

    for (g = 0; g < XA_GROUPS_PER_SECTOR; g++) {
        const u8 *group = adpcm + (size_t)g * XA_SOUND_GROUP_SIZE;

        for (b = 0; b < XA_BLOCKS_PER_GROUP; b++) {
            u8  param  = (b < 4) ? group[4 + b] : group[12 + (b - 4)];
            u8  shift  = param & 0x0F;
            u8  filter = param >> 4;
            /* Even blocks feed one output channel, odd blocks the other. */
            int ch     = b & 1;

            if (filter > 3)
                filter = 0;
            if (shift > 12)
                shift = 12;

            for (n = 0; n < XA_SAMPLES_PER_BLOCK; n++) {
                u32 word = q2_rd_u32(group + 16 + (size_t)n * 4);
                s32 nibble = (s32)((word >> (4 * b)) & 0x0F);
                s32 sample;

                /* Sign-extend the 4-bit residual from bit 3, then scale up by
                 * (12 - shift) rather than down: XA stores the shift as an
                 * attenuation, the opposite sense to SPU-ADPCM. */
                if (nibble > 7)
                    nibble -= 16;

                sample = nibble << (12 - shift);
                sample += (XA_K0[filter] * dec->prev1[ch] +
                           XA_K1[filter] * dec->prev2[ch]) / 64;

                if (sample >  32767) sample =  32767;
                if (sample < -32768) sample = -32768;

                dec->prev2[ch] = dec->prev1[ch];
                dec->prev1[ch] = sample;

                /*
                 * Interleave to stereo. Within a group, blocks 0/1 carry the
                 * first 28 frames, 2/3 the next 28, and so on, so the output
                 * frame index is (b/2)*28 + n and the channel is b&1.
                 */
                {
                    u32 frame = (u32)((b / 2) * XA_SAMPLES_PER_BLOCK + n);
                    u32 slot  = (u32)(g * (XA_BLOCKS_PER_GROUP / 2) * XA_SAMPLES_PER_BLOCK
                                      + frame);
                    u32 index = slot * 2 + (u32)ch;

                    if (index >= out_capacity)
                        continue;

                    out[index] = (s16)sample;
                    if (index + 1 > written)
                        written = index + 1;
                }
            }
        }
    }

    return written;
}

/* ------------------------------------------------------------------------- */
/* Track access                                                               */
/* ------------------------------------------------------------------------- */
q2_result q2_xa_track_open(q2_xa_track *out, const disc *d, char letter, u8 channel)
{
    char path[64];
    const disc_file *f;

    if (!out || !d || channel >= XAI_CHANNEL_COUNT)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/AUD/QUAKE_%c.XAI", letter);

    f = disc_find(d, path);
    if (!f)
        return Q2_ERR_NOT_FOUND;

    out->disc    = d;
    out->channel = channel;
    out->first_lba = f->lba;

    /* The directory size is a Form 1 style byte count, so derive the sector
     * span from it rather than from the Form 2 payload size. */
    out->sector_count = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;

    return Q2_OK;
}

u32 q2_xa_track_read(q2_xa_track *track, q2_xa_decoder *dec,
                     u32 *cursor, s16 *out, u32 out_capacity)
{
    if (!track || !dec || !cursor || !out)
        return 0;

    while (*cursor < track->sector_count) {
        u8 raw[CD_SECTOR_RAW];
        u32 index = (*cursor)++;
        u8 channel, submode;

        if (disc_read_raw_sector(track->disc, track->first_lba + index, raw) != Q2_OK)
            return 0;

        channel = raw[17];
        submode = raw[18];

        /* Only real-time Form 2 audio for our own channel. Everything else is
         * either another track or post-EOF filler, and the filler is NOT zeroed
         * on this disc, so decoding it would emit noise. */
        if (channel != track->channel)
            continue;
        if (!(submode & CD_SUBMODE_AUDIO) || !(submode & CD_SUBMODE_FORM2))
            continue;

        {
            u32 n = q2_xa_decode_sector(dec, raw + 24, out, out_capacity);

            /* An EOF sector is the last real audio in this channel. Decode it,
             * then stop the caller advancing into the filler behind it. */
            if (submode & CD_SUBMODE_EOF)
                *cursor = track->sector_count;

            return n;
        }
    }

    return 0;
}
