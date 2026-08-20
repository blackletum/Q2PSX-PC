#include "classtable.h"

#include "exe.h"

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

q2_result q2_class_table_load(q2_class_table *out, const disc *d,
                              const q2_build_id *id)
{
    q2_exe exe;
    q2_result r;
    u32 addr, end, count, i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("class table location is unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }

    r = q2_exe_load(&exe, d, id->exe_name);
    if (r != Q2_OK)
        return r;

    addr = Q2_CLASSTABLE_ADDR_SLES01534;
    end  = Q2_CLASSTABLE_END_SLES01534;

    if (!q2_exe_contains(&exe, addr, end - addr)) {
        q2_exe_free(&exe);
        return Q2_ERR_BAD_FORMAT;
    }

    count = (end - addr) / Q2_CLASS_RECORD_SIZE;
    out->entries = (q2_class_entry *)calloc(count, sizeof(q2_class_entry));
    if (!out->entries) {
        q2_exe_free(&exe);
        return Q2_ERR_NO_MEMORY;
    }

    for (i = 0; i < count; i++) {
        const u8 *rec = q2_exe_ptr(&exe, addr + i * Q2_CLASS_RECORD_SIZE,
                                   Q2_CLASS_RECORD_SIZE);
        q2_class_entry *e = &out->entries[i];

        if (!rec)
            break;

        e->id = q2_rd_u32(rec + 0x00);
        memcpy(e->name, rec + 0x04, Q2_CLASS_NAME_LEN);
        e->name[Q2_CLASS_NAME_LEN] = '\0';
        e->fn_a   = q2_rd_u32(rec + 0x14);
        e->fn_b   = q2_rd_u32(rec + 0x24);
        e->gib_health = q2_rd_s16(rec + 0x28);
        e->class_byte = rec[0x20];
        e->health = q2_rd_s16(rec + 0x2A);
        e->is_player = (e->fn_a != 0 && e->fn_b != 0);
    }

    out->count = i;
    q2_exe_free(&exe);
    return Q2_OK;
}

void q2_class_table_free(q2_class_table *t)
{
    if (!t)
        return;
    free(t->entries);
    memset(t, 0, sizeof(*t));
}

const q2_class_entry *q2_class_find(const q2_class_table *t, u32 id)
{
    u32 i;

    if (!t)
        return NULL;

    for (i = 0; i < t->count; i++)
        if (t->entries[i].id == id && t->entries[i].name[0])
            return &t->entries[i];
    return NULL;
}

const q2_class_entry *q2_class_find_name(const q2_class_table *t,
                                         const char *name)
{
    u32 i;

    if (!t || !name)
        return NULL;

    for (i = 0; i < t->count; i++)
        if (name_casecmp(t->entries[i].name, name) == 0)
            return &t->entries[i];
    return NULL;
}
