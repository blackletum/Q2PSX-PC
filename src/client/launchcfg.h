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

#endif /* Q2PSX_LAUNCHCFG_H */
