/*
 * vram.h — SNDVRAM.DAT's first section: the compressed VRAM images.
 *
 * This was the last unsolved format on the disc and the one blocking all
 * texturing. The codec is PackBits run-length encoding, the same scheme
 * TIFF and early Mac tooling used, producing 8-bit data.
 *
 * ---------------------------------------------------------------------------
 * Section layout (all offsets relative to file offset 0x0C)
 * ---------------------------------------------------------------------------
 *     0x0C  u8   texpage_count      1..12
 *     0x0D  u8   image_count        0..12
 *     0x0E  u8   unknown
 *     0x0F  u8   unknown
 *     0x10       record[texpage_count + image_count]   8 bytes each
 *     ...        512 bytes of u16 0x8000, an unused palette slot
 *     ...        unidentified section data
 *     ...        packed NUL-terminated image names
 *     ...        the compressed payloads, up to the sound bank offset
 *
 * A record is:
 *     0x00  u32  payload offset, RELATIVE TO 0x0C -- add 0x0C for a file offset
 *     0x04  u16  width
 *     0x06  u16  height
 *
 * Payload i spans [record[i].offset, record[i+1].offset), and the last runs to
 * the sound bank. The offsets being section-relative rather than absolute is
 * easy to get wrong: the raw value lands twelve bytes inside the image-name
 * strings, so a decoder using it feeds ASCII into the first image.
 *
 * ---------------------------------------------------------------------------
 * The codec
 * ---------------------------------------------------------------------------
 * PackBits, read one control byte at a time:
 *
 *     c == 0x80          no-op, skip it
 *     c <  0x80          copy the next (c + 1) bytes literally
 *     c >  0x80          repeat the next single byte (257 - c) times
 *
 * Decoding is bounded by the expected output size of width * height bytes, and
 * stops there rather than consuming all input. That bound matters: on 35 of the
 * 553 payloads a final run would otherwise emit one byte too many or read past
 * the end. With the bound, all 553 payloads on the disc decode to exactly
 * width * height bytes.
 *
 * After an exact decode, 0 to 3 input bytes remain unconsumed — payloads are
 * padded to a 4-byte boundary. The distribution (518 with none, then 15, 11 and
 * 9 with one, two and three) is exactly what alignment padding looks like, and
 * is a useful confirmation that the stop condition is right rather than merely
 * convenient.
 *
 * ---------------------------------------------------------------------------
 * What width and height mean
 * ---------------------------------------------------------------------------
 * They are the image's dimensions in 8-BIT TEXELS, so the decoded buffer is
 * `width` bytes per row for `height` rows.
 *
 * That is established, not assumed. Decoding an image and measuring the mean
 * absolute difference between vertically adjacent bytes at several candidate
 * row strides picks out the true stride sharply — real texture data correlates
 * down columns, misaligned data does not. Across 42 sampled images, 41 scored
 * best at exactly the declared width, and the margin is not subtle: a typical
 * page scores ~28 at the correct stride against ~40 at double and ~88 at half.
 *
 * Note this means the record dimensions are NOT a rectangle of 16-bit VRAM
 * words. A 128x256 image is 32768 texels, which at 8bpp occupies 64 words by
 * 256 rows in VRAM, not 128 by 256.
 *
 * ---------------------------------------------------------------------------
 * What is NOT yet established
 * ---------------------------------------------------------------------------
 * Where each decoded image is uploaded, and which palette goes with it. The
 * MapMod polygons carry tpage and clut ids that index whatever the executable's
 * uploader arranges, and that mapping is still unread.
 *
 * So this module decodes images but cannot yet place them, and the renderer
 * still draws untextured. The codec was the hard part; the placement is
 * bookkeeping that one function in the EXE will settle.
 */
#ifndef Q2PSX_VRAM_H
#define Q2PSX_VRAM_H

#include "disc.h"
#include "q2psx.h"

#define Q2_VRAM_RECORD_SIZE 8
#define Q2_VRAM_SECTION_BASE 0x0C

typedef struct q2_vram_image {
    u32 offset;        /* file offset of the packed payload */
    u32 packed_size;
    u16 width;
    u16 height;
} q2_vram_image;

typedef struct q2_vram_section {
    q2_buf         buf;          /* owns the whole SNDVRAM.DAT       */
    q2_vram_image *images;       /* owned                            */
    u32            image_count;  /* texpages + images                */
    u32            texpage_count;
} q2_vram_section;

/* Load "<map>/SNDVRAM.DAT" and index its image records. */
q2_result q2_vram_load(q2_vram_section *out, const disc *d, const char *map);
void      q2_vram_free(q2_vram_section *section);

/*
 * Decode image `index` into `out`, which must hold at least
 * width * height bytes. Writes the decoded size to `out_size`.
 *
 * Returns Q2_ERR_BAD_FORMAT if the payload does not decode to exactly
 * width * height bytes — that is a hard error rather than a warning, because
 * every payload on a good disc does.
 */
q2_result q2_vram_decode(const q2_vram_section *section, u32 index,
                         u8 *out, size_t out_capacity, size_t *out_size);

/*
 * The PackBits decoder on its own, for testing and for any other packed data
 * that turns up. Stops once `target` bytes have been produced. Returns the
 * number of bytes written and reports how much input was consumed.
 */
size_t q2_packbits_decode(const u8 *src, size_t src_size,
                          u8 *dst, size_t target, size_t *consumed);

#endif /* Q2PSX_VRAM_H */
