/*
 * main.c — the playable client.
 *
 * Opens a disc, loads a zone, and lets you fly through it in real time. The
 * internal framebuffer is the console's own — 512x248, read out of the display
 * init at 0x800764DC rather than assumed — and is upscaled to the window. That
 * is not a concession to performance; rendering at the original resolution is
 * part of looking right, because the dither pattern and the vertex snapping are
 * both defined in terms of real pixels.
 *
 * The frame is put together the way the console puts one together: swap, one
 * background clear, then each viewport into its own slice of a single 217-entry
 * ordering table, then one walk of that table with the draw-env packets in it
 * doing the clipping. See src/screen.
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
 *   F5           cycle the console's viewport layouts (one, the two splits,
 *                the 2x2, and the boot screen's single-buffered full screen)
 *   space/ctrl   jump / crouch (simulated movement only)
 *   Esc          the pause menu — and QUIT GAME inside it leaves
 *
 * In the menu the keyboard stands in for the pad, because the menu engine is
 * written against the console's 16-bit button mask and nothing is gained by
 * giving it a second input model:
 *
 *   arrows       d-pad          Enter / Space   cross   (select)
 *   Esc          triangle       Backspace       triangle (back)
 */
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "entity.h"
#include "ident.h"
#include "menu.h"
#include "menudraw.h"
#include "q2psx.h"
#include "raster.h"
#include "screen.h"
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

    /* The pause menu, and the settings it edits. The settings outlive a zone
     * load; the menu does not own them. */
    q2_menu_settings settings;
    q2_menu          menu;
    q2_menu_style    menu_style;
    psx_ot           ot;
    gte_state        gte;
    q2_screen        screen;
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

        /* SOUND OPTIONS -> MUSIC. The original scales by the slider doubled
         * (0x800205F4 stores music*2 into the volume global), so the top of
         * the slider is full scale and the bottom is silence. */
        {
            s32 vol = c->settings.v[Q2_SET_MUSIC] * 2;
            if (vol < 255) {
                u32 i;
                for (i = 0; i < n; i++)
                    pcm[i] = (s16)((pcm[i] * vol) / 255);
            }
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
    in.attack = keys[SDL_SCANCODE_LALT] != 0 || keys[SDL_SCANCODE_F] != 0;

    /*
     * Weapon switching is edge-triggered here rather than held, because the
     * console's own repeat is a 70-tick countdown in the player's weapon block
     * (0x8004ECF0) and the view model that paces it is not reconstructed yet.
     * Holding the key would cycle at the frame rate, which the console never
     * does.
     */
    {
        static bool was_next, was_prev;
        bool next = keys[SDL_SCANCODE_RIGHTBRACKET] != 0 ||
                    keys[SDL_SCANCODE_E] != 0;
        bool prev = keys[SDL_SCANCODE_LEFTBRACKET] != 0 ||
                    keys[SDL_SCANCODE_Q] != 0;

        if (next && !was_next) q2_sim_cycle_weapon(&c->sim, +1);
        if (prev && !was_prev) q2_sim_cycle_weapon(&c->sim, -1);
        was_next = next;
        was_prev = prev;
    }

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
/*
 * The menu.
 *
 * The engine it drives is written against the console's button mask, so the
 * keyboard is translated into one rather than the menu being taught about
 * scancodes. That keeps the navigation rules — wrap, skip, press-versus-release
 * — exactly as they were read out of the executable.
 */
static u16 client_menu_pad(void)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    u16 pad = 0;

    if (k[SDL_SCANCODE_UP])        pad |= Q2_PAD_UP;
    if (k[SDL_SCANCODE_DOWN])      pad |= Q2_PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT])      pad |= Q2_PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT])     pad |= Q2_PAD_RIGHT;
    if (k[SDL_SCANCODE_RETURN] ||
        k[SDL_SCANCODE_KP_ENTER] ||
        k[SDL_SCANCODE_SPACE])     pad |= Q2_PAD_CROSS;
    if (k[SDL_SCANCODE_BACKSPACE]) pad |= Q2_PAD_TRIANGLE;

    return pad;
}

/*
 * Push the settings the menu edits into the systems that consume them. The
 * original does this from the menu's own hooks; here it is one place so the
 * effect of a page is visible rather than scattered.
 */
static void client_apply_settings(client *c)
{
    q2_menu_rules rules;

    /* GAME VARIABLES only exist in a multiplayer session, which is exactly
     * when the original enables them (0x8002033C passes 1 there and 0 in
     * single player). */
    q2_menu_apply_variables(&c->settings, c->menu.multiplayer,
                            q2_build_tick_rate(&c->build), &rules);

    c->sim.gravity = rules.gravity;
    if (rules.tick_rate > 0)
        c->sim.dt_per_field = 300 / rules.tick_rate;
    if (c->sim.dt_per_field <= 0)
        c->sim.dt_per_field = 1;
}

static void client_menu_requests(client *c)
{
    switch (q2_menu_take_request(&c->menu)) {
    case Q2_MREQ_RESUME:
        break;
    case Q2_MREQ_RESTART:
    case Q2_MREQ_RESUPPLY:
        Q2_INFO("restarting %s zone %d", c->map, c->zone_index);
        client_load_zone(c, c->map, c->zone_index);
        q2_menu_close(&c->menu);
        break;
    case Q2_MREQ_QUIT:
        c->running = false;
        break;
    case Q2_MREQ_MISSION:
        /* The mission screen is a separate page of the original's HUD system
         * and is not reconstructed yet; the menu closing is the part that is. */
        Q2_INFO("mission screen: not implemented yet");
        break;
    default:
        break;
    }
}

static void client_menu_frame(client *c)
{
    q2_menu_sound snd;

    q2_menu_advance(&c->menu, client_menu_pad());

    snd = q2_menu_take_sound(&c->menu);
    if (snd != Q2_MSND_NONE) {
        /* The bank these live in decodes, but nothing plays one-shot effects
         * yet; naming the sound is the honest half of the wiring. */
        Q2_DEBUG("menu sound: %s", q2_menu_sound_name(snd));
    }

    client_menu_requests(c);
}

/* ------------------------------------------------------------------------- */
/*
 * One frame, in the order 0x800182C8 does it.
 *
 * The swap comes first, so a frame is always built into the buffer that is not
 * being shown. Then one full-screen background clear, then each live viewport
 * in turn — each one loading the GTE from its own view record and filling its
 * own 51-bucket slice of the single ordering table. Composition walks that one
 * table once; the draw-env packets sitting in it are what clip each viewport.
 *
 * There is one simulated player, so every viewport gets the same camera. That
 * is not a stand-in for split screen — it is what makes the reconstructed
 * layouts visible, since a second player would change nothing about the screen
 * work itself.
 */
static void client_frame(client *c)
{
    q2_world_stats stats;
    void *pixels;
    int pitch;
    const psx_framebuffer *front;
    psx_framebuffer *back;
    int p;

    q2_screen_frame_begin(&c->screen, &c->ot);

    /*
     * 0x800780C0 clears the whole screen once and turns the per-viewport clears
     * off, which is what paints the gutters between split viewports.
     */
    c->screen.disp.bg_rgb[0] = 16;
    c->screen.disp.bg_rgb[1] = 16;
    c->screen.disp.bg_rgb[2] = 32;
    c->screen.disp.bg_enable = 1;
    q2_screen_background(&c->screen);

    for (p = 0; p < c->screen.view_count; p++) {
        const q2_screen_view *v = &c->screen.view[p];

        q2_screen_view_begin(&c->screen, p, &c->ot, &c->gte);

        /* The viewport owns the field of view: SetGeomScreen(view+262) at
         * 0x80076B90, and a geometry offset at the viewport's own centre. */
        c->cam.projection = (u16)v->proj;
        q2_world_build_ot(&c->zone, &c->cam, v->w, v->h,
                          &c->ot, &c->gte, &stats);
    }

    q2_screen_compose(&c->screen, &c->ot, c->vram, &c->opts);

    /* The menu draws over the frozen world, which is what pausing looks like
     * on the console: the scene stays on screen behind it. It belongs to the
     * overlay camera's full-screen space, not to any one viewport. */
    back = q2_screen_back(&c->screen);
    q2_menu_draw(&c->menu, back, &c->menu_style);

    q2_screen_present(&c->screen);
    front = q2_screen_front(&c->screen);

    if (SDL_LockTexture(c->texture, NULL, &pixels, &pitch)) {
        int y;
        for (y = 0; y < c->height; y++) {
            memcpy((u8 *)pixels + (size_t)y * pitch,
                   front->px + (size_t)y * c->width,
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

    /*
     * The console's own framebuffer, brought up the way 0x800764DC brings it
     * up: 512 x 248 on PAL, read out of the executable rather than assumed.
     * Everything is rendered here and upscaled; the dither and the vertex
     * snapping are defined in these pixels, so rendering at a higher resolution
     * would change the look.
     *
     * This is also what the menu's coordinates were always in — its tables put
     * the title at x = 256 of 512 — so the screen and the menu now agree
     * instead of the menu being mapped onto a smaller buffer.
     */
    if (q2_screen_init(&c.screen, c.build.video) != Q2_OK) {
        fprintf(stderr, "cannot bring the screen up\n");
        disc_close(c.disc);
        return 1;
    }
    if (c.screen.disp.height_is_inferred)
        Q2_WARN("no NTSC framebuffer has been read out of an NTSC build; "
                "using PAL's 512x248");

    c.width  = c.screen.disp.width;
    c.height = c.screen.disp.height;

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

    /* 217 buckets, `ClearOTag(db + 10984, 217)` at 0x80018398 — the table is
     * carved into per-viewport slices, so its size is not a tuning knob. */
    psx_ot_init(&c.ot, Q2_SCREEN_OT_ENTRIES, 300000);
    psx_raster_opts_default(&c.opts);
    c.vram = (psx_vram *)calloc(1, sizeof(psx_vram));

    q2_camera_default(&c.cam, c.width, c.height);

    /*
     * The menu's layout is in the console's own 512x248 framebuffer, which is
     * where every coordinate in its tables was authored; the renderer maps that
     * onto whatever this window is. Passing the NTSC height would be a guess —
     * only PAL's 248 has been read out of a build (openquestions #30).
     */
    /*
     * A session starts by installing a layout (0x8003F8D8's jump table on the
     * session mode). The boot state the screen came up in is the front end's;
     * this is single player in a level.
     */
    q2_screen_set_layout(&c.screen, Q2_SCREEN_LAYOUT_ONE, 1);

    q2_menu_settings_defaults(&c.settings);
    q2_menu_init(&c.menu, &c.settings, Q2_MENU_SCREEN_H);
    q2_menu_style_default(&c.menu_style);
    q2_menu_set_multiplayer(&c.menu, false);

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
                case SDLK_ESCAPE:
                    /* START on the console: it opens the pause menu, and
                     * closes it again from the root page. Deeper in, the
                     * menu's own TRIANGLE handling owns going back. */
                    if (!c.menu.open)
                        q2_menu_open(&c.menu);
                    else if (c.menu.depth == 0)
                        q2_menu_close(&c.menu);
                    break;
                case SDLK_F1: c.opts.dither    = !c.opts.dither;    break;
                case SDLK_F2: c.opts.affine_uv = !c.opts.affine_uv; break;
                case SDLK_F4:
                    c.sim_enabled = !c.sim_enabled;
                    Q2_INFO("movement: %s", c.sim_enabled ? "simulated" : "free-fly");
                    break;
                case SDLK_F5: {
                    /* Every layout the session code can install. There is one
                     * simulated player, so the extra viewports show the same
                     * camera — what is on show here is the screen work, not a
                     * second player. */
                    int next = (int)c.screen.layout + 1;
                    if (next >= Q2_SCREEN_LAYOUT_COUNT)
                        next = 0;
                    q2_screen_set_layout(&c.screen, (q2_screen_layout)next,
                                         next == Q2_SCREEN_LAYOUT_QUAD ? 4 : 2);
                    Q2_INFO("layout: %s, %d viewport%s",
                            q2_screen_layout_name(c.screen.layout),
                            c.screen.view_count,
                            c.screen.view_count == 1 ? "" : "s");
                    break;
                }
                default:
                    if (!c.menu.open &&
                        ev.key.key >= SDLK_0 && ev.key.key <= SDLK_9) {
                        int z = (int)(ev.key.key - SDLK_0);
                        client_load_zone(&c, c.map, z);
                    }
                    break;
                }
            }
        }

        if (c.menu.open) {
            /* The world is frozen while the menu is up: no input, no tick. */
            client_menu_frame(&c);
        } else if (c.sim_enabled) {
            client_input_simulated(&c, dt);
        } else {
            client_input(&c, dt);
        }

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
        /* Every frame, not just menu frames: a zone load rebuilds the sim and
         * would otherwise drop back to the compiled-in constants. */
        client_apply_settings(&c);
        client_music_pump(&c);
        client_frame(&c);
    }

done:
    q2_sim_free(&c.sim);
    q2_common_close(&c.common);
    q2_world_free_zone(&c.zone);
    free(c.vram);
    q2_screen_free(&c.screen);
    psx_ot_free(&c.ot);
    if (c.audio)    SDL_DestroyAudioStream(c.audio);
    if (c.texture)  SDL_DestroyTexture(c.texture);
    if (c.renderer) SDL_DestroyRenderer(c.renderer);
    if (c.window)   SDL_DestroyWindow(c.window);
    SDL_Quit();
    disc_close(c.disc);
    return 0;
}
