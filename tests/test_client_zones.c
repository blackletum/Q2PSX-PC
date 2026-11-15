/* Zone seams exercised through the actual loader, without a window or input. */
int q2_test_zones_client_main(int argc, char **argv);
#define main q2_test_zones_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} } while (0)
#include "client_fixture.h"

static void test_population_history(void)
{
    q2_sim *sim = calloc(1, sizeof(*sim));
    q2_zone_history history = {0}, copy = {0};
    q2_common_file common = {0};
    q2_creature_world *world = calloc(1, sizeof(*world));
    q2_monster monsters[2];
    dat_chunk chunk = {0};
    u8 data[72] = {0};
    u32 i;
    if (!sim || !world) exit(2);
    memcpy(data, "Zone0Key", 8);
    q2_wr_u32(data + 16, 28);
    q2_wr_u16(data + 42, 27);
    q2_wr_u32(data + 44, Q2_POP_TERM_FFFF);
    chunk.data = data; chunk.size = sizeof(data);
    common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 1);
    q2_entity_remove(&sim->entities.ent[0]);
    CHECK(q2_entity_alloc(&sim->entities) == &sim->entities.ent[0]); /* reused slot */
    world->pop = sim->item_population;
    world->pop_ready = world->ready = true;
    world->set.monsters = monsters; world->set.count = 2;
    for (i = 0; i < 2; i++) {
        q2_monster_init(&monsters[i]);
        monsters[i].population_slot = i + 7;
        monsters[i].population_active = true;
        monsters[i].pop_class_id = 18;
        monsters[i].in_use = i == 1;
        monsters[i].dead = i == 0;
        monsters[i].health = i == 0 ? -100 : 13;
        monsters[i].pos[0] = 345;
        monsters[i].angles[2] = 1234;
        monsters[i].spawnflags = Q2_SPAWNFLAG_AMBUSH | Q2_SVFLAG_INUSE | 0x5u;
        monsters[i].target = 9;
    }
    CHECK(q2_zone_history_capture(&history, "TEST", 0, sim, world, NULL, NULL, 0) == Q2_OK);
    CHECK(history.count == 1 && history.map[0].count == 3);
    CHECK(q2_zone_history_copy(&copy, &history) == Q2_OK);
    CHECK(copy.map[0].object != history.map[0].object);
    q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&history, "test", 0, sim, NULL);
    CHECK(sim->item_group_run[0] && sim->entities.count == 1 && !sim->entities.ent[0].in_use);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 0);
    for (i = 0; i < 2; i++) {
        q2_monster_init(&monsters[i]);
        monsters[i].population_slot = 8 - i; /* changed module/roster order */
        monsters[i].pop_class_id = 18;
        monsters[i].spawn_pending = true;
    }
    q2_zone_history_creatures(&history, "TEST", 0, sim, world);
    /* GeneralRespawnStuffStore's 12-byte form (0x8007D318): health 13 is
     * stored as 13 >> 2 and comes back as 12; the yaw loses its low six bits
     * (1234 -> 1216); of the edict's spawn flags only bits 18..26 are kept. */
    CHECK(monsters[0].in_use && monsters[0].health == 12 && monsters[0].pos[0] == 345);
    CHECK(monsters[0].angles[2] == 1216 && monsters[0].ideal_yaw == 1216);
    CHECK(monsters[0].spawnflags == Q2_SPAWNFLAG_AMBUSH);
    CHECK(monsters[0].target == 9 && !monsters[0].spawn_pending);
    CHECK(!monsters[0].spawn_initialized && !monsters[0].resume_pending);
    CHECK(!monsters[1].in_use && monsters[1].dead && !monsters[1].spawn_pending);
    /* Object 0 is the item place; 1 the body, whose word's bit 18 is record
     * bit 0; 2 the living record, nine bits plus whatever 0x600 the record has. */
    CHECK(history.map[0].object[1].kind == Q2_ZONE_CREATURE &&
          history.map[0].object[1].spawnflags == 0x1u);
    CHECK((history.map[0].object[2].spawnflags & 0x1FFu) == 0x1u);
    /* The tail of GeneralRespawnStuff (0x8007D304): the record's health over
     * whatever SpawnGeneral and export 0 left, once the spawn has run. */
    monsters[0].spawn_initialized = true; monsters[0].health = 20;
    q2_zone_history_spawned(&history, "TEST", sim, world);
    CHECK(monsters[0].health == 12);
    CHECK(q2_creature_world_summon(world, "Zone0Key") == 0);
    q2_zone_history_creatures(&history, "TEST", 1, sim, world);
    CHECK(!monsters[0].in_use && !monsters[0].population_active);
    CHECK(!monsters[1].dead && !monsters[1].population_active);
    CHECK(q2_zone_history_capture(&history, "TEST", 1, sim, world, NULL, NULL, 0) == Q2_OK);
    q2_zone_history_creatures(&history, "TEST", 0, sim, world);
    CHECK(monsters[0].health == 12 && monsters[1].dead);
    data[8] = 'x'; /* different Population must never inherit old objects */
    q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&history, "TEST", 0, sim, NULL);
    CHECK(!sim->item_group_run[0]);
    CHECK(q2_zone_history_valid(&history));
    free(history.map[0].object[1].runtime);
    history.map[0].object[1] = history.map[0].object[0];
    CHECK(!q2_zone_history_valid(&history));
    CHECK(q2_zone_history_copy(&copy, &history) == Q2_ERR_INVALID_ARG);
    CHECK(copy.count == 1 && copy.map[0].object[1].kind == Q2_ZONE_CREATURE);
    q2_zone_history_free(&history); q2_zone_history_free(&copy);
    q2_sim_free(sim); free(sim); free(world);
}

static bool gate(client *c, int zone)
{
    c->carry_player = c->carry_same_map = true;
    return client_load_zone(c, c->map, zone);
}

static void test_ambush_return(const char *disc_path)
{
    client *c = open_client(disc_path);
    u32 i, group = UINT32_MAX, slot = 0;
    s32 pos[3] = {0};
    s16 health = 0;
    CHECK(client_load_zone(c, "BASE1", 1));
    CHECK(q2_creature_world_summon(&c->creatures, "LiftRoom") > 0);
    q2_sim_activate_item_group(&c->sim[0], "LiftRoom");
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (!m->in_use || !q2_pop_get_group(&c->creatures.pop, m->group, &g) ||
            strcmp(g.name, "LiftRoom")) continue;
        group = m->group; slot = m->population_slot;
        q2_monster_damage(m, 1);
        /* The record reads the actor's origin (+0x54), which is the motion's. */
        m->pos[0] += 3; m->motion.ent.pos[0] += 3; m->motion.target[0] += 3;
        health = m->health;
        memcpy(pos, q2_cre_origin(m), sizeof(pos));
        break;
    }
    CHECK(group != UINT32_MAX);
    CHECK(gate(c, 0));
    CHECK(gate(c, 1));
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group != group || m->population_slot != slot) continue;
        /* GeneralRespawnStuffStore keeps health >> 2 (toward zero), so the
         * wound comes back rounded to a multiple of four; the position goes
         * through SpawnGeneral's box drop and lands on the same floor. */
        printf("ambush return: health %d -> %d, pos %d %d %d -> %d %d %d\n", health,
               m->health, pos[0], pos[1], pos[2], m->pos[0], m->pos[1], m->pos[2]);
        CHECK(m->in_use && !m->spawn_pending && m->health == (health & ~3));
        CHECK(!memcmp(pos, m->pos, sizeof(pos)));
        /* A fresh spawn: the go runs a frame after it (monster_start). */
        CHECK(m->think == q2_monster_go_think && m->spawn_initialized);
        break;
    }
    CHECK(i < c->creatures.set.count);
    CHECK(q2_creature_world_summon(&c->creatures, "LiftRoom") == 0);
    close_client(c);
}

static void capture(client *c, const char *dir, const char *name, const q2_camera *camera)
{
    char path[1024];
    if (!dir) return;
    CHECK(c->headless);
    c->sim_enabled = false;
    c->cam = *camera;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    printf("capture %s: loading %d, views %d, quads %u, camera %d %d %d\n",
           c->map, c->loading.open, c->screen.view_count, c->shot_stats.quads_emitted,
           c->cam.pos[0], c->cam.pos[1], c->cam.pos[2]);
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

/* One creature, framed from a little above and in front of it, as the captures
 * for a gate round trip want it: the pose is the subject, not the room. */
static void capture_creature(client *c, const char *dir, const char *name, u32 index)
{
    const q2_monster *m = &c->creatures.set.monsters[index];
    q2_camera camera = c->cam;
    const s32 *at = q2_cre_origin(m);
    /* Four stands around the creature; the first inside the hull is used. */
    static const s32 side[4][3] = {{0, 0, 700}, {0, 0, -700}, {700, 0, 0}, {-700, 0, 0}};
    static const s32 yaw[4] = {2048, 0, 1024, 3072};
    u32 k;
    if (!dir) return;
    for (k = 0; k < 4; k++) {
        camera.pos[0] = at[0] + side[k][0]; camera.pos[1] = at[1] - 450;
        camera.pos[2] = at[2] + side[k][2];
        camera.yaw = (s16)yaw[k]; camera.pitch = -200; camera.roll = 0;
        if (!c->sim[0].coll_primary_ready ||
            q2_coll_find_node(&c->sim[0].coll_primary, camera.pos, -1, true) >= 0) break;
    }
    capture(c, dir, name, &camera);
}

/* Put the actor's animation cursor where a live tick would have it for the AI
 * frame the test chose, so a capture shows that frame rather than the seed. */
static void pose_at_frame(client *c, u32 index)
{
    q2_monster *m = &c->creatures.set.monsters[index];
    const q2_cre_frame_name *rec;
    u32 pos = 0;
    if (!c->cre_model_ok || !c->cre_model_ok[index]) return;
    rec = q2_creature_world_frame_name(&c->creatures, m, m->frame);
    if (!rec || !q2_model_position_for_move(&c->cre_model[index], rec->name, m->frame,
                                            rec->first, &pos)) return;
    m->motion.anim_position = m->motion.anim_previous = (s16)pos;
    m->motion.anim_base = (u16)pos & 0x7FFFu;
    m->motion.anim_flags = 0;
    m->motion.animation_ready = true;
}

static void test_return(const char *disc_path, const char *dir)
{
    client *c = open_client(disc_path);
    u32 i, removed = 0, killed = 0, gi;
    q2_pop_group group;
    q2_camera camera;
    CHECK(client_load_zone(c, "JAIL4", 3));
    for (gi = 0; gi < c->sim[0].item_population.group_count; gi++) {
        if (q2_pop_get_group(&c->sim[0].item_population, gi, &group) &&
            !strcmp(group.name, "Zone3")) break;
    }
    CHECK(gi < c->sim[0].item_population.group_count);
    camera = c->cam;
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && e->population_group == (s32)gi && e->effect) {
            camera.pos[0] = e->pos[0]; camera.pos[1] = e->pos[1] - 300;
            camera.pos[2] = e->pos[2] + 1200;
            camera.yaw = 2048; camera.pitch = -160; camera.roll = 0;
            if (q2_coll_find_node(&c->sim[0].coll_primary, camera.pos, -1, true) >= 0) break;
        }
    }
    CHECK(i < c->sim[0].entities.count);
    capture(c, dir, "before-collection", &camera);
    if (i < c->sim[0].entities.count) {
        q2_entity *e = &c->sim[0].entities.ent[i];
        c->sim[0].combat.inv.health = 1;
        q2_entity_world_move_player(&c->sim[0].ent_world, 0, e->pos);
        q2_item_think(e, &c->sim[0].ent_world);
        CHECK(!e->in_use); /* exercise the actual pickup/removal path */
        if (!e->in_use) removed++;
    }
    for (i = 0; i < c->sim[0].entities.count; i++) {
        q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->in_use && e->population_group == (s32)gi && e->effect) {
            q2_entity_remove(e);
            removed++;
        }
    }
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->in_use && m->group == gi) {
            q2_monster_damage(m, 30000);
            killed++;
        }
    }
    CHECK(removed > 0);
    CHECK(killed > 0);
    c->sim[0].level_time = 1234;
    c->sim[0].tick_count = 321;
    c->sim[0].dt_accum = 7;
    c->sim[0].dt_frac = 0.375;
    CHECK(gate(c, 0));
    CHECK(c->sim[0].level_time == 1234 && c->sim[0].tick_count == 321);
    CHECK(c->sim[0].dt_accum == 7 && c->sim[0].dt_frac == 0.375);
    /* Save in another zone, reopen it in a fresh client, and return. */
    if (dir) {
        char path[1024];
        q2_save saved = {0};
        client *cold;
        CHECK(client_capture(c));
        snprintf(path, sizeof(path), "%s/zone-history.q2s", dir);
        CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
        CHECK(q2_save_read(&saved, path) == Q2_OK);
        CHECK(saved.zone_history_present && saved.zone_history.count == 1);
        close_client(c);
        cold = open_client(disc_path);
        CHECK(client_apply_save(cold, &saved));
        q2_save_free(&saved);
        c = cold;
    }
    CHECK(gate(c, 3));
    for (i = 0; i < c->sim[0].entities.count; i++) {
        const q2_entity *e = &c->sim[0].entities.ent[i];
        if (e->population_group == (s32)gi && e->effect) CHECK(!e->in_use);
    }
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group == gi && m->in_use) CHECK(m->dead);
    }
    capture(c, dir, "after-return", &camera);
    /* A failed load must not clear mission/screen state or consume history. */
    {
        u32 serial = c->level_change_serial, loads = c->loads_done;
        u32 history_count = c->zone_history.count;
        q2_player player = c->sim[0].player[0];
        c->running = true;
        c->mission_row = 2;
        c->level_frames_drawn = true;
        c->loading_raises = 4;
        CHECK(!client_load_zone(c, "MISSING", 0));
        CHECK(c->zone_index == 3 && !strcmp(c->map, "JAIL4"));
        CHECK(c->mission_row == 2 && c->level_frames_drawn);
        CHECK(c->level_change_serial == serial && c->loads_done == loads);
        CHECK(c->loading_raises == 4 && !c->loading.open);
        CHECK(c->zone_history.count == history_count);
        CHECK(!memcmp(&player, &c->sim[0].player[0], sizeof(player)));
        CHECK(!gate(c, 99));
        CHECK(!c->carry_player && !c->carry_same_map && !c->carry_pos_valid);
    }
    /* A genuine level trip also keeps the visited map's population. */
    CHECK(client_load_zone(c, "BASE0", 0));
    CHECK(client_load_zone(c, "JAIL4", 3));
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].group == gi)
            CHECK(!c->creatures.set.monsters[i].in_use || c->creatures.set.monsters[i].dead);
    snprintf(c->first_map, sizeof(c->first_map), "%s", "JAIL4");
    client_start_game(c);
    CHECK(!c->zone_history.count && !c->event_store.count);
    CHECK(gate(c, 3));
    {
        u32 alive = 0;
        for (i = 0; i < c->creatures.set.count; i++)
            if (c->creatures.set.monsters[i].group == gi &&
                c->creatures.set.monsters[i].in_use && !c->creatures.set.monsters[i].dead) alive++;
        CHECK(alive == killed);
    }
    close_client(c);
}

static int drop_rand(void) { return 12345; }

static void test_deferred_drops(void)
{
    q2_sim *sim = calloc(1, sizeof(*sim));
    q2_zone_history h = {0};
    q2_common_file common = {0};
    dat_chunk chunk = {0};
    u8 data[72] = {0};
    u32 i, live = 0;
    if (!sim) exit(2);
    memcpy(data, "Zone0Key", 8); q2_wr_u32(data + 16, 28);
    q2_wr_u16(data + 42, 27); q2_wr_u32(data + 44, Q2_POP_TERM_FFFF);
    chunk.data = data; chunk.size = sizeof(data); common.chunk[Q2_COMMON_POPULATION] = &chunk;
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    h.map[0].count = h.map[0].capacity = h.map[0].next_serial = Q2_ITEM_TOSS_MAX + 1;
    h.map[0].object = calloc(h.map[0].count, sizeof(*h.map[0].object));
    if (!h.map[0].object) exit(2);
    for (i = 0; i < h.map[0].count; i++) {
        q2_zone_object *o = &h.map[0].object[i];
        q2_zone_runtime *r = o->runtime = calloc(1, sizeof(*r));
        if (!r) exit(2);
        o->kind = Q2_ZONE_DYNAMIC; o->slot = i + 1; o->id = 27;
        q2_entity_init(&r->entity.state);
        r->entity.state.in_use = true; r->entity.state.kind = Q2_ENT_KIND_ITEM;
        r->entity.state.place_id = 27; r->entity.state.zone_serial = o->slot;
        r->entity.think = Q2_ZTHINK_DROP; r->entity.source_slot = UINT32_MAX;
        r->entity.velocity[1] = -2000;
    }
    CHECK(q2_zone_history_valid(&h));
    q2_zone_history_items(&h, "TEST", 0, sim, NULL);
    CHECK(q2_item_drop_in_flight() == Q2_ITEM_TOSS_MAX);
    CHECK(!h.map[0].object[Q2_ITEM_TOSS_MAX].runtime->materialized);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    /* The console parks nothing: a dynamic record is rebuilt from the live
     * world at every capture, so the one capacity deferred is forgotten --
     * on the console it would have been a live slot, never a record. */
    CHECK(h.map[0].count == Q2_ITEM_TOSS_MAX);
    q2_entity_remove(&sim->entities.ent[0]);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    CHECK(h.map[0].count == Q2_ITEM_TOSS_MAX - 1);
    q2_item_drop_reset(); q2_sim_free(sim); q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&h, "TEST", 0, sim, NULL);
    for (i = 0; i < sim->entities.count; i++)
        if (sim->entities.ent[i].in_use && sim->entities.ent[i].zone_serial == Q2_ITEM_TOSS_MAX + 1) live++;
    CHECK(live == 0 && q2_item_drop_in_flight() == Q2_ITEM_TOSS_MAX - 1);
    q2_zone_history_free(&h); q2_item_drop_reset(); q2_sim_free(sim);
    q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    CHECK(q2_sim_activate_item_group(sim, "Zone0Key") == 1);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    h.map[0].object[0].runtime->entity.model_bound = true;
    q2_sim_free(sim); q2_sim_init(sim, NULL, 50);
    CHECK(q2_sim_attach_items(sim, &common, 0, NULL, NULL) == Q2_OK);
    q2_zone_history_items(&h, "TEST", 0, sim, NULL); /* missing destination CastList */
    CHECK(!sim->entities.ent[0].in_use && !h.map[0].object[0].runtime->materialized);
    CHECK(q2_zone_history_capture(&h, "TEST", 0, sim, NULL, NULL, NULL, 0) == Q2_OK);
    CHECK(!h.map[0].object[0].removed && h.map[0].object[0].runtime != NULL);
    q2_zone_history_free(&h); q2_sim_free(sim); free(sim);
}

static q2_entity *dynamic_entity(client *c, u32 serial)
{
    u32 i;
    for (i = 0; i < c->sim[0].entities.count; i++)
        if (c->sim[0].entities.ent[i].in_use && c->sim[0].entities.ent[i].zone_serial == serial)
            return &c->sim[0].entities.ent[i];
    return NULL;
}

static q2_projectile *dynamic_projectile(client *c, u32 serial)
{
    u32 i;
    for (i = 0; i < Q2_PROJ_MAX; i++)
        if (c->sim[0].combat.projectiles.p[i].in_use && c->sim[0].combat.projectiles.p[i].zone_serial == serial)
            return &c->sim[0].combat.projectiles.p[i];
    return NULL;
}

static q2_monster *authored_creature(client *c, u32 group, u32 slot, u32 *index)
{
    u32 i;
    for (i = 0; i < c->creatures.set.count; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        if (m->group == group && m->population_slot == slot) {
            if (index) *index = i;
            return m;
        }
    }
    return NULL;
}

static client *cold_save(client *c, const char *disc, const char *dir, const char *name)
{
    q2_save saved = {0};
    char path[1024];
    if (!dir) return c;
    CHECK(client_capture(c));
    snprintf(path, sizeof(path), "%s/%s.q2s", dir, name);
    CHECK(q2_save_write(&c->snapshot, path) == Q2_OK);
    CHECK(q2_save_read(&saved, path) == Q2_OK);
    close_client(c);
    c = open_client(disc);
    CHECK(client_apply_save(c, &saved));
    q2_save_free(&saved);
    return c;
}

static void census_death_clips(client *c);

static void test_overlap(const char *disc)
{
    client *c = open_client(disc);
    q2_world_zone destination = {0};
    q2_collision hull;
    s32 pos[3] = {0};
    bool found = false;
    u32 i, j, serial, shot_serial, shooter = 0, shooter_group, shooter_slot;
    const q2_model_bank *banks[2];
    q2_entity *e;
    CHECK(client_load_zone(c, "BASE1", 0));
    census_death_clips(c);
    CHECK(q2_world_load_zone(&destination, c->disc, "BASE1", 1) == Q2_OK);
    CHECK(q2_collision_parse(&hull, &destination.zone, Q2_COLL_PRIMARY) == Q2_OK);
    for (i = 0; i < c->sim[0].triggers.count; i++) {
        q2_trigger t;
        if (!q2_trigger_get(&c->sim[0].triggers, i, &t)) continue;
        for (j = 0; j < 3; j++) pos[j] = (t.min[j] + t.max[j]) / 2;
        if (q2_coll_find_node(&c->sim[0].coll_primary, pos, -1, true) >= 0 &&
            q2_coll_find_node(&hull, pos, -1, true) >= 0) { found = true; break; }
    }
    CHECK(found);
    if (!found) { q2_world_free_zone(&destination); close_client(c); return; }
    for (i = 0; i < c->creatures.set.count; i++)
        if (c->creatures.set.monsters[i].in_use &&
            q2_coll_find_node(&hull, c->creatures.set.monsters[i].pos, -1, true) < 0) { shooter = i; break; }
    CHECK(i < c->creatures.set.count);
    shooter_group = c->creatures.set.monsters[shooter].group;
    shooter_slot = c->creatures.set.monsters[shooter].population_slot;
    q2_world_free_zone(&destination);
    banks[0] = c->sim[0].item_bank; banks[1] = &c->zone_bank;
    e = q2_item_drop_spawn(&c->sim[0].entities, c->sim[0].item_table, banks, 2, 27, 0, pos, -1, drop_rand);
    CHECK(e != NULL);
    if (!e) { close_client(c); return; }
    q2_projectiles_init(&c->sim[0].combat.projectiles);
    c->sim[0].combat.projectiles.p[0].in_use = true;
    c->sim[0].combat.projectiles.p[0].kind = Q2_PROJ_ROCKET;
    c->sim[0].combat.projectiles.p[0].owner = -1;
    c->sim[0].combat.projectiles.p[0].shooter = &c->cre_actor[shooter];
    memcpy(c->sim[0].combat.projectiles.p[0].pos, pos, sizeof(pos));
    c->sim[0].combat.projectiles.live = 1;
    CHECK(client_capture(c));
    serial = e->zone_serial; shot_serial = c->sim[0].combat.projectiles.p[0].zone_serial;
    /* A drop is a class-46 pickup with +0x44 bit 4 (0x80020B38); LoadStoreQuakePickup
     * (0x8005B500..0x8005B50C) refuses to store it and RememberMe kills the slot
     * (0x8007F910). The rocket is in the missile pool, which LoadZone's path never
     * resets (InitialiseMissiles 0x80048C54 runs only from InitEveryLevel), so it
     * flies on through the doorway with its shooter still bound. */
    CHECK(gate(c, 1));
    CHECK(dynamic_entity(c, serial) == NULL);
    for (j = 0; j < 4; j++) {
        if (j) CHECK(gate(c, (int)(1 - j % 2)));
        CHECK(dynamic_projectile(c, shot_serial) != NULL);
        CHECK(c->sim[0].combat.projectiles.live == 1);
        CHECK(authored_creature(c, shooter_group, shooter_slot, &shooter) != NULL);
        if (dynamic_projectile(c, shot_serial))
            CHECK(dynamic_projectile(c, shot_serial)->shooter == &c->cre_actor[shooter]);
    }
    CHECK(gate(c, 1));
    CHECK(q2_projectile_expire(&c->sim[0].combat.projectiles, 0));
    CHECK(gate(c, 0));
    CHECK(dynamic_entity(c, serial) == NULL && dynamic_projectile(c, shot_serial) == NULL);
    close_client(c);
}

/*
 * (iii) Where a FinishDying'd body comes to rest.
 *
 * QuakeStoreInanimate (0x80059B08) stores the CLIP the body's position is in:
 * FindAnimSequenceNumFromFrame (0x8006DA3C) walks the chain from model+0x34
 * subtracting each clip's length (0x8006DA70..0x8006DA7C, Relocate 0x80070188
 * as the step) until the position lands inside one. RespawnQuakeDeadBody poses
 * the returning body with LastFrameFromSequenceNum (0x8006DAB4): the lengths of
 * clips 0..n summed, minus one (0x8006DAC4 `s1 = -1`, 0x8006DAEC `addu`). The
 * port's body rests on its MOVE's last frame, which is the same pose only when
 * the frame the creature died on and the move's last frame land in one clip and
 * that last frame is the clip's final one -- or past it, where the position
 * walk holds the last pose.
 */
static void check_death_clip_frames(client *c, u32 index, const char *who,
                                    const q2_mmove *mv, s32 died_frame)
{
    q2_monster *m = &c->creatures.set.monsters[index];
    const q2_model *mdl = &c->cre_model[index];
    const q2_cre_frame_name *r0, *r1;
    u32 p0 = 0, p1 = 0, w0 = 0, w1 = 0;
    q2_model_anim c0, c1;
    bool h0 = false, h1 = false;
    if (!mv || !c->cre_model_ok || !c->cre_model_ok[index]) return;
    r0 = q2_creature_world_frame_name(&c->creatures, m, died_frame);
    r1 = q2_creature_world_frame_name(&c->creatures, m, mv->last_frame);
    CHECK(r0 && r1);
    if (!r0 || !r1) return;
    CHECK(q2_model_position_for_move(mdl, r0->name, died_frame, r0->first, &p0));
    CHECK(q2_model_position_for_move(mdl, r1->name, mv->last_frame, r1->first, &p1));
    CHECK(q2_model_anim_at_position_held(mdl, p0, &c0, &w0, &h0));
    CHECK(q2_model_anim_at_position_held(mdl, p1, &c1, &w1, &h1));
    printf("death clip %s '%s' %d..%d, died on %d: pos %u -> clip @%u (%u frames), "
           "rest pos %u -> clip @%u within %u%s\n", who, r1->name, mv->first_frame,
           mv->last_frame, died_frame, p0, c0.offset, c0.frames, p1, c1.offset, w1,
           h1 ? " (held)" : "");
    CHECK(c0.offset == c1.offset);
    /*
     * What the disc shows for every named death move: the clip is exactly
     * Q2_CRE_TICKS_PER_FRAME (3) animation frames per AI frame, so the move's
     * last frame sits at animation frame 3n-3 and LastFrameFromSequenceNum
     * answers the clip's last tick, in frame 3n-1. The console's returning
     * body is therefore two animation frames -- the tail of the last AI
     * frame's interpolation -- further into the clip than the body the player
     * walked away from, and client_cre_pose_returned_body puts it there
     * (check_death_clip). Pinned as the disc's arithmetic.
     */
    CHECK(!h1 && (u32)c1.frames == Q2_CRE_TICKS_PER_FRAME *
                               (u32)(mv->last_frame - mv->first_frame + 1));
    CHECK(!h1 && (u32)c1.frames - 1u - w1 / Q2_MODEL_TICKS_PER_FRAME ==
                 Q2_CRE_TICKS_PER_FRAME - 1u);
}

/* LastFrameFromSequenceNum (0x8006DAB4) for the clip a frame's position is in:
 * the clip lengths through it, in tenths, minus one -- the clip's last tick. */
static bool clip_end_tick(client *c, u32 index, s32 frame, u32 *out)
{
    const q2_monster *m = &c->creatures.set.monsters[index];
    const q2_model *mdl = &c->cre_model[index];
    const q2_cre_frame_name *rec = q2_creature_world_frame_name(&c->creatures, m, frame);
    q2_model_anim clip;
    u32 pos = 0, within = 0;
    bool held = false;
    if (!rec || !c->cre_model_ok || !c->cre_model_ok[index] ||
        !q2_model_position_for_move(mdl, rec->name, frame, rec->first, &pos) ||
        !q2_model_anim_at_position_held(mdl, pos, &clip, &within, &held)) return false;
    *out = q2_model_anim_base(mdl, &clip) + (u32)clip.frames * Q2_MODEL_TICKS_PER_FRAME - 1u;
    return true;
}

static void check_death_clip(client *c, u32 index, s32 died_frame)
{
    const q2_monster *m = &c->creatures.set.monsters[index];
    u32 end = 0;
    CHECK(m->currentmove && c->cre_model_ok && c->cre_model_ok[index]);
    check_death_clip_frames(c, index, "corpse", m->currentmove, died_frame);
    /* RespawnQuakeDeadBody, 0x8007D748..0x8007D75C: the body's +0x100 is the
     * clip's last tick, two animation frames and nine tenths past where the
     * move's last AI frame left the cursor (3n-3 frames, tick 30n-30). */
    CHECK(clip_end_tick(c, index, m->frame, &end));
    printf("corpse pose: frame %d, cursor %d, clip end %u\n", m->frame,
           m->motion.anim_position, end);
    CHECK(m->motion.animation_ready && m->motion.anim_position == (s16)end);
    CHECK(m->motion.anim_flags == 0);
}

/* Every named death move of every module resident in this zone, from its
 * first frame: the frame a creature killed a tick before the gate rests on. */
static void census_death_clips(client *c)
{
    u32 i, j, k, seen = 0;
    const struct q2_cre_bind *done[64];
    for (i = 0; i < c->creatures.set.count; i++) {
        const q2_monster *m = &c->creatures.set.monsters[i];
        const q2_creature_module *mod = NULL;
        if (!m->cre_bind || !c->cre_model_ok || !c->cre_model_ok[i]) continue;
        for (k = 0; k < seen; k++) if (done[k] == m->cre_bind) break;
        if (k < seen || seen == 64) continue;
        done[seen++] = m->cre_bind;
        for (j = 0; j < c->creatures.mod_count; j++)
            if (&c->creatures.mod[j].bind == m->cre_bind) mod = &c->creatures.mod[j];
        if (!mod) continue;
        for (k = 0; k < mod->cre.move_count && k < mod->bind.move_count; k++) {
            const q2_mmove *mv = &mod->bind.move[k];
            const q2_cre_frame_name *rec;
            if (mod->cre.move[k].clip_piece || !mv->frames) continue;
            rec = q2_creature_world_frame_name(&c->creatures, m, mv->first_frame);
            if (!rec || strncmp(rec->name, "Death", 5)) continue;
            check_death_clip_frames(c, i, mod->name, mv, mv->first_frame);
        }
    }
}

static void test_runtime_return(const char *disc, const char *dir)
{
    client *c = open_client(disc);
    q2_entity entities[4];
    q2_projectile shots[6];
    q2_zone_runtime creature[2] = {0};
    q2_zone_animation animation[2];
    u32 group[2], slot[2], indices[2], entity_slots[4], i, j, n = 0;
    q2_camera camera;
    s16 velocity[3];
    s32 anchor[3] = {-7349, 6113, -4798};
    const q2_model_bank *banks[2];
    q2_entity *e;
    CHECK(client_load_zone(c, "JAIL4", 3));
    census_death_clips(c);
    c->sim[0].level_time = 1000; c->sim[0].ent_world.level_time = 1000;
    q2_level_state.time = q2_level_state.framenum = 30;
    c->ai_accum = 0.037;
    banks[0] = c->sim[0].item_bank; banks[1] = &c->zone_bank;
    for (i = 0; i < 2; i++) {
        s32 pos[3] = {anchor[0] - 300 + (s32)i * 600, anchor[1] - 100, anchor[2]};
        e = q2_item_drop_spawn(&c->sim[0].entities, c->sim[0].item_table, banks, 2,
                               27, 768, pos, -1, drop_rand);
        CHECK(e != NULL);
        if (!e) { close_client(c); return; }
        entity_slots[i] = (u32)(e - c->sim[0].entities.ent);
        if (i == 0) CHECK(q2_item_drop_velocity(e, velocity));
        else {
            /* A settled drop uses the ordinary pickup think with a timed life. */
            e->think = q2_item_think; e->remove_in = 1234;
            e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
            q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);
        }
        e->frame = 37 + (s32)i; e->scale = 2048 + (s16)i;
    }
    e = q2_model_ent_spawn(&c->sim[0].entities, c->sim[0].item_bank,
                            0, anchor, 0, 2048, NULL);
    CHECK(e != NULL);
    if (!e) { close_client(c); return; }
    entity_slots[2] = (u32)(e - c->sim[0].entities.ent);
    e->frame = 173; e->fade = 3000;
    {
        q2_gib_parent parent;
        s16 impulse[3] = {20, -40, 10};
        q2_gib_parent_init(&parent);
        e = q2_gib_spawn_one(q2_gib_attached(), &parent, Q2_GIB_STYLE_MEAT,
                              768, impulse, anchor, -1, 1200, "Gib0");
        /* Retail allows a gib with an unresolved model; it must keep moving
         * and expiring across loads just like a visible body. */
        CHECK(e != NULL);
        if (!e) { close_client(c); return; }
        entity_slots[3] = (u32)(e - c->sim[0].entities.ent);
        CHECK(q2_gib_body_of(e) != NULL);
    }
    for (i = 0; i < c->creatures.set.count && n < 2; i++) {
        q2_monster *m = &c->creatures.set.monsters[i];
        q2_pop_group g;
        if (!m->in_use || !m->attack || !q2_pop_get_group(&c->creatures.pop, m->group, &g) ||
            strcmp(g.name, "Zone3")) continue;
        group[n] = m->group; slot[n] = m->population_slot; indices[n] = i;
        /* The start's go runs a frame after the spawn (monster_start). */
        if (m->think == q2_monster_go_think) q2_monster_go_think(m);
        m->enemy = m->goalentity = &c->creatures.sight;
        if (n == 0) m->attack(m);
        else {
            /* The tail of T_Damage (0x80062940): the hit has already landed. */
            m->health = 0;
            q2_monster_damage_reaction(m, NULL, 30); /* dying, not gibbed */
        }
        CHECK(m->currentmove != NULL);
        /* The attacker halfway through its attack, the other one frame into
         * its fall; the display cursor follows (capture_creature shows it). */
        if (m->currentmove)
            m->frame = (s16)(n == 0 ? (m->currentmove->first_frame + m->currentmove->last_frame) / 2
                                    : m->currentmove->first_frame + 1);
        pose_at_frame(c, i);
        m->next_think = q2_level_state.time + 4;
        m->attack_finished = q2_level_state.time + 9;
        m->pain_debounce = q2_level_state.time + 6;
        m->aiflags |= Q2_AI_HOLD_FRAME;
        n++;
    }
    CHECK(n == 2);
    if (n != 2) { close_client(c); return; }
    q2_projectiles_init(&c->sim[0].combat.projectiles);
    for (i = 0; i < 6; i++) {
        q2_projectile *p = &c->sim[0].combat.projectiles.p[i];
        p->in_use = true; p->kind = (q2_proj_kind)(i < 5 ? i + 1 : Q2_PROJ_HAND_GRENADE);
        p->owner = i == 3 ? -1 : 0;
        if (i == 3) p->shooter = &c->cre_actor[indices[0]];
        memcpy(p->pos, anchor, sizeof(p->pos));
        p->pos[0] += (s32)i * 80 - 200; p->pos[1] -= 400; p->pos[2] += 200;
        p->vel[0] = 8192; p->vel[1] = -4096; p->vel[2] = 12288;
        p->node = i == 5 ? Q2_PROJ_NODE_HELD : Q2_PROJ_NODE_UNKNOWN;
        p->expires = i == 5 ? 6000 : 1456 + (s32)i; p->damage = 21; p->mod = 5;
        p->splash_radius = 200; p->flags = 11; p->bounced = i == 1;
        p->angles[0] = 123; p->angles[1] = 789; p->angles[2] = 11;
        p->base[0] = 101; p->base[1] = 2000; p->phase = 1357;
        p->anim = 77; p->anim_end = 320; p->beam_time = 17;
    }
    c->sim[0].combat.projectiles.live = 6;
    camera = c->cam;
    camera.pos[0] = anchor[0]; camera.pos[1] = anchor[1] - 300; camera.pos[2] = anchor[2] + 1200;
    camera.yaw = 2048; camera.pitch = -160; camera.roll = 0;
    capture(c, dir, "before-zone-change", &camera);
    capture_creature(c, dir, "attacker-before-gate", indices[0]);
    capture_creature(c, dir, "dying-before-gate", indices[1]);
    for (i = 0; i < 2; i++) {
        q2_monster *m = &c->creatures.set.monsters[indices[i]];
        /* Two display samples between AI ticks; retain the fractional pose. */
        client_model_anim_sample(&c->cre_anim[indices[i]], m->currentmove, 2, m->frame * 30, 10, 100);
        client_model_anim_sample(&c->cre_anim[indices[i]], m->currentmove, 2, m->frame * 30, 10, 101);
        CHECK(c->cre_anim[indices[i]].cursor.ready);
    }
    /* A cold save in the zone carries all of this -- a save is not a gate. */
    CHECK(client_capture(c)); /* gives dynamics stable IDs */
    e = dynamic_entity(c, c->sim[0].entities.ent[entity_slots[0]].zone_serial);
    CHECK(e && q2_item_drop_velocity(e, velocity) && (e->flags & Q2_ITEM_TIMED));
    CHECK(c->sim[0].entities.ent[entity_slots[1]].flags & Q2_ITEM_TIMED);
    for (i = 0; i < c->snapshot.zone_history.count; i++) {
        const q2_zone_population *zp = &c->snapshot.zone_history.map[i];
        u32 k, dynamics = 0, projectiles = 0;
        if (strcmp(zp->map, "JAIL4")) continue;
        for (k = 0; k < zp->count; k++) {
            dynamics += zp->object[k].kind == Q2_ZONE_DYNAMIC;
            projectiles += zp->object[k].kind == Q2_ZONE_PROJECTILE;
        }
        CHECK(dynamics == 4 && projectiles == 6);
    }
    for (i = 0; i < 4; i++) entities[i] = c->sim[0].entities.ent[entity_slots[i]];
    for (i = 0; i < 4; i++) printf("runtime entity %u: serial %u, kind %u, place %u, model %s, live %d\n",
        i, entities[i].zone_serial, entities[i].kind, entities[i].place_id, entities[i].model, entities[i].in_use);
    memcpy(shots, c->sim[0].combat.projectiles.p, sizeof(shots));
    for (i = 0; i < 2; i++) {
        CHECK(q2_zone_capture_monster(&creature[i], &c->creatures, &c->creatures.set.monsters[indices[i]]));
        animation[i] = c->cre_anim[indices[i]];
    }
    CHECK(gate(c, 0));
    /*
     * Nothing of zone 3's dynamics joins zone 0. The two drops and the gib died
     * at the gate (q2_zone_leaving: +0x44 bit 4 at 0x80020B38 refused by
     * LoadStoreQuakePickup 0x8005B500, class 48's DestroyMe at 0x800A3A44). The
     * explosion model and the five flying shots are live objects on the console
     * -- no table hook, no InitCreFreeList or InitialiseMissiles on LoadZone's
     * path -- but their positions are outside zone 0's hull; only the held
     * grenade follows the player's hand.
     */
    for (i = 0; i < 4; i++) CHECK(dynamic_entity(c, entities[i].zone_serial) == NULL);
    for (i = 0; i < 5; i++) CHECK(dynamic_projectile(c, shots[i].zone_serial) == NULL);
    CHECK(q2_projectile_hand_held_index(&c->sim[0].combat.projectiles, 0) >= 0);
    CHECK(c->sim[0].combat.projectiles.live == 1);
    /* The active zone advances. */
    c->sim[0].level_time += 1000; c->sim[0].ent_world.level_time = c->sim[0].level_time;
    q2_level_state.time += 100; q2_level_state.framenum += 100;
    c->ai_accum = 0.037;
    c = cold_save(c, disc, dir, "unloaded-runtime");
    CHECK(gate(c, 3));
    CHECK(q2_level_state.time == 130 && q2_level_state.framenum == 130);
    CHECK(c->ai_accum > 0.036999 && c->ai_accum < 0.037001);
    for (j = 0; j < 2; j++) {
        u32 index = 0;
        q2_zone_runtime actual = {0};
        q2_monster *m;
        q2_projectile *p;
        /* Also exercise a cold native save in the zone the objects were in. */
        if (j) c = cold_save(c, disc, dir, "resident-runtime");
        /*
         * The console parks nothing: a slot SaveCreatures does not store is
         * killed or lives on in the next zone, where it runs to its end. So
         * what did not join zone 0 is not waiting here either
         * (q2_zone_history_capture rebuilds the dynamic records from the live
         * world); only the hand grenade, still in the hand, is back.
         */
        for (i = 0; i < 4; i++) CHECK(dynamic_entity(c, entities[i].zone_serial) == NULL);
        for (i = 0; i < 5; i++) CHECK(dynamic_projectile(c, shots[i].zone_serial) == NULL);
        p = dynamic_projectile(c, shots[5].zone_serial);
        CHECK(p != NULL);
        if (p) {
            CHECK(p->kind == shots[5].kind && p->node == Q2_PROJ_NODE_HELD && p->owner == 0);
            CHECK(p->expires == shots[5].expires && !memcmp(p->vel, shots[5].vel, sizeof(p->vel)));
        }
        /*
         * The attacker is a FRESH SPAWN. GeneralRespawnStuffStore (0x8007D318)
         * kept its class, position, yaw >> 6, health >> 2, nine spawn-flag
         * bits and target; GeneralRespawnStuff (0x8007D158) handed that to
         * SpawnGeneral (0x800565C4) and wrote the health again after export 0
         * (0x8007D304). Its attack move, frame, deadlines, enemy and display
         * cursor did not cross the gate; monster_start parked the go think a
         * frame ahead, as it does for any spawn.
         */
        m = authored_creature(c, group[0], slot[0], &index);
        CHECK(m && m->in_use && !m->dead);
        if (m) {
            const q2_monster *was = &creature[0].creature.state;
            printf("attacker return: health %d -> %d, yaw %d -> %d, frame %d -> %d, "
                   "think %s, next %d (level %d)\n", was->health, m->health,
                   was->angles[2], m->angles[2], was->frame, m->frame,
                   m->think == q2_monster_go_think ? "go" : "other",
                   m->next_think, q2_level_state.time);
            CHECK(m->health == (was->health & ~3) && m->health != was->health);
            CHECK(m->angles[2] == (was->angles[2] & 0xFC0) && m->ideal_yaw == m->angles[2]);
            CHECK(m->pos[0] == was->pos[0] && m->pos[2] == was->pos[2]);
            CHECK(m->spawnflags == was->spawnflags);
            CHECK(m->spawn_initialized && !m->resume_pending);
            CHECK(m->think == q2_monster_go_think && m->next_think == q2_level_state.time + 1);
            CHECK(m->enemy == NULL && m->goalentity == NULL && m->oldenemy == NULL);
            CHECK(m->attack_finished == 0 && m->pain_debounce == 0);
            CHECK(!(m->aiflags & Q2_AI_HOLD_FRAME));
            CHECK(m->frame != was->frame);
            CHECK(q2_zone_capture_monster(&actual, &c->creatures, m));
            CHECK(actual.creature.move_offset != creature[0].creature.move_offset ||
                  strcmp(actual.creature.move_module, creature[0].creature.move_module) ||
                  !actual.creature.has_move);
            /* Nothing of the live cursor came back either. */
            CHECK(!c->cre_anim[index].cursor.ready || !animation[0].cursor.ready ||
                  c->cre_anim[index].cursor.position != animation[0].cursor.position);
        }
        /*
         * The one caught dying was finished by FinishDying (0x8007F630): a
         * class-47 body with no edict, no think and no AI methods, posed on its
         * death clip's last frame (RespawnQuakeDeadBody 0x8007D748), one frame
         * into its fall when the gate was crossed.
         */
        m = authored_creature(c, group[1], slot[1], &index);
        CHECK(m && m->in_use && m->dead && m->corpse);
        if (m) {
            CHECK(m->class_id == Q2_CLASS_CORPSE &&
                  m->corpse_was_class == creature[1].creature.state.class_id);
            CHECK(m->currentmove != NULL);
            if (m->currentmove) {
                CHECK(m->frame == (s16)m->currentmove->last_frame);
                CHECK(m->frame != creature[1].creature.state.frame);
                CHECK(creature[1].creature.state.frame == m->currentmove->first_frame + 1);
            }
            CHECK(m->think == NULL && m->next_think == 0);
            CHECK(m->stand == NULL && m->attack == NULL && m->enemy == NULL);
            CHECK((m->svflags & Q2_SVF_DEADMONSTER) && !(m->svflags & Q2_SVF_MONSTER));
            check_death_clip(c, index, creature[1].creature.state.frame);
        }
    }
    capture(c, dir, "after-zone-return", &camera);
    {
        u32 index = 0;
        if (authored_creature(c, group[0], slot[0], &index))
            capture_creature(c, dir, "attacker-after-return", index);
        if (authored_creature(c, group[1], slot[1], &index))
            capture_creature(c, dir, "corpse-after-return", index);
    }
    /* The attacker's parked go installs its stand and runs from there; the
     * body's cursor does not move (the corpse tick drives it from here, as
     * 0x8007F71C does on the console, and a drawn frame leaves it alone). */
    {
        u32 index = 0, end = 0;
        q2_monster *m = authored_creature(c, group[0], slot[0], NULL);
        if (m && m->think == q2_monster_go_think) {
            m->think(m);
            CHECK(m->currentmove != NULL && m->think == q2_M_MoveFrame);
            if (m->currentmove) {
                const q2_mmove *move = m->currentmove;
                s16 frame = m->frame;
                CHECK(m->frame >= move->first_frame && m->frame <= move->last_frame);
                q2_M_MoveFrame(m);
                CHECK(m->frame != frame || m->currentmove != move);
            }
        }
        m = authored_creature(c, group[1], slot[1], &index);
        if (m && m->currentmove) {
            s16 frame = m->frame, cursor = m->motion.anim_position;
            CHECK(clip_end_tick(c, index, frame, &end) && cursor == (s16)end);
            q2_monster_corpse_tick(m);
            CHECK(m->frame == frame && m->in_use && m->corpse);
            CHECK(m->motion.anim_position == cursor && m->motion.animation_ready);
        }
    }
    /* Detached corpses have their own dissolve timer and callback policy. */
    {
        q2_monster *m = authored_creature(c, group[1], slot[1], NULL);
        CHECK(m != NULL);
        if (m) {
            q2_monster_corpse_detach(m);
            m->dissolve = 1007; m->dissolve_arm = true;
        }
    }
    /* Consumed dynamics stay gone even after their allocator slot is reused. */
    e = dynamic_entity(c, entities[0].zone_serial);
    if (e) q2_entity_remove(e);
    e = q2_entity_alloc(&c->sim[0].entities);
    CHECK(e && e->zone_serial == 0);
    for (i = 0; i < Q2_PROJ_MAX; i++)
        if (c->sim[0].combat.projectiles.p[i].zone_serial == shots[3].zone_serial)
            CHECK(q2_projectile_expire(&c->sim[0].combat.projectiles, i));
    CHECK(gate(c, 0)); CHECK(gate(c, 3));
    CHECK(dynamic_entity(c, entities[0].zone_serial) == NULL);
    CHECK(dynamic_projectile(c, shots[3].zone_serial) == NULL);
    {
        q2_monster *m = authored_creature(c, group[1], slot[1], NULL);
        CHECK(m && m->corpse && m->dissolve == 1007 && m->dissolve_arm);
        if (m) {
            q2_monster expected = *m;
            q2_monster_corpse_tick(&expected);
            q2_monster_corpse_tick(m);
            CHECK(m->dissolve == expected.dissolve && m->in_use == expected.in_use);
        }
    }
    close_client(c);
}

int main(int argc, char **argv)
{
    test_population_history();
    test_deferred_drops();
    if (argc >= 3 && !strcmp(argv[1], "--headless")) {
        test_return(argv[2], argc > 3 ? argv[3] : NULL);
        test_ambush_return(argv[2]);
        test_runtime_return(argv[2], argc > 3 ? argv[3] : NULL);
        test_overlap(argv[2]);
    }
    else if (argc != 1) return 2;
    printf("%d zone checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
