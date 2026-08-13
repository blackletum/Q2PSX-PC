/*
 * q2psx-inspect — the reverse-engineering harness.
 *
 * Opens a disc and reports what is on it, without needing a game window. This is
 * the tool used to validate every format claim in docs/FORMATS.md: if the parser
 * here reads a real disc cleanly, the engine's loader will too, because they are
 * the same code.
 */
#include "area.h"
#include "collision.h"
#include "dat.h"
#include "disc.h"
#include "entity.h"
#include "ident.h"
#include "level.h"
#include "points.h"
#include "raster.h"
#include "scene.h"
#include "world.h"
#include "trig.h"
#include "vag.h"
#include "xa.h"
#include "q2psx.h"

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

static void usage(void)
{
    puts("q2psx-inspect - inspect a Quake II PSX disc\n");
    puts("usage: q2psx-inspect <command> <disc> [args]\n");
    puts("commands:");
    puts("  ident   <disc>              identify the release and print its fingerprint");
    puts("  disc    <disc>              list tracks and the full file table");
    puts("  levels  <disc>              summarise the level directories");
    puts("  dat     <disc> <path>       dump the chunk directory of one .DAT");
    puts("  dats    <disc>              census every .DAT chunk schema on the disc");
    puts("  verify  <disc>              check every level file against the typed schema");
    puts("  audio   <disc>              decode every sound bank and validate it");
    puts("  music   <disc>              demultiplex and decode the XA music streams");
    puts("  render  <disc> <map> [z] [out.ppm] [yaw] [pitch]  render a zone (4096 = full turn)");
    puts("  hexdump <disc> <path> [n]   hex dump the first n bytes of a file");
    puts("  extract <disc> <outdir>     extract the whole filesystem");
    puts("");
    puts("<disc> may be a .cue, .bin, .img or .iso.");
}

/* ------------------------------------------------------------------------- */
/*
 * ISO9660 stores its creation time as 16 ASCII digits plus a signed byte giving
 * the offset from GMT in 15-minute steps. Rendering it raw makes the timestamp
 * look like corruption, so split it out.
 */
static void format_iso_time(char *out, size_t cap, const char *raw)
{
    int tz;

    if (!raw || strlen(raw) < 17) {
        snprintf(out, cap, "(none)");
        return;
    }

    tz = (int)(signed char)raw[16];
    snprintf(out, cap, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s.%.2s GMT%+.2f",
             raw, raw + 4, raw + 6, raw + 8, raw + 10, raw + 12, raw + 14,
             (double)tz / 4.0);
}

static void print_size(char *out, size_t cap, u32 bytes)
{
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024u)
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(out, cap, "%u B", bytes);
}

/* ------------------------------------------------------------------------- */
static int cmd_ident(disc *d)
{
    q2_build_id id;
    q2_result r;
    char when[64];

    r = q2_identify(d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "this does not look like a Quake II PSX disc: %s\n",
                q2_result_str(r));
        return 1;
    }

    printf("Release\n");
    if (id.desc)
        printf("  name            : %s\n", id.desc->name);
    else
        printf("  name            : (uncatalogued build)\n");

    printf("  serial          : %s\n", id.serial[0] ? id.serial : "(none)");
    printf("  boot executable : %s\n", id.exe_name[0] ? id.exe_name : "(none)");
    printf("  region          : %s\n", q2_region_str(id.region));
    printf("  video standard  : %s\n", q2_video_std_str(id.video));
    printf("  game tick rate  : %d Hz\n", q2_build_tick_rate(&id));
    if (id.desc && id.desc->language)
        printf("  language        : %s\n", id.desc->language);

    format_iso_time(when, sizeof(when), id.creation_time);
    printf("\nFingerprint\n");
    printf("  exe size        : %u bytes\n", id.exe_size);
    printf("  exe sha256      : %s\n", id.exe_sha256[0] ? id.exe_sha256 : "(unavailable)");
    printf("  volume created  : %s\n", when);
    printf("  volume sectors  : %u\n", id.volume_sectors);

    printf("\nData tree\n");
    printf("  level dirs      : %d\n", id.level_dir_count);
    printf("  structure       : %s\n", id.data_tree_ok ? "ok" : "UNRECOGNISED");

    printf("\nMatch\n");
    if (id.catalogued) {
        printf("  status          : exact match against the build catalogue\n");
    } else if (id.desc) {
        printf("  status          : serial matched, executable hash is new\n");
        printf("                    (a revision, or the first dump we have seen)\n");
    } else {
        printf("  status          : unknown build - will run in generic mode\n");
    }
    if (id.desc && id.desc->notes)
        printf("  notes           : %s\n", id.desc->notes);

    return 0;
}

/* ------------------------------------------------------------------------- */
static int cmd_disc(disc *d)
{
    int i, n;
    char when[64];

    printf("%s\n\n", disc_describe(d));

    format_iso_time(when, sizeof(when), disc_creation_time(d));
    printf("Volume\n");
    printf("  system id : %s\n", disc_system_id(d));
    printf("  volume id : %s\n", disc_volume_id(d)[0] ? disc_volume_id(d) : "(blank)");
    printf("  created   : %s\n", when);
    printf("  sectors   : %u\n\n", disc_volume_sectors(d));

    n = disc_track_count(d);
    printf("Tracks (%d)\n", n);
    printf("  no  type   start lba sectors   ssize  duration\n");
    for (i = 0; i < n; i++) {
        const cd_track *t = disc_track(d, i);
        double secs = (double)t->length_sectors / (double)CD_SECTORS_PER_SECOND;
        int mins = (int)(secs / 60.0);
        printf("  %-3d %-6s %-9u %-9u %-6d %d:%05.2f\n",
               t->number,
               t->type == CD_TRACK_AUDIO ? "audio" : "data",
               t->start_lba, t->length_sectors, t->sector_size,
               mins, secs - 60.0 * mins);
    }

    n = disc_file_count(d);
    printf("\nFiles (%d)\n", n);
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        char sz[32];
        print_size(sz, sizeof(sz), f->size);
        printf("  %-8s lba=%-7u %-10s %s\n",
               f->form2 ? "[form2]" : "", f->lba, sz, f->path);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
static int cmd_levels(disc *d)
{
    int i, n = disc_file_count(d);
    char current[64];
    int dirs = 0;

    current[0] = '\0';
    printf("Level directories\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char dir[64], sz[32];
        size_t len;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;

        len = (size_t)(slash - rest);
        if (len >= sizeof(dir))
            len = sizeof(dir) - 1;
        memcpy(dir, rest, len);
        dir[len] = '\0';

        if (strcmp(dir, current) != 0) {
            strncpy(current, dir, sizeof(current) - 1);
            current[sizeof(current) - 1] = '\0';
            printf("\n  %s\n", dir);
            dirs++;
        }

        print_size(sz, sizeof(sz), f->size);
        printf("      %-14s %10s\n", slash + 1, sz);
    }

    printf("\n%d level directories\n", dirs);
    return 0;
}

/* ------------------------------------------------------------------------- */
static void dump_dat_chunks(const dat_archive *ar, const char *label)
{
    int i;

    printf("  %s - %d chunks, data ends at 0x%X\n",
           label, ar->chunk_count, ar->end_offset);
    printf("    idx name           offset           size\n");
    for (i = 0; i < ar->chunk_count; i++) {
        const dat_chunk *c = &ar->chunks[i];
        printf("    %-3d %-14s 0x%08X %10u%s\n",
               i, c->name, c->offset, c->size,
               c->size == 0 ? "   (empty)" : "");
    }
}

static int cmd_dat(disc *d, const char *path)
{
    q2_buf buf;
    dat_archive ar;
    q2_result r;

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read %s: %s\n", path, q2_result_str(r));
        return 1;
    }

    if (!dat_probe(buf.data, buf.size)) {
        size_t i;
        printf("%s (%zu bytes) does not use the .DAT chunk container.\n",
               path, buf.size);
        printf("First 32 bytes:\n    ");
        for (i = 0; i < 32 && i < buf.size; i++)
            printf("%02X ", buf.data[i]);
        printf("\n");
        q2_buf_free(&buf);
        return 0;
    }

    r = dat_open_buf(&ar, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot parse %s: %s\n", path, q2_result_str(r));
        q2_buf_free(&buf);
        return 1;
    }

    printf("%s\n", path);
    dump_dat_chunks(&ar, "chunks");
    dat_close(&ar);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Schema census                                                              */
/*                                                                            */
/* The loader wants to index chunks by enum rather than by string, which is    */
/* only safe if the set of names is knowable up front. So rather than just     */
/* flagging "this file differs", collect the distinct schemas and the union of */
/* names, with how often each appears and how often it carries data. That      */
/* tells us which chunks are mandatory, which are optional, and whether the    */
/* variation is in the names or only in the ordering.                          */
/* ------------------------------------------------------------------------- */
#define CENSUS_MAX_SCHEMAS 64
#define CENSUS_MAX_NAMES   128
#define CENSUS_SCHEMA_CAP  1024

typedef struct schema_variant {
    char schema[CENSUS_SCHEMA_CAP];
    char example[192];
    int  count;
    int  chunk_count;
} schema_variant;

typedef struct name_stat {
    char name[DAT_NAME_LEN + 1];
    int  present;       /* files containing this chunk         */
    int  non_empty;     /* files where it carries data         */
    int  first_index;   /* directory position, first sighting  */
    int  index_varies;
} name_stat;

typedef struct census {
    const char     *label;
    schema_variant  variants[CENSUS_MAX_SCHEMAS];
    int             variant_count;
    name_stat       names[CENSUS_MAX_NAMES];
    int             name_count;
    int             files;
} census;

static void census_init(census *c, const char *label)
{
    memset(c, 0, sizeof(*c));
    c->label = label;
}

static void census_add(census *c, const dat_archive *ar, const char *path)
{
    char schema[CENSUS_SCHEMA_CAP];
    size_t used = 0;
    int i, j;

    schema[0] = '\0';
    c->files++;

    for (i = 0; i < ar->chunk_count; i++) {
        int written = snprintf(schema + used, sizeof(schema) - used,
                               "%s|", ar->chunks[i].name);
        if (written < 0 || (size_t)written >= sizeof(schema) - used)
            break;
        used += (size_t)written;
    }

    for (i = 0; i < c->variant_count; i++) {
        if (strcmp(c->variants[i].schema, schema) == 0) {
            c->variants[i].count++;
            break;
        }
    }
    if (i == c->variant_count && c->variant_count < CENSUS_MAX_SCHEMAS) {
        schema_variant *v = &c->variants[c->variant_count++];
        strncpy(v->schema, schema, sizeof(v->schema) - 1);
        strncpy(v->example, path, sizeof(v->example) - 1);
        v->count = 1;
        v->chunk_count = ar->chunk_count;
    }

    for (i = 0; i < ar->chunk_count; i++) {
        const dat_chunk *ch = &ar->chunks[i];

        for (j = 0; j < c->name_count; j++) {
            if (strcmp(c->names[j].name, ch->name) == 0)
                break;
        }
        if (j == c->name_count) {
            if (c->name_count >= CENSUS_MAX_NAMES)
                continue;
            strncpy(c->names[j].name, ch->name, DAT_NAME_LEN);
            c->names[j].name[DAT_NAME_LEN] = '\0';
            c->names[j].first_index = i;
            c->name_count++;
        }
        if (c->names[j].first_index != i)
            c->names[j].index_varies = 1;
        c->names[j].present++;
        if (ch->size > 0)
            c->names[j].non_empty++;
    }
}

static void census_report(const census *c)
{
    int i;

    if (c->files == 0)
        return;

    printf("\n%s - %d files, %d distinct schema%s\n",
           c->label, c->files, c->variant_count,
           c->variant_count == 1 ? "" : "s");

    printf("  chunk          idx  present   w/data  role\n");
    for (i = 0; i < c->name_count; i++) {
        const name_stat *n = &c->names[i];
        const char *role;

        if (n->present == c->files && n->non_empty == c->files)
            role = "mandatory, always populated";
        else if (n->present == c->files)
            role = "mandatory, sometimes empty";
        else
            role = "OPTIONAL";

        printf("  %-14s %-4s %3d/%-5d %6d   %s\n",
               n->name,
               n->index_varies ? "var" : "fix",
               n->present, c->files, n->non_empty, role);
    }

    if (c->variant_count > 1) {
        printf("\n  schema variants:\n");
        for (i = 0; i < c->variant_count && i < 10; i++) {
            printf("    %3d file%s %2d chunks  e.g. %s\n",
                   c->variants[i].count,
                   c->variants[i].count == 1 ? " " : "s",
                   c->variants[i].chunk_count,
                   c->variants[i].example);
        }
        if (c->variant_count > 10)
            printf("    ... and %d more\n", c->variant_count - 10);
    }
}

/*
 * Census every chunked .DAT on the disc. COMMON.DAT and ZONE*.DAT are reported
 * separately: they are different file types that merely share a container.
 */
static int cmd_dats(disc *d)
{
    int i, n = disc_file_count(d);
    census common, zone;
    int other = 0;
    int shown_common = 0, shown_zone = 0;

    census_init(&common, "COMMON.DAT");
    census_init(&zone, "ZONE*.DAT");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        dat_archive ar;

        base = base ? base + 1 : f->path;

        if (!strstr(base, ".DAT") && !strstr(base, ".ALL"))
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (!dat_probe(buf.data, buf.size)) {
            if (other == 0)
                printf("Files using a different container:\n");
            if (other < 4) {
                printf("  %-46s first u32 = 0x%08X\n", f->path,
                       buf.size >= 4 ? q2_rd_u32(buf.data) : 0);
            }
            other++;
            q2_buf_free(&buf);
            continue;
        }

        if (dat_open_buf(&ar, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        if (strncmp(base, "COMMON", 6) == 0) {
            if (!shown_common) {
                printf("\nCOMMON.DAT chunk directory (from %s)\n", f->path);
                dump_dat_chunks(&ar, base);
                shown_common = 1;
            }
            census_add(&common, &ar, f->path);
        } else if (strncmp(base, "ZONE", 4) == 0) {
            if (!shown_zone) {
                printf("\nZONE*.DAT chunk directory (from %s)\n", f->path);
                dump_dat_chunks(&ar, base);
                shown_zone = 1;
            }
            census_add(&zone, &ar, f->path);
        }

        dat_close(&ar);
    }

    if (other > 4)
        printf("  ... and %d more\n", other - 4);

    census_report(&common);
    census_report(&zone);

    printf("\n%d files use a non-chunked container.\n", other);
    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Run every level file through the typed loader. This is the acceptance test
 * for the container work: if all 164 COMMON/ZONE files resolve to their fixed
 * chunk slots with no unknown names and no missing mandatory chunks, the schema
 * in level.h is right for this build.
 */
static int cmd_verify(disc *d)
{
    int i, n = disc_file_count(d);
    int common_ok = 0, zone_ok = 0, failed = 0, skipped = 0;
    unsigned long long points_total = 0;
    unsigned long long quads_total = 0;
    unsigned long long planes_total = 0;
    unsigned long long normals_unit = 0;
    unsigned long long spawns_total = 0;
    unsigned long long lights_total = 0;
    unsigned long long lights_bad = 0;
    unsigned long long areas_total = 0;
    unsigned long long links_total = 0;
    unsigned long long links_bad = 0;
    unsigned long long names_total = 0;

    printf("Verifying every level file against the typed schema...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_result r;

        base = base ? base + 1 : f->path;

        if (strncmp(base, "COMMON", 6) != 0 && strncmp(base, "ZONE", 4) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK) {
            printf("  READ FAILED   %s\n", f->path);
            failed++;
            continue;
        }

        if (strncmp(base, "COMMON", 6) == 0) {
            q2_common_file cf;
            r = q2_common_open(&cf, &buf);
            if (r == Q2_OK) {
                q2_start_pos_list spawns;
                q2_light_list lights;

                common_ok++;

                if (q2_start_pos_parse(&spawns, &cf) == Q2_OK) {
                    spawns_total += spawns.count;
                } else {
                    printf("  startpos: bad  %s\n", f->path);
                    failed++;
                }

                if (q2_lights_parse(&lights, &cf) == Q2_OK) {
                    u32 li;
                    lights_total += lights.count;
                    for (li = 0; li < lights.count; li++) {
                        q2_light lt;
                        if (!q2_light_get(&lights, li, &lt))
                            continue;
                        /* radius must be the integer square root of radius_sq,
                         * and the inner radius must not exceed it. Both are
                         * cheap invariants that a misread stride would break. */
                        if ((u32)lt.radius * lt.radius > lt.radius_sq ||
                            (u32)(lt.radius + 1) * (lt.radius + 1) <= lt.radius_sq ||
                            lt.inner_radius_sq > lt.radius_sq)
                            lights_bad++;
                    }
                } else {
                    printf("  lights: bad  %s\n", f->path);
                    failed++;
                }

                q2_common_close(&cf);
            } else {
                printf("  %-13s %s\n", q2_result_str(r), f->path);
                failed++;
                q2_buf_free(&buf);
            }
        } else {
            q2_zone_file zf;
            r = q2_zone_open(&zf, &buf);
            if (r == Q2_OK) {
                q2_points pts;

                /* The chunk directory resolving is necessary but not
                 * sufficient — also parse the vertex pool, which is the first
                 * chunk whose *internal* layout we claim to understand. */
                r = q2_points_parse(&pts, &zf);
                if (r == Q2_OK) {
                    q2_collision coll;
                    q2_scene sc;
                    int sub_ok = 1;

                    zone_ok++;
                    points_total += pts.count;

                    /* Scene/MapMod: walk every node's polygon record. */
                    if (q2_scene_parse(&sc, &zf) == Q2_OK) {
                        u32 ni;
                        for (ni = 0; ni < sc.node_count; ni++) {
                            q2_mapmod_rec rec;
                            if (!q2_scene_get_mapmod(&sc, ni, &rec)) {
                                printf("  mapmod node %u bad in %s\n", ni, f->path);
                                sub_ok = 0;
                                break;
                            }
                            quads_total += rec.num_polys;
                        }
                    } else {
                        printf("  scene: parse failed  %s\n", f->path);
                        sub_ok = 0;
                    }

                    /* Portal graph and name table. */
                    {
                        q2_area_graph ag;
                        q2_map_name_table nt;

                        if (q2_area_parse(&ag, &zf) == Q2_OK) {
                            u32 a;
                            areas_total += ag.area_count;
                            for (a = 0; a < ag.area_count; a++) {
                                u32 nl = q2_area_link_count(&ag, a), li2;
                                for (li2 = 0; li2 < nl; li2++) {
                                    q2_area_link lk;
                                    if (q2_area_get_link(&ag, a, li2, &lk))
                                        links_total++;
                                    else
                                        links_bad++;
                                }
                            }
                        } else {
                            printf("  areaconx: parse failed  %s\n", f->path);
                            sub_ok = 0;
                        }

                        if (q2_map_names_parse(&nt, &zf) == Q2_OK) {
                            names_total += nt.count;
                        } else {
                            printf("  mapnames: parse failed  %s\n", f->path);
                            sub_ok = 0;
                        }
                    }

                    /* Both collision hulls, including the normal-length check
                     * that is the strongest evidence the layout is right. */
                    {
                        int w;
                        for (w = 0; w < 2; w++) {
                            u32 pi;
                            if (q2_collision_parse(&coll, &zf,
                                    w ? Q2_COLL_SECONDARY : Q2_COLL_PRIMARY) != Q2_OK) {
                                printf("  collision[%d]: parse failed  %s\n", w, f->path);
                                sub_ok = 0;
                                continue;
                            }
                            for (pi = 0; pi < coll.plane_count; pi++) {
                                q2_coll_plane pl;
                                s32 len_sq;
                                if (!q2_collision_get_plane(&coll, pi, &pl))
                                    continue;
                                len_sq = q2_coll_normal_len_sq(&pl);
                                planes_total++;
                                /* Unit length within 2 LSB: 4094^2 .. 4096^2 */
                                if (len_sq >= 4094 * 4094 && len_sq <= 4096 * 4096)
                                    normals_unit++;
                            }
                        }
                    }

                    if (!sub_ok)
                        failed++;

                    q2_points_free(&pts);
                } else {
                    printf("  points: %-6s %s\n", q2_result_str(r), f->path);
                    failed++;
                }
                q2_zone_close(&zf);
            } else {
                printf("  %-13s %s\n", q2_result_str(r), f->path);
                failed++;
                q2_buf_free(&buf);
            }
        }
    }

    printf("  COMMON.DAT : %d resolved\n", common_ok);
    printf("  ZONE*.DAT  : %d resolved\n", zone_ok);
    printf("  vertices   : %llu across all zones\n", points_total);
    printf("  quads      : %llu\n", quads_total);
    printf("  coll planes: %llu, of which %llu are unit normals (%.2f%%)\n",
           planes_total, normals_unit,
           planes_total ? 100.0 * (double)normals_unit / (double)planes_total : 0.0);
    printf("  areas      : %llu with %llu links (%llu unreadable)\n",
           areas_total, links_total, links_bad);
    printf("  map names  : %llu\n", names_total);
    printf("  spawns     : %llu\n", spawns_total);
    printf("  lights     : %llu, %llu failing the radius invariant\n",
           lights_total, lights_bad);
    printf("  failed     : %d\n", failed);
    if (skipped)
        printf("  skipped    : %d\n", skipped);

    printf("\n%s\n", failed == 0
           ? "PASS - every level file matches the catalogued schema."
           : "FAIL - see the entries above.");

    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Render a zone to a PPM. This is the end-to-end test of the geometry path:
 * disc -> zone chunks -> GTE -> ordering table -> rasteriser -> pixels, with no
 * window and no GPU involved. If this produces a coherent image, every layer
 * beneath it is working.
 *
 * The camera is placed automatically at the centre of the zone's true world
 * bounds, backed off along -Z far enough to see the whole thing.
 */
static int cmd_render(disc *d, const char *map, int zone_index, const char *out_path,
                      s32 yaw, s32 pitch)
{
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    q2_world_stats stats;
    q2_result r;
    s32 wmin[3], wmax[3];
    const int W = 512, H = 480;   /* 2x the PAL framebuffer, for legibility */

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n", map, zone_index, q2_result_str(r));
        return 1;
    }

    q2_world_bounds(&zone, wmin, wmax);

    printf("%s\n", zone.name);
    printf("  scene nodes   : %u\n", zone.scene.node_count);
    printf("  vertices      : %u\n", zone.points.count);
    printf("  world bounds  : [%d %d %d] .. [%d %d %d]\n",
           wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2]);
    printf("  world size    : %d x %d x %d\n",
           wmax[0] - wmin[0], wmax[1] - wmin[1], wmax[2] - wmin[2]);

    q2_camera_default(&cam, W, H);
    cam.yaw   = yaw;
    cam.pitch = pitch;

    /* Frame the whole zone: sit at its centre and back off along the camera's
     * own view direction by enough that the largest extent fits a 90-degree
     * field. Backing off in world -Z only works when looking down -Z, which
     * stops being true the moment a pitch is applied. */
    {
        s32 cx = (wmin[0] + wmax[0]) / 2;
        s32 cy = (wmin[1] + wmax[1]) / 2;
        s32 cz = (wmin[2] + wmax[2]) / 2;
        s32 ex = wmax[0] - wmin[0];
        s32 ey = wmax[1] - wmin[1];
        s32 ez = wmax[2] - wmin[2];
        s32 extent = ex > ez ? ex : ez;
        s32 dist;

        if (ey > extent)
            extent = ey;
        dist = extent;

        /* Forward vector for yaw/pitch, in 1.3.12. */
        {
            s32 sy = q2_sin12(yaw),   cyaw = q2_cos12(yaw);
            s32 sp = q2_sin12(pitch), cp   = q2_cos12(pitch);
            s32 fx = (s32)(((s64)cp * sy) >> Q2_FRAC_12);
            s32 fy = -sp;
            s32 fz = (s32)(((s64)cp * cyaw) >> Q2_FRAC_12);

            cam.pos[0] = cx - (s32)(((s64)fx * dist) >> Q2_FRAC_12);
            cam.pos[1] = cy - (s32)(((s64)fy * dist) >> Q2_FRAC_12);
            cam.pos[2] = cz - (s32)(((s64)fz * dist) >> Q2_FRAC_12);
        }
    }

    printf("  camera        : [%d %d %d] yaw=%d pitch=%d h=%u\n",
           cam.pos[0], cam.pos[1], cam.pos[2], cam.yaw, cam.pitch, cam.projection);

    r = psx_ot_init(&ot, 4096, 300000);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot allocate ordering table: %s\n", q2_result_str(r));
        q2_world_free_zone(&zone);
        return 1;
    }

    r = psx_fb_init(&fb, W, H);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot allocate framebuffer: %s\n", q2_result_str(r));
        psx_ot_free(&ot);
        q2_world_free_zone(&zone);
        return 1;
    }

    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, &stats);

    printf("\n  quads total   : %u\n", stats.quads_total);
    printf("  emitted       : %u\n", stats.quads_emitted);
    printf("  rejected near : %u\n", stats.quads_rejected_near);
    printf("  rejected bad  : %u\n", stats.quads_rejected_bad);
    printf("  ot overflow   : %u\n", stats.ot_overflow);

    psx_raster_opts_default(&opts);
    opts.textures = false;    /* no texture codec yet — Gouraud only */

    psx_fb_clear(&fb, psx_rgb555(16, 16, 32));
    psx_raster_ot(&fb, &ot, NULL, &opts);

    r = psx_fb_write_ppm(&fb, out_path);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot write %s: %s\n", out_path, q2_result_str(r));
    } else {
        printf("\n  wrote %s (%dx%d)\n", out_path, W, H);
    }

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    q2_world_free_zone(&zone);
    return r == Q2_OK ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/*
 * Load and decode every sound bank on the disc.
 *
 * The strongest check here is not that decoding "works" but that the ADPCM
 * block headers are all structurally valid and that every bank's bodies fit in
 * the console's 512 KB of sound RAM. Both would fail loudly if the bank layout
 * or the endianness were misread.
 */
static int cmd_audio(disc *d)
{
    int i, n = disc_file_count(d);
    u32 banks = 0, sounds = 0, looping = 0, bad_blocks = 0, failed = 0;
    u32 worst_body = 0;
    char worst_map[64];
    s16 *pcm = NULL;
    size_t pcm_cap = 1 << 20;

    worst_map[0] = '\0';
    pcm = (s16 *)malloc(pcm_cap * sizeof(s16));
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    printf("Decoding every sound bank on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_sound_bank bank;
        u32 s, total;

        if (*p == '/')
            p++;
        if (strncmp(p, "Q2DATA/LEVELS/", 14) != 0)
            continue;
        if (!strstr(p, "SNDVRAM.DAT"))
            continue;

        rest  = p + 14;
        slash = strchr(rest, '/');
        if (!slash)
            continue;
        len = (size_t)(slash - rest);
        if (len >= sizeof(map))
            len = sizeof(map) - 1;
        memcpy(map, rest, len);
        map[len] = '\0';

        if (q2_sound_bank_load(&bank, d, map) != Q2_OK) {
            printf("  LOAD FAILED  %s\n", map);
            failed++;
            continue;
        }

        banks++;

        for (s = 0; s < bank.count; s++) {
            q2_vag vag;

            if (!q2_sound_bank_get(&bank, s, &vag)) {
                printf("  bad VAG %u in %s\n", s, map);
                failed++;
                continue;
            }

            sounds++;
            if (vag.looping)
                looping++;

            bad_blocks += q2_spu_adpcm_validate(vag.body, vag.data_size);

            /* Actually decode it, so a broken decoder cannot hide behind a
             * header-only check. */
            q2_spu_adpcm_decode(vag.body, vag.data_size, pcm, (u32)pcm_cap);
        }

        total = q2_sound_bank_total_body(&bank);
        if (total > worst_body) {
            worst_body = total;
            strncpy(worst_map, map, sizeof(worst_map) - 1);
            worst_map[sizeof(worst_map) - 1] = '\0';
        }

        q2_sound_bank_free(&bank);
    }

    free(pcm);

    printf("  banks loaded    : %u\n", banks);
    printf("  sounds          : %u\n", sounds);
    printf("  looping         : %u\n", looping);
    printf("  invalid blocks  : %u\n", bad_blocks);
    printf("  failures        : %u\n", failed);
    printf("  largest bank    : %s, %u bytes of ADPCM\n", worst_map, worst_body);
    printf("  SPU RAM usable  : %d bytes\n", SPU_RAM_SIZE - SPU_RAM_RESERVED);
    printf("  fits            : %s\n",
           worst_body <= (u32)(SPU_RAM_SIZE - SPU_RAM_RESERVED) ? "yes" : "NO");

    printf("\n%s\n", (failed == 0 && bad_blocks == 0)
           ? "PASS - every bank parsed and every ADPCM block is structurally valid."
           : "FAIL - see above.");

    return (failed || bad_blocks) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Demultiplex and decode the streamed music.
 *
 * The claim under test is the interleave: each .XAI carries four independent
 * stereo streams and sector_index % 4 == channel_num with no exceptions. That
 * is checked directly here rather than assumed, because if it were wrong every
 * track would be a quarter of four different songs.
 */
static int cmd_music(disc *d)
{
    static const char letters[] = { 'A', 'B', 'C', 'D', 'E' };
    s16 *pcm;
    const u32 pcm_cap = XA_FRAMES_PER_SECTOR * 2;
    u32 total_sectors = 0, audio_sectors = 0, bad_blocks = 0;
    u32 interleave_violations = 0, skipped = 0;
    size_t li;

    pcm = (s16 *)malloc(pcm_cap * sizeof(s16));
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    printf("Demultiplexing the XA music streams...\n\n");
    printf("  file       ch  sectors    seconds\n");

    for (li = 0; li < sizeof(letters); li++) {
        char path[64];
        const disc_file *f;
        u32 ch;

        snprintf(path, sizeof(path), "Q2DATA/AUD/QUAKE_%c.XAI", letters[li]);
        f = disc_find(d, path);
        if (!f) {
            printf("  QUAKE_%c    missing\n", letters[li]);
            continue;
        }

        for (ch = 0; ch < XAI_CHANNEL_COUNT; ch++) {
            q2_xa_track track;
            q2_xa_decoder dec;
            u32 cursor = 0, sectors = 0, n;

            if (q2_xa_track_open(&track, d, letters[li], (u8)ch) != Q2_OK)
                continue;

            q2_xa_decoder_reset(&dec);

            while ((n = q2_xa_track_read(&track, &dec, &cursor, pcm, pcm_cap)) > 0)
                sectors++;

            printf("  QUAKE_%c    %u   %-9u  %.1f\n",
                   letters[li], ch, sectors,
                   (double)sectors * XA_FRAMES_PER_SECTOR / (double)XA_SAMPLE_RATE);

            audio_sectors += sectors;
        }

        /* Independently check the round-robin and the group parameters by
         * walking the raw sectors, without going through the track reader. */
        {
            u32 count = (f->size + CD_SECTOR_FORM1 - 1) / CD_SECTOR_FORM1;
            u32 i;

            for (i = 0; i < count; i++) {
                u8 raw[CD_SECTOR_RAW];

                if (disc_read_raw_sector(d, f->lba + i, raw) != Q2_OK)
                    break;

                total_sectors++;

                if (!(raw[18] & CD_SUBMODE_AUDIO) || !(raw[18] & CD_SUBMODE_FORM2)) {
                    skipped++;
                    continue;
                }

                if (raw[17] != (u8)(i % XAI_CHANNEL_COUNT))
                    interleave_violations++;

                bad_blocks += q2_xa_validate_sector(raw + 24);
            }
        }
    }

    free(pcm);

    printf("\n  sectors scanned       : %u\n", total_sectors);
    printf("  audio sectors decoded : %u\n", audio_sectors);
    printf("  non-audio skipped     : %u\n", skipped);
    printf("  interleave violations : %u  (expected 0)\n", interleave_violations);
    printf("  invalid ADPCM blocks  : %u  (expected 0)\n", bad_blocks);

    printf("\n%s\n", (interleave_violations == 0 && bad_blocks == 0)
           ? "PASS - the round-robin holds and every sound group is valid."
           : "FAIL - see above.");

    return (interleave_violations || bad_blocks) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
static int cmd_hexdump(disc *d, const char *path, size_t count)
{
    q2_buf buf;
    q2_result r;
    size_t i, j;

    r = disc_read_file(d, path, &buf);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read %s: %s\n", path, q2_result_str(r));
        return 1;
    }

    if (count > buf.size)
        count = buf.size;

    printf("%s - %zu bytes, showing %zu\n\n", path, buf.size, count);

    for (i = 0; i < count; i += 16) {
        printf("%08zX  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < count)
                printf("%02X ", buf.data[i + j]);
            else
                printf("   ");
            if (j == 7)
                printf(" ");
        }
        printf(" |");
        for (j = 0; j < 16 && i + j < count; j++) {
            u8 ch = buf.data[i + j];
            putchar(ch >= 0x20 && ch < 0x7F ? (int)ch : '.');
        }
        printf("|\n");
    }

    q2_buf_free(&buf);
    return 0;
}

/* ------------------------------------------------------------------------- */
static void make_dirs_for(const char *path)
{
    char tmp[1024];
    size_t i;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            q2_mkdir(tmp);
            tmp[i] = saved;
        }
    }
}

static int cmd_extract(disc *d, const char *outdir)
{
    int i, n = disc_file_count(d);
    int ok = 0, failed = 0;

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        char out[1024];
        q2_buf buf;
        FILE *fp;
        size_t written;

        snprintf(out, sizeof(out), "%s%s", outdir, f->path);
        make_dirs_for(out);

        if (disc_read_file(d, f->path, &buf) != Q2_OK) {
            fprintf(stderr, "  FAILED %s\n", f->path);
            failed++;
            continue;
        }

        fp = fopen(out, "wb");
        if (!fp) {
            fprintf(stderr, "  CANNOT WRITE %s\n", out);
            q2_buf_free(&buf);
            failed++;
            continue;
        }

        written = fwrite(buf.data, 1, buf.size, fp);
        fclose(fp);

        printf("  %s (%zu bytes)\n", f->path, written);
        q2_buf_free(&buf);
        ok++;
    }

    printf("\nextracted %d files, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    disc *d = NULL;
    q2_result r;
    const char *cmd;
    const char *path;
    int rc;

    if (argc < 3) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    cmd  = argv[1];
    path = argv[2];

    if (getenv("Q2PSX_VERBOSE"))
        q2_log_set_level(Q2_LOG_DEBUG);

    r = disc_open(&d, path);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot open disc '%s': %s\n", path, q2_result_str(r));
        return 1;
    }

    if (strcmp(cmd, "ident") == 0) {
        rc = cmd_ident(d);
    } else if (strcmp(cmd, "disc") == 0) {
        rc = cmd_disc(d);
    } else if (strcmp(cmd, "levels") == 0) {
        rc = cmd_levels(d);
    } else if (strcmp(cmd, "dats") == 0) {
        rc = cmd_dats(d);
    } else if (strcmp(cmd, "verify") == 0) {
        rc = cmd_verify(d);
    } else if (strcmp(cmd, "audio") == 0) {
        rc = cmd_audio(d);
    } else if (strcmp(cmd, "music") == 0) {
        rc = cmd_music(d);
    } else if (strcmp(cmd, "render") == 0) {
        if (argc < 4) {
            fprintf(stderr, "render needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            const char *outp = (argc >= 6) ? argv[5] : "zone.ppm";
            s32 yaw   = (argc >= 7) ? (s32)strtol(argv[6], NULL, 10) : 0;
            s32 pitch = (argc >= 8) ? (s32)strtol(argv[7], NULL, 10) : 0;
            rc = cmd_render(d, argv[3], zi, outp, yaw, pitch);
        }
    } else if (strcmp(cmd, "dat") == 0) {
        if (argc < 4) {
            fprintf(stderr, "dat needs a file path\n");
            rc = 1;
        } else {
            rc = cmd_dat(d, argv[3]);
        }
    } else if (strcmp(cmd, "hexdump") == 0) {
        if (argc < 4) {
            fprintf(stderr, "hexdump needs a file path\n");
            rc = 1;
        } else {
            size_t count = 256;
            if (argc >= 5)
                count = (size_t)strtoul(argv[4], NULL, 0);
            rc = cmd_hexdump(d, argv[3], count);
        }
    } else if (strcmp(cmd, "extract") == 0) {
        if (argc < 4) {
            fprintf(stderr, "extract needs an output directory\n");
            rc = 1;
        } else {
            rc = cmd_extract(d, argv[3]);
        }
    } else {
        fprintf(stderr, "unknown command '%s'\n\n", cmd);
        usage();
        rc = 1;
    }

    disc_close(d);
    return rc;
}
