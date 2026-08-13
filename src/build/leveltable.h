/*
 * leveltable.h — the executable's level table, and cross-map progression.
 *
 * Zone gates move the player between zones of one map. Moving between MAPS
 * needs this table, which is the only place a display name is tied to a
 * directory on the disc.
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 * 53 records of 56 bytes, at file offset 0x84EC8 in the PAL executable:
 *
 *     +0x00  char[12]  display name    e.g. "Base0", "COLD STORAGE"
 *     +0x0C  char[12]  directory name  e.g. "BASE0" — the Q2DATA/LEVELS folder
 *     +0x18  s32       always 0
 *     +0x1C  s32       always 0x80050008 — a pointer, identical in every record
 *     +0x20  u8[4]     see below
 *     +0x24  u8[4]     see below
 *     +0x28  s32       0xF900 | n, with n in 2..17; zero on two records
 *     +0x2C  s32[3]    always 0
 *
 * The stride was established empirically rather than assumed. Scanning for
 * mixed-case names with an uppercase twin nearby gives 31 hits, and the gaps
 * between them are 56 in 24 of 30 cases — the outliers being the records whose
 * display name is not a simple capitalisation of the directory.
 *
 * ---------------------------------------------------------------------------
 * The eight bytes at +0x20, and what is NOT claimed about them
 * ---------------------------------------------------------------------------
 * Bytes +0x23 through +0x27 form a RUN OF CONSECUTIVE VALUES that wraps from 17
 * back to 2. BASE1 reads 11,12,13,14,15; JAIL3 reads 13,14,15,16,17; LAB reads
 * 16,17,2,3,4 — the wrap is visible in the data. The same 2..17 range appears
 * in the low byte at +0x28.
 *
 * Sixteen values, five per level, drawn in sequence. That is suggestive of a
 * music playlist — the disc carries 19 distinct XA tracks — but it is only
 * suggestive, and no attempt is made here to act on it. The bytes are exposed
 * raw and named for their shape rather than a guessed purpose, because naming
 * them "music_track" would make every later reader believe it was established.
 *
 * The byte at +0x22 varies per level without an obvious pattern (BASE0 14,
 * BASE1 8, BASE2 19, JAIL2 2) and is likewise left unnamed.
 *
 * ---------------------------------------------------------------------------
 * Two records that are not levels
 * ---------------------------------------------------------------------------
 * "BADLEVEL" is an error placeholder. The final record pairs two names with an
 * all-zero body and is a terminator rather than a map. Both are reported so a
 * caller can skip them deliberately instead of trying to load them.
 */
#ifndef Q2PSX_LEVELTABLE_H
#define Q2PSX_LEVELTABLE_H

#include "ident.h"
#include "q2psx.h"

#define Q2_LEVEL_RECORD_SIZE 56
#define Q2_LEVEL_NAME_LEN    12

/* Where the table sits in the catalogued PAL build. A different build moves it,
 * which is exactly why identification is per-build — see ident.h. */
#define Q2_LEVELTABLE_OFFSET_SLES01534 0x84EC8
#define Q2_LEVELTABLE_COUNT_SLES01534  53

typedef struct q2_level_entry {
    char display[Q2_LEVEL_NAME_LEN + 1];   /* shown to the player      */
    char directory[Q2_LEVEL_NAME_LEN + 1]; /* Q2DATA/LEVELS/<this>     */

    u8   unknown_22;      /* varies per level, meaning unknown         */
    u8   sequence[5];     /* the consecutive run at +0x23..+0x27       */
    s32  tail;            /* 0xF900 | n, or 0                          */

    bool is_placeholder;  /* BADLEVEL or the terminator                */
} q2_level_entry;

typedef struct q2_level_table {
    q2_buf         exe;      /* owns the executable image */
    q2_level_entry *entries;
    u32             count;
} q2_level_table;

/*
 * Read the level table out of the disc's boot executable.
 *
 * Fails with Q2_ERR_UNSUPPORTED on a build whose table location is not known,
 * rather than reading a plausible-looking wrong address.
 */
q2_result q2_level_table_load(q2_level_table *out, const disc *d,
                              const q2_build_id *id);
void      q2_level_table_free(q2_level_table *t);

/* Find a level by its disc directory name, case-insensitively. */
const q2_level_entry *q2_level_find(const q2_level_table *t, const char *directory);

/* Find by the display name instead, which is what a script's LOADMAP names. */
const q2_level_entry *q2_level_find_display(const q2_level_table *t,
                                            const char *display);

#endif /* Q2PSX_LEVELTABLE_H */
