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
 *   F4           toggle simulated movement vs free-fly camera
 *   space/ctrl   jump / crouch (simulated movement only)
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
#include "sim.h"
#include "trig.h"
#include "vram.h"
#include "world.h"
#include "xa.h"

typedef struct client {
    disc            *disc;
    q2_build_id      build;
    q2_world_zone    zone;
    q2_common_file   common;
    char             map[64];
    int              zone_index;

    /* Streamed music. The XA decoder carries per-channel history across
     * sectors, so it must persist for the life of a track. */
    q2_xa_track      music;
    q2_xa_decoder    music_dec;
    u32              music_cursor;
    bool             music_open;
    SDL_AudioStream *audio;

    q2_camera        cam;
    q2_sim           sim;
    bool             sim_enabled;
    psx_ot           ot;
    gte_state        gte;
    psx_framebuffer  fb;
    psx_vram        *vram;
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
                /* The sim borrows the triggers and script out of this file, so
                 * it has to outlive the zone. Release the previous map's copy
                 * and take ownership of this one. */
                q2_common_close(&c->common);
                c->common = common;
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

    /* Upload this map's texture pages and palettes. Each map has its own bank,
     * so this must happen per zone load, not once at startup. */
    if (c->vram) {
        q2_vram_section vs;
        memset(c->vram, 0, sizeof(*c->vram));
        if (q2_vram_load(&vs, c->disc, map) == Q2_OK) {
            c->opts.textures = (q2_vram_upload(&vs, c->vram) == Q2_OK);
            Q2_INFO("textures: %u pages, %u palettes",
                    vs.texpage_count, vs.clut4_count);
            q2_vram_free(&vs);
        } else {
            c->opts.textures = false;
        }
    }

    /* q2_sim_init memsets the struct, so the previous zone's trigger bitmap and
     * event runtime have to be released first or they leak on every zone
     * change -- and zone changes are exactly what the gates now cause. */
    q2_sim_free(&c->sim);
    q2_sim_init(&c->sim, &c->zone, q2_build_tick_rate(&c->build));
    {
        s32 feet[3];
        feet[0] = c->cam.pos[0];
        feet[1] = c->cam.pos[1];
        feet[2] = c->cam.pos[2];
        q2_sim_attach_gameplay(&c->sim, &c->common);
        q2_sim_spawn(&c->sim, feet, c->cam.yaw);
        c->sim.player.ground_y = feet[1];
    }

    Q2_INFO("%s: %u nodes, %u vertices",
            c->zone.name, c->zone.scene.node_count, c->zone.points.count);
    return true;
}

/* ------------------------------------------------------------------------- */
/*
 * Music.
 *
 * The XA streams run at 37800 Hz, which no sound card wants. Rather than write
 * a resampler, hand SDL an audio stream declared as 37800 Hz stereo and let it
 * convert — the conversion is not part of the console's character, so there is
 * nothing to be gained by reproducing it by hand.
 *
 * Sectors are decoded on demand rather than up front: one .XAI channel is
 * several minutes of audio, and the original streamed it off the disc for
 * exactly the same reason.
 */
static bool client_music_start(client *c, char letter, u8 channel)
{
    if (q2_xa_track_open(&c->music, c->disc, letter, channel) != Q2_OK) {
        Q2_WARN("no music track QUAKE_%c channel %u", letter, channel);
        return false;
    }

    q2_xa_decoder_reset(&c->music_dec);
    c->music_cursor = 0;
    c->music_open   = true;

    Q2_INFO("music: QUAKE_%c channel %u", letter, channel);
    return true;
}

static void client_music_pump(client *c)
{
    s16 pcm[XA_FRAMES_PER_SECTOR * 2];
    int queued;

    if (!c->music_open || !c->audio)
        return;

    /* Keep roughly a quarter second buffered. Less and it stutters when a frame
     * runs long; much more and switching zones would keep playing the old
     * track for noticeably too long. */
    queued = SDL_GetAudioStreamQueued(c->audio);
    while (queued < (int)(XA_SAMPLE_RATE / 4 * 2 * (int)sizeof(s16))) {
        u32 n = q2_xa_track_read(&c->music, &c->music_dec, &c->music_cursor,
                                 pcm, (u32)(sizeof(pcm) / sizeof(pcm[0])));
        if (n == 0) {
            /* End of stream: loop, which is what the original did for level
             * music rather than falling silent. */
            c->music_cursor = 0;
            q2_xa_decoder_reset(&c->music_dec);
            break;
        }

        SDL_PutAudioStreamData(c->audio, pcm, (int)(n * sizeof(s16)));
        queued += (int)(n * sizeof(s16));
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Simulated movement: gather the pad state, hand it to the game tick, and read
 * the camera back out of the player. The simulation runs at its own fixed rate
 * regardless of how fast we render, which is the whole point of it owning the
 * clock rather than the frame loop doing so.
 */
static void client_input_simulated(client *c, float dt)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    q2_input in;
    s32 eye[3];

    memset(&in, 0, sizeof(in));

    if (keys[SDL_SCANCODE_W]) in.forward =  1024;
    if (keys[SDL_SCANCODE_S]) in.forward = -1024;
    if (keys[SDL_SCANCODE_D]) in.strafe  =  1024;
    if (keys[SDL_SCANCODE_A]) in.strafe  = -1024;

    /* Turn rate is per second, so scale by the real frame time; the simulation
     * consumes it as a per-tick delta. */
    if (keys[SDL_SCANCODE_LEFT])  in.yaw_delta   -= (s32)(1500.0f * dt);
    if (keys[SDL_SCANCODE_RIGHT]) in.yaw_delta   += (s32)(1500.0f * dt);
    if (keys[SDL_SCANCODE_UP])    in.pitch_delta -= (s32)(1500.0f * dt);
    if (keys[SDL_SCANCODE_DOWN])  in.pitch_delta += (s32)(1500.0f * dt);

    in.jump   = keys[SDL_SCANCODE_SPACE] != 0;
    in.crouch = keys[SDL_SCANCODE_LCTRL] != 0 || keys[SDL_SCANCODE_C] != 0;

    q2_sim_advance(&c->sim, &in, (double)dt);

    q2_sim_eye(&c->sim, eye);
    c->cam.pos[0] = eye[0];
    c->cam.pos[1] = eye[1];
    c->cam.pos[2] = eye[2];
    c->cam.yaw    = c->sim.player.yaw;
    c->cam.pitch  = c->sim.player.pitch;
}

/* Free-fly camera, kept for inspecting geometry without physics in the way. */
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
    psx_raster_ot(&c->fb, &c->ot, c->vram, &c->opts);

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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        disc_close(c.disc);
        return 1;
    }

    /* Declare the stream at the console's own 37800 Hz and let SDL resample to
     * whatever the device wants. */
    {
        SDL_AudioSpec spec;
        spec.format   = SDL_AUDIO_S16LE;
        spec.channels = XA_CHANNELS;
        spec.freq     = XA_SAMPLE_RATE;

        c.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, NULL, NULL);
        if (c.audio)
            SDL_ResumeAudioStreamDevice(c.audio);
        else
            Q2_WARN("no audio device: %s", SDL_GetError());
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
    c.vram = (psx_vram *)calloc(1, sizeof(psx_vram));

    q2_camera_default(&c.cam, c.width, c.height);

    if (!client_load_zone(&c, map, zone_index)) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        goto done;
    }

    /* Level music. Which .XAI channel belongs to which map is part of the EXE's
     * music table and is not decoded yet, so start on the first track rather
     * than guess at a mapping that would be wrong. */
    client_music_start(&c, 'A', 0);

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
                case SDLK_F4:
                    c.sim_enabled = !c.sim_enabled;
                    Q2_INFO("movement: %s", c.sim_enabled ? "simulated" : "free-fly");
                    break;
                default:
                    if (ev.key.key >= SDLK_0 && ev.key.key <= SDLK_9) {
                        int z = (int)(ev.key.key - SDLK_0);
                        client_load_zone(&c, c.map, z);
                    }
                    break;
                }
            }
        }

        if (c.sim_enabled)
            client_input_simulated(&c, dt);
        else
            client_input(&c, dt);

        /* A zone gate fired somewhere in the script. Load the target zone of
         * the same map -- cross-map progression needs the level table, which
         * lives in the executable and is not read yet. */
        {
            u32 target;
            if (q2_sim_take_zone_change(&c.sim, &target)) {
                Q2_INFO("zone gate -> zone %u", target);
                client_load_zone(&c, c.map, (int)target);
            }
        }
        client_music_pump(&c);
        client_frame(&c);
    }

done:
    q2_sim_free(&c.sim);
    q2_common_close(&c.common);
    q2_world_free_zone(&c.zone);
    free(c.vram);
    psx_fb_free(&c.fb);
    psx_ot_free(&c.ot);
    if (c.audio)    SDL_DestroyAudioStream(c.audio);
    if (c.texture)  SDL_DestroyTexture(c.texture);
    if (c.renderer) SDL_DestroyRenderer(c.renderer);
    if (c.window)   SDL_DestroyWindow(c.window);
    SDL_Quit();
    disc_close(c.disc);
    return 0;
}
