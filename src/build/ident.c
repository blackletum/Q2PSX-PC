#include "ident.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* SLUS-00757 against SLES-01534                                              */
/* ------------------------------------------------------------------------- */
/*
 * The two executables aligned word for word, with every word that can hold an
 * address — a jump target, a `lui`/`%lo` pair, a $gp offset, a pointer in data
 * — masked before comparing. 158,097 of SLES-01534's 158,208 words find a
 * partner, and they fall into these runs. What the gaps are is the NTSC build's
 * actual difference:
 *
 *   0x8001FA30  4 words   FrontEndResetVideoDefaults drops the PAL screen Y of
 *                         24 and stores zero instead — one instruction shorter
 *   0x800701B4  59 words  GetStringFromStringtable, rewritten at 136 words to
 *                         try `<key>US` first; and MOVED, to 0x8006FA28, ahead
 *                         of the code it used to follow. Its entry point is
 *                         the one address inside it that still translates.
 *   0x800718B4  2 words   SeekAndPlay: tenths x5 becomes tenths x6
 *   0x80082620  33 words  MemoryCardUpdate2: NTSC deletes the old save before
 *                         writing the new one (MemCardDeleteFile, 105 words)
 *   0x800ABFA4  13 words  the armour captions, respelt Armor
 *
 * The final run carries on past the image into the BSS and the heap. The NTSC
 * BSS starts 0x270 later and is exactly as long (0x3C048 bytes in both), which
 * is what a relinked image with the same globals looks like.
 *
 * Checked against the NTSC disc's own symbol table (MAIN.SYM, which ships on
 * it): of the 1,104 distinct call targets in SLES-01534's game code, 1,057
 * translate onto the start of a named NTSC function, and the other 47 are
 * private routines inside the Psy-Q libraries, which the symbol table does not
 * name.
 */
static const q2_addr_run k_layout_slus00757[] = {
    { 0x80018000u, 0x8001FA30u,      0x0  },
    { 0x8001FA40u, 0x8006FA2Cu,     -0x4  },
    { 0x8006FA2Cu, 0x800701B4u,   +0x21C  },
    { 0x800701B4u, 0x800701B8u,   -0x78C  },   /* GetStringFromStringtable */
    { 0x800702A0u, 0x800718B4u,   +0x130  },
    { 0x800718BCu, 0x80082620u,   +0x134  },
    { 0x800826A4u, 0x800ABFA4u,   +0x254  },
    { 0x800ABFD8u, 0x800AC2C4u,   +0x250  },
    { 0x800AC2C4u, 0x800AD93Cu,   +0x254  },
    { 0x800AD93Cu, 0x80200000u,   +0x270  },   /* ...and the BSS and heap  */
};

/* ------------------------------------------------------------------------- */
/* Catalogue                                                                  */
/*                                                                            */
/* Entries are added as discs are verified. An entry with an empty hash still  */
/* matches on serial, so a release we know exists but have not fingerprinted   */
/* is recognised by name rather than falling through to "unknown".            */
/* ------------------------------------------------------------------------- */
static const q2_build_desc g_catalogue[] = {
    {
        "Quake II (Europe)",
        "SLES-01534",
        "SLES_015.34",
        /* Verified against a physical-disc dump on 2026-08-13. */
        "9aa15f349308f953062330a593fe3d91204fba301da23368f7727ba92a162e0e",
        634880,
        Q2_REGION_PAL,
        Q2_VIDEO_PAL,
        "English",
        "Activision / Hammerhead. ISO volume dated 1999-09-22.",
        NULL, 0
    },
    {
        "Quake II (USA)",
        "SLUS-00757",
        "SLUS_007.57",
        /* Verified against a dump on 2026-09-11. The catalogue had carried
         * SLUS-00658 here, a serial nobody had checked; the disc says 757. */
        "b07204f7de3864f3d000a1e945573fbc242e65c841f15693942b1a458ced5638",
        636928,
        Q2_REGION_NTSC_U,
        Q2_VIDEO_NTSC,
        "English (American)",
        "Activision / Hammerhead. ISO volume dated 1999-09-23. Ships the "
        "linker's symbol table as MAIN.SYM.",
        k_layout_slus00757,
        (u32)Q2PSX_ARRAY_COUNT(k_layout_slus00757)
    },
};

const q2_build_desc *q2_build_catalogue(int *count)
{
    if (count)
        *count = (int)Q2PSX_ARRAY_COUNT(g_catalogue);
    return g_catalogue;
}

const char *q2_region_str(q2_region r)
{
    switch (r) {
    case Q2_REGION_NTSC_U: return "NTSC-U";
    case Q2_REGION_PAL:    return "PAL";
    case Q2_REGION_NTSC_J: return "NTSC-J";
    case Q2_REGION_UNKNOWN:
    default:               return "unknown";
    }
}

const char *q2_video_std_str(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? "PAL 50Hz" : "NTSC 60Hz";
}

int q2_build_tick_rate(const q2_build_id *id)
{
    if (!id)
        return 60;
    return q2_video_field_hz(id->video);
}

const q2_build_desc *q2_build_by_hash(const char *sha256_hex)
{
    size_t i;

    if (!sha256_hex || !sha256_hex[0])
        return NULL;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(g_catalogue); i++) {
        const q2_build_desc *b = &g_catalogue[i];
        if (b->exe_sha256_hex[0] && strcmp(b->exe_sha256_hex, sha256_hex) == 0)
            return b;
    }
    return NULL;
}

const q2_build_desc *q2_build_by_exe_name(const char *exe_name)
{
    size_t i;

    if (!exe_name || !exe_name[0])
        return NULL;
    for (i = 0; i < Q2PSX_ARRAY_COUNT(g_catalogue); i++) {
        const char *a = g_catalogue[i].exe_name, *b = exe_name;

        while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return &g_catalogue[i];
    }
    return NULL;
}

bool q2_build_has_layout(const q2_build_desc *b)
{
    if (!b)
        return false;
    /* SLES-01534 is the address space; everything else needs its runs. */
    return b->layout_runs > 0 || strcmp(b->serial, "SLES-01534") == 0;
}

/* ------------------------------------------------------------------------- */
/* What the video standard changes                                            */
/* ------------------------------------------------------------------------- */
int q2_video_field_hz(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? 50 : 60;
}

int q2_video_dt_per_field(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? Q2_PAL_DT_PER_FIELD : Q2_NTSC_DT_PER_FIELD;
}

int q2_video_fields_per_tenth(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? 5 : 6;
}

int q2_video_fb_height(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? Q2_PAL_FB_HEIGHT : Q2_NTSC_FB_HEIGHT;
}

int q2_video_mode(q2_video_std v)
{
    return v == Q2_VIDEO_PAL ? 1 : 0;   /* MODE_PAL / MODE_NTSC */
}

int q2_video_centre_shift(q2_video_std v)
{
    return (q2_video_fb_height(v) - Q2_PAL_FB_HEIGHT) / 2;
}

const char *q2_region_string_suffix(q2_region r)
{
    return r == Q2_REGION_NTSC_U ? "US" : "";
}

/* ------------------------------------------------------------------------- */
/* Region inference from the serial prefix                                    */
/* ------------------------------------------------------------------------- */
static q2_region region_from_serial(const char *serial)
{
    if (!serial || strlen(serial) < 4)
        return Q2_REGION_UNKNOWN;

    if (strncmp(serial, "SLUS", 4) == 0 || strncmp(serial, "SCUS", 4) == 0)
        return Q2_REGION_NTSC_U;
    if (strncmp(serial, "SLES", 4) == 0 || strncmp(serial, "SCES", 4) == 0)
        return Q2_REGION_PAL;
    if (strncmp(serial, "SLPS", 4) == 0 || strncmp(serial, "SLPM", 4) == 0 ||
        strncmp(serial, "SCPS", 4) == 0)
        return Q2_REGION_NTSC_J;

    return Q2_REGION_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Data-tree sanity check                                                     */
/* ------------------------------------------------------------------------- */
static void count_level_dirs(const disc *d, q2_build_id *id)
{
    int i, n = disc_file_count(d);
    char seen[128][32];
    int seen_count = 0;

    id->level_dir_count = 0;

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest;
        const char *slash;
        char dir[32];
        size_t len;
        int j;
        bool dup = false;

        if (*p == '/')
            p++;

        /* Want: Q2DATA/LEVELS/<NAME>/... */
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;

        len = (size_t)(slash - rest);
        if (len == 0 || len >= sizeof(dir))
            continue;

        memcpy(dir, rest, len);
        dir[len] = '\0';

        for (j = 0; j < seen_count; j++) {
            if (strcmp(seen[j], dir) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        if (seen_count < (int)Q2PSX_ARRAY_COUNT(seen)) {
            memcpy(seen[seen_count], dir, len + 1);
            seen_count++;
        }
        id->level_dir_count++;
    }

    /* A real disc has dozens of level directories. A handful means we are
     * looking at something else that happens to have a Q2DATA folder. */
    id->data_tree_ok = id->level_dir_count >= 8;
}

/* ------------------------------------------------------------------------- */
/* Identification                                                             */
/* ------------------------------------------------------------------------- */
q2_result q2_identify(const disc *d, q2_build_id *out)
{
    disc_boot_info boot;
    q2_buf exe;
    q2_result r;
    const disc_file *exe_file;
    int i, catalogue_count;

    if (!d || !out)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    q2_str_copy(out->creation_time, sizeof(out->creation_time),
                disc_creation_time(d));
    out->volume_sectors = disc_volume_sectors(d);

    r = disc_read_boot_info(d, &boot);
    if (r != Q2_OK) {
        Q2_WARN("no SYSTEM.CNF on this disc — cannot read the boot executable name");
    } else {
        q2_str_copy(out->serial,   sizeof(out->serial),   boot.serial);
        q2_str_copy(out->exe_name, sizeof(out->exe_name), boot.exe_name);
    }

    out->region = region_from_serial(out->serial);
    out->video  = (out->region == Q2_REGION_PAL) ? Q2_VIDEO_PAL : Q2_VIDEO_NTSC;

    /* Fingerprint the boot executable. */
    exe_file = out->exe_name[0] ? disc_find(d, out->exe_name) : NULL;
    if (exe_file) {
        out->exe_size = exe_file->size;

        r = disc_read_file(d, out->exe_name, &exe);
        if (r == Q2_OK) {
            u8 digest[SHA256_DIGEST_SIZE];
            sha256_buffer(exe.data, exe.size, digest);
            sha256_hex(digest, out->exe_sha256);
            q2_buf_free(&exe);
        } else {
            Q2_WARN("could not read %s to fingerprint it: %s",
                    out->exe_name, q2_result_str(r));
        }
    }

    count_level_dirs(d, out);

    /* Match against the catalogue: hash first, then serial. */
    q2_build_catalogue(&catalogue_count);

    for (i = 0; i < catalogue_count; i++) {
        const q2_build_desc *b = &g_catalogue[i];
        if (b->exe_sha256_hex[0] && out->exe_sha256[0] &&
            strcmp(b->exe_sha256_hex, out->exe_sha256) == 0) {
            out->desc       = b;
            out->catalogued = true;
            break;
        }
    }

    if (!out->desc) {
        for (i = 0; i < catalogue_count; i++) {
            const q2_build_desc *b = &g_catalogue[i];
            if (out->serial[0] && strcmp(b->serial, out->serial) == 0) {
                /* Serial matched but the hash did not — either a revision we
                 * have not catalogued, or a release we know of by name but have
                 * never fingerprinted. Either way this is not an exact match,
                 * and the caller needs to know the data tables may not line up. */
                out->desc       = b;
                out->catalogued = false;
                break;
            }
        }
    }

    if (out->desc) {
        out->region = out->desc->region;
        out->video  = out->desc->video;
    }

    if (!out->data_tree_ok && !out->serial[0])
        return Q2_ERR_BAD_FORMAT;

    return Q2_OK;
}
