/*
 * The client's level VRAM against the console's own load.
 *
 * generate_vram_reference.py runs each level's retail load path on the R3000
 * fixture and records every LoadImage rectangle with an FNV-1a hash of what the
 * load leaves inside it. Here the real client loads the same map's zone 0 —
 * the zone's pages and CLUTs, the UI pages, the HUD and front-end images, all
 * through the paths the game uses — into a fresh client each time, as each
 * native load starts from empty VRAM, and the same rectangles are hashed. A
 * match is texel-for-texel and entry-for-entry equality of everything the
 * loader places, including the CLUT STP fix-up (0x800762B4).
 *
 *     test_vram_native --headless <disc.cue>
 *
 * Without a disc it reports itself skipped and passes.
 */
int q2_test_vram_native_main(int argc, char **argv);
#define main q2_test_vram_native_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} } while (0)
#include "client_fixture.h"

#include "fixtures/vram_reference.h"

static unsigned rect_hash(const psx_vram *v, const native_vram_rect *r)
{
    unsigned value = 2166136261u;
    int x, y;

    for (y = 0; y < r->h; y++)
        for (x = 0; x < r->w; x++) {
            u16 px = v->px[r->y + y][r->x + x];
            value = (value ^ (px & 0xFFu)) * 16777619u;
            value = (value ^ (px >> 8)) * 16777619u;
        }
    return value;
}

/*
 * THE ONE PLACE THE CLIENT IS AHEAD OF THE FIXTURE. A QENDMIS map opens its
 * uplink screen as it loads, and that screen's code (module+0x3ED4, a plain
 * LoadImage) writes the unit's screen tint into palette 0 and the globe's
 * palette into palette 4 (client_uplink_open). The fixture stops at the
 * level loader, before any module code, so the console's VRAM still holds the
 * section's placeholders there (0x8000 after the STP fix-up). Each half is
 * checked against its own source: the two palettes against the module's data,
 * and the rest of the block, with the loader's values put back in those two
 * palettes, against the native hash.
 */
static bool uplink_palettes(const client *c, const native_vram_case *nc,
                            const native_vram_rect *r)
{
    return !strncmp(nc->map, "QENDMIS", 7) && r->x == 0 && r->y == 256 &&
           c->uplink_open;
}

static unsigned uplink_loader_hash(client *c, const native_vram_rect *r)
{
    static psx_vram copy;
    const dat_chunk *lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
    q2_vram_section vs;
    u32 k, p;

    for (k = 0; k < 16; k++) {
        CHECK(c->vram->px[256][k] ==
              q2_rd_u16(lb->data + Q2_UPLINK_CLUT_SCREEN +
                        0x20u * (u32)(c->uplink_unit - 1) + 2u * k));
        CHECK(c->vram->px[257][k] == q2_rd_u16(lb->data + Q2_UPLINK_CLUT_GLOBE + 2u * k));
    }
    copy = *c->vram;
    CHECK(q2_vram_load(&vs, c->disc, c->map) == Q2_OK);
    for (p = 0; p <= 4; p += 4) {
        const u8 *entries = q2_vram_clut(&vs, p);
        u32 x, y;

        CHECK(entries != NULL);
        if (!entries)
            continue;
        q2_vram_clut_pos(p, &x, &y);
        for (k = 0; k < Q2_VRAM_CLUT4_ENTRIES; k++) {
            u16 e = q2_rd_u16(entries + 2u * k);
            copy.px[y][x + k] = e ? (u16)(e | Q2_VRAM_STP_BIT) : 0;
        }
    }
    q2_vram_free(&vs);
    return rect_hash(&copy, r);
}

static void compare_all(const char *disc)
{
    unsigned i, maps = 0, rects = 0, skipped = 0, held = 0;
    client *probe = open_client(disc);
    int ntsc = probe->build.region == Q2_REGION_NTSC_U;

    close_client(probe);
    for (i = 0; i < sizeof(native_vram) / sizeof(native_vram[0]); i++) {
        const native_vram_case *nc = &native_vram[i];
        client *c;
        int k;

        if (nc->ntsc != ntsc)
            continue;
        c = open_client(disc);
        if (!client_load_zone(c, nc->map, 0)) {
            /* A map the client does not load as a level (a film or a
             * results screen it drives another way) has nothing to compare. */
            skipped++;
            close_client(c);
            continue;
        }
        maps++;
        for (k = 0; k < nc->count; k++) {
            const native_vram_rect *r = &nc->rect[k];
            unsigned got;

            if (uplink_palettes(c, nc, r)) {
                got = uplink_loader_hash(c, r);
                held++;
            } else {
                got = rect_hash(c->vram, r);
            }

            rects++;
            if (got != r->hash)
                fprintf(stderr, "FAIL %s: VRAM (%d,%d) %dx%d hash %08X, native %08X\n",
                        nc->map, r->x, r->y, r->w, r->h, got, r->hash);
            CHECK(got == r->hash);
        }
        {
            /* Opt-in: each map's whole VRAM, 1024x512 little-endian
             * halfwords, to diff against or view beside a native dump. */
            const char *dump = getenv("Q2PSX_VRAM_DUMP");

            if (dump && dump[0]) {
                char path[1024];
                FILE *f;

                snprintf(path, sizeof(path), "%s/%s%s.vram", dump, nc->map,
                         ntsc ? "-US" : "");
                if ((f = fopen(path, "wb")) != NULL) {
                    fwrite(c->vram->px, 1, sizeof(c->vram->px), f);
                    fclose(f);
                }
            }
        }
        close_client(c);
    }
    printf("native VRAM (%s): %u maps, %u rectangles (%u with the uplink's own palettes "
           "checked apart), %u not loadable as a level\n",
           ntsc ? "US" : "PAL", maps, rects, held, skipped);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--headless"))
        compare_all(argv[2]);
    else if (argc != 1)
        return 2;
    else
        printf("native VRAM: skipped (pass --headless <disc>)\n");
    printf("native VRAM: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
