/*
 * cmd_screen.c — the screen reconstruction, checked against the disc.
 *
 * `src/screen/screen.[ch]` claims a great many literal numbers: that the
 * framebuffer is 512 x 248, that the ordering table has 217 entries carved into
 * four 51-entry viewport slices and an eleven-entry overlay slice, that the
 * quad split puts its top-right viewport at x = 257, that the vertical split's
 * projection distance is 175 while the horizontal split's is 160. Every one of
 * those came out of a single MIPS instruction in the boot executable.
 *
 * So rather than assert them in a comment, this reads that instruction back off
 * a real disc and compares its immediate field. Nothing here needs a
 * disassembler: each check names an address whose instruction is known to be an
 * `addiu rt, rs, imm` (or one `slti`), and the immediate is the low sixteen
 * bits, sign-extended. If a transcription is wrong, or a different build moves
 * a function, the command fails and says which number disagreed.
 *
 * The negative results are checked too, because they are the load-bearing part
 * of two answered questions:
 *
 *   - the 512 x 256 display envs at 0x8006DFF8 are followed four instructions
 *     later by a call to the 512 x 248 setup that overwrites them, which is why
 *     the game never displays 512 x 256 (openquestions #19);
 *   - the `VSync(3)` at 0x80069188 sits between `CdlSetmode(0x80)` and the
 *     drive bring-up that follows it, so it is a settling delay and not a frame
 *     rate (openquestions #20).
 */
#include "cmd_screen.h"

#include "entity.h"
#include "exe.h"
#include "screen.h"
#include "vram.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
typedef struct check {
    u32         addr;      /* the instruction the constant lives in           */
    s32         expect;    /* what src/screen says it is                      */
    const char *what;
} check;

/* The immediate of an I-type instruction, sign-extended as the hardware does. */
static bool imm_at(const q2_exe *e, u32 addr, s32 *out)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;

    *out = (s32)(s16)(u16)(word & 0xFFFFu);
    return true;
}

static int g_total;

static int run_checks(const q2_exe *e, const char *title,
                      const check *list, size_t n, int *failed)
{
    size_t i;
    int bad = 0;

    g_total += (int)n;

    printf("\n%s\n", title);

    for (i = 0; i < n; i++) {
        s32 got;

        if (!imm_at(e, list[i].addr, &got)) {
            printf("  %08X  %-44s  UNMAPPED\n", list[i].addr, list[i].what);
            bad++;
            continue;
        }

        if (got == list[i].expect) {
            printf("  %08X  %-44s  %6d  ok\n",
                   list[i].addr, list[i].what, got);
        } else {
            printf("  %08X  %-44s  %6d  MISMATCH (port says %d)\n",
                   list[i].addr, list[i].what, got, list[i].expect);
            bad++;
        }
    }

    *failed += bad;
    return bad;
}

/* ------------------------------------------------------------------------- */
/* A `jal` to a known target, encoded as the hardware encodes it. Used for the  */
/* one structural claim that is about control flow rather than a constant.     */
/* ------------------------------------------------------------------------- */
static bool jal_targets(const q2_exe *e, u32 addr, u32 target)
{
    u32 word;

    if (!q2_exe_u32(e, addr, &word))
        return false;
    if ((word >> 26) != 0x03u)          /* jal */
        return false;

    return ((word & 0x03FFFFFFu) << 2) == (q2_exe_norm(target) & 0x0FFFFFFFu);
}

/* ------------------------------------------------------------------------- */
static void print_view(const char *tag, const q2_screen_view *v)
{
    printf("  %-8s  %4d,%-4d %4dx%-4d  ofs %4d,%-4d  proj %4d  far %5d"
           "  aspect %5d  depth %5d\n",
           tag, v->x, v->y, v->w, v->h, v->ofs_x, v->ofs_y,
           v->proj, v->far_z, v->aspect, v->depth_scale);
}

static void print_layout(q2_screen *s, q2_screen_layout l, int players)
{
    int i;
    char tag[16];

    q2_screen_set_layout(s, l, players);

    printf("\n%s (%d viewport%s)\n",
           q2_screen_layout_name(l), s->view_count,
           s->view_count == 1 ? "" : "s");

    for (i = 0; i < s->view_count; i++) {
        snprintf(tag, sizeof(tag), "view %d", i);
        print_view(tag, &s->view[i]);
    }

    printf("  buffers   VRAM (%d,%d) and (%d,%d)%s\n",
           s->disp.buf[0].x, s->disp.buf[0].y,
           s->disp.buf[1].x, s->disp.buf[1].y,
           (s->disp.buf[0].x == s->disp.buf[1].x &&
            s->disp.buf[0].y == s->disp.buf[1].y) ? "  -- single buffered" : "");

    for (i = 0; i < s->view_count; i++) {
        printf("  view %d    ordering table [%3u..%3u], draw env at %3u\n", i,
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + i * Q2_SCREEN_OT_VIEW_STRIDE),
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + (i + 1) * Q2_SCREEN_OT_VIEW_STRIDE - 1),
               (unsigned)(Q2_SCREEN_OT_VIEW_BASE + i * Q2_SCREEN_OT_VIEW_STRIDE
                          + Q2_SCREEN_OT_VIEW_ENV));
    }
}

/* ------------------------------------------------------------------------- */
/* Composing one real frame                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The same sequence the client runs and the same sequence 0x800182C8 runs:
 * swap, one background clear, each viewport into its own slice with its own
 * projection distance, then one walk of the table. Every viewport gets the same
 * camera because there is only one of them to place — what this renders is the
 * screen, not a multiplayer session.
 */
static int render_frame(disc *d, q2_screen *s, const char *map, int zone_index,
                        const char *out)
{
    q2_world_zone zone;
    q2_camera cam;
    psx_ot ot;
    gte_state gte;
    psx_raster_opts opts;
    psx_vram *vram = NULL;
    q2_world_stats stats;
    q2_result r;
    int p;

    r = q2_world_load_zone(&zone, d, map, zone_index);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load %s zone %d: %s\n",
                map, zone_index, q2_result_str(r));
        return 1;
    }

    q2_camera_default(&cam, s->disp.width, s->disp.height);
    psx_raster_opts_default(&opts);
    psx_ot_init(&ot, Q2_SCREEN_OT_ENTRIES, 300000);
    memset(&gte, 0, sizeof(gte));

    /* Stand at a spawn for this zone, eye height above the feet. */
    {
        char cpath[256];
        q2_buf cbuf;
        bool placed = false;

        snprintf(cpath, sizeof(cpath), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
        if (disc_read_file(d, cpath, &cbuf) == Q2_OK) {
            q2_common_file cf;
            if (q2_common_open(&cf, &cbuf) == Q2_OK) {
                q2_start_pos_list sl;
                if (q2_start_pos_parse(&sl, &cf) == Q2_OK) {
                    u32 k;
                    for (k = 0; k < sl.count; k++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&sl, k, &sp) || sp.zone != zone_index)
                            continue;
                        cam.pos[0] = sp.x;
                        cam.pos[1] = sp.y - 576;   /* Q2PSX_VIEW_STAND */
                        cam.pos[2] = sp.z;
                        cam.yaw    = sp.angle;
                        placed = true;
                        printf("  eye at spawn '%s'\n", sp.name);
                        break;
                    }
                }
                q2_common_close(&cf);
            } else {
                q2_buf_free(&cbuf);
            }
        }
        if (!placed) {
            s32 wmin[3], wmax[3];
            q2_world_bounds(&zone, wmin, wmax);
            cam.pos[0] = (wmin[0] + wmax[0]) / 2;
            cam.pos[1] = (wmin[1] + wmax[1]) / 2;
            cam.pos[2] = (wmin[2] + wmax[2]) / 2;
            printf("  no spawn in this zone; standing at its centre\n");
        }
    }

    vram = (psx_vram *)calloc(1, sizeof(*vram));
    if (vram) {
        q2_vram_section vs;
        if (q2_vram_load(&vs, d, map) == Q2_OK) {
            opts.textures = (q2_vram_upload(&vs, vram) == Q2_OK);
            q2_vram_free(&vs);
        } else {
            opts.textures = false;
        }
    }

    q2_screen_frame_begin(s, &ot);

    s->disp.bg_rgb[0] = 16;
    s->disp.bg_rgb[1] = 16;
    s->disp.bg_rgb[2] = 32;
    s->disp.bg_enable = 1;
    q2_screen_background(s);

    for (p = 0; p < s->view_count; p++) {
        const q2_screen_view *v = &s->view[p];
        q2_screen_view_begin(s, p, &ot, &gte);
        cam.projection = (u16)v->proj;
        q2_world_build_ot(&zone, &cam, v->w, v->h, &ot, &gte, &stats);
        printf("  viewport %d  %4d,%-4d %3dx%-3d  proj %3d  %u quads\n",
               p, v->x, v->y, v->w, v->h, v->proj, stats.quads_emitted);
    }

    q2_screen_compose(s, &ot, vram, &opts);
    q2_screen_present(s);

    r = psx_fb_write_ppm(q2_screen_front(s), out);
    if (r == Q2_OK)
        printf("  wrote %s (%d x %d)\n", out,
               s->disp.width, s->disp.height);
    else
        fprintf(stderr, "cannot write %s: %s\n", out, q2_result_str(r));

    free(vram);
    psx_ot_free(&ot);
    q2_world_free_zone(&zone);
    return (r == Q2_OK) ? 0 : 1;
}

static bool layout_by_name(const char *name, q2_screen_layout *out, int *players)
{
    int i;

    *players = 4;
    for (i = 0; i < Q2_SCREEN_LAYOUT_COUNT; i++) {
        if (strcmp(name, q2_screen_layout_name((q2_screen_layout)i)) == 0) {
            *out = (q2_screen_layout)i;
            return true;
        }
    }
    if (strcmp(name, "three") == 0) {
        *out = Q2_SCREEN_LAYOUT_QUAD;
        *players = 3;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
int cmd_screen(disc *d, const char *out, const char *layout_name,
               const char *map, int zone_index)
{
    q2_exe e;
    q2_screen s;
    int failed = 0;
    q2_result r;

    static const check display[] = {
        { 0x800764E0u,   1, "SetVideoMode argument (1 == MODE_PAL)"      },
        { 0x800764F0u, 512, "framebuffer width -> 0x800B2DA0"            },
        { 0x800764FCu, 248, "framebuffer height -> 0x800B2DA2"           },
        { 0x800765E0u, 14128, "double-buffer block base (0x800B3730)"    },
        { 0x8006E190u, 32408, "double-buffer block stride"               },
    };

    static const check envs[] = {
        { 0x800769DCu,   436, "background draw env, offset in the block"  },
        { 0x80076BD8u,   528, "viewport draw envs, first of four"         },
        { 0x8007738Cu,   896, "overlay draw env"                          },
        { 0x800185B0u, 10940, "display env"                               },
        { 0x8001839Cu, 10984, "ordering table"                            },
    };

    static const check table[] = {
        { 0x80018394u,   217, "ordering table entries"                    },
        { 0x80076B10u, 10992, "first viewport slice, offset in the block" },
        { 0x8006E18Cu,    51, "viewport slice length"                     },
        { 0x80077274u, 11808, "overlay slice, offset in the block"        },
    };

    static const check lock[] = {
        { 0x80018974u,  2, "VSync divisor, the frame lock"                },
        { 0x800184B8u, 31, "dt clamp test"                                },
        { 0x800184C0u, 30, "dt clamp value"                               },
        { 0x8006918Cu,  3, "VSync(3): CD bring-up settle, not a frame rate" },
    };

    static const check l_one[] = {
        { 0x80077D2Cu,    1, "viewports"                                  },
        { 0x80077E50u,  160, "projection distance"                        },
        { 0x80077E58u, 6400, "far distance"                               },
        { 0x80077D98u, 8192, "depth scale"                                },
        { 0x80077E60u,   93, "pad a"                                      },
        { 0x80077E6Cu,  201, "pad b"                                      },
    };

    static const check l_full[] = {
        { 0x80077660u,    1, "viewports"                                  },
        { 0x8007766Cu,  320, "projection distance"                        },
        { 0x80077674u, 6400, "far distance"                               },
        { 0x800775C4u, 8192, "depth scale"                                },
        { 0x8007767Cu,   93, "pad a"                                      },
        { 0x80077688u,  201, "pad b"                                      },
    };

    static const check l_two_h[] = {
        { 0x80077954u,    2, "viewports"                                  },
        { 0x80077948u,  160, "projection distance, and the 2D height"     },
        { 0x800779BCu,  320, "2D width"                                   },
        { 0x80077A20u, 6400, "far distance"                               },
        { 0x80077A68u, 6144, "depth scale"                                },
        { 0x80077A70u,   95, "pad b"                                      },
        { 0x80077ACCu,  121, "second viewport y"                          },
    };

    static const check l_two_v[] = {
        { 0x80077B3Cu,    2, "viewports"                                  },
        { 0x80077C18u,  175, "projection distance"                        },
        { 0x80077C20u, 6400, "far distance"                               },
        { 0x80077C64u, 6144, "depth scale"                                },
        { 0x80077C9Cu,  -32, "pad b, as framebuffer height minus 32"      },
        { 0x80077CECu,  256, "second viewport x"                          },
    };

    static const check l_quad[] = {
        { 0x8007776Cu,    4, "viewports"                                  },
        { 0x80077794u,  256, "viewport width"                             },
        { 0x8007779Cu,  123, "viewport height"                            },
        { 0x80077810u,  160, "projection distance"                        },
        { 0x80077818u, 4000, "far distance"                               },
        { 0x8007786Cu, 6144, "depth scale"                                },
        { 0x800778BCu,    1, "the one-pixel inset"                        },
        { 0x800778C8u,  257, "right column x"                             },
        { 0x800778D8u,  124, "bottom row y"                               },
        { 0x8007773Cu, 3920, "the view array, five records of 784"        },
        { 0x800778B0u,  784, "view record stride"                         },
    };

    static const check l_overlay[] = {
        { 0x80077F8Cu,  320, "projection distance"                        },
        { 0x80077EF4u, 8192, "depth scale"                                },
    };

    if (q2_screen_init(&s, Q2_VIDEO_PAL) != Q2_OK) {
        fprintf(stderr, "cannot bring the screen up\n");
        return 1;
    }

    /* The rendering mode does not need the executable — it needs a level. */
    if (out) {
        q2_screen_layout l = Q2_SCREEN_LAYOUT_ONE;
        int players = 1;

        if (layout_name && !layout_by_name(layout_name, &l, &players)) {
            fprintf(stderr, "unknown layout '%s'; try one, two-horizontal, "
                            "two-vertical, three, quad, full-single\n", layout_name);
            q2_screen_free(&s);
            return 1;
        }

        q2_screen_set_layout(&s, l, players);
        printf("screen: %s, %d viewport%s, %u x %u\n",
               q2_screen_layout_name(s.layout), s.view_count,
               s.view_count == 1 ? "" : "s", s.disp.width, s.disp.height);

        failed = render_frame(d, &s, map ? map : "BASE0", zone_index, out);
        q2_screen_free(&s);
        return failed;
    }

    r = q2_exe_load(&e, d, NULL);
    if (r != Q2_OK) {
        fprintf(stderr, "cannot load the boot executable: %s\n", q2_result_str(r));
        q2_screen_free(&s);
        return 1;
    }

    printf("screen — %s\n", e.name);
    printf("  %u x %u, %u Hz fields, VSync(%u) -> %u Hz logic\n",
           s.disp.width, s.disp.height, s.disp.field_hz, s.disp.vsync_divisor,
           s.disp.field_hz / (s.disp.vsync_divisor ? s.disp.vsync_divisor : 1));

    run_checks(&e, "display", display, sizeof(display) / sizeof(display[0]), &failed);
    run_checks(&e, "environments, as offsets into one buffer's block",
               envs, sizeof(envs) / sizeof(envs[0]), &failed);
    run_checks(&e, "ordering table", table, sizeof(table) / sizeof(table[0]), &failed);
    run_checks(&e, "frame lock and timing", lock, sizeof(lock) / sizeof(lock[0]), &failed);

    /* 2 + 4*51 + 11 == 217. Worth stating as its own check: it is the reason to
     * believe the slicing is real rather than a plausible reading. */
    printf("\nslice arithmetic\n");
    {
        int total = Q2_SCREEN_OT_VIEW_BASE
                  + Q2_SCREEN_MAX_VIEWS * Q2_SCREEN_OT_VIEW_STRIDE
                  + Q2_SCREEN_OT_OVERLAY_LEN;
        printf("  %d root + env, %d x %d viewport, %d overlay = %d, table is %d  %s\n",
               Q2_SCREEN_OT_VIEW_BASE, Q2_SCREEN_MAX_VIEWS,
               Q2_SCREEN_OT_VIEW_STRIDE, Q2_SCREEN_OT_OVERLAY_LEN,
               total, Q2_SCREEN_OT_ENTRIES,
               total == Q2_SCREEN_OT_ENTRIES ? "ok" : "MISMATCH");
        g_total++;
        if (total != Q2_SCREEN_OT_ENTRIES)
            failed++;
    }

    run_checks(&e, "layout: one",         l_one,     sizeof(l_one) / sizeof(l_one[0]), &failed);
    run_checks(&e, "layout: full-single", l_full,    sizeof(l_full) / sizeof(l_full[0]), &failed);
    run_checks(&e, "layout: two-horizontal", l_two_h, sizeof(l_two_h) / sizeof(l_two_h[0]), &failed);
    run_checks(&e, "layout: two-vertical",   l_two_v, sizeof(l_two_v) / sizeof(l_two_v[0]), &failed);
    run_checks(&e, "layout: quad",        l_quad,    sizeof(l_quad) / sizeof(l_quad[0]), &failed);
    run_checks(&e, "layout: overlay",     l_overlay, sizeof(l_overlay) / sizeof(l_overlay[0]), &failed);

    /* The two answered questions, as structure rather than as constants. */
    printf("\nanswered questions\n");
    {
        bool overwritten = jal_targets(&e, 0x8006E0C8u, 0x80077540u);
        s32  w = 0, h = 0;
        bool got = imm_at(&e, 0x8006E004u, &w) && imm_at(&e, 0x8006E008u, &h);

        printf("  #19  secondary env is %d x %d, and 0x8006E0C8 calls the "
               "512 x 248 setup: %s\n",
               got ? w : -1, got ? h : -1, overwritten ? "yes" : "NO");
        g_total++;
        if (!overwritten || !got || w != 512 || h != 256)
            failed++;

        printf("  #20  VSync(3) at 0x80069188 follows CdlSetmode: %s\n",
               jal_targets(&e, 0x80069180u, 0x800890B4u) ? "yes" : "NO");
        g_total++;
        if (!jal_targets(&e, 0x80069180u, 0x800890B4u))
            failed++;
    }

    print_layout(&s, Q2_SCREEN_LAYOUT_ONE, 1);
    print_layout(&s, Q2_SCREEN_LAYOUT_TWO_H, 2);
    print_layout(&s, Q2_SCREEN_LAYOUT_TWO_V, 2);
    print_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 3);
    print_layout(&s, Q2_SCREEN_LAYOUT_QUAD, 4);
    print_layout(&s, Q2_SCREEN_LAYOUT_FULL_SINGLE, 1);
    printf("\noverlay camera\n");
    print_view("overlay", &s.overlay);

    printf("\n%d of %d checks passed\n", g_total - failed, g_total);

    q2_screen_free(&s);
    q2_exe_free(&e);
    return failed ? 1 : 0;
}
