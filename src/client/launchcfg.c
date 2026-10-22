/*
 * launchcfg.c — the launcher's remembered disc. See launchcfg.h.
 */
/* mkdir is POSIX, not C11; disc.c explains the macro. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "launchcfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define q2_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define q2_mkdir(p) mkdir((p), 0755)
#endif

static void copy_path(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;

    if (n >= Q2_LAUNCH_PATH_MAX)
        n = Q2_LAUNCH_PATH_MAX - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

const char *q2_launch_cfg_default_path(void)
{
    static char path[Q2_LAUNCH_PATH_MAX];
    const char *env;

    if (path[0])
        return path;

#if defined(_WIN32)
    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(path, sizeof(path), "%s\\Q2PSX-PC\\launcher.cfg", env);
        return path;
    }
#else
    env = getenv("XDG_CONFIG_HOME");
    if (env && *env) {
        snprintf(path, sizeof(path), "%s/q2psx-pc/launcher.cfg", env);
        return path;
    }
    env = getenv("HOME");
    if (env && *env) {
        snprintf(path, sizeof(path), "%s/.config/q2psx-pc/launcher.cfg", env);
        return path;
    }
#endif
    snprintf(path, sizeof(path), "%s", "launcher.cfg");
    return path;
}

bool q2_launch_path_same(const char *a, const char *b)
{
    if (!a || !b)
        return false;
#if defined(_WIN32)
    for (;; a++, b++) {
        int ca = (*a == '/') ? '\\' : tolower((unsigned char)*a);
        int cb = (*b == '/') ? '\\' : tolower((unsigned char)*b);

        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
#else
    return strcmp(a, b) == 0;
#endif
}

static void strip_line(char *s)
{
    size_t n = strlen(s);

    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

bool q2_launch_cfg_load(q2_launch_cfg *cfg, const char *path)
{
    char line[Q2_LAUNCH_PATH_MAX + 32];
    FILE *fp;

    if (!cfg)
        return false;
    memset(cfg, 0, sizeof(*cfg));
    if (!path)
        return false;

    fp = fopen(path, "rb");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp)) {
        strip_line(line);
        if (!strncmp(line, "last=", 5)) {
            copy_path(cfg->last, line + 5);
        } else if (!strncmp(line, "recent=", 7) && line[7] &&
                   cfg->recent_count < Q2_LAUNCH_RECENT_MAX) {
            int i;
            bool dup = false;

            for (i = 0; i < cfg->recent_count; i++)
                dup |= q2_launch_path_same(cfg->recent[i], line + 7);
            if (!dup)
                copy_path(cfg->recent[cfg->recent_count++], line + 7);
        }
    }
    fclose(fp);
    return true;
}

/* Every directory above `path`'s file name, the way save.c's writer does it:
 * only on the way to a write, so a run that never chooses leaves nothing. */
static void make_parents(const char *path)
{
    char dir[Q2_LAUNCH_PATH_MAX];
    size_t i;

    copy_path(dir, path);
    for (i = 1; dir[i]; i++) {
        if (dir[i] == '/' || dir[i] == '\\') {
            char c = dir[i];

            /* A drive root ("C:\") already exists. */
            if (i == 2 && dir[1] == ':')
                continue;
            dir[i] = '\0';
            q2_mkdir(dir);
            dir[i] = c;
        }
    }
}

bool q2_launch_cfg_save(const q2_launch_cfg *cfg, const char *path)
{
    FILE *fp;
    int i;

    if (!cfg || !path)
        return false;

    make_parents(path);
    fp = fopen(path, "wb");
    if (!fp)
        return false;

    fprintf(fp, "# Q2PSX-PC launcher: the disc played last, and the ones "
                "before it.\n");
    if (cfg->last[0])
        fprintf(fp, "last=%s\n", cfg->last);
    for (i = 0; i < cfg->recent_count; i++)
        fprintf(fp, "recent=%s\n", cfg->recent[i]);

    return fclose(fp) == 0;
}

void q2_launch_cfg_forget(q2_launch_cfg *cfg, const char *path)
{
    int i, out = 0;

    if (!cfg || !path)
        return;
    for (i = 0; i < cfg->recent_count; i++) {
        if (q2_launch_path_same(cfg->recent[i], path))
            continue;
        if (out != i)
            memcpy(cfg->recent[out], cfg->recent[i], Q2_LAUNCH_PATH_MAX);
        out++;
    }
    cfg->recent_count = out;
    if (q2_launch_path_same(cfg->last, path))
        cfg->last[0] = '\0';
}

void q2_launch_cfg_use(q2_launch_cfg *cfg, const char *path)
{
    char keep[Q2_LAUNCH_PATH_MAX];
    int i;

    if (!cfg || !path || !path[0])
        return;

    copy_path(keep, path);   /* `path` may be one of the slots moved below */
    q2_launch_cfg_forget(cfg, keep);

    if (cfg->recent_count == Q2_LAUNCH_RECENT_MAX)
        cfg->recent_count--;
    for (i = cfg->recent_count; i > 0; i--)
        memcpy(cfg->recent[i], cfg->recent[i - 1], Q2_LAUNCH_PATH_MAX);
    copy_path(cfg->recent[0], keep);
    cfg->recent_count++;
    copy_path(cfg->last, keep);
}
