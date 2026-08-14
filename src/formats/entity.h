/*
 * entity.h — the COMMON.DAT chunks that populate a map: spawn points and lights.
 *
 * These are the simplest chunks on the disc and the first ones a running game
 * needs. Both are flat arrays with no internal offsets, which is why they were
 * fully decoded well before the geometry was.
 *
 * ---------------------------------------------------------------------------
 * StartPos — 28-byte records, terminated by 12 NUL bytes
 * ---------------------------------------------------------------------------
 * `(chunk_size - 12) % 28 == 0` on all 49 maps.
 *
 *     0x00  char[12]  name        every map has a default entry
 *     0x0C  s32       x
 *     0x10  s32       y
 *     0x14  s32       z
 *     0x18  s16       angle       observed -2047..+2047, i.e. signed on the
 *                                 4096-step circle. The unit is consistent but
 *                                 not proven, so treat it as INFERRED.
 *     0x1A  s16       zone        always less than the map's zone count
 *
 * The `zone` field matters: a spawn point names which zone of the map it lives
 * in, so placing a camera or a player means loading that zone, not zone 0.
 *
 * ---------------------------------------------------------------------------
 * Lights — 28-byte records, flat array, no header and no terminator
 * ---------------------------------------------------------------------------
 * `chunk_size % 28 == 0` on all 49 maps; 7,814 lights exist game-wide.
 *
 *     0x00  s32       x
 *     0x04  s32       y
 *     0x08  s32       z
 *     0x0C  u8        r
 *     0x0D  u8        g
 *     0x0E  u8        b
 *     0x0F  u8        pad         zero in all 7,814
 *     0x10  u8        always_255  0xFF in all 7,814
 *     0x11  u8        type        exactly five values game-wide: 7, 15, 23, 31,
 *                                 39, which is ((n<<3)|7) for n in 0..4. Those
 *                                 five n are the five arms of the STYLE switch
 *                                 at 0x8007572C, each selecting an animation
 *                                 curve: 0x800A1FDC, 0x800A2014, 0x800A2024 and
 *                                 0x800A203C, runs of 8-byte records in 1.3.12
 *                                 ending at a zero pair. The curves themselves
 *                                 are not decoded; the dispatch is.
 *     0x12  u16       radius      equals isqrt(radius_sq) for all 7,814
 *     0x14  u32       inner_radius_sq   always <= radius_sq
 *     0x18  u32       radius_sq   the authoritative cut-off
 *
 * `always_255` is often labelled "intensity" in similar formats, but it never
 * varies here, so calling it that would be inventing meaning. It is exposed
 * under a name that says what is actually known about it.
 *
 * Use `radius_sq` for culling rather than squaring `radius`: the stored radius
 * is the rounded integer square root, so squaring it reintroduces error the
 * original did not have.
 */
#ifndef Q2PSX_ENTITY_H
#define Q2PSX_ENTITY_H

#include "level.h"
#include "q2psx.h"

#define Q2_STARTPOS_SIZE 28
#define Q2_LIGHT_SIZE    28

typedef struct q2_start_pos {
    char name[13];
    s32  x, y, z;
    s16  angle;      /* signed, 4096 == a full turn (INFERRED) */
    s16  zone;       /* which zone of the map this spawn is in */
} q2_start_pos;

typedef struct q2_light {
    s32 x, y, z;
    u8  r, g, b;
    u8  always_255;  /* 0xFF everywhere; meaning unproven */
    u8  type;        /* one of 7, 15, 23, 31, 39          */
    u16 radius;      /* isqrt(radius_sq)                  */
    u32 inner_radius_sq;
    u32 radius_sq;   /* prefer this for culling           */
} q2_light;

typedef struct q2_start_pos_list {
    const u8 *data;
    u32       count;
} q2_start_pos_list;

typedef struct q2_light_list {
    const u8 *data;
    u32       count;
} q2_light_list;

/* Both borrow the COMMON.DAT buffer, which must outlive them. */
q2_result q2_start_pos_parse(q2_start_pos_list *out, const q2_common_file *common);
q2_result q2_lights_parse(q2_light_list *out, const q2_common_file *common);

bool q2_start_pos_get(const q2_start_pos_list *list, u32 index, q2_start_pos *out);
bool q2_light_get(const q2_light_list *list, u32 index, q2_light *out);

/* Find a spawn point by name, case-insensitively. Returns false if absent. */
bool q2_start_pos_find(const q2_start_pos_list *list, const char *name,
                       q2_start_pos *out);

#endif /* Q2PSX_ENTITY_H */
