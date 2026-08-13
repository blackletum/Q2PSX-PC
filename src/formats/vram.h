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
 * What is NOT yet established
 * ---------------------------------------------------------------------------
 * Where each decoded image belongs in VRAM. The output is width * height bytes
 * of 8-bit data, but a 128x256 record decoding to 32768 bytes does not fill a
 * 128x256 rectangle of 16-bit VRAM words (that would need 65536). So `width`
 * and `height` are not simply the VRAM rectangle, and the mapping from image to
 * texture page — which the MapMod polygons' tpage and clut ids index — still
 * needs the executable's uploader.
 *
 * Until that is resolved this module decodes images but cannot place them, so
 * the renderer still draws untextured. The codec is the hard part and it is
 * done; the placement is bookkeeping that one function in the EXE will settle.
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
