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
    Q2_VIDEO_NTSC = 0,     /* 60 Hz fields, a 512x240 framebuffer */
    Q2_VIDEO_PAL           /* 50 Hz fields, a 512x248 framebuffer */
} q2_video_std;

/*
 * One run of the relocation from SLES-01534's address space into another
 * build's: [begin, end) in SLES-01534 addresses, plus `delta` to land in the
 * other executable.
 *
 * Every address this project writes down — in FORMATS.md, in a comment, in a
 * table loader — is SLES-01534's, because that is the disc the executable was
 * read from. A second build is the same source linked again with a handful of
 * functions changed, so its image is SLES-01534's in a few pieces, each moved
 * by a constant. The runs say which pieces and how far; `q2_exe_addr` (exe.h)
 * is what applies them. A range no run covers is code that CHANGED, and has no
 * counterpart to translate to.
 */
typedef struct q2_addr_run {
    u32 begin, end;
    s32 delta;
} q2_addr_run;

/*
 * A catalogued release. `exe_sha256_hex` is the primary key; an entry with an
 * empty hash matches on serial alone, which is how we support a release whose
 * hash we have not been given yet.
 *
 * `layout` is how its executable relates to SLES-01534's. SLES-01534 itself has
 * none and is the identity; a build with neither a layout nor that serial has
 * no known layout, and nothing is read out of its executable at all.
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
    const q2_addr_run *layout;     /* NULL on the reference build        */
    u32          layout_runs;
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

/* The catalogued build whose executable hashes to `sha256_hex`, or NULL. */
const q2_build_desc *q2_build_by_hash(const char *sha256_hex);

/* ...or whose boot file is `exe_name` ("SLES_015.34"), case-insensitively. */
const q2_build_desc *q2_build_by_exe_name(const char *exe_name);

/* True when the build's executable can be read by SLES-01534 address. */
bool q2_build_has_layout(const q2_build_desc *b);

/* ------------------------------------------------------------------------- */
/* What the video standard changes                                            */
/* ------------------------------------------------------------------------- */
/*
 * Read out of both executables and both discs, which are the same source built
 * twice: SLES-01534 with the video standard set to PAL and SLUS-00757 with it
 * set to NTSC. Aligning the two images word for word leaves 167 words that
 * relocation does not explain, and every one of them is on this list or in the
 * two text lists below. FORMATS.md §9.13 has the full census.
 *
 * TIME. The console counts game time in 1/300 s whichever the standard: the
 * vertical blank adds 6 on PAL and 5 on NTSC (main at 0x80018DB8, VBlank at
 * 0x800190F0 — both builds, same address), so dt is the same clock and a field
 * is a different number of it. Anything measured in FIELDS rather than dt —
 * a track's length, a logo's hold — is a different number per standard, and
 * the builds carry both.
 */
#define Q2_PAL_DT_PER_FIELD    6
#define Q2_NTSC_DT_PER_FIELD   5

int q2_video_field_hz(q2_video_std v);      /* 50 / 60                       */
int q2_video_dt_per_field(q2_video_std v);  /* 6 / 5                         */

/*
 * A music track's length is stored in tenths of a second and turned into
 * fields by SeekAndPlay: `x5` on PAL (0x800718B4) and `x6` on NTSC (0x800719E4).
 */
int q2_video_fields_per_tenth(q2_video_std v);

/*
 * PICTURE. SetVideoMode(1) and 248 lines on PAL, SetVideoMode(0) and 240 on
 * NTSC (InitialiseDrawNewGame, 0x800764E0 / 0x800764FC in SLES-01534). What is
 * laid out about the centre moves up by HALF the difference — four lines — in
 * the NTSC build's own constants: 105 of the 111 rows in the executable's page
 * tables (the memory card's SAVE FILE screen is the exception), every row of
 * QFRONT's pages, the pause screen's status line, the 2x2 split and the
 * four-player status bars.
 */
#define Q2_PAL_FB_HEIGHT       248
#define Q2_NTSC_FB_HEIGHT      240

int q2_video_fb_height(q2_video_std v);
int q2_video_mode(q2_video_std v);           /* the SetVideoMode argument    */

/* How far a screen laid out about the centre sits from PAL's: 0 or -4. */
int q2_video_centre_shift(q2_video_std v);

/*
 * TEXT. The North American build is American English, and says so in two
 * places. Its executable's own strings are respelt — AUTOCENTER, and Armor for
 * the four armour captions — and its string-table lookup, rewritten from 59
 * instructions to 136, asks for `<key>US` before `<key>`. The level data is
 * the SAME on both discs and already carries those variants: 16 of them, from
 * `Comm Center` to `Defense Command`, which only the NTSC executable asks for.
 * An empty string is "no variant".
 */
const char *q2_region_string_suffix(q2_region r);

#endif /* Q2PSX_IDENT_H */
