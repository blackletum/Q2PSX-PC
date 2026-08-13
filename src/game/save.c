#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr_u32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static void wr_s32(u8 *p, s32 v) { wr_u32(p, (u32)v); }

void q2_save_free(q2_save *s)
{
    if (!s)
        return;
    free(s->event_flags);
    memset(s, 0, sizeof(*s));
}

q2_result q2_save_capture(q2_save *out, const q2_sim *sim,
                          const q2_inventory *inv,
                          const char *serial, const char *map, s32 zone)
{
    if (!out || !sim || !inv || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (serial)
        snprintf(out->serial, sizeof(out->serial), "%s", serial);
    snprintf(out->map, sizeof(out->map), "%s", map);
    out->zone = zone;

    out->pos[0] = sim->player.pos[0];
    out->pos[1] = sim->player.pos[1];
    out->pos[2] = sim->player.pos[2];
    out->yaw    = sim->player.yaw;
    out->pitch  = sim->player.pitch;

    out->inventory = *inv;

    /*
     * Script state. A save that restores position and inventory but not which
     * triggers have fired drops the player into a level where every door they
     * opened is shut again — so this is copied, not skipped.
     */
    if (sim->events_ready && sim->event_rt.record_count > 0) {
        u32 n = sim->event_rt.record_count;

        out->event_flags = (u8 *)malloc(n);
        if (!out->event_flags)
            return Q2_ERR_NO_MEMORY;

        memcpy(out->event_flags, sim->event_rt.flags, n);
        out->event_count = n;
    }

    return Q2_OK;
}

q2_result q2_save_apply(const q2_save *s, q2_sim *sim, q2_inventory *inv,
                        const char *serial, const char *map)
{
    if (!s || !sim || !inv)
        return Q2_ERR_INVALID_ARG;

    /* A save from a different release would restore coordinates into a
     * different map's geometry, because the level table and script offsets are
     * per-build. Refusing is the only safe answer. */
    if (serial && s->serial[0] && strcmp(serial, s->serial) != 0) {
        Q2_ERROR("save is for build %s, this disc is %s", s->serial, serial);
        return Q2_ERR_UNSUPPORTED;
    }

    /* The caller loads the map; this only restores into it. Applying a save to
     * the wrong map would put the player inside geometry. */
    if (map && strcmp(map, s->map) != 0) {
        Q2_ERROR("save is for map %s, but %s is loaded", s->map, map);
        return Q2_ERR_INVALID_ARG;
    }

    sim->player.pos[0] = s->pos[0];
    sim->player.pos[1] = s->pos[1];
    sim->player.pos[2] = s->pos[2];
    sim->player.yaw    = s->yaw;
    sim->player.pitch  = s->pitch;
    sim->player.vel[0] = sim->player.vel[1] = sim->player.vel[2] = 0;

    *inv = s->inventory;

    if (s->event_flags && sim->events_ready) {
        u32 n = s->event_count;
        if (n > sim->event_rt.record_count)
            n = sim->event_rt.record_count;
        memcpy(sim->event_rt.flags, s->event_flags, n);
    }

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Header: magic(4) version(4) serial(16) map(16) zone(4) event_count(4)      */
/* then pos(12) yaw(4) pitch(4), the inventory, then the event flags.         */
/* ------------------------------------------------------------------------- */
#define SAVE_HEADER_SIZE (4 + 4 + Q2_SAVE_SERIAL_LEN + Q2_SAVE_MAP_LEN + 4 + 4)
#define SAVE_PLAYER_SIZE (12 + 4 + 4)

q2_result q2_save_write(const q2_save *s, const char *path)
{
    FILE *f;
    u8 header[SAVE_HEADER_SIZE];
    u8 player[SAVE_PLAYER_SIZE];
    size_t at = 0;

    if (!s || !path)
        return Q2_ERR_INVALID_ARG;

    memset(header, 0, sizeof(header));
    memcpy(header, Q2_SAVE_MAGIC, 4);            at = 4;
    wr_u32(header + at, Q2_SAVE_VERSION);        at += 4;
    memcpy(header + at, s->serial, Q2_SAVE_SERIAL_LEN); at += Q2_SAVE_SERIAL_LEN;
    memcpy(header + at, s->map, Q2_SAVE_MAP_LEN);       at += Q2_SAVE_MAP_LEN;
    wr_s32(header + at, s->zone);                at += 4;
    wr_u32(header + at, s->event_count);

    memset(player, 0, sizeof(player));
    wr_s32(player + 0,  s->pos[0]);
    wr_s32(player + 4,  s->pos[1]);
    wr_s32(player + 8,  s->pos[2]);
    wr_s32(player + 12, s->yaw);
    wr_s32(player + 16, s->pitch);

    f = fopen(path, "wb");
    if (!f)
        return Q2_ERR_IO;

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(player, 1, sizeof(player), f) != sizeof(player) ||
        fwrite(&s->inventory, 1, sizeof(s->inventory), f) != sizeof(s->inventory)) {
        fclose(f);
        return Q2_ERR_IO;
    }

    if (s->event_count > 0 && s->event_flags) {
        if (fwrite(s->event_flags, 1, s->event_count, f) != s->event_count) {
            fclose(f);
            return Q2_ERR_IO;
        }
    }

    fclose(f);
    return Q2_OK;
}

q2_result q2_save_read(q2_save *out, const char *path)
{
    FILE *f;
    u8 header[SAVE_HEADER_SIZE];
    u8 player[SAVE_PLAYER_SIZE];
    size_t at = 0;
    u32 version;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    f = fopen(path, "rb");
    if (!f)
        return Q2_ERR_NOT_FOUND;

    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return Q2_ERR_BAD_FORMAT;
    }

    if (memcmp(header, Q2_SAVE_MAGIC, 4) != 0) {
        fclose(f);
        return Q2_ERR_BAD_FORMAT;
    }
    at = 4;

    version = q2_rd_u32(header + at); at += 4;
    if (version != Q2_SAVE_VERSION) {
        Q2_ERROR("save is version %u, this build reads version %d",
                 version, Q2_SAVE_VERSION);
        fclose(f);
        return Q2_ERR_UNSUPPORTED;
    }

    memcpy(out->serial, header + at, Q2_SAVE_SERIAL_LEN);
    out->serial[Q2_SAVE_SERIAL_LEN - 1] = '\0';
    at += Q2_SAVE_SERIAL_LEN;

    memcpy(out->map, header + at, Q2_SAVE_MAP_LEN);
    out->map[Q2_SAVE_MAP_LEN - 1] = '\0';
    at += Q2_SAVE_MAP_LEN;

    out->zone        = q2_rd_s32(header + at); at += 4;
    out->event_count = q2_rd_u32(header + at);

    if (fread(player, 1, sizeof(player), f) != sizeof(player) ||
        fread(&out->inventory, 1, sizeof(out->inventory), f) != sizeof(out->inventory)) {
        fclose(f);
        return Q2_ERR_BAD_FORMAT;
    }

    out->pos[0] = q2_rd_s32(player + 0);
    out->pos[1] = q2_rd_s32(player + 4);
    out->pos[2] = q2_rd_s32(player + 8);
    out->yaw    = q2_rd_s32(player + 12);
    out->pitch  = q2_rd_s32(player + 16);

    if (out->event_count > 0) {
        /* Guard the allocation against a corrupt count claiming gigabytes. */
        if (out->event_count > (1u << 20)) {
            fclose(f);
            return Q2_ERR_BAD_FORMAT;
        }

        out->event_flags = (u8 *)malloc(out->event_count);
        if (!out->event_flags) {
            fclose(f);
            return Q2_ERR_NO_MEMORY;
        }

        if (fread(out->event_flags, 1, out->event_count, f) != out->event_count) {
            q2_save_free(out);
            fclose(f);
            return Q2_ERR_BAD_FORMAT;
        }
    }

    fclose(f);
    return Q2_OK;
}
