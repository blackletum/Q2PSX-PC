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

#include <sys/types.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  define q2_mkdir(p) _mkdir(p)
#else
#  include <dirent.h>
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
    /* An empty path has nothing above it, and the walk starts at [1]. */
    if (!dir[0])
        return;
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

/* ------------------------------------------------------------------------- */
/* Why a disc did not open                                                    */
/* ------------------------------------------------------------------------- */
/*
 * disc_open answers with a q2_result, and "not found" or "bad format" is all a
 * player used to be told: `cannot open disc 'X'` for a typo, for a folder, for
 * PC Quake II's pak0.pak and for a CUE whose BIN had been renamed alike. Each
 * of those has a different fix, and every one of them is visible from outside
 * the disc layer, so they are told apart here without touching it.
 */
enum { KIND_NONE, KIND_FILE, KIND_DIR };

static int path_kind(const char *path)
{
#if defined(_WIN32)
    struct _stat st;

    if (!path || !*path || _stat(path, &st) != 0)
        return KIND_NONE;
    return (st.st_mode & _S_IFDIR) ? KIND_DIR : KIND_FILE;
#else
    struct stat st;

    if (!path || !*path || stat(path, &st) != 0)
        return KIND_NONE;
    return S_ISDIR(st.st_mode) ? KIND_DIR : KIND_FILE;
#endif
}

static const char *leaf_of(const char *path)
{
    const char *leaf = path;
    const char *p;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            leaf = p + 1;
    return leaf;
}

/* The extension of the final component, without its dot; "" without one. */
static const char *ext_of(const char *path)
{
    const char *dot = strrchr(leaf_of(path), '.');

    return dot ? dot + 1 : "";
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

static void join_path(char *out, size_t size, const char *dir, const char *leaf)
{
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');

    snprintf(out, size, "%s%s%s", dir, sep ? "" : "/", leaf);
}

/* "D:" or "D:\" on Windows, a /dev node elsewhere: what cddrive.h takes. */
static bool drive_path(const char *path)
{
#if defined(_WIN32)
    return isalpha((unsigned char)path[0]) && path[1] == ':' &&
           (!path[2] || ((path[2] == '\\' || path[2] == '/') && !path[3]));
#else
    return strncmp(path, "/dev/", 5) == 0;
#endif
}

/* Retail PC Quake II: baseq2\pak0.pak, or the executable, or the pak itself. */
static bool pc_quake2_folder(const char *dir)
{
    static const char *const marks[] = {
        "baseq2", "pak0.pak", "quake2.exe", "baseq2/pak0.pak"
    };
    char probe[Q2_LAUNCH_PATH_MAX];
    size_t i;

    for (i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        join_path(probe, sizeof(probe), dir, marks[i]);
        if (path_kind(probe) != KIND_NONE)
            return true;
    }
    return false;
}

/* How many .cue files `dir` holds, and the name of the first. */
static int cues_in(const char *dir, char *first, size_t size)
{
    int count = 0;
#if defined(_WIN32)
    char pattern[Q2_LAUNCH_PATH_MAX];
    struct _finddata_t found;
    intptr_t h;

    join_path(pattern, sizeof(pattern), dir, "*.cue");
    h = _findfirst(pattern, &found);
    if (h == -1)
        return 0;
    do {
        if (!(found.attrib & _A_SUBDIR) && same_ci(ext_of(found.name), "cue")) {
            if (!count++ && first)
                snprintf(first, size, "%s", found.name);
        }
    } while (_findnext(h, &found) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (same_ci(ext_of(e->d_name), "cue")) {
            if (!count++ && first)
                snprintf(first, size, "%s", e->d_name);
        }
    }
    closedir(d);
#endif
    return count;
}

/*
 * The CUE's FILE lines, resolved as disc.c resolves them (a quoted name,
 * beside the sheet unless it is absolute). The first that is not there is
 * named in `missing`. Returns 0 when every track is present, 1 for a missing
 * one, 2 for a sheet naming no track, -1 for a sheet that cannot be read.
 */
static int cue_tracks(const char *cue, char *missing, size_t size)
{
    char line[Q2_LAUNCH_PATH_MAX + 64];
    int files = 0;
    FILE *fp = fopen(cue, "rb");

    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        char name[Q2_LAUNCH_PATH_MAX];
        char resolved[Q2_LAUNCH_PATH_MAX];
        char *p = line, *q1, *q2;

        while (*p == ' ' || *p == '\t' || (unsigned char)*p == 0xEF ||
               (unsigned char)*p == 0xBB || (unsigned char)*p == 0xBF)
            p++;                                  /* indentation, a UTF-8 BOM */
        if (!(tolower((unsigned char)p[0]) == 'f' &&
              tolower((unsigned char)p[1]) == 'i' &&
              tolower((unsigned char)p[2]) == 'l' &&
              tolower((unsigned char)p[3]) == 'e'))
            continue;
        q1 = strchr(p, '"');
        q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2)
            continue;
        files++;
        snprintf(name, sizeof(name), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
        if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':')) {
            snprintf(resolved, sizeof(resolved), "%s", name);
        } else {
            char dir[Q2_LAUNCH_PATH_MAX];

            snprintf(dir, sizeof(dir), "%s", cue);
            dir[leaf_of(dir) - dir] = '\0';
            snprintf(resolved, sizeof(resolved), "%s%s", dir, name);
        }
        if (path_kind(resolved) != KIND_FILE) {
            fclose(fp);
            if (missing)
                snprintf(missing, size, "%s", leaf_of(name));
            return 1;
        }
    }
    fclose(fp);
    return files ? 0 : 2;
}

static void say(char *out, size_t size, const char *text)
{
    if (out && size)
        snprintf(out, size, "%s", text);
}

#define Q2_PSX_DISCS "Europe SLES-01534 or USA SLUS-00757"

q2_disc_problem q2_launch_disc_problem(const char *path,
                                       char *what, size_t what_size,
                                       char *todo, size_t todo_size)
{
    const char *ext;
    int kind;

    say(what, what_size, "");
    say(todo, todo_size, "");
    if (!path || !*path) {
        say(what, what_size, "No disc was given.");
        say(todo, todo_size, "Give the .cue file of your Quake II PlayStation "
                             "disc image, or a CD drive.");
        return Q2_DISC_PROBLEM_MISSING;
    }

    if (drive_path(path)) {
        say(what, what_size, "No readable disc in that drive, or it is not a CD drive.");
        say(todo, todo_size, "Insert the Quake II PlayStation disc and try "
                             "again, or give a disc image instead.");
        return Q2_DISC_PROBLEM_DRIVE;
    }

    kind = path_kind(path);
    ext  = ext_of(path);
    if (kind == KIND_NONE) {
        say(what, what_size, "Nothing is there: no file or folder has that name.");
        say(todo, todo_size, "Check the spelling, and quote a path that has "
                             "spaces in it. Give the .cue file of your disc image.");
        return Q2_DISC_PROBLEM_MISSING;
    }

    if (kind == KIND_DIR) {
        char cue[256];
        int cues;

        if (pc_quake2_folder(path)) {
            say(what, what_size, "That is PC Quake II, not the PlayStation disc.");
            say(todo, todo_size, "This game reads the PlayStation release ("
                                 Q2_PSX_DISCS "), as a CUE/BIN image or in a "
                                 "CD drive.");
            return Q2_DISC_PROBLEM_PC_GAME;
        }
        cues = cues_in(path, cue, sizeof(cue));
        say(what, what_size, "That is a folder, not a disc image.");
        if (cues > 0 && todo) {
            snprintf(todo, todo_size, "Give the .cue file inside it%s: %s",
                     cues > 1 ? " (it holds several)" : "", cue);
        } else {
            say(todo, todo_size, "Give a .cue, .bin, .img or .iso file, or a "
                                 "CD drive.");
        }
        return Q2_DISC_PROBLEM_FOLDER;
    }

    if (same_ci(ext, "pak") || same_ci(leaf_of(path), "quake2.exe")) {
        say(what, what_size, "That is PC Quake II, not the PlayStation disc.");
        say(todo, todo_size, "This game reads the PlayStation release ("
                             Q2_PSX_DISCS "), as a CUE/BIN image or in a CD drive.");
        return Q2_DISC_PROBLEM_PC_GAME;
    }

    if (same_ci(ext, "zip") || same_ci(ext, "7z") || same_ci(ext, "rar") ||
        same_ci(ext, "gz")  || same_ci(ext, "xz")) {
        say(what, what_size, "That is an archive, not a disc image.");
        say(todo, todo_size, "Extract it first, then give the .cue file inside it.");
        return Q2_DISC_PROBLEM_ARCHIVE;
    }

    if (same_ci(ext, "ccd") || same_ci(ext, "sub")) {
        say(what, what_size, "CloneCD descriptors are not read.");
        say(todo, todo_size, "Give the .img file beside it instead.");
        return Q2_DISC_PROBLEM_FORMAT;
    }
    if (same_ci(ext, "chd") || same_ci(ext, "pbp") || same_ci(ext, "mds") ||
        same_ci(ext, "mdf") || same_ci(ext, "ecm") || same_ci(ext, "nrg") ||
        same_ci(ext, "cdi")) {
        if (what)
            snprintf(what, what_size, ".%s images are not supported.", ext);
        say(todo, todo_size, "Convert the disc to CUE/BIN first (raw, 2352-byte "
                             "sectors keep the music and films).");
        return Q2_DISC_PROBLEM_FORMAT;
    }

    if (same_ci(ext, "cue")) {
        char missing[256];
        int r = cue_tracks(path, missing, sizeof(missing));

        if (r == 1) {
            if (what)
                snprintf(what, what_size, "Missing track file: %s", missing);
            say(todo, todo_size, "The .cue lists it, but it is not beside the "
                                 ".cue. Keep the .cue and every .bin it lists in "
                                 "one folder, with their original names.");
            return Q2_DISC_PROBLEM_CUE_TRACK;
        }
        if (r == 2) {
            say(what, what_size, "That .cue names no track file.");
            say(todo, todo_size, "It may be damaged: re-create it, or give the "
                                 "data track's .bin directly.");
            return Q2_DISC_PROBLEM_CUE_EMPTY;
        }
        if (r < 0) {
            say(what, what_size, "That .cue cannot be read.");
            say(todo, todo_size, "Check that your account may read the file.");
            return Q2_DISC_PROBLEM_UNREADABLE;
        }
    }

    say(what, what_size, "That is not a CD image this game can read.");
    say(todo, todo_size, "Use a CUE/BIN, ISO or IMG copy of your Quake II "
                         "PlayStation disc (" Q2_PSX_DISCS ").");
    return Q2_DISC_PROBLEM_UNREADABLE;
}
