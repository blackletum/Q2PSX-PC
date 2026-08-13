/*
 * dat.h — the .DAT chunk container used by every level directory on the disc.
 *
 * Each Q2DATA/LEVELS/<MAP>/ directory holds:
 *
 *   COMMON.DAT   data shared by the whole map (entity classes, resources)
 *   ZONE<N>.DAT  one file per streamed zone — the actual playable geometry
 *   SNDVRAM.DAT  the sound bank uploaded to SPU RAM for this map
 *   MAP.ALL      present on one map only
 *
 * COMMON.DAT and ZONE*.DAT share a container format: a fixed-schema directory of
 * named chunks at the head of the file. Each directory entry is 16 bytes —
 * a 12-byte NUL-padded ASCII name followed by a little-endian u32 offset from
 * the start of the file.
 *
 *   offset  size  field
 *   0x00    12    name, NUL-padded ASCII
 *   0x0C     4    u32 offset to this chunk's data
 *
 * The directory has no explicit entry count. It does not need one: the first
 * entry's offset is where the chunk data begins, which is exactly where the
 * directory ends, so
 *
 *     entry_count = first_entry_offset / 16
 *
 * The final directory entry is a sentinel: its 12 name bytes are all zero and
 * its offset is the end of the chunk data, which on a well-formed file equals
 * the file size. That makes every chunk's length uniformly
 *
 *     size = next_entry.offset - this_entry.offset
 *
 * with no special case for the last one. Chunks whose offset equals their
 * successor's are empty — the schema is fixed per file type, so unused slots
 * are present but zero-length rather than omitted.
 *
 * See docs/FORMATS.md for the per-chunk layouts.
 */
#ifndef Q2PSX_DAT_H
#define Q2PSX_DAT_H

#include "q2psx.h"

#define DAT_NAME_LEN     12
#define DAT_ENTRY_SIZE   16
#define DAT_MAX_CHUNKS   64

typedef struct dat_chunk {
    char        name[DAT_NAME_LEN + 1];
    u32         offset;
    u32         size;
    const u8   *data;      /* points into the owning archive's buffer */
} dat_chunk;

typedef struct dat_archive {
    q2_buf     buf;                     /* owns the file contents        */
    dat_chunk  chunks[DAT_MAX_CHUNKS];
    int        chunk_count;             /* real chunks, excluding the sentinel */
    u32        end_offset;              /* sentinel offset — end of chunk data  */
} dat_archive;

/* Parse an in-memory .DAT. Takes ownership of `buf` on success, and leaves it
 * untouched on failure so the caller can still free it. */
q2_result dat_open_buf(dat_archive *ar, q2_buf *buf);

void dat_close(dat_archive *ar);

/* Case-insensitive chunk lookup. Returns NULL if absent. */
const dat_chunk *dat_find(const dat_archive *ar, const char *name);

/* True when the file at least *looks* like this container: plausible entry
 * count, ASCII names, and strictly non-decreasing in-bounds offsets. Used to
 * tell COMMON/ZONE files apart from SNDVRAM.DAT and MAP.ALL, which do not use
 * this layout. */
bool dat_probe(const u8 *data, size_t size);

#endif /* Q2PSX_DAT_H */
