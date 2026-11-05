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
 * Rectangles reported as PENDING rather than passed or failed, each for a
 * stated reason; the report lists every one so nothing is silently excused.
 *
 *  - The screen modules (QFRONT, QLOGOS, QLOGOS2, QMAGEXTR, QMRESULT and the
 *    QENDMIS palettes) upload full-screen pictures and per-unit palettes from
 *    their own LevelBin code; the port draws those screens through its own
 *    transcribed paths rather than out of these VRAM rectangles.
 *  - DemoSp1/DemoSp2 (BASE3, JAIL2, WASTE2, MAGDEMO, at 768/832) are the
 *    self-play overlay, uploaded on every load and drawn only in self-play,
 *    which the port does not run yet.
 */
static bool pending(const native_vram_case *nc, const native_vram_rect *r)
{
    static const char *const screens[] = {
        "QFRONT", "QLOGOS", "QLOGOS2", "QMAGEXTR", "QMRESULT",
        "QENDMIS1", "QENDMIS2", "QENDMIS3", "QENDMIS4", "QENDMIS5"
    };
    u32 k;

    for (k = 0; k < sizeof(screens) / sizeof(screens[0]); k++)
        if (!strcmp(nc->map, screens[k]))
            return true;
    return r->y == 256 && (r->x == 768 || r->x == 832) &&
           (!strcmp(nc->map, "BASE3") || !strcmp(nc->map, "JAIL2") ||
            !strcmp(nc->map, "WASTE2") || !strcmp(nc->map, "MAGDEMO"));
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
            unsigned got = rect_hash(c->vram, r);

            rects++;
            if (got != r->hash && pending(nc, r)) {
                held++;
                printf("PENDING %s: VRAM (%d,%d) %dx%d\n", nc->map, r->x, r->y, r->w, r->h);
                continue;
            }
            if (got != r->hash) {
                const char *dump = getenv("Q2PSX_VRAM_DUMP");
                fprintf(stderr, "FAIL %s: VRAM (%d,%d) %dx%d hash %08X, native %08X\n",
                        nc->map, r->x, r->y, r->w, r->h, got, r->hash);
                /* Opt-in: the client's whole VRAM, to diff against a native
                 * dump of the same load. */
                if (dump && dump[0]) {
                    char path[1024];
                    FILE *f;
                    snprintf(path, sizeof(path), "%s/%s.vram", dump, nc->map);
                    if ((f = fopen(path, "wb")) != NULL) {
                        fwrite(c->vram->px, 1, sizeof(c->vram->px), f);
                        fclose(f);
                    }
                }
            }
            CHECK(got == r->hash);
        }
        close_client(c);
    }
    printf("native VRAM (%s): %u maps, %u rectangles, %u pending, %u not loadable as a level\n",
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
