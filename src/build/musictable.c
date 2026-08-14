#include "musictable.h"

#include <string.h>

const char *const q2_music_files[Q2_MUSIC_FILES] = {
    "QUAKE_A", "QUAKE_B", "QUAKE_C", "QUAKE_D", "QUAKE_E"
};

q2_result q2_music_table_load(q2_music_table *out, const disc *d,
                              const q2_build_id *id)
{
    q2_exe exe;
    q2_result r;
    const u8 *rec;
    u32 i;

    if (!out || !d || !id)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (strcmp(id->serial, "SLES-01534") != 0) {
        Q2_WARN("music table location is unknown for build %s",
                id->serial[0] ? id->serial : "(unidentified)");
        return Q2_ERR_UNSUPPORTED;
    }

    r = q2_exe_load(&exe, d, id->exe_name[0] ? id->exe_name : NULL);
    if (r != Q2_OK)
        return r;

    rec = q2_exe_ptr(&exe, Q2_MUSICTABLE_ADDR_SLES01534,
                     Q2_MUSIC_COUNT * Q2_MUSIC_RECORD_SIZE);
    if (!rec) {
        q2_exe_free(&exe);
        return Q2_ERR_BAD_FORMAT;
    }

    for (i = 0; i < Q2_MUSIC_COUNT; i++) {
        const u8 *p = rec + (size_t)i * Q2_MUSIC_RECORD_SIZE;

        out->entry[i].file    = (s8)p[0];
        out->entry[i].channel = p[1];
        out->entry[i].tenths  = q2_rd_u16(p + 4);
    }
    out->count = Q2_MUSIC_COUNT;

    q2_exe_free(&exe);
    return Q2_OK;
}

const q2_music_entry *q2_music_get(const q2_music_table *t, int id)
{
    if (!t || id < 0 || (u32)id >= t->count)
        return NULL;
    return &t->entry[id];
}

const char *q2_music_file_name(const q2_music_table *t, int id)
{
    const q2_music_entry *e = q2_music_get(t, id);

    if (!e || e->file < 0 || e->file >= Q2_MUSIC_FILES)
        return NULL;
    return q2_music_files[e->file];
}
