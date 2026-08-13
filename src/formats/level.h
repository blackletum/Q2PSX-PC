/*
 * level.h — the chunk schema of COMMON.DAT and ZONE*.DAT.
 *
 * These names were recovered by taking a census of every level file on the disc
 * (`q2psx-inspect dats`), not by reading one file and guessing. The census over
 * the PAL build reports:
 *
 *   COMMON.DAT — 49 files, 14 chunks each, 2 schema variants.
 *                The only variation is BIGGUN, which carries one extra chunk
 *                ("GlintMod"). Every other name is present in all 49 files.
 *
 *   ZONE*.DAT  — 115 files, 5 schema variants, 12 or 14 chunks.
 *                The 14-chunk variant adds "CreAIRel" and "CreAIBin", the same
 *                pair COMMON.DAT carries. 98 of 115 zones have them.
 *
 * Two consequences for the loader:
 *
 *   1. The name set is closed. Every chunk on the disc maps to one of the enums
 *      below, so chunks can be resolved to a typed slot at load time.
 *   2. A chunk's directory *index* is not stable across files — the census marks
 *      most ZONE chunks "var". So lookup must be by name, never by position.
 *      That is why q2_level_chunk_index() exists rather than a constant table.
 *
 * What each chunk is believed to hold is documented in docs/FORMATS.md; this
 * header deliberately only commits to what the census proves, which is the name
 * set, the cardinality and the optionality.
 */
#ifndef Q2PSX_LEVEL_H
#define Q2PSX_LEVEL_H

#include "dat.h"
#include "q2psx.h"

/* ------------------------------------------------------------------------- */
/* COMMON.DAT — per-map data shared by every zone of that map.                */
/* ------------------------------------------------------------------------- */
typedef enum q2_common_chunk {
    Q2_COMMON_CRE_AI_REL = 0,   /* creature AI relocation/fixup table        */
    Q2_COMMON_LEVEL_REL,        /* level script relocation table; empty on 2 maps */
    Q2_COMMON_RESOURCES,        /* resource table — small, ~44 bytes typical  */
    Q2_COMMON_CAST_LIST,        /* entity/actor class list — the largest chunk */
    Q2_COMMON_TRIG_BOUNDS,      /* trigger volumes                            */
    Q2_COMMON_LIGHTS,
    Q2_COMMON_MODEL_NAMES,
    Q2_COMMON_START_POS,        /* spawn points — small, ~68 bytes            */
    Q2_COMMON_POPULATION,       /* which actors populate the map              */
    Q2_COMMON_STRINGS,          /* per-map text                               */
    Q2_COMMON_CRE_AI_BIN,       /* creature AI bytecode/data                  */
    Q2_COMMON_LEVEL_BIN,        /* level script bytecode; empty on 2 maps     */
    Q2_COMMON_USER_FUNCS,
    Q2_COMMON_EVENTS,
    Q2_COMMON_GLINT_MOD,        /* OPTIONAL — present on BIGGUN only          */
    Q2_COMMON_CHUNK_COUNT
} q2_common_chunk;

/* ------------------------------------------------------------------------- */
/* ZONE*.DAT — one streamed zone of a map. This is the playable world.        */
/* ------------------------------------------------------------------------- */
typedef enum q2_zone_chunk {
    Q2_ZONE_EVENTS = 0,
    Q2_ZONE_SCENE,              /* scene graph / world description            */
    Q2_ZONE_CAST_LIST,          /* per-zone actor spawns; empty in 17 zones   */
    Q2_ZONE_MAP_NAMES,
    Q2_ZONE_SPACE_LIGHTS,
    Q2_ZONE_SORT_DATA,          /* ordering-table / draw-sort data            */
    Q2_ZONE_MAP_MOD,            /* map geometry — one of the two largest      */
    Q2_ZONE_POINTS,             /* vertex pool — typically the largest chunk  */
    Q2_ZONE_PRIMARY_COLL,       /* collision, primary                         */
    Q2_ZONE_SECONDARY_COL,      /* collision, secondary                       */
    Q2_ZONE_PRIMARY_REMAP,
    Q2_ZONE_AREA_CONX,          /* area connectivity — portals / streaming    */
    Q2_ZONE_CRE_AI_REL,         /* OPTIONAL — present in 98 of 115 zones      */
    Q2_ZONE_CRE_AI_BIN,         /* OPTIONAL — present in 98 of 115 zones      */
    Q2_ZONE_CHUNK_COUNT
} q2_zone_chunk;

/* Canonical on-disc names, indexed by the enums above. */
extern const char *const q2_common_chunk_names[Q2_COMMON_CHUNK_COUNT];
extern const char *const q2_zone_chunk_names[Q2_ZONE_CHUNK_COUNT];

/* ------------------------------------------------------------------------- */
/* Typed access                                                               */
/* ------------------------------------------------------------------------- */

/* A level file with its chunks resolved to fixed slots. A slot is NULL when the
 * chunk is absent, and may point at a zero-length chunk when it is present but
 * empty — those are different states and the loader must not conflate them. */
typedef struct q2_common_file {
    dat_archive      ar;
    const dat_chunk *chunk[Q2_COMMON_CHUNK_COUNT];
} q2_common_file;

typedef struct q2_zone_file {
    dat_archive      ar;
    const dat_chunk *chunk[Q2_ZONE_CHUNK_COUNT];
} q2_zone_file;

/* Take ownership of `buf` and resolve its chunks. Fails if a mandatory chunk is
 * missing, which is the earliest point at which an unsupported build shows up. */
q2_result q2_common_open(q2_common_file *out, q2_buf *buf);
q2_result q2_zone_open(q2_zone_file *out, q2_buf *buf);

void q2_common_close(q2_common_file *f);
void q2_zone_close(q2_zone_file *f);

/* Name -> enum. Returns -1 for a name not in the known set, which is how an
 * unrecognised build announces itself. */
int q2_common_chunk_index(const char *name);
int q2_zone_chunk_index(const char *name);

#endif /* Q2PSX_LEVEL_H */
