/* Exercise the actual client glue without a disc, window or physical input.
 * Keeping main in this translation unit lets regressions in its pad latches,
 * respawn callers and owner selection be caught by ordinary CTest runs. */
int q2_test_client_main(int argc, char **argv);
#define main q2_test_client_main
#include "../src/client/main.c"
#undef main

static int failures;
static int checks;
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; } } while (0)

static client *fixture(int players)
{
    client *c = calloc(1, sizeof(*c));
    q2_inventory inv;
    int pi;
    if (!c) exit(2);
    c->headless = true;
    c->frame_index = 1;
    c->pad_frame = 0;
    c->mp_enabled = true;
    c->sim_enabled = true;
    q2_sim_init(&c->sim[0], NULL, 50);
    c->sim[0].multiplayer = true;
    c->sim[0].fire_from_input = false;
    q2_inventory_init(&inv);
    q2_mp_session_init(&c->mp, Q2_MP_DEATHMATCH, players);
    for (pi = 0; pi < players; pi++) {
        s32 feet[3] = {pi * 2000, 0, 0};
        q2_sim_select_player(&c->sim[0], pi);
        q2_sim_spawn(&c->sim[0], feet, 0);
        q2_sim_player_loadout(&c->sim[0], pi, &inv, 1);
        q2_player_death_init(&c->death[pi]);
        c->sim[0].player[pi].look_scheme = Q2_PAD_STYLE_RIGHT_STICK;
        c->sim_ready[pi] = true;
    }
    q2_sim_select_player(&c->sim[0], 0);
    c->sim_ready[0] = false; /* The client uses this array for EXTRA slots. */
    c->mp_start_valid = true;
    c->mp_start_inv = inv;
    c->mp_start_weapon = 1;
    c->mp_spawn_count = 1;
    c->mp_spawns[0].present = true;
    c->mp_spawns[0].pos[0] = 9000;
    return c;
}

static void release_fixture(client *c)
{
    q2_sim_free(&c->sim[0]);
    free(c);
}

static void buttons(client *c, int pi, SDL_GamepadButton button, bool down)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = c->gamepads.id[pi];
    event.gbutton.button = (Uint8)button;
    event.gbutton.down = down;
    CHECK(q2_gamepads_event(&c->gamepads, &event) == pi);
}

static void test_inputs(void)
{
    client *c = fixture(4);
    q2_input in;
    int pi;
    c->shoot = true;
    client_input_simulated(c, 1.0f / 30.0f);
    CHECK(c->mp_input[0].attack);
    for (pi = 1; pi < 4; pi++) CHECK(!c->mp_input[pi].attack);
    for (pi = 1; pi < 4; pi++) {
        SDL_Event ev;
        c->gamepads.id[pi] = (SDL_JoystickID)(100 + pi);
        memset(&c->mp_pad[pi], 0, sizeof(c->mp_pad[pi]));
        /* A shoulder tap starts and ends between ticks. It must survive,
         * then stop firing on the next tick without affecting another pad. */
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
        client_extra_input(c, pi, 0, false, &in);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(in.attack && (in.buttons & Q2_BTN_ATTACK_PRESS));
        client_extra_input(c, pi, 12, false, &in);
        CHECK(!in.attack);
        /* Resume remains pending across a non-ticking frame. */
        buttons(c, pi, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
        client_extra_input(c, pi, 0, true, &in);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(in.attack && !(in.buttons & Q2_BTN_ATTACK_PRESS));
        /* Disconnect releases both held input and accumulated taps. */
        memset(&ev, 0, sizeof(ev));
        ev.type = SDL_EVENT_GAMEPAD_REMOVED;
        ev.gdevice.which = c->gamepads.id[pi];
        CHECK(q2_gamepads_event(&c->gamepads, &ev) == pi);
        client_extra_input(c, pi, 12, false, &in);
        CHECK(!in.attack && !in.forward && !in.yaw);
    }
    /* Weapon-cycle edges reach each owner's inventory and carousel. */
    c->sim[0].pcombat[2].inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_SHOTGUN];
    c->sim[0].pcombat[2].inv.ammo[Q2_AMMO_SHELLS] = 20;
    memset(&in, 0, sizeof(in)); in.buttons = Q2_BTN_WEAP_NEXT;
    client_cycle_input(c, 2, &in);
    CHECK(c->sim[0].pcombat[2].weapon_id == 2);
    CHECK(c->sim[0].combat.weapon_id == 1);
    CHECK(c->sim[0].cur_player == 0);
    release_fixture(c);
}

static void test_pickups_and_respawn(void)
{
    client *c = fixture(4);
    int pi, live;
    /* A real shared item sweep must let each extra player collect a nearby
     * shell box, without granting it to the current combat owner. */
    for (pi = 1; pi < 4; pi++) {
        q2_pop_place place;
        q2_entity *item;
        memset(&place, 0, sizeof(place));
        place.id = 27; /* Shells P, touch effect 18. */
        place.x = c->sim[0].player[pi].pos[0];
        item = q2_item_spawn(&c->sim[0].entities, &place, NULL, 0, NULL);
        CHECK(item != NULL);
    }
    c->sim[0].ent_world.dt = 12;
    c->sim[0].ent_world.deathmatch = true;
    q2_entity_run(&c->sim[0].entities, &c->sim[0].ent_world);
    CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_SHELLS] == 0);
    for (pi = 1; pi < 4; pi++)
        CHECK(c->sim[0].pcombat[pi].inv.ammo[Q2_AMMO_SHELLS] > 0);
    for (live = 0; live < 4; live++) {
        q2_sim_select_player(&c->sim[0], live);
        for (pi = 0; pi < 4; pi++) {
            q2_inventory *inv = pi == live ? &c->sim[0].combat.inv
                                           : &c->sim[0].pcombat[pi].inv;
            CHECK(c->sim[0].ent_world.player[pi].present);
            CHECK(c->sim[0].ent_world.player[pi].inv == inv);
            /* These are the actual pointers used by the shared item sweep. */
            c->sim[0].ent_world.player[pi].inv->health = (s16)(60 + pi);
        }
    }
    q2_sim_select_player(&c->sim[0], 0);
    c->sim[0].combat.inv.health = 3;
    c->sim[0].combat.weapon_id = 11;
    c->sim[0].pcombat[2].self.last_attacker = 1;
    c->sim[0].pcombat[2].self.effect[2] = 30;
    c->sim[0].pcombat[2].shot_serial = 123;
    c->sim[0].pcombat[2].kick[0] = 55;
    CHECK(client_mp_respawn(c, 2));
    CHECK(c->sim[0].pcombat[2].inv.health == 100);
    CHECK(c->sim[0].pcombat[2].weapon_id == 1);
    CHECK(c->sim[0].pcombat[2].shot_serial == 0);
    CHECK(c->sim[0].pcombat[2].kick[0] == 0);
    CHECK(c->sim[0].pcombat[2].self.last_attacker == Q2_MP_NOT_A_PLAYER);
    CHECK(c->sim[0].pcombat[2].self.effect[2] == 0);
    CHECK(c->sim[0].player[2].prev_health == 100);
    CHECK(c->sim[0].combat.inv.health == 3 && c->sim[0].combat.weapon_id == 11);
    CHECK(c->sim[0].ent_world.player[2].pos[0] == 9000);
    CHECK(c->sim[0].ent_world.player[2].inv == &c->sim[0].pcombat[2].inv);
    release_fixture(c);
}

static void test_fast_frames(void)
{
    const int rates[] = {60, 144, 240};
    int rate;
    for (rate = 0; rate < 3; rate++) {
        client *c = fixture(4);
        q2_input neutral = {0};
        int frame, pi, jumps[4] = {0};
        for (pi = 1; pi < 4; pi++) c->gamepads.id[pi] = (SDL_JoystickID)pi;
        for (frame = 0; frame < 980; frame++) {
            double dt = 1.0 / rates[rate];
            s32 step = q2_sim_next_dt(&c->sim[0], dt);
            for (pi = 1; pi < 4; pi++) {
                q2_input in;
                c->gamepads.held[pi] = frame < 960 && (frame + pi * 7) % 48 == 0
                                          ? Q2_PAD_L2 : 0;
                client_extra_input(c, pi, step, false, &in);
                if (step && (in.buttons & Q2_BTN_JUMP)) jumps[pi]++;
            }
            q2_sim_advance(&c->sim[0], &neutral, dt);
        }
        for (pi = 1; pi < 4; pi++) CHECK(jumps[pi] == 20);
        release_fixture(c);
    }
}

static void test_owned_respawn_input(void)
{
    client *c = fixture(4);
    int frame;
    q2_player_die(&c->death[2], Q2_MP_NOT_A_PLAYER, 0, 2, true, false, NULL);
    q2_player_death_anim_ended(&c->death[2]);
    q2_player_death_tick(&c->death[2], -5, 12, true, 0);
    CHECK(c->death[2].stage == Q2_PDEATH_DOWN);
    c->sim[0].pcombat[2].inv.health = -5;
    c->sim[0].player[2].ent2_flags |= Q2_ENT2_DEAD;
    c->shoot = true;
    for (frame = 0; frame < 3; frame++) {
        client_input_simulated(c, 0.05f);
        c->frame_index++;
    }
    CHECK(c->death[2].stage == Q2_PDEATH_DOWN);
    CHECK(c->sim[0].pcombat[2].inv.health <= 0);
    c->shoot = false;
    c->gamepads.id[2] = 102;
    buttons(c, 2, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    client_input_simulated(c, 0.05f);
    CHECK(c->death[2].stage == Q2_PDEATH_ALIVE);
    CHECK(c->sim[0].pcombat[2].inv.health == 100);
    CHECK(c->sim[0].pcombat[2].weapon_id == 1);
    CHECK(c->sim[0].player[2].pos[0] == 9000);
    CHECK(c->death[1].stage == Q2_PDEATH_ALIVE);
    release_fixture(c);
}

static void test_virtual_controllers(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID ids[4];
    SDL_Joystick *joystick[4];
    q2_gamepads pads;
    SDL_Event event;
    int i;
    memset(&pads, 0, sizeof(pads));
    pads.first_slot = 1;
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "Q2 split regression virtual controller";
    for (i = 0; i < 4; i++) {
        ids[i] = SDL_AttachVirtualJoystick(&desc);
        CHECK(ids[i] != 0);
        joystick[i] = SDL_OpenJoystick(ids[i]);
        CHECK(joystick[i] != NULL);
        memset(&event, 0, sizeof(event));
        event.type = SDL_EVENT_GAMEPAD_ADDED;
        event.gdevice.which = ids[i];
        CHECK(q2_gamepads_event(&pads, &event) == (i < 3 ? i + 1 : -1));
    }
    CHECK(pads.id[0] == 0 && pads.id[1] == ids[0] && pads.id[3] == ids[2]);
    /* SDL generates real mapped controller events from these virtual devices.
     * Only gamepad events naming our virtual IDs are read; there is no window,
     * keyboard/mouse polling, or physical input injection in this test. */
    CHECK(SDL_SetJoystickVirtualAxis(joystick[1], SDL_GAMEPAD_AXIS_RIGHTX, 32767));
    CHECK(SDL_SetJoystickVirtualAxis(joystick[1], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    CHECK(SDL_SetJoystickVirtualButton(joystick[1], SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true));
    SDL_UpdateJoysticks();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                          SDL_EVENT_GAMEPAD_AXIS_MOTION,
                          SDL_EVENT_GAMEPAD_UPDATE_COMPLETE) > 0) {
        SDL_JoystickID id = event.gdevice.which;
        for (i = 0; i < 4; i++) if (id == ids[i]) break;
        if (i < 4) (void)q2_gamepads_event(&pads, &event);
    }
    {
        q2_pad_state pad = {0};
        u32 raw = q2_gamepads_read(&pads, 2, Q2_PAD_STYLE_RIGHT_STICK, &pad);
        CHECK((raw & (Q2_PAD_L1 | Q2_PAD_L2)) == (Q2_PAD_L1 | Q2_PAD_L2));
        CHECK(pad.rx == 127);
        CHECK(pads.held[1] == 0 && pads.held[3] == 0);
    }
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_GAMEPAD_REMOVED; event.gdevice.which = ids[1];
    CHECK(q2_gamepads_event(&pads, &event) == 2);
    CHECK(pads.id[3] == ids[2] && !pads.id[2]);
    event.type = SDL_EVENT_GAMEPAD_ADDED; event.gdevice.which = ids[3];
    CHECK(q2_gamepads_event(&pads, &event) == 2);
    q2_gamepads_close(&pads);
    pads.first_slot = 0;
    for (i = 0; i < 4; i++) {
        event.type = SDL_EVENT_GAMEPAD_ADDED; event.gdevice.which = ids[i];
        CHECK(q2_gamepads_event(&pads, &event) == i);
    }
    q2_gamepads_close(&pads);
    for (i = 0; i < 4; i++) {
        SDL_CloseJoystick(joystick[i]);
        CHECK(SDL_DetachVirtualJoystick(ids[i]));
    }
    SDL_Quit();
}

static void test_damage(void)
{
    client *c = fixture(2);
    q2_sim *sim = &c->sim[0];
    q2_input neutral = {0};
    int pi, step;
    /* Each player is the victim once, including player 0 while parked and
     * while live during the shared projectile sweep. */
    for (pi = 0; pi < 2; pi++) {
        q2_fire_result_v2 shot;
        q2_inventory inv;
        int victim = 1 - pi;
        q2_inventory_init(&inv);
        inv.health = 100; inv.armour = 100; inv.armour_class = 2;
        inv.flags |= Q2_INV_POWER_SHIELD;
        inv.ammo[Q2_AMMO_CELLS] = 100;
        q2_sim_player_loadout(sim, victim, &inv, 1);
        sim->player[pi].pos[0] = 0;
        sim->player[pi].pos[2] = 0;
        sim->player[victim].pos[0] = 0;
        sim->player[victim].pos[2] = 1300;
        q2_sim_select_player(sim, pi);
        sim->combat.inv.ammo[Q2_AMMO_SLUGS] = 10;
        sim->combat.weapon_id = Q2_WID_RAILGUN;
        sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_RAILGUN];
        client_targets_for(c, pi);
        shot = q2_sim_fire(sim);
        CHECK(shot.fired);
        CHECK(sim->pcombat[victim].inv.health < 100);
        CHECK(sim->pcombat[victim].inv.armour < 100);
        CHECK(sim->pcombat[victim].inv.ammo[Q2_AMMO_CELLS] < 100);
        CHECK(sim->player[victim].impulse_armed);
        CHECK(sim->player[victim].impulse[2] > 0);
        CHECK(!sim->pcombat[victim].self.knocked);
        inv = sim->pcombat[victim].inv;
        q2_sim_select_player(sim, victim);
        CHECK(sim->combat.inv.armour == inv.armour);
        CHECK(sim->combat.inv.ammo[Q2_AMMO_CELLS] == inv.ammo[Q2_AMMO_CELLS]);
    }
    q2_sim_select_player(sim, 1);
    sim->combat.weapon_id = 1;
    sim->combat.next_fire = 0;
    sim->pcombat[0].inv.health = 100;
    sim->pcombat[0].inv.armour = 0;
    sim->pcombat[0].inv.flags = 0;
    client_targets_for(c, 1);
    CHECK(q2_sim_fire(sim).fired);
    q2_sim_select_player(sim, 0);
    client_targets_for(c, 0);
    for (step = 0; step < 20; step++) q2_sim_tick(sim, &neutral, 12);
    CHECK(sim->combat.inv.health < 100);
    CHECK(sim->combat.inv.health == sim->combat.self.health);
    /* A shield pickup gained between shots is respected by the next trace;
     * firing an energy weapon must not restore the cells it just consumed. */
    sim->combat.weapon_id = Q2_WID_HYPERBLASTER;
    sim->combat.inv.weapons |= (u16)q2_weapon_tables_builtin()->owned_bit[Q2_WID_HYPERBLASTER];
    sim->combat.inv.ammo[Q2_AMMO_CELLS] = 10;
    sim->combat.next_fire = 0;
    CHECK(q2_sim_fire(sim).fired);
    CHECK(sim->combat.inv.ammo[Q2_AMMO_CELLS] == 9);
    release_fixture(c);
}

static void test_rounds_and_cameras(void)
{
    client *c = fixture(4);
    int pi;
    c->mp.mode = Q2_MP_VERSUS;
    c->mp.round_limit = 2;
    for (pi = 1; pi < 4; pi++) c->sim[0].pcombat[pi].inv.health = 0;
    client_score_deaths(c);
    CHECK(c->mp.end == Q2_MP_END_ROUND_OVER && c->mp.team_frags[0] == 1);
    q2_mp_round_start(&c->mp);
    CHECK(c->mp.end == Q2_MP_RUNNING && c->mp.team_frags[0] == 1);
    client_score_deaths(c);
    CHECK(c->mp.end == Q2_MP_END_MATCH_OVER && c->mp.team_frags[0] == 2);
    c->sim[0].player[2].pitch = 123; c->sim[0].player[2].yaw = 456;
    client_extra_camera(c, 2);
    c->sim[0].player[2].ent2_flags |= Q2_ENT2_DEAD;
    c->sim[0].player[2].pitch = 321; c->sim[0].player[2].yaw = 654;
    for (pi = 0; pi < 200; pi++) client_extra_camera(c, 2);
    CHECK(c->mp_camera_angles[2][0] == 123 && c->mp_camera_angles[2][1] == 456);
    CHECK(c->mp_camera_angles[2][2] == (-384 & 4095));
    CHECK(c->mp_camera_angles[1][2] == 0);
    c->mp.player_count = 2;
    c->gamepads.id[1] = 101;
    CHECK(!client_mp_results_input(c, 1.0f / 30.0f));
    buttons(c, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    CHECK(!client_mp_results_input(c, 1.0f / 30.0f));
    CHECK(c->mp_results.ready == 2);
    CHECK(!q2_mp_results_tick(&c->mp_results, 2, 0, 3000));
    CHECK(c->mp_results.countdown == 150);
    CHECK(!q2_mp_results_tick(&c->mp_results, 2, 1, 150));
    CHECK(q2_mp_results_tick(&c->mp_results, 2, 0, 1));
    release_fixture(c);
}

static void test_teleports_lighting_and_bodies(void)
{
    client *c = fixture(4);
    q2_sim *sim = &c->sim[0];
    q2_start_pos sp = {0};
    s32 original[3];
    const q2_pdeath_stage retired[] = {
        Q2_PDEATH_DISSOLVING, Q2_PDEATH_FADING, Q2_PDEATH_GIBBED, Q2_PDEATH_GONE
    };
    int pi, stage;
    memcpy(original, sim->player[0].pos, sizeof(original));
    c->cam.yaw = 123;
    for (pi = 0; pi < 4; pi++) sim->player[pi].ent.node = pi + 11;
    for (pi = 1; pi < 4; pi++) {
        q2_sim_select_player(sim, pi);
        CHECK(sim->current_node == pi + 11);
        CHECK(client_light_node(c, 0) == 11);
        CHECK(client_light_node(c, pi) == pi + 11);
        sp.x = 7000 + pi * 1000; sp.y = 300 + pi * 100; sp.z = pi * 900;
        sp.angle = (s16)(pi * 512);
        client_queue_teleport(c, &sp);
        sim->combat.inv.health = (s16)(70 + pi);
    }
    q2_sim_select_player(sim, 0);
    CHECK(sim->current_node == 11);
    CHECK(client_apply_teleports(c));
    CHECK(memcmp(original, sim->player[0].pos, sizeof(original)) == 0);
    CHECK(c->cam.yaw == 123 && sim->cur_player == 0 && sim->current_node == 11);
    for (pi = 1; pi < 4; pi++) {
        CHECK(sim->player[pi].pos[0] == 7000 + pi * 1000);
        CHECK(sim->player[pi].pos[2] == pi * 900);
        CHECK(sim->ent_world.player[pi].pos[0] == sim->player[pi].pos[0]);
        CHECK(sim->pcombat[pi].self.origin[1] == q2_sim_origin_y(sim->player[pi].pos[1]));
        CHECK(sim->pcombat[pi].inv.health == 70 + pi);
        CHECK(c->mp_camera_angles[pi][1] == pi * 512);
        CHECK(!c->pending_teleport_have[pi]);
    }
    /* Player one's teleport updates its actual eye, without moving the others. */
    sp.x = 1234; sp.angle = 2048;
    client_queue_teleport(c, &sp);
    CHECK(client_apply_teleports(c));
    CHECK(c->cam.pos[0] == 1234 && c->cam.yaw == 2048);
    CHECK(c->cam.pos[1] == sp.y - Q2_VIEW_STAND);
    CHECK(sim->player[1].pos[0] == 8000);

    CHECK(client_targets_for(c, 0) == 3);
    c->death[1].stage = Q2_PDEATH_DYING;
    c->death[2].stage = Q2_PDEATH_DOWN;
    CHECK(client_targets_for(c, 0) == 3); /* Bodies can still be gibbed. */
    for (stage = 0; stage < 4; stage++) {
        c->death[1].stage = retired[stage];
        CHECK(client_targets_for(c, 0) == 2);
        CHECK(sim->world_target_count == 3);
        CHECK(sim->pcombat[1].self.takedamage == Q2_DAMAGE_NO);
    }
    CHECK(client_mp_respawn(c, 1));
    CHECK(client_targets_for(c, 0) == 3);
    CHECK(sim->pcombat[1].self.takedamage == Q2_DAMAGE_AIM);
    release_fixture(c);
}

/*
 * The GAME VARIABLES rows that have to survive the trip out of the menu.
 *
 * WEAPON STAY is 0x800B3360, a halfword of its own rather than a bit of the
 * cheat word at 0x800B29EC (gamevars.h), and the row wrote nothing on the far
 * end: `q2_entity_world.weapons_stay` had two readers and no writer, so the
 * weapons-stay branches at 0x80037E60 and 0x8005988C were dead code.
 */
static void test_game_variables(void)
{
    client *c = fixture(1);
    q2_sim *sim = &c->sim[0];

    c->menu.multiplayer = true;
    c->settings.v[Q2_SET_WEAPON_STAY] = 1;
    client_apply_settings(c);
    CHECK(sim->weapons_stay);

    c->settings.v[Q2_SET_WEAPON_STAY] = 0;
    client_apply_settings(c);
    CHECK(!sim->weapons_stay);

    /*
     * Single player too: 0x8001C698 zeroes the cheat word on its disabled arm
     * and never touches 0x800B3360, so this setting is not gated on the
     * session type here either. Both readers test deathmatch themselves.
     */
    c->menu.multiplayer = false;
    c->settings.v[Q2_SET_WEAPON_STAY] = 1;
    client_apply_settings(c);
    CHECK(sim->weapons_stay);
    CHECK(sim->cheats == 0);

    release_fixture(c);
}

int main(void)
{
    test_inputs();
    test_game_variables();
    test_fast_frames();
    test_owned_respawn_input();
    test_pickups_and_respawn();
    test_damage();
    test_rounds_and_cameras();
    test_teleports_lighting_and_bodies();
    test_virtual_controllers();
    printf("%d split client checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
