#include "dat.h"

#include <ctype.h>
#include <string.h>

static int dat_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* True when all 12 name bytes are zero — the directory's end sentinel. */
static bool dat_name_is_empty(const u8 *p)
{
    int i;

    for (i = 0; i < DAT_NAME_LEN; i++) {
        if (p[i] != 0)
            return false;
    }
    return true;
}

/* A directory name is 12 bytes: printable ASCII, then NUL padding to the end.
 * The all-zero sentinel is handled separately by the caller. */
static bool dat_name_is_plausible(const u8 *p)
{
    int i = 0;
    bool seen_nul = false;

    if (p[0] == 0)
        return false;

    for (i = 0; i < DAT_NAME_LEN; i++) {
        u8 c = p[i];
        if (c == 0) {
            seen_nul = true;
            continue;
        }
        if (seen_nul)
            return false;                       /* text after the terminator */
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

bool dat_probe(const u8 *data, size_t size)
{
    u32 first;
    int count, i;
    u32 prev = 0;

    if (!data || size < DAT_ENTRY_SIZE)
        return false;

    if (!dat_name_is_plausible(data))
        return false;

    first = q2_rd_u32(data + DAT_NAME_LEN);

    if (first < DAT_ENTRY_SIZE || first % DAT_ENTRY_SIZE != 0)
        return false;
    if ((size_t)first > size)
        return false;

    count = (int)(first / DAT_ENTRY_SIZE);
    if (count < 1 || count > DAT_MAX_CHUNKS)
        return false;

    for (i = 0; i < count; i++) {
        const u8 *e = data + (size_t)i * DAT_ENTRY_SIZE;
        u32 off = q2_rd_u32(e + DAT_NAME_LEN);

        /* The final entry carries no name; its offset is end-of-data. Any
         * *earlier* nameless entry means this is not the container we think. */
        if (dat_name_is_empty(e)) {
            if (i != count - 1)
                return false;
        } else if (!dat_name_is_plausible(e)) {
            return false;
        }

        if (off < prev)                 /* offsets never go backwards */
            return false;
        if ((size_t)off > size)
            return false;
        prev = off;
    }

    return true;
}

q2_result dat_open_buf(dat_archive *ar, q2_buf *buf)
{
    u32 first;
    int count, i;

    if (!ar || !buf || !buf->data)
        return Q2_ERR_INVALID_ARG;

    if (!dat_probe(buf->data, buf->size))
        return Q2_ERR_BAD_FORMAT;

    memset(ar, 0, sizeof(*ar));

    first = q2_rd_u32(buf->data + DAT_NAME_LEN);
    count = (int)(first / DAT_ENTRY_SIZE);

    /* The last directory entry is a nameless sentinel whose offset is the end of
     * the chunk data, so every real chunk's size is simply the gap to the next
     * entry — no special case for the final one. */
    ar->end_offset  = q2_rd_u32(buf->data + (size_t)(count - 1) * DAT_ENTRY_SIZE + DAT_NAME_LEN);
    ar->chunk_count = count - 1;

    if (ar->chunk_count < 1)
        return Q2_ERR_BAD_FORMAT;

    for (i = 0; i < ar->chunk_count; i++) {
        const u8 *e = buf->data + (size_t)i * DAT_ENTRY_SIZE;
        dat_chunk *c = &ar->chunks[i];
        u32 end;

        memcpy(c->name, e, DAT_NAME_LEN);
        c->name[DAT_NAME_LEN] = '\0';

        c->offset = q2_rd_u32(e + DAT_NAME_LEN);
        end       = q2_rd_u32(e + DAT_ENTRY_SIZE + DAT_NAME_LEN);

        if (end < c->offset)
            end = c->offset;

        c->size = end - c->offset;
        c->data = buf->data + c->offset;
    }

    /* The sentinel should land exactly on end-of-file. When it does not, the
     * file is either truncated or padded, and callers deserve to know. */
    if (ar->end_offset != (u32)buf->size) {
        Q2_DEBUG("dat: end sentinel is 0x%X but the file is 0x%zX bytes",
                 ar->end_offset, buf->size);
    }

    ar->buf = *buf;

    /* Ownership transferred. */
    buf->data = NULL;
    buf->size = 0;

    return Q2_OK;
}

void dat_close(dat_archive *ar)
{
    if (!ar)
        return;
    q2_buf_free(&ar->buf);
    memset(ar, 0, sizeof(*ar));
}

const dat_chunk *dat_find(const dat_archive *ar, const char *name)
{
    int i;

    if (!ar || !name)
        return NULL;

    for (i = 0; i < ar->chunk_count; i++) {
        if (dat_casecmp(ar->chunks[i].name, name) == 0)
            return &ar->chunks[i];
    }
    return NULL;
}
