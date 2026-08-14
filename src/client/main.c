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
 * Upscaled, NOT stretched to fill. A framebuffer pixel is two thirds as wide as
 * it is tall on a PAL television — every one of the GPU's horizontal modes spans
 * the same active line, so 512 columns are narrow columns, not a wider picture —
 * and putting the buffer on a window one-for-one is a 1.5x horizontal stretch
 * that makes a correctly reconstructed field of view read as a wrong one. The
 * shape is q2_screen_fit_rect's, the window may be any size or aspect, and V
 * cycles the choice. See the pixel-aspect section of src/screen/screen.h.
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
 *   F6           show the GlintMod glint (BIGGUN only; off by default because
 *                nothing the engine does turns one on — see effect.h)
 *   F7 / F8      the memory-card front end, saving and loading
 *   F9 / F10     quick save and quick load, slot 1
 *   F11          screenshot, of the 512x248 framebuffer rather than the window
 *   V            cycle how the picture is shaped: the console's own pixel, the
 *                raw buffer, a forced 4:3, or filling the window
 *   space        jump — and, held, swim up. One key because it is one BUTTON:
 *                the pad's tail writes bit 22 from its press edge and bit 21
 *                from it being held (pad.h)
 *   up/down      look — and holding BOTH is the console's own view recentre,
 *                which is a chord rather than a setting (0x8003A780)
 *   ctrl / c     hold a crouch. The one key here that is NOT the console's:
 *                crouching is authored per map as a trigger volume, and this
 *                asserts the same flag such a volume would (worldscale.h). The
 *                map's own crouch volumes work without it
 *   Esc          the pause menu — and QUIT GAME inside it leaves
 *
 * In the menu the keyboard stands in for the pad, because the menu engine is
 * written against the console's 16-bit button mask and nothing is gained by
 * giving it a second input model:
 *
 *   arrows       d-pad          Enter / Space   cross   (select)
 *   Esc          triangle       Backspace       triangle (back)
 *
 * The two save keys are the port's, and the reason they exist is that the
 * console's own route to SAVE? is not reachable here: on the disc that prompt
 * is reached from the front end and at a level boundary, neither of which this
 * client has. Everything BEHIND the prompt is the console's — the screens, the
 * four rows, the release rule and the state machine (memcard.h, saveui.h).
 */
#include <SDL3/SDL.h>

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "aiworld.h"
#include "crebind.h"
#include "creworld.h"
#include "lighting.h"
#include "rotator.h"
#include "spacelights.h"
/* Creatures on the biggest map plus three other players, with room to spare. */
#define Q2_CLIENT_MAX_TARGETS 96

#include "multiplayer.h"
#include "userfuncs.h"
#include "disc.h"
#include "entity.h"
#include "entitydraw.h"
#include "fxtables.h"
#include "hudtables.h"
#include "ident.h"
#include "itemtable.h"
#include "menu.h"
#include "menudraw.h"
#include "memcard.h"
#include "menufont.h"
#include "leveltable.h"
#include "musictable.h"
#include "briefing.h"
#include "leveltext.h"
#include "mission.h"
#include "panel.h"
#include "prompt.h"
#include "q2psx.h"
#include "raster.h"
#include "save.h"
#include "saveui.h"
#include "screen.h"
#include "pad.h"
#include "sim.h"
#include "statusbar.h"
#include "trig.h"
#include "vag.h"
#include "viewweapon.h"
#include "vmtables.h"
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

    /*
     * WHICH music, which used to be "track A, because the mapping is not
     * decoded yet". It is decoded now (musictable.h): a level record carries a
     * seven-entry playlist of track ids, and an id names a file and a channel
     * through the table at 0x800A1DD8. `music_cursor_at` is the engine's own
     * walk position — the cursor at gp+1536 — so a track that ends advances the
     * list instead of looping itself, which is what the console does.
     */
    q2_music_table   music_table;
    bool             music_table_ready;
    q2_level_table   level_table;
    bool             level_table_ready;
    const q2_level_entry *level;
    int              music_cursor_at;
    int              music_id;
    SDL_AudioStream *audio;

    q2_camera        cam;
    /*
     * One sim per player. Index 0 owns the WORLD — the items, the script, the
     * entity list, the effects — and 1..3 are movement instances: each has its
     * own position, view, inventory and pad, and their world-side halves are
     * ticked but never read or drawn. Sharing one world properly means pulling
     * the player out of q2_sim, which is a change to sim.c and not to its
     * caller; see openquestions #53.
     */
    q2_sim           sim[Q2_MP_MAX_PLAYERS];
    bool             sim_ready[Q2_MP_MAX_PLAYERS];
    q2_pad_state     mp_pad[Q2_MP_MAX_PLAYERS];
    bool             sim_enabled;

    /* World render state. It lives here rather than in the draw because the
     * texture-page table's ABR promotions must persist across frames — see
     * q2_world_render. */
    q2_world_render  render;

    /* The pause menu, and the settings it edits. The settings outlive a zone
     * load; the menu does not own them.
     *
     * The FONT does not outlive one: its two atlases are VRAM images inside the
     * map's own SNDVRAM.DAT (menufont.h), so it is uploaded per zone load
     * alongside the texture pages, exactly as the console's image registration
     * runs per level. */
    q2_menu_settings settings;
    q2_menu          menu;
    q2_menu_font     menu_font;
    bool             menu_font_ready;
    q2_hud_tables    hud_tables;
    bool             hud_tables_ready;

    /* The HUD's own font — the same atlas, reached through the overlay's
     * markup layer rather than the menu's glyph path — and the level-completion
     * screen that draws through it. */
    q2_hud_font      hud_font;
    bool             hud_font_ready;
    q2_mission       mission;
    bool             mission_open;

    /* The briefing screen, and the two pieces of UI chrome it shares with the
     * mission screen and the memory-card questions. */
    q2_briefing      briefing;
    bool             briefing_open;
    q2_prompt_bar    prompts;

    /*
     * The overlay itself: the notification ring, the centre line, the crosshair
     * and the damage flash. It was reconstructed before anything called it —
     * this is the call, and it is now fed the player's real condition every
     * tick, so the flash reacts to damage the way the console's does.
     */
    q2_hud           hud;
    bool             hud_ready;

    /* The map's own sound bank, for the menu's five effects. Per zone load, as
     * the bank is per map. */
    q2_sound_bank    sfx;
    bool             sfx_ready;

    /*
     * The status bar — the thing this project once proved did not exist. It is
     * drawn per viewport by the same hook that draws that viewport's world
     * (statusbar.h), so it is fed and emitted inside client_draw_view.
     */
    q2_icon_tables   icons;
    bool             icons_ready;
    q2_statusbar     sbar;

    /*
     * The memory-card front end. Its screens and its release-gated state
     * machine are the console's (memcard.h); the card operations behind them
     * are `libmcrd` talking to hardware this port does not have, so what sits
     * behind the three function pointers here is the port's own file-backed
     * save system (saveui.h) rather than a stub.
     *
     * `card_menu` is a second menu instance that exists only to NAVIGATE and
     * DRAW one of those screens. The screens are ordinary 24-byte item tables,
     * so the menu engine already knows how to walk them — running them through
     * it is what gives the front end the same cursor rules, the same selection
     * bar and the same font as every other page, instead of a second
     * implementation that would drift.
     */
    q2_mcard         mcard;
    q2_mcard_host    mcard_host;
    bool             mcard_open;
    q2_save_ui       save_ui;
    q2_save_ui_mode  card_mode;
    q2_menu          card_menu;
    q2_mcard_screen  card_screen;

    /* What a save writes. Held here rather than built inside the front end
     * because q2_save_ui borrows it and the capture needs the whole client —
     * the sim, the mission tallies and the menu settings. */
    q2_save          snapshot;

    /*
     * The item table — 64 records, the 55-slot touch dispatch and the eleven
     * sound names — out of the same executable as everything else here. Per
     * disc rather than per map, and handed to the spawner so the items standing
     * in the level come from the disc's own table rather than from the
     * transcription of it.
     */
    q2_item_table    item_table;
    bool             item_table_ready;

    q2_fx_tables     fx_tables;
    bool             fx_tables_ready;
    /*
     * The weapon in the player's hands. The bank is per disc — the animation
     * clips live in the executable, not on a map — while the model itself comes
     * out of whichever map is loaded, so the two are bound at different times.
     */
    q2_vm_tables     vm_tables;
    bool             vm_ready;
    q2_viewweapon    vw;
    q2_model_bank    model_bank;
    bool             model_bank_ready;
    q2_model         vw_model;
    bool             vw_model_ready;
    int              vw_last_weapon;

    /*
     * The things in the level that are trying to kill you.
     *
     * Every piece of this was written and nothing called it: the modules
     * relocate, decode and bind, the Population records spawn, and the AI runs
     * — but only the inspector ever asked, and it draws a creature standing
     * still at its spawn point. A level in the client was its geometry, its
     * items, and nothing that moves. This is the join (creworld.h).
     *
     * `cre_model` is resolved once per zone load rather than per frame: a
     * creature's model is named by its class and lives in either the map's
     * CastList or the zone's, and searching two banks for every monster every
     * frame is work with a fixed answer.
     */
    q2_creature_world creatures;
    bool              creatures_ready;
    q2_model_bank     zone_bank;
    bool              zone_bank_ready;
    q2_model         *cre_model;      /* one per monster, NULL when unresolved */
    bool             *cre_model_ok;
    q2_actor         *cre_actor;      /* what combat shoots at                 */
    q2_actor        **cre_target;
    /*
     * The world the AI asks its three questions of — line of sight, a box
     * move, and whether there is ground under a creature's feet. Without this
     * the AI runs on the open stand-in, where every creature can see through
     * every wall and stand on thin air. It borrows the sim's SecondaryCol, so
     * it must not outlive a zone.
     */
    q2_ai_world_bind  ai_world;

    double            ai_accum;       /* seconds owed to the 10 Hz AI clock    */
    u32               ai_thoughts;
    u32               cre_swings, cre_shots;   /* hook invocations */
    u32               cre_sounds;
    u32               cre_sound_missing;
    u32               ent_light_added;
    u32               script_lights;
    q2_flklights      flklights;
    u32               ent_light_dropped;
    u32               ent_bursts;
    u32               burst_no_fx, burst_no_table, burst_no_model;
    u32               burst_no_bank, burst_bad_model, burst_no_verts;
    u32               player_attacks;
    u32               rot_moved;
    u32               rot_steps;   /* step requests the script has made */
    u32               vw_events;
    s16               vw_last_event;
    int               cre_last_sound;
    /* ------------------------------------------------------------------- */
    /* The multiplayer session. QMULTI.C is a per-map LevelBin module and the
     * engine only carries the hook, so this is what stands in for the module
     * being installed: the rules ran nowhere before it. */
    bool              mp_enabled;
    q2_mp_session     mp;
    u32               mp_spawn_count;
    u32               mp_rng_state;
    s32               mp_level_time;   /* 0x800AEBAC, in dt units             */
    q2_mp_request     mp_last_request;
    bool              mp_reported;

    u32               mp_deaths;      /* kills fed to the session              */
    bool              mp_scoreboard;  /* QMRESULT is up                        */

    /* Creatures plus the other players, rebuilt per player. */
    q2_actor         *mp_target[Q2_CLIENT_MAX_TARGETS];
    q2_actor         *mp_world_target[Q2_CLIENT_MAX_TARGETS];
    bool              mp_targets_logged;
    bool              mp_stage;
    u32               mp_shots[Q2_MP_MAX_PLAYERS];
    u32               mp_dry[Q2_MP_MAX_PLAYERS];
    bool              mp_dead[Q2_MP_MAX_PLAYERS];
    int               trace_cre;      /* creature index to trace, -1 for none  */
    u32               trace_ticks;

    /* Where each player's viewport looks from. Player 0's is the sim's. */
    s32               mp_view_pos[Q2_MP_MAX_PLAYERS][3];
    s16               mp_view_yaw[Q2_MP_MAX_PLAYERS];
    bool              mp_view_valid[Q2_MP_MAX_PLAYERS];
    u32               cre_bodies;     /* deaths that found a death move        */
    u32               cre_drawn;      /* creatures with faces in the last view */
    u32               cre_faces;
    s32              *cre_home;      /* where each creature spawned          */

    /* The map's CLUT split. A model face's palette index is offset by it —
     * model palettes live in the second section of the array (model.h §233). */
    u32              clut4_count_a;

    psx_ot           ot;
    gte_state        gte;
    q2_screen        screen;
    psx_vram        *vram;
    psx_raster_opts  opts;

    SDL_Window      *window;
    SDL_Renderer    *renderer;
    SDL_Texture     *texture;

    int              width, height;

    /*
     * How the 512x248 buffer is fitted into the window. The default is 4:3, the
     * shape the game is captured and played at; it is NOT the
     * one-buffer-pixel-to-one-window-pixel a framebuffer dump suggests, which is
     * a 1.5x horizontal stretch. V cycles it.
     */
    q2_screen_fit    fit;

    /*
     * The title screen. `QFRONT` is a real level — the level table's record 0 —
     * so the front end is that level loaded with the menu's page 46 over it,
     * not a page of art. While it is up the simulation does not run: there is
     * no player in it.
     */
    /*
     * Rotating brush geometry — ROTHATCH, SIMROT, SIMROT2, ROTBUTTON. The
     * builder and the integrator have both existed since rotator.[ch] was
     * written and the only caller was an inspector command, so nothing in the
     * game ever turned. The set borrows the zone, which draws through it.
     */
    q2_rotator_set   rotators;
    bool             rotators_ready;

    /*
     * The zone's lights, for everything that is not the world. The world's own
     * lighting is baked into MapMod's per-corner RGB and nothing at runtime
     * touches it (FORMATS §17), so this exists to shade MODELS — the items and
     * the creatures — which the client had been drawing at a flat glow tint
     * because it passed NULL for the light world.
     */
    q2_light_list    lights;
    q2_spacelights   spacelights;
    q2_light_world   light_world;
    bool             lights_ready;

    bool             in_front_end;
    char             first_map[64];

    bool             show_glint;
    bool             force_underwater;   /* F3 — stands in for a water volume */
    bool             running;

    /*
     * ---------------------------------------------------------------------
     * Running without a window
     * ---------------------------------------------------------------------
     * The whole of the game's per-frame work — the sim tick, the screen's
     * viewport build, the world draw, the ordering table, the rasteriser, the
     * HUD and the menu — happens before a single SDL call. Only the last
     * twenty lines of `client_frame` need a renderer, so the frame loop can run
     * with none, at a fixed step, and write its framebuffer out.
     *
     * That is not a convenience. It is the only way the CLIENT's own wiring can
     * be checked the way `q2psx-inspect` checks the libraries: the inspector
     * composes its own frames and so cannot catch anything that goes wrong
     * between the client's systems — a table loaded after the thing that reads
     * it, a model never bound, a screen never fed.
     */
    bool             headless;
    bool             demo;               /* drive the pad from a script     */
    bool             watch;              /* frame the nearest live creature  */
    q2_world_stats   shot_stats;         /* what the last viewport drew     */
    long             frame_index;
    long             frames_total;       /* 0 = run until the window closes */
    long             shot_every;         /* 0 = only the last frame         */
    const char      *shot_path;          /* NULL = do not capture           */
    long             shots_written;
} client;

static void client_bind_view_model(client *c);

/* Defined with the rest of the sound path, and called from the tick. */
/*
 * The pickup particle burst — 0x8005B6C0, which is a four-line wrapper around
 * the shared spawner at 0x8005AB70:
 *
 *     q2_burst(pos, ramp 10, ramp 0, size 6144, area 0)
 *
 * The two pointers it passes are `0x8009BF88` and `0x8009BA60`, and the second
 * is the ramp table itself (fxtables.h, nineteen 132-byte records), so their
 * difference of 1320 makes the first ramp index 10.
 *
 * Its fifth argument is zero, which selects the spawner's second branch:
 * **life 32**, and a velocity per component of
 *
 *     v = ((rand() - 16384) * 3) / 16384        ; truncating toward zero
 *
 * — the `sll 1 / addu / bgez +16383 / sra 14` at 0x8005AC4C, giving a drift of
 * plus or minus three.
 *
 * And the COUNT is not a constant, which is why this could not be one of the
 * port's seven presets: 0x8006D6AC totals the model's vertices across its
 * eight-byte part records (`num_verts` at +3) and the caller divides by fifteen.
 * A bigger item bursts bigger, in proportion to its own mesh.
 */
static void client_pickup_burst(client *c, const s32 pos[3], s32 model_index)
{
    q2_model mdl;
    s16 vel[Q2_FX_GROUP_QUADS][3];
    u32 total = 0, count, k;

    /* Counted, not assumed: an early return here is silent otherwise, and
     * "0 drawn" would read as "no bursts happened". */
    if (!c->sim[0].fx_ready)      { c->burst_no_fx++;    return; }
    if (!c->fx_tables_ready)      { c->burst_no_table++; return; }
    if (model_index < 0)          { c->burst_no_model++; return; }
    if (!c->model_bank_ready)     { c->burst_no_bank++;  return; }

    if (q2_model_get(&c->model_bank, (u32)model_index, &mdl) != Q2_OK) {
        c->burst_bad_model++;
        return;
    }

    for (k = 0; k < mdl.hdr.num_parts; k++) {
        q2_model_part part;

        if (q2_model_get_part(&mdl, k, &part))
            total += part.num_verts;
    }

    count = total / 15;
    if (count == 0) {
        c->burst_no_verts++;
        return;
    }
    if (count > Q2_FX_GROUP_QUADS)
        count = Q2_FX_GROUP_QUADS;

    for (k = 0; k < count; k++) {
        int a;

        for (a = 0; a < 3; a++) {
            s32 v = ((s32)q2_rng_next(&c->sim[0].combat.rng) & 0x7FFF) - 16384;

            v *= 3;
            if (v < 0)
                v += 16383;
            vel[k][a] = (s16)(v >> 14);
        }
    }

    q2_fx_group_spawn(&c->sim[0].fx, pos, vel, count,
                      q2_fx_ramp_at(&c->fx_tables, 10),
                      q2_fx_ramp_at(&c->fx_tables, 0), 32, 6144, 0);
    c->ent_bursts++;
}

static void client_entity_events(client *c);
static bool client_play_sound(client *c, const char *want);

/* The creature hooks, defined below with the AI clock they run on. */
static void client_cre_melee(q2_monster *m, const s32 aim[3], s32 damage,
                             s32 kick, void *user);
static void client_cre_sound(q2_monster *m, int which, void *user);
static void client_cre_fire(q2_monster *m, int flash, void *user);

/* ------------------------------------------------------------------------- */
/*
 * The creatures a map places, made live.
 *
 * Called from the zone load, after the sim exists and the player has been put
 * where the level starts them, because the AI's sight client is the player and
 * a creature that wakes before there is one has nothing to acquire.
 */
static void client_free_creatures(client *c)
{
    free(c->cre_model);
    free(c->cre_model_ok);
    free(c->cre_actor);
    free(c->cre_target);
    free(c->cre_home);
    c->cre_home     = NULL;
    c->cre_model    = NULL;
    c->cre_model_ok = NULL;
    c->cre_actor    = NULL;
    c->cre_target   = NULL;

    if (c->creatures_ready) {
        q2_sim_set_targets(&c->sim[0], NULL, 0);
        q2_creature_world_free(&c->creatures);
        c->creatures_ready = false;
    }
    c->zone_bank_ready = false;
}

static void client_load_creatures(client *c, const s32 eye[3])
{
    u32 i, resolved = 0;

    client_free_creatures(c);

    if (q2_creature_world_load(&c->creatures, c->disc, &c->build,
                               &c->common) != Q2_OK) {
        Q2_WARN("%s: creatures will not load", c->map);
        return;
    }
    c->creatures_ready = true;

    /* The zone's own CastList as well as the map's: a creature's model is as
     * likely to be in one as the other, which is why the inspector's census
     * searches both. */
    c->zone_bank_ready =
        (q2_model_bank_from_zone(&c->zone_bank, &c->zone.zone) == Q2_OK);

    if (c->creatures.set.count) {
        c->cre_model    = (q2_model *)calloc(c->creatures.set.count,
                                             sizeof(*c->cre_model));
        c->cre_model_ok = (bool *)calloc(c->creatures.set.count,
                                         sizeof(*c->cre_model_ok));
        c->cre_actor    = (q2_actor *)calloc(c->creatures.set.count,
                                             sizeof(*c->cre_actor));
        c->cre_target   = (q2_actor **)calloc(c->creatures.set.count,
                                              sizeof(*c->cre_target));
    }

    if (!c->cre_model || !c->cre_model_ok || !c->cre_actor || !c->cre_target) {
        if (c->creatures.set.count)
            Q2_WARN("no memory for %u creatures", c->creatures.set.count);
        client_free_creatures(c);
        return;
    }

    /*
     * Confine them to THIS zone.
     *
     * Population is per MAP and a session is in one ZONE — the same thing that
     * makes q2_item_spawn_zone necessary — but a spawn record carries no zone
     * field, so the test has to be geometric: a creature that is inside no cell
     * of this zone's hull belongs to another one. On BASE1 that is eleven of
     * twenty, standing in ZONE1's rooms while ZONE0 is loaded, thinking and
     * being drawn and shootable through the void.
     */
    if (c->sim[0].coll_primary_ready) {
        u32 elsewhere = 0;
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m = &c->creatures.set.monsters[i];
            if (q2_coll_find_node(&c->sim[0].coll_primary, m->pos, -1, true) < 0) {
                m->in_use = false;
                elsewhere++;
            }
        }
        if (elsewhere)
            Q2_INFO("creatures: %u of %u belong to another zone",
                    elsewhere, c->creatures.set.count);
    }

    c->cre_home = (s32 *)calloc(c->creatures.set.count ?
                                c->creatures.set.count * 3 : 1, sizeof(s32));

    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        const char *name = q2_creature_world_model_name(&c->creatures, m);
        s32 idx;

        if (c->cre_home) {
            c->cre_home[i * 3 + 0] = m->pos[0];
            c->cre_home[i * 3 + 1] = m->pos[1];
            c->cre_home[i * 3 + 2] = m->pos[2];
        }

        c->cre_target[i] = &c->cre_actor[i];
        q2_actor_init(&c->cre_actor[i]);
        q2_actor_from_monster(&c->cre_actor[i], m);

        if (!name)
            continue;

        if (c->model_bank_ready) {
            idx = q2_model_bank_find(&c->model_bank, name);
            if (idx >= 0 &&
                q2_model_get(&c->model_bank, (u32)idx,
                             &c->cre_model[i]) == Q2_OK) {
                c->cre_model_ok[i] = true;
                resolved++;
                continue;
            }
        }
        if (c->zone_bank_ready) {
            idx = q2_model_bank_find(&c->zone_bank, name);
            if (idx >= 0 &&
                q2_model_get(&c->zone_bank, (u32)idx,
                             &c->cre_model[i]) == Q2_OK) {
                c->cre_model_ok[i] = true;
                resolved++;
            }
        }
    }

    q2_sim_set_targets(&c->sim[0], c->cre_target, c->creatures.set.count);

    /*
     * The world the AI asks its three questions of, and it is `PrimaryColl`.
     *
     * The un-eroded QUERY hull, which is the one the original uses for
     * everything that is not a player move (sim.h), and the only one a creature
     * is actually inside: `SecondaryCol` is `PrimaryColl` eroded by the
     * PLAYER's 286-unit half-extent, and a Population spawn point sits in the
     * part that erosion cuts away. Measured on BASE1 with the eroded hull, 214
     * of 214 traces cannot place their start and 412 of 432 sight lines are
     * blocked; with this one, 1 of 30 and 342 of 432.
     */
    q2_ai_world_bind_init(&c->ai_world,
                          c->sim[0].coll_primary_ready ? &c->sim[0].coll_primary
                                                    : NULL);
    q2_ai_world_bind_install(&c->ai_world);

    /*
     * The hooks, before anything wakes. A creature that swings on its first
     * think would otherwise swing into a null pointer.
     *
     * The fire hook carries the Soldier's own figures, read out of its module
     * and matching id's exactly. Other creatures reach it with a table it does
     * not know and are dropped rather than given a Soldier's gun.
     */
    /*
     * The breadcrumb trail the AI hunts along — `0x800D517C`, the sixteen-slot
     * ring at gp+17892. `q2_trail_add` had no caller anywhere in the tree, so
     * the trail was always empty and the three-stage pursuit a creature runs
     * when it loses you could never reach its second stage: it had nowhere to
     * follow you to.
     */
    q2_trail_init();

    q2_cre_set_melee_hook(client_cre_melee, c);
    q2_cre_set_sound_hook(client_cre_sound, c);
    q2_cre_set_fire_hook(client_cre_fire, c);

    q2_creature_world_wake(&c->creatures, eye);
    c->ai_accum = 0.0;

    {
        u32 live = 0;
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].in_use)
                live++;

        Q2_INFO("creatures: %u live in this zone, %u of %u spawn records "
                "placed, %u with a model%s",
                live, c->creatures.stats.placed, c->creatures.stats.records,
                resolved,
                c->creatures.stats.no_module
                    ? ", some classes have no module" : "");
    }
}

/* ------------------------------------------------------------------------- */
/*
 * The three hooks a creature reaches the rest of the game through.
 *
 * `crebind.h` has always defined them and NOTHING had ever set them, so every
 * claw, every shot and every sound a creature made went to a null pointer.
 * Creatures chased the player and could not touch them.
 *
 * They are hooks rather than direct calls for the reason the header gives: the
 * module reaches the engine through its import table in the original, and
 * keeping that shape stops every creature having to know about combat.
 */
static void client_cre_melee(q2_monster *m, const s32 aim[3], s32 damage,
                             s32 kick, void *user)
{
    client *c = (client *)user;

    (void)aim; (void)kick;

    if (!c || !c->creatures_ready || damage <= 0)
        return;

    /*
     * Only a creature that has actually acquired the player. A module's melee
     * runs off its own animation frame and does not check who is in front of
     * it, which is the engine's job — here that check is "is the player the
     * thing it is hunting", because the port has one player and the AI's
     * `enemy` is the sight client whenever it has one.
     */
    if (m->enemy != &c->creatures.sight)
        return;

    c->cre_swings++;

    /* MOD 7 is `0x800612F0`, a creature's contact hit (combat.h) — armour
     * applies, which is what makes it different from the environment's. */
    q2_sim_hurt_player(&c->sim[0], NULL, (s16)damage, Q2_MOD_MELEE,
                       c->creatures.sight.pos);
}

/*
 * A creature's shot, with the figures read out of its own module.
 *
 * `soldier_fire` hands over `table * 8 + flash`, and the table is chosen by
 * skin: 0 blaster, 1 shotgun, 2 machinegun. Each arm's call is now read, and
 * every figure in it is id's own — which is the check that says the read is
 * right rather than merely self-consistent:
 *
 *     table 0  import +0x80  0x80062000  blaster   dmg 5, speed 600
 *     table 1  import +0x88  0x80061ED0  shotgun   dmg 2, kick 1,
 *                                                  spread 1000/500, 12 pellets
 *     table 2  import +0x84  0x80061DFC  bullet    dmg 2, kick 4,
 *                                                  spread 300/500
 *
 * Only the Soldier's are known, so only the Soldier shoots: another creature's
 * fire reaches this with a table this does not recognise and is dropped rather
 * than given a Soldier's gun.
 */
static void client_cre_fire(q2_monster *m, int flash, void *user)
{
    client *c = (client *)user;
    int table = flash >> 3;
    s16 damage;
    int shots;

    if (!c || !c->creatures_ready)
        return;
    if (m->enemy != &c->creatures.sight)
        return;

    switch (table) {
    case 0:  damage = 5; shots = 1;  break;   /* blaster    */
    case 1:  damage = 2; shots = 12; break;   /* shotgun    */
    case 2:  damage = 2; shots = 1;  break;   /* machinegun */
    default: return;
    }

    /*
     * The spread is not modelled here, so a shotgun's twelve pellets would all
     * hit and make a guard four times deadlier than the console's. Halving the
     * count is not a figure from anywhere, so instead the trace is run once per
     * shot through the sim's own line of sight and only the shots that reach
     * land — which for a single-pellet weapon is exact and for the shotgun is
     * the honest approximation, stated rather than hidden.
     */
    c->cre_shots++;

    while (shots-- > 0) {
        if (!q2_visible(m, &c->creatures.sight))
            break;
        q2_sim_hurt_player(&c->sim[0], NULL, damage,
                           table == 0 ? Q2_MOD_ENERGY_BOLT : Q2_MOD_BULLET,
                           c->creatures.sight.pos);
    }
}

static void client_cre_sound(q2_monster *m, int which, void *user)
{
    client *c = (client *)user;
    const char *name = NULL;

    (void)m;
    if (!c)
        return;

    /*
     * The module names a sound by an index into its OWN table, and that table
     * is not resolved yet (#6). This used to map indices 0 and 1 to
     * `cre_pain1` and `cre_die1`, which was an invention twice over: the bank
     * has no such names, so it silently played nothing, and the index-to-name
     * mapping was never read.
     *
     * The bank's real convention is `<creature>_<action><n>`, the same shape as
     * `wep_` and `itm_`: BASE0 carries `sol_atck1`, `sol_atck2`, `sol_atck3`,
     * `sol_deth1..3`, `sol_idle1`, `sol_pain1`, `sol_pain2` and `sol_srch1` —
     * exactly the five families id's soldier has. So the names are there to be
     * matched once the module's table says which index is which; until then
     * this stays silent rather than guessing, and the index is recorded so a
     * caller can see what was asked for.
     */
    c->cre_sounds++;
    c->cre_last_sound = which;

    /*
     * The Soldier's names are read out of its module and every one of them is
     * in the map's bank, so it can actually be played. Another creature's
     * table has not been read, so it stays silent rather than borrowing these.
     */
    /*
     * The Soldier's names are transcribed; every other module carries its own
     * table and it is read the same way. The transcription is preferred where it
     * exists because it was read out of code rather than inferred from slot
     * order.
     *
     * NOT "all seven creatures make their own sounds", which this comment used
     * to claim. `q2psx-inspect creatures` prints each table, and two of the
     * seven come back EMPTY:
     *
     *     Soldier  8   Insane 3   Arachner 6   Gunner 6   Infantry 4
     *     Tankcomm 0   Berserk 0
     *
     * The finder locates the module's own name string and then takes the first
     * run of three consecutive 12-byte name slots after it; for those two that
     * heuristic finds nothing. Their sounds ARE on the disc -- 63 VAG entries
     * begin `tnk_` -- so this is a gap in the finder, not in the data. See
     * openquestions #60 and #61.
     */
    name = q2_cre_soldier_sound_name(which);
    if (!name)
        name = q2_creature_world_sound_name(&c->creatures, m, (u32)which);
    if (name && !client_play_sound(c, name))
        c->cre_sound_missing++;
}

/*
 * The AI clock, which is not the frame clock.
 *
 * `next_think` is on the engine's 10 Hz tick (monster.h), so the tick is run
 * from an accumulator rather than once per drawn frame — otherwise a creature
 * would think three times as often at 30 fps as it does on the console, and
 * every one of the AI's timers is expressed in those ticks.
 */
static void client_creatures_tick(client *c, float dt, const s32 eye[3])
{
    int guard = 0;

    if (!c->creatures_ready)
        return;

    c->ai_accum += (double)dt;
    while (c->ai_accum >= 0.1 && guard++ < 8) {
        c->ai_accum -= 0.1;
        c->ai_thoughts += q2_creature_world_tick(&c->creatures, eye);

        /*
         * One creature, one line per AI tick: the move it is playing, the frame
         * it is on, and its attack state. Counters say what happened across a
         * capture and cannot say what happened between two consecutive ticks —
         * and "the attack move is replaced within five ticks" is a question
         * only consecutive ticks can answer. See openquestions #57.
         */
        if (c->trace_cre >= 0 &&
            (u32)c->trace_cre < c->creatures.set.count &&
            c->trace_ticks < 400) {
            const q2_monster *m = &c->creatures.set.monsters[c->trace_cre];

            Q2_INFO("t%-4u move %-4d frame %-4d as %d flags %08X %s%s%s",
                    c->trace_ticks,
                    m->currentmove ? m->currentmove->first_frame : -1,
                    m->frame, m->attack_state, m->aiflags,
                    m->enemy ? "enemy " : "no-enemy ",
                    m->dead ? "dead " : "",
                    m->in_use ? "" : "gone");
            c->trace_ticks++;
        }
    }
    if (c->ai_accum > 0.5)
        c->ai_accum = 0.0;
}

/*
 * How many rotators are standing at an angle other than the one they started
 * at. `rot moved` counts TICK movement and a SNAP never tick-moves — it takes
 * its whole rotation the moment it is asked (0x8002BFD8) — so a level whose
 * only rotator is a button reads as still while its geometry has turned.
 */
static u32 client_rot_turned(const client *c)
{
    u32 i, n = 0;

    if (!c->rotators_ready)
        return 0;

    for (i = 0; i < c->rotators.count; i++)
        if (c->rotators.rotators[i].angle != 0)
            n++;

    return n;
}

/*
 * How many of this creature's moves before `mv` have the same length, so a move
 * can pick the matching one when several clips share a length. Both lists are
 * walked in their own order, which is the same technique the move NAMES use.
 */
static u32 client_move_ordinal(const q2_monster *m, const q2_mmove *mv)
{
    const q2_cre_bind *b = q2_cre_bind_for(m);
    s32 len;
    u32 i, n = 0;

    if (!b || !mv)
        return 0;

    len = mv->last_frame - mv->first_frame + 1;
    for (i = 0; i < b->move_count; i++) {
        if (&b->move[i] == mv)
            break;
        if (b->move[i].last_frame - b->move[i].first_frame + 1 == len)
            n++;
    }

    return n;
}

/*
 * The tie-break the spawn selector asks for. The original's is the engine's own
 * RNG; any source does here, because it is only consulted when two spawn points
 * are exactly equally far from everybody, and it must not be a constant or the
 * same point wins every draw.
 */
static u32 client_mp_rng(void *user)
{
    client *c = (client *)user;

    /* Numerical Recipes' LCG. The value is used modulo a small count. */
    c->mp_rng_state = c->mp_rng_state * 1664525u + 1013904223u;
    return c->mp_rng_state >> 16;
}

/*
 * One frame of the multiplayer session — the per-frame hook QMULTI.C installs
 * into the engine's level slot, which nothing in this port had ever called.
 *
 * The clock is the engine's at 0x800AEBAC and advances by the frame's dt, both
 * in the sim's units, because that is what the time limit is compared against:
 * `level_time > minutes * 18000`, and 18000 units is sixty seconds at 300 to
 * the second.
 */
static void client_mp_tick(client *c, float dt)
{
    s32 ticks = (s32)((double)dt * 300.0 + 0.5);
    q2_mp_request req;

    if (!c->mp_enabled || c->mp_last_request != Q2_MP_REQ_NONE)
        return;                     /* the match is over and asked for a screen */

    if (ticks < 1)
        ticks = 1;
    c->mp_level_time += ticks;

    req = q2_mp_frame(&c->mp, c->mp_level_time, ticks);

    /* The frame that ends it, announced once. */
    if (c->mp.end != Q2_MP_RUNNING && !c->mp_reported) {
        c->mp_reported = true;
        Q2_INFO("multiplayer: %s at %d dt (%d s) — banner '%s'",
                c->mp.end == Q2_MP_END_TIME_UP     ? "time limit reached" :
                c->mp.end == Q2_MP_END_FRAG_LIMIT  ? "frag limit reached" :
                c->mp.end == Q2_MP_END_ROUND_OVER  ? "round over"         :
                c->mp.end == Q2_MP_END_MATCH_OVER  ? "match over"         :
                                                     "round drawn",
                c->mp_level_time, c->mp_level_time / 300,
                q2_mp_banner(&c->mp) ? q2_mp_banner(&c->mp) : "(none)");
    }

    if (req == Q2_MP_REQ_NONE)
        return;

    /*
     * The banner has run out and the runtime wants a game state: 11 loads the
     * scoreboard, 19 restarts the round. Both are the engine's own ids, and
     * this port has neither screen, so the request is recorded and reported
     * rather than acted on — which is the honest half of the pair.
     *
     * Taking it also stops the session: on the console the request CHANGES THE
     * GAME STATE, so the level hook stops running. Leaving it ticking here made
     * the runtime re-ask on every frame, which is what the first run of this
     * code did — sixty-odd identical requests for one match that ended once.
     */
    c->mp_last_request = q2_mp_take_request(&c->mp);

    /* State 11 is "load MPResults". The port shows the scoreboard rather than
     * loading QMRESULT's own level, because what that level draws is its
     * module's business and what it draws it FROM is the session. */
    if (c->mp_last_request == Q2_MP_REQ_RESULTS)
        c->mp_scoreboard = true;

    {
        int w = q2_mp_find_winner(&c->mp);
        char buf[64];

        Q2_INFO("multiplayer: request %d (%s); winner %d — %s",
                (int)c->mp_last_request,
                c->mp_last_request == Q2_MP_REQ_RESULTS ? "load MPResults"
                                                        : "restart the round",
                w, q2_mp_winner_text(&c->mp, w, NULL, buf, sizeof(buf)));
        Q2_INFO("multiplayer: %s — %s, HUD set %s",
                q2_mp_score_title(c->mp.mode), q2_mp_mode_name(c->mp.mode),
                q2_mp_hud_image(true, c->mp.player_count));
    }
}

/*
 * The things player `who` can hurt: every creature, plus every OTHER player.
 *
 * A player's own hurt-actor lives in the sim — the live one's in `combat.self`,
 * a parked one's in `pcombat[i].self` — so the pointers are stable and the list
 * is rebuilt per player rather than per frame. Registering a player against
 * themselves would let a blaster bolt hit its own muzzle, which is why `who` is
 * skipped.
 *
 * Nothing registered players before this: `combat.targets` held creatures only,
 * so in a deathmatch every shot passed straight through everybody.
 */
static u32 client_targets_for(client *c, int who)
{
    u32 n = 0, i;

    if (c->creatures_ready && c->cre_target)
        for (i = 0; i < c->creatures.set.count && n < Q2_CLIENT_MAX_TARGETS; i++)
            c->mp_target[n++] = c->cre_target[i];

    if (c->mp_enabled)
        for (i = 0; i < Q2_MP_MAX_PLAYERS && n < Q2_CLIENT_MAX_TARGETS; i++) {
            if ((int)i == who || !c->sim_ready[i])
                continue;
            /*
             * ALWAYS the parked slot, never `combat.self`.
             *
             * The list is built before `q2_sim_advance_player` swaps, and the
             * swap moves the live player's half OUT of `combat` and the target
             * player's IN. So a pointer to `combat.self` chosen here for "the
             * player who is live right now" points at somebody else by the time
             * the shot is traced — player 1 was firing 301 shots at its own
             * actor and nobody was ever hit.
             *
             * During `who`'s tick every OTHER player is parked, so
             * `pcombat[i].self` is exactly right for all of them, and `who`
             * itself is skipped above.
             */
            c->mp_target[n++] = &c->sim[0].pcombat[i].self;
        }

    q2_sim_set_targets(&c->sim[0], c->mp_target, n);

    /*
     * And the world's list, which is every player and every creature with
     * nobody left out — what a projectile in flight can hit. Built once here
     * because it does not depend on who is shooting.
     */
    if (c->mp_enabled) {
        u32 w = 0, k;

        if (c->creatures_ready && c->cre_target)
            for (k = 0; k < c->creatures.set.count &&
                        w < Q2_CLIENT_MAX_TARGETS; k++)
                c->mp_world_target[w++] = c->cre_target[k];

        for (k = 0; k < Q2_MP_MAX_PLAYERS && w < Q2_CLIENT_MAX_TARGETS; k++) {
            if (k > 0 && !c->sim_ready[k])
                continue;
            c->mp_world_target[w++] = (k == (u32)c->sim[0].cur_player)
                                          ? &c->sim[0].combat.self
                                          : &c->sim[0].pcombat[k].self;
        }

        q2_sim_set_world_targets(&c->sim[0], c->mp_world_target, w);
    }

    /* Once, so a run says plainly how many things a player can hit. */
    if (!c->mp_targets_logged) {
        c->mp_targets_logged = true;
        Q2_INFO("multiplayer: player %d has %u targets (%u creatures, "
                "%d other players)", who, n,
                c->creatures_ready ? c->creatures.set.count : 0,
                (int)n - (int)(c->creatures_ready ? c->creatures.set.count : 0));
    }

    return n;
}

/*
 * A parked player takes damage on their ACTOR; their inventory is a separate
 * field and only the live player's pair is synchronised. Copy it back so a hit
 * landed while they were parked is still there when their frame runs.
 */
/*
 * Any player whose health has crossed zero scores a frag for whoever did it.
 *
 * This is the engine's own hook at 0x800396AC — `(*module)->[4](killer,
 * victim)` — with the killer taken from the actor's `last_attacker`, which is
 * the byte the original keeps at entity+222. `q2_mp_attribute_kill` decides
 * whether it counts: a world kill and the level's own hazards are nobody's
 * frag, however the victim came to be standing in them.
 */
static void client_score_deaths(client *c)
{
    int i;

    if (!c->mp_enabled || c->mp.end != Q2_MP_RUNNING)
        return;

    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) {
        const q2_actor *a = (i == c->sim[0].cur_player)
                                ? &c->sim[0].combat.self
                                : &c->sim[0].pcombat[i].self;

        if (i > 0 && !c->sim_ready[i])
            continue;
        if (a->health > 0) {
            c->mp_dead[i] = false;
            continue;
        }
        if (c->mp_dead[i])
            continue;                /* already counted this death */

        c->mp_dead[i] = true;
        {
            int killer = q2_mp_attribute_kill(a->last_attacker, a->last_mod);

            q2_mp_player_killed(&c->mp, killer, i);
            c->mp_deaths++;
            Q2_INFO("multiplayer: player %d killed by %d — frags %d %d %d %d",
                    i, killer, c->mp.frags[0], c->mp.frags[1],
                    c->mp.frags[2], c->mp.frags[3]);
        }
    }
}

static void client_sync_parked_health(client *c)
{
    int i;

    if (!c->mp_enabled)
        return;

    for (i = 0; i < Q2_MP_MAX_PLAYERS; i++) {
        if (i == c->sim[0].cur_player || !c->sim_ready[i])
            continue;
        if (c->sim[0].pcombat[i].self.health != c->sim[0].pcombat[i].inv.health)
            c->sim[0].pcombat[i].inv.health =
                c->sim[0].pcombat[i].self.health;
    }
}

/*
 * A script CALL reached a rotation primitive: ask that node's rotator to take
 * one step.
 *
 * The event runtime reports a CALL without interpreting it, because which
 * index is SIMROT is a per-map question only the map's UserFuncs answers. What
 * the operands mean is `rotator.[ch]`'s business, beside the builder that
 * reads the same offsets.
 */
static void client_event_call(void *user, const q2_event_item *item,
                              u8 call_index)
{
    client *c = (client *)user;

    if (!c || !c->rotators_ready || !c->sim[0].userfuncs_ready)
        return;

    c->rot_steps += q2_rotators_call(&c->rotators, &c->sim[0].userfuncs,
                                     item, call_index);

    /*
     * TIMEDLIGHT — a script-placed dynamic light, and the last piece of the
     * fifteen `0x80075C34` call sites that this port could reach without
     * tracing a runtime value.
     *
     * The operand table (userfuncs.c) has carried its layout for a while with
     * nothing behind it: origin at +4 as three s32, `radius` at +18 "tripled
     * before the call", and a packed colour at +24. The triple is the engine's,
     * not a guess. The colour's own consumer is 0x80075D14; the low three bytes
     * are taken as r, g, b here, which is what every other packed colour on this
     * path does.
     *
     * FLKLIGHT is deliberately NOT handled: its on/off times are randomised as
     * ((rand()*500)>>15)+400, so it needs the engine's RNG stream to look right
     * rather than merely to appear.
     */
    /*
     * FLKLIGHT — registered once and then blinked by q2_flklights_tick. Origin
     * at +4, light_id at +16, colour bytes at +18/+19/+20 (userfuncs.c). It
     * needs phase, which is why it is a set rather than a transient like
     * TIMEDLIGHT below.
     */
    if (q2_userfuncs_prim(&c->sim[0].userfuncs, call_index) == Q2_UF_FLKLIGHT
        && item->len >= 24 && item->payload) {
        const u8 *p = item->payload - 2;
        s32 at[3];
        u8  rgb[3];

        at[0]  = (s32)q2_rd_u32(p + 4);
        at[1]  = (s32)q2_rd_u32(p + 8);
        at[2]  = (s32)q2_rd_u32(p + 12);
        rgb[0] = q2_rd_u8(p + 18);
        rgb[1] = q2_rd_u8(p + 19);
        rgb[2] = q2_rd_u8(p + 20);

        if (q2_flklight_add(&c->flklights, at, rgb, (s16)q2_rd_u16(p + 16),
                            &c->sim[0].combat.rng, c->sim[0].level_time))
            c->script_lights++;
    }

    if (q2_userfuncs_prim(&c->sim[0].userfuncs, call_index) == Q2_UF_TIMEDLIGHT
        && item->len >= 28 && item->payload) {
        const u8 *p = item->payload - 2;
        s32 at[3];
        u8  rgb[3];
        u32 packed;
        s32 radius;

        at[0]  = (s32)q2_rd_u32(p + 4);
        at[1]  = (s32)q2_rd_u32(p + 8);
        at[2]  = (s32)q2_rd_u32(p + 12);
        radius = (s32)q2_rd_u16(p + 18) * 3;
        packed = q2_rd_u32(p + 24);
        rgb[0] = (u8)(packed & 0xFF);
        rgb[1] = (u8)((packed >> 8) & 0xFF);
        rgb[2] = (u8)((packed >> 16) & 0xFF);

        if (radius > 0)
            q2_ent_light_at(&c->sim[0].ent_world.events, at, rgb, 0, radius);
        c->script_lights++;
    }
}


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

                    /*
                     * A deathmatch starts at a MultiSpawn, chosen the way the
                     * original chooses one — the farthest from everybody who is
                     * already standing somewhere, with ties broken by the RNG.
                     * The eight names are fixed (`MultiSpawn0`..`MultiSpawn7`)
                     * and only an arena carries any.
                     */
                    if (c->mp_enabled) {
                        q2_mp_spawn ms[Q2_MP_MAX_SPAWNS];
                        u32 n = 0;

                        memset(ms, 0, sizeof(ms));
                        for (i = 0; i < spawns.count && n < Q2_MP_MAX_SPAWNS; i++) {
                            q2_start_pos sp;

                            if (!q2_start_pos_get(&spawns, i, &sp))
                                continue;
                            if (sp.zone != index)
                                continue;
                            if (strncmp(sp.name, "MultiSpawn", 10) != 0)
                                continue;

                            ms[n].pos[0]  = sp.x;
                            ms[n].pos[1]  = sp.y;
                            ms[n].pos[2]  = sp.z;
                            ms[n].angle   = sp.angle;
                            ms[n].present = true;
                            n++;
                        }

                        c->mp_spawn_count = n;
                        if (n) {
                            q2_mp_player_view pv[Q2_MP_MAX_PLAYERS];
                            int players = c->mp.player_count;
                            int pi;

                            memset(pv, 0, sizeof(pv));
                            if (players < 1)
                                players = 1;

                            /*
                             * Every player, not just the local one, and each
                             * placed AGAINST the ones already placed — which is
                             * what the selector is for: it takes the spawn
                             * farthest from everybody standing somewhere, so
                             * four players spread out instead of piling onto
                             * whichever point happens to be first.
                             */
                            for (pi = 0; pi < players; pi++) {
                                int pick = q2_mp_select_spawn(ms, pv,
                                                              (u32)pi,
                                                              client_mp_rng, c);

                                if (pick < 0)
                                    break;

                                c->mp_view_pos[pi][0] = ms[pick].pos[0];
                                c->mp_view_pos[pi][1] = ms[pick].pos[1];
                                c->mp_view_pos[pi][2] = ms[pick].pos[2];
                                c->mp_view_yaw[pi]    = ms[pick].angle;
                                c->mp_view_valid[pi]  = true;

                                pv[pi].alive  = true;
                                pv[pi].pos[0] = ms[pick].pos[0];
                                pv[pi].pos[1] = ms[pick].pos[1];
                                pv[pi].pos[2] = ms[pick].pos[2];

                                if (pi == 0) {
                                    c->cam.pos[0] = ms[pick].pos[0];
                                    c->cam.pos[1] = ms[pick].pos[1];
                                    c->cam.pos[2] = ms[pick].pos[2];
                                    c->cam.yaw    = ms[pick].angle;
                                    placed = true;
                                }
                                Q2_INFO("deathmatch: player %d at MultiSpawn %d",
                                        pi, pick);
                            }
                            Q2_INFO("deathmatch: %u MultiSpawn points on %s",
                                    n, map);
                        } else {
                            Q2_WARN("deathmatch: %s zone %d has no MultiSpawn "
                                    "points — this is not an arena", map, index);
                        }
                    }

                    for (i = 0; !placed && i < spawns.count; i++) {
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

                /*
                 * The briefing's three fields, out of the map's own `Strings`
                 * chunk (leveltext.h). `MapTitle` is the location; the orders
                 * and the objective are keyed by unit number, which the game
                 * knows and the port does not yet — so the first key that
                 * resolves is taken, which for a single-unit map is the right
                 * one and for a shared directory is the lowest unit present.
                 */
                {
                    q2_leveltext tx;

                    q2_briefing_init(&c->briefing);
                    if (q2_leveltext_open(&tx, &c->common) == Q2_OK) {
                        char key[Q2_LEVELTEXT_NAME_LEN + 1];
                        const char *s2;
                        int unit, step;

                        s2 = q2_leveltext_find(&tx, "MapTitle");
                        if (s2)
                            q2_briefing_set_location(&c->briefing, s2);

                        for (unit = 1; unit <= 9; unit++) {
                            q2_leveltext_key_objective(key, unit);
                            s2 = q2_leveltext_find(&tx, key);
                            if (s2) {
                                q2_briefing_set_objective(&c->briefing, s2);
                                break;
                            }
                        }
                        for (unit = 1; unit <= 9; unit++) {
                            bool got = false;
                            for (step = 0; step <= 15; step++) {
                                q2_leveltext_key_orders(key, unit, step);
                                s2 = q2_leveltext_find(&tx, key);
                                if (s2) {
                                    q2_briefing_set_orders(&c->briefing, s2);
                                    got = true;
                                    break;
                                }
                            }
                            if (got)
                                break;
                        }
                    }
                }
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
            c->clut4_count_a = vs.clut4_count_a;

            /*
             * The UI's own images, into the cells their registration slots
             * name (0x8003FE20): `frontend.lbm` for the menu's 16- and
             * 32-pixel faces, `chars.lbm` for the 8-pixel face and the HUD's
             * atlas, and the icon sheet. Three maps carry no `frontend.lbm`
             * and two no `chars.lbm`, so this is allowed to come back empty —
             * the menu then has no letterforms and says so once.
             */
            c->menu_font_ready = false;
            if (c->hud_tables_ready) {
                /*
                 * WHICH icon sheet is a session question, and asking the wrong
                 * one fails silently: `q2_menu_icons_name` picks `qk_menu.lbm`
                 * in single player, `qk2_menu.lbm` for a two-player match and
                 * `qkm_menu.lbm` for three or four, and an arena carries only
                 * the multiplayer ones. The upload still returns Q2_OK because
                 * the atlases went in, so `menu_font_ready` was true, the
                 * status bar drew into an empty texture page, and every arena
                 * on the disc showed no health, no armour and no ammo. See
                 * openquestions #52.
                 */
                int hud_players = c->mp_enabled ? c->mp.player_count : 1;
                q2_result fr = q2_menu_font_upload(&c->menu_font,
                                                   &c->hud_tables, &vs,
                                                   c->vram,
                                                   c->mp_enabled, hud_players);
                c->menu_font_ready = (fr == Q2_OK);
                if (!c->menu_font_ready)
                    Q2_WARN("%s carries no menu font", map);
                if (!c->menu_font.icons_resident)
                    Q2_WARN("%s carries no '%s' — the status bar will be blank",
                            map, q2_menu_icons_name(c->mp_enabled,
                                                    hud_players));

                /* The overlay's own view of the same atlas. It re-uploads
                 * chars.lbm, which is harmless — the same halfwords to the
                 * same place — and gives the markup layer its palettes. */
                c->hud_font_ready =
                    (q2_hud_font_upload(&c->hud_font, &c->hud_tables, &vs,
                                        c->vram) == Q2_OK);
            }

            q2_vram_free(&vs);
        } else {
            c->opts.textures = false;
            c->menu_font_ready = false;
        }
    }

    /* The same file's second section: the map's sound bank, which is where the
     * menu's five effects live. */
    if (c->sfx_ready) {
        q2_sound_bank_free(&c->sfx);
        c->sfx_ready = false;
    }
    c->sfx_ready = (q2_sound_bank_load(&c->sfx, c->disc, map) == Q2_OK);
    if (c->sfx_ready)
        Q2_INFO("sound bank: %u effects", c->sfx.count);

    /* q2_sim_init memsets the struct, so the previous zone's trigger bitmap and
     * event runtime have to be released first or they leak on every zone
     * change -- and zone changes are exactly what the gates now cause. */
    q2_sim_free(&c->sim[0]);
    q2_sim_init(&c->sim[0], &c->zone, q2_build_tick_rate(&c->build));
    {
        s32 feet[3];
        feet[0] = c->cam.pos[0];
        feet[1] = c->cam.pos[1];
        feet[2] = c->cam.pos[2];
        q2_sim_attach_gameplay(&c->sim[0], &c->common);

        /*
         * The map's model bank, and the view weapon that draws out of it. The
         * weapon starts already raised, which is what a level start does — the
         * machine's own reset lands in RAISE at frame 0.
         */
        c->model_bank_ready =
            (q2_model_bank_from_common(&c->model_bank, &c->common) == Q2_OK);

        /*
         * The things standing in the room when you arrive.
         *
         * Population's place records are the map's items, and until now the
         * client was the one caller that never spawned them: the sim had the
         * entity set, the thinks and the touch sweep, and the set was empty, so
         * every level was a walk through an empty building.
         *
         * It goes AFTER the bank is opened because the bank is what resolves
         * each item's model at spawn, which is where the engine resolves it
         * (0x80058850) — an item whose model this map does not ship never
         * spawns at all rather than being looked up mid-frame. And after
         * q2_sim_attach_gameplay because a place list is per MAP, exactly as
         * the triggers and the script are, and both come out of the same
         * COMMON.DAT this call borrows.
         *
         * The player is registered by the attach and moved every tick, so the
         * touch sweep works from the spawn below without anything here having
         * to order the two.
         *
         * The zone goes in because Population is per MAP and a session is in
         * one ZONE: without it a map's other four zones' items stand around in
         * this one. What that can and cannot decide is q2_item_spawn_zone's.
         */
        {
            q2_result ir = q2_sim_attach_items(
                &c->sim[0], &c->common, index,
                c->item_table_ready ? &c->item_table : NULL,
                c->model_bank_ready ? &c->model_bank : NULL);

            if (ir == Q2_OK)
                Q2_INFO("items: %u placed", c->sim[0].entities.count);
            else
                Q2_WARN("%s places no items: %s", map, q2_result_str(ir));
        }

        if (c->vm_ready) {
            q2_vw_init(&c->vw, &c->vm_tables, c->sim[0].combat.weapon_id);
            c->vw_last_weapon = c->sim[0].combat.weapon_id;
            client_bind_view_model(c);
        }
        /* The zone number seeds the effect generator, so re-entering a zone
         * looks the same twice and two zones do not share a sequence. */
        if (c->fx_tables_ready) {
            q2_sim_attach_effects(&c->sim[0], &c->fx_tables,
                                  0x51A5E5u + (u32)index);

            /*
             * The particle quads live on `chars.lbm`'s page, so the overlay's
             * atlas is also the effect atlas. Without this they fall back to
             * flat quads; with it they are the console's own textured ones.
             */
            if (c->hud_font_ready)
                q2_fx_use_hud_atlas(&c->sim[0].fx, &c->hud_font);

            /*
             * The glint mesh is the map's own `GlintMod` chunk, and only BIGGUN
             * has one. A map without it simply has no glint.
             */
            q2_sim_attach_glint(&c->sim[0], &c->common);
        }
        /*
         * The zone's lights: COMMON.DAT's `Lights` array and the zone's own
         * per-node index lists, which is exactly the pair `q2_light_gather`
         * wants. SpaceLights is partitioned by the SECONDARY collision node, so
         * it is opened against the hull the sim already has.
         */
        c->lights_ready = false;
        if (c->sim[0].coll_ready &&
            q2_lights_parse(&c->lights, &c->common) == Q2_OK &&
            q2_spacelights_open(&c->spacelights, &c->zone.zone,
                                &c->sim[0].coll) == Q2_OK) {
            memset(&c->light_world, 0, sizeof(c->light_world));
            c->light_world.statics = &c->lights;
            c->light_world.space   = &c->spacelights;
            c->lights_ready = true;
            Q2_INFO("lights: %u in the map, %u index entries",
                    c->lights.count, c->spacelights.count);
        }

        /*
         * The map's rotating brushes. Built from the same Events and UserFuncs
         * the movers come from, and handed to the zone, which adds each node's
         * rotation when it draws it.
         */
        if (c->rotators_ready) {
            q2_rotators_free(&c->rotators);
            c->rotators_ready = false;
        }
        {
            q2_events    ev;
            q2_userfuncs uf;

            q2_events zev;
            bool have_zev = (q2_events_parse_zone(&zev, &c->zone.zone) == Q2_OK);

            if (q2_events_parse_common(&ev, &c->common) == Q2_OK &&
                q2_userfuncs_parse(&uf, &c->common) == Q2_OK) {
                /*
                 * A rotation CALL's object slots are read from the ZONE's Events
                 * chunk at the same offset, not from COMMON's -- 0x800285F4
                 * rebases the cursor into gp+376, which the zone loader fills at
                 * 0x8007C234. Reading COMMON's alone sees the -1 the engine
                 * stamps there as it consumes each slot, which left most of the
                 * disc's rotating geometry inert. See openquestions #56.
                 */
                if (have_zev)
                    q2_rotators_set_operand_source(&c->rotators, ev.data,
                                                   zev.data, zev.size);
                if (q2_rotators_build(&c->rotators, &ev, &uf) == Q2_OK) {
                    c->rotators_ready = true;
                    c->zone.rotators  = &c->rotators;
                    Q2_INFO("rotators: %u (operands from %s)",
                            c->rotators.count,
                            have_zev ? "the zone's Events" : "COMMON only");
                }
            }
        }

        /*
         * And the other half of a rotator: the step request. Building the set
         * only says which nodes CAN turn — every kind sits still until a script
         * CALL asks for a step (rotator.c, 0x8002F1B8), which is why the set
         * built last round reported `rot moved 0` on every map.
         */
        c->sim[0].event_rt.on_call      = client_event_call;
        c->sim[0].event_rt.on_call_user = c;

        q2_sim_spawn(&c->sim[0], feet, c->cam.yaw);
        c->sim[0].player[0].ground_y = feet[1];
        c->sim[0].combat.self.owner  = 0;

        /*
         * The other players. Each gets its own sim, standing at its own
         * MultiSpawn, and from here on each moves under its own pad — so a
         * split-screen viewport shows a player walking rather than a fixed
         * camera parked at a spawn point.
         *
         * Their world halves run and are ignored: each instance spawns its own
         * copy of the map's items and runs its own script, and nothing reads or
         * draws any of it. That is the cost of the player living inside q2_sim,
         * and it is a cost rather than a bug — the duplicate worlds are
         * invisible and self-consistent. Question 53 is the fix.
         */
        if (c->mp_enabled) {
            int pi;

            for (pi = 1; pi < c->mp.player_count &&
                         pi < Q2_MP_MAX_PLAYERS; pi++) {
                s32 pfeet[3];

                if (!c->mp_view_valid[pi])
                    continue;

                pfeet[0] = c->mp_view_pos[pi][0];
                pfeet[1] = c->mp_view_pos[pi][1];
                pfeet[2] = c->mp_view_pos[pi][2];

                /*
                 * Into player 0's sim, as player `pi` — one world, four
                 * players. Each used to get a q2_sim of its own, which meant
                 * four copies of the map's items and four scripts, and only
                 * player 0's was ever read or drawn. Now they share the world
                 * they are standing in, which is what lets them collect the
                 * same pickup and see the same doors.
                 */
                {
                    /*
                     * Through the sim's own spawn, not by copying player 0.
                     * Copying carried player 0's collision node across, and a
                     * node is where you ARE — so a player placed somewhere else
                     * with someone else's node fell out of the world. Two of
                     * four ended a capture at y 64847.
                     */
                    int saved = c->sim[0].cur_player;

                    c->sim[0].cur_player = pi;
                    q2_sim_spawn(&c->sim[0], pfeet, c->mp_view_yaw[pi]);
                    c->sim[0].player[pi].ground_y = pfeet[1];
                    c->sim[0].cur_player = saved;
                }
                q2_sim_player_reset_combat(&c->sim[0], pi);
                c->sim[0].pcombat[pi].self.owner = (s8)pi;
                c->sim[0].player_count = pi + 1;
                c->sim_ready[pi] = true;
            }
        }

        /* Last, because it wakes the AI onto the player and therefore needs
         * the player to already be standing somewhere. */
        {
            s32 eye[3];
            q2_sim_eye(&c->sim[0], eye);
            client_load_creatures(c, eye);
        }
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

/*
 * Play a track id, which is the unit the game thinks in.
 *
 * An id below 2 is silence — the table's `file` is negative there and the
 * player's own `bltz` at 0x80071778 takes a different arm entirely — so this
 * stops rather than pretending.
 */
static bool client_music_play_id(client *c, int id)
{
    const q2_music_entry *e = q2_music_get(&c->music_table, id);

    if (!c->music_table_ready || !e || e->file < 0 ||
        e->file >= Q2_MUSIC_FILES) {
        c->music_open = false;
        return false;
    }

    c->music_id = id;
    return client_music_start(c, q2_music_files[e->file][6],
                              (u8)e->channel);
}

/*
 * The next track in this level's playlist, by the engine's own walk. Called
 * when a stream runs out, which is the only thing that advances it.
 */
static void client_music_advance(client *c)
{
    int id;

    if (!c->level || !c->music_table_ready) {
        /* No playlist: loop what is playing, which is what the port did before
         * any of this was decoded. */
        c->music_cursor = 0;
        q2_xa_decoder_reset(&c->music_dec);
        return;
    }

    id = q2_level_playlist_next(c->level, &c->music_cursor_at);
    if (id < 0) {
        /* A list that ends rather than looping: restart it. */
        c->music_cursor_at = -1;
        id = q2_level_playlist_next(c->level, &c->music_cursor_at);
    }

    if (id < 0 || !client_music_play_id(c, id)) {
        c->music_cursor = 0;
        q2_xa_decoder_reset(&c->music_dec);
    }
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
            /* End of stream: the playlist advances. The engine's walk is what
             * decides what comes next, and for every real level it eventually
             * jumps back and starts the seven again. */
            client_music_advance(c);
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
/*
 * The pad, when the client is driving itself.
 *
 * A demo is not a recording — nothing on the disc is being replayed — it is a
 * fixed button script, so that `--headless --demo` produces the same frames on
 * every machine and a captured frame can be compared against the last one. The
 * cycle walks, shoots, turns and jumps, because those are the four things that
 * reach the most systems: the mover and the hull trace, the weapon state
 * machine and the projectile list, the view's own kick decay, and the ground
 * projection that fall damage is measured off.
 */
#define CLIENT_DEMO_PERIOD 150

static u16 client_demo_pad(long frame)
{
    long t = frame % CLIENT_DEMO_PERIOD;
    u16  pad = 0;

    if (t >=  15 && t <  75) pad |= Q2_PAD_UP;       /* walk forward     */
    if (t >=  45 && t <  56) pad |= Q2_PAD_CROSS;    /* and shoot        */
    if (t >=  78 && t <  90) pad |= Q2_PAD_RIGHT;    /* turn             */
    if (t >=  60 && t <  75) pad |= Q2_PAD_R2;       /* strafe, for the  */
                                                     /* lean it rolls    */
    if (t >=  95 && t <  98) pad |= Q2_PAD_TRIANGLE; /* next weapon      */
    if (t >= 112 && t < 122) pad |= Q2_PAD_CROSS;    /* shoot again      */
    if (t >= 130 && t < 134) pad |= Q2_PAD_SQUARE;   /* a tap is a jump  */

    return pad;
}

/*
 * The pad this frame. The keyboard is wired to PAD BUTTONS, not to the input
 * record, and the mapping from those to the record is 0x80019154's — see pad.h.
 *
 * This is not ceremony. Three things the player feels are decided in there
 * rather than here: full deflection is 127 and not 128, so the walk speed is
 * the console's 2778 and not 2800; jump and swim-up come out of ONE button, a
 * tap for the former and a hold for the latter; and the configured style
 * decides whether the look rate is eased or set, which is the difference
 * between a view that glides and one that snaps.
 */
static u16 client_pad_mask(const client *c)
{
    const bool *keys;
    u16 pad = 0;

    if (c->demo)
        return client_demo_pad(c->frame_index);

    keys = SDL_GetKeyboardState(NULL);
    if (!keys)
        return 0;

    if (keys[SDL_SCANCODE_W])     pad |= Q2_PAD_UP;
    if (keys[SDL_SCANCODE_S])     pad |= Q2_PAD_DOWN;
    if (keys[SDL_SCANCODE_A])     pad |= Q2_PAD_L2;
    if (keys[SDL_SCANCODE_D])     pad |= Q2_PAD_R2;
    if (keys[SDL_SCANCODE_LEFT])  pad |= Q2_PAD_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) pad |= Q2_PAD_RIGHT;

    /* R1 looks down and L1 up, and holding BOTH is the chord that walks the
     * pitch back to level — the console's own recentre, which is why there is no
     * separate key for it. */
    if (keys[SDL_SCANCODE_DOWN])  pad |= Q2_PAD_R1;
    if (keys[SDL_SCANCODE_UP])    pad |= Q2_PAD_L1;

    if (keys[SDL_SCANCODE_SPACE]) pad |= Q2_PAD_SQUARE;   /* jump/swim */
    if (keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_F])
        pad |= Q2_PAD_CROSS;                              /* fire      */

    if (keys[SDL_SCANCODE_RIGHTBRACKET] || keys[SDL_SCANCODE_E])
        pad |= Q2_PAD_TRIANGLE;                           /* weap +    */
    if (keys[SDL_SCANCODE_LEFTBRACKET] || keys[SDL_SCANCODE_Q])
        pad |= Q2_PAD_CIRCLE;                             /* weap -    */

    return pad;
}

/* True while a debug key that stands in for a level's own volume is held. The
 * demo never holds one, and a headless run has no keyboard to ask. */
static bool client_key_down(const client *c, SDL_Scancode a, SDL_Scancode b)
{
    const bool *keys;

    if (c->demo || c->headless)
        return false;

    keys = SDL_GetKeyboardState(NULL);
    return keys && (keys[a] || keys[b]);
}

static void client_input_simulated(client *c, float dt)
{
    q2_input in;
    s32 eye[3], view[3];

    static q2_pad_state pad;
    q2_pad_config       cfg;

    pad.prev    = pad.buttons;
    pad.buttons = client_pad_mask(c);

    q2_pad_config_default(&cfg);
    cfg.style = c->sim[0].player[0].look_scheme;

    q2_pad_read(&pad, &cfg, &in);

    (void)dt;

    /*
     * Crouch is not an input on the console — INCROUCH and INLOWCROUCH are event
     * script primitives a trigger volume runs, so where you crouch is authored
     * per map. The key drives the same environment flag the dispatcher would set,
     * which is the honest way to keep a debug crouch without inventing a mechanic.
     */
    c->sim[0].env_flags &= ~(u32)(Q2_ENT_INCROUCH | Q2_ENT_INLOWCROUCH);
    if (client_key_down(c, SDL_SCANCODE_LCTRL, SDL_SCANCODE_C))
        c->sim[0].env_flags |= Q2_ENT_INLOWCROUCH;

    /*
     * Being submerged is the same kind of thing and is held the same way. The
     * map's own water volumes now work on their own — the sim resolves a
     * volume's record to its UserFuncs primitive at load — so this is no longer
     * the only source of the flag, just the one that does not need you to go
     * and find water. F3 holds it on (see the key handler), which drives both
     * the swimming physics and the water screen effect.
     */
    c->sim[0].env_flags &= ~(u32)(Q2_ENT_INWATER | Q2_ENT_UNDERWATER);
    if (c->force_underwater)
        c->sim[0].env_flags |= Q2_ENT_INWATER | Q2_ENT_UNDERWATER;

    /*
     * Weapon switching. The edge is the PAD's now — bits 26 and 27 are already
     * press edges out of q2_pad_read, so the "was it down last frame" bookkeeping
     * this used to do by hand is the shared tail's job and happens once for every
     * button rather than once per key.
     */
    /*
     * `--watch` aims the PLAYER, and it has to happen BEFORE the tick: the shot
     * is taken inside `q2_sim_advance`, so an aim written after it applies to
     * the frame after the one that fired.
     */
    if (c->watch && c->creatures_ready) {
        const q2_monster *best = NULL;
        s64 best_d = 0;
        s32 eye0[3];
        u32 i;

        q2_sim_eye(&c->sim[0], eye0);

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s64 dx, dy, dz, d;

            if (!m->in_use || m->dead || !c->cre_model_ok[i])
                continue;

            dx = m->pos[0] - eye0[0];
            dy = m->pos[1] - eye0[1];
            dz = m->pos[2] - eye0[2];
            d  = dx * dx + dy * dy + dz * dz;
            if (!best || d < best_d) { best = m; best_d = d; }
        }

        if (best) {
            s32 to[3];
            double horiz, p;

            /*
             * Stand the PLAYER in front of it as well, 700 units along the
             * creature's own facing and at head height — the same framing the
             * camera uses below.
             *
             * Without this the demo can only shoot from wherever it wandered
             * to, and on BASE1 that is a floor above: aiming correctly then
             * put every bolt into the floor between them, which is geometry
             * and not a combat fault. A test of whether the player can hurt a
             * creature has to be able to see one.
             */
            c->sim[0].player[0].pos[0] = best->pos[0] +
                ((q2_sin12(best->angles[2]) * 700) >> Q2_FRAC_12);
            c->sim[0].player[0].pos[1] = best->pos[1];
            c->sim[0].player[0].pos[2] = best->pos[2] +
                ((q2_cos12(best->angles[2]) * 700) >> Q2_FRAC_12);
            q2_sim_eye(&c->sim[0], eye0);

            to[0] = best->pos[0] - eye0[0];
            to[1] = best->pos[1] - eye0[1] - 150;
            to[2] = best->pos[2] - eye0[2];

            horiz = sqrt((double)to[0] * to[0] + (double)to[2] * to[2]);
            p = atan2((double)to[1], horiz > 1.0 ? horiz : 1.0);

            c->sim[0].player[0].yaw   = (s16)q2_vectoyaw(to);
            c->sim[0].player[0].pitch = (s16)(s32)(p * (double)Q2_ANGLE_360 /
                                             (2.0 * 3.14159265358979323846));
        }
    }

    if (in.buttons & Q2_BTN_WEAP_NEXT) q2_sim_cycle_weapon(&c->sim[0], +1);
    if (in.buttons & Q2_BTN_WEAP_PREV) q2_sim_cycle_weapon(&c->sim[0], -1);

    /*
     * The creatures, published to combat as actors before the tick that may
     * shoot one, and read back after it.
     *
     * They are two structures because they are two things: `q2_monster` is what
     * the AI drives and `q2_actor` is what the damage function at 0x80057D54
     * operates on, and combat.h supplies the pair of converters precisely so
     * neither has to know about the other. Syncing on both sides of the tick is
     * what makes a monster that has walked somewhere shootable where it now is,
     * and a monster that has been shot notice.
     */
    if (c->creatures_ready && c->cre_actor) {
        u32 i;
        for (i = 0; i < c->creatures.set.count; i++)
            q2_actor_from_monster(&c->cre_actor[i],
                                  &c->creatures.set.monsters[i]);
    }

    if (in.attack) c->player_attacks++;

    /*
     * `--dm-stage`: put the other players in front of player 0 and point
     * everyone at each other, with fire held.
     *
     * The same reason `--watch` exists. A scripted demo wanders; it does not
     * arrange a fight, and four players scattered across an arena firing
     * blindly produced no hits in 1200 frames — which says nothing about
     * whether a hit would have registered. This stages the encounter so the
     * scoring path can be exercised rather than reasoned about, and it is a
     * harness, not gameplay.
     */
    if (c->mp_stage && c->mp_enabled) {
        int pi;
        s32 eye0[3];

        q2_sim_eye(&c->sim[0], eye0);

        for (pi = 1; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_player *pl;

            if (!c->sim_ready[pi])
                continue;

            pl = &c->sim[0].player[pi];

            /*
             * IN FRONT of player 0, along the way they are facing — not at a
             * blind diagonal offset, which is what the first version did and
             * which put a wall between them: the scan counted 44 shots stopped
             * by the world before reaching a target 339 units away. Player 0
             * walked to where they are, so the space ahead of them is space
             * they can see, the same reasoning `--watch` uses to frame a
             * creature.
             */
            {
                /* Close enough that a bolt connects often: the actors' reach
                 * is 286 + 286, so a few hundred units apart makes the target
                 * subtend a wide angle and the staged exchange conclusive in a
                 * capture short enough to run. */
                s32 fwd = 360 + 120 * pi;

                pl->pos[0] = c->sim[0].player[0].pos[0] +
                    ((q2_sin12(c->sim[0].player[0].yaw) * fwd) >> Q2_FRAC_12);
                pl->pos[1] = c->sim[0].player[0].pos[1];
                pl->pos[2] = c->sim[0].player[0].pos[2] +
                    ((q2_cos12(c->sim[0].player[0].yaw) * fwd) >> Q2_FRAC_12);
            }
            pl->ent.node = c->sim[0].player[0].ent.node;
            /*
             * Aimed at player 0's POSITION, not at the reverse of their
             * facing. The first version set `yaw + 2048`, which points a player
             * back down player 0's own line of sight and only coincides with
             * pointing AT them when player 0 happens to be looking at the
             * spot — 900 frames of that produced no hits at all.
             */
            {
                s32 v[3];

                v[0] = c->sim[0].player[0].pos[0] - pl->pos[0];
                v[1] = 0;
                v[2] = c->sim[0].player[0].pos[2] - pl->pos[2];

                pl->yaw   = q2_vectoyaw(v);
                pl->pitch = 0;
            }

            /*
             * The hurt-actor's origin is NOT set here. The sim maintains it
             * every tick, at the eye, and writing the feet over it each frame
             * put the target 572 units — two eye-heights — below the muzzle
             * and made every bolt miss. A harness that overwrites the field it
             * is measuring measures the harness.
             */
        }

        /*
         * And player 0's own aim is HELD too: fire on, no look input. Holding
         * only the extra players' aim is what made three code changes produce
         * byte-identical counters — player 0 was doing all the shooting, being
         * aimed at the top of each frame and turning away inside its own tick.
         */
        in.attack   = true;
        in.buttons |= Q2_BTN_ATTACK_PRESS;
        in.yaw      = 0;
        in.pitch    = 0;

        /* And player 0 looks back at the first of them. */
        if (c->sim_ready[1]) {
            s32 v[3];

            v[0] = c->sim[0].player[1].pos[0] - c->sim[0].player[0].pos[0];
            v[1] = 0;
            v[2] = c->sim[0].player[1].pos[2] - c->sim[0].player[0].pos[2];

            c->sim[0].player[0].yaw   = q2_vectoyaw(v);
            c->sim[0].player[0].pitch = 0;
        }
    }

    if (c->mp_enabled)
        client_targets_for(c, 0);

    q2_combat_scan_who = c->mp_enabled ? 0 : Q2_COMBAT_SCAN_OTHER;
    q2_sim_advance(&c->sim[0], &in, (double)dt);
    q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;
    client_sync_parked_health(c);
    client_score_deaths(c);

    /*
     * The other players, each on its own pad. In a headless demo run there is
     * one script, so each is given a rotated slice of it — otherwise four
     * players would walk in lockstep and a split screen would show one man
     * reflected four times, which proves nothing about four sims running.
     */
    {
        int pi;

        for (pi = 1; pi < Q2_MP_MAX_PLAYERS; pi++) {
            q2_input pin;

            if (!c->sim_ready[pi])
                continue;

            pin = in;
            if (c->demo) {
                /* Each player reads the same script at a different phase, so
                 * four sims produce four walks rather than one reflected. */
                q2_pad_config pcfg;

                c->mp_pad[pi].prev    = c->mp_pad[pi].buttons;
                c->mp_pad[pi].buttons =
                    client_demo_pad((long)c->frame_index + (long)pi * 37);

                q2_pad_config_default(&pcfg);
                /* Player `pi` lives in sim[0] now; `sim[pi]` has been an
                 * uninitialised struct since they moved there. */
                pcfg.style = c->sim[0].player[pi].look_scheme;
                q2_pad_read(&c->mp_pad[pi], &pcfg, &pin);
            }

            {
                s32 ticks = (s32)((double)dt * 300.0 + 0.5);

                if (ticks < 1)
                    ticks = 1;
                if (ticks > 30)
                    ticks = 30;      /* the same clamp q2_sim_advance applies */
                if (c->mp_stage) {
                    pin.attack   = true;
                    pin.buttons |= Q2_BTN_ATTACK_PRESS;

                    /*
                     * And no look input: the aim is written before the tick and
                     * `update_look` would turn them off it before the shot is
                     * taken inside the same tick. The scan counted 1988 shots
                     * with the target BEHIND the muzzle — a staged player was
                     * being aimed and then immediately turning away.
                     */
                    pin.yaw   = 0;
                    pin.pitch = 0;
                }
                client_targets_for(c, pi);
                q2_combat_scan_who = pi;
                q2_sim_advance_player(&c->sim[0], pi, &pin, ticks);
                q2_combat_scan_who = Q2_COMBAT_SCAN_OTHER;

                /*
                 * Did that player's frame actually take a shot? `last_shot` is
                 * part of the swapped half, so after the tick it is parked in
                 * that player's slot. Counting it is what tells "the shot
                 * missed" apart from "no shot was ever fired", and those want
                 * very different fixes.
                 */
                if (c->sim[0].pcombat[pi].last_shot.fired)
                    c->mp_shots[pi]++;
                else if (c->sim[0].pcombat[pi].last_shot.dry)
                    c->mp_dry[pi]++;
                client_sync_parked_health(c);
                client_score_deaths(c);
            }
        }
    }

    if (c->creatures_ready && c->cre_actor) {
        u32 i;
        for (i = 0; i < c->creatures.set.count; i++) {
            q2_monster *m    = &c->creatures.set.monsters[i];
            bool        was  = m->dead;

            q2_actor_to_monster(&c->cre_actor[i], m);

            /*
             * The frame it died on. T_Damage ends by calling the entity's own
             * `die` (entity+0xA4, 0x80062A9C); what can be reconstructed from
             * the module's data rather than its code is the animation, so the
             * body is put into its death move and left to play it out.
             *
             * Without this a killed creature simply vanished — the tick and the
             * draw both skipped anything with `dead` set, so a Soldier shot
             * dead was gone on the frame it died.
             */
            if (!was && m->dead) {
                s32 f = q2_creature_world_death_frame(&c->creatures, m);

                if (f >= 0 && q2_cre_set_move(m, f)) {
                    m->frame     = (s16)f;
                    c->cre_bodies++;
                }
            }
        }
    }

    /*
     * What the items did while that ran. Immediately after the tick, because
     * the event list is cleared at the top of the next one.
     */
    client_entity_events(c);

    /*
     * The weapon in the hands, advanced on the same clock. The selection comes
     * from the simulation, but the SWAP does not happen when the selection
     * changes — it happens when the lower clip has run and the 70-tick countdown
     * has expired, which is the machine's job, not this caller's.
     */
    if (c->vm_ready) {
        s32 ticks = (s32)((double)dt * 300.0 + 0.5);
        bool swapped;

        if (ticks < 1) ticks = 1;
        if (ticks > Q2_SCREEN_DT_MAX) ticks = Q2_SCREEN_DT_MAX;

        if (c->sim[0].combat.weapon_id != c->vw_last_weapon) {
            q2_vw_select(&c->vw, c->sim[0].combat.weapon_id);
            c->vw_last_weapon = c->sim[0].combat.weapon_id;
        }

        /*
         * What the sim's own shot did, rather than an unconditional "it fired".
         * `combat.last_shot.dry` is "out of ammo", which is the machine's
         * `Q2_VW_FIRE_DENIED` (viewweapon.h) — and it is what makes an empty
         * gun auto-switch instead of clicking. `fired` is false for an ordinary
         * refire wait too, so it is the wrong flag to test.
         */
        swapped = q2_vw_advance(&c->vw, ticks, in.attack,
                                (in.attack && c->sim[0].combat.last_shot.dry)
                                    ? Q2_VW_FIRE_DENIED : Q2_VW_FIRED);
        if (swapped)
            client_bind_view_model(c);

        /*
         * The machine's two outputs, neither of which anything had ever
         * drained. `q2_vw_take_refire`, `q2_vw_take_event` and
         * `q2_vw_wants_fire` were all declared, implemented and never called.
         *
         * The refire signal is the pass on which the original recomputes the
         * player's next and previous weapons (0x8004FB5C), so draining it is
         * what makes running dry switch you off the empty gun. The event is the
         * animation's own — a muzzle flash or a shell eject on the frame the
         * clip says, not the frame the trigger was pressed.
         */
        if (q2_vw_take_refire(&c->vw))
            q2_sim_cycle_weapon(&c->sim[0], +1);

        {
            s16 ev;
            if (q2_vw_take_event(&c->vw, &ev)) {
                c->vw_events++;
                c->vw_last_event = ev;
            }
        }
    }

    /* The overlay ages on logic ticks, not on drawn frames — one notification
     * retires every 60 (hud.h). The flash is the other way round and is
     * decremented inside q2_hud_build_ot. */
    if (c->hud_ready) {
        q2_hud_tick(&c->hud, 1);

        /*
         * The damage flash, fed the player's real condition. `q2_hud_track`
         * raises it when either figure FALLS, with armour taking precedence
         * exactly as the original's branch order does — grey for a hit the
         * armour took, red for one that reached flesh, and the asymmetric
         * strength arithmetic that gives an armour graze a fainter flash than
         * a solid hit (hud.h).
         *
         * This is the last thing the overlay was missing: it was built, it was
         * drawn, and nothing had ever told it how the player was doing.
         *
         * And raising it is only half of it. The overlay owns the arithmetic;
         * the TILE is the screen's, sized to the viewport and linked into that
         * viewport's own slice (screen.h), because on the console the two are
         * one record — the raise at 0x8003AE10 writes `ctx+0x2A0` and the draw
         * at 0x80076764 reads `view+672`, which are the same halfwords. Here
         * they are two structs, so the frame the flash is raised is the frame
         * it has to be handed over; after that the screen owns the countdown
         * and the overlay must not touch it.
         *
         * Viewport 0 because there is one player. A split-screen session would
         * hand each player's flash to its own viewport, which is exactly what
         * the shared record does for free on the console.
         */
        if (q2_hud_track(&c->hud, c->sim[0].combat.inv.health,
                         c->sim[0].combat.inv.armour))
            q2_screen_flash_set(&c->screen, 0, c->hud.flash.rgb,
                                c->hud.flash.strength, c->hud.flash.mode);
    }

    /*
     * The camera is NOT the player's aim. 0x80038260 composes three decaying
     * kicks — firing over 30 ticks, damage over 150, landing over 90 — on top of
     * the aim angles, and 0x8004F41C is where the result becomes the view. Using
     * `player.pitch/yaw/roll` straight, which this did, throws all three away:
     * no recoil, no flinch, and no thump when you land.
     */
    q2_sim_eye(&c->sim[0], eye);
    q2_sim_view_angles(&c->sim[0], view);

    /*
     * Drop a breadcrumb. The original writes one as the player moves, and the
     * AI's lost-you pursuit walks them backwards; ten frames apart is close
     * enough that a sixteen-slot ring covers the few seconds the pursuit
     * looks over.
     */
    if (c->creatures_ready && (c->frame_index % 10) == 0)
        q2_trail_add(eye, (s16)c->sim[0].player[0].yaw);

    /* The multiplayer session's own frame, on the same clock. */
    client_mp_tick(c, dt);

    /* The rotating brushes, on the same 1/300 s clock as everything else. */
    if (c->rotators_ready) {
        s32 ticks = (s32)((double)dt * 300.0 + 0.5);
        if (ticks < 1) ticks = 1;
        c->rot_moved += q2_rotators_tick(&c->rotators, ticks);
    }

    /* The AI, on its own clock and looking at where the player is now. */
    client_creatures_tick(c, dt, eye);

    /*
     * DEATH. Page 41 has been transcribed since the menu was reconstructed —
     * RESTART LEVEL, the resupply line with its own greying rule, QUIT GAME —
     * and nothing had ever opened it, so the player's health simply ran
     * negative and the game carried on. Measured before this: a Soldier took
     * the player to -353 and the run continued as if nothing had happened.
     *
     * It is raised here rather than inside the sim because the sim has no menu
     * and the page IS the death sequence on this console: the world stays
     * frozen behind it, which is what `client_menu_frame` already does for
     * every other page.
     */
    if (c->sim[0].combat.inv.health <= 0 && !c->menu.open && !c->mcard_open) {
        /*
         * In a match the death goes to the scoring first. The engine's hook at
         * 0x800396AC hands the module a killer and a victim, taken from the
         * entity's own bytes at +222 and +223; the port's single local player
         * is victim 0, and a creature or the world killed them, which the
         * attribution turns into the world's -1. The runtime's own first act is
         * then to blame the victim for it.
         */
        if (c->mp_enabled && c->mp.end == Q2_MP_RUNNING) {
            int killer = q2_mp_attribute_kill(-1, c->sim[0].combat.self.last_mod);

            q2_mp_player_killed(&c->mp, killer, 0);
            c->mp_deaths++;
            Q2_INFO("multiplayer: player 0 killed by %d, frags now %d %d %d %d",
                    killer, c->mp.frags[0], c->mp.frags[1], c->mp.frags[2],
                    c->mp.frags[3]);
        }

        Q2_INFO("player died");

        /* Every mode but VERSUS lets a dead player back in (0x8003DEB4). The
         * pad and menu gates the engine also applies belong to the client. */
        if (c->mp_enabled && q2_mp_may_respawn(&c->mp))
            Q2_INFO("multiplayer: respawn is allowed in this mode");

        q2_menu_open(&c->menu);
        q2_menu_goto(&c->menu, Q2_PAGE_DEATH);
    }

    c->cam.pos[0] = eye[0];
    c->cam.pos[1] = eye[1];
    c->cam.pos[2] = eye[2];
    c->cam.yaw    = view[1];
    c->cam.pitch  = view[0];
    c->cam.roll   = view[2];

    /*
     * `--watch`: turn the CAMERA, and only the camera, onto the nearest live
     * creature. The player still walks, shoots and is shot at; what changes is
     * where the view points, which is the one thing that decides whether a
     * creature the frame has already emitted is a creature you can see.
     *
     * It exists because "20 drawn, 4100 faces" says nothing about whether a
     * Soldier is standing in front of you: with an ordering table and no depth
     * buffer, a creature behind a wall is emitted and then painted over.
     */
    if (c->watch && c->creatures_ready) {
        const q2_monster *best = NULL;
        s64 best_d = 0;
        u32 i;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s64 dx, dy, dz, d;

            if (!m->in_use || m->dead || !c->cre_model_ok[i])
                continue;

            dx = m->pos[0] - eye[0];
            dy = m->pos[1] - eye[1];
            dz = m->pos[2] - eye[2];
            d  = dx * dx + dy * dy + dz * dz;
            if (!best || d < best_d) { best = m; best_d = d; }
        }

        if (best) {
            s32 to[3];

            /*
             * Stand in front of it, at head height, looking at it — the
             * inspector's `mob` framing, but of a LIVE creature: this one has
             * thought, turned, and is playing whatever move its AI put it in.
             * The camera moves and nothing else does; the player is still
             * where the simulation left them.
             */
            c->cam.pos[0] = best->pos[0] +
                            ((q2_sin12(best->angles[2]) * 700) >> Q2_FRAC_12);
            c->cam.pos[1] = best->pos[1] - 250;
            c->cam.pos[2] = best->pos[2] +
                            ((q2_cos12(best->angles[2]) * 700) >> Q2_FRAC_12);
            eye[0] = c->cam.pos[0];
            eye[1] = c->cam.pos[1];
            eye[2] = c->cam.pos[2];

            to[0] = best->pos[0] - eye[0];
            to[1] = best->pos[1] - eye[1] - 150;   /* look at the chest */
            to[2] = best->pos[2] - eye[2];

            double horiz = sqrt((double)to[0] * to[0] +
                                (double)to[2] * to[2]);
            double p = atan2((double)to[1], horiz > 1.0 ? horiz : 1.0);

            /*
             * The PLAYER is turned too, not just the camera. Without it the
             * demo fires on a timer into whatever it happens to be facing, so
             * a run could never show whether a shot hits a creature — measured
             * as 135 creature shots against the player and zero damage the
             * other way, which looked like a bug and was only ever the aim.
             */
            /* +Y is down, so a target below the eye needs a positive pitch. */
            c->cam.pitch = (s32)(p * (double)Q2_ANGLE_360 /
                                 (2.0 * 3.14159265358979323846));
            c->cam.roll  = 0;
        }
    }
}

/* Free-fly camera, kept for inspecting geometry without physics in the way. */
static void client_input(client *c, float dt)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    s32 speed = (s32)(4000.0f * dt);
    s32 turn  = (s32)(1500.0f * dt);
    s32 fwd[3], right[3];
    s32 sy, cy;

    if (!keys)
        return;

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
static u16 client_menu_pad(const client *c)
{
    const bool *k;
    u16 pad = 0;

    /*
     * A scripted run has to be able to answer a page too. Without this the
     * death screen ends the run: the world freezes behind it, the demo's pad
     * goes to the simulation which is no longer ticking, and every later frame
     * is the same picture.
     *
     * CROSS on a slow cycle is enough — it takes the row the page opens on,
     * which for the death page is RESTART LEVEL.
     */
    if (c && c->demo)
        return ((c->frame_index % 30) < 3) ? Q2_PAD_CROSS : 0;

    k = SDL_GetKeyboardState(NULL);
    if (!k)
        return 0;

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

    c->sim[0].gravity = rules.gravity;
    if (rules.tick_rate > 0)
        c->sim[0].dt_per_field = 300 / rules.tick_rate;
    if (c->sim[0].dt_per_field <= 0)
        c->sim[0].dt_per_field = 1;
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
    /*
     * The front end's three leaves. SINGLE PLAYER is what turns the title
     * screen into a game: the level table's own first playable map is loaded
     * and the simulation takes over.
     */
    case Q2_MREQ_NEW_GAME:
        /* The difficulty is the AI's, and it is chosen before the level loads
         * so the creatures spawned by that load already have it. */
        q2_cre_set_skill(c->menu.skill);
        Q2_INFO("front end: new game on skill %d -> %s",
                c->menu.skill, c->first_map);
        c->in_front_end = false;
        client_load_zone(c, c->first_map, 0);
        q2_menu_close(&c->menu);
        break;
    case Q2_MREQ_CREDITS:
    case Q2_MREQ_NOT_BUILT:
        /* A real page of the front end's module that the port has not built.
         * Going back to the title is visible; doing nothing would not be. */
        Q2_INFO("front end: that page is not reconstructed yet");
        q2_menu_open(&c->menu);
        q2_menu_goto(&c->menu, Q2_PAGE_FRONT_TITLE);
        break;
    case Q2_MREQ_MISSION:
        /*
         * The mission screen belongs to the HUD rather than the menu, and it
         * draws into the overlay camera's context — so opening it is closing
         * the menu and raising a flag the frame reads, not entering a page.
         */
        c->mission_open = true;
        q2_menu_close(&c->menu);
        break;
    default:
        break;
    }
}

/*
 * Play one of the menu's five effects.
 *
 * The engine names them by their bank keys — `msc_menu2` on a cursor move,
 * `msc_menu1` on an activation, `msc_menu3` on back, `itm_pkup` on a toggle,
 * `msc_comp_up` while a slider moves (FORMATS.md §10.3) — so playing one is a
 * lookup by name in the map's own bank, a decode, and a push into the same
 * stream the music uses. Mixed in rather than replacing: the console has an SPU
 * with 24 voices and the effect does not stop the track.
 *
 * The SFX slider scales it. STEREO is not consulted because this path is mono
 * and panning a UI sound centre is what stereo would do anyway.
 */
/* The bank's names are ASCII and the engine's keys are lower case; compare
 * without dragging in a locale-aware `stricmp`. */
static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* The same comparison, stopping after `n` characters — `pre` is a prefix of
 * `s`. See client_find_sound for why that is a thing worth having. */
static int name_is_prefix(const char *pre, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int ca = (pre[i] >= 'A' && pre[i] <= 'Z') ? pre[i] + 32 : pre[i];
        int cb = (s[i]   >= 'A' && s[i]   <= 'Z') ? s[i]   + 32 : s[i];
        if (ca != cb)
            return 0;
    }
    return 1;
}

/*
 * Find one effect in the map's bank by the name a table gave.
 *
 * Exact first, and then the truncation rule — because a table's name field is
 * TWELVE BYTES (itemtable.h) and several of the bank's names are longer than
 * that. The disc carries `msc_ar2_pkup22k`; the item table can only hold
 * `msc_ar2_pkup`. So a key that FILLS the field may be a truncation and has to
 * be matched as a prefix, while one that does not fill it was not truncated and
 * must match exactly — otherwise a short key would collide with anything that
 * merely begins with it.
 *
 * That distinction is not a guess. Across all 49 banks on the disc, every one of
 * the eleven item names shorter than twelve characters matches exactly in every
 * bank that carries it, and every one that is exactly twelve — the three health
 * names and both armour names — matches nowhere exactly and everywhere as a
 * prefix of the same name with the sample rate appended. No name is ambiguous
 * under this rule. Five of the eleven are unreachable without it.
 */
static bool client_find_sound(client *c, const char *want, q2_vag *out)
{
    size_t len;
    u32 i, pass;

    if (!c->sfx_ready || !want || !want[0])
        return false;

    len = strlen(want);

    for (pass = 0; pass < 2; pass++) {
        /* The second pass only applies to a key that filled the field. */
        if (pass == 1 && len < Q2_ITEM_MODEL_LEN)
            return false;

        for (i = 0; i < c->sfx.count; i++) {
            if (!q2_sound_bank_get(&c->sfx, i, out))
                continue;
            if (pass == 0 ? name_eq(out->name, want)
                          : name_is_prefix(want, out->name, len))
                return true;
        }
    }

    return false;
}

/*
 * Play one effect out of the map's own bank, by name.
 *
 * The menu names its five and the item table names its eleven, and both are
 * keys into the same per-map bank, so there is one decoder here rather than
 * two. Returns false when the map does not carry the name, which is a thing
 * that happens and is not an error — three maps ship no `frontend.lbm` either.
 */
static bool client_play_sound(client *c, const char *want)
{
    q2_vag vag;

    if (!c->audio || !client_find_sound(c, want, &vag))
        return false;

    {
        s16 pcm[16384];
        u32 n, k;
        s32 vol;

        n = q2_spu_adpcm_decode(vag.body, vag.data_size, pcm,
                                (u32)(sizeof(pcm) / sizeof(pcm[0])));
        if (n == 0)
            return false;

        /* 0..127 from the slider, and the console doubles the music one but
         * not this (0x800205F4 is the music path alone). */
        vol = c->settings.v[Q2_SET_SFX];
        if (vol < 0)   vol = 0;
        if (vol > 127) vol = 127;
        for (k = 0; k < n; k++)
            pcm[k] = (s16)((pcm[k] * vol) / 127);

        SDL_PutAudioStreamData(c->audio, pcm, (int)(n * sizeof(s16)));
        return true;
    }
}

static void client_play_menu_sound(client *c, q2_menu_sound snd)
{
    const char *want = q2_menu_sound_name(snd);

    if (!client_play_sound(c, want) && want && want[0])
        Q2_DEBUG("menu sound '%s' is not in %s's bank", want, c->map);
}

/* ------------------------------------------------------------------------- */
/*
 * What the tick asked to be heard.
 *
 * A think has no audio path of its own — it records what it would have played
 * and the caller drains it (entity.h) — and so does the player's own frame. This
 * is the other end of both: the queue is cleared at the TOP of a tick precisely
 * so the caller can drain it afterwards (sim.c), and the drain cannot miss one,
 * because q2_sim_advance runs the world exactly once per frame with a variable
 * dt rather than sub-stepping (sim.h).
 *
 * The LIGHT and BURST events are not consumed. Both are real and both are
 * dropped rather than faked: a glow light wants a q2_light_world for the entity
 * draw to gather from and the client has none yet, and the pickup burst is a
 * particle effect whose emitter is not reconstructed. An item therefore glows
 * through its own tint (entitydraw.c) and vanishes without sparks, which is less
 * than the console does rather than something the console does not do.
 */
static const char *client_ent_sound_name(const client *c, u32 which)
{
    const q2_item_table *t = c->item_table_ready ? &c->item_table
                                                 : q2_item_table_builtin();

    /*
     * Q2_SND_TELEPORT is the materialise effect and is deliberately NOT in the
     * eleven-name table at 0x800AC240 — the materialise block names it inline
     * (FORMATS.md §"Materialise"), so it is named inline here too.
     */
    if (which == Q2_SND_TELEPORT)
        return "msc_tele1";

    /*
     * The PLAYER's own sounds — footsteps, the landing thump, the four pain
     * grunts — which share this queue because it is the one a headless caller
     * can already drain (entity.h).
     *
     * These are NOT in the eleven-name table either: the executable holds them
     * as resolved sound POINTERS at `0x800B28EC` and the seven beside it. The
     * names are recoverable anyway, because the initialiser that fills those
     * pointers looks each one up by name — `0x8003B900`…`0x8003C590`, a run of
     * `find_sound(name)` / `sw v0, gp+N` pairs against the string pool at
     * `0x800AC458`.
     *
     * The one trap in reading it: the compiler hoists the NEXT name's setup
     * above the current store, so the `addiu t0, "pla_step2"` sitting one
     * instruction before `sw v0, gp+17172` belongs to the following entry and
     * not to that one. Pair them off by one and every sound here is wrong by
     * exactly one slot, which sounds plausible and is not.
     */
    switch (which) {
    case Q2_SND_FOOTSTEP_A:   return "pla_step1";    /* 0x800B2914 */
    case Q2_SND_FOOTSTEP_B:   return "pla_step2";    /* 0x800B2918 */
    case Q2_SND_FOOTSTEP_WET: return "pla_wade3";    /* 0x800B292C */
    case Q2_SND_LAND:         return "pla_fall2";    /* 0x800B28EC */
    case Q2_SND_PAIN_25:      return "mal_pn25_1";   /* 0x800B294C */
    case Q2_SND_PAIN_50:      return "mal_pn50_1";   /* 0x800B2950 */
    case Q2_SND_PAIN_75:      return "mal_pn75_1";   /* 0x800B2954 */
    case Q2_SND_PAIN_100:     return "mal_pn100_1";  /* 0x800B2958 */
    default: break;
    }

    if (which < sizeof(t->sound) / sizeof(t->sound[0]))
        return t->sound[which];

    return NULL;
}

static void client_entity_events(client *c)
{
    const q2_ent_events *ev = q2_sim_entity_events(&c->sim[0]);
    u32 i;

    /*
     * Empty last frame's runtime lights first — 0x80075B94 does this at the top
     * of every frame and nothing in this port was calling it. Without it the
     * sixteen dynamic slots fill on the first frames a projectile flies and stay
     * full forever: a 500-frame BASE3 capture added 16 lights and dropped 481.
     */
    if (c->lights_ready)
        q2_light_world_begin_frame(&c->light_world);

    /* Blink the script flickers, and raise the ones that are lit this frame. */
    {
        u32 f;
        q2_flklights_tick(&c->flklights, &c->sim[0].combat.rng,
                          c->sim[0].level_time);
        for (f = 0; f < c->flklights.count; f++) {
            const q2_flklight *fl = &c->flklights.f[f];
            if (fl->in_use && fl->lit && c->lights_ready &&
                q2_light_add_dynamic(&c->light_world, fl->pos, fl->rgb,
                                     Q2_PROJ_LIGHT_INNER, Q2_PROJ_LIGHT_OUTER,
                                     0, 0))
                c->ent_light_added++;
        }
    }

    if (!ev)
        return;

    for (i = 0; i < ev->count; i++) {
        const char *name;

        /*
         * Only SOUND is acted on. The entity world also raises _LIGHT (a
         * dynamic light of `glow` and `radius`) and _BURST (the pickup particle
         * burst at 0x8005B6C0), and both are dropped here — counted rather than
         * silently ignored, because "the client handles entity events" was true
         * of one kind in three and nothing said so.
         *
         * Neither is guessed at: the port has no preset for a pickup burst —
         * its seven are explosion, blood, BFG, gib, scripted, spark and laser
         * end — and choosing one of those would invent an effect rather than
         * reconstruct it. See openquestions #60.
         */
        if (ev->e[i].kind == Q2_ENT_EVENT_LIGHT) {
            /*
             * Fed to the light world rather than dropped. The event carries the
             * colour and the outer radius the projectile sweep reads out of
             * 0x800AE954; the inner comes from the same preset. Sixteen dynamic
             * lights is the engine's own ceiling (lighting.h) and the
             * seventeenth is dropped there, so a busy frame still counts what it
             * could not take.
             */
            /*
             * The inner radius is chosen from the outer the event carries,
             * because the event has no room for both: the BFG's 1400 pairs with
             * 1000 and every other bolt's 800 pairs with 300. Both pairs are
             * read from 0x800AE9C0 and 0x800AE958 -- see projectile.h.
             */
            /* The event carries both radii now; 0 means the raiser had no
             * inner to give, and the projectile's is the sane default. */
            s32 inner = ev->e[i].inner_radius ? ev->e[i].inner_radius
                                              : Q2_PROJ_LIGHT_INNER;

            if (!c->lights_ready ||
                !q2_light_add_dynamic(&c->light_world, ev->e[i].pos,
                                      ev->e[i].glow, inner,
                                      ev->e[i].radius, 0, 0))
                c->ent_light_dropped++;
            else
                c->ent_light_added++;
            continue;
        }
        if (ev->e[i].kind == Q2_ENT_EVENT_BURST) {
            client_pickup_burst(c, ev->e[i].pos, ev->e[i].model_index);
            continue;
        }
        if (ev->e[i].kind != Q2_ENT_EVENT_SOUND)
            continue;

        name = client_ent_sound_name(c, ev->e[i].sound);
        if (name && !client_play_sound(c, name))
            Q2_DEBUG("sound '%s' is not in %s's bank", name, c->map);
    }
}

/* ------------------------------------------------------------------------- */
/* Saving and loading                                                         */
/*                                                                            */
/* The three function pointers the front end calls (`0x800B3234` poll,        */
/* `0x800B3238` request, `0x800B324C` act on a row) are filled straight from   */
/* saveui.h, which has their exact signatures — so the reconstruction drives   */
/* the port's save system without either side knowing about the other.        */
/* ------------------------------------------------------------------------- */

/*
 * Everything a save has to contain that does not live in the sim: the mission
 * tallies, which the HUD owns, and the menu settings, which the pause menu
 * edits and which state 14 "applies" on the console (memcard.h).
 */
static bool client_capture(client *c)
{
    q2_result rc;

    q2_save_free(&c->snapshot);

    rc = q2_save_capture(&c->snapshot, &c->sim[0], NULL, c->build.serial,
                         c->map, c->zone_index);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot capture a save: %s", q2_result_str(rc));
        return false;
    }

    q2_save_capture_mission(&c->snapshot, &c->mission);
    q2_save_set_settings(&c->snapshot, c->settings.v, Q2_SET_COUNT);
    return true;
}

/*
 * Put a loaded save back into the running game.
 *
 * The zone is reloaded first and unconditionally, even when the map and zone
 * already match. Applying onto whatever the player happened to be standing in
 * would leave anything the save does not cover — the models bound to this map,
 * the effect generator's attachment, the spawn the mover cached — carrying over
 * from a session that is being discarded. A load IS a level load; treating it
 * as one is both simpler and correct.
 */
static bool client_apply_save(client *c, const q2_save *s)
{
    q2_result rc;
    s32 eye[3];

    if (!client_load_zone(c, s->map, s->zone)) {
        Q2_ERROR("the save names %s zone %d, which will not load",
                 s->map, (int)s->zone);
        return false;
    }

    rc = q2_save_apply(s, &c->sim[0], NULL, c->build.serial, c->map);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot apply the save: %s", q2_result_str(rc));
        return false;
    }

    /* The settings travel with the save, and applying them is what states 14
     * and 16 do on the console (0x8001C698, the GAME VARIABLES application). */
    {
        s16 v[Q2_SET_COUNT];
        u32 n = q2_save_get_settings(s, v, (u32)Q2_SET_COUNT);
        u32 k;
        for (k = 0; k < n; k++)
            c->settings.v[k] = v[k];
    }
    client_apply_settings(c);

    q2_save_apply_mission(s, &c->mission);

    /* The weapon in the hands follows the restored selection. Without this the
     * player holds whatever the fresh spawn gave them while the simulation
     * thinks they are holding the railgun. */
    if (c->vm_ready) {
        q2_vw_init(&c->vw, &c->vm_tables, c->sim[0].combat.weapon_id);
        c->vw_last_weapon = c->sim[0].combat.weapon_id;
        client_bind_view_model(c);
    }

    /* A restored game is a played game, so it resumes under the simulation
     * rather than in the free-fly camera. */
    c->sim_enabled = true;

    q2_sim_eye(&c->sim[0], eye);
    c->cam.pos[0] = eye[0];
    c->cam.pos[1] = eye[1];
    c->cam.pos[2] = eye[2];
    c->cam.yaw    = c->sim[0].player[0].yaw;
    c->cam.pitch  = c->sim[0].player[0].pitch;

    Q2_INFO("loaded %s zone %d at %d:%02d",
            s->map, (int)s->zone,
            (int)(s->level_time / 300 / 60), (int)(s->level_time / 300 % 60));
    return true;
}

static void client_notify(client *c, const char *text)
{
    Q2_INFO("%s", text);
    if (c->hud_ready)
        q2_hud_message(&c->hud, text);
}

/* ------------------------------------------------------------------------- */
/* The front end                                                              */
/* ------------------------------------------------------------------------- */
static void client_card_open(client *c, q2_save_ui_mode mode)
{
    if (mode == Q2_SAVE_UI_SAVE && !client_capture(c)) {
        client_notify(c, "CANNOT SAVE");
        return;
    }

    c->card_mode = mode;
    if (mode == Q2_SAVE_UI_SAVE)
        q2_save_ui_open_save(&c->save_ui, &c->snapshot);
    else
        q2_save_ui_open_load(&c->save_ui);

    /* A fresh session: the cursor, the pad edge and the screen all start
     * clean, so the button that opened the front end cannot also pick a row. */
    q2_mcard_init(&c->mcard, &c->mcard_host);
    c->card_screen  = Q2_MCARD_NONE;
    c->card_menu.page = NULL;
    c->mcard_open   = true;

    q2_menu_close(&c->menu);
    c->mission_open = false;
}

static void client_card_close(client *c)
{
    q2_save_ui_close(&c->save_ui);
    c->mcard_open = false;
}

/*
 * The row text, and the one place the port has to put something on screen that
 * the console's own screen gets from the card's directory.
 *
 * An empty slot is the EMPTY STRING when loading, which is exactly right: the
 * selection bar tests the label against the empty string, so the row draws
 * nothing and cannot be aimed at (memcard.h). When SAVING it cannot be empty,
 * because a player has to be able to pick a free slot to write into — so it
 * carries the slot number and nothing else, which is the least this port can
 * invent and still work.
 */
static void client_card_rows(client *c)
{
    int i;

    for (i = 0; i < Q2_SAVE_SLOTS && i < Q2_MENU_MAX_ITEMS - 1; i++) {
        const q2_save_info *info = &c->save_ui.info[i];
        char *dst = c->card_menu.text[i + 1];

        if (info->used) {
            snprintf(dst, Q2_MENU_TEXT_MAX, "%s", q2_save_ui_row(&c->save_ui, i));
            c->card_menu.disabled[i + 1] = 0;
        } else if (c->card_mode == Q2_SAVE_UI_SAVE) {
            snprintf(dst, Q2_MENU_TEXT_MAX, "%d", i + 1);
            c->card_menu.disabled[i + 1] = 0;
        } else {
            dst[0] = '\0';
            c->card_menu.disabled[i + 1] = 1;
        }
    }
}

/* Point the shadow menu at the screen the current state shows, and fill in
 * whatever that screen composes at run time. */
static void client_card_sync(client *c)
{
    q2_mcard_screen want = q2_mcard_screen_for_state_port(c->save_ui.state);
    const q2_menu_page *page;

    if (want != c->card_screen) {
        c->card_screen = want;
        page = q2_mcard_page(want);

        memset(c->card_menu.text, 0, sizeof(c->card_menu.text));
        memset(c->card_menu.disabled, 0, sizeof(c->card_menu.disabled));
        c->card_menu.page   = page;
        c->card_menu.cursor = page ? (int)page->first : 0;
        c->card_menu.open   = (page != NULL);
        c->mcard.cursor     = 0;
    }

    page = c->card_menu.page;
    if (!page)
        return;

    if (c->card_screen == Q2_MCARD_SAVE_FILE) {
        client_card_rows(c);
    } else if (c->card_screen == Q2_MCARD_LOAD_MESSAGE) {
        /*
         * The screen whose text the runtime composes — which is why state 13
         * maps to it (memcard.h). BOTH of its rows are placeholders, and both
         * have to be written: an empty override falls back to the table's own
         * label, so leaving the second alone leaves the word HERE on screen.
         * A single space is what blanks a line the report does not need.
         */
        snprintf(c->card_menu.text[0], Q2_MENU_TEXT_MAX, "%s",
                 c->save_ui.message);
        snprintf(c->card_menu.text[1], Q2_MENU_TEXT_MAX, "%s",
                 c->save_ui.detail[0] ? c->save_ui.detail : " ");
    }
}

/* What the front end left behind when it closed. */
static void client_card_finish(client *c)
{
    q2_save loaded;

    switch (c->save_ui.status) {
    case Q2_SAVE_UI_SAVED:
        client_notify(c, "GAME SAVED");
        break;

    case Q2_SAVE_UI_LOADED:
        if (q2_save_ui_take_loaded(&c->save_ui, &loaded)) {
            bool ok = client_apply_save(c, &loaded);
            q2_save_free(&loaded);
            client_notify(c, ok ? "GAME LOADED" : "LOAD FAILED");
        }
        break;

    case Q2_SAVE_UI_FAILED:
        client_notify(c, c->save_ui.message[0] ? c->save_ui.message
                                               : "SAVE FAILED");
        break;

    default:
        break;
    }

    c->mcard_open = false;
}

static void client_card_frame(client *c)
{
    u16 pad = client_menu_pad(c);
    const q2_menu_page *page;
    q2_menu_sound snd;

    /*
     * Last frame's work first. The read or write is deferred by exactly one
     * frame so the busy screen is actually drawn — which is what the console
     * has a DO NOT POWER-OFF screen for, and what a save that completes inside
     * the same frame never shows.
     */
    q2_save_ui_update(&c->save_ui);

    client_card_sync(c);
    page = c->card_menu.page;

    /* TRIANGLE backs out. The console's own arms do not handle it — they are
     * four instructions long and test CROSS only — so this is the port's, and
     * without it a front end with no live arm would be a trap. */
    if ((pad & Q2_PAD_TRIANGLE) && !(c->card_menu.pad_prev & Q2_PAD_TRIANGLE)) {
        c->card_menu.pad_prev = pad;
        client_card_close(c);
        return;
    }

    /* Navigation, through the real menu engine so the wrap and skip rules are
     * the ones read out of the executable. */
    q2_menu_advance(&c->card_menu, pad);

    snd = q2_menu_take_sound(&c->card_menu);
    if (snd != Q2_MSND_NONE)
        client_play_menu_sound(c, snd);

    /* The front end reads the cursor POSITIONALLY, as `cursor - first`
     * (0x800B32AC minus 0x800B32AE). */
    if (page) {
        int rel = c->card_menu.cursor - (int)page->first;
        c->mcard.cursor = rel < 0 ? 0 : rel;
    }

    if (q2_mcard_advance(&c->mcard, pad)) {
        /* The accept arm applies the game variables and leaves (0x8001F0A4). */
        client_apply_settings(c);
    }

    /*
     * State 13 is live and has no arm of its own, so the press that dismisses
     * the report is the port's — see q2_save_ui_acknowledge.
     */
    if (c->mcard.fired && c->save_ui.state == Q2_SAVEUI_STATE_REPORT)
        q2_save_ui_acknowledge(&c->save_ui);

    if (!c->save_ui.open) {
        client_card_finish(c);
        return;
    }

    /*
     * Again, because the arms above may have changed the state and this frame
     * still has to be DRAWN. Without it the busy screen would be skipped
     * entirely: the work happens at the top of the next frame, so the frame in
     * between is the only one that can show it.
     */
    client_card_sync(c);
}

/* ------------------------------------------------------------------------- */
/* Quick save and quick load — slot 1, no screens.                            */
/*                                                                            */
/* Entirely the port's: the console has no such thing, and it is here because  */
/* a four-screen front end is the wrong tool for "try that jump again". It     */
/* goes through exactly the same capture, file and apply paths, so it cannot   */
/* drift from what the front end writes.                                      */
/* ------------------------------------------------------------------------- */
static void client_quick_save(client *c)
{
    q2_result rc;

    if (!client_capture(c)) {
        client_notify(c, "CANNOT SAVE");
        return;
    }

    rc = q2_save_slot_write(&c->snapshot, 0);
    if (rc != Q2_OK) {
        Q2_ERROR("quick save failed: %s", q2_result_str(rc));
        client_notify(c, "SAVE FAILED");
        return;
    }

    client_notify(c, "QUICK SAVED");
}

static void client_quick_load(client *c)
{
    q2_save s;
    q2_result rc = q2_save_slot_read(&s, 0);

    if (rc != Q2_OK) {
        Q2_ERROR("quick load failed: %s", q2_result_str(rc));
        client_notify(c, rc == Q2_ERR_NOT_FOUND ? "NO QUICK SAVE"
                                                : "LOAD FAILED");
        return;
    }

    client_notify(c, client_apply_save(c, &s) ? "QUICK LOADED" : "LOAD FAILED");
    q2_save_free(&s);
}

/* ------------------------------------------------------------------------- */
/*
 * A screenshot, of the console's framebuffer rather than of the window.
 *
 * Entirely the port's, and the distinction is the point: the window is an
 * upscale of a 512x248 buffer, so grabbing it back off the desktop resamples
 * the very pixels — the dither pattern, the vertex snapping — that the whole
 * renderer exists to get right. This writes the buffer the frame was composed
 * into, at its own size, through the same P6 writer the offline tools use.
 */
static void client_screenshot(client *c)
{
    static int n = 0;
    char path[64];
    q2_result rc;

    snprintf(path, sizeof(path), "q2psx-%03d.ppm", n);

    rc = psx_fb_write_ppm(q2_screen_front(&c->screen), path);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot write %s: %s", path, q2_result_str(rc));
        return;
    }

    n++;
    Q2_INFO("screenshot: %s", path);
}

/*
 * The capture a scripted run writes.
 *
 * `--shot out.ppm` alone writes the last frame to that name; with `--shot-every`
 * it becomes a stem — `out.ppm` -> `out_0000.ppm`, `out_0030.ppm` — so a run
 * produces a strip that can be flipped through. The framebuffer is written, not
 * the window: these are the console's own 512x248 pixels, which is what every
 * comparison in this project is made in.
 */
static void client_write_shot(client *c, bool numbered)
{
    char path[512];
    q2_result rc;

    if (!c->shot_path)
        return;

    if (numbered) {
        const char *dot = strrchr(c->shot_path, '.');
        size_t stem = dot ? (size_t)(dot - c->shot_path) : strlen(c->shot_path);

        if (stem > sizeof(path) - 32)
            stem = sizeof(path) - 32;
        memcpy(path, c->shot_path, stem);
        snprintf(path + stem, sizeof(path) - stem, "_%04ld%s",
                 c->frame_index, dot ? dot : ".ppm");
    } else {
        snprintf(path, sizeof(path), "%s", c->shot_path);
    }

    rc = psx_fb_write_ppm(q2_screen_front(&c->screen), path);
    if (rc != Q2_OK) {
        Q2_ERROR("cannot write %s: %s", path, q2_result_str(rc));
        return;
    }

    c->shots_written++;
    Q2_INFO("frame %ld -> %s", c->frame_index, path);
    Q2_INFO("  eye %d %d %d  yaw %d pitch %d  cell %d  "
            "%u/%u quads, %u nodes, near %u back %u ot %u",
            c->cam.pos[0], c->cam.pos[1], c->cam.pos[2],
            c->cam.yaw, c->cam.pitch, c->sim[0].current_node,
            c->shot_stats.quads_emitted, c->shot_stats.quads_total,
            c->shot_stats.nodes_visited,
            c->shot_stats.quads_rejected_near,
            c->shot_stats.quads_rejected_back,
            c->shot_stats.ot_overflow);

    if (c->mp_enabled) {
            int pi;

            for (pi = 0; pi < Q2_MP_MAX_PLAYERS; pi++) {
                const q2_combat_scan_stats *sc0 = &q2_combat_scan_by[pi];

                if (pi > 0 && !c->sim_ready[pi])
                    continue;
                if (pi == 0)
                    Q2_INFO("  scan[0]: %u tested, %u behind, %u beyond world,"
                            " %u off axis, %u hit",
                            sc0->tested, sc0->behind, sc0->beyond_world,
                            sc0->off_axis, sc0->hit);
                Q2_INFO("  player %d shots %u, dry %u", pi,
                        c->mp_shots[pi], c->mp_dry[pi]);
                if (pi == 1)
                    Q2_INFO("  proj: %u launched, %u stepped, %u expired, "
                            "%u hit; near %u (past end %u), closest^2 %lld, "
                            "seg^2 %d", q2_sim_proj_scan.launched,
                            q2_sim_proj_scan.stepped, q2_sim_proj_scan.expired,
                            q2_sim_proj_scan.hit, q2_sim_proj_scan.near_miss,
                            q2_sim_proj_scan.past_end,
                            (long long)q2_sim_proj_scan.closest_sq,
                            q2_sim_proj_scan.seg_len);
                if (pi == 1)
                    Q2_INFO("  closest: owner %d, bolt at [%d %d %d], "
                            "target origin [%d %d %d]",
                            q2_sim_proj_scan.closest_owner,
                            q2_sim_proj_scan.closest_from[0],
                            q2_sim_proj_scan.closest_from[1],
                            q2_sim_proj_scan.closest_from[2],
                            q2_sim_proj_scan.closest_origin[0],
                            q2_sim_proj_scan.closest_origin[1],
                            q2_sim_proj_scan.closest_origin[2]);
                {
                    const q2_combat_scan_stats *sc = &q2_combat_scan_by[pi];

                    Q2_INFO("  scan[%d]: %u tested, %u behind, %u beyond world,"
                            " %u off axis, %u hit",
                            pi, sc->tested, sc->behind, sc->beyond_world,
                            sc->off_axis, sc->hit);
                }
                Q2_INFO("  player %d at [%d %d %d] yaw %d, %d hp, moved %ld",
                        pi, c->sim[0].player[pi].pos[0],
                        c->sim[0].player[pi].pos[1],
                        c->sim[0].player[pi].pos[2],
                        c->sim[0].player[pi].yaw,
                        pi == c->sim[0].cur_player
                            ? c->sim[0].combat.inv.health
                            : c->sim[0].pcombat[pi].inv.health,
                        labs(c->sim[0].player[pi].pos[0] - c->mp_view_pos[pi][0]) +
                        labs(c->sim[0].player[pi].pos[2] - c->mp_view_pos[pi][2]));
            }
        }

    if (c->creatures_ready && c->creatures.set.count) {
        u32 i, live = 0, hunting = 0, dead = 0;
        long hp = 0;
        s32 near_d = -1;
        long moved = 0;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            s32 dx, dz, d;

            if (!m->in_use || m->dead) { if (m->dead) dead++; continue; }
            live++;
            hp += m->health;
            if (m->enemy)
                hunting++;

            moved += c->cre_home ? (labs(m->pos[0] - c->cre_home[i*3+0]) +
                                    labs(m->pos[2] - c->cre_home[i*3+2])) : 0;

            dx = m->pos[0] - c->cam.pos[0];
            dz = m->pos[2] - c->cam.pos[2];
            d  = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (near_d < 0 || d < near_d)
                near_d = d;
        }
        Q2_INFO("  creatures %u live, %u hunting, %u drawn (%u faces), "
                "nearest %d units, moved %ld, player %d hp, "
                "%u swings %u shots, %u sounds (%u not in bank), %u dead, "
                "%ld hp total, "
                "player attacked %u, targets %u, bolts %u, %u bodies, "
                "rot %u steps %u moved %u turned, %u calls",
                live, hunting, c->cre_drawn, c->cre_faces, near_d, moved,
                c->sim[0].combat.inv.health, c->cre_swings, c->cre_shots,
                c->cre_sounds, c->cre_sound_missing, dead, hp,
                c->player_attacks,
                c->sim[0].combat.target_count,
                c->sim[0].combat.projectiles.live, c->cre_bodies, c->rot_steps,
                c->rot_moved, client_rot_turned(c),
                c->sim[0].event_rt.call_count);
        Q2_INFO("  entity ev %u lights added, %u dropped, %u bursts drawn, "
                "%u script lights",
                c->ent_light_added, c->ent_light_dropped, c->ent_bursts,
                c->script_lights);
        Q2_INFO("  burst why %u no fx, %u no table, %u no model, %u no bank, "
                "%u bad model, %u no verts",
                c->burst_no_fx, c->burst_no_table, c->burst_no_model,
                c->burst_no_bank, c->burst_bad_model, c->burst_no_verts);

        Q2_INFO("  attacks   %u checkattack (%u blind, %u decided, %u yes), "
                "%u attack calls, %u missing",
                q2_ai_stats.checkattack_calls, q2_ai_stats.checkattack_blind,
                q2_ai_stats.checkattack_decided, q2_ai_stats.checkattack_yes,
                q2_ai_stats.attack_called, q2_ai_stats.attack_missing);

        Q2_INFO("  moves     attack set %u / missing %u, melee %u / %u, "
                "run %u / %u, pain %u, die %u, stand %u",
                q2_cre_actions.move_via_set[6],
                q2_cre_actions.move_via_missing[6],
                q2_cre_actions.move_via_set[7],
                q2_cre_actions.move_via_missing[7],
                q2_cre_actions.move_via_set[4],
                q2_cre_actions.move_via_missing[4],
                q2_cre_actions.move_via_set[11],
                q2_cre_actions.move_via_set[12],
                q2_cre_actions.move_via_set[0]);

        {
            char buf[160];
            int  used = 0, ti;

            buf[0] = ' ';
            for (ti = 0; ti < 32; ti++)
                if (q2_cre_actions.think_hits[ti] && used < 140)
                    used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                                     " %d:%u", ti,
                                     q2_cre_actions.think_hits[ti]);
            Q2_INFO("  think hit%s", buf[0] ? buf : " (none)");
        }

        Q2_INFO("  decoded   %u thinks (%u unbound), %u calls (%u unclassified), "
                "%u fire calls: %u sent, %u no enemy, %u dead enemy",
                q2_cre_actions.thinks_run, q2_cre_actions.thinks_unbound,
                q2_cre_actions.calls_seen, q2_cre_actions.calls_unclassified,
                q2_cre_actions.fire_calls, q2_cre_actions.fire_sent,
                q2_cre_actions.fire_no_enemy, q2_cre_actions.fire_dead_enemy);

        Q2_INFO("  ai world  %u traces (%u unplaced, %u clear), "
                "%u bottom (%u fail), %u los (%u blocked)",
                c->ai_world.stats.traces, c->ai_world.stats.trace_unplaced,
                c->ai_world.stats.trace_clear, c->ai_world.stats.bottom_calls,
                c->ai_world.stats.bottom_fail, c->ai_world.stats.los_calls,
                c->ai_world.stats.los_blocked);
    }
}

/* ------------------------------------------------------------------------- */
/*
 * Bind the model the view weapon wants.
 *
 * The clip bank names it — "Blaster G", "Supershot G" — and the map's own
 * CastList is where the geometry lives, so this runs both when a zone loads and
 * whenever the state machine finishes a swap. A weapon whose model this map
 * does not ship simply draws nothing rather than drawing the wrong thing.
 */
static void client_bind_view_model(client *c)
{
    const char *name;
    s32 index;

    c->vw_model_ready = false;
    q2_vw_set_model(&c->vw, NULL);

    if (!c->vm_ready || !c->model_bank_ready)
        return;

    name = q2_vw_model_name(&c->vw);
    if (!name || !name[0])
        return;

    index = q2_model_bank_find(&c->model_bank, name);
    if (index < 0) {
        Q2_DEBUG("no view model '%s' in %s", name, c->map);
        return;
    }

    if (q2_model_get(&c->model_bank, (u32)index, &c->vw_model) != Q2_OK)
        return;

    c->vw_model_ready = true;
    q2_vw_set_model(&c->vw, &c->vw_model);
    Q2_INFO("view weapon: %s", name);
}

static void client_menu_frame(client *c)
{
    q2_menu_sound snd;

    q2_menu_advance(&c->menu, client_menu_pad(c));

    snd = q2_menu_take_sound(&c->menu);
    if (snd != Q2_MSND_NONE)
        client_play_menu_sound(c, snd);

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
/*
 * The world draw, in the place 0x80066858 occupies: called from inside the
 * viewport's own draw, after its state is published and under its own gate.
 */
static void client_draw_view(void *user, q2_screen *s, int p,
                             psx_ot *ot, gte_state *gte)
{
    client *c = (client *)user;
    q2_world_stats stats;

    /*
     * The viewport owns the field of view: SetGeomScreen(view+262) at
     * 0x80076B90, and a geometry offset at the viewport's own centre
     * (view+266/+268, SetGeomOffset at 0x80076B78).
     *
     * Both are taken from the view record rather than from the framebuffer,
     * because in a split they are not the same thing — the quad layout puts four
     * centres in one frame — and because this is the state q2_screen_view_begin
     * has already installed in the GTE for this viewport. Handing it back keeps
     * the world's own reload from quietly disagreeing with the screen's.
     */
    c->cam.projection = (u16)s->view[p].proj;
    c->cam.ofs_x      = s->view[p].ofs_x;
    c->cam.ofs_y      = s->view[p].ofs_y;
    c->cam.far_z      = s->view[p].far_z;

    /*
     * In a split, each viewport is a different PLAYER, and until now every one
     * of them showed the same camera — the screen work was right and there was
     * only ever one thing to look at.
     *
     * Viewport 0 is player 0 and keeps the camera the frame built; the others
     * follow their OWN sim's eye and view angles. Each of players 1..3 has a
     * q2_sim of its own, spawned at its own MultiSpawn and advanced on its own
     * pad every frame, so a split shows four people walking about rather than
     * one camera reflected.
     *
     * What is still shared and should not be is the WORLD: each instance owns a
     * copy of the map's items and its own script runtime, and only player 0's
     * is read or drawn. See openquestions #53 — the fix is pulling the player
     * out of q2_sim, which is a change to sim.c rather than to this caller.
     */
    if (c->mp_enabled && p > 0 && p < Q2_MP_MAX_PLAYERS && c->sim_ready[p]) {
        const q2_player *pl = &c->sim[0].player[p];

        c->cam.pos[0] = pl->pos[0];
        c->cam.pos[1] = pl->pos[1] - Q2_EYE_BASE;
        c->cam.pos[2] = pl->pos[2];
        c->cam.yaw    = pl->yaw;
        c->cam.pitch  = pl->pitch;
    }

    /* The viewport's far distance is also the subdivision threshold: the same
     * view+264 the original parks at 0x800B2CCC serves both. */
    c->render.subdiv_threshold = s->view[p].far_z;

    q2_world_build_ot(&c->zone, &c->cam, s->view[p].w, s->view[p].h,
                      ot, gte, &c->render, &stats);
    c->shot_stats = stats;
    c->cre_drawn  = 0;
    c->cre_faces  = 0;

    /*
     * The map's items, into the table the world has just been built into — the
     * same reason the weapon and the effects go there, and the reason
     * entitydraw is a module rather than something done inline: an item sorts
     * against the crate it stands behind because both are in one list.
     *
     * It comes straight after the world and before everything else because that
     * is what it is: level content, not presentation.
     *
     * `player` is 0 because there is one, and it is what makes an item this
     * player has already collected invisible to this view — the per-player
     * block's whole purpose. The texture-page table is the world's, so an item
     * on a page the world has already promoted blends at the promoted mode.
     * `lights` is NULL: the client has no q2_light_world, so an item is drawn
     * at its own glow tint exactly as this module did before lighting existed.
     */
    if (c->sim[0].entities_ready) {
        q2_entity_draw_ctx ectx;
        q2_entity_draw_stats estats;

        memset(&ectx, 0, sizeof(ectx));
        ectx.bank          = c->model_bank_ready ? &c->model_bank : NULL;
        ectx.clut4_count_a = c->clut4_count_a;
        ectx.player        = 0;
        ectx.tpage         = &c->render.tpage;

        /*
         * The lights, and the cell to gather them from. `coll_node` was -1,
         * which is "no node" — so even had a light world been passed, every
         * entity would have taken the fallback. The sim tracks the player's
         * own cell every tick and that is the one the engine uses.
         */
        ectx.lights        = c->lights_ready ? &c->light_world : NULL;
        ectx.coll_node     = c->sim[0].current_node;

        q2_entity_build_ot(&c->sim[0].entities, &ectx, &c->cam, ot, gte, &estats);
    }

    /*
     * The creatures, into the same table for the same reason: a Soldier behind
     * a crate sorts behind it because both are in one list.
     *
     * WHICH ANIMATION A CREATURE IS PLAYING, and it is not chosen by index.
     *
     * `0x8006B924` keeps the animation position in a halfword at `entity+0x100`
     * and the current clip at `model+0x34`, and while the position is past the
     * clip's length it advances the pointer by that clip's own `next` delta and
     * subtracts its `frames`. So a model's clips are ONE CONTINUOUS TIMELINE
     * and the position is an offset into it — there is no clip index to find,
     * which is what the port was previously trying to reconstruct by matching a
     * move's length against a clip's.
     *
     * A creature's AI frame is a position on that same timeline (its module's
     * moves are numbered 0..474 for the Soldier), at three ticks per frame, so
     * the walk lands in the right clip on its own.
     */
    if (c->creatures_ready && c->cre_model) {
        u32 i;

        for (i = 0; i < c->creatures.set.count; i++) {
            const q2_monster *m = &c->creatures.set.monsters[i];
            q2_model_instance inst;
            q2_model_draw_stats st;
            q2_model_pose pose[64];
            q2_model_anim clip;
            q2_light_env  cre_env;
            bool posed = false;

            if (!m->in_use || !c->cre_model_ok[i])
                continue;

            {
                const q2_model *mdl = &c->cre_model[i];
                s32 frame = m->frame;
                u32 within = 0;
                bool have_clip = false;

                if (frame < 0)
                    frame = 0;

                if (mdl->hdr.num_parts > Q2PSX_ARRAY_COUNT(pose)) {
                    have_clip = false;
                } else if (m->currentmove) {
                    /*
                     * The clip its CURRENT MOVE plays, and the position within
                     * that clip — not a position on one continuous timeline.
                     * The engine keeps a current clip at model+0x34 and only
                     * walks the chain when the position overruns it, so a move
                     * selects a clip and the frame indexes into it. Walking the
                     * whole chain instead drifts: the Soldier's death move at
                     * AI frame 308 lands in the wrong clip and the body stands
                     * up halfway through falling over.
                     */
                    const q2_mmove *mv = m->currentmove;
                    s32 len = mv->last_frame - mv->first_frame + 1;

                    if (len > 0) {
                        s32 into = frame - mv->first_frame;

                        if (into < 0)
                            into = 0;
                        if (into >= len)
                            into = len - 1;

                        have_clip = q2_model_anim_by_length(
                            mdl, (u32)len * Q2_CRE_TICKS_PER_FRAME,
                            client_move_ordinal(m, mv), &clip);
                        within = (u32)into * Q2_CRE_TICKS_PER_FRAME;
                        if (have_clip && within >= clip.frames)
                            within = clip.frames ? clip.frames - 1 : 0;
                    }
                }

                /* No move installed, or no clip of that length: the timeline
                 * walk, which is what every creature used before this. */
                if (!have_clip && mdl->hdr.num_parts <= Q2PSX_ARRAY_COUNT(pose))
                    have_clip = q2_model_anim_at(
                        mdl, (u32)frame * Q2_CRE_TICKS_PER_FRAME,
                        &clip, &within);

                if (have_clip)
                    posed = (q2_model_pose_at(mdl, &clip, within,
                                              pose) == Q2_OK);
            }

            q2_model_instance_init(&inst);
            inst.model         = &c->cre_model[i];
            inst.pose          = posed ? pose : NULL;

            /*
             * The lights reaching this creature. The item draw gets these
             * through the entity context; this loop calls q2_model_build_ot
             * directly, so it has to gather its own — three lights per entity,
             * which is all the GTE's light matrix has rows for (FORMATS §17).
             */
            if (c->lights_ready) {
                q2_light_set  set;
                s32 cell = q2_coll_find_node(&c->sim[0].coll, m->pos, -1, true);

                q2_light_gather(&set, &c->light_world, m->pos, cell, false);
                q2_light_env_build(&cre_env, &set, Q2_LIGHT_ONE,
                                   Q2_LIGHT_ONE, NULL);
                inst.light = &cre_env;
            }
            inst.origin[0]     = m->pos[0];
            inst.origin[1]     = m->pos[1];
            inst.origin[2]     = m->pos[2];
            inst.yaw           = m->angles[2];
            inst.clut4_count_a = c->clut4_count_a;

            q2_model_build_ot(&inst, &c->cam, ot, gte, &st);
            if (st.faces_emitted) {
                c->cre_drawn++;
                c->cre_faces += st.faces_emitted;
            }
        }
    }

    /*
     * Effects go into the SAME table as the world, which is the whole point of
     * an ordering table: a spark behind a crate sorts behind it because both
     * are in one list, not because anything tested them against each other.
     *
     * The beam pool is emptied after the last viewport rather than here, since
     * one queue feeds every view — 0x80064F10 draws and then resets, and doing
     * the reset per view would make split screen lose the beams in every
     * viewport but the first.
     */
    q2_fx_build_ot(&c->sim[0].fx, &c->cam, (u32)p, ot, gte);

    /*
     * The status bar, into this viewport's own slice — because the console
     * draws it from this very hook (`0x800337D0`), not from an overlay pass.
     * Its anchor is the viewport's `sbar_x`/`sbar_y`, which is what those two
     * halfwords turn out to be (statusbar.h).
     *
     * The data is the sim's, read here rather than pushed: health and armour
     * come straight off the inventory, and the ammo shown is the ammo the
     * CURRENT weapon uses, which is the same indirection the console makes
     * through its weapon-to-ammo map.
     */
    /* `icons_resident`, not `menu_font_ready`: the upload succeeds when any of
     * the three images lands, and the status bar needs THIS one. */
    if (c->icons_ready && c->menu_font_ready && c->menu_font.icons_resident) {
        const q2_inventory *inv = &c->sim[0].combat.inv;
        /* The LIVE weapon, which combat owns — 1-based, 0 for none. */
        int weapon = c->sim[0].combat.weapon_id;
        int ammo = 0;

        if (weapon > 0 && weapon < Q2_WEAPON_COUNT) {
            s8 kind = q2_weapon_ammo[weapon];
            if (kind >= 0 && kind < Q2_AMMO_COUNT)
                ammo = inv->ammo[kind];
        }

        q2_statusbar_anchor(&c->sbar, s->view[p].sbar_x, s->view[p].sbar_y);
        c->sbar.players = s->view_count > 0 ? s->view_count : 1;
        c->sbar.health  = inv->health;
        c->sbar.armour  = inv->armour;
        c->sbar.ammo    = (s16)ammo;
        c->sbar.weapon  = weapon;

        /*
         * The icons, named now that the vocabulary is decoded (icontable.h):
         * the fifth byte of a rect record is the item's effect id, so an icon
         * is asked for by the item it belongs to rather than by position.
         * Health shows the medikit's cross; armour shows whichever armour is
         * worn, and nothing at all when none is.
         */
        c->sbar.health_icon = Q2_SBAR_ICON_MEDIKIT;
        c->sbar.armour_icon = inv->armour > 0 ? Q2_SBAR_ICON_ARMOUR_JACKET : 0;

        q2_statusbar_build_ot(&c->sbar, c->menu_font.tpage_icons,
                              c->menu_font.clut_text, ot,
                              q2_screen_view_otz(s, p, 0), 0, 0);
    }

    /*
     * The weapon in the hands, into the SAME table as the world it stands in.
     * That is the whole reason it is a model and not an overlay: it sorts
     * against the wall the player has walked into rather than always winning,
     * which is exactly what the console does and exactly what a blit cannot.
     *
     * It is placed on the eye — `feet.y + 286 - view_height` is the camera's own
     * expression (FORMATS §9.12) — so it crouches when the view crouches without
     * anything here having to know that.
     */
    if (c->vw_model_ready) {
        q2_model_instance proto;
        q2_model_draw_stats mstats;
        s16 aim[3], kick[3];

        q2_model_instance_init(&proto);
        proto.tpage         = &c->render.tpage;
        proto.clut4_count_a = c->clut4_count_a;

        aim[0]  = (s16)c->sim[0].player[0].pitch;
        aim[1]  = (s16)c->sim[0].player[0].yaw;
        aim[2]  = (s16)c->sim[0].player[0].roll;
        kick[0] = c->sim[0].combat.kick[0];
        kick[1] = c->sim[0].combat.kick[1];
        kick[2] = c->sim[0].combat.kick[2];

        q2_vw_build_ot(&c->vw, &proto,
                       c->sim[0].player[0].pos, c->sim[0].player[0].view_height,
                       aim, kick, &c->cam, ot, gte, &mstats);
    }

    /*
     * The glint, OFF by default (F6 shows it).
     *
     * Nothing the engine does raises the `0x04000000` flag it draws on — only
     * BIGGUN's level script does, and this port does not run relocated level
     * modules yet. Drawing one anyway would be putting an effect on screen that
     * the console never puts there, so the reconstruction sits behind a toggle
     * and the default frame has no glint in it.
     *
     * The phase runs 4..1, which is what the script writes and what the band
     * formula's `4 - phase` expects.
     */
    if (c->sim[0].glint.ready && (c->sim[0].glint.raised || c->show_glint)) {
        s32 at[3];

        q2_sim_eye(&c->sim[0], at);
        q2_fx_glint_draw(&c->sim[0].glint, at, c->cam.yaw, &c->cam, ot, gte);
    }
}

static void client_frame(client *c)
{
    void *pixels;
    int pitch;
    const psx_framebuffer *front;
    q2_screen_hooks hooks;

    q2_screen_frame_begin(&c->screen, &c->ot);

    /*
     * 0x800780C0 clears the whole screen once and turns the per-viewport clears
     * off, which is what paints the gutters between split viewports.
     */
    c->screen.disp.bg_rgb[0] = 16;
    c->screen.disp.bg_rgb[1] = 16;
    c->screen.disp.bg_rgb[2] = 32;
    c->screen.disp.bg_enable = 1;
    c->screen.background_enable = true;

    /*
     * What the water effect reads off view+288, published before the build
     * because the effect writes the shake and the build's draw envs read it.
     *
     * On the console this is not a publication at all: the viewport holds a
     * pointer to its player's entity and reads bit 0x100 of the flag word
     * itself. The port has no such pointer to hand the screen, so the one bit
     * it wants is handed over instead. Every viewport gets the same answer for
     * the same reason they all get the same camera — there is one player.
     */
    {
        bool submerged =
            (c->sim[0].player[0].ent.flags & Q2_ENT_UNDERWATER) != 0;
        int p;

        for (p = 0; p < c->screen.view_count; p++)
            q2_screen_water_set(&c->screen, p, true, submerged);
    }

    memset(&hooks, 0, sizeof(hooks));
    hooks.view = client_draw_view;
    hooks.user = c;
    q2_screen_build(&c->screen, &c->ot, &c->gte, &hooks);

    /* Every viewport has now drawn from the beam queue, so it can go. This is
     * the tail of 0x80064F10, moved out to where "the last viewport" is a
     * thing that can be said. */
    q2_fx_beams_reset(&c->sim[0].fx);

    /*
     * The menu is part of the frame, not something painted over it afterwards.
     * It links into the overlay slice (menudraw.h) BEFORE composition, so the
     * one walk of the ordering table produces the world and then the menu on
     * top of it — which is what the console does, and is why the frozen world
     * shows through where the menu draws nothing.
     */
    if (c->menu.open && c->menu_font_ready) {
        q2_menu_draw_opts mo;

        q2_menu_draw_opts_default(&mo, &c->menu_font);
        /* The layout is authored for 512x248; centre that block in whatever
         * this window is rather than scaling 4bpp texels. */
        mo.origin_x = (c->width  - Q2_MENU_SCREEN_W) / 2;
        mo.origin_y = (c->height - Q2_MENU_SCREEN_H) / 2;
        mo.view_x   = 0;
        mo.view_w   = c->width < Q2_MENU_SCREEN_W ? c->width
                                                  : Q2_MENU_SCREEN_W;
        q2_menu_build_ot(&c->menu, &c->ot, &mo);
    }

    /*
     * The card front end, through the same path � its screens ARE menu pages
     * in every respect but having a page id, so they draw with the same font,
     * the same bar and the same rules.
     */
    if (c->mcard_open && c->menu_font_ready && c->card_menu.page) {
        q2_menu_draw_opts mo;

        q2_menu_draw_opts_default(&mo, &c->menu_font);
        mo.origin_x = (c->width  - Q2_MENU_SCREEN_W) / 2;
        mo.origin_y = (c->height - Q2_MENU_SCREEN_H) / 2;
        mo.view_x   = 0;
        mo.view_w   = c->width < Q2_MENU_SCREEN_W ? c->width
                                                  : Q2_MENU_SCREEN_W;

        q2_menu_build_ot(&c->card_menu, &c->ot, &mo);
    }

    /*
     * The overlay, whenever neither the menu nor the mission screen is up —
     * which is the console's arrangement, since both of those are the overlay
     * camera's and it draws one thing at a time. The crosshair follows the
     * PLAYER page's setting, which is the one menu toggle the HUD reads
     * (0x80043A58).
     */
    if (c->hud_ready && c->hud_font_ready &&
        !c->menu.open && !c->mission_open && !c->mcard_open) {
        q2_hud_ctx ctx;

        c->hud.crosshair = (c->settings.v[Q2_SET_CROSSHAIR] != 0);
        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_build_ot(&c->hud, &c->hud_font, &ctx, &c->ot, 0);
    }

    /*
     * The end-of-match scoreboard — what Q2_MP_REQ_RESULTS asks for.
     *
     * The console loads QMRESULT, a level directory of its own whose LevelBin
     * draws the screen; this shows the lines that module composes, in its
     * order, with the overlay's own text emitter. The words are the module's,
     * read out of its strings; the LAYOUT is not — where QMRESULT puts each
     * line goes through engine text calls whose offsets have not been read, so
     * these are stacked and centred rather than placed.
     */
    if (c->mp_scoreboard && c->hud_font_ready) {
        char lines[12][Q2_MP_SCORE_LINE];
        u32 n = q2_mp_scoreboard(&c->mp, NULL, lines, 12);
        u32 li;
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);

        /*
         * A line's position is the CONTEXT's home, not the pen's x and y —
         * the pen carries state across a string, the context says where the
         * string starts. Setting the pen instead put all five lines on one
         * row, each starting where the last one ended.
         */
        for (li = 0; li < n; li++) {
            /* `q2_hud_measure` is the original's measurer and returns
             * CHARACTERS, not pixels — the glyph advance is a constant 8. */
            ctx.home_x = (s16)(ctx.width / 2 -
                               q2_hud_measure(lines[li]) * 8 / 2);
            ctx.home_y = (s16)(ctx.height / 4 + (s32)li * 16);
            q2_hud_print(&c->hud_font, &ctx, &pen, &c->ot, 0, lines[li]);
        }
    }

    /*
     * The mission screen, into the same overlay slice — which is where the
     * console's own overlay camera puts it (mission.h). It is mutually
     * exclusive with the menu because opening it closes the menu.
     */
    if (c->mission_open && c->hud_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        /* The HUD layer takes a DEPTH rather than a bucket (gpu.h), and zero
         * is the front — which lands in the overlay slice, since no viewport
         * window is installed once q2_screen_build has returned. */
        q2_mission_build_ot(&c->mission, &c->hud_font, &ctx, &pen,
                            &c->ot, 0);
    }

    /*
     * The briefing screen — the panel, the text over it, and the BACK prompt
     * sliding up from the bottom. Mutually exclusive with the other two
     * overlay screens for the same reason they are with each other.
     */
    if (c->briefing_open && c->hud_font_ready && c->menu_font_ready) {
        q2_hud_ctx ctx;
        q2_hud_pen pen;

        q2_hud_ctx_centre_in(&ctx, c->width, c->height);
        q2_hud_pen_default(&pen);
        q2_briefing_build_ot(&c->briefing, &c->hud_font, &c->menu_font,
                             &ctx, &pen, &c->ot, 2, 1, 0);
        q2_prompt_build_ot(&c->prompts, &c->menu_font, &c->ot, 1);
    }

    /* The prompts animate whether or not anything is showing them, which is
     * how they slide away when a screen closes (prompt.h). */
    q2_prompt_step(&c->prompts);

    q2_screen_compose(&c->screen, &c->ot, c->vram, &c->opts);

    q2_screen_present(&c->screen);
    front = q2_screen_front(&c->screen);

    /*
     * The capture comes off the finished front buffer, before anything SDL
     * touches it — so a headless run and a windowed one write byte-identical
     * frames, and neither depends on a driver's idea of what a 15-bit texture
     * looks like.
     */
    if (c->shot_path && c->shot_every > 0 &&
        (c->frame_index % c->shot_every) == 0)
        client_write_shot(c, true);

    if (!c->texture || !c->renderer)
        return;

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
    {
        /*
         * SCREEN POSITION, honoured — openquestions #40.
         *
         * The page writes `0x800B3368` / `0x800B336A` (defaults 0 and 24) and
         * an exhaustive sweep finds **no reader anywhere in the executable**:
         * the obvious consumer would be the display env's screen rectangle,
         * which `SetDefDispEnv` explicitly zeroes. So on this build the page is
         * inert, and the port must not pretend otherwise about the CONSOLE.
         *
         * It can still do the honest thing for the player: a control that
         * exists and does nothing is a bug from the outside. The offset is
         * applied here, at presentation, where it shifts the finished image the
         * way a television's own position control would — and nowhere near the
         * ordering table, so it cannot perturb clipping or the viewport
         * rectangles that the reconstruction does depend on.
         *
         * The default y of 24 is treated as the neutral point, because that is
         * what the reset routine writes and a fresh install must not be
         * off-centre.
         */
        /*
         * THE PICTURE'S SHAPE, which is not the buffer's.
         *
         * The GPU's five horizontal modes all span the same active line, so a
         * 512-wide frame is the same picture as a 320-wide one with pixels half
         * as wide; PAL fills the 4:3 raster with 256 lines. That makes a
         * framebuffer pixel exactly 2:3, and blitting the buffer to fill the
         * window — which is what this did — a 1.5x horizontal stretch.
         *
         * q2_screen_fit_rect does the whole of it: the largest rectangle of the
         * right shape that fits, centred, with the rest of the window left as
         * border. It takes any window aspect, so a 16:9 monitor pillarboxes and
         * a tall window letterboxes without this having to know which.
         */
        SDL_FRect dst;
        int out_w = 0, out_h = 0;
        int px = 0, py = 0, pw = 0, ph = 0;
        float sx = (float)c->settings.v[Q2_SET_SCREEN_X];
        float sy = (float)(c->settings.v[Q2_SET_SCREEN_Y] - 24);

        SDL_GetCurrentRenderOutputSize(c->renderer, &out_w, &out_h);
        q2_screen_fit_rect(&c->screen, c->fit, out_w, out_h,
                           &px, &py, &pw, &ph);

        /*
         * SCREEN POSITION moves the picture, so its units are buffer pixels
         * scaled by the PICTURE's size and not by the window's — otherwise the
         * same setting would shift by a different amount depending on how much
         * of the window is border.
         */
        dst.x = (float)px + sx * (float)pw / (float)Q2_SCREEN_PAL_WIDTH;
        dst.y = (float)py + sy * (float)ph / (float)Q2_SCREEN_PAL_HEIGHT;
        dst.w = (float)pw;
        dst.h = (float)ph;

        SDL_RenderTexture(c->renderer, c->texture, NULL, &dst);
    }
    SDL_RenderPresent(c->renderer);
}

/* ------------------------------------------------------------------------- */
static void usage(void)
{
    printf("q2psx - native Quake II PSX\n\n");
    printf("usage: q2psx --disc <path> [--map NAME] [--zone N] [--scale N]\n"
           "             [--aspect MODE] [--saves DIR]\n\n");
    printf("  --disc   a .cue, .bin, .img or .iso, or a drive letter\n");
    printf("  --map    level directory name (default BASE0)\n");
    printf("  --zone   zone index within the map (default 0)\n");
    printf("  --scale  buffer pixels across, i.e. horizontal zoom (default 3)\n");
    printf("  --aspect 4:3 (default) | tv | square | stretch\n");
    printf("           4:3    the drawn buffer as 4:3, as the game is played\n");
    printf("           tv     the strict pixel shape, 2:3 on PAL: 3%% taller\n");
    printf("           square one buffer pixel per window pixel: a 1.5x stretch\n");
    printf("           stretch fill the window, whatever shape it is\n");
    printf("  --saves  where save files live (default: the platform's own)\n");
    printf("\n  running without a player:\n");
    printf("  --headless    no window, no audio; a fixed 1/30 s step\n");
    printf("  --demo        drive the pad from a fixed script rather than keys\n");
    printf("  --frames N    stop after N frames\n");
    printf("  --shot P.ppm  write the console's own framebuffer to P.ppm\n");
    printf("  --shot-every N  ...and one every N frames, numbered\n");
}

int main(int argc, char **argv)
{
    client c;
    const char *disc_path = NULL;
    const char *map = "BASE0";
    bool map_given = false;
    int zone_index = 0;
    int scale = 3;
    int i;
    u64 last;

    memset(&c, 0, sizeof(c));
    /* Deathmatch settings, applied after the map loads. -1 keeps the
     * shipped default the session initialiser installs. */
    q2_mp_mode mp_mode    = Q2_MP_DEATHMATCH;

    c.trace_cre = -1;
    int        mp_players = 2;
    s16        mp_frags   = -2;
    s16        mp_minutes = -2;


    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc)       disc_path = argv[++i];
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) { map = argv[++i]; map_given = true; }
        else if (!strcmp(argv[i], "--zone") && i + 1 < argc)  zone_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless"))              c.headless = true;
        else if (!strcmp(argv[i], "--demo"))                  c.demo = true;
        else if (!strcmp(argv[i], "--watch"))                 c.watch = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            c.frames_total = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            c.shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc)
            c.shot_every = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) q2_save_set_dir(argv[++i]);
        else if (!strcmp(argv[i], "--dm")) {
            c.mp_enabled = true;
            if (!map_given) { map = "MATRIX1"; map_given = true; }
        }
        else if (!strcmp(argv[i], "--dm-mode") && i + 1 < argc)
            mp_mode = (q2_mp_mode)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-players") && i + 1 < argc)
            mp_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-stage"))
            c.mp_stage = true;
        else if (!strcmp(argv[i], "--trace-cre") && i + 1 < argc)
            c.trace_cre = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-frags") && i + 1 < argc)
            mp_frags = (s16)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dm-minutes") && i + 1 < argc)
            mp_minutes = (s16)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) {
            const char *a = argv[++i];
            if      (!strcmp(a, "4:3"))     c.fit = Q2_SCREEN_FIT_FULL_4_3;
            else if (!strcmp(a, "tv"))      c.fit = Q2_SCREEN_FIT_TELEVISION;
            else if (!strcmp(a, "square"))  c.fit = Q2_SCREEN_FIT_SQUARE;
            else if (!strcmp(a, "stretch")) c.fit = Q2_SCREEN_FIT_STRETCH;
            else { usage(); return 1; }
        }
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

    /* Texture pages start at ABR 0 and are promoted as opaque geometry is
     * drawn, exactly as the engine's own table is. */
    q2_world_render_init(&c.render);

    /*
     * A headless run brings SDL up at all only to keep the shutdown path
     * uniform; there is no video, no audio device and no window. Everything the
     * frame needs — the ordering table, the rasteriser, the screen — is the
     * port's own code and does not know SDL exists.
     */
    if (c.headless) {
        Q2_INFO("headless: %ld frame%s at 1/30 s%s", c.frames_total,
                c.frames_total == 1 ? "" : "s", c.demo ? ", demo pad" : "");
        if (c.frames_total <= 0)
            c.frames_total = 1;
        goto no_window;
    }

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

    /*
     * The window opens at `scale` buffer pixels across and however tall the
     * console's pixel shape makes that — 1536 x 1116 at the default scale of 3,
     * not the 1536 x 744 the buffer's own dimensions suggest. Stretching
     * vertically rather than squeezing horizontally keeps every one of the 512
     * columns the 512-wide mode was chosen for.
     *
     * It is clamped to the display it opens on so a large scale on a small
     * screen does not put the title bar off the top, and the fit is recomputed
     * from the real window size every frame anyway, so a clamped window is
     * simply a smaller correct picture.
     */
    {
        int ww = 0, wh = 0;
        SDL_Rect usable;

        q2_screen_window_size(&c.screen, c.fit, scale, &ww, &wh);

        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
            usable.w > 0 && usable.h > 0) {
            int max_w = usable.w * 9 / 10;
            int max_h = usable.h * 9 / 10;

            if (ww > max_w) { wh = (int)((s64)wh * max_w / ww); ww = max_w; }
            if (wh > max_h) { ww = (int)((s64)ww * max_h / wh); wh = max_h; }
        }

        if (ww < 64) ww = 64;
        if (wh < 64) wh = 64;

        c.window = SDL_CreateWindow("Q2PSX-PC", ww, wh, SDL_WINDOW_RESIZABLE);
    }
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

no_window:
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
    q2_menu_set_multiplayer(&c.menu, false);

    /* The level-completion screen. Its counters are the sim's to fill; until
     * kills and secrets are tallied it honestly reads zero. */
    q2_mission_init(&c.mission);
    q2_briefing_init(&c.briefing);
    q2_prompt_init(&c.prompts);

    /*
     * The memory-card front end, with the port's file-backed save system behind
     * its three function pointers. The signatures match exactly, so this is a
     * plain assignment rather than three thunks.
     */
    q2_save_ui_init(&c.save_ui);
    c.mcard_host.poll    = q2_save_ui_poll;
    c.mcard_host.request = q2_save_ui_request;
    c.mcard_host.choose  = q2_save_ui_choose;
    c.mcard_host.user    = &c.save_ui;
    q2_mcard_init(&c.mcard, &c.mcard_host);

    /* The shadow menu the card screens navigate and draw through. It shares the
     * settings block so a screen with a widget on it would work, though none
     * of the nine has one. */
    q2_menu_init(&c.card_menu, &c.settings, Q2_MENU_SCREEN_H);

    Q2_INFO("saves: %s", q2_save_dir());

    /*
     * The UI's tables come out of the boot executable, not off the disc's data
     * files: the glyph coordinates for the 8-pixel face and — the part that
     * matters here — the built-in palette bank every UI primitive samples
     * through (hudtables.h §"Palettes"). Without it the menu's letterforms are
     * in VRAM with no colours to read them by, so this is a hard requirement
     * for the menu rather than an optional extra, and it is loaded once
     * because a build's tables do not change per level.
     */
    c.hud_tables_ready = (q2_hud_tables_load(&c.hud_tables, c.disc,
                                             &c.build) == Q2_OK);

    /* The status bar's tables, from the same executable. */
    c.icons_ready = (q2_icon_tables_load(&c.icons, c.disc, &c.build) == Q2_OK);
    if (c.icons_ready)
        q2_statusbar_init(&c.sbar, &c.icons, 1);
    else
        Q2_WARN("no status-bar tables for this build");
    if (!c.hud_tables_ready)
        Q2_WARN("no UI tables for this build — the menu will not draw");

    /*
     * The overlay, AFTER the tables it reads. This block used to sit above the
     * load, testing a flag that `memset(&c, 0, ...)` had just cleared, so
     * `q2_hud_init` never ran and `hud_ready` never became true — the client
     * drew no notifications, no centre line and no crosshair, on a build whose
     * tables load perfectly well.
     */
    if (c.hud_tables_ready) {
        /* One player, so four notification lines — the table at 0x8009D648
         * indexed by player count (hudtables.h). */
        q2_hud_init(&c.hud, &c.hud_tables, 1);
        c.hud.crosshair = (c.settings.v[Q2_SET_CROSSHAIR] != 0);
        c.hud_ready = true;
        q2_hud_message(&c.hud, "Quake II");
    }

    /*
     * The view weapon's animation bank, out of the same executable. It is per
     * disc rather than per map because the clips are code-segment data — only
     * the model the clips drive comes off a map.
     */
    c.vm_ready = (q2_vm_tables_load(&c.vm_tables, c.disc, &c.build) == Q2_OK);
    if (c.vm_ready)
        Q2_INFO("view weapon: %u animation keys", c.vm_tables.key_count);
    else
        Q2_WARN("no view-model bank for this build — no weapon in hand");

    /*
     * The item table, from the same executable. A build with no catalogued
     * addresses falls back to the transcribed PAL table rather than to no items:
     * unlike the effect ramps, the transcription is checked against the disc on
     * every run of `q2psx-inspect items`, so it is a known-good copy of exactly
     * this data rather than a guess.
     */
    c.item_table_ready = (q2_item_table_load(&c.item_table, c.disc,
                                             &c.build) == Q2_OK);
    if (c.item_table_ready)
        Q2_INFO("item table: %u records", c.item_table.count);
    else
        Q2_WARN("no item table for this build — using the built-in one");

    /*
     * The effect tables, from the same executable and for the same reason: a
     * ramp is nineteen gradients at a fixed address, and a build we have no
     * addresses for gets no effects rather than nineteen gradients read out of
     * somebody else's data.
     */
    c.fx_tables_ready = (q2_fx_tables_load_disc(&c.fx_tables, c.disc,
                                                &c.build) == Q2_OK);
    if (!c.fx_tables_ready)
        Q2_WARN("no effect tables for this build — nothing will spark");

    /*
     * Boot into the FRONT END, which is what the console does: `q2_menu_open`
     * special-cases page 46 (0x8001A40C) and the level it draws over is record
     * 0 of the level table, `QFront` -> `LEVELS/QFRONT/`.
     *
     * `--map` overrides it, because going straight to a level is what every
     * capture and every check in this project wants; without one, the game
     * starts where a player starts it.
     */
    snprintf(c.first_map, sizeof(c.first_map), "%s", map);
    if (!map_given) {
        c.in_front_end = true;
        map = "QFRONT";
        zone_index = 0;
    }

    /*
     * Start the match BEFORE the map loads, because placing the local player is
     * part of loading it and the spawn selector needs the session to exist.
     *
     * This is what the port never did: multiplayer.[ch] reconstructs the whole
     * of QMULTI.C — the scoring, the frag and time limits, the VERSUS round
     * rules, the banner countdown and the two game-state requests — and not one
     * of those entry points had a caller anywhere in the game. The rules ran in
     * the test suite and nowhere else.
     */
    if (c.mp_enabled) {
        q2_mp_session_init(&c.mp, mp_mode, mp_players);
        if (mp_frags != -2)
            c.mp.frag_limit = mp_frags;
        if (mp_minutes != -2)
            c.mp.time_limit = mp_minutes;
        c.mp_rng_state = 0x13572468u;

        /*
         * The split the session implies. The layout is chosen by the PLAYER
         * COUNT at 0x800B3356 through the jump table at 0x800AC90C: one or none
         * is full screen, two is a split whose axis is the HORIZONTAL SPLIT
         * setting, three is the quad layout with the view count forced to three
         * (0x8003FAE4), and four is the quad. Until now the client installed
         * ONE and left every other layout to an F5 debug cycle.
         */
        {
            q2_screen_layout lay = Q2_SCREEN_LAYOUT_ONE;
            int views = c.mp.player_count;

            if (views < 1)
                views = 1;

            if (views == 2)
                lay = c.settings.v[Q2_SET_HORIZONTAL_SPLIT]
                          ? Q2_SCREEN_LAYOUT_TWO_H : Q2_SCREEN_LAYOUT_TWO_V;
            else if (views >= 3)
                lay = Q2_SCREEN_LAYOUT_QUAD;

            q2_screen_set_layout(&c.screen, lay, views);
            Q2_INFO("multiplayer: %s, %d viewport%s",
                    q2_screen_layout_name(lay), c.screen.view_count,
                    c.screen.view_count == 1 ? "" : "s");
        }

        Q2_INFO("multiplayer: %s, %d players, frag limit %d, time limit %d min"
                "%s", q2_mp_mode_name(mp_mode), c.mp.player_count,
                c.mp.frag_limit, c.mp.time_limit,
                q2_mp_mode_selectable(mp_mode) ? ""
                    : "  (this mode is CUT — the front end cannot select it)");
    }

    if (!client_load_zone(&c, map, zone_index)) {
        fprintf(stderr, "cannot load %s zone %d\n", map, zone_index);
        goto done;
    }

    /*
     * Level music — this map's own playlist, not "track A because the mapping
     * is not decoded yet".
     *
     * The level record carries seven track ids and a jump-back byte, and an id
     * names a file and a channel through the table at 0x800A1DD8 (musictable.h).
     * A build whose tables are not catalogued falls silent rather than picking
     * a track that would be somebody else's.
     */
    c.music_table_ready = (q2_music_table_load(&c.music_table, c.disc,
                                               &c.build) == Q2_OK);
    c.level_table_ready = (q2_level_table_load(&c.level_table, c.disc,
                                               &c.build) == Q2_OK);
    if (c.level_table_ready)
        c.level = q2_level_find(&c.level_table, c.map);

    c.music_cursor_at = -1;
    if (c.level && c.music_table_ready) {
        int id = q2_level_playlist_next(c.level, &c.music_cursor_at);
        if (id >= 0)
            client_music_play_id(&c, id);
        Q2_INFO("music: %s plays %d first", c.map, id);
    } else {
        Q2_WARN("no music playlist for %s", c.map);
    }

    /*
     * A session starts IN the game. The free-fly camera is a debug view for
     * looking at geometry with no physics in the way, and F4 is how you get to
     * it; booting into it meant a fresh launch ran none of the player's frame —
     * no movement model, no view kicks, no weapon in the hands, no status bar —
     * until a key was pressed that nothing tells the player about. A loaded save
     * already forced this on for exactly the same reason.
     */
    c.sim_enabled = true;

    /* The title screen is the menu's page 46 over the QFRONT scene. It is
     * opened rather than drawn, so it navigates with the same engine, the same
     * selection bar and the same font as every other page. */
    if (c.in_front_end) {
        q2_menu_open(&c.menu);
        q2_menu_goto(&c.menu, Q2_PAGE_FRONT_TITLE);
    }

    c.running = true;
    last = c.headless ? 0 : SDL_GetTicks();

    while (c.running) {
        SDL_Event ev;
        u64   now;
        float dt;

        /*
         * A scripted run advances on a FIXED step rather than on the wall
         * clock, so its output is a function of the frame number alone. The
         * step is the console's own 1/30 s — everything the port times is in
         * 1/300 s units and the screen clamps a frame at 30 of them.
         */
        if (c.headless) {
            dt = 1.0f / 30.0f;
        } else {
            now = SDL_GetTicks();
            dt  = (float)(now - last) / 1000.0f;
            last = now;

            if (dt > 0.1f)
                dt = 0.1f;
        }

        while (!c.headless && SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                c.running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE:
                    /* START on the console: it opens the pause menu, and
                     * closes it again from the root page. Deeper in, the
                     * menu's own TRIANGLE handling owns going back. The
                     * mission screen sits in front of all of that and takes
                     * the press first. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else if (c.mission_open)
                        c.mission_open = false;
                    else if (!c.menu.open)
                        q2_menu_open(&c.menu);
                    else if (c.menu.depth == 0)
                        q2_menu_close(&c.menu);
                    break;
                case SDLK_F12:
                    /*
                     * The briefing. On the console it is shown between levels
                     * by the outer state machine; what triggers it is not
                     * established, so it gets a key rather than an invented
                     * trigger — the same call the memory-card screens got.
                     */
                    c.briefing_open = !c.briefing_open;
                    if (c.briefing_open) {
                        c.menu.open = false;
                        c.mission_open = false;
                        q2_prompt_show(&c.prompts, Q2_PROMPT_BACK, 216);
                    } else {
                        q2_prompt_hide_all(&c.prompts);
                    }
                    break;
                case SDLK_F7:
                    /* The card front end, saving. On the console it is reached
                     * from SAVE?'s YES (page 39); what SHOWS that prompt is not
                     * established, so the port gives it a key rather than
                     * inventing a trigger. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else
                        client_card_open(&c, Q2_SAVE_UI_SAVE);
                    break;
                case SDLK_F8:
                    /* The same front end, loading. Same screens, same rules,
                     * the other direction. */
                    if (c.mcard_open)
                        client_card_close(&c);
                    else
                        client_card_open(&c, Q2_SAVE_UI_LOAD);
                    break;
                /* Not while a screen is up: quick load reloads the zone, and
                 * doing that under an open front end would pull the world out
                 * from under it. */
                case SDLK_F9:
                    if (!c.mcard_open && !c.menu.open)
                        client_quick_save(&c);
                    break;
                case SDLK_F10:
                    if (!c.mcard_open && !c.menu.open)
                        client_quick_load(&c);
                    break;
                case SDLK_F11:
                    /* The framebuffer, not the window — see above. */
                    client_screenshot(&c);
                    break;
                case SDLK_V: {
                    /*
                     * How the picture is shaped on the way out. The default is
                     * 4:3; `square` is the raw buffer, which is what every
                     * framebuffer dump of this game looks like and is a 1.5x
                     * horizontal stretch of what a television showed. Having
                     * both a key away is the point — the two are easy to argue
                     * about and trivial to compare.
                     */
                    int next = (int)c.fit + 1;
                    int pn = 1, pd = 1;

                    if (next >= Q2_SCREEN_FIT_COUNT)
                        next = 0;
                    c.fit = (q2_screen_fit)next;
                    q2_screen_pixel_aspect(&c.screen, &pn, &pd);
                    Q2_INFO("aspect: %s (console pixel %d:%d)",
                            q2_screen_fit_name(c.fit), pn, pd);
                    break;
                }
                case SDLK_F1: c.opts.dither    = !c.opts.dither;    break;
                case SDLK_F2: c.opts.affine_uv = !c.opts.affine_uv; break;
                case SDLK_F3:
                    /*
                     * Submerge. Nothing in the port yet resolves a trigger
                     * volume's event to UNDERWATER, so this stands in for the
                     * volume exactly as the crouch key does — it sets the flag
                     * the dispatcher would set, and everything downstream is
                     * the console's: the swimming physics, and the screen
                     * effect that ramps up over about fourteen frames.
                     */
                    c.force_underwater = !c.force_underwater;
                    Q2_INFO("underwater: %s", c.force_underwater ? "on" : "off");
                    break;
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
                case SDLK_F6:
                    /* The glint. Off by default because only BIGGUN's level
                     * script raises the flag that draws it, and this port does
                     * not run relocated level modules — so showing one is a
                     * deliberate look at a reconstruction, not gameplay. */
                    c.show_glint = !c.show_glint;
                    Q2_INFO("glint: %s%s", c.show_glint ? "on" : "off",
                            c.sim[0].glint.ready ? "" : " (this map has no mesh)");
                    break;
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

        if (c.mcard_open) {
            /*
             * The card front end sits in front of everything: it is a separate
             * engine with its own state and its own release rule, not a page,
             * so it takes the pad first and nothing ticks underneath it.
             */
            client_card_frame(&c);
        } else if (c.menu.open) {
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
            if (q2_sim_take_zone_change(&c.sim[0], &target)) {
                Q2_INFO("zone gate -> zone %u", target);
                client_load_zone(&c, c.map, (int)target);
            }
        }
        /* Every frame, not just menu frames: a zone load rebuilds the sim and
         * would otherwise drop back to the compiled-in constants. */
        client_apply_settings(&c);
        client_music_pump(&c);

        /*
         * The screen's own clock, in the 1/300 s units everything the console
         * times is expressed in, clamped at 30 the way 0x800184B8 clamps it.
         *
         * It matters now that something reads it: the water effect ramps by
         * 24 per unit, so without this a 144 Hz host would fade the effect in
         * three times faster than the console does. Driving it from the real
         * elapsed time is what keeps a frame-rate-independent port timing the
         * effect the way the hardware timed it.
         */
        q2_screen_tick_dt(&c.screen, (double)dt);
        client_frame(&c);

        c.frame_index++;
        if (c.frames_total > 0 && c.frame_index >= c.frames_total)
            c.running = false;
    }

    /*
     * The last frame, always — a run with no `--shot-every` asks for one
     * picture and gets exactly that, at the name it gave, with no number in it.
     */
    if (c.shot_path && (c.shot_every <= 0 || c.shots_written == 0)) {
        c.frame_index--;
        client_write_shot(&c, false);
    }

done:
    if (c.hud_tables_ready)
        q2_hud_tables_free(&c.hud_tables);
    if (c.sfx_ready)
        q2_sound_bank_free(&c.sfx);
    if (c.icons_ready)
        q2_icon_tables_free(&c.icons);
    if (c.level_table_ready)
        q2_level_table_free(&c.level_table);
    q2_save_ui_free(&c.save_ui);
    q2_save_free(&c.snapshot);
    q2_sim_free(&c.sim[0]);
    q2_common_close(&c.common);
    q2_world_free_zone(&c.zone);
    free(c.vram);
    q2_vm_tables_free(&c.vm_tables);
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
