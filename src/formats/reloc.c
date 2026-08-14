#include "reloc.h"

#include <string.h>

/* Write a 32-bit word back into the image, little-endian like the console. */
static void wr_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/*
 * Walk a fixup stream once.
 *
 * `image` may be NULL, in which case nothing is written and this becomes a
 * validating scan. Both paths share the walk so an audit cannot disagree with
 * the thing it audits.
 */
static q2_result reloc_walk(u8 *image, size_t image_size,
                            const u8 *stream, size_t stream_size,
                            u32 base, q2_reloc_stats *stats)
{
    size_t at = 0;
    q2_reloc_stats local;
    bool terminated = false;

    memset(&local, 0, sizeof(local));

    if (!stream)
        return Q2_ERR_INVALID_ARG;

    while (at + 4 <= stream_size) {
        u32 entry = q2_rd_u32(stream + at);
        u32 offset, type;

        at += 4;

        if (entry == Q2_RELOC_TERMINATOR) {
            terminated = true;
            break;
        }

        offset = entry & ~3u;
        type   = entry & 3u;

        local.fixups++;
        local.by_type[type]++;

        /* Every target is a whole word inside the image. */
        if ((size_t)offset + 4 > image_size) {
            local.out_of_range++;
            /* A HI16 still owns its addend word, so the stream must stay in
             * step even when a target is rejected. */
            if (type == Q2_RELOC_HI16 && at + 4 <= stream_size) {
                at += 4;
                local.addend_words++;
            }
            continue;
        }

        switch (type) {
        case Q2_RELOC_WORD32: {
            if (image) {
                u32 v = q2_rd_u32(image + offset);
                wr_u32(image + offset, v + base);
            }
            break;
        }

        case Q2_RELOC_HI16: {
            u32 addend;

            /*
             * The addend is the NEXT RAW WORD, not a tagged entry. Failing to
             * consume it leaves the walk one word out of phase, which still
             * decodes almost everything correctly — the flat-scan reading
             * scores 99.78% — and quietly corrupts the module.
             */
            if (at + 4 > stream_size)
                return Q2_ERR_BAD_FORMAT;

            addend = q2_rd_u32(stream + at);
            at += 4;
            local.addend_words++;

            if (image) {
                u32 v = q2_rd_u32(image + offset);
                /* +0x8000 is the standard MIPS HI16 bias: LO16 is
                 * sign-extended, so the high half is pre-compensated. */
                u32 hi = ((addend + base + 0x8000u) >> 16) & 0xFFFFu;
                wr_u32(image + offset, (v & 0xFFFF0000u) | hi);
            }
            break;
        }

        case Q2_RELOC_LO16: {
            if (image) {
                u32 v = q2_rd_u32(image + offset);
                wr_u32(image + offset,
                       (v & 0xFFFF0000u) | ((v + base) & 0xFFFFu));
            }
            break;
        }

        case Q2_RELOC_TARGET26:
        default: {
            if (image) {
                u32 v = q2_rd_u32(image + offset);
                u32 target = ((v & 0x03FFFFFFu) << 2) + base;
                wr_u32(image + offset,
                       (v & 0xFC000000u) | ((target >> 2) & 0x03FFFFFFu));
            }
            break;
        }
        }
    }

    if (!terminated) {
        Q2_ERROR("reloc: stream of %zu bytes has no terminator", stream_size);
        return Q2_ERR_BAD_FORMAT;
    }

    if (stats)
        *stats = local;

    return Q2_OK;
}

q2_result q2_reloc_apply(u8 *image, size_t image_size,
                         const u8 *stream, size_t stream_size,
                         u32 base, q2_reloc_stats *stats)
{
    if (!image)
        return Q2_ERR_INVALID_ARG;
    return reloc_walk(image, image_size, stream, stream_size, base, stats);
}

q2_result q2_reloc_scan(const u8 *stream, size_t stream_size,
                        size_t image_size, q2_reloc_stats *stats)
{
    return reloc_walk(NULL, image_size, stream, stream_size, 0, stats);
}

/* ------------------------------------------------------------------------- */
/* Module loading                                                             */
/* ------------------------------------------------------------------------- */

/* Both chunks are exactly four zero bytes when a map has no creatures. */
static bool chunk_is_empty(const dat_chunk *c)
{
    return !c || c->size <= 4;
}

/* The two module kinds differ only in how much of the chunk is preamble. */
static q2_result module_load(q2_ai_module *out, const dat_chunk *bin,
                             const dat_chunk *rel, size_t pre, u32 base)
{
    q2_result r;

    if (!out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (chunk_is_empty(bin) || chunk_is_empty(rel)) {
        /* 29 of 98 zone files have no creatures. Not an error. */
        out->empty = true;
        return Q2_OK;
    }

    if (bin->size <= pre || rel->size <= pre)
        return Q2_ERR_BAD_FORMAT;

    /* Relocation writes into the image, so take a private copy rather than
     * mutating the borrowed chunk. */
    r = q2_buf_alloc(&out->image, bin->size - pre);
    if (r != Q2_OK)
        return r;

    memcpy(out->image.data, bin->data + pre, bin->size - pre);

    r = q2_reloc_apply(out->image.data, out->image.size,
                       rel->data + pre, rel->size - pre,
                       base, NULL);
    if (r != Q2_OK) {
        q2_buf_free(&out->image);
        return r;
    }

    out->base = base;
    return Q2_OK;
}

q2_result q2_ai_module_load(q2_ai_module *out, const q2_common_file *common,
                            u32 base)
{
    if (!common)
        return Q2_ERR_INVALID_ARG;

    return module_load(out, common->chunk[Q2_COMMON_CRE_AI_BIN],
                       common->chunk[Q2_COMMON_CRE_AI_REL],
                       Q2_RELOC_CREAI_PREAMBLE, base);
}

q2_result q2_level_module_load(q2_ai_module *out, const q2_common_file *common,
                               u32 base)
{
    if (!common)
        return Q2_ERR_INVALID_ARG;

    return module_load(out, common->chunk[Q2_COMMON_LEVEL_BIN],
                       common->chunk[Q2_COMMON_LEVEL_REL],
                       Q2_RELOC_LEVEL_PREAMBLE, base);
}

void q2_ai_module_free(q2_ai_module *m)
{
    if (!m)
        return;
    q2_buf_free(&m->image);
    memset(m, 0, sizeof(*m));
}

u32 q2_ai_module_export(const q2_ai_module *m, u32 slot)
{
    if (!m || m->empty || !m->image.data || slot > 3)
        return 0;
    if (m->image.size < 16)
        return 0;
    return q2_rd_u32(m->image.data + slot * 4);
}
