/*
 * musictable.h — which stream a music id names, and answer to open question #13.
 *
 * The question was "which in-game situation selects which music id", and it was
 * stuck because the id lives in a `$gp` global (`0x800B2E5C`) whose writer had
 * not been located. Both halves are now read, and they are two separate tables.
 *
 * ---------------------------------------------------------------------------
 * The id names a stream — 0x800A1DD8, 22 records of six bytes
 * ---------------------------------------------------------------------------
 * The player at `0x80071718` indexes it by the id with the multiply spelled out
 * at 0x80071760 (`id*2 + id`, doubled — a stride of six), then:
 *
 *     +0  s8   file, 0..4, indexing the five `QUAKE_x.XAI` names at 0x800A1E5C;
 *              NEGATIVE means "no stream", which is what ids 0 and 1 hold
 *              (0x80071778's `bltz` takes the other arm entirely)
 *     +1  u8   channel, 0..3            (0x800717E8)
 *     +2  u16  zero on every record
 *     +4  u16  duration in TENTHS OF A SECOND
 *
 * The duration's unit is not asserted, it is checked: `q2psx-inspect music`
 * demultiplexes every stream on the disc and measures it, and all twenty agree
 * with this field to within the rounding of one sector — QUAKE_A channel 2
 * reads 1374 against a measured 137.4 s, QUAKE_E channel 3 reads 2729 against
 * 273.0 s. Twenty independent agreements is what makes the reading a fact
 * rather than a plausible scale.
 *
 * Ids 2..21 are therefore exactly the twenty (file, channel) pairs the disc
 * carries, in file-major order. Ids 0 and 1 are silence.
 *
 * ---------------------------------------------------------------------------
 * What SELECTS an id — the level's own playlist
 * ---------------------------------------------------------------------------
 * `0x80071A68` is a cursor walk, and the list it walks is in the LEVEL TABLE:
 *
 *     p = cursor;  cursor = p + 1;  v = *cursor
 *     v == 0   ->  the list is over
 *     v <  0   ->  cursor += v          (a relative jump BACK)
 *     id = *cursor
 *
 * A level record's bytes at +0x22..+0x28 are seven ids, +0x29 is `0xF9` — minus
 * seven, which lands the cursor back on +0x22 — and +0x2A is the zero that is
 * therefore never reached. So **every level loops a seven-track playlist**, and
 * the "always 0xF900 | n at +0x28" that leveltable.h recorded was a misread of
 * the seventh id, the loop byte and two zeros as one word.
 *
 * The seven are a lead track and then six consecutive ones from lead + 3,
 * wrapped into 2..17: BASE0 is 14 then 17, 2, 3, 4, 5, 6; BASE1 is 8 then
 * 11..16; BASE2 is 19 then 6..11; JAIL2 is 2 then 5..10. Ids 18..21 —
 * QUAKE_E's four channels — appear in no level's list at all.
 */
#ifndef Q2PSX_MUSICTABLE_H
#define Q2PSX_MUSICTABLE_H

#include "disc.h"
#include "exe.h"
#include "ident.h"
#include "q2psx.h"

#define Q2_MUSIC_RECORD_SIZE 6
#define Q2_MUSIC_COUNT      22   /* 0 and 1 are silence; 2..21 are the streams */
#define Q2_MUSIC_FILES       5   /* QUAKE_A .. QUAKE_E                         */

/* PAL build. Per-build like every other table here, and refusing an
 * uncatalogued executable is the point. */
#define Q2_MUSICTABLE_ADDR_SLES01534  0x800A1DD8u

/* The five stream file names, in the order the id's `file` field indexes. */
extern const char *const q2_music_files[Q2_MUSIC_FILES];

typedef struct q2_music_entry {
    s8  file;        /* < 0: this id is silence          */
    u8  channel;
    u16 tenths;      /* duration, in tenths of a second  */
} q2_music_entry;

typedef struct q2_music_table {
    q2_music_entry entry[Q2_MUSIC_COUNT];
    u32            count;
} q2_music_table;

/*
 * Read the table out of the disc's boot executable. Fails with
 * Q2_ERR_UNSUPPORTED on a build whose address is not catalogued rather than
 * reading a plausible-looking wrong one.
 */
q2_result q2_music_table_load(q2_music_table *out, const disc *d,
                              const q2_build_id *id);

/* The entry for an id, or NULL when the id is out of range. */
const q2_music_entry *q2_music_get(const q2_music_table *t, int id);

/* `Q2DATA/<NAME>.XAI` for an id, or NULL when the id names no stream. NULL is
 * the honest answer for ids 0 and 1, which the player's own `bltz` treats as a
 * different case entirely. */
const char *q2_music_file_name(const q2_music_table *t, int id);

#endif /* Q2PSX_MUSICTABLE_H */
