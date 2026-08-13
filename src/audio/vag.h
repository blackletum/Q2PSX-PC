/*
 * vag.h — SNDVRAM.DAT's sound bank: VAG headers and SPU-ADPCM decoding.
 *
 * Despite its name, SNDVRAM.DAT holds two unrelated sections. The first is
 * texture/VRAM images; the second, starting at the header's `ofs_sound_bank`,
 * is the block of sample data the console uploaded verbatim into its 512 KB of
 * sound RAM. This file handles the second.
 *
 * ---------------------------------------------------------------------------
 * Bank layout (offsets relative to ofs_sound_bank)
 * ---------------------------------------------------------------------------
 *     u32 offsets[num_entries];
 *
 * The table is self-describing in the same idiom the rest of the disc uses:
 * offsets[0] is the table's own byte size, so num_entries == offsets[0]/4, and
 * the final entry is a sentinel equal to the section size. Entries 0..n-2 point
 * at VAG headers. Confirmed across 49 files and 2,524 table entries.
 *
 * num_sounds == num_entries - 1, observed 2..87, never above the runtime clamp
 * of 96. Note that 54 entries disc-wide are aliases: two table slots pointing at
 * the same sample.
 *
 * Runtime sound IDs are 1-based — slot 0 is a null-sound placeholder — so sound
 * id N is bank index N-1.
 *
 * ---------------------------------------------------------------------------
 * VAG header — 48 bytes, and its integers are BIG endian
 * ---------------------------------------------------------------------------
 * This is the one place on the disc where byte order flips. Sony's VAG format
 * was defined big-endian and the tooling emitted it that way even for a
 * little-endian console. The reading is proven by construction: big-endian
 * yields exactly two sample rates (11025 and 22050) and 16-aligned body sizes,
 * while little-endian yields values in the hundreds of millions.
 *
 *     0x00  char[4]  'VAGp'
 *     0x04  u32 be   version, always 0x20
 *     0x08  u32 be   reserved, always 0
 *     0x0C  u32 be   data_size, always a multiple of 16
 *     0x10  u32 be   sample_rate, only 11025 or 22050
 *     0x14  u8[12]   reserved, all zero
 *     0x20  char[16] name, NUL-padded
 *
 * The engine truncated names to 11 characters plus a terminator at load, so the
 * runtime name is not always the stored one.
 *
 * ---------------------------------------------------------------------------
 * SPU-ADPCM — 16-byte blocks, 28 samples each
 * ---------------------------------------------------------------------------
 *     0x00  u8      bits 0-3 right-shift, bits 4-7 predictor filter index
 *     0x01  u8      bit0 End, bit1 Repeat, bit2 LoopStart
 *     0x02  u8[14]  28 signed 4-bit residuals, low nibble first
 *
 * Verified over 1,167,540 blocks: zero invalid flag bytes, zero invalid shifts,
 * zero invalid filter indices.
 *
 * Two decoder traps, both of which produce plausible-sounding but wrong output:
 *
 *   - Shift can be 0 or 1. A decoder that assumes the usual 2..12 range will
 *     mis-scale those blocks rather than fail loudly.
 *   - One-shot samples end two ways. 2,181 have flags==1 at block nb-2 followed
 *     by a 0x07 terminator; 176 have flags==1 followed by a plain flags==0 block
 *     with no terminator at all. A decoder that stops only on the terminator
 *     overruns by one block on those 176.
 */
#ifndef Q2PSX_VAG_H
#define Q2PSX_VAG_H

#include "disc.h"
#include "q2psx.h"

#define VAG_HEADER_SIZE   48
#define SPU_BLOCK_SIZE    16
#define SPU_SAMPLES_PER_BLOCK 28

/* Total sound RAM, and the low region reserved for the capture buffers. */
#define SPU_RAM_SIZE      0x80000
#define SPU_RAM_RESERVED  0x800

enum {
    SPU_FLAG_END        = 1 << 0,
    SPU_FLAG_REPEAT     = 1 << 1,
    SPU_FLAG_LOOP_START = 1 << 2
};

typedef struct q2_vag {
    char      name[17];
    u32       sample_rate;
    u32       data_size;      /* ADPCM body bytes */
    const u8 *body;           /* borrowed         */
    bool      looping;
} q2_vag;

typedef struct q2_sound_bank {
    q2_buf  buf;              /* owns the whole SNDVRAM.DAT file */
    const u8 *section;        /* start of the sound bank section */
    u32     section_size;
    u32     count;            /* number of sounds (entries - 1)  */
    u32    *offsets;          /* owned; relative to `section`    */
} q2_sound_bank;

/* Load "<map>/SNDVRAM.DAT" and locate its sound bank. */
q2_result q2_sound_bank_load(q2_sound_bank *out, const disc *d, const char *map);
void      q2_sound_bank_free(q2_sound_bank *bank);

/* Decode the VAG header at bank index `index` (0-based; runtime id is index+1). */
bool q2_sound_bank_get(const q2_sound_bank *bank, u32 index, q2_vag *out);

/* Total ADPCM body bytes, for checking the bank fits in sound RAM. */
u32 q2_sound_bank_total_body(const q2_sound_bank *bank);

/*
 * Decode SPU-ADPCM to signed 16-bit PCM.
 *
 * `out` must hold at least (data_size / 16) * 28 samples. Returns the number of
 * samples written, stopping at the sample's true end so the trailing block of a
 * terminator-less one-shot is not emitted.
 */
u32 q2_spu_adpcm_decode(const u8 *adpcm, u32 size, s16 *out, u32 out_capacity);

/* Validate an ADPCM stream's block headers without decoding it. Returns the
 * number of structurally invalid blocks — zero is expected for real data. */
u32 q2_spu_adpcm_validate(const u8 *adpcm, u32 size);

#endif /* Q2PSX_VAG_H */
