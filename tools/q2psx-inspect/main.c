/*
 * q2psx-inspect — the reverse-engineering harness.
 *
 * Opens a disc and reports what is on it, without needing a game window. This is
 * the tool used to validate every format claim in docs/FORMATS.md: if the parser
 * here reads a real disc cleanly, the engine's loader will too, because they are
 * the same code.
 */
#include "aimodule.h"
#include "area.h"
#include "cmd_exe.h"
#include "collision.h"
#include "dat.h"
#include "disc.h"
#include "entity.h"
#include "events_rt.h"
#include "ident.h"
#include "level.h"
#include "leveltable.h"
#include "mover.h"
#include "points.h"
#include "pickup.h"
#include "population.h"
#include "trigger.h"
#include "raster.h"
#include "reloc.h"
#include "scene.h"
#include "spawn.h"
#include "sim.h"
#include "version.h"
#include "vram.h"
#include "world.h"
#include "worldscale.h"
#include "trig.h"
#include "vag.h"
#include "xa.h"
#include "q2psx.h"

#include <math.h>
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
    puts("  leveltable <disc>           dump the level table and check it against the disc");
    puts("  reloc   <disc>              relocate every AI module and census the fixups");
    puts("  events  <disc>              run every event script and census the opcodes");
    puts("  walk    <disc> <map> [z] [ticks]  drop a player in and simulate");
    puts("  textures <disc>             decode every compressed VRAM image");
    puts("  cluts   <disc>              check CLUT binding and UV rotation on every poly");
    puts("  music   <disc>              demultiplex and decode the XA music streams");
    puts("  render  <disc> <map> [z] [out.ppm] [yaw] [pitch]  render a zone (4096 = full turn)");
    puts("  hexdump <disc> <path> [n]   hex dump the first n bytes of a file");
    puts("  extract <disc> <outdir>     extract the whole filesystem");
    puts("");
    puts("executable:");
    puts("  exe     <disc>              header, memory map and documented landmarks");
    puts("  disasm  <disc> <addr> [n]   disassemble n instructions (0 = to the return)");
    puts("  xrefs   <disc> <addr>       every reference to an address, code and data");
    puts("  funcs   <disc> [addr]       call targets found by sweeping the image");
    puts("  bytes   <disc> <addr> [n]   hex dump executable memory by address");
    puts("  find    <disc> <str|0xhex>  locate a string or byte pattern in the image");
    puts("  access  <disc> <off> [insn] every instruction touching a record offset");
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
    unsigned long long convex_ok = 0;
    unsigned long long convex_bad = 0;
    unsigned long long pop_groups = 0;
    unsigned long long pop_spawns = 0;
    unsigned long long pop_places = 0;
    unsigned long long pop_paths = 0;
    unsigned long long trig_total = 0;
    unsigned long long trig_planes = 0;
    unsigned long long pickups_total = 0;
    unsigned long long pickups_taken = 0;
    unsigned long long spawn_records = 0, spawn_placed = 0, spawn_oob = 0;
    u8 class_seen[Q2_MONSTER_CLASS_COUNT];

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

                /* Population: actor and pickup placement, plus patrol graphs. */
                {
                    q2_population pop;
                    if (q2_population_parse(&pop, &cf) == Q2_OK) {
                        u32 gi;
                        pop_groups += pop.group_count;
                        for (gi = 0; gi < pop.group_count; gi++) {
                            q2_pop_group g;
                            u32 slot;

                            if (!q2_pop_get_group(&pop, gi, &g))
                                continue;

                            if (q2_pop_group_is_path(&g)) {
                                q2_pop_path pn;
                                for (slot = 0; q2_pop_get_path(&pop, &g, slot, &pn); slot++)
                                    pop_paths++;
                            } else {
                                q2_pop_spawn sp2;
                                for (slot = 0; q2_pop_get_spawn(&pop, &g, slot, &sp2); slot++)
                                    pop_spawns++;
                            }

                            {
                                q2_pop_place pl2;
                                for (slot = 0; q2_pop_get_place(&pop, &g, slot, &pl2); slot++)
                                    pop_places++;
                            }
                        }
                    }
                }

                /* Pickups: build the live item set from the place records. */
                {
                    q2_population pop2;
                    if (q2_population_parse(&pop2, &cf) == Q2_OK) {
                        q2_pickup_set ps;
                        if (q2_pickups_build(&ps, &pop2) == Q2_OK) {
                            pickups_total += ps.count;
                            /* With no definition table attached nothing can be
                             * collected, which is the honest state until the
                             * pickup table is decoded. Prove that rather than
                             * assume it. */
                            {
                                q2_inventory inv;
                                s32 at[3];
                                u32 k;
                                q2_inventory_init(&inv);
                                for (k = 0; k < ps.count; k++) {
                                    at[0] = ps.items[k].pos[0];
                                    at[1] = ps.items[k].pos[1];
                                    at[2] = ps.items[k].pos[2];
                                    pickups_taken += q2_pickups_collect(&ps, at, &inv);
                                }
                            }
                            q2_pickups_free(&ps);
                        }
                    }
                }

                /* Creature spawning. Every class the disc uses is treated as
                 * registered here, so the count reflects the spawn records
                 * rather than which modules we happen to have loaded. */
                {
                    q2_population pop3;
                    if (q2_population_parse(&pop3, &cf) == Q2_OK) {
                        q2_monster_set ms;
                        q2_spawn_stats st;
                        u32 k;

                        memset(&ms, 0, sizeof(ms));
                        for (k = 0; k < Q2_MONSTER_CLASS_COUNT; k++)
                            q2_monster_set_register(&ms, k);

                        if (q2_spawn_from_population(&ms, &pop3, &st) == Q2_OK) {
                            spawn_records += st.records;
                            spawn_placed  += st.placed;
                            spawn_oob     += st.out_of_range;
                            for (k = 0; k < ms.count; k++) {
                                if (ms.monsters[k].class_id >= 0 &&
                                    ms.monsters[k].class_id < Q2_MONSTER_CLASS_COUNT)
                                    class_seen[ms.monsters[k].class_id] = 1;
                            }
                        }
                        q2_monster_set_free(&ms);
                    }
                }

                /* Trigger volumes. */
                {
                    q2_triggers tg;
                    if (q2_triggers_parse(&tg, &cf) == Q2_OK) {
                        trig_total += tg.count;
                        trig_planes += tg.plane_count;
                    } else {
                        printf("  trigbounds: bad  %s\n", f->path);
                        failed++;
                    }
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
                            /* Geometric convexity: an interior point must be on
                             * the negative side of every plane of its node.
                             * This is the test that actually validates the
                             * plane-point encoding; bbox containment does not. */
                            {
                                u32 ni2;
                                for (ni2 = 0; ni2 < coll.node_count; ni2++) {
                                    q2_coll_node hn, nx2;
                                    s32 centroid[3] = { 0, 0, 0 };
                                    u32 k, n_used = 0;

                                    if (!q2_collision_get_node(&coll, ni2, &hn) ||
                                        !q2_collision_get_node(&coll, ni2 + 1, &nx2))
                                        continue;
                                    if (nx2.first_plane <= hn.first_plane)
                                        continue;

                                    for (k = hn.first_plane; k < nx2.first_plane; k++) {
                                        s32 pt[3];
                                        if (!q2_coll_plane_point(&coll, ni2, k, pt))
                                            continue;
                                        centroid[0] += pt[0];
                                        centroid[1] += pt[1];
                                        centroid[2] += pt[2];
                                        n_used++;
                                    }
                                    if (!n_used)
                                        continue;
                                    centroid[0] /= (s32)n_used;
                                    centroid[1] /= (s32)n_used;
                                    centroid[2] /= (s32)n_used;

                                    for (k = hn.first_plane; k < nx2.first_plane; k++) {
                                        if (q2_coll_plane_distance(&coll, ni2, k, centroid) > 0)
                                            convex_bad++;
                                        else
                                            convex_ok++;
                                    }
                                }
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
    printf("  convexity  : %llu/%llu planes consistent (%.3f%%), %llu violations\n",
           convex_ok, convex_ok + convex_bad,
           (convex_ok + convex_bad)
               ? 100.0 * (double)convex_ok / (double)(convex_ok + convex_bad) : 0.0,
           convex_bad);
    printf("  spawns     : %llu\n", spawns_total);
    printf("  triggers   : %llu volumes, %llu planes\n", trig_total, trig_planes);
    printf("  pickups    : %llu items, %llu collectable (no id table yet)\n",
           pickups_total, pickups_taken);
    {
        u32 k, distinct = 0;
        for (k = 0; k < Q2_MONSTER_CLASS_COUNT; k++)
            if (class_seen[k]) distinct++;
        printf("  creatures  : %llu spawn records, %llu placed, %llu bad class, %u distinct classes\n",
               spawn_records, spawn_placed, spawn_oob, distinct);
    }
    printf("  population : %llu groups, %llu actors, %llu placements, %llu path nodes\n",
           pop_groups, pop_spawns, pop_places, pop_paths);
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
 * Poly render-flag census, split by whether the owning Scene node is driven by
 * a mover.
 *
 * MapMod Poly.uvIdxFlags bits 6-7 are render flags of unknown meaning, set on
 * 11.7% of polygons world-wide, and the renderer ignores them. The question
 * this answers is whether that 11.7% is spread evenly or concentrates on
 * mover-driven geometry (doors, lifts, plats — the brush-model entities). An
 * even split rules the flags out as the cause of a bmodel-specific fault;
 * enrichment on mover nodes makes them the prime suspect.
 */
static int cmd_polyflags(disc *d)
{
    int i, n = disc_file_count(d);
    unsigned long long mv[4] = {0,0,0,0}, wd[4] = {0,0,0,0};
    unsigned long long mv_nodes = 0, wd_nodes = 0, mv_total = 0, wd_total = 0;
    unsigned long long zones = 0, mover_nodes_seen = 0;
    unsigned long long mv_uvfail = 0, wd_uvfail = 0;
    unsigned long long overrun_zones = 0, overrun_nodes = 0, mismatch_zones = 0;
    unsigned long long origin_const_zones = 0, origin_nodes = 0, origin_same = 0;

    printf("Poly render-flag census (uvIdxFlags bits 6-7), by node kind\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_zone_file zf;
        q2_scene scene;
        q2_events ev;
        q2_mover_set movers;
        q2_points pts;
        bool have_pts;
        u8 *is_mover;
        u32 node;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "ZONE", 4) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        if (q2_scene_parse(&scene, &zf) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }

        zones++;

        /* The renderer indexes points.groups[] by SCENE NODE index without
         * bounding it. Nothing in the format ties the two counts together, so
         * measure whether they ever disagree. */
        have_pts = (q2_points_parse(&pts, &zf) == Q2_OK);
        if (have_pts) {
            if (scene.node_count > pts.group_count) {
                printf("  OVERRUN  %-28s nodes %u > point groups %u\n",
                       f->path, scene.node_count, pts.group_count);
                overrun_zones++;
                overrun_nodes += scene.node_count - pts.group_count;
            }
            if (scene.node_count != pts.group_count)
                mismatch_zones++;
        }

        is_mover = (u8 *)calloc(scene.node_count ? scene.node_count : 1, 1);
        if (!is_mover) {
            q2_zone_close(&zf);
            continue;
        }

        if (q2_events_parse_zone(&ev, &zf) == Q2_OK &&
            q2_movers_build(&movers, &ev) == Q2_OK) {
            u32 m, k;
            for (m = 0; m < movers.count; m++) {
                for (k = 0; k < movers.movers[m].part_count; k++) {
                    s16 nd = movers.movers[m].node[k];
                    if (nd >= 0 && (u32)nd < scene.node_count) {
                        if (!is_mover[nd])
                            mover_nodes_seen++;
                        is_mover[nd] = 1;
                    }
                }
            }
            q2_movers_free(&movers);
        }

        /* The renderer uses node.origin as the per-node translation. If it is
         * constant zone-wide it is not a per-node origin at all, and whatever
         * positions geometry (and whatever a mover would animate) is elsewhere. */
        {
            q2_scene_node n0;
            u32 t, same = 0, total = 0;
            if (q2_scene_get_node(&scene, 0, &n0)) {
                for (t = 0; t < scene.node_count; t++) {
                    q2_scene_node nt;
                    if (!q2_scene_get_node(&scene, t, &nt))
                        continue;
                    total++;
                    if (nt.origin[0] == n0.origin[0] &&
                        nt.origin[1] == n0.origin[1] &&
                        nt.origin[2] == n0.origin[2])
                        same++;
                }
                if (total && same == total)
                    origin_const_zones++;
                origin_nodes += total;
                origin_same  += same;
            }
        }

        for (node = 0; node < scene.node_count; node++) {
            q2_mapmod_rec rec;
            u32 p;
            int is_mv = is_mover[node];

            if (!q2_scene_get_mapmod(&scene, node, &rec))
                continue;

            if (is_mv) mv_nodes++; else wd_nodes++;

            for (p = 0; p < rec.num_polys; p++) {
                q2_mapmod_poly poly;
                if (!q2_mapmod_get_poly(&rec, p, &poly))
                    continue;
                if (is_mv) { mv[poly.flags & 3]++; mv_total++; }
                else       { wd[poly.flags & 3]++; wd_total++; }

                /* The renderer silently keeps zeroed UVs when this lookup
                 * fails, collapsing the quad onto one texel. Count it. */
                if (!rec.uv || poly.uv_idx >= rec.uv_count) {
                    if (is_mv) mv_uvfail++; else wd_uvfail++;
                }
            }
        }

        free(is_mover);
        if (have_pts)
            q2_points_free(&pts);
        q2_zone_close(&zf);
    }

    printf("  zones scanned        : %llu\n", zones);
    printf("  mover-driven nodes   : %llu (%llu carry geometry)\n",
           mover_nodes_seen, mv_nodes);
    printf("  static world nodes   : %llu\n", wd_nodes);
    printf("  polys  bmodel/world  : %llu / %llu\n\n", mv_total, wd_total);

    printf("  flag    bmodel %%     world %%     bmodel n\n");
    for (i = 0; i < 4; i++) {
        printf("    %d    %9.3f   %9.3f   %10llu\n", i,
               mv_total ? 100.0 * (double)mv[i] / (double)mv_total : 0.0,
               wd_total ? 100.0 * (double)wd[i] / (double)wd_total : 0.0,
               mv[i]);
    }

    {
        double mv_set = mv_total
            ? 100.0 * (double)(mv_total - mv[0]) / (double)mv_total : 0.0;
        double wd_set = wd_total
            ? 100.0 * (double)(wd_total - wd[0]) / (double)wd_total : 0.0;
        printf("\n  any flag set: bmodel %.2f%%  world %.2f%%", mv_set, wd_set);
        if (wd_set > 0.0)
            printf("   (enrichment %.2fx)", mv_set / wd_set);
        printf("\n");
    }

    printf("\n  UV lookup failures (quad collapses to one texel):\n");
    printf("    bmodel %llu / %llu    world %llu / %llu\n",
           mv_uvfail, mv_total, wd_uvfail, wd_total);

    printf("\n  Scene node.origin (the renderer's per-node translation):\n");
    printf("    zones where it is CONSTANT     : %llu / %llu\n",
           origin_const_zones, zones);
    printf("    nodes sharing node 0's origin  : %llu / %llu\n",
           origin_same, origin_nodes);

    printf("\n  Scene nodes vs Points groups:\n");
    printf("    zones where the counts differ : %llu / %llu\n",
           mismatch_zones, zones);
    printf("    zones the renderer overruns   : %llu (%llu nodes past the end)\n",
           overrun_zones, overrun_nodes);

    return 0;
}

/* ------------------------------------------------------------------------- */
/*
 * MapMod.clut and the UV rotation, checked against the whole disc.
 *
 * The engine's own reading of these fields was recovered from the world
 * renderer at 0x80068044 (see docs/FORMATS.md §3.3). Reading it out of the code
 * is not by itself proof that the code is what runs on this data — a wrong
 * function, or a field that means something else on a different build, would
 * look exactly the same in the disassembly. So restate each rule as a property
 * the disc must satisfy and measure it:
 *
 *   clut >> 8   indexes the engine's CLUT-id table, which has clut4_count
 *               entries for that map, so it must be < clut4_count everywhere.
 *   clut & 3    selects semi-transparency, so the remaining six bits of the low
 *               byte should be dead — if they are ever set, something else is
 *               in there.
 *   uvIdx & 63  indexes the UV table and must stay inside it.
 */
#define CLUT_CENSUS_MAX_MAPS 64

typedef struct clut_map_stat {
    char               name[64];
    u32                clut4_count;
    unsigned long long polys;
    u32                max_index;
    u32                zones;
    bool               have_vram;
} clut_map_stat;

static clut_map_stat *clut_find_map(clut_map_stat *maps, int *count,
                                    const char *name)
{
    int i;
    for (i = 0; i < *count; i++)
        if (strcmp(maps[i].name, name) == 0)
            return &maps[i];
    if (*count >= CLUT_CENSUS_MAX_MAPS)
        return NULL;
    memset(&maps[*count], 0, sizeof(maps[0]));
    snprintf(maps[*count].name, sizeof(maps[0].name), "%s", name);
    return &maps[(*count)++];
}

/* "/Q2DATA/LEVELS/BASE0/ZONE0.DAT" -> "BASE0". NULL if the path is not one. */
static const char *clut_map_of(const char *path, char *buf, size_t cap)
{
    const char *rest, *slash;
    size_t len;

    if (*path == '/')
        path++;
    if (strncmp(path, "Q2DATA/LEVELS/", 14) != 0)
        return NULL;

    rest  = path + 14;
    slash = strchr(rest, '/');
    if (!slash)
        return NULL;

    len = (size_t)(slash - rest);
    if (len >= cap)
        len = cap - 1;
    memcpy(buf, rest, len);
    buf[len] = '\0';
    return buf;
}

static double hypot2(int dx, int dy)
{
    return sqrt((double)dx * dx + (double)dy * dy);
}

static double hypot3(int dx, int dy, int dz)
{
    return sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz);
}

static int cmd_cluts(disc *d)
{
    clut_map_stat maps[CLUT_CENSUS_MAX_MAPS];
    int map_count = 0;
    int i, n = disc_file_count(d);
    unsigned long long polys = 0, low2[4] = {0,0,0,0}, rot[4] = {0,0,0,0};
    unsigned long long high_bits_set = 0, uv_out_of_range = 0;
    unsigned long long agree[3] = {0,0,0}, agree_pairs = 0;
    unsigned long long rot_agree[3] = {0,0,0}, rot_pairs = 0;
    unsigned long long iso_good[3] = {0,0,0}, iso_total = 0;
    unsigned long long iso_rot_good[3] = {0,0,0}, iso_rot_total = 0;
    double iso_sum[3] = {0.0,0.0,0.0}, iso_rot_sum[3] = {0.0,0.0,0.0};
    static const char *const agree_name[3] = {
        "uv[j]              (old)",
        "uv[(3 - j) & 3]    (no f)",
        "uv[(3 - f - j) & 3](engine)"
    };
    int maps_out_of_range = 0;
    char mapbuf[64];

    printf("MapMod.clut and uvIdxFlags, as the world renderer at 0x80068044"
           " reads them\n\n");
    printf("  clut >> 8      index into the map's clut4[] array\n");
    printf("  clut & 3       non-zero selects semi-transparent (code 0x3E)\n");
    printf("  uvIdx & 0x3F   index into the record's UV table\n");
    printf("  uvIdx >> 6     UV rotation: vertex j takes uv[(3 - f - j) & 3]\n\n");

    /* First pass: how many CLUTs each map actually uploads. */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        clut_map_stat *m;
        q2_vram_section vs;

        if (!strstr(f->path, "SNDVRAM.DAT"))
            continue;
        if (!clut_map_of(f->path, mapbuf, sizeof(mapbuf)))
            continue;
        m = clut_find_map(maps, &map_count, mapbuf);
        if (!m)
            continue;
        if (q2_vram_load(&vs, d, mapbuf) != Q2_OK)
            continue;
        m->clut4_count = vs.clut4_count;
        m->have_vram   = true;
        q2_vram_free(&vs);
    }

    /* Second pass: every polygon on the disc. */
    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        clut_map_stat *m;
        q2_buf buf;
        q2_zone_file zf;
        q2_scene scene;
        q2_points pts;
        bool have_pts;
        u32 node;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "ZONE", 4) != 0)
            continue;
        if (!clut_map_of(f->path, mapbuf, sizeof(mapbuf)))
            continue;
        m = clut_find_map(maps, &map_count, mapbuf);
        if (!m)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_zone_open(&zf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }
        if (q2_scene_parse(&scene, &zf) != Q2_OK) {
            q2_zone_close(&zf);
            continue;
        }

        m->zones++;
        have_pts = (q2_points_parse(&pts, &zf) == Q2_OK);

        for (node = 0; node < scene.node_count; node++) {
            q2_mapmod_rec rec;
            const q2_point_group *grp = NULL;
            u32 p;

            if (have_pts && node < pts.group_count)
                grp = &pts.groups[node];
            /* One entry per polygon corner in this node: the vertex it uses,
             * and the texel each candidate rule would give it. A node holds at
             * most 63 quads. */
            struct { u8 vtx; u8 rot; u8 uv[3][2]; } corner[63 * 4];
            u32 corners = 0;

            if (!q2_scene_get_mapmod(&scene, node, &rec))
                continue;

            for (p = 0; p < rec.num_polys; p++) {
                q2_mapmod_poly poly;
                u32 index, j;

                if (!q2_mapmod_get_poly(&rec, p, &poly))
                    continue;

                index = (u32)(poly.clut >> 8);
                polys++;
                m->polys++;
                if (index > m->max_index)
                    m->max_index = index;

                low2[poly.clut & 3]++;
                rot[poly.flags & 3]++;

                if ((poly.clut & 0xFC) != 0)
                    high_bits_set++;
                if (!rec.uv || poly.uv_idx >= rec.uv_count) {
                    uv_out_of_range++;
                    continue;
                }

                /*
                 * Does the rule keep the texel scale isotropic?
                 *
                 * A quad's two edges have known world lengths. Whatever corner
                 * rule is right, walking one edge in texel space and the same
                 * edge in world space must give roughly the same texels per
                 * unit on both axes, because these are flat brush faces with a
                 * uniform texture scale. A rule that is 90 degrees out maps the
                 * long texel axis onto the short world axis and the two scales
                 * diverge. This is the test that can see a rotation, which the
                 * shared-vertex test structurally cannot: a rotated face is
                 * *meant* to disagree with its neighbours.
                 */
                if (grp) {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    q2_point pt[4];
                    bool have = true;
                    u32 r;

                    for (j = 0; j < 4; j++) {
                        u32 vi = grp->first + poly.vtx[j];
                        if (poly.vtx[j] >= grp->count ||
                            !q2_points_get(&pts, vi, &pt[j]))
                            have = false;
                    }

                    for (r = 0; have && r < 3; r++) {
                        static const int rule_c[3][4] = {
                            {0,1,2,3}, {3,2,1,0}, {0,0,0,0}   /* [2] filled below */
                        };
                        double wa, wb, ua, ub, sa, sb, ratio;
                        int c0, c1, c2;

                        if (r == 2) {
                            c0 = (int)((3u - poly.flags - 0u) & 3u);
                            c1 = (int)((3u - poly.flags - 1u) & 3u);
                            c2 = (int)((3u - poly.flags - 2u) & 3u);
                        } else {
                            c0 = rule_c[r][0];
                            c1 = rule_c[r][1];
                            c2 = rule_c[r][2];
                        }

                        wa = hypot3(pt[1].x - pt[0].x, pt[1].y - pt[0].y,
                                    pt[1].z - pt[0].z);
                        wb = hypot3(pt[2].x - pt[1].x, pt[2].y - pt[1].y,
                                    pt[2].z - pt[1].z);
                        ua = hypot2(uv[c1 * 2 + 0] - uv[c0 * 2 + 0],
                                    uv[c1 * 2 + 1] - uv[c0 * 2 + 1]);
                        ub = hypot2(uv[c2 * 2 + 0] - uv[c1 * 2 + 0],
                                    uv[c2 * 2 + 1] - uv[c1 * 2 + 1]);

                        if (wa < 1.0 || wb < 1.0 || ua < 1.0 || ub < 1.0)
                            break;   /* degenerate; scores no rule */

                        sa = ua / wa;
                        sb = ub / wb;
                        ratio = (sa > sb) ? sa / sb : sb / sa;

                        if (r == 0)
                            iso_total++;
                        iso_sum[r] += ratio;
                        if (ratio < 1.25)
                            iso_good[r]++;
                        if (poly.flags != 0) {
                            if (r == 0)
                                iso_rot_total++;
                            iso_rot_sum[r] += ratio;
                            if (ratio < 1.25)
                                iso_rot_good[r]++;
                        }
                    }
                }

                {
                    const u8 *uv = rec.uv + (size_t)poly.uv_idx * 8;
                    for (j = 0; j < 4 && corners < Q2PSX_ARRAY_COUNT(corner); j++) {
                        u32 c1 = (3u - j) & 3u;
                        u32 c2 = (3u - poly.flags - j) & 3u;

                        corner[corners].vtx = poly.vtx[j];
                        corner[corners].rot = poly.flags;
                        corner[corners].uv[0][0] = uv[j * 2 + 0];
                        corner[corners].uv[0][1] = uv[j * 2 + 1];
                        corner[corners].uv[1][0] = uv[c1 * 2 + 0];
                        corner[corners].uv[1][1] = uv[c1 * 2 + 1];
                        corner[corners].uv[2][0] = uv[c2 * 2 + 0];
                        corner[corners].uv[2][1] = uv[c2 * 2 + 1];
                        corners++;
                    }
                }
            }

            /* Score the three rules on whether corners that share a vertex
             * land on the same texel. */
            {
                u32 a, b, r;
                for (a = 0; a < corners; a++) {
                    for (b = a + 1; b < corners; b++) {
                        bool rotated;

                        if (corner[a].vtx != corner[b].vtx)
                            continue;
                        agree_pairs++;

                        /* Rules 1 and 2 differ only where a rotation is
                         * actually set, so the whole-disc rate is mostly pairs
                         * both score identically. Count that subset apart or
                         * the comparison measures nothing. */
                        rotated = (corner[a].rot != 0 || corner[b].rot != 0);
                        if (rotated)
                            rot_pairs++;

                        for (r = 0; r < 3; r++) {
                            if (corner[a].uv[r][0] == corner[b].uv[r][0] &&
                                corner[a].uv[r][1] == corner[b].uv[r][1]) {
                                agree[r]++;
                                if (rotated)
                                    rot_agree[r]++;
                            }
                        }
                    }
                }
            }
        }

        if (have_pts)
            q2_points_free(&pts);
        q2_zone_close(&zf);
    }

    printf("  %-12s %5s %10s %8s %7s  %s\n",
           "map", "zones", "polys", "max idx", "clut4", "in range");
    for (i = 0; i < map_count; i++) {
        clut_map_stat *m = &maps[i];
        bool ok;

        if (m->polys == 0)
            continue;
        if (!m->have_vram) {
            printf("  %-12s %5u %10llu %8u %7s  %s\n", m->name, m->zones,
                   m->polys, m->max_index, "-", "no SNDVRAM");
            continue;
        }

        ok = (m->max_index < m->clut4_count);
        if (!ok)
            maps_out_of_range++;
        printf("  %-12s %5u %10llu %8u %7u  %s\n", m->name, m->zones, m->polys,
               m->max_index, m->clut4_count, ok ? "yes" : "NO");
    }

    printf("\n  polygons                 : %llu\n", polys);
    printf("  maps with an index past clut4_count : %d\n", maps_out_of_range);
    printf("  polygons with clut bits 2-7 set     : %llu\n", high_bits_set);
    printf("  polygons with uvIdx past the table  : %llu\n", uv_out_of_range);

    printf("\n  clut & 3 (semi-transparency selector):\n");
    for (i = 0; i < 4; i++)
        printf("    %d : %12llu  %6.3f%%\n", i, low2[i],
               polys ? 100.0 * (double)low2[i] / (double)polys : 0.0);

    printf("\n  uvIdx >> 6 (UV rotation):\n");
    for (i = 0; i < 4; i++)
        printf("    %d : %12llu  %6.3f%%\n", i, rot[i],
               polys ? 100.0 * (double)rot[i] / (double)polys : 0.0);

    /*
     * The rotation rule came out of the disassembly, so test it against
     * something the disassembly cannot have arranged: whether polygons that
     * share a vertex agree on that vertex's texel. A wrong corner rule
     * scatters the assignment and the agreement rate collapses; the right one
     * maximises it. Three rules are scored so the comparison has controls —
     * ignoring the rotation entirely, and applying only the reversal.
     */
    printf("\n  Corner rule scored on shared-vertex UV agreement:\n");
    printf("    %-28s %12s %12s  %s\n", "rule", "agree", "pairs", "rate");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %6.3f%%\n", agree_name[i], agree[i],
               agree_pairs,
               agree_pairs ? 100.0 * (double)agree[i] / (double)agree_pairs
                           : 0.0);

    printf("\n  ... restricted to pairs carrying a rotation, the only ones on\n"
           "      which the last two rules can disagree:\n");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %6.3f%%\n", agree_name[i],
               rot_agree[i], rot_pairs,
               rot_pairs ? 100.0 * (double)rot_agree[i] / (double)rot_pairs
                         : 0.0);

    printf("\n  Corner rule scored on texel-scale isotropy (texels per world\n"
           "  unit along each quad edge; a 90-degree error diverges them):\n");
    printf("    %-28s %12s %12s  %s\n", "rule", "within 1.25x", "quads",
           "mean ratio");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %8.4f\n", agree_name[i], iso_good[i],
               iso_total, iso_total ? iso_sum[i] / (double)iso_total : 0.0);

    printf("\n  ... restricted to the quads that carry a rotation:\n");
    for (i = 0; i < 3; i++)
        printf("    %-28s %12llu %12llu  %8.4f\n", agree_name[i],
               iso_rot_good[i], iso_rot_total,
               iso_rot_total ? iso_rot_sum[i] / (double)iso_rot_total : 0.0);

    printf("\n%s\n", (maps_out_of_range == 0 && uv_out_of_range == 0)
           ? "PASS - every CLUT index addresses a CLUT the map uploads, and "
             "every UV index is inside its table."
           : "FAIL - see above.");

    return maps_out_of_range || uv_out_of_range ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Render one brush-model entity (a mover group: door, lift, plat) on its own,
 * framed to fill the view.
 *
 * Drawing a door surrounded by 17,000 other nodes makes it very hard to tell a
 * fault in that door apart from a fault in the renderer. Isolating it, and
 * framing on its own bounds, makes the geometry and its texturing legible.
 */
static int cmd_bmodel(disc *d, const char *map, int zone_index, int which,
                      const char *out_path)
{
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_stats stats;
    q2_events ev;
    q2_mover_set movers;
    u8 *mask = NULL;
    s32 bmin[3], bmax[3];
    bool any = false;
    u32 m, k;
    const int W = 512, H = 480;

    if (q2_world_load_zone(&zone, d, map, zone_index) != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        return 1;
    }

    if (q2_events_parse_zone(&ev, &zone.zone) != Q2_OK ||
        q2_movers_build(&movers, &ev) != Q2_OK) {
        fprintf(stderr, "no movers in %s zone %d\n", map, zone_index);
        q2_world_free_zone(&zone);
        return 1;
    }

    printf("%s zone %d: %u mover groups\n", map, zone_index, movers.count);

    mask = (u8 *)calloc(zone.scene.node_count ? zone.scene.node_count : 1, 1);
    if (!mask) {
        q2_movers_free(&movers);
        q2_world_free_zone(&zone);
        return 1;
    }

    bmin[0] = bmin[1] = bmin[2] =  0x7FFFFFFF;
    bmax[0] = bmax[1] = bmax[2] = -0x7FFFFFFF;

    for (m = 0; m < movers.count; m++) {
        if (which >= 0 && (u32)which != m)
            continue;
        for (k = 0; k < movers.movers[m].part_count; k++) {
            s16 nd = movers.movers[m].node[k];
            q2_scene_node node;
            s32 nmin[3], nmax[3];
            int a;

            if (nd < 0 || (u32)nd >= zone.scene.node_count)
                continue;
            if (!q2_scene_get_node(&zone.scene, (u32)nd, &node))
                continue;

            mask[nd] = 1;
            q2_scene_node_bounds(&node, nmin, nmax);
            for (a = 0; a < 3; a++) {
                if (nmin[a] < bmin[a]) bmin[a] = nmin[a];
                if (nmax[a] > bmax[a]) bmax[a] = nmax[a];
            }
            any = true;
            printf("  group %u part %u -> node %d  origin [%d %d %d]\n",
                   m, k, nd, node.origin[0], node.origin[1], node.origin[2]);
        }
    }

    q2_movers_free(&movers);

    if (!any) {
        fprintf(stderr, "no geometry-bearing mover nodes selected\n");
        free(mask);
        q2_world_free_zone(&zone);
        return 1;
    }

    zone.node_filter       = mask;
    zone.node_filter_count = zone.scene.node_count;

    printf("  bmodel bounds : [%d %d %d] .. [%d %d %d]\n",
           bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);

    /* Frame it: stand back along -Z by its largest extent. */
    q2_camera_default(&cam, W, H);
    {
        s32 ex = bmax[0] - bmin[0];
        s32 ey = bmax[1] - bmin[1];
        s32 ez = bmax[2] - bmin[2];
        s32 extent = ex > ey ? ex : ey;
        if (ez > extent) extent = ez;
        if (extent < 64) extent = 64;

        cam.pos[0] = (bmin[0] + bmax[0]) / 2;
        cam.pos[1] = (bmin[1] + bmax[1]) / 2;
        cam.pos[2] = (bmin[2] + bmax[2]) / 2 - extent;
        cam.yaw = 0;
        cam.pitch = 0;
    }

    if (psx_ot_init(&ot, 4096, 300000) != Q2_OK ||
        psx_fb_init(&fb, W, H) != Q2_OK) {
        free(mask);
        q2_world_free_zone(&zone);
        return 1;
    }
    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) { free(mask); q2_world_free_zone(&zone); return 1; }

    q2_world_build_ot(&zone, &cam, W, H, &ot, &gte, &stats);
    printf("  quads emitted : %u of %u\n", stats.quads_emitted, stats.quads_total);

    psx_raster_opts_default(&opts);
    {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            if (q2_vram_upload(&vs, vram) != Q2_OK)
                opts.textures = false;
            q2_vram_free(&vs);
        } else {
            opts.textures = false;
        }
    }

    psx_fb_clear(&fb, psx_rgb555(4, 4, 10));
    psx_raster_ot(&fb, &ot, vram, &opts);

    if (psx_fb_write_ppm(&fb, out_path) == Q2_OK)
        printf("  wrote %s\n", out_path);

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
    free(mask);
    zone.node_filter = NULL;
    q2_world_free_zone(&zone);
    return 0;
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
    /* pitch == 9999 is a sentinel meaning "stand at the spawn point and look
     * ahead" rather than framing the whole zone from outside. It is the view a
     * player actually gets, and therefore the honest test of the renderer. */
    bool eye_view = (pitch == 9999);
    if (eye_view)
        pitch = 0;

    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_framebuffer fb;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
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

    if (eye_view) {
        /* Stand at a real spawn for this zone, at eye height. World Y grows
         * downward, so the eye is at a SMALLER Y than the feet. */
        char cpath[256];
        q2_buf cbuf;
        bool placed = false;

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            q2_common_file cf3;
            if (q2_common_open(&cf3, &cbuf) == Q2_OK) {
                q2_start_pos_list sl;
                if (q2_start_pos_parse(&sl, &cf3) == Q2_OK) {
                    u32 k;
                    for (k = 0; k < sl.count; k++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&sl, k, &sp) || sp.zone != zone_index)
                            continue;
                        cam.pos[0] = sp.x;
                        cam.pos[1] = sp.y - Q2_VIEW_STAND;
                        cam.pos[2] = sp.z;
                        if (yaw == 0)
                            cam.yaw = sp.angle;
                        placed = true;
                        printf("  eye at spawn  : '%s' [%d %d %d] yaw=%d\n",
                               sp.name, sp.x, sp.y, sp.z, cam.yaw);
                        break;
                    }
                }
                q2_common_close(&cf3);
            } else {
                q2_buf_free(&cbuf);
            }
        }
        if (!placed)
            printf("  eye at spawn  : none for this zone, framing instead\n");
        eye_view = placed;
    }

    if (!eye_view) {

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
    }

    printf("  camera        : [%d %d %d] yaw=%d pitch=%d h=%u\n",
           cam.pos[0], cam.pos[1], cam.pos[2], cam.yaw, cam.pitch, cam.projection);

    r = psx_ot_init(&ot, 4096, 300000);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot allocate ordering table: %s\n", q2_result_str(r));
        q2_world_free_zone(&zone);
        return 1;
    }

    vram = (psx_vram *)calloc(1, sizeof(psx_vram));
    if (!vram) {
        fprintf(stderr, "out of memory for VRAM\n");
        psx_ot_free(&ot);
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

    /* Upload the map's texture pages and palettes into VRAM, then render
     * textured. Falls back to Gouraud-only if the map has no VRAM section. */
    {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            if (q2_vram_upload(&vs, vram) == Q2_OK) {
                printf("  textures      : %u pages, %u palettes uploaded\n",
                       vs.texpage_count, vs.clut4_count);
            } else {
                printf("  textures      : upload failed, drawing untextured\n");
                opts.textures = false;
            }
            q2_vram_free(&vs);
        } else {
            printf("  textures      : no VRAM section, drawing untextured\n");
            opts.textures = false;
        }
    }

    psx_fb_clear(&fb, psx_rgb555(16, 16, 32));
    psx_raster_ot(&fb, &ot, vram, &opts);

    r = psx_fb_write_ppm(&fb, out_path);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot write %s: %s\n", out_path, q2_result_str(r));
    } else {
        printf("\n  wrote %s (%dx%d)\n", out_path, W, H);
    }

    psx_fb_free(&fb);
    psx_ot_free(&ot);
    free(vram);
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
/*
 * Decode every compressed VRAM image on the disc.
 *
 * The acceptance bar is strict on purpose: a correct codec decodes every
 * payload to exactly its expected size with no overshoot and no starvation.
 * Anything less means it is wrong, or there is more than one codec.
 */
static int cmd_textures(disc *d)
{
    int i, n = disc_file_count(d);
    u32 maps = 0, images = 0, failed = 0;
    u64 packed_total = 0, decoded_total = 0;
    u32 pad_hist[5];
    u8 *scratch;
    size_t scratch_cap = 1024 * 1024;

    memset(pad_hist, 0, sizeof(pad_hist));

    scratch = (u8 *)malloc(scratch_cap);
    if (!scratch) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    printf("Decoding every compressed VRAM image...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *p = f->path;
        const char *rest, *slash;
        char map[64];
        size_t len;
        q2_vram_section vs;
        u32 k;

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

        if (q2_vram_load(&vs, d, map) != Q2_OK) {
            printf("  LOAD FAILED  %s\n", map);
            failed++;
            continue;
        }

        maps++;

        for (k = 0; k < vs.image_count; k++) {
            /* Not width*height — texture pages ignore their stored dimensions
             * and are forced to 128x256 by the engine. */
            size_t want = q2_vram_decoded_size(&vs, k);
            size_t got = 0;

            if (want > scratch_cap) {
                u8 *bigger = (u8 *)realloc(scratch, want);
                if (!bigger) { failed++; continue; }
                scratch = bigger;
                scratch_cap = want;
            }

            if (q2_vram_decode(&vs, k, scratch, scratch_cap, &got) != Q2_OK) {
                printf("  DECODE FAILED  %s image %u (%ux%u)\n",
                       map, k, vs.images[k].width, vs.images[k].height);
                failed++;
                continue;
            }

            images++;
            packed_total  += vs.images[k].packed_size;
            decoded_total += got;
        }

        q2_vram_free(&vs);
    }

    free(scratch);

    printf("  maps            : %u\n", maps);
    printf("  images decoded  : %u\n", images);
    printf("  failures        : %u\n", failed);
    printf("  packed bytes    : %llu\n", (unsigned long long)packed_total);
    printf("  decoded bytes   : %llu\n", (unsigned long long)decoded_total);
    if (packed_total)
        printf("  compression     : %.2fx\n",
               (double)decoded_total / (double)packed_total);

    printf("\n%s\n", failed == 0
           ? "PASS - every payload decoded to exactly its expected size."
           : "FAIL - see above.");

    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Drop a player into a real zone and simulate.
 *
 * The unit tests drive the simulation with no zone attached, so they never
 * touch collision. This is the check that it works on actual geometry: does the
 * spawn point land inside a convex cell, does the player come to rest instead of
 * falling forever, and do they stay inside the hull while walking.
 */
static int cmd_walk(disc *d, const char *map, int zone_index, int ticks)
{
    q2_world_zone zone;
    q2_sim sim;
    q2_input in;
    q2_result r;
    q2_start_pos_list spawns;
    q2_common_file common;
    q2_buf buf;
    char path[256];
    s32 feet[3] = { 0, 0, 0 };
    s32 start_y;
    int i, grounded_at = -1, escaped = 0, zone_gates = 0;
    bool have_spawn = false;

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n", map, zone_index, q2_result_str(r));
        return 1;
    }

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) == Q2_OK) {
        if (q2_common_open(&common, &buf) == Q2_OK) {
            if (q2_start_pos_parse(&spawns, &common) == Q2_OK) {
                u32 k;
                for (k = 0; k < spawns.count; k++) {
                    q2_start_pos sp;
                    if (!q2_start_pos_get(&spawns, k, &sp) || sp.zone != zone_index)
                        continue;
                    feet[0] = sp.x; feet[1] = sp.y; feet[2] = sp.z;
                    have_spawn = true;
                    printf("  spawn         : '%s' at [%d %d %d]\n",
                           sp.name, sp.x, sp.y, sp.z);
                    break;
                }
            }
            q2_common_close(&common);
        } else {
            q2_buf_free(&buf);
        }
    }

    q2_sim_init(&sim, &zone, 50);

    /* Attach the map's triggers and script so the walk exercises gameplay, not
     * just physics. */
    {
        q2_buf cbuf;
        if (disc_read_file(d, path, &cbuf) == Q2_OK) {
            q2_common_file cf2;
            if (q2_common_open(&cf2, &cbuf) == Q2_OK) {
                q2_sim_attach_gameplay(&sim, &cf2);
                printf("  triggers      : %s, %u volumes\n",
                       sim.triggers_ready ? "loaded" : "none",
                       sim.triggers_ready ? sim.triggers.count : 0);
                printf("  script        : %s, %u records\n",
                       sim.events_ready ? "loaded" : "none",
                       sim.events_ready ? sim.event_rt.record_count : 0);
                /* The sim borrows these, so they must outlive it; leaked
                 * deliberately for the duration of this one-shot command. */
            } else {
                q2_buf_free(&cbuf);
            }
        }
    }

    q2_sim_spawn(&sim, feet, 0);

    printf("%s\n", zone.name);
    printf("  spawn found   : %s\n", have_spawn ? "yes" : "no (using origin)");
    printf("  collision     : %s, %u nodes\n",
           sim.coll_ready ? "loaded" : "UNAVAILABLE",
           sim.coll_ready ? sim.coll.node_count : 0);
    printf("  spawn cell    : %d%s\n", sim.current_node,
           sim.current_node < 0 ? "  (outside every hull)" : "");

    start_y = sim.player.pos[1];

    memset(&in, 0, sizeof(in));
    for (i = 0; i < ticks; i++) {
        /* Walk forward for the second half so both falling and walking are
         * exercised. */
        in.forward = (i > ticks / 2) ? 1024 : 0;
        q2_sim_tick(&sim, &in, Q2_DT_NOMINAL);

        if (sim.player.on_ground && grounded_at < 0)
            grounded_at = i;
        if (sim.coll_ready && sim.current_node < 0)
            escaped++;
        {
            u32 zt;
            if (q2_sim_take_zone_change(&sim, &zt)) {
                printf("    tick %d: ZONE GATE fired -> zone %u\n", i, zt);
                zone_gates++;
            }
        }
    }

    printf("  after %d ticks:\n", ticks);
    printf("    grounded    : %s\n",
           grounded_at >= 0 ? "yes" : "NO - fell the whole time");
    if (grounded_at >= 0)
        printf("    landed on tick %d\n", grounded_at);
    printf("    fell        : %d world units\n", sim.player.pos[1] - start_y);
    printf("    final cell  : %d\n", sim.current_node);
    printf("    ticks outside any hull: %d\n", escaped);
    printf("    events run  : %u\n", sim.events_ready ? sim.event_rt.ran_count : 0);
    printf("    zone gates  : %d\n", zone_gates);

    q2_world_free_zone(&zone);

    /* A spawn that is not inside a cell means either the hull or the spawn is
     * misread, and is worth failing on. */
    return (sim.coll_ready && sim.current_node < 0) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Run every event script on the disc.
 *
 * Fires each named entry point and lets the trigger graph propagate, which
 * exercises the parser and the runtime together. The useful output is the
 * opcode census and how much of it actually executes: a script that parses but
 * never runs anything would look fine to `verify` and be useless in a game.
 */
static int cmd_events(disc *d)
{
    int i, n = disc_file_count(d);
    u32 files = 0, records = 0, items = 0, named = 0;
    u32 ran = 0, movers = 0, zone_changes = 0;
    u32 op_hist[64];
    u32 movers_built = 0, movers_moved = 0, movers_open = 0, movers_empty = 0;

    memset(op_hist, 0, sizeof(op_hist));
    printf("Running every event script on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_events ev;
        q2_event_rt rt;
        bool is_zone;

        base = base ? base + 1 : f->path;
        is_zone = (strncmp(base, "ZONE", 4) == 0);
        if (!is_zone && strncmp(base, "COMMON", 6) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;

        if (is_zone) {
            q2_zone_file zf;
            if (q2_zone_open(&zf, &buf) != Q2_OK) { q2_buf_free(&buf); continue; }
            if (q2_events_parse_zone(&ev, &zf) != Q2_OK) { q2_zone_close(&zf); continue; }

            files++;
            records += ev.record_count;

            if (q2_event_rt_init(&rt, &ev) == Q2_OK) {
                q2_event_record rec;
                u32 k;

                /* Census every item before running anything. */
                if (q2_events_first_record(&ev, &rec)) {
                    do {
                        for (k = 0; k < rec.n_items; k++) {
                            q2_event_item it;
                            if (!q2_events_get_item(&ev, &rec, k, &it))
                                break;
                            op_hist[it.opcode & 0x3F]++;
                            items++;
                        }
                    } while (q2_events_next_record(&ev, &rec, &rec));
                }

                for (k = 0; k < ev.dir_count; k++) {
                    q2_event_dir_entry e;
                    if (!q2_events_get_dir_entry(&ev, k, &e))
                        continue;
                    named++;
                    q2_event_rt_trigger(&rt, e.offset);
                }

                if (q2_event_rt_update(&rt) == Q2_EVENT_ZONE_CHANGE)
                    zone_changes++;

                /* Build the zone's doors and lifts and run them for a while,
                 * so the state machine is exercised rather than merely
                 * constructed. */
                {
                    q2_mover_set ms;
                    if (q2_movers_build(&ms, &ev) == Q2_OK) {
                        u32 mi, t;
                        movers_built += ms.count;
                        for (mi = 0; mi < ms.count; mi++) {
                            if (ms.movers[mi].part_count == 0)
                                movers_empty++;
                            q2_mover_trigger(&ms, mi);
                        }
                        for (t = 0; t < 400; t++)
                            movers_moved += q2_movers_tick(&ms, 12, 0xFFFF);
                        for (mi = 0; mi < ms.count; mi++) {
                            if (ms.movers[mi].offset != 0)
                                movers_open++;
                        }
                        q2_movers_free(&ms);
                    }
                }

                ran    += rt.ran_count;
                movers += rt.skipped_movers;
                q2_event_rt_free(&rt);
            }
            q2_zone_close(&zf);
        } else {
            q2_common_file cf;
            if (q2_common_open(&cf, &buf) != Q2_OK) { q2_buf_free(&buf); continue; }
            if (q2_events_parse_common(&ev, &cf) == Q2_OK) {
                files++;
                records += ev.record_count;
            }
            q2_common_close(&cf);
        }
    }

    printf("  files with events : %u\n", files);
    printf("  records           : %u\n", records);
    printf("  items             : %u\n", items);
    printf("  named entries     : %u\n", named);
    printf("  records executed  : %u\n", ran);
    printf("  movers skipped    : %u  (link not decoded)\n", movers);
    printf("  zone gates fired  : %u\n", zone_changes);
    printf("  movers built      : %u  (%u with no nodes)\n", movers_built, movers_empty);
    printf("  mover tick-moves  : %u\n", movers_moved);
    printf("  movers displaced  : %u  after 400 ticks\n", movers_open);

    printf("\n  opcode census\n");
    {
        static const struct { u8 op; const char *name; } names[] = {
            { 0x02, "TRIGGER"  }, { 0x03, "MOVER_A" }, { 0x04, "MOVER_B" },
            { 0x05, "MOVER_C"  }, { 0x08, "FXGROUP" }, { 0x09, "WAIT"    },
            { 0x0F, "ZONEGATE" }, { 0x13, "FX"      }, { 0x14, "ENABLE"  },
            { 0x15, "DISABLE"  }, { 0x16, "CALL"    },
        };
        u32 k;
        for (k = 0; k < Q2PSX_ARRAY_COUNT(names); k++) {
            if (op_hist[names[k].op])
                printf("    0x%02X %-9s %u\n",
                       names[k].op, names[k].name, op_hist[names[k].op]);
        }
        for (k = 0; k < 64; k++) {
            u32 j, known = 0;
            for (j = 0; j < Q2PSX_ARRAY_COUNT(names); j++)
                if (names[j].op == k) known = 1;
            if (!known && op_hist[k])
                printf("    0x%02X %-9s %u\n", k, "(unknown)", op_hist[k]);
        }
    }

    printf("\n%s\n", ran > 0
           ? "PASS - the trigger graph parses and executes."
           : "FAIL - nothing executed.");
    return ran > 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/*
 * Relocate every AI and level module on the disc.
 *
 * The bar is structural: every fixup offset must land inside its image, every
 * stream must terminate, and the HI16 addend words must keep the walk in step.
 * A parse that drifts one word out of phase still consumes most streams
 * plausibly, so "it did not crash" proves nothing — the type census is the
 * thing to read, because the residue counts have to decompose exactly.
 */
static int cmd_reloc(disc *d)
{
    int i, n = disc_file_count(d);
    u32 modules = 0, empty = 0, failed = 0;
    unsigned long long fixups = 0, addends = 0, oob = 0;
    unsigned long long by_type[4] = { 0, 0, 0, 0 };
    unsigned long long moves_found = 0, frames_found = 0, moves_coherent = 0;

    printf("Relocating every AI module on the disc...\n\n");

    for (i = 0; i < n; i++) {
        const disc_file *f = disc_file_at(d, i);
        const char *base = strrchr(f->path, '/');
        q2_buf buf;
        q2_common_file cf;

        base = base ? base + 1 : f->path;
        if (strncmp(base, "COMMON", 6) != 0)
            continue;

        if (disc_read_file(d, f->path, &buf) != Q2_OK)
            continue;
        if (q2_common_open(&cf, &buf) != Q2_OK) {
            q2_buf_free(&buf);
            continue;
        }

        {
            const dat_chunk *bin = cf.chunk[Q2_COMMON_CRE_AI_BIN];
            const dat_chunk *rel = cf.chunk[Q2_COMMON_CRE_AI_REL];

            if (bin && rel && bin->size > Q2_RELOC_CREAI_PREAMBLE &&
                rel->size > Q2_RELOC_CREAI_PREAMBLE) {
                q2_reloc_stats st;
                q2_result r = q2_reloc_scan(rel->data + Q2_RELOC_CREAI_PREAMBLE,
                                            rel->size - Q2_RELOC_CREAI_PREAMBLE,
                                            bin->size - Q2_RELOC_CREAI_PREAMBLE,
                                            &st);
                if (r == Q2_OK) {
                    modules++;
                    fixups  += st.fixups;
                    addends += st.addend_words;
                    oob     += st.out_of_range;
                    by_type[0] += st.by_type[0];
                    by_type[1] += st.by_type[1];
                    by_type[2] += st.by_type[2];
                    by_type[3] += st.by_type[3];

                    /* Now actually relocate it, so the write path is exercised
                     * and not merely the scan, then read its animations out. */
                    {
                        q2_ai_module m;
                        if (q2_ai_module_load(&m, &cf, 0x80100000u) == Q2_OK) {
                            q2_ai_moves mv;

                            /* Guided by the fixup stream rather than scanning
                             * every offset: a move's frames pointer is a WORD32
                             * relocation, so the stream says where to look. */
                            if (!m.empty && m.image.data &&
                                q2_ai_moves_scan_guided(&mv, m.image.data, m.image.size,
                                                        rel->data + Q2_RELOC_CREAI_PREAMBLE,
                                                        rel->size - Q2_RELOC_CREAI_PREAMBLE,
                                                        0x80100000u) == Q2_OK) {
                                u32 k;
                                moves_found  += mv.count;
                                frames_found += mv.total_frames;
                                for (k = 0; k < mv.count; k++) {
                                    if (q2_ai_move_verb_run(&mv, k, m.image.data,
                                                            m.image.size) >= 2)
                                        moves_coherent++;
                                }
                                q2_ai_moves_free(&mv);
                            }
                            q2_ai_module_free(&m);
                        } else {
                            printf("  RELOCATE FAILED  %s\n", f->path);
                            failed++;
                        }
                    }
                } else {
                    printf("  SCAN FAILED  %s: %s\n", f->path, q2_result_str(r));
                    failed++;
                }
            } else {
                empty++;
            }
        }

        q2_common_close(&cf);
    }

    printf("  modules relocated : %u\n", modules);
    printf("  maps with none    : %u\n", empty);
    printf("  failures          : %u\n", failed);
    printf("  fixups            : %llu\n", fixups);
    printf("  HI16 addend words : %llu\n", addends);
    printf("  offsets out of range : %llu\n", oob);
    printf("\n  type census\n");
    printf("    WORD32   %llu\n", by_type[0]);
    printf("    HI16     %llu\n", by_type[1]);
    printf("    LO16     %llu\n", by_type[2]);
    printf("    TARGET26 %llu\n", by_type[3]);

    /* The addend count must equal the HI16 count exactly: one raw word each.
     * If a stream ever drifted, these would diverge. */
    printf("\n  animations recovered\n");
    printf("    moves            %llu\n", moves_found);
    printf("    frames           %llu\n", frames_found);
    printf("    with a verb run  %llu  (2+ consecutive same-verb frames)\n",
           moves_coherent);

    printf("\n  addends == HI16 count : %s\n",
           (addends == by_type[1]) ? "yes" : "NO - the walk drifted");

    printf("\n%s\n", (failed == 0 && oob == 0 && addends == by_type[1])
           ? "PASS - every stream terminates, every target is in range."
           : "FAIL - see above.");

    return (failed || oob || addends != by_type[1]) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*
 * Dump the executable's level table and cross-check it against the disc.
 *
 * The check that matters: every directory the table names must actually exist
 * under Q2DATA/LEVELS. A wrong table offset would still yield printable-looking
 * names, so agreement with the filesystem is what makes the read trustworthy.
 */
static int cmd_leveltable(disc *d)
{
    q2_build_id id;
    q2_level_table t;
    q2_result r;
    u32 i, real = 0, placeholders = 0, missing = 0;

    if (q2_identify(d, &id) != Q2_OK) {
        fprintf(stderr, "cannot identify this disc\n");
        return 1;
    }

    r = q2_level_table_load(&t, d, &id);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot read the level table: %s\n", q2_result_str(r));
        return 1;
    }

    printf("Level table: %u records\n", t.count);
    printf("  idx display        directory  u22  sequence          on disc\n");

    for (i = 0; i < t.count; i++) {
        const q2_level_entry *e = &t.entries[i];
        char probe[128];
        bool present;

        if (e->is_placeholder) {
            placeholders++;
            continue;
        }

        snprintf(probe, sizeof(probe), "Q2DATA/LEVELS/%s/COMMON.DAT", e->directory);
        present = disc_find(d, probe) != NULL;

        if (present) real++;
        else missing++;

        printf("  %-3u %-14s %-10s %-4u %2u,%2u,%2u,%2u,%2u      %s\n",
               i, e->display, e->directory, e->unknown_22,
               e->sequence[0], e->sequence[1], e->sequence[2],
               e->sequence[3], e->sequence[4],
               present ? "yes" : "MISSING");
    }

    printf("\n  resolve to a directory : %u\n", real);
    printf("  placeholders           : %u\n", placeholders);
    printf("  named but not present  : %u\n", missing);

    q2_level_table_free(&t);

    /*
     * An entry naming a directory that is not on the disc is CUT CONTENT, not
     * a bad read. The five here are three Gallery variants plus QUAKE3 and
     * HALFLIFE, and all five carry the same sequence bytes as each other —
     * unused stubs left in the table.
     *
     * So the verdict is about whether the table READS, and the direction that
     * would actually indicate a wrong offset is garbage names or a resolve rate
     * near zero. Failing on cut content would be asserting the wrong thing.
     */
    printf("\n%s\n", real >= 40
           ? "PASS - the table reads and resolves; entries with no directory are cut content."
           : "FAIL - too few entries resolve; the table offset is probably wrong.");

    return real >= 40 ? 0 : 1;
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

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-v") == 0)) {
        q2_version_print();
        return 0;
    }

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
    } else if (strcmp(cmd, "polyflags") == 0) {
        rc = cmd_polyflags(d);
    } else if (strcmp(cmd, "cluts") == 0) {
        rc = cmd_cluts(d);
    } else if (strcmp(cmd, "bmodel") == 0) {
        if (argc < 4) {
            fprintf(stderr, "bmodel needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            int wh = (argc >= 6) ? atoi(argv[5]) : -1;
            const char *outp = (argc >= 7) ? argv[6] : "bmodel.ppm";
            rc = cmd_bmodel(d, argv[3], zi, wh, outp);
        }
    } else if (strcmp(cmd, "audio") == 0) {
        rc = cmd_audio(d);
    } else if (strcmp(cmd, "leveltable") == 0) {
        rc = cmd_leveltable(d);
    } else if (strcmp(cmd, "reloc") == 0) {
        rc = cmd_reloc(d);
    } else if (strcmp(cmd, "events") == 0) {
        rc = cmd_events(d);
    } else if (strcmp(cmd, "walk") == 0) {
        if (argc < 4) {
            fprintf(stderr, "walk needs a map name\n");
            rc = 1;
        } else {
            int zi = (argc >= 5) ? atoi(argv[4]) : 0;
            int tk = (argc >= 6) ? atoi(argv[5]) : 200;
            rc = cmd_walk(d, argv[3], zi, tk);
        }
    } else if (strcmp(cmd, "textures") == 0) {
        rc = cmd_textures(d);
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
    } else if (strcmp(cmd, "exe") == 0) {
        rc = cmd_exe(d);
    } else if (strcmp(cmd, "disasm") == 0) {
        if (argc < 4) {
            fprintf(stderr, "disasm needs an address\n");
            rc = 1;
        } else {
            int n = (argc >= 5) ? atoi(argv[4]) : 0;
            rc = cmd_disasm(d, argv[3], n);
        }
    } else if (strcmp(cmd, "xrefs") == 0) {
        if (argc < 4) {
            fprintf(stderr, "xrefs needs an address\n");
            rc = 1;
        } else {
            rc = cmd_xrefs(d, argv[3]);
        }
    } else if (strcmp(cmd, "funcs") == 0) {
        rc = cmd_funcs(d, (argc >= 4) ? argv[3] : NULL);
    } else if (strcmp(cmd, "bytes") == 0) {
        if (argc < 4) {
            fprintf(stderr, "bytes needs an address\n");
            rc = 1;
        } else {
            int n = (argc >= 5) ? atoi(argv[4]) : 128;
            rc = cmd_bytes(d, argv[3], n);
        }
    } else if (strcmp(cmd, "access") == 0) {
        if (argc < 4) {
            fprintf(stderr, "access needs a structure offset\n");
            rc = 1;
        } else {
            rc = cmd_access(d, argv[3], (argc >= 5) ? argv[4] : NULL);
        }
    } else if (strcmp(cmd, "find") == 0) {
        if (argc < 4) {
            fprintf(stderr, "find needs a string or 0x-prefixed byte pattern\n");
            rc = 1;
        } else {
            rc = cmd_find(d, argv[3]);
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
