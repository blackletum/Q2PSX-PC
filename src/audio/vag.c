#include "vag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The SPU's four-tap predictor. Coefficients are divided by 64. Index 0 is
 * "no prediction", which is why a silent or freshly-started sample decodes
 * cleanly without special-casing.
 */
static const s32 SPU_F0[5] = { 0, 60, 115,  98, 122 };
static const s32 SPU_F1[5] = { 0,  0, -52, -55, -60 };

/* ------------------------------------------------------------------------- */
/* Bank loading                                                               */
/* ------------------------------------------------------------------------- */
q2_result q2_sound_bank_load(q2_sound_bank *out, const disc *d, const char *map)
{
    char path[256];
    q2_result r;
    u32 ofs_sound_bank, total_size;
    u32 table0, entries, i;
    const u8 *section;

    if (!out || !d || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/SNDVRAM.DAT", map);

    r = disc_read_file(d, path, &out->buf);
    if (r != Q2_OK)
        return r;

    if (out->buf.size < 12) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    /* Three-word header: the VRAM section offset (always 12), the sound bank
     * offset, and the file size as an end sentinel. */
    ofs_sound_bank = q2_rd_u32(out->buf.data + 4);
    total_size     = q2_rd_u32(out->buf.data + 8);

    if (q2_rd_u32(out->buf.data) != 12 ||
        total_size != (u32)out->buf.size ||
        ofs_sound_bank >= total_size) {
        Q2_ERROR("%s: sound bank header failed validation", path);
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    section = out->buf.data + ofs_sound_bank;
    out->section      = section;
    out->section_size = total_size - ofs_sound_bank;

    if (out->section_size < 8) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    /* The offset table describes its own length. */
    table0 = q2_rd_u32(section);
    if (table0 < 8 || table0 % 4 != 0 || table0 > out->section_size) {
        Q2_ERROR("%s: sound bank offset table is malformed", path);
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    entries = table0 / 4;
    out->offsets = (u32 *)calloc(entries, sizeof(u32));
    if (!out->offsets) {
        q2_buf_free(&out->buf);
        return Q2_ERR_NO_MEMORY;
    }

    for (i = 0; i < entries; i++)
        out->offsets[i] = q2_rd_u32(section + i * 4);

    /* The last entry is a sentinel equal to the section size. Checking it turns
     * the whole table from plausible into verified for this file. */
    if (out->offsets[entries - 1] != out->section_size) {
        Q2_ERROR("%s: bank sentinel is %u but the section is %u bytes",
                 path, out->offsets[entries - 1], out->section_size);
        free(out->offsets);
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    out->count = entries - 1;
    return Q2_OK;
}

void q2_sound_bank_free(q2_sound_bank *bank)
{
    if (!bank)
        return;
    free(bank->offsets);
    q2_buf_free(&bank->buf);
    memset(bank, 0, sizeof(*bank));
}

bool q2_sound_bank_get(const q2_sound_bank *bank, u32 index, q2_vag *out)
{
    const u8 *hdr;
    u32 offset;

    if (!bank || !out || index >= bank->count)
        return false;

    memset(out, 0, sizeof(*out));

    offset = bank->offsets[index];
    if (offset + VAG_HEADER_SIZE > bank->section_size)
        return false;

    hdr = bank->section + offset;

    if (memcmp(hdr, "VAGp", 4) != 0)
        return false;

    /* Big endian, uniquely on the disc. */
    out->data_size   = q2_rd_u32be(hdr + 0x0C);
    out->sample_rate = q2_rd_u32be(hdr + 0x10);

    memcpy(out->name, hdr + 0x20, 16);
    out->name[16] = '\0';

    if (offset + VAG_HEADER_SIZE + out->data_size > bank->section_size)
        return false;

    out->body = hdr + VAG_HEADER_SIZE;

    /*
     * Loop detection, done exactly as the engine did it: inspect the flag bytes
     * of body blocks 1 and 2. Deriving it instead from "contains a
     * LoopStart|Repeat block" agrees on all 2,475 samples, but this is the
     * original's own test and costs two byte reads.
     */
    if (out->data_size >= 3 * SPU_BLOCK_SIZE) {
        out->looping = (out->body[17] & SPU_FLAG_REPEAT) &&
                       (out->body[33] & SPU_FLAG_REPEAT);
    }

    return true;
}

u32 q2_sound_bank_total_body(const q2_sound_bank *bank)
{
    u32 total = 0, i;

    if (!bank)
        return 0;

    for (i = 0; i < bank->count; i++) {
        q2_vag vag;
        if (q2_sound_bank_get(bank, i, &vag))
            total += vag.data_size;
    }
    return total;
}

/* ------------------------------------------------------------------------- */
/* SPU-ADPCM                                                                  */
/* ------------------------------------------------------------------------- */
u32 q2_spu_adpcm_validate(const u8 *adpcm, u32 size)
{
    u32 bad = 0, off;

    if (!adpcm)
        return 0;

    for (off = 0; off + SPU_BLOCK_SIZE <= size; off += SPU_BLOCK_SIZE) {
        u8 shift  = adpcm[off] & 0x0F;
        u8 filter = adpcm[off] >> 4;
        u8 flags  = adpcm[off + 1];

        /* Shift 0 and 1 both occur in real data, so only >12 is invalid. */
        if (shift > 12 || filter > 4 || flags > 7)
            bad++;
    }
    return bad;
}

void q2_spu_voice_start(q2_spu_voice *v, const u8 *adpcm, u32 size)
{
    if (!v)
        return;

    memset(v, 0, sizeof(*v));
    v->adpcm = adpcm;
    v->size  = size;
    v->done  = (adpcm == NULL || size < SPU_BLOCK_SIZE);
}

u32 q2_spu_voice_block(q2_spu_voice *v, s16 *out)
{
    const u8 *adpcm;
    u32 off;
    u8  shift, filter, flags;
    int i;

    if (!v || !out || v->done || !v->adpcm)
        return 0;

    off = v->off;
    if (off + SPU_BLOCK_SIZE > v->size) {
        v->done = true;
        return 0;
    }

    adpcm  = v->adpcm;
    shift  = adpcm[off] & 0x0F;
    filter = adpcm[off] >> 4;
    flags  = adpcm[off + 1];

    if (filter > 4)
        filter = 0;
    /* A shift above 12 is undefined on hardware; treat it as maximum
     * attenuation rather than shifting by a wild amount. */
    if (shift > 12)
        shift = 12;

    for (i = 0; i < SPU_SAMPLES_PER_BLOCK; i++) {
        u8  byte   = adpcm[off + 2 + i / 2];
        s32 nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        s32 sample;

        /* Sign-extend the 4-bit residual into the high bits, then scale. */
        sample = (s32)((u32)nibble << 12);
        sample = (s32)((s16)sample) >> shift;

        sample += (SPU_F0[filter] * v->prev1 + SPU_F1[filter] * v->prev2) / 64;

        if (sample >  32767) sample =  32767;
        if (sample < -32768) sample = -32768;

        out[i] = (s16)sample;

        v->prev2 = v->prev1;
        v->prev1 = sample;
    }

    v->off = off + SPU_BLOCK_SIZE;

    /* Stop on the end marker, AFTER emitting its block. Samples that simply run
     * out without one are handled by the bound above, which is why the 176
     * terminator-less one-shots do not overrun here. */
    if (flags & SPU_FLAG_END)
        v->done = true;

    return SPU_SAMPLES_PER_BLOCK;
}

u32 q2_spu_adpcm_decode(const u8 *adpcm, u32 size, s16 *out, u32 out_capacity)
{
    q2_spu_voice v;
    s16 block[SPU_SAMPLES_PER_BLOCK];
    u32 written = 0;

    if (!adpcm || !out)
        return 0;

    q2_spu_voice_start(&v, adpcm, size);

    for (;;) {
        u32 n = q2_spu_voice_block(&v, block);
        u32 take;

        if (n == 0)
            break;

        /* A capacity that ends mid-block keeps the samples that fit, which is
         * what the flat decoder has always done. */
        take = n;
        if (written + take > out_capacity)
            take = out_capacity - written;

        memcpy(out + written, block, take * sizeof(s16));
        written += take;

        if (written >= out_capacity)
            break;
    }

    return written;
}
