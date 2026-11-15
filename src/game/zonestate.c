#include "zonestate.h"
#include "sim.h"
#include "creworld.h"
#include "entitydraw.h"
#include "spawn.h"        /* q2_spawn_place: the Population record's box drop */
#include "worldscale.h"   /* Q2_EYE_BASE: the record's y is the object's +286  */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool map_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 'a' - 'A';
        if (y >= 'a' && y <= 'z') y -= 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

static u32 identity(const q2_population *pop)
{
    u32 i, hash = 2166136261u;
    for (i = 0; i < pop->size; i++) hash = (hash ^ pop->data[i]) * 16777619u;
    return (hash ^ (u32)q2_cre_skill()) * 16777619u;
}

static const q2_zone_population *find_map(const q2_zone_history *h,
                                          const char *name)
{
    u32 i;
    for (i = 0; h && name && i < h->count; i++)
        if (map_equal(h->map[i].map, name)) return &h->map[i];
    return NULL;
}

static q2_zone_object *find_object(const q2_zone_population *p, u8 kind,
                                   u32 group, u32 slot)
{
    u32 i;
    for (i = 0; p && i < p->count; i++) {
        q2_zone_object *o = &p->object[i];
        if (o->kind == kind && o->group == group && o->slot == slot) return o;
    }
    return NULL;
}

static q2_zone_object *object_open(q2_zone_population *p, u8 kind,
                                   u32 group, u32 slot)
{
    q2_zone_object *o = find_object(p, kind, group, slot);
    if (o) return o;
    if (p->count == Q2_ZONE_HISTORY_OBJECTS) return NULL;
    if (p->count == p->capacity) {
        u32 cap = p->capacity ? p->capacity * 2 : 32;
        if (cap > Q2_ZONE_HISTORY_OBJECTS) cap = Q2_ZONE_HISTORY_OBJECTS;
        q2_zone_object *next = realloc(p->object, cap * sizeof(*next));
        if (!next) return NULL;
        p->object = next;
        p->capacity = cap;
    }
    o = &p->object[p->count++];
    memset(o, 0, sizeof(*o));
    o->kind = kind; o->group = group; o->slot = slot;
    return o;
}

static void population_free(q2_zone_population *p)
{
    u32 i;
    for (i = 0; p->object && i < p->count; i++) free(p->object[i].runtime);
    free(p->object);
    memset(p, 0, sizeof(*p));
}

static bool population_copy(q2_zone_population *dst, const q2_zone_population *src)
{
    u32 i;
    *dst = *src;
    dst->object = NULL;
    dst->count = 0;
    dst->capacity = src->count;
    if (!src->count) return true;
    dst->object = calloc(src->count, sizeof(*dst->object));
    if (!dst->object) return false;
    for (i = 0; i < src->count; i++) {
        dst->object[i] = src->object[i];
        dst->object[i].runtime = NULL;
        dst->count++;
        if (src->object[i].runtime) {
            dst->object[i].runtime = malloc(sizeof(q2_zone_runtime));
            if (!dst->object[i].runtime) { population_free(dst); return false; }
            *dst->object[i].runtime = *src->object[i].runtime;
        }
    }
    return true;
}

void q2_zone_history_free(q2_zone_history *h)
{
    u32 i;
    if (!h) return;
    for (i = 0; h->map && i < h->count; i++) population_free(&h->map[i]);
    free(h->map);
    memset(h, 0, sizeof(*h));
}

static bool valid_ref(q2_zone_ref r)
{
    return r.kind <= Q2_ZREF_CORNER &&
           (r.kind != Q2_ZREF_CREATURE ||
            (r.group < Q2_ZONE_HISTORY_OBJECTS && r.slot < Q2_ZONE_HISTORY_OBJECTS && r.id < 256)) &&
           (r.kind != Q2_ZREF_CORNER || r.slot < Q2_ZONE_HISTORY_OBJECTS);
}

bool q2_zone_runtime_valid(u8 kind, const q2_zone_runtime *r)
{
    u32 i;
    if (!r) return kind == Q2_ZONE_ITEM || kind == Q2_ZONE_CREATURE;
    if (kind == Q2_ZONE_CREATURE) {
        if (r->creature.has_move > 1 || r->creature.thinking > Q2_CRE_THINK_LAST ||
            r->creature.animation.source > 4 ||
            !memchr(r->creature.move_module, 0, sizeof(r->creature.move_module)) ||
            (r->creature.has_move && !r->creature.move_module[0])) return false;
        for (i = 0; i < 5; i++) if (!valid_ref(r->creature.refs[i])) return false;
        return true;
    }
    if (kind == Q2_ZONE_PROJECTILE)
        return r->projectile.state.in_use && r->projectile.state.kind > Q2_PROJ_NONE &&
               r->projectile.state.kind <= Q2_PROJ_BFG &&
               r->projectile.state.owner >= -1 && r->projectile.state.owner < Q2_MAX_PLAYERS &&
               (r->projectile.source_slot == UINT32_MAX || r->projectile.source_slot < Q2_PROJ_MAX) &&
               (r->projectile.state.node == Q2_PROJ_NODE_UNKNOWN || r->projectile.state.node == Q2_PROJ_NODE_HELD) &&
               (r->projectile.shooter.kind == Q2_ZREF_NONE || r->projectile.shooter.kind == Q2_ZREF_CREATURE) &&
               valid_ref(r->projectile.shooter);
    if (kind == Q2_ZONE_ITEM || kind == Q2_ZONE_DYNAMIC)
        return r->entity.state.in_use && r->entity.think <= Q2_ZTHINK_GIB &&
               (r->entity.source_slot == UINT32_MAX || r->entity.source_slot < Q2_ZONE_HISTORY_OBJECTS) &&
               r->entity.state.shadow_vertex_count <= Q2_ITEM_SHADOW_VERTEX_MAX &&
               memchr(r->entity.state.model, 0, sizeof(r->entity.state.model)) != NULL &&
               (r->entity.state.kind == Q2_ENT_KIND_ITEM ||
                r->entity.state.kind == Q2_ENT_KIND_MODEL ||
                r->entity.state.kind == Q2_GIB_BOSS_CLASS);
    return false;
}

bool q2_zone_history_valid(const q2_zone_history *h)
{
    u32 i, j, k;
    if (!h || h->count > Q2_ZONE_HISTORY_MAPS || (h->count && !h->map)) return false;
    for (i = 0; i < h->count; i++) {
        const q2_zone_population *p = &h->map[i];
        if (!p->map[0] || p->map[Q2_ZONE_MAP_NAME - 1] ||
            p->count > Q2_ZONE_HISTORY_OBJECTS || (p->count && !p->object) ||
            p->ai_phase_us > 500000 || p->trail.head >= Q2_TRAIL_LENGTH) return false;
        for (j = 0; j < i; j++) if (map_equal(p->map, h->map[j].map)) return false;
        for (j = 0; j < 3; j++) if (!valid_ref(p->alerts[j])) return false;
        for (j = 0; j < p->count; j++) {
            const q2_zone_object *o = &p->object[j];
            if (o->kind < Q2_ZONE_ITEM || o->kind > Q2_ZONE_PROJECTILE ||
                o->group >= Q2_ZONE_HISTORY_OBJECTS ||
                (o->kind <= Q2_ZONE_CREATURE ? o->slot >= Q2_ZONE_HISTORY_OBJECTS :
                 (!o->slot || o->slot > p->next_serial || o->group != 0)) ||
                o->zone < 0 || o->zone > 255 || o->removed > 1 || o->dead > 1 ||
                o->hidden > 1 || o->shrink > 1 || o->taken > 15 ||
                !q2_zone_runtime_valid(o->kind, o->runtime)) return false;
            if (o->runtime) {
                if (!p->runtime_present || o->removed) return false;
                if (o->kind == Q2_ZONE_PROJECTILE &&
                    o->runtime->projectile.state.zone_serial != o->slot) return false;
                if (o->kind == Q2_ZONE_DYNAMIC &&
                    (o->runtime->entity.state.zone_serial != o->slot ||
                     o->runtime->entity.state.population_group != -1)) return false;
                if ((o->kind == Q2_ZONE_ITEM || o->kind == Q2_ZONE_DYNAMIC) &&
                    o->runtime->entity.state.place_id != o->id) return false;
            }
            for (k = 0; k < j; k++)
                if (p->object[k].kind == o->kind && p->object[k].group == o->group &&
                    p->object[k].slot == o->slot) return false;
        }
    }
    return true;
}

q2_result q2_zone_history_copy(q2_zone_history *dst, const q2_zone_history *src)
{
    q2_zone_history copy = {0};
    u32 i;
    if (!dst || !q2_zone_history_valid(src)) return Q2_ERR_INVALID_ARG;
    if (src->count) {
        copy.map = calloc(src->count, sizeof(*copy.map));
        if (!copy.map) return Q2_ERR_NO_MEMORY;
    }
    copy.count = src->count;
    for (i = 0; i < copy.count; i++) {
        if (!population_copy(&copy.map[i], &src->map[i])) {
            q2_zone_history_free(&copy); return Q2_ERR_NO_MEMORY;
        }
    }
    q2_zone_history_free(dst);
    *dst = copy;
    return Q2_OK;
}

static q2_zone_runtime *runtime_open(q2_zone_object *o, const q2_sim *sim)
{
    if (!o->runtime) o->runtime = calloc(1, sizeof(*o->runtime));
    if (o->runtime) {
        o->runtime->sim_time = sim->level_time;
        o->runtime->ai_time = q2_level_state.time;
        o->runtime->ai_frame = q2_level_state.framenum;
        o->runtime->materialized = true;
    }
    return o->runtime;
}

static bool capture_entity(q2_zone_runtime *r, const q2_entity *e, u32 slot)
{
    const q2_gib_body *gib;
    r->entity.state = *e;
    r->entity.model_bound = e->model_bank != NULL && e->model_index >= 0;
    r->entity.state.think = NULL;
    r->entity.state.model_bank = NULL;
    r->entity.state.def = NULL;
    r->entity.state.model_index = r->entity.state.node = -1;
    r->entity.source_slot = slot;
    if (!e->think) r->entity.think = Q2_ZTHINK_NONE;
    else if (e->think == q2_item_think) r->entity.think = Q2_ZTHINK_ITEM;
    else if (e->think == q2_item_shrink_think) r->entity.think = Q2_ZTHINK_SHRINK;
    else if (e->think == q2_item_drop_think) {
        r->entity.think = Q2_ZTHINK_DROP;
        if (!q2_item_drop_velocity(e, r->entity.velocity)) return false;
    } else if (e->think == q2_model_ent_think) r->entity.think = Q2_ZTHINK_MODEL;
    else if (e->think == q2_gib_think) {
        r->entity.think = Q2_ZTHINK_GIB;
        gib = q2_gib_body_of(e);
        if (!gib) return false;
        r->entity.gib = *gib;
        r->entity.gib.cell = -1;
    } else return false;
    return true;
}

/*
 * GeneralRespawnStuffStore, 0x8007D318 -- what the console keeps of a LIVING
 * creature at a zone gate, written here as GeneralRespawnStuff (0x8007D158)
 * decodes it, so that the record restores without a second conversion.
 *
 * The record is 12 bytes when every coordinate (+0x54/+0x58/+0x5C, raw) fits a
 * signed halfword and health/4 (toward zero: `bgez`, `addiu 3`, `sra 2`,
 * 0x8007D37C..0x8007D390) fits a signed byte (0x8007D334..0x8007D3A4); then
 * the position is stored as halfwords (y + 286, 0x8007D3F0) and the health as
 * that byte, decoded `<< 2` (0x8007D1E4..0x8007D1E8). Otherwise 20 bytes carry
 * the position and the health whole. Both forms pack into the first word the
 * class (+0xD2 & 0x3F, bits 1..6), eleven spawn-flag bits (bits 7..17: the
 * edict's +0x1C >> 18 & 0x1FF or the record's +0xF2 & 0x600, 0x8007D3BC..
 * 0x8007D3D4), the target in eight bits (bits 18..25, 0x8007D524) and the yaw
 * in six (+0xE8 & 0xFFF >> 6, bits 26..31, 0x8007D40C..0x8007D418). The target
 * is the edict's +0x16, or, when that is zero and the creature is walking to a
 * class-114 path corner (+0x88, byte +0x23), the corner's +0x18 (0x8007D454..
 * 0x8007D488). Nothing else: no move, frame, enemy, think or deadline.
 */
static void store_living_record(q2_zone_object *o, const q2_creature_world *w,
                                const q2_monster *m)
{
    const s32 *pos = q2_cre_origin(m);
    s32 h = m->health;
    s32 quarter = h < 0 ? (h + 3) >> 2 : h >> 2;
    bool compact = pos[0] >= INT16_MIN && pos[0] <= INT16_MAX &&
                   pos[1] >= INT16_MIN && pos[1] <= INT16_MAX &&
                   pos[2] >= INT16_MIN && pos[2] <= INT16_MAX &&
                   quarter >= INT8_MIN && quarter <= INT8_MAX;
    s32 target = m->target;
    u32 flags = (m->spawnflags >> 18) & 0x1FFu;
    q2_pop_group g;
    q2_pop_spawn rec;
    if (compact) {
        /* `sh` of y + 286, read back with `lh`: whole unless y lies within
         * 286 of the halfword's top, where the console's record wraps. */
        o->pos[0] = (s16)pos[0];
        o->pos[1] = (s32)(s16)(pos[1] + Q2_EYE_BASE) - Q2_EYE_BASE;
        o->pos[2] = (s16)pos[2];
        o->health = (s16)(quarter * 4);
    } else {
        memcpy(o->pos, pos, sizeof(o->pos));
        o->health = (s16)h;     /* `lhu` +0x108, `sh` into the record */
    }
    o->yaw = q2_cre_yaw(m) & 0xFC0;
    if (!target && m->movetarget && m->movetarget->class_id == Q2_CLASS_PATH_CORNER)
        target = m->movetarget->targetname;
    o->target = (s16)(target & 0xFF);
    if (w && w->pop_ready && q2_pop_get_group(&w->pop, m->group, &g) &&
        q2_pop_get_spawn(&w->pop, &g, m->population_slot, &rec))
        flags |= rec.flags & 0x600u;
    o->spawnflags = flags;
}

static bool assign_serial(q2_zone_population *p, u32 *serial)
{
    if (!*serial) {
        if (p->next_serial == UINT32_MAX) return false;
        *serial = ++p->next_serial;
    } else if (*serial > p->next_serial) p->next_serial = *serial;
    return true;
}

q2_result q2_zone_history_capture(q2_zone_history *h, const char *map, int zone,
                                  q2_sim *sim, const q2_creature_world *world,
                                  const q2_actor *actors,
                                  const q2_zone_animation *animation, u32 phase_us)
{
    q2_zone_population p = {0};
    const q2_zone_population *old;
    const q2_population *pop;
    u32 i, gi, slot;
    q2_result failure = Q2_ERR_NO_MEMORY;
    if (!h || !map || !map[0] || strlen(map) >= sizeof(p.map) || !sim ||
        zone < 0 || zone > 255)
        return Q2_ERR_INVALID_ARG;
    /* QFRONT rebuilds its logo and player-preview props on every load. They
     * are presentation objects with private thinks, not a persistent zone
     * population. The outgoing sim identifies them even when the client has
     * already changed its front-end flag for the destination. */
    if (sim->multiplayer || sim->scene_ready) return Q2_OK;
    pop = sim->item_population_ready ? &sim->item_population :
          world && world->pop_ready ? &world->pop : NULL;
    if (!pop) return Q2_OK;
    memcpy(p.map, map, strlen(map) + 1);
    p.identity = identity(pop);
    old = find_map(h, map);
    if (old && old->identity == p.identity && !population_copy(&p, old))
        return Q2_ERR_NO_MEMORY;
    p.runtime_present = true;
    p.ai_time = q2_level_state.time; p.ai_frame = q2_level_state.framenum;
    p.ai_phase_us = phase_us;
    q2_trail_capture(&p.trail);
    p.alerts[0] = q2_zone_ref_capture(world, q2_level_state.sight_entity);
    p.alerts[1] = q2_zone_ref_capture(world, q2_level_state.sound_entity);
    p.alerts[2] = q2_zone_ref_capture(world, q2_level_state.sound2_entity);
    p.alert_frame[0] = q2_level_state.sight_entity_framenum;
    p.alert_frame[1] = q2_level_state.sound_entity_framenum;
    p.alert_frame[2] = q2_level_state.sound2_entity_framenum;
    for (i = 0; i < 3; i++) if (!valid_ref(p.alerts[i])) goto invalid;
    for (i = 0; i < p.count; i++) {
        q2_zone_object *o = &p.object[i];
        if (o->zone != zone || !o->runtime) continue;
        if (o->kind == Q2_ZONE_ITEM) o->runtime->entity.source_slot = UINT32_MAX;
    }
    /*
     * The console parks nothing. A slot SaveCreatures (0x800569C8) does not
     * store is either killed at the gate or still live in the next zone, where
     * it runs to its own end; nothing waits in an unloaded zone to resume on a
     * return. So every dynamic and projectile record is rebuilt from the live
     * world below, and one that did not join the last destination is gone.
     */
    for (i = 0; i < p.count; ) {
        if (p.object[i].kind >= Q2_ZONE_DYNAMIC) {
            free(p.object[i].runtime);
            p.object[i] = p.object[--p.count];
        } else i++;
    }
    /* Walk authored places, including ones whose reusable runtime slot was
     * collected and subsequently overwritten by a drop or CREBATCH. */
    for (gi = 0; sim->item_population_ready && sim->item_group_run &&
                 gi < pop->group_count; gi++) {
        q2_pop_group g;
        if (!sim->item_group_run[gi] || !q2_pop_get_group(pop, gi, &g)) continue;
        for (slot = 0; ; slot++) {
            q2_pop_place place;
            const q2_entity *e = NULL;
            q2_zone_object *o;
            if (!q2_pop_get_place(pop, &g, slot, &place)) break;
            if (!q2_pop_place_allows_skill(place.angle_flags, q2_cre_skill()) ||
                !q2_item_find(sim->item_table, place.id)) continue;
            o = object_open(&p, Q2_ZONE_ITEM, gi, slot);
            if (!o) goto no_memory;
            for (i = 0; i < sim->entities.count; i++) {
                const q2_entity *candidate = &sim->entities.ent[i];
                if (candidate->kind == Q2_ENT_KIND_ITEM && candidate->place_id == place.id &&
                    candidate->population_group == (s32)gi && candidate->population_slot == slot) {
                    e = candidate; break;
                }
            }
            /* A model unavailable in the destination was deferred, not taken.
             * Only a record actually materialized here can become a tombstone. */
            if ((!e || !e->in_use) && o->runtime && !o->runtime->materialized) continue;
            o->zone = zone; o->id = place.id; o->removed = !e || !e->in_use;
            if (!e || !e->in_use) { free(o->runtime); o->runtime = NULL; continue; }
            if (!runtime_open(o, sim)) goto no_memory;
            if (!capture_entity(o->runtime, e, (u32)(e - sim->entities.ent))) goto invalid;
            o->health = e->health; o->scale = e->scale; o->frame = e->frame;
            o->yaw = e->angles[1]; o->hidden = e->hidden; o->taken = 0;
            o->shrink = e->think == q2_item_shrink_think;
            o->remove_in = e->remove_in; o->respawn_at = e->respawn_at;
            memcpy(o->pos, e->pos, sizeof(o->pos));
            for (i = 0; i < Q2_MAX_PLAYERS; i++) if (e->taken[i]) o->taken |= (u8)(1u << i);
        }
    }
    for (i = 0; world && world->ready && i < world->set.count; i++) {
        const q2_monster *m = &world->set.monsters[i];
        q2_zone_object *o;
        if (!m->population_active && !m->in_use) continue;
        o = object_open(&p, Q2_ZONE_CREATURE, m->group, m->population_slot);
        if (!o) goto no_memory;
        o->zone = zone; o->id = m->pop_class_id; o->removed = !m->in_use;
        o->dead = m->dead; o->frame = m->frame;
        if (m->in_use && !m->dead && !m->corpse) store_living_record(o, world, m);
        else {
            /* A body's runtime is what comes back; a cold save's dying creature
             * keeps full precision until a gate finishes it. */
            o->health = m->health; o->yaw = q2_cre_yaw(m); o->target = m->target;
            o->spawnflags = (m->spawnflags >> 18) & 0x1FFu;
            memcpy(o->pos, q2_cre_origin(m), sizeof(o->pos));
        }
        if (!m->in_use) { free(o->runtime); o->runtime = NULL; continue; }
        if (!runtime_open(o, sim)) goto no_memory;
        if (!q2_zone_capture_monster(o->runtime, world, m)) goto invalid;
        memset(&o->runtime->creature.animation, 0, sizeof(q2_zone_animation));
        if (animation && (animation[i].move == m->currentmove || animation[i].source == 3)) {
            o->runtime->creature.animation.cursor = animation[i].cursor;
            o->runtime->creature.animation.source = animation[i].source;
        }
    }
    for (i = 0; i < sim->entities.count; i++) {
        q2_entity *e = &sim->entities.ent[i];
        q2_zone_object *o;
        if (!e->in_use || e->population_group >= 0 ||
            (e->kind != Q2_ENT_KIND_ITEM && e->kind != Q2_ENT_KIND_MODEL &&
             e->kind != Q2_GIB_BOSS_CLASS)) continue;
        if (!assign_serial(&p, &e->zone_serial)) goto no_memory;
        o = object_open(&p, Q2_ZONE_DYNAMIC, 0, e->zone_serial);
        if (!o || !runtime_open(o, sim)) goto no_memory;
        o->zone = zone; o->removed = false; o->id = e->place_id;
        memcpy(o->pos, e->pos, sizeof(o->pos));
        if (!capture_entity(o->runtime, e, i)) goto invalid;
    }
    for (i = 0; i < Q2_PROJ_MAX; i++) {
        q2_projectile *shot = &sim->combat.projectiles.p[i];
        q2_zone_object *o;
        q2_zone_ref shooter = {0};
        if (!shot->in_use) continue;
        if (!assign_serial(&p, &shot->zone_serial)) goto no_memory;
        o = object_open(&p, Q2_ZONE_PROJECTILE, 0, shot->zone_serial);
        if (!o || !runtime_open(o, sim)) goto no_memory;
        if (shot->shooter && world && actors) {
            for (gi = 0; gi < world->set.count; gi++)
                if (shot->shooter == &actors[gi]) {
                    shooter = q2_zone_ref_capture(world, &world->set.monsters[gi]);
                    break;
                }
            if (gi == world->set.count) goto invalid;
        } else shooter = o->runtime->projectile.shooter;
        o->zone = zone; o->removed = false;
        memcpy(o->pos, shot->pos, sizeof(o->pos));
        o->runtime->projectile.state = *shot;
        o->runtime->projectile.source_slot = i;
        o->runtime->projectile.state.shooter = NULL;
        if (shot->node != Q2_PROJ_NODE_HELD) o->runtime->projectile.state.node = Q2_PROJ_NODE_UNKNOWN;
        o->runtime->projectile.shooter = shooter;
    }
    /* Dynamic slots are reusable and have no authored respawn to suppress. */
    for (i = 0; i < p.count; ) {
        if (p.object[i].kind >= Q2_ZONE_DYNAMIC && p.object[i].removed) {
            free(p.object[i].runtime);
            p.object[i] = p.object[--p.count];
        } else i++;
    }
    if (old) {
        i = (u32)(old - h->map);
        population_free(&h->map[i]);
        h->map[i] = p;
    } else {
        q2_zone_population *next;
        if (h->count == Q2_ZONE_HISTORY_MAPS) goto no_memory;
        next = realloc(h->map, (h->count + 1) * sizeof(*next));
        if (!next) goto no_memory;
        h->map = next;
        h->map[h->count++] = p;
    }
    return Q2_OK;
invalid:
    failure = Q2_ERR_BAD_FORMAT;
no_memory:
    population_free(&p);
    return failure;
}

void q2_zone_history_items(const q2_zone_history *h, const char *map, int zone,
                            q2_sim *sim, const q2_model_bank *zone_bank)
{
    const q2_zone_population *p = find_map(h, map);
    u32 i, k;
    if (!sim || sim->multiplayer || !sim->item_population_ready || !p ||
        p->identity != identity(&sim->item_population)) return;
    for (i = 0; i < p->count; i++)
        if (p->object[i].runtime) p->object[i].runtime->materialized = false;
    /* A previously selected ambush stays selected even if its CREBATCH was
     * invoked from a repeatable event which the EVE_ replay does not run. */
    for (i = 0; i < p->count; i++) {
        const q2_zone_object *o = &p->object[i];
        q2_pop_group g;
        if (o->kind == Q2_ZONE_ITEM && o->zone == zone &&
            q2_pop_get_group(&sim->item_population, o->group, &g))
            q2_sim_activate_item_group(sim, g.name);
    }
    for (i = 0; i < sim->entities.count; i++) {
        q2_entity *e = &sim->entities.ent[i];
        const q2_zone_object *o;
        if (e->kind != Q2_ENT_KIND_ITEM || e->population_group < 0) continue;
        o = find_object(p, Q2_ZONE_ITEM, (u32)e->population_group, e->population_slot);
        if (!o || o->id != e->place_id) continue;
        if (o->removed) { q2_entity_remove(e); continue; }
        if (o->runtime) {
            if (!q2_zone_apply_entity(o, sim, i, zone_bank)) q2_entity_remove(e);
            continue;
        }
        e->health = o->health; e->scale = o->scale; e->frame = o->frame;
        e->angles[1] = o->yaw; e->hidden = o->hidden != 0;
        e->remove_in = o->remove_in; e->respawn_at = o->respawn_at;
        e->think = o->shrink ? q2_item_shrink_think : q2_item_think;
        for (k = 0; k < Q2_MAX_PLAYERS; k++) e->taken[k] = (o->taken & (1u << k)) != 0;
        memcpy(e->pos, o->pos, sizeof(e->pos));
        memcpy(e->origin, o->pos, sizeof(e->origin));
        e->origin[1] += Q2_EYE_BASE - e->model_offset;
        q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);
        e->node = sim->coll_ready ? q2_coll_find_node(&sim->coll, e->pos, -1, true) : -1;
    }
    for (i = 0; i < p->count; i++) {
        q2_zone_object *o = &p->object[i];
        q2_entity *e;
        if (o->kind != Q2_ZONE_DYNAMIC || o->removed || !o->runtime) continue;
        if (o->zone != zone && (!sim->coll_primary_ready ||
            q2_coll_find_node(&sim->coll_primary, o->pos, -1, true) < 0)) continue;
        for (k = 0; k < sim->entities.count; k++)
            if (sim->entities.ent[k].in_use && sim->entities.ent[k].zone_serial == o->slot) break;
        if (k < sim->entities.count) continue;
        e = q2_entity_alloc(&sim->entities);
        if (!e) { Q2_WARN("zone: dynamic entity deferred (entity capacity)"); continue; }
        k = (u32)(e - sim->entities.ent);
        if (!q2_zone_apply_entity(o, sim, k, zone_bank)) q2_entity_remove(e);
        else o->zone = zone;
    }
}

void q2_zone_history_creatures(q2_zone_history *h, const char *map,
                                int zone, q2_sim *sim, q2_creature_world *world)
{
    const q2_zone_population *p = find_map(h, map);
    u32 i;
    if (!world || !world->pop_ready || !sim || sim->multiplayer) return;
    if (p && p->identity != identity(&world->pop)) p = NULL;
    if (p && p->runtime_present) {
        q2_level_state.time = p->ai_time;
        q2_level_state.framenum = p->ai_frame;
        q2_level_tick = p->ai_time;             /* and the global count, 0x800B29E0 */
        q2_trail_restore(&p->trail);
    }
    for (i = 0; i < world->set.count; i++) {
        q2_monster *m = &world->set.monsters[i];
        q2_zone_object *o = find_object(p, Q2_ZONE_CREATURE, m->group, m->population_slot);
        bool here;
        m->population_active = m->in_use;
        if (!o || o->id != m->pop_class_id) continue;
        here = sim->coll_primary_ready
                 ? q2_coll_find_node(&sim->coll_primary, o->pos, -1, true) >= 0
                 : o->zone == zone;
        /* Never let an out-of-zone placeholder overwrite a saved resident.
         * A creature which moved into overlapping geometry follows its saved
         * position, rather than being duplicated at its authored spawn. */
        m->in_use = here && !o->removed;
        m->spawn_pending = false;
        m->population_active = here;
        m->dead = here && o->dead != 0;
        m->health = o->health; m->angles[2] = (s16)o->yaw;
        m->ideal_yaw = m->angles[2]; m->target = o->target;
        /* 0x8007E618..0x8007E62C: the record's nine bits into +0x1C 18..26. */
        m->spawnflags = (m->spawnflags & 0xF803FFFFu) | ((o->spawnflags & 0x1FFu) << 18);
        memcpy(m->pos, o->pos, sizeof(m->pos));
        if (!o->dead) {
            /*
             * GeneralRespawnStuff, 0x8007D158: the record becomes a spawn
             * record -- class, position, yaw & 0xFFF, target, spawn flags
             * (0x8007D2A8..0x8007D2E4) -- and SpawnGeneral (0x800565C4) spawns
             * it afresh. Its skill test on +0x14 (0x800/0x1000/0x2000,
             * 0x80056620..0x80056660) cannot refuse a record whose flags are
             * eleven bits, and 0x8007E440 places it through SetupBoxDrop
             * (0x80050FA0) from the record's y: the drop a Population record
             * takes here (q2_spawn_place; the console passes a zero drop for
             * record classes 2, 7, 8 and 11 -- Boss2, an unused id, Flyer and
             * Hover -- at 0x8007E4CC..0x8007E508, which the shared placement
             * does not distinguish for a Population record either). The stored health
             * is written before export 0 (0x8007E680) -- here, before the wake
             * -- and again after it, by q2_zone_history_spawned (0x8007D304).
             * Nothing of the live move survives: no runtime is resumed, and
             * the one a cold save may have left on this record is consumed.
             */
            if (m->in_use && world->placement_coll) {
                q2_pop_spawn rec = {0};
                s32 origin[3];
                rec.x = o->pos[0]; rec.y = o->pos[1] + Q2_EYE_BASE; rec.z = o->pos[2];
                if (q2_spawn_place(&rec, world->placement_coll, world->placement_world, origin))
                    memcpy(m->pos, origin, sizeof(m->pos));
            }
            free(o->runtime);
            o->runtime = NULL;
            q2_cre_motion_init(m);
            m->spawn_initialized = false;
            m->resume_pending = false;
            continue;
        }
        /* A body: FinishDying's result, carried whole (zonestate.h). */
        q2_cre_motion_init(m);
        if (o->runtime) {
            /* Already past export 0; its runtime restores after the wake
             * without consuming another spawn's random draws. */
            m->spawn_initialized = true;
            m->resume_pending = here;
        } else {
            /* A version-1 history: no runtime, only the resting frame. */
            if (m->in_use) m->spawnflags |= Q2_SVFLAG_INUSE;
            q2_monster_corpse_detach(m);
            m->frame = (s16)o->frame;
            m->think = NULL;
        }
    }
}

void q2_zone_history_spawned(const q2_zone_history *h, const char *map,
                             q2_sim *sim, q2_creature_world *world)
{
    const q2_zone_population *p = find_map(h, map);
    u32 i;
    if (!world || !world->pop_ready || !sim || sim->multiplayer || !p ||
        p->identity != identity(&world->pop)) return;
    for (i = 0; i < world->set.count; i++) {
        q2_monster *m = &world->set.monsters[i];
        const q2_zone_object *o = find_object(p, Q2_ZONE_CREATURE, m->group, m->population_slot);
        if (!m->in_use || m->dead || m->corpse || !m->spawn_initialized || !o ||
            o->removed || o->dead || o->id != m->pop_class_id) continue;
        /* 0x8007D2FC..0x8007D304: `lhu` the decoded health, `sh` into +0x108
         * of the object SpawnGeneral returned -- over whatever export 0 set. */
        m->health = o->health;
    }
}

static s32 rebase_deadline(s32 value, s32 from, s32 to)
{
    s64 v;
    if (!value) return 0;
    v = (s64)value - from + to;
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v == 0 ? -1 : (s32)v;
}

bool q2_zone_apply_entity(const q2_zone_object *o, q2_sim *sim, u32 slot,
                           const q2_model_bank *zone_bank)
{
    q2_entity e, *dest;
    q2_gib_body gib;
    const q2_zone_runtime *r;
    static const q2_think_fn think[] = {
        NULL, q2_item_think, q2_item_shrink_think, q2_item_drop_think,
        q2_model_ent_think, q2_gib_think
    };
    if (!o || !o->runtime || !sim || slot >= sim->entities.count ||
        (o->kind != Q2_ZONE_ITEM && o->kind != Q2_ZONE_DYNAMIC) ||
        !q2_zone_runtime_valid(o->kind, o->runtime)) return false;
    r = o->runtime;
    e = r->entity.state;
    e.think = think[r->entity.think];
    e.model_bank = NULL; e.model_index = -1;
    e.def = e.kind == Q2_ENT_KIND_ITEM ? q2_item_find(sim->item_table, e.place_id) : NULL;
    if (e.model[0] && !q2_entity_resolve_model(&e, sim->item_bank) &&
        !q2_entity_resolve_model(&e, zone_bank) && r->entity.model_bound) return false;
    /* Resolving the model derives spawn defaults; keep the exact live pose. */
    memcpy(e.origin, r->entity.state.origin, sizeof(e.origin));
    memcpy(e.bounds_min, r->entity.state.bounds_min, sizeof(e.bounds_min));
    memcpy(e.bounds_max, r->entity.state.bounds_max, sizeof(e.bounds_max));
    e.respawn_at = rebase_deadline(e.respawn_at, r->sim_time, sim->level_time);
    e.node = sim->coll_ready ? q2_coll_find_node(&sim->coll, e.pos, -1, true) : -1;
    e.zone_serial = o->kind == Q2_ZONE_DYNAMIC ? o->slot : 0;
    e.population_group = o->kind == Q2_ZONE_ITEM ? (s32)o->group : -1;
    e.population_slot = o->kind == Q2_ZONE_ITEM ? o->slot : 0;
    dest = &sim->entities.ent[slot];
    *dest = e;
    if (r->entity.think == Q2_ZTHINK_DROP &&
        !q2_item_drop_restore(&sim->entities, slot, r->entity.velocity)) return false;
    if (r->entity.think == Q2_ZTHINK_GIB) {
        gib = r->entity.gib;
        gib.cell = sim->coll_primary_ready ? q2_coll_find_node(&sim->coll_primary, e.pos, -1, true) : -1;
        if (!q2_gib_body_restore(&sim->entities, slot, &gib)) return false;
    }
    o->runtime->materialized = true;
    return true;
}

const q2_zone_object *q2_zone_saved_entity(const q2_zone_history *h,
                                           const char *map, int zone, u32 slot)
{
    const q2_zone_population *p = find_map(h, map);
    u32 i;
    for (i = 0; p && i < p->count; i++) {
        const q2_zone_object *o = &p->object[i];
        if (o->zone == zone && !o->removed && o->runtime &&
            (o->kind == Q2_ZONE_ITEM || o->kind == Q2_ZONE_DYNAMIC) &&
            o->runtime->entity.source_slot == slot) return o;
    }
    return NULL;
}

void q2_zone_history_resume(q2_zone_history *h, const char *map, int zone,
                             q2_sim *sim, q2_creature_world *world,
                             q2_actor *actors, q2_zone_animation *animation,
                             u32 *phase_us)
{
    const q2_zone_population *p = find_map(h, map);
    u32 i, k;
    if (!sim || sim->multiplayer || !p || !p->runtime_present ||
        !sim->item_population_ready || p->identity != identity(&sim->item_population)) return;
    if (phase_us) *phase_us = p->ai_phase_us;
    for (i = 0; world && world->ready && i < world->set.count; i++) {
        q2_monster *m = &world->set.monsters[i];
        const q2_zone_object *o = find_object(p, Q2_ZONE_CREATURE, m->group, m->population_slot);
        if (!m->in_use || !o || o->removed || !o->runtime || o->id != m->pop_class_id) continue;
        if (!q2_zone_apply_monster(o->runtime, world, m)) {
            /* Its class is unavailable in this zone; retain the old snapshot. */
            m->in_use = m->population_active = false;
            Q2_WARN("zone: creature %u/%u deferred (move unavailable)", o->group, o->slot);
            continue;
        }
        if (animation) {
            animation[i] = o->runtime->creature.animation;
            animation[i].move = m->currentmove;
            animation[i].stamped = false;
        }
    }
    if (actors && world)
        for (i = 0; i < world->set.count; i++) q2_actor_from_monster(&actors[i], &world->set.monsters[i]);
    q2_level_state.sight_entity = q2_zone_ref_resolve(world, p->alerts[0]);
    q2_level_state.sound_entity = q2_zone_ref_resolve(world, p->alerts[1]);
    q2_level_state.sound2_entity = q2_zone_ref_resolve(world, p->alerts[2]);
    q2_level_state.sight_entity_framenum = p->alert_frame[0];
    q2_level_state.sound_entity_framenum = p->alert_frame[1];
    q2_level_state.sound2_entity_framenum = p->alert_frame[2];
    q2_projectiles_init(&sim->combat.projectiles);
    for (i = 0; i < p->count; i++) {
        q2_zone_object *o = &p->object[i];
        q2_projectile *shot;
        q2_monster *shooter;
        if (o->kind != Q2_ZONE_PROJECTILE || !o->runtime || o->removed) continue;
        o->runtime->materialized = false;
        if (o->runtime->projectile.state.node != Q2_PROJ_NODE_HELD && o->zone != zone &&
            (!sim->coll_primary_ready || q2_coll_find_node(&sim->coll_primary, o->pos, -1, true) < 0)) continue;
        k = o->zone == zone ? o->runtime->projectile.source_slot : Q2_PROJ_MAX;
        if (k >= Q2_PROJ_MAX || sim->combat.projectiles.p[k].in_use)
            for (k = 0; k < Q2_PROJ_MAX && sim->combat.projectiles.p[k].in_use; k++) { }
        if (k == Q2_PROJ_MAX) { Q2_WARN("zone: projectile deferred (pool capacity)"); continue; }
        shot = &sim->combat.projectiles.p[k];
        *shot = o->runtime->projectile.state;
        shot->shooter = NULL;
        shot->zone_serial = o->slot;
        shot->expires = rebase_deadline(shot->expires, o->runtime->sim_time, sim->level_time);
        if (shot->node != Q2_PROJ_NODE_HELD) {
            /* Projectile movement uses PrimaryColl. A SecondaryColl index is
             * a different cell even when its number is valid in both hulls;
             * restoring that hint can let the next move pass through a wall. */
            q2_collision *hull = sim->coll_primary_ready ? &sim->coll_primary
                                  : sim->coll_ready ? &sim->coll : NULL;
            shot->node = hull ? q2_coll_find_node(hull, shot->pos, -1, true)
                               : Q2_PROJ_NODE_UNKNOWN;
        }
        shooter = q2_zone_ref_resolve(world, o->runtime->projectile.shooter);
        if (shooter && actors && world && o->runtime->projectile.shooter.kind == Q2_ZREF_CREATURE)
            shot->shooter = &actors[shooter - world->set.monsters];
        sim->combat.projectiles.live++;
        o->runtime->materialized = true;
        o->zone = zone;
    }
}

/* ------------------------------------------------------------------------- */
/* OnLeavingZone's prelude: FinishDying and SaveCreatures' kill hooks         */
/* ------------------------------------------------------------------------- */
void q2_zone_leaving(q2_sim *sim, q2_creature_world *world)
{
    u32 i;
    if (!sim || sim->multiplayer) return;
    /*
     * FinishDying, 0x8007F630. The test is the edict's deadflag alone
     * (0x8007F680..0x8007F690: `lw +0x20`, `srl 22`, `andi 3`, `bne 2`), so a
     * creature whose `die` ran this very frame converts like one five frames
     * into its fall; a body already detached has no edict and is skipped
     * (0x8007F670..0x8007F678). `dead` is the port's other spelling of that
     * bit (monster.c keeps the two in step; q2_monster_damage sets it alone). The kill is already in the tally -- the port
     * counts `dead` bodies, which ProcessQuakeDeath (0x8007F698) would raise
     * here on the console -- and the frame is left for the store; the body
     * comes back on its clip's last frame, which the port spells as the move's
     * last frame (zonestate.h).
     */
    for (i = 0; world && world->ready && i < world->set.count; i++) {
        q2_monster *m = &world->set.monsters[i];
        if (!m->in_use || m->corpse || !(m->dead || m->deadflag == Q2_DEAD_DEAD)) continue;
        if (m->currentmove) m->frame = (s16)m->currentmove->last_frame;
        q2_monster_corpse_detach(m);    /* 0x8007F6A0..0x8007F6EC, as 0x8007F098 */
        m->svflags |= Q2_SVF_DEADMONSTER;
        m->next_think = 0;              /* the edict and its think are gone */
    }
    /*
     * SaveCreatures, 0x80056B24..0x80056B98: each live slot's class-table
     * +0x1C hook. Fragments (class 48) carry DestroyMe: KillModel, no store.
     * A pickup carries RememberMe, whose store (LoadStoreQuakePickup
     * 0x8005B4EC) refuses a +0x44 with bit 4 -- the dropped and the collected
     * -- and then kills the slot regardless (0x8007F910). Explosion models
     * (class 0) match no entry and are left alone, like the missile pool.
     */
    for (i = 0; i < sim->entities.count; i++) {
        q2_entity *e = &sim->entities.ent[i];
        if (!e->in_use) continue;
        if (e->kind == Q2_GIB_BOSS_CLASS || e->think == q2_gib_think ||
            (e->kind == Q2_ENT_KIND_ITEM && (e->flags & Q2_ITEM_TIMED)))
            q2_entity_remove(e);
    }
}
