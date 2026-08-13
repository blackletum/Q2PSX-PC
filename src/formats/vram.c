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

    /* Bounded by the expected output, not by the input — that is what the
     * engine's own loop does (bgtz on the remaining output count at
     * 0x80068C7C), and running to the end of the input instead overshoots on
     * 35 of the disc's 553 payloads, which all carry alignment padding. */
    while (out < target) {
        u8 control;

        if (in >= src_size)
            break;

        control = src[in++];

        if (control < 0x80) {
            size_t count = (size_t)control + 1;

            if (count > target - out)
                count = target - out;
            if (count > src_size - in)
                count = src_size - in;

            memcpy(dst + out, src + in, count);
            out += count;
            in  += count;
        } else {
            /* 257 - control, so 0x80 is 129 repeats and 0xFF is 2. This is NOT
             * the Apple PackBits no-op; the engine has no special case for
             * 0x80, and no stream on the disc uses it either way. */
            size_t count = (size_t)(257 - (int)control);
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
/* Case-insensitive compare, so callers can pass a name in any case without
 * dragging in a platform-specific stricmp. */
static bool name_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (int)(u8)*a++;
        int cb = (int)(u8)*b++;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return *a == '\0' && *b == '\0';
}

q2_result q2_vram_load(q2_vram_section *out, const disc *d, const char *map)
{
    char path[256];
    q2_result r;
    u32 ofs_sound_bank, total_size;
    u32 count, i;
    u32 section_size, cursor, names_offset;
    const u8 *p;        /* the whole file            */
    const u8 *a;        /* section A base, == p + 12 */
    const char *name;

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

    if (total_size != (u32)out->buf.size ||
        ofs_sound_bank < Q2_VRAM_SECTION_BASE ||
        ofs_sound_bank > total_size) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    a            = p + Q2_VRAM_SECTION_BASE;
    section_size = ofs_sound_bank - Q2_VRAM_SECTION_BASE;

    if (section_size < 4) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    out->texpage_count = q2_rd_u8(a + 0);
    out->clut4_count_a = q2_rd_u8(a + 2);
    out->clut4_count_b = q2_rd_u8(a + 3);
    out->clut4_count   = (u32)out->clut4_count_a + out->clut4_count_b;
    out->clut8_count   = q2_rd_u8(a + 1);

    count = out->texpage_count + out->clut8_count;

    if (count == 0) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }

    /* Walk the section's fixed-size arrays in order, checking each against the
     * section length before advancing. Getting this wrong is how the earlier
     * reading mis-sized the CLUT region: the arrays are contiguous and the name
     * list's position is derived from all three counts, so one bad count moves
     * the name list into the middle of a payload. */
    cursor = 4;
    if (count > (section_size - cursor) / Q2_VRAM_RECORD_SIZE) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += count * Q2_VRAM_RECORD_SIZE;

    out->clut4 = a + cursor;
    if (out->clut4_count > (section_size - cursor) / (Q2_VRAM_CLUT4_ENTRIES * 2)) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += out->clut4_count * (Q2_VRAM_CLUT4_ENTRIES * 2);

    out->clut8 = a + cursor;
    if (out->clut8_count > (section_size - cursor) / (Q2_VRAM_CLUT8_ENTRIES * 2)) {
        q2_buf_free(&out->buf);
        return Q2_ERR_BAD_FORMAT;
    }
    cursor += out->clut8_count * (Q2_VRAM_CLUT8_ENTRIES * 2);

    names_offset = cursor;

    out->images = (q2_vram_image *)calloc(count, sizeof(q2_vram_image));
    if (!out->images) {
        q2_buf_free(&out->buf);
        return Q2_ERR_NO_MEMORY;
    }
    out->image_count = count;

    for (i = 0; i < count; i++) {
        const u8 *rec = a + 4 + (size_t)i * Q2_VRAM_RECORD_SIZE;

        /* Payload offsets are section-relative, not file-absolute. The raw
         * value lands twelve bytes inside the name list. */
        out->images[i].offset = Q2_VRAM_SECTION_BASE + q2_rd_u32(rec);
        out->images[i].width  = q2_rd_u16(rec + 4);
        out->images[i].height = q2_rd_u16(rec + 6);

        /* Texture pages ignore their stored dimensions (0x80068B74). */
        if (i < out->texpage_count)
            out->images[i].decoded_size =
                (u32)Q2_VRAM_TEXPAGE_W * Q2_VRAM_TEXPAGE_H;
        else
            out->images[i].decoded_size =
                (u32)out->images[i].width * out->images[i].height;
    }

    /* A payload runs until the next one starts; the last ends at the sound
     * bank. */
    for (i = 0; i < count; i++) {
        u32 end = (i + 1 < count) ? out->images[i + 1].offset : ofs_sound_bank;

        if (out->images[i].offset < Q2_VRAM_SECTION_BASE + names_offset ||
            end < out->images[i].offset || end > ofs_sound_bank) {
            Q2_ERROR("%s: image %u spans 0x%X..0x%X, which is out of bounds",
                     path, i, out->images[i].offset, end);
            free(out->images);
            q2_buf_free(&out->buf);
            return Q2_ERR_BAD_FORMAT;
        }
        out->images[i].packed_size = end - out->images[i].offset;
    }

    /* The packed name list runs from names_offset to the first payload, and
     * holds exactly `count` NUL-terminated names. This is the cross-check that
     * validates the whole layout formula: if any of the three counts were
     * wrong, the walk would not land on the first payload. */
    name = (const char *)(a + names_offset);
    for (i = 0; i < count; i++) {
        const char *end = (const char *)(p + out->images[0].offset);
        const char *q   = name;

        while (q < end && *q != '\0')
            q++;

        if (q >= end) {
            Q2_ERROR("%s: name list holds fewer than %u names — the section "
                     "layout does not add up", path, count);
            free(out->images);
            q2_buf_free(&out->buf);
            return Q2_ERR_BAD_FORMAT;
        }

        out->images[i].name = name;
        name = q + 1;
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

size_t q2_vram_decoded_size(const q2_vram_section *section, u32 index)
{
    if (!section || index >= section->image_count)
        return 0;
    return section->images[index].decoded_size;
}

q2_result q2_vram_decode(const q2_vram_section *section, u32 index,
                         u8 *out, size_t out_capacity, size_t *out_size)
{
    const q2_vram_image *img;
    size_t target, produced, consumed = 0;

    if (!section || !out || index >= section->image_count)
        return Q2_ERR_INVALID_ARG;

    img    = &section->images[index];
    target = img->decoded_size;

    if (target == 0)
        return Q2_ERR_BAD_FORMAT;
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

    /* Only the final record of a file carries padding, and never more than
     * three bytes, so anything else means the stream is not what we think. */
    if (img->packed_size - consumed > 3) {
        Q2_WARN("vram: image %u left %zu bytes unconsumed",
                index, (size_t)img->packed_size - consumed);
    }

    if (out_size)
        *out_size = produced;

    return Q2_OK;
}

bool q2_vram_find_by_name(const q2_vram_section *section, const char *name,
                          u32 *index_out)
{
    u32 i;

    if (!section || !name)
        return false;

    for (i = 0; i < section->image_count; i++) {
        if (section->images[i].name &&
            name_equal_ci(section->images[i].name, name)) {
            if (index_out)
                *index_out = i;
            return true;
        }
    }
    return false;
}

bool q2_vram_image_rect(const q2_vram_section *section, u32 index, u32 slot,
                        q2_vram_rect *out)
{
    const q2_vram_image *img;
    u32 w_bytes, h_rows;

    if (!section || !out || index >= section->image_count)
        return false;

    img = &section->images[index];

    if (index < section->texpage_count) {
        w_bytes = Q2_VRAM_TEXPAGE_W;
        h_rows  = Q2_VRAM_TEXPAGE_H;
        out->x  = (s16)Q2_VRAM_TEXPAGE_X(slot);
        out->y  = (s16)Q2_VRAM_TEXPAGE_Y;
    } else {
        w_bytes = img->width;
        h_rows  = img->height;
        /* Standalone image placement is not established; the caller positions
         * these itself rather than us inventing coordinates. */
        out->x  = 0;
        out->y  = 0;
    }

    /* The engine halves the byte width to get halfwords (srl at 0x80069214). */
    out->w = (s16)(w_bytes >> 1);
    out->h = (s16)h_rows;

    return true;
}

void q2_vram_clut4_rect(const q2_vram_section *section, q2_vram_rect *out)
{
    u32 rows;

    if (!out)
        return;

    /* Four 16-halfword CLUTs per 64-halfword row, rounded up. */
    rows = section ? (section->clut4_count + 3) / 4 : 0;

    out->x = Q2_VRAM_CLUT4_X;
    out->y = Q2_VRAM_CLUT4_Y;
    out->w = Q2_VRAM_CLUT4_W;
    out->h = (s16)rows;
}

u16 q2_vram_clut4_id(u32 index)
{
    u32 x = 16u * (index & 3u);
    u32 y = (u32)Q2_VRAM_CLUT4_Y + (index >> 2);

    return (u16)(((y & 0x1FFu) << 6) | ((x & 0x3FFu) >> 4));
}

bool q2_vram_get_clut4(const q2_vram_section *section, u32 index, u16 out[16])
{
    const u8 *src;
    u32 i;

    if (!section || !out || !section->clut4 || index >= section->clut4_count)
        return false;

    src = section->clut4 + (size_t)index * (Q2_VRAM_CLUT4_ENTRIES * 2);

    for (i = 0; i < Q2_VRAM_CLUT4_ENTRIES; i++) {
        u16 entry = q2_rd_u16(src + i * 2);

        /* The load-time fix-up at 0x800762B4: every non-zero entry gets the STP
         * bit. A zero entry stays zero, which is how fully transparent texels
         * are expressed. */
        if (entry != 0)
            entry |= Q2_VRAM_STP_BIT;

        out[i] = entry;
    }
    return true;
}

bool q2_vram_get_clut8(const q2_vram_section *section, u32 image_index,
                       u16 out[256])
{
    const u8 *src;
    u32 i;

    if (!section || !out || !section->clut8 ||
        image_index >= section->clut8_count)
        return false;

    src = section->clut8 + (size_t)image_index * (Q2_VRAM_CLUT8_ENTRIES * 2);

    for (i = 0; i < Q2_VRAM_CLUT8_ENTRIES; i++)
        out[i] = q2_rd_u16(src + i * 2);

    return true;
}
