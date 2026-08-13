/*
 * main.c — the playable client.
 *
 * Opens a disc, loads a zone, and lets you fly through it in real time. The
 * internal framebuffer is the console's own resolution — 320x240 on an NTSC
 * build, 320x256 on PAL — and is upscaled to the window. That is not a
 * concession to performance; rendering at the original resolution is part of
 * looking right, because the dither pattern and the vertex snapping are both
 * defined in terms of real pixels.
 *
 * Controls
 *   W/A/S/D      move
 *   Q/E          down / up
 *   arrows       look
 *   shift        move faster
 *   1..9, 0      switch zone
 *   F1           toggle dithering
 *   F2           toggle affine UVs (perspective-correct comparison)
 *   F3           toggle the ordering-table sort
 *   Esc          quit
 */
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "entity.h"
#include "ident.h"
#include "q2psx.h"
#include "raster.h"
#include "trig.h"
#include "world.h"

typedef struct client {
    disc            *disc;
    q2_build_id      build;
    q2_world_zone    zone;
    char             map[64];
    int              zone_index;

    q2_camera        cam;
    psx_ot           ot;
    gte_state        gte;
    psx_framebuffer  fb;
    psx_raster_opts  opts;

    SDL_Window      *window;
    SDL_Renderer    *renderer;
    SDL_Texture     *texture;

    int              width, height;
    bool             running;
} client;

/* ------------------------------------------------------------------------- */
static bool client_load_zone(client *c, const char *map, int index)
{
    q2_world_zone loaded;
    s32 wmin[3], wmax[3];
    bool placed = false;

    if (q2_world_load_zone(&loaded, c->disc, map, index) != Q2_OK) {
        Q2_WARN("no zone %d in %s", index, map);
        return false;
    }

    q2_world_free_zone(&c->zone);
    c->zone = loaded;
    c->zone_index = index;
    snprintf(c->map, sizeof(c->map), "%s", map);

    /*
     * Prefer a real spawn point in this zone. StartPos records carry the zone
     * they belong to, so a map's spawns are not all valid here — filtering by
     * zone is the difference between starting in the level and starting inside
     * a wall somewhere else.
     */
    {
        char path[256];
        q2_buf buf;

        snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);

        if (disc_read_file(c->disc, path, &buf) == Q2_OK) {
            q2_common_file common;

            if (q2_common_open(&common, &buf) == Q2_OK) {
                q2_start_pos_list spawns;

                if (q2_start_pos_parse(&spawns, &common) == Q2_OK) {
                    u32 i;
                    for (i = 0; i < spawns.count; i++) {
                        q2_start_pos sp;
                        if (!q2_start_pos_get(&spawns, i, &sp))
                            continue;
                        if (sp.zone != index)
                            continue;

                        c->cam.pos[0] = sp.x;
                        c->cam.pos[1] = sp.y;
                        c->cam.pos[2] = sp.z;
                        c->cam.yaw    = sp.angle;
                        placed = true;
                        Q2_INFO("spawned at '%s'", sp.name);
                        break;
                    }
                }
                q2_common_close(&common);
            } else {
                q2_buf_free(&buf);
            }
        }
    }

    if (!placed) {
        /* No spawn for this zone — fall back to its centre so there is still
         * something on screen. */
        q2_world_bounds(&c->zone, wmin, wmax);
        c->cam.pos[0] = (wmin[0] + wmax[0]) / 2;
        c->cam.pos[1] = (wmin[1] + wmax[1]) / 2;
        c->cam.pos[2] = (wmin[2] + wmax[2]) / 2;
    }

    Q2_INFO("%s: %u nodes, %u vertices",
            c->zone.name, c->zone.scene.node_count, c->zone.points.count);
    return true;
}

/* ------------------------------------------------------------------------- */
static void client_input(client *c, float dt)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    s32 speed = (s32)(4000.0f * dt);
    s32 turn  = (s32)(1500.0f * dt);
    s32 fwd[3], right[3];
    s32 sy, cy;

    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
        speed *= 4;

    if (keys[SDL_SCANCODE_LEFT])  c->cam.yaw   -= turn;
    if (keys[SDL_SCANCODE_RIGHT]) c->cam.yaw   += turn;
    if (keys[SDL_SCANCODE_UP])    c->cam.pitch -= turn;
    if (keys[SDL_SCANCODE_DOWN])  c->cam.pitch += turn;

    if (c->cam.pitch >  Q2_ANGLE_90) c->cam.pitch =  Q2_ANGLE_90;
    if (c->cam.pitch < -Q2_ANGLE_90) c->cam.pitch = -Q2_ANGLE_90;

    sy = q2_sin12(c->cam.yaw);
    cy = q2_cos12(c->cam.yaw);

    /* Movement stays on the horizontal plane regardless of pitch, which is what
     * a player controller wants and what makes flying around a level bearable. */
    fwd[0]   =  sy; fwd[1]   = 0; fwd[2]   =  cy;
    right[0] =  cy; right[1] = 0; right[2] = -sy;

    if (keys[SDL_SCANCODE_W]) {
        c->cam.pos[0] += (s32)(((s64)fwd[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] += (s32)(((s64)fwd[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_S]) {
        c->cam.pos[0] -= (s32)(((s64)fwd[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] -= (s32)(((s64)fwd[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_D]) {
        c->cam.pos[0] += (s32)(((s64)right[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] += (s32)(((s64)right[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_A]) {
        c->cam.pos[0] -= (s32)(((s64)right[0] * speed) >> Q2_FRAC_12);
        c->cam.pos[2] -= (s32)(((s64)right[2] * speed) >> Q2_FRAC_12);
    }
    if (keys[SDL_SCANCODE_E]) c->cam.pos[1] -= speed;
    if (keys[SDL_SCANCODE_Q]) c->cam.pos[1] += speed;
}

/* ------------------------------------------------------------------------- */
static void client_frame(client *c)
{
    q2_world_stats stats;
    void *pixels;
    int pitch;

    q2_world_build_ot(&c->zone, &c->cam, c->width, c->height,
                      &c->ot, &c->gte, &stats);

    psx_fb_clear(&c->fb, psx_rgb555(16, 16, 32));
    psx_raster_ot(&c->fb, &c->ot, NULL, &c->opts);

    if (SDL_LockTexture(c->texture, NULL, &pixels, &pitch)) {
        int y;
        for (y = 0; y < c->height; y++) {
            memcpy((u8 *)pixels + (size_t)y * pitch,
                   c->fb.px + (size_t)y * c->width,
                   (size_t)c->width * sizeof(u16));
        }
        SDL_UnlockTexture(c->texture);
    }

    SDL_RenderClear(c->renderer);
    SDL_RenderTexture(c->renderer, c->texture, NULL, NULL);
    SDL_RenderPresent(c->renderer);
}

/* ------------------------------------------------------------------------- */
static void usage(void)
{
    printf("q2psx - native Quake II PSX\n\n");
    printf("usage: q2psx --disc <path> [--map NAME] [--zone N] [--scale N]\n\n");
    printf("  --disc   a .cue, .bin, .img or .iso, or a drive letter\n");
    printf("  --map    level directory name (default BASE0)\n");
    printf("  --zone   zone index within the map (default 0)\n");
    printf("  --scale  window scale factor (default 3)\n");
}

int main(int argc, char **argv)
{
    client c;
    const char *disc_path = NULL;
    const char *map = "BASE0";
    int zone_index = 0;
    int scale = 3;
    int i;
    u64 last;

    memset(&c, 0, sizeof(c));

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc)       disc_path = argv[++i];
        else if (!strcmp(argv[i], "--map") && i + 1 < argc)   map = argv[++i];
        else if (!strcmp(argv[i], "--zone") && i + 1 < argc)  zone_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else { usage(); return 1; }
    }

    if (!disc_path) {
        usage();
        return 1;
    }

    if (disc_open(&c.disc, disc_path) != Q2_OK) {
        fprintf(stderr, "cannot open disc '%s'\n", disc_path);
        return 1;
    }

    if (q2_identify(c.disc, &c.build) != Q2_OK) {
        fprintf(stderr, "this does not look like a Quake II PSX disc\n");
        disc_close(c.disc);
        return 1;
    }

    printf("%s (%s, %s)\n",
           c.build.desc ? c.build.desc->name : "uncatalogued build",
           c.build.serial, q2_video_std_str(c.build.video));

    /* The console's own framebuffer size for this build. Everything is rendered
     * here and upscaled; the dither and the vertex snapping are defined in these
     * pixels, so rendering at a higher resolution would change the look. */
    c.width  = 320;
    c.height = (c.build.video == Q2_VIDEO_PAL) ? 256 : 240;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        disc_close(c.disc);
        return 1;
    }

    c.window = SDL_CreateWindow("Q2PSX-PC",
                                c.width * scale, c.height * scale,
                                SDL_WINDOW_RESIZABLE);
    if (!c.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        disc_close(c.disc);
        return 1;
    }

    c.renderer = SDL_CreateRenderer(c.window, NULL);
    c.texture  = SDL_CreateTexture(c.renderer, SDL_PIXELFORMAT_XRGB1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   c.width, c.height);

    /* Nearest-neighbour: the whole point is to show the original's pixels. */
    SDL_SetTextureScaleMode(c.texture, SDL_SCALEMODE_NEAREST);

    psx_ot_init(&c.ot, 4096, 300000);
    psx_fb_init(&c.fb, c.width, c.height);
    psx_raster_opts_default(&c.opts);
    c.opts.textures = false;      /* no texture codec yet */

    q2_camera_default(&c.cam, c.width, c.height);

    if (!client_load_zone(&c, map, zone_index)) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        goto done;
    }

    c.running = true;
    last = SDL_GetTicks();

    while (c.running) {
        SDL_Event ev;
        u64 now = SDL_GetTicks();
        float dt = (float)(now - last) / 1000.0f;
        last = now;

        if (dt > 0.1f)
            dt = 0.1f;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                c.running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: c.running = false; break;
                case SDLK_F1: c.opts.dither    = !c.opts.dither;    break;
                case SDLK_F2: c.opts.affine_uv = !c.opts.affine_uv; break;
                default:
                    if (ev.key.key >= SDLK_0 && ev.key.key <= SDLK_9) {
                        int z = (int)(ev.key.key - SDLK_0);
                        client_load_zone(&c, c.map, z);
                    }
                    break;
                }
            }
        }

        client_input(&c, dt);
        client_frame(&c);
    }

done:
    q2_world_free_zone(&c.zone);
    psx_fb_free(&c.fb);
    psx_ot_free(&c.ot);
    if (c.texture)  SDL_DestroyTexture(c.texture);
    if (c.renderer) SDL_DestroyRenderer(c.renderer);
    if (c.window)   SDL_DestroyWindow(c.window);
    SDL_Quit();
    disc_close(c.disc);
    return 0;
}
