/*
 * stx.h — `.STX`, the disc's three movies: Sony STR video with XA audio.
 *
 * The container is settled and verified across all 32,442 sectors of all three
 * files (FORMATS.md §6): an 8-sector interleave with slots 0-6 carrying MDEC
 * video and slot 7 carrying XA ADPCM, every video sector opening with the same
 * 32-byte frame header, 320x192 at 25 fps, BS version 2.
 *
 * What was missing was the decoder. `q2_stx_frame_decode` is it: the BS v2
 * bitstream to an 8-bit RGB image, which is the last thing standing between
 * this port and the movies the campaign ends on (#92).
 *
 * The player is deliberately NOT here. This decodes a frame; who schedules
 * frames, and against which clock, is the caller's business — the timing is
 * forced by the container (one XA sector is 2016/37800 s, eight of those per
 * six video sectors, so exactly 25.000 fps) and stated where it is used.
 */
#ifndef Q2PSX_FORMATS_STX_H
#define Q2PSX_FORMATS_STX_H

#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* The container                                                              */
/* ------------------------------------------------------------------------- */
#define Q2_STX_SECTOR_SIZE     2048u
#define Q2_STX_INTERLEAVE         8u
#define Q2_STX_AUDIO_SLOT         7u
#define Q2_STX_HEADER_SIZE       32u
#define Q2_STX_VIDEO_PAYLOAD   (Q2_STX_SECTOR_SIZE - Q2_STX_HEADER_SIZE)

#define Q2_STX_WIDTH            320u
#define Q2_STX_HEIGHT           192u
#define Q2_STX_FPS               25u

/* The most sectors one frame ever spans: the cadence is 6,5,5,5. */
#define Q2_STX_MAX_CHUNKS         6u

/* 0x0160 in 27831/27831 video sectors; 0x8001 is "MDEC video". */
#define Q2_STX_MAGIC         0x0160u
#define Q2_STX_SUBTYPE_MDEC  0x8001u
#define Q2_STX_BS_MAGIC      0x3800u
#define Q2_STX_BS_VERSION         2u

typedef struct q2_stx_header {
    u16 magic;
    u16 sub_type;
    u16 chunk_index;
    u16 chunk_count;
    u32 frame_number;
    u32 frame_size_bytes;
    u16 width;
    u16 height;
    /* The next four mirror the frame bitstream's own first eight bytes, so a
     * demuxed buffer ALREADY carries its header — 5301/5301 frames confirm the
     * mirror, and prepending one is the classic way to break this format. */
    u16 bs_num_codes;
    u16 bs_magic;
    u16 bs_qscale;
    u16 bs_version;
    u32 reserved;
} q2_stx_header;

/* Read a video sector's 32-byte header. False if it is not one. */
bool q2_stx_header_read(const u8 *sector, q2_stx_header *out);

/* Is sector `index` of a `.STX` an audio sector? `index % 8 == 7`, with zero
 * violations in 4055/4055 audio sectors. */
Q2PSX_INLINE bool q2_stx_sector_is_audio(u32 index)
{
    return (index % Q2_STX_INTERLEAVE) == Q2_STX_AUDIO_SLOT;
}

/* ------------------------------------------------------------------------- */
/* Frames                                                                     */
/* ------------------------------------------------------------------------- */
typedef struct q2_stx_frame {
    u32 number;                 /* 1-based, contiguous          */
    u32 size;                   /* valid bitstream bytes        */
    u16 qscale;                 /* per FRAME, 1..20 — not fixed */
    u16 width, height;
    u8  data[Q2_STX_MAX_CHUNKS * Q2_STX_VIDEO_PAYLOAD];
} q2_stx_frame;

/*
 * Assemble the frame beginning at video sector `*cursor` of `buf`, advancing
 * the cursor past it. Audio and null sectors are stepped over. Returns false at
 * the end of the video region — which is NOT the end of the file: audio keeps
 * streaming for a few seconds after the last frame, and the video slots are
 * nulled rather than the file ending.
 */
bool q2_stx_frame_next(const u8 *buf, size_t size, size_t *cursor,
                       q2_stx_frame *out);

/* ------------------------------------------------------------------------- */
/* The BS v2 bitstream                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Decode one frame to RGB, `width * height * 3` bytes, top row first.
 *
 * `out` must hold at least that. Returns false if the bitstream desynchronises,
 * which is the check that matters: a wrong Huffman table does not produce a
 * slightly wrong picture, it runs out of blocks or bits within the first frame.
 *
 * `out_blocks` and `out_bits`, when not NULL, receive how many 8x8 blocks were
 * decoded and how many bits were consumed — the two numbers that say a decode
 * was exact rather than merely non-crashing.
 */
bool q2_stx_frame_decode(const q2_stx_frame *f, u8 *out,
                         u32 *out_blocks, u32 *out_bits);

/*
 * How many lookaheads the AC table could not match, bucketed by LEADING ZEROS.
 *
 * Table B.14 is organised by that run, so the bucket names the missing group:
 * everything landing in one bucket is one group left to transcribe, and a
 * scatter across many would mean the fault is not in the table at all.
 */
u32 q2_stx_unmatched_report(u32 *by_leading_zeros, u32 max);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_FORMATS_STX_H */
