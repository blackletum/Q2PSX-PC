#include "vram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t q2_packbits_decode(const u8 *src, size_t src_size,
                          u8 *dst, size_t target, size_t *consumed)
{
    size_t in = 0, out = 0;

    if (!src || !dst) {
        if (consumed) *consumed = 0;
        return 0;
    }

    /* Bounded by the expected output, not by the input. Running to the end of
     * the input instead overshoots on 35 of the disc's 553 payloads. */
    while (out < target) {
        u8 control;

        if (in >= src_size)
            break;

        control = src[in++];

        if (control == 0x80) {
            continue;                       /* no-op */
        } else if (control < 0x80) {
            size_t count = (size_t)control + 1;

            if (count > target - out)
                count = target - out;
            if (in + count > src_size)
                count = src_size - in;

            memcpy(dst + out, src + in, count);
            out += count;
            in  += count;
        } else {
            size_t count = (size_t)(257 - control);
            u8 value;

            if (in >= src_size)
                break;
            value = src[in++];

            if (count > target - out)
                count = target - out;

            memset(dst + out, value, count);
            out += count;
        }
    }

    if (consumed)
        *consumed = in;

    return out;
}

/* ------------------------------------------------------------------------- */
q2_result q2_vram_load(q2_vram_section *out, const disc *d, const char *map)
{
    char path[256];
    q2_result r;
    u32 ofs_sound_bank, total_size;
    u32 count, i;
    const u8 *p;

    if (!out || !d || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/SNDVRAM.DAT", map);

    r = disc_read_file(d, path, &out->buf);
    if (r != Q2_OK)
        return r;

    if (out->buf.size < 16) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    p = out->buf.data;

    if (q2_rd_u32(p) != Q2_VRAM_SECTION_BASE) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    ofs_sound_bank = q2_rd_u32(p + 4);
    total_size     = q2_rd_u32(p + 8);

    if (total_size != (u32)out->buf.size || ofs_sound_bank > total_size) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    out->texpage_count = p[0x0C];
    count              = (u32)p[0x0C] + (u32)p[0x0D];

    if (count == 0 || 0x10 + count * Q2_VRAM_RECORD_SIZE > out->buf.size) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    out->images = (q2_vram_image *)calloc(count, sizeof(q2_vram_image));
    if (!out->images) {
        q2_buf_free(&out->buf);
        return Q2_ERR_NO_MEMORY;
    }
    out->image_count = count;

    for (i = 0; i < count; i++) {
        const u8 *rec = p + 0x10 + (size_t)i * Q2_VRAM_RECORD_SIZE;

        /* Offsets are relative to the section base, not the file. */
        out->images[i].offset = Q2_VRAM_SECTION_BASE + q2_rd_u32(rec);
        out->images[i].width  = q2_rd_u16(rec + 4);
        out->images[i].height = q2_rd_u16(rec + 6);
    }

    /* A payload runs until the next one starts; the last ends at the sound
     * bank. */
    for (i = 0; i < count; i++) {
        u32 end = (i + 1 < count) ? out->images[i + 1].offset : ofs_sound_bank;

        if (end < out->images[i].offset || end > total_size) {
            Q2_ERROR("%s: image %u spans 0x%X..0x%X, which is out of bounds",
                     path, i, out->images[i].offset, end);
            free(out->images);
            q2_buf_free(&out->buf);
            return Q2_ERR_BAD_FORMAT;
        }
        out->images[i].packed_size = end - out->images[i].offset;
    }

    return Q2_OK;
}

void q2_vram_free(q2_vram_section *section)
{
    if (!section)
        return;
    free(section->images);
    q2_buf_free(&section->buf);
    memset(section, 0, sizeof(*section));
}

q2_result q2_vram_decode(const q2_vram_section *section, u32 index,
                         u8 *out, size_t out_capacity, size_t *out_size)
{
    const q2_vram_image *img;
    size_t target, produced, consumed = 0;

    if (!section || !out || index >= section->image_count)
        return Q2_ERR_INVALID_ARG;

    img    = &section->images[index];
    target = (size_t)img->width * (size_t)img->height;

    if (out_capacity < target)
        return Q2_ERR_RANGE;

    produced = q2_packbits_decode(section->buf.data + img->offset,
                                  img->packed_size,
                                  out, target, &consumed);

    if (produced != target) {
        Q2_ERROR("vram: image %u decoded to %zu bytes, expected %zu",
                 index, produced, target);
        return Q2_ERR_BAD_FORMAT;
    }

    /* Payloads are padded to four bytes, so 0..3 unconsumed bytes are normal
     * and anything more means the stream is not what we think it is. */
    if (img->packed_size - consumed > 3) {
        Q2_WARN("vram: image %u left %zu bytes unconsumed",
                index, (size_t)img->packed_size - consumed);
    }

    if (out_size)
        *out_size = produced;

    return Q2_OK;
}
