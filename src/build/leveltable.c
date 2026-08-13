#include "leveltable.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int name_casecmp(const char *a, const char *b)
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

/* Copy a fixed-width, NUL-padded name and trim trailing blanks. */
static void copy_name(char *dst, const u8 *src)
{
    size_t n;

    memcpy(dst, src, Q2_LEVEL_NAME_LEN);
    dst[Q2_LEVEL_NAME_LEN] = '\0';

    n = strlen(dst);
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t'))
        dst[--n] = '\0';
}

static bool name_is_printable(const char *s)
{
    if (!*s)
        return false;
    while (*s) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E)
            return false;
        s++;
    }
    return true;
}

q2_result q2_level_table_load(q2_level_table *out, const disc *d,
                              const q2_build_id *id)
{
    q2_result r;
    u32 offset, count, i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /*
     * The table's location is per-build. Refusing an uncatalogued build is the
     * point: a localised release moves this table, and reading the PAL offset
     * out of a different executable would yield names that look almost right.
     */
    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("level table location is unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }

    offset = Q2_LEVELTABLE_OFFSET_SLES01534;
    count  = Q2_LEVELTABLE_COUNT_SLES01534;

    if (!id->exe_name[0])
        return Q2_ERR_NOT_FOUND;

    r = disc_read_file(d, id->exe_name, &out->exe);
    if (r != Q2_OK)
        return r;

    if ((size_t)offset + (size_t)count * Q2_LEVEL_RECORD_SIZE > out->exe.size) {
        q2_buf_free(&out->exe);
        return Q2_ERR_BAD_FORMAT;
    }

    out->entries = (q2_level_entry *)calloc(count, sizeof(q2_level_entry));
    if (!out->entries) {
        q2_buf_free(&out->exe);
        return Q2_ERR_NO_MEMORY;
    }

    for (i = 0; i < count; i++) {
        const u8 *rec = out->exe.data + offset + (size_t)i * Q2_LEVEL_RECORD_SIZE;
        q2_level_entry *e = &out->entries[i];
        int k;

        copy_name(e->display, rec);
        copy_name(e->directory, rec + Q2_LEVEL_NAME_LEN);

        e->unknown_22 = rec[0x22];
        for (k = 0; k < 5; k++)
            e->sequence[k] = rec[0x23 + k];
        e->tail = q2_rd_s32(rec + 0x28);

        /* BADLEVEL is the engine's error placeholder, and the final record has
         * an all-zero body. Flagging both stops a caller trying to load them
         * as maps. */
        e->is_placeholder = (name_casecmp(e->directory, "BADLEVEL") == 0) ||
                            (e->tail == 0 && e->sequence[0] == 0 &&
                             e->unknown_22 == 0);

        if (!name_is_printable(e->directory))
            e->is_placeholder = true;
    }

    out->count = count;
    return Q2_OK;
}

void q2_level_table_free(q2_level_table *t)
{
    if (!t)
        return;
    free(t->entries);
    q2_buf_free(&t->exe);
    memset(t, 0, sizeof(*t));
}

const q2_level_entry *q2_level_find(const q2_level_table *t, const char *directory)
{
    u32 i;

    if (!t || !directory)
        return NULL;

    for (i = 0; i < t->count; i++) {
        if (t->entries[i].is_placeholder)
            continue;
        if (name_casecmp(t->entries[i].directory, directory) == 0)
            return &t->entries[i];
    }
    return NULL;
}

const q2_level_entry *q2_level_find_display(const q2_level_table *t,
                                            const char *display)
{
    u32 i;

    if (!t || !display)
        return NULL;

    for (i = 0; i < t->count; i++) {
        if (t->entries[i].is_placeholder)
            continue;
        if (name_casecmp(t->entries[i].display, display) == 0)
            return &t->entries[i];
    }
    return NULL;
}
