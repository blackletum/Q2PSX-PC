/*
 * test_build.c — the release catalogue, the SLES-01534 -> SLUS-00757 address
 * relocation, and what the video standard changes. No disc needed: the
 * relocation is data, and every pair checked here is one the NTSC disc's own
 * symbol table (MAIN.SYM) names on the NTSC side.
 */
#include "exe.h"
#include "ident.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static const q2_build_desc *find(const char *serial)
{
    int i, n = 0;
    const q2_build_desc *c = q2_build_catalogue(&n);

    for (i = 0; i < n; i++)
        if (strcmp(c[i].serial, serial) == 0)
            return &c[i];
    return NULL;
}

/* An executable that is only a build: the translation needs nothing else. */
static q2_exe as_build(const q2_build_desc *b)
{
    q2_exe e;

    memset(&e, 0, sizeof(e));
    e.build = b;
    return e;
}

/* ------------------------------------------------------------------------- */
static void test_catalogue(void)
{
    const q2_build_desc *pal = find("SLES-01534");
    const q2_build_desc *usa = find("SLUS-00757");

    CHECK(pal && usa, "both releases catalogued");
    if (!pal || !usa)
        return;

    CHECK(strcmp(usa->exe_name, "SLUS_007.57") == 0, "USA boot file %s",
          usa->exe_name);
    CHECK(usa->exe_size == 636928, "USA executable size %u", usa->exe_size);
    CHECK(usa->region == Q2_REGION_NTSC_U && usa->video == Q2_VIDEO_NTSC,
          "USA is NTSC-U");

    CHECK(q2_build_by_hash(usa->exe_sha256_hex) == usa, "hash finds USA");
    CHECK(q2_build_by_hash(pal->exe_sha256_hex) == pal, "hash finds PAL");
    CHECK(q2_build_by_hash("00") == NULL, "an unknown hash finds nothing");

    /* A revision or a patched copy is known by its boot file, as the loaders
     * knew it by serial before there was a relocation to apply. */
    CHECK(q2_build_by_exe_name("sles_015.34") == pal, "PAL by boot file");
    CHECK(q2_build_by_exe_name("SLUS_007.57") == usa, "USA by boot file");
    CHECK(q2_build_by_exe_name("SLPS_000.00") == NULL, "an unknown boot file");

    CHECK(q2_build_has_layout(pal), "PAL is the address space");
    CHECK(q2_build_has_layout(usa), "USA has a layout");
    CHECK(!q2_build_has_layout(NULL), "no build, no layout");
}

/* ------------------------------------------------------------------------- */
static void test_relocation(void)
{
    /* SLES-01534 address -> SLUS-00757 address, and the NTSC symbol there. */
    static const struct { u32 pal, usa; const char *name; } k[] = {
        { 0x80019154u, 0x80019154u, "MapPadInputToCommands"    },
        { 0x8003A1C8u, 0x8003A1C4u, "PrimaryQuakePlayer"       },
        { 0x800627F8u, 0x800627F4u, "T_Damage"                 },
        { 0x8005D8C8u, 0x8005D8C4u, "M_CheckAttack"            },
        { 0x8006FE3Cu, 0x80070058u, "Seek"                     },
        { 0x800701B4u, 0x8006FA28u, "GetStringFromStringtable" },
        { 0x800759F0u, 0x80075B24u, "ProcessLensFlares"        },
        { 0x80071718u, 0x80071848u, "SeekAndPlay"              },
        { 0x800B2F78u, 0x800B31E8u, "StringTable (BSS)"        },
        { 0x800AEC4Cu, 0x800AEEBCu, "ScreenYOff"               },
    };
    const q2_build_desc *usa = find("SLUS-00757");
    const q2_build_desc *pal = find("SLES-01534");
    q2_exe u, p, none;
    size_t i;

    if (!usa || !pal)
        return;
    u = as_build(usa);
    p = as_build(pal);
    none = as_build(NULL);

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        CHECK(q2_exe_addr(&u, k[i].pal) == k[i].usa, "%s: %08X -> %08X, want %08X",
              k[i].name, k[i].pal, q2_exe_addr(&u, k[i].pal), k[i].usa);
        CHECK(q2_exe_pal(&u, k[i].usa) == k[i].pal, "%s back: %08X -> %08X",
              k[i].name, k[i].usa, q2_exe_pal(&u, k[i].usa));
        CHECK(q2_exe_addr(&p, k[i].pal) == k[i].pal, "PAL is the identity");
        CHECK(q2_exe_addr(&none, k[i].pal) == 0, "an uncatalogued image maps nothing");
    }

    /* Inside code that changed there is nothing to translate to. */
    CHECK(q2_exe_addr(&u, 0x800701B8u) == 0,
          "the rewritten string lookup's body does not translate");
    CHECK(q2_exe_addr(&u, 0x8001FA30u) == 0,
          "nor does FrontEndResetVideoDefaults' dropped instruction");
    CHECK(q2_exe_addr(&u, 0x80082620u) == 0,
          "nor MemoryCardUpdate2's changed tail");

    /* The item table, read by the port on every level load. */
    CHECK(q2_exe_addr(&u, 0x8009F5CCu) == 0x8009F820u, "item table %08X",
          q2_exe_addr(&u, 0x8009F5CCu));
}

/* ------------------------------------------------------------------------- */
static void test_word_checks(void)
{
    const q2_build_desc *usa = find("SLUS-00757");
    const q2_build_desc *pal = find("SLES-01534");
    q2_exe u, p;

    if (!usa || !pal)
        return;
    u = as_build(usa);
    p = as_build(pal);

    /* `addiu v0, v0, 0x3730`, the %lo of 0x800B3730, is 0x39A0 on NTSC. */
    CHECK(q2_exe_lo_relocates(&u, 0x3730, 0x39A0), "a moved %%lo relocates");
    CHECK(!q2_exe_lo_relocates(&u, 0x3730, 0x3731), "a wrong %%lo does not");
    CHECK(!q2_exe_lo_relocates(&p, 0x3730, 0x39A0), "nothing moves on PAL");

    /* `jal 0x800396AC` (player_die) is `jal 0x800396A8` on NTSC. */
    CHECK(q2_exe_word_relocates(&u, 0x0C00E5ABu, 0x0C00E5AAu), "a moved jal");
    CHECK(!q2_exe_word_relocates(&u, 0x0C00E5ABu, 0x0C00E5ACu), "a wrong jal");
    CHECK(!q2_exe_word_relocates(&u, 0x0C0141D6u, 0),
          "zero padding is not a relocated weapon-walk jal");
    CHECK(!q2_exe_word_relocates(&u, 0x800701B8u, 0),
          "an unmapped pointer is not a null relocation");
    CHECK(q2_exe_word_relocates(&u, 0, 0), "an unchanged zero still matches");
    /* A pointer in data, and an unrelated constant. */
    CHECK(q2_exe_word_relocates(&u, 0x8005CDA8u, 0x8005CDA4u), "a moved pointer");
    CHECK(!q2_exe_word_relocates(&u, 0x00000010u, 0x00000008u),
          "ScreenYOff's 16 -> 8 is a real difference, not a relocation");
}

/* ------------------------------------------------------------------------- */
static void test_video(void)
{
    CHECK(q2_video_field_hz(Q2_VIDEO_PAL) == 50 &&
          q2_video_field_hz(Q2_VIDEO_NTSC) == 60, "field rates");
    CHECK(q2_video_dt_per_field(Q2_VIDEO_PAL) == 6 &&
          q2_video_dt_per_field(Q2_VIDEO_NTSC) == 5, "dt per field");
    /* ...so a second is 300 dt on both. */
    CHECK(q2_video_field_hz(Q2_VIDEO_NTSC) * q2_video_dt_per_field(Q2_VIDEO_NTSC) ==
          q2_video_field_hz(Q2_VIDEO_PAL) * q2_video_dt_per_field(Q2_VIDEO_PAL),
          "the same game clock");
    CHECK(q2_video_fields_per_tenth(Q2_VIDEO_PAL) == 5 &&
          q2_video_fields_per_tenth(Q2_VIDEO_NTSC) == 6, "music tenths");
    CHECK(q2_video_fb_height(Q2_VIDEO_PAL) == 248 &&
          q2_video_fb_height(Q2_VIDEO_NTSC) == 240, "framebuffer heights");
    CHECK(q2_video_mode(Q2_VIDEO_PAL) == 1 && q2_video_mode(Q2_VIDEO_NTSC) == 0,
          "SetVideoMode arguments");
    CHECK(q2_video_centre_shift(Q2_VIDEO_PAL) == 0 &&
          q2_video_centre_shift(Q2_VIDEO_NTSC) == -4, "centre shift");
    CHECK(strcmp(q2_region_string_suffix(Q2_REGION_NTSC_U), "US") == 0 &&
          strcmp(q2_region_string_suffix(Q2_REGION_PAL), "") == 0,
          "string variants");
}

int main(void)
{
    test_catalogue();
    test_relocation();
    test_word_checks();
    test_video();

    if (g_fail) {
        printf("test_build: %d check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("test_build: all checks passed\n");
    return 0;
}
