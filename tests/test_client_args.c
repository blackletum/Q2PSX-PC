/*
 * test_client_args.c -- what the client says when it is started wrongly.
 *
 * The real `main`, called with the command lines a player or a script gets
 * wrong. Each must end with a non-zero exit and ONE message naming the flag
 * and what it wanted -- not a silent default (`--frames abc` used to run one
 * frame and exit 0), not the whole usage with nothing pointing at the mistake,
 * and not a disc that is "uncatalogued" when it is a different game.
 *
 *   test_client_args                      the command-line checks (no disc)
 *   test_client_args --headless <cue>     ...and the ones that need the disc
 */
int q2_test_args_client_main(int argc, char **argv);
#define main q2_test_args_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
#include "other_disc_fixture.h"

/* Run the client on `args` (NULL-terminated) and return its exit code. */
static int run(const char *const *args)
{
    static char store[32][512];
    char *argv[32];
    int argc = 0;

    snprintf(store[0], sizeof(store[0]), "q2psx");
    argv[argc] = store[argc];
    argc++;
    while (*args && argc < 31) {
        snprintf(store[argc], sizeof(store[argc]), "%s", *args++);
        argv[argc] = store[argc];
        argc++;
    }
    argv[argc] = NULL;
    q2_client_failure[0] = '\0';
    return q2_test_args_client_main(argc, argv);
}

/* Exit `code`, and the failure text holds `needle`. */
static void expect(int code, const char *needle, const char *const *args)
{
    int got = run(args);

    CHECK(got == code);
    CHECK(strstr(q2_client_failure, needle) != NULL);
    if (got != code || !strstr(q2_client_failure, needle))
        fprintf(stderr, "  %s: exit %d, said \"%s\"\n", args[0], got,
                q2_client_failure);
}

#define ARGS(...) ((const char *const[]){ __VA_ARGS__, NULL })

static void test_flags(void)
{
    /* Unknown, and known but with its value left off the end. */
    expect(2, "unknown option '--bogus'", ARGS("--bogus"));
    expect(2, "--frames needs a value", ARGS("--headless", "--frames"));
    expect(2, "--screen-pos needs a value", ARGS("--screen-pos", "5"));

    /* A script that forgot the disc gets the reason, not the usage. */
    expect(2, "--headless needs --disc", ARGS("--headless"));
    expect(2, "needs --shot", ARGS("--launcher", "--headless"));

    /* Numbers are whole or refused, and in range. */
    expect(2, "--frames wants a whole number", ARGS("--headless", "--frames", "abc"));
    expect(2, "not '12x'", ARGS("--frames", "12x"));
    expect(2, "--zone wants a whole number from 0 to 99", ARGS("--zone", "x"));
    expect(2, "--scale wants a whole number from 1 to 8", ARGS("--scale", "0"));
    expect(2, "--weapon wants a whole number from 0 to 11", ARGS("--weapon", "42"));
    expect(2, "--dm-players wants a whole number from 2 to 4", ARGS("--dm-players", "9"));
    expect(2, "--dm-mode wants a whole number from 0 to 5", ARGS("--dm-mode", "7"));
    expect(2, "--yaw wants", ARGS("--yaw", "abc"));
    expect(2, "--trace-cre wants", ARGS("--trace-cre", "q"));
    expect(2, "--dm-rounds wants a whole number from 1", ARGS("--dm-rounds", "0"));
    expect(2, "--screen-pos Y wants a whole number from -32 to 64",
           ARGS("--screen-pos", "5", "99"));
    expect(2, "--at wants X,Y,Z", ARGS("--at", "1,2"));
    expect(2, "not '1,2,3x'", ARGS("--at", "1,2,3x"));
    expect(2, "--size wants WxH", ARGS("--size", "640"));

    /* Words, naming what was given. */
    expect(2, "--aspect wants auto|4:3|tv|square|stretch, not 'wide'",
           ARGS("--aspect", "wide"));
    expect(2, "not 'diagonal'", ARGS("--dm-split", "diagonal"));
    expect(2, "not 'mega'", ARGS("--powerup", "mega"));
    expect(2, "not 'tin'", ARGS("--armour", "tin"));

    /* A bare path is the disc (a file dropped on the executable); two are a
     * mistake. */
    expect(2, "two discs given", ARGS("a.cue", "b.cue", "--headless"));

    /* --help and --version still answer without a disc. */
    CHECK(run(ARGS("--help")) == 0);
    CHECK(run(ARGS("--version")) == 0);
}

/* Discs that are not Quake II, or not there at all. */
static void test_discs(void)
{
    char path[512];
    const char *tmp = getenv("TEMP");
    FILE *fp;

    if (!tmp || !*tmp) tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = ".";

    snprintf(path, sizeof(path), "%s/q2psx_args_missing_%u.cue", tmp, 7u);
    expect(1, "Nothing is there", ARGS("--disc", path, "--headless"));

    /* A CUE whose BIN is not beside it names the BIN. */
    fp = fopen(path, "wb");
    if (fp) {
        fputs("FILE \"Quake II (Europe) (Track 1).bin\" BINARY\r\n"
              "  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n", fp);
        fclose(fp);
    }
    expect(1, "Missing track file: Quake II (Europe) (Track 1).bin",
           ARGS("--disc", path, "--headless"));
    remove(path);

    /*
     * ANOTHER PLAYSTATION GAME. An ISO 9660 volume holding only a SYSTEM.CNF
     * that boots SLUS_999.99: q2_identify accepts it (it has a serial), and the
     * client used to start it as an "uncatalogued build" and fail to load
     * QFRONT. It has no Q2DATA, so it is refused by name.
     */
    snprintf(path, sizeof(path), "%s/q2psx_args_other_%u.iso", tmp, 7u);
    CHECK(write_other_psx_disc(path));
    expect(1, "is not Quake II for the PlayStation (it is SLUS-99999)",
           ARGS("--disc", path, "--headless", "--frames", "1"));
    remove(path);
}

/* The two --map mistakes, told apart. Needs the real disc. */
static void test_map(const char *disc)
{
    expect(1, "this disc has no level 'NOSUCH'",
           ARGS("--disc", disc, "--headless", "--frames", "1", "--map", "NOSUCH"));
    expect(1, "BASE1 has no zone 99: its zones are 0 to",
           ARGS("--disc", disc, "--headless", "--frames", "1",
                "--map", "BASE1", "--zone", "99"));
}

int main(int argc, char **argv)
{
    test_flags();
    test_discs();
    if (argc >= 3 && !strcmp(argv[1], "--headless"))
        test_map(argv[2]);
    else if (argc != 1)
        return 2;
    printf("%d argument checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
