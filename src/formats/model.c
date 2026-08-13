#include "model.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Bank walking                                                               */
/* ------------------------------------------------------------------------- */

/* Count the models in a chunk by walking the ofs_end chain. A model whose
 * ofs_end is zero is the last one and owns the rest of the chunk. */
static q2_result bank_init(q2_model_bank *out, const dat_chunk *chunk)
{
    u32 cursor = 0, count = 0;

    if (!out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (!chunk)
        return Q2_ERR_BAD_FORMAT;

    /* A zero-length CastList is legal — 17 zones ship one. */
    if (chunk->size == 0)
        return Q2_OK;

    out->data = chunk->data;
    out->size = chunk->size;

    while (cursor + Q2_MODEL_HEADER_SIZE <= chunk->size) {
        u32 ofs_end = q2_rd_u32(chunk->data + cursor + 0x3C);

        count++;

        if (ofs_end == 0)
            break;                   /* last model; its blocks run to the end */

        if (ofs_end < Q2_MODEL_HEADER_SIZE ||
            ofs_end > chunk->size - cursor) {
            Q2_ERROR("model: model %u at 0x%X declares size %u, which escapes "
                     "the %u-byte chunk", count - 1, cursor, ofs_end,
                     chunk->size);
            memset(out, 0, sizeof(*out));
            return Q2_ERR_BAD_FORMAT;
        }

        cursor += ofs_end;
    }

    out->count = count;
    return Q2_OK;
}

q2_result q2_model_bank_from_common(q2_model_bank *out, const q2_common_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return bank_init(out, f->chunk[Q2_COMMON_CAST_LIST]);
}

q2_result q2_model_bank_from_zone(q2_model_bank *out, const q2_zone_file *f)
{
    if (!out || !f)
        return Q2_ERR_INVALID_ARG;
    return bank_init(out, f->chunk[Q2_ZONE_CAST_LIST]);
}

/* ------------------------------------------------------------------------- */
/* One model                                                                  */
/* ------------------------------------------------------------------------- */

q2_result q2_model_get(const q2_model_bank *bank, u32 index, q2_model *out)
{
    const u8 *p;
    u32 cursor = 0, i;
    u32 sum_faces = 0, sum_verts = 0, hi = 0;
    q2_model_header *h;

    if (!bank || !out || index >= bank->count)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /* Chain-walk to the requested model. bank_init has already proved every
     * step in the chain is in bounds, so this cannot run off the end. */
    for (i = 0; i < index; i++)
        cursor += q2_rd_u32(bank->data + cursor + 0x3C);

    p = bank->data + cursor;
    h = &out->hdr;

    memcpy(h->name, p + 0x08, 12);
    h->name[12] = '\0';

    h->num_faces   = q2_rd_u16(p + 0x14);
    h->num_parts   = q2_rd_u16(p + 0x16);
    h->ext0        = q2_rd_s16(p + 0x18);
    h->ext1        = q2_rd_s16(p + 0x1A);
    h->ext2        = q2_rd_s16(p + 0x1C);
    h->ext3        = q2_rd_s16(p + 0x1E);
    h->ofs_faces   = q2_rd_u32(p + 0x20);
    h->ofs_verts   = q2_rd_u32(p + 0x24);
    h->ofs_parts   = q2_rd_u32(p + 0x28);
    h->ofs_block_a = q2_rd_u32(p + 0x2C);
    h->ofs_block_b = q2_rd_u32(p + 0x30);
    h->ofs_block_c = q2_rd_u32(p + 0x34);
    h->ofs_block_d = q2_rd_u32(p + 0x38);
    h->ofs_end     = q2_rd_u32(p + 0x3C);

    out->base = p;
    out->size = h->ofs_end ? h->ofs_end : (bank->size - cursor);

    /* Every identity below holds on all 1,723 models on the disc, so a failure
     * means we have misread the chunk, not that the file is merely unusual. */
    if (h->ofs_verts != Q2_MODEL_HEADER_SIZE) {
        Q2_ERROR("model %u (%s): ofs_verts is 0x%X, not 0x40",
                 index, h->name, h->ofs_verts);
        return Q2_ERR_BAD_FORMAT;
    }
    if (h->ofs_parts < h->ofs_verts || h->ofs_faces < h->ofs_parts ||
        h->ofs_parts > out->size || h->ofs_faces > out->size)
        return Q2_ERR_BAD_FORMAT;
    if ((h->ofs_parts - h->ofs_verts) % Q2_MODEL_VERT_SIZE)
        return Q2_ERR_BAD_FORMAT;
    if (h->ofs_faces - h->ofs_parts != (u32)Q2_MODEL_PART_SIZE * h->num_parts)
        return Q2_ERR_BAD_FORMAT;
    if (out->size - h->ofs_faces < (u32)Q2_MODEL_FACE_SIZE * h->num_faces)
        return Q2_ERR_BAD_FORMAT;

    h->num_verts = (h->ofs_parts - h->ofs_verts) / Q2_MODEL_VERT_SIZE;

    /* Derive the scratch window size and check the part table sums exactly.
     * Both sums are exact on 1,723/1,723, so they are a real integrity gate
     * rather than a heuristic. */
    for (i = 0; i < h->num_parts; i++) {
        const u8 *pp  = p + h->ofs_parts + (size_t)i * Q2_MODEL_PART_SIZE;
        u32 base      = q2_rd_u8(pp + 2);
        u32 nverts    = q2_rd_u8(pp + 3);
        u32 end       = base + nverts;

        if (end > Q2_MODEL_SCRATCH_MAX) {
            Q2_ERROR("model %u (%s): part %u needs scratch slot %u, past the "
                     "engine's %u-entry buffer",
                     index, h->name, i, end, Q2_MODEL_SCRATCH_MAX);
            return Q2_ERR_BAD_FORMAT;
        }
        if (end > hi)
            hi = end;

        sum_faces += q2_rd_u16(pp + 0);
        sum_verts += nverts;
    }

    if (sum_faces != h->num_faces || sum_verts != h->num_verts) {
        Q2_ERROR("model %u (%s): parts sum to %u faces / %u verts, header says "
                 "%u / %u", index, h->name, sum_faces, sum_verts,
                 h->num_faces, h->num_verts);
        return Q2_ERR_BAD_FORMAT;
    }

    out->scratch_size = hi;
    return Q2_OK;
}

bool q2_model_get_vertex(const q2_model *m, u32 index, q2_model_vertex *out)
{
    const u8 *v;

    if (!m || !out || index >= m->hdr.num_verts)
        return false;

    v = m->base + m->hdr.ofs_verts + (size_t)index * Q2_MODEL_VERT_SIZE;

    out->x = q2_rd_s16(v + 0);
    out->y = q2_rd_s16(v + 2);
    out->z = q2_rd_s16(v + 4);

    /* The stored order is z, x, y. Reading it as x, y, z — as an earlier pass
     * documented — yields a vector uncorrelated with the surface. */
    out->nz = q2_rd_s16(v + 6);
    out->nx = q2_rd_s16(v + 8);
    out->ny = q2_rd_s16(v + 10);

    return true;
}

bool q2_model_get_part(const q2_model *m, u32 index, q2_model_part *out)
{
    const u8 *p;

    if (!m || !out || index >= m->hdr.num_parts)
        return false;

    p = m->base + m->hdr.ofs_parts + (size_t)index * Q2_MODEL_PART_SIZE;

    out->num_faces = q2_rd_u16(p + 0);
    out->vert_base = q2_rd_u8(p + 2);
    out->num_verts = q2_rd_u8(p + 3);

    return true;
}

bool q2_model_get_face(const q2_model *m, u32 index, q2_model_face *out)
{
    const u8 *f;
    int i;

    if (!m || !out || index >= m->hdr.num_faces)
        return false;

    f = m->base + m->hdr.ofs_faces + (size_t)index * Q2_MODEL_FACE_SIZE;

    for (i = 0; i < 4; i++) {
        out->v[i]     = q2_rd_u8(f + i);
        out->uv[i][0] = q2_rd_u8(f + 4 + i * 2);
        out->uv[i][1] = q2_rd_u8(f + 4 + i * 2 + 1);
    }
    out->flags   = q2_rd_u8(f + 12);
    out->texture = q2_rd_u8(f + 13);

    return true;
}

bool q2_model_is_static(const q2_model *m)
{
    u32 i;

    if (!m)
        return false;

    for (i = 0; i < m->hdr.num_parts; i++) {
        const u8 *p = m->base + m->hdr.ofs_parts + (size_t)i * Q2_MODEL_PART_SIZE;

        if (q2_rd_u8(p + 2) != 0)
            return false;
    }
    return true;
}

q2_result q2_model_bake_indices(const q2_model *m, u16 *out)
{
    s32 map[Q2_MODEL_SCRATCH_MAX];
    u32 p, k, f, i, cv = 0, fi = 0;

    if (!m || !out)
        return Q2_ERR_INVALID_ARG;

    for (i = 0; i < Q2_MODEL_SCRATCH_MAX; i++)
        map[i] = -1;

    /* Parts MUST be walked forward in file order. A part's faces may read slots
     * written by an EARLIER part — 21,217 faces do, all in articulated models —
     * and reversing the walk produces exactly 18,995 reads of never-written
     * slots across the disc. */
    for (p = 0; p < m->hdr.num_parts; p++) {
        const u8 *pp = m->base + m->hdr.ofs_parts + (size_t)p * Q2_MODEL_PART_SIZE;
        u32 nfaces   = q2_rd_u16(pp + 0);
        u32 base     = q2_rd_u8(pp + 2);
        u32 nverts   = q2_rd_u8(pp + 3);

        /*
         * LAST WRITER WINS. This one line is the INFERRED part of the format.
         * A rival rule — storage = cv + (index - base), with no shared buffer —
         * also resolves 100% of the disc's indices in range and differs on
         * 18,283 articulated faces; the geometry cannot separate them. We take
         * this one because it is the only rule reaching 100.0000% storage
         * coverage on every model, where the rival orphans 746 vertices. If the
         * per-part transform matrices ever settle the question the other way,
         * this loop is the only thing that changes.
         */
        for (k = 0; k < nverts; k++)
            map[base + k] = (s32)(cv + k);
        cv += nverts;

        for (f = 0; f < nfaces; f++, fi++) {
            const u8 *fp;

            if (fi >= m->hdr.num_faces)
                return Q2_ERR_BAD_FORMAT;

            fp = m->base + m->hdr.ofs_faces + (size_t)fi * Q2_MODEL_FACE_SIZE;

            for (i = 0; i < 4; i++) {
                u32 ix = q2_rd_u8(fp + i);
                s32 s;

                /* Range-check BEFORE indexing, not after. The natural way to
                 * write this reads map[ix] first and tests the result, which
                 * cannot catch an out-of-range index at all. */
                if (ix >= m->scratch_size)
                    return Q2_ERR_BAD_FORMAT;

                s = map[ix];
                if (s < 0)
                    return Q2_ERR_BAD_FORMAT;   /* never fires on retail data */

                out[fi * 4 + i] = (u16)s;
            }
        }
    }

    return (fi == m->hdr.num_faces) ? Q2_OK : Q2_ERR_BAD_FORMAT;
}
