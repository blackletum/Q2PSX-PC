/*
 * test_launchcfg.c — the launcher's remembered disc, round-tripped.
 *
 * What the player chose last has to come back first next time, the list has
 * to stay a list of distinct discs however often one is played, and a file
 * from somewhere else — missing, or with lines this build does not know — must
 * load as far as it can rather than not at all.
 */
/* rmdir and getpid are POSIX, not C11; disc.c explains the macro. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <process.h>
#  define test_pid()     ((unsigned long)_getpid())
#  define test_rmdir(p)  _rmdir(p)
#  define test_mkdir(p)  _mkdir(p)
#else
#  include <unistd.h>
#  define test_pid()     ((unsigned long)getpid())
#  define test_rmdir(p)  rmdir(p)
#  include <sys/stat.h>
#  define test_mkdir(p)  mkdir((p), 0755)
#endif

#include "launchcfg.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void temp_path(char *out, size_t cap, const char *leaf)
{
    const char *dir = getenv("TEMP");

    if (!dir || !*dir)
        dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = ".";
    snprintf(out, cap, "%s/q2psx_launch_%lu/%s", dir, test_pid(), leaf);
}

static void test_use_orders_and_dedupes(void)
{
    q2_launch_cfg cfg;
    char name[32];
    int i;

    memset(&cfg, 0, sizeof(cfg));
    q2_launch_cfg_use(&cfg, "a.cue");
    q2_launch_cfg_use(&cfg, "b.cue");
    q2_launch_cfg_use(&cfg, "a.cue");
    check(strcmp(cfg.last, "a.cue") == 0, "the disc played last is last");
    check(cfg.recent_count == 2, "playing a disc twice lists it once");
    check(strcmp(cfg.recent[0], "a.cue") == 0 &&
          strcmp(cfg.recent[1], "b.cue") == 0,
          "the recent list is most recent first");

    /* The slot handed back in is moved while it is being read. */
    q2_launch_cfg_use(&cfg, cfg.recent[1]);
    check(strcmp(cfg.recent[0], "b.cue") == 0 && strcmp(cfg.last, "b.cue") == 0,
          "a path taken from the list itself survives being moved");

    for (i = 0; i < Q2_LAUNCH_RECENT_MAX + 3; i++) {
        snprintf(name, sizeof(name), "disc%d.iso", i);
        q2_launch_cfg_use(&cfg, name);
    }
    check(cfg.recent_count == Q2_LAUNCH_RECENT_MAX, "the list is capped");
    snprintf(name, sizeof(name), "disc%d.iso", Q2_LAUNCH_RECENT_MAX + 2);
    check(strcmp(cfg.recent[0], name) == 0, "the newest is kept at the cap");
    check(strcmp(cfg.recent[Q2_LAUNCH_RECENT_MAX - 1], "disc3.iso") == 0,
          "the oldest falls off at the cap");

    q2_launch_cfg_forget(&cfg, name);
    check(cfg.last[0] == '\0', "forgetting the last disc clears it");
    check(cfg.recent_count == Q2_LAUNCH_RECENT_MAX - 1 &&
          strcmp(cfg.recent[0], name) != 0, "and takes it out of the list");
}

static void test_path_sameness(void)
{
#if defined(_WIN32)
    check(q2_launch_path_same("C:\\Games\\Q2.cue", "c:/games/q2.CUE"),
          "Windows paths compare without case or slash direction");
#else
    check(!q2_launch_path_same("/games/Q2.cue", "/games/q2.cue"),
          "POSIX paths compare exactly");
#endif
    check(q2_launch_path_same("E:", "E:"), "a drive is itself");
    check(!q2_launch_path_same("a.cue", "a.cue2"), "a prefix is not a match");
}

static void test_round_trip(void)
{
    q2_launch_cfg cfg, back;
    char path[512];
    FILE *fp;

    temp_path(path, sizeof(path), "sub/launcher.cfg");

    memset(&cfg, 0, sizeof(cfg));
    q2_launch_cfg_use(&cfg, "E:");
    q2_launch_cfg_use(&cfg, "D:/Games/Quake II (Europe)/Quake II (Europe).cue");
    check(q2_launch_cfg_save(&cfg, path), "the file is written, directory and all");
    /* An empty path has no parents to make; the walk used to read past the
     * terminator into the uninitialised rest of its buffer. */
    check(!q2_launch_cfg_save(&cfg, ""), "an empty path is refused cleanly");
    check(q2_launch_cfg_load(&back, path), "and read back");
    check(strcmp(back.last, cfg.last) == 0, "the last disc survives, spaces "
          "and parentheses included");
    check(back.recent_count == 2 && strcmp(back.recent[1], "E:") == 0,
          "the recent list survives in order");

    /* A newer build's keys, a CRLF ending, and a duplicate are all shrugged
     * off rather than failing the load. */
    fp = fopen(path, "wb");
    if (fp) {
        fputs("theme=dark\r\nlast=x.cue\r\nrecent=x.cue\r\nrecent=x.cue\r\n"
              "recent=y.iso\r\n", fp);
        fclose(fp);
    }
    check(q2_launch_cfg_load(&back, path), "a file with unknown keys loads");
    check(strcmp(back.last, "x.cue") == 0, "CRLF endings are stripped");
    check(back.recent_count == 2, "duplicates in the file are dropped");

    remove(path);
    check(!q2_launch_cfg_load(&back, path), "a missing file reports so");
    check(back.last[0] == '\0' && back.recent_count == 0,
          "and leaves an empty configuration");

    /* The two directories the save made. */
    temp_path(path, sizeof(path), "sub");
    test_rmdir(path);
    temp_path(path, sizeof(path), "");
    test_rmdir(path);
}

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");

    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

/*
 * What a player is told when a disc does not open. disc_open's own answer is
 * a bare q2_result, which made a typo, a folder, PC Quake II and a CUE whose
 * BIN had been renamed all print the same `cannot open disc 'X'`.
 */
static void test_disc_problem(void)
{
    char root[512], path[512], what[256], todo[256];
    q2_disc_problem p;

    temp_path(root, sizeof(root), "");
    test_mkdir(root);
    temp_path(root, sizeof(root), "disc");
    test_mkdir(root);

    p = q2_launch_disc_problem(NULL, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_MISSING && what[0] && todo[0],
          "no path at all is a missing disc, with advice");

    temp_path(path, sizeof(path), "disc/nope.cue");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_MISSING, "a path with nothing behind it");
    check(strstr(todo, ".cue") != NULL, "...asks for the .cue");

#if defined(_WIN32)
    p = q2_launch_disc_problem("Q:", what, sizeof(what), todo, sizeof(todo));
#else
    p = q2_launch_disc_problem("/dev/sr9", what, sizeof(what), todo, sizeof(todo));
#endif
    check(p == Q2_DISC_PROBLEM_DRIVE && strstr(todo, "Insert") != NULL,
          "a drive with no disc says to insert one");

    /* A CUE whose track is not beside it names the track. */
    temp_path(path, sizeof(path), "disc/game.cue");
    write_file(path, "\xEF\xBB\xBF" "FILE \"game (Track 1).bin\" BINARY\r\n"
                     "  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_CUE_TRACK, "a CUE with a missing BIN");
    check(strstr(what, "game (Track 1).bin") != NULL, "...names the BIN");

    /* With the track present it is just not a readable image. */
    temp_path(path, sizeof(path), "disc/game (Track 1).bin");
    write_file(path, "not a CD");
    temp_path(path, sizeof(path), "disc/game.cue");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_UNREADABLE, "a complete CUE over junk");

    temp_path(path, sizeof(path), "disc/empty.cue");
    write_file(path, "REM no tracks\n");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_CUE_EMPTY, "a CUE naming no track");

    /* The folder holding them points at a .cue inside it. */
    p = q2_launch_disc_problem(root, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_FOLDER, "a folder is not a disc");
    check(strstr(todo, ".cue") != NULL && strstr(todo, "several") != NULL,
          "...and the advice names a .cue in it");

    temp_path(path, sizeof(path), "disc/x.chd");
    write_file(path, "MComprHD");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_FORMAT && strstr(what, ".chd") != NULL,
          "an unsupported format is named");

    temp_path(path, sizeof(path), "disc/x.zip");
    write_file(path, "PK");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_ARCHIVE && strstr(todo, "Extract") != NULL,
          "an archive is to be extracted first");

    /* PC Quake II, by its folder and by its pak. */
    temp_path(path, sizeof(path), "disc/pc");
    test_mkdir(path);
    temp_path(path, sizeof(path), "disc/pc/baseq2");
    test_mkdir(path);
    temp_path(path, sizeof(path), "disc/pc/baseq2/pak0.pak");
    write_file(path, "PACK");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_PC_GAME, "pak0.pak is PC Quake II");
    temp_path(path, sizeof(path), "disc/pc");
    p = q2_launch_disc_problem(path, what, sizeof(what), todo, sizeof(todo));
    check(p == Q2_DISC_PROBLEM_PC_GAME && strstr(todo, "SLES-01534") != NULL,
          "...and so is its install folder, with the serials to look for");
    p = q2_launch_disc_problem(path, NULL, 0, NULL, 0);
    check(p == Q2_DISC_PROBLEM_PC_GAME, "the buffers are optional");

    {
        static const char *const leaves[] = {
            "disc/pc/baseq2/pak0.pak", "disc/pc/baseq2", "disc/pc",
            "disc/x.zip", "disc/x.chd", "disc/empty.cue",
            "disc/game (Track 1).bin", "disc/game.cue", "disc", ""
        };
        size_t i;

        for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
            temp_path(path, sizeof(path), leaves[i]);
            if (remove(path) != 0)
                test_rmdir(path);
        }
    }
}

int main(void)
{
    printf("launcher configuration tests\n");
    test_use_orders_and_dedupes();
    test_path_sameness();
    test_round_trip();
    test_disc_problem();

    if (g_failures) {
        printf("%d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("all %d checks passed\n", g_checks);
    return 0;
}
