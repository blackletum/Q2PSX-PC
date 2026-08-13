/*
 * ident.h — identifying which release of Quake II PSX the user handed us.
 *
 * Detection is per *build*, not per region. That distinction matters: the game's
 * text, level table and pickup table all live inside the executable, so a
 * localised or revised release moves them. Keying data-table offsets off "PAL"
 * would be wrong the moment a German disc shows up. Keying them off the exact
 * executable is always right.
 *
 * The identification ladder, most to least specific:
 *
 *   1. SHA-256 of the boot executable   — exact, distinguishes revisions.
 *   2. Serial from SYSTEM.CNF           — e.g. SLES-01534. Names the release.
 *   3. Executable size + PVD timestamp  — corroborating evidence.
 *   4. Presence of the Q2DATA tree      — enough to attempt a generic run.
 *
 * An unknown disc that still has a plausible Q2DATA tree is not rejected. It is
 * reported as unknown and run in generic mode, because refusing to boot on a
 * disc we have not catalogued would fail the "just works" promise for regional
 * releases nobody has dumped for us yet.
 */
#ifndef Q2PSX_IDENT_H
#define Q2PSX_IDENT_H

#include "disc.h"
#include "q2psx.h"
#include "sha256.h"

typedef enum q2_region {
    Q2_REGION_UNKNOWN = 0,
    Q2_REGION_NTSC_U,      /* SLUS — North America   */
    Q2_REGION_PAL,         /* SLES — Europe          */
    Q2_REGION_NTSC_J       /* SLPS/SLPM — Japan      */
} q2_region;

/* Video standard, which on this console also sets the game's tick rate. */
typedef enum q2_video_std {
    Q2_VIDEO_NTSC = 0,     /* 60 Hz, 256x240 */
    Q2_VIDEO_PAL           /* 50 Hz, 256x256 */
} q2_video_std;

/*
 * A catalogued release. `exe_sha256_hex` is the primary key; an entry with an
 * empty hash matches on serial alone, which is how we support a release whose
 * hash we have not been given yet.
 */
typedef struct q2_build_desc {
    const char  *name;             /* human-readable release name        */
    const char  *serial;           /* "SLES-01534"                       */
    const char  *exe_name;         /* "SLES_015.34"                      */
    const char  *exe_sha256_hex;   /* "" if unknown                      */
    u32          exe_size;         /* 0 if unknown                       */
    q2_region    region;
    q2_video_std video;
    const char  *language;
    const char  *notes;
} q2_build_desc;

/* What we concluded about the disc in hand. */
typedef struct q2_build_id {
    const q2_build_desc *desc;      /* NULL when the build is uncatalogued */

    char        serial[16];
    char        exe_name[32];
    char        exe_sha256[SHA256_HEX_SIZE];
    u32         exe_size;
    char        creation_time[18];
    u32         volume_sectors;

    q2_region   region;             /* inferred from the serial prefix even
                                     * when the exact build is unknown     */
    q2_video_std video;

    bool        catalogued;         /* matched a known build exactly       */
    bool        data_tree_ok;       /* Q2DATA/LEVELS present and populated */
    int         level_dir_count;
} q2_build_id;

/* Inspect a disc and fill in `out`. Returns Q2_ERR_BAD_FORMAT only when the disc
 * is not plausibly Quake II PSX at all. */
q2_result q2_identify(const disc *d, q2_build_id *out);

/* The catalogue, for tooling and for listing what is supported. */
const q2_build_desc *q2_build_catalogue(int *count);

const char *q2_region_str(q2_region r);
const char *q2_video_std_str(q2_video_std v);

/* Nominal game tick rate for a build. PAL releases of this era generally ran
 * their logic off the 50 Hz vertical blank, which makes them play slower than
 * NTSC; the port exposes this so it can be overridden. */
int q2_build_tick_rate(const q2_build_id *id);

#endif /* Q2PSX_IDENT_H */
