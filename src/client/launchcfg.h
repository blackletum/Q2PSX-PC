/*
 * launchcfg.h — which disc the launcher opened last, kept between runs.
 *
 * One small text file, next to the saves directory:
 *
 *     Windows   %APPDATA%\Q2PSX-PC\launcher.cfg
 *     elsewhere $XDG_CONFIG_HOME/q2psx-pc/launcher.cfg
 *               (~/.config/q2psx-pc/launcher.cfg without it)
 *
 * holding `last=<path>` and up to Q2_LAUNCH_RECENT_MAX `recent=<path>` lines,
 * most recent first. A path is a disc image or a drive ("E:", "/dev/sr0"),
 * whatever disc_open was handed. Unknown lines are ignored, so a newer build's
 * file still loads in an older one. The environment variable
 * Q2PSX_LAUNCHER_CFG names another file, for a portable install or a test.
 *
 * No SDL here: the launcher draws, this remembers, and the tests can reach it.
 */
#ifndef Q2PSX_LAUNCHCFG_H
#define Q2PSX_LAUNCHCFG_H

#include "q2psx.h"

#define Q2_LAUNCH_PATH_MAX   1024
#define Q2_LAUNCH_RECENT_MAX 8

typedef struct q2_launch_cfg {
    char last[Q2_LAUNCH_PATH_MAX];
    char recent[Q2_LAUNCH_RECENT_MAX][Q2_LAUNCH_PATH_MAX];
    int  recent_count;
} q2_launch_cfg;

/* The platform's file, as above. Never NULL; a relative "launcher.cfg" when
 * no home directory can be found. */
const char *q2_launch_cfg_default_path(void);

/* A missing or unreadable file is an empty configuration, not an error; the
 * return says whether a file was read. */
bool q2_launch_cfg_load(q2_launch_cfg *cfg, const char *path);

/* Writes the whole file, creating its directory. */
bool q2_launch_cfg_save(const q2_launch_cfg *cfg, const char *path);

/* `path` was just played: it becomes `last` and moves to the front of the
 * recent list, which drops its oldest entry when full. */
void q2_launch_cfg_use(q2_launch_cfg *cfg, const char *path);

/* Take `path` out of the recent list (and out of `last`). */
void q2_launch_cfg_forget(q2_launch_cfg *cfg, const char *path);

/* Whether two paths name the same disc: case-insensitively and with either
 * slash on Windows, exactly elsewhere. */
bool q2_launch_path_same(const char *a, const char *b);

/*
 * WHY A DISC DID NOT OPEN, for a player rather than a log.
 *
 * Called after disc_open has refused `path`. It looks at the path again from
 * the outside — is anything there, is it a folder, a PC Quake II install, an
 * archive, a format the disc layer does not read, a CUE whose tracks are not
 * beside it — and writes `what` (one short sentence, under the selector status
 * line's 70 characters apart from a file name it quotes) and `todo` (what to
 * do about it). Either buffer may be NULL. Reads only directory entries and the
 * CUE's own text.
 */
typedef enum q2_disc_problem {
    Q2_DISC_PROBLEM_UNREADABLE = 0, /* there, but not a CD image it can read  */
    Q2_DISC_PROBLEM_MISSING,        /* nothing at that path                   */
    Q2_DISC_PROBLEM_DRIVE,          /* a drive letter with no readable disc   */
    Q2_DISC_PROBLEM_FOLDER,         /* a folder (with or without a .cue in it)*/
    Q2_DISC_PROBLEM_PC_GAME,        /* PC Quake II's files, not the PSX disc  */
    Q2_DISC_PROBLEM_ARCHIVE,        /* .zip/.7z/.rar: not extracted yet       */
    Q2_DISC_PROBLEM_FORMAT,         /* .chd/.pbp/.mds/.ccd...: not supported  */
    Q2_DISC_PROBLEM_CUE_TRACK,      /* the CUE names a track that is missing  */
    Q2_DISC_PROBLEM_CUE_EMPTY       /* the CUE names no track at all          */
} q2_disc_problem;

q2_disc_problem q2_launch_disc_problem(const char *path,
                                       char *what, size_t what_size,
                                       char *todo, size_t todo_size);

#endif /* Q2PSX_LAUNCHCFG_H */
