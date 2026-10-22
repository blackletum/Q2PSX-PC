#include "save.h"

#include "mission.h"
#include "explosive.h"
#include "modelent.h"
#include "ai.h"
#include "creworld.h"
#include "random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#  define SAVE_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define SAVE_MKDIR(p) mkdir((p), 0777)
#endif

/* ------------------------------------------------------------------------- */
/* Chunk tags                                                                 */
/*                                                                            */
/* Four characters, low byte first, so a hex dump of the file reads them left  */
/* to right. A reader skips a tag it does not know, which is what lets a later */
/* version add state without invalidating an existing save.                   */
/* ------------------------------------------------------------------------- */
#define TAG(a, b, c, d) \
    ((u32)(u8)(a) | ((u32)(u8)(b) << 8) | ((u32)(u8)(c) << 16) | ((u32)(u8)(d) << 24))

#define TAG_HEAD TAG('H', 'E', 'A', 'D')   /* identity                        */
#define TAG_PLYR TAG('P', 'L', 'Y', 'R')   /* the player, mover state and all */
#define TAG_SIMS TAG('S', 'I', 'M', 'S')   /* the clock and the world's rules */
#define TAG_INVN TAG('I', 'N', 'V', 'N')   /* inventory                       */
#define TAG_CMBT TAG('C', 'M', 'B', 'T')   /* weapon gate and the generators  */
#define TAG_PROJ TAG('P', 'R', 'O', 'J')   /* projectiles in flight           */
#define TAG_EVNT TAG('E', 'V', 'N', 'T')   /* script flags                    */
#define TAG_EVIT TAG('E', 'V', 'I', 'T')   /* script item latches (v6)        */
#define TAG_EOBJ TAG('E', 'O', 'B', 'J')   /* permanent object changes        */
#define TAG_EVHS TAG('E', 'V', 'H', 'S')   /* previously visited maps        */
#define TAG_TRIG TAG('T', 'R', 'I', 'G')   /* trigger residency               */
#define TAG_ENTS TAG('E', 'N', 'T', 'S')   /* per-entity mutable state        */
#define TAG_ITEM TAG('I', 'T', 'E', 'M')   /* group order and stable item keys */
#define TAG_MISN TAG('M', 'I', 'S', 'N')   /* the mission tallies             */
#define TAG_LVLC TAG('L', 'V', 'L', 'C')   /* the level's live counters (v7)  */
#define TAG_HELP TAG('H', 'E', 'L', 'P')   /* the help computer's pair (v8)   */
#define TAG_BRKS TAG('B', 'R', 'K', 'S')   /* which panes have been shot      */
#define TAG_EXPL TAG('E', 'X', 'P', 'L')   /* shared destroyable-group state  */
#define TAG_MOVR TAG('M', 'O', 'V', 'R')   /* which doors are open, and where */
#define TAG_CRES TAG('C', 'R', 'E', 'S')   /* who is dead and where the rest are */
#define TAG_ROTR TAG('R', 'O', 'T', 'R')   /* rotating brush motion */
#define TAG_CAIS TAG('C', 'A', 'I', 'S')   /* creature behavior */
#define TAG_SKIL TAG('S', 'K', 'I', 'L')   /* difficulty before roster build */
#define TAG_RAND TAG('R', 'A', 'N', 'D')   /* shared gameplay random state */
#define TAG_VWPN TAG('V', 'W', 'P', 'N')   /* live weapon animation */
#define TAG_ERTS TAG('E', 'R', 'T', 'S')   /* resident script scheduler */
#define TAG_PRST TAG('P', 'R', 'S', 'T')   /* projectile flags/beam clock */
#define TAG_SETT TAG('S', 'E', 'T', 'T')   /* the menu settings               */

#define SAVE_FILE_HEADER 16                /* magic, version, size, crc       */
#define SETTINGS_MAGIC "Q2CF"
#define SETTINGS_VERSION 1u

/* A corrupt count must not become a gigabyte allocation. These are far above
 * anything a real map produces and far below anything that hurts. */
#define SAVE_MAX_EVENTS   (1u << 20)
#define SAVE_MAX_TRIGGERS (1u << 16)
#define SAVE_MAX_ENTITIES (1u << 16)
#define SAVE_MAX_ITEM_GROUPS (1u << 16)
#define SAVE_MAX_EVENT_MAPS 64u

/* These snapshots carry scalar state only. Runtime callbacks stay with the
 * rebuilt module, links use roster indices, and moves use module-relative
 * addresses. None of q2_monster's pointers are copied or written to disk. */
typedef struct save_ai_monster {
    q2_monster state;
    s32 refs[5];               /* enemy, oldenemy, goal, movetarget, owner */
    char move_module[13];
    u32 move_offset;
    u8 has_move, thinking;
} save_ai_monster;

struct q2_save_ai {
    save_ai_monster *monsters;
    u32 count, corner_count, phase_us;
    q2_monster sight;
    q2_level level;
    s32 level_refs[4];
    q2_trail_state trail;
};

/* One field list keeps capture, portable encoding and apply in agreement. */
#define AI_SCALARS(X) \
    X(s16, hit_radius) X(s16, hit_height) X(s16, view_height) \
    X(u32, spawnflags) X(u16, flags) X(u8, solid) X(u8, movetype) \
    X(u8, deadflag) X(u8, takedamage) X(s16, mass) X(u8, skinnum) \
    X(u8, class_id) X(u8, class_byte) X(u8, pop_class_id) X(u8, start_kind) \
    X(s16, model_ext2) X(u16, group) X(u32, svflags) X(u8, client) \
    X(s16, health) X(s16, max_health) X(s16, gib_health) X(s16, frame) \
    X(s32, next_think) X(s32, pain_debounce) X(u8, hurt) \
    X(s32, show_hostile) X(s32, teleport_time) X(s16, targetname) \
    X(s16, target) X(s16, combattarget) X(s16, yaw_speed) X(s16, ideal_yaw) \
    X(u32, aiflags) X(s32, bigturn_threshold) X(s32, pausetime) \
    X(s32, attack_finished) X(s32, search_time) X(s32, trail_time) \
    X(s32, idle_time) X(u16, nextframe) X(u8, attack_state) X(u8, lefty) \
    X(u8, blindfire) X(u8, speed_scale) X(u8, in_use) X(u8, dead) \
    X(u8, gibbed) X(u8, corpse) X(u8, corpse_was_class) X(u8, dissolve_arm) \
    X(s16, dissolve) X(u8, on_ground) X(s32, ground_height)
#define AI_ARRAYS(X) \
    X(s32, pos, 3) X(s32, velocity, 3) X(s16, angles, 3) \
    X(s16, mins, 3) X(s16, maxs, 3) X(s32, blind_target, 3) \
    X(s32, saved_goal, 3) X(s32, last_sighting, 3) X(u8, effect, 6)

#define VW_SCALARS(X) \
    X(s16, scale) X(s16, fade) X(s16, light_selector) \
    X(s32, weapon) X(s32, pending) X(u32, state) X(u32, frame) \
    X(s32, total) X(s32, left) X(s16, aim_assist) \
    X(bool, fire_latch) X(bool, refire) X(s16, switch_ticks) \
    X(s16, event) X(bool, event_pending) X(s16, hyper_ramp) \
    X(bool, quad_active) X(bool, quad_sound) \
    X(s16, anim_pos) X(s16, anim_end) X(u16, anim_flags) \
    X(u32, keys_played) X(u32, anims_started) X(u32, fires_started) \
    X(s32, spin_accum) X(s32, spin_rate) X(s32, last_fire_frame) \
    X(bool, cook) X(s16, hand_prev_anim) X(s32, hand_cook_ticks) \
    X(bool, hand_release) X(u32, frame_fires) X(s16, frame_sound)
#define VW_ARRAYS(X) \
    X(u8, glow, 3) X(s16, from_t, 3) X(s16, from_r, 3) \
    X(s16, cur_t, 3) X(s16, cur_r, 3)

static void free_ai(struct q2_save_ai *ai)
{
    if (ai) free(ai->monsters);
    free(ai);
}

/* ------------------------------------------------------------------------- */
/* CRC32                                                                      */
/*                                                                            */
/* The chunk sizes already catch truncation. This catches the other half — a   */
/* file that is the right length and wrong in the middle — which for a save is */
/* the failure worth having, because a silently corrupt restore looks like a   */
/* game bug rather than a bad file.                                            */
/* ------------------------------------------------------------------------- */
static u32 crc32_of(const u8 *p, size_t n)
{
    u32 crc = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(s32)(crc & 1u)));
    }
    return ~crc;
}

/* ------------------------------------------------------------------------- */
/* A growable little-endian writer                                            */
/* ------------------------------------------------------------------------- */
typedef struct wbuf {
    u8    *p;
    size_t len, cap;
    bool   bad;      /* an allocation failed; every later write is a no-op */
} wbuf;

static void w_free(wbuf *w)
{
    free(w->p);
    w->p = NULL;
    w->len = w->cap = 0;
}

static bool w_need(wbuf *w, size_t n)
{
    size_t want;
    u8 *bigger;

    if (w->bad)
        return false;
    if (w->len + n <= w->cap)
        return true;

    want = w->cap ? w->cap : 1024;
    while (want < w->len + n)
        want *= 2;

    bigger = (u8 *)realloc(w->p, want);
    if (!bigger) {
        w->bad = true;
        return false;
    }
    w->p   = bigger;
    w->cap = want;
    return true;
}

static void w_bytes(wbuf *w, const void *src, size_t n)
{
    if (!w_need(w, n))
        return;
    memcpy(w->p + w->len, src, n);
    w->len += n;
}

static void w_u8(wbuf *w, u8 v)
{
    if (!w_need(w, 1))
        return;
    w->p[w->len++] = v;
}

static void w_u16(wbuf *w, u16 v)
{
    w_u8(w, (u8)v);
    w_u8(w, (u8)(v >> 8));
}

static void w_u32(wbuf *w, u32 v)
{
    w_u8(w, (u8)v);
    w_u8(w, (u8)(v >> 8));
    w_u8(w, (u8)(v >> 16));
    w_u8(w, (u8)(v >> 24));
}

static void w_s16(wbuf *w, s16 v) { w_u16(w, (u16)v); }
static void w_s32(wbuf *w, s32 v) { w_u32(w, (u32)v); }
static void w_bool(wbuf *w, bool v) { w_u8(w, v ? 1u : 0u); }

/* A fixed-width, NUL-padded string. Fixed width so the field can be read
 * without a length prefix and so a shorter name cannot shift what follows. */
static void w_str(wbuf *w, const char *s, size_t width)
{
    size_t i, n = s ? strlen(s) : 0;

    if (n > width - 1)
        n = width - 1;
    for (i = 0; i < width; i++)
        w_u8(w, i < n ? (u8)s[i] : 0u);
}

static void w_s16v(wbuf *w, const s16 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        w_s16(w, v[i]);
}

static void w_s32v(wbuf *w, const s32 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        w_s32(w, v[i]);
}

/* Open a chunk, returning where its size field sits so w_chunk_end can patch
 * it. Writing the size afterwards rather than counting it first is what keeps
 * every chunk writer a straight run of fields. */
static size_t w_chunk_begin(wbuf *w, u32 tag)
{
    size_t at;
    w_u32(w, tag);
    at = w->len;
    w_u32(w, 0);
    return at;
}

static void w_chunk_end(wbuf *w, size_t size_at)
{
    u32 size;

    if (w->bad || w->len < size_at + 4)
        return;
    size = (u32)(w->len - (size_at + 4));
    w->p[size_at + 0] = (u8)size;
    w->p[size_at + 1] = (u8)(size >> 8);
    w->p[size_at + 2] = (u8)(size >> 16);
    w->p[size_at + 3] = (u8)(size >> 24);
}

/* ------------------------------------------------------------------------- */
/* The matching reader                                                        */
/* ------------------------------------------------------------------------- */
typedef struct rbuf {
    const u8 *p;
    size_t    len, at;
    bool      bad;   /* a read ran off the end; every later read yields zero */
} rbuf;

static bool r_need(rbuf *r, size_t n)
{
    if (r->bad)
        return false;
    if (r->at + n > r->len) {
        r->bad = true;
        return false;
    }
    return true;
}

static u8 r_u8(rbuf *r)
{
    if (!r_need(r, 1))
        return 0;
    return r->p[r->at++];
}

static u16 r_u16(rbuf *r)
{
    u16 lo = r_u8(r);
    return (u16)(lo | ((u16)r_u8(r) << 8));
}

static u32 r_u32(rbuf *r)
{
    u32 v = r_u16(r);
    return v | ((u32)r_u16(r) << 16);
}

static s16 r_s16(rbuf *r) { return (s16)r_u16(r); }
static s32 r_s32(rbuf *r) { return (s32)r_u32(r); }
static bool r_bool(rbuf *r) { return r_u8(r) != 0; }

static void r_str(rbuf *r, char *out, size_t width)
{
    size_t i;

    for (i = 0; i < width; i++) {
        u8 c = r_u8(r);
        if (out && i < width)
            out[i] = (char)c;
    }
    if (out && width)
        out[width - 1] = '\0';
}

static void r_s16v(rbuf *r, s16 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        v[i] = r_s16(r);
}

static void r_s32v(rbuf *r, s32 *v, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        v[i] = r_s32(r);
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                   */
/* ------------------------------------------------------------------------- */
void q2_save_free(q2_save *s)
{
    if (!s)
        return;
    free(s->event_flags);
    free(s->event_item_flags);
    free(s->event_object_flags);
    q2_event_store_free(&s->event_history);
    free(s->trigger_inside);
    free(s->entities);
    free(s->item_group_order);
    free(s->item_keys);
    free(s->breakables);
    free(s->explosives);
    free(s->movers);
    free(s->rotators);
    free(s->creatures);
    free_ai(s->ai);
    memset(s, 0, sizeof(*s));
}

static bool valid_event_runtime(const q2_save_event_runtime *e)
{
    u32 i, j;
    if (e->script_size > SAVE_MAX_EVENTS ||
        e->record_count > e->script_size / 4 ||
        e->pending_count > Q2_EVENT_RT_PENDING_MAX) return false;
    for (i = 0; i < e->pending_count; i++)
        if (e->pending[i] >= e->script_size) return false;
    for (i = 0; i < Q2_EVENT_RT_TIMER_MAX; i++) {
        const q2_event_timer *t = &e->timers[i];
        if (!t->item) continue;
        if (t->item >= e->script_size || t->offset >= e->script_size ||
            t->period < 0 || t->next_item <= t->timer_index) return false;
        for (j = 0; j < i; j++) if (e->timers[j].item == t->item) return false;
    }
    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        if (!e->waits[i].in_use) continue;
        if (e->waits[i].offset >= e->script_size) return false;
        for (j = 0; j < i; j++)
            if (e->waits[j].in_use && e->waits[j].offset == e->waits[i].offset)
                return false;
    }
    return true;
}

static bool event_runtime_matches(const q2_save_event_runtime *e,
                                   const q2_event_rt *rt)
{
    u32 i, j, k;
    q2_event_record rec;
    q2_event_item item;
    if (!valid_event_runtime(e) || !rt->events.data ||
        e->script_size != rt->events.size || e->record_count != rt->record_count ||
        e->script_crc != crc32_of(rt->events.data, rt->events.size)) return false;
    for (i = 0; i < e->pending_count; i++)
        if (!q2_events_record_at(&rt->events, e->pending[i], &rec)) return false;
    for (i = 0; i < Q2_EVENT_RT_TIMER_MAX; i++) {
        const q2_event_timer *t = &e->timers[i];
        if (!t->item) continue;
        if (!q2_events_record_at(&rt->events, t->offset, &rec) ||
            t->next_item > rec.n_items ||
            !q2_events_get_item(&rt->events, &rec, t->timer_index, &item) ||
            item.offset != t->item || item.opcode != Q2_EVOP_CALL) return false;
    }
    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        bool found = false;
        if (!e->waits[i].in_use) continue;
        for (j = 0; j < rt->record_count && !found; j++) {
            if (!q2_events_record_at(&rt->events, rt->offsets[j], &rec)) return false;
            for (k = 0; k < rec.n_items; k++) {
                if (!q2_events_get_item(&rt->events, &rec, k, &item)) return false;
                if (item.offset == e->waits[i].offset &&
                    item.opcode == Q2_EVOP_WAIT && item.len == 12) { found = true; break; }
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool valid_event_carry(const q2_event_carry *c)
{
    return c->size <= Q2_EVENT_CARRY_MAX &&
           c->record_count <= c->size / 4;
}

static q2_result copy_event_history(q2_event_store *out,
                                     const q2_event_store *in)
{
    q2_event_store copy = {0};
    u32 i;

    if (!out || !in || in->count > SAVE_MAX_EVENT_MAPS ||
        (in->count && !in->block))
        return Q2_ERR_INVALID_ARG;
    for (i = 0; i < in->count; i++) {
        const q2_event_block *b = &in->block[i];
        q2_event_carry *dst;

        if (!b->map[0] || !memchr(b->map, 0, sizeof(b->map)) ||
            !valid_event_carry(&b->carry) ||
            q2_event_store_find(&copy, b->map)) {
            q2_event_store_free(&copy);
            return Q2_ERR_BAD_FORMAT;
        }
        dst = q2_event_store_open(&copy, b->map);
        if (!dst) {
            q2_event_store_free(&copy);
            return Q2_ERR_NO_MEMORY;
        }
        *dst = b->carry;
    }
    q2_event_store_free(out);
    *out = copy;
    return Q2_OK;
}

q2_result q2_save_capture_event_history(q2_save *s, const q2_event_store *store)
{
    q2_result rc;
    if (!s) return Q2_ERR_INVALID_ARG;
    rc = copy_event_history(&s->event_history, store);
    if (rc == Q2_OK) s->event_history_present = true;
    return rc;
}

q2_result q2_save_restore_event_history(const q2_save *s, q2_event_store *store)
{
    const q2_event_store empty = {0};
    if (!s) return Q2_ERR_INVALID_ARG;
    return copy_event_history(store, s->event_history_present
                                       ? &s->event_history : &empty);
}

q2_result q2_save_event_carry(const q2_save *s, const q2_event_rt *rt,
                              q2_event_carry *out)
{
    u32 i;
    if (!s || !rt || !out) return Q2_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s->event_count && !s->event_item_count) return Q2_OK;
    if (!s->event_flags || s->event_count != rt->record_count ||
        (s->event_item_count && (!s->event_item_flags ||
                                s->event_item_count != rt->item_flags_size)) ||
        (s->event_object_count && (!s->event_object_flags ||
                                  s->event_object_count != s->event_item_count)))
        return Q2_ERR_BAD_FORMAT;
    q2_event_rt_carry_out(rt, out);
    if (!out->size) return Q2_ERR_BAD_FORMAT;
    memcpy(out->record_flags, s->event_flags, s->event_count);
    memset(out->item_flags, 0, sizeof(out->item_flags));
    memset(out->destroyed_items, 0, sizeof(out->destroyed_items));
    if (s->event_item_count)
        memcpy(out->item_flags, s->event_item_flags, s->event_item_count);
    if (s->event_object_count)
        memcpy(out->destroyed_items, s->event_object_flags, s->event_object_count);
    for (i = 0; i < s->explosive_count; i++)
        if (s->explosives[i].destroyed && s->explosives[i].item_offset < out->size)
            out->destroyed_items[s->explosives[i].item_offset] = 1;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Capture                                                                    */
/* ------------------------------------------------------------------------- */
/*
 * The think pointer, as something a file can hold. Only the two the item path
 * installs are named; anything else is recorded as OTHER, which the restore
 * leaves alone rather than replacing with a guess.
 */
static q2_save_think think_id(q2_think_fn fn)
{
    if (!fn)
        return Q2_SAVE_THINK_NONE;
    if (fn == q2_item_think)
        return Q2_SAVE_THINK_ITEM;
    if (fn == q2_item_shrink_think)
        return Q2_SAVE_THINK_SHRINK;
    return Q2_SAVE_THINK_OTHER;
}

static u8 taken_mask(const q2_entity *e)
{
    u8 mask = 0;
    int i;

    for (i = 0; i < Q2_MAX_PLAYERS && i < 8; i++)
        if (e->taken[i])
            mask |= (u8)(1u << i);
    return mask;
}

q2_result q2_save_capture(q2_save *out, const q2_sim *sim,
                          const q2_inventory *inv,
                          const char *serial, const char *map, s32 zone)
{
    if (!out || !sim || !map)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (serial)
        snprintf(out->serial, sizeof(out->serial), "%s", serial);
    snprintf(out->map, sizeof(out->map), "%s", map);
    out->zone      = zone;
    out->timestamp = (u32)time(NULL);
    out->skill_present = true;
    out->skill = q2_cre_skill();

    /* --- the player, whole ---------------------------------------------- */
    out->player = sim->player[0];

    /* --- the clock and the rules ----------------------------------------- */
    out->level_time          = sim->level_time;
    out->tick_count          = sim->tick_count;
    out->dt_accum            = sim->dt_accum;
    out->dt_per_field        = sim->dt_per_field;
    out->gravity             = sim->gravity;
    out->env_flags           = sim->env_flags;
    out->cheats              = sim->cheats;
    out->current_node        = sim->current_node;
    out->multiplayer         = sim->multiplayer ? 1u : 0u;
    out->full_basis_movement = sim->full_basis_movement ? 1u : 0u;
    out->no_fall_damage      = sim->no_fall_damage ? 1u : 0u;

    /* --- combat ----------------------------------------------------------- */
    out->inventory        = inv ? *inv : sim->combat.inv;
    out->weapon_id        = sim->combat.weapon_id;
    out->next_fire        = sim->combat.next_fire;
    out->kick[0]          = sim->combat.kick[0];
    out->kick[1]          = sim->combat.kick[1];
    out->kick[2]          = sim->combat.kick[2];
    out->chaingun_bullets = sim->combat.chaingun_bullets;
    out->rng_state        = sim->combat.rng.state;
    out->fx_rng_state     = sim->fx_rng.state;
    out->shot_serial      = sim->combat.shot_serial;
    out->shot_fired       = sim->combat.last_shot.fired;
    out->shot_dry         = sim->combat.last_shot.dry;
    out->shot_sound       = sim->combat.last_shot.sound;
    out->game_rng_present = true;
    out->game_rng_state   = q2_random_state();

    memcpy(out->proj, sim->combat.projectiles.p, sizeof(out->proj));
    out->projectile_runtime_present = true;

    if (sim->events_ready && sim->event_rt.events.data) {
        const q2_event_rt *rt = &sim->event_rt;
        q2_save_event_runtime *e = &out->event_runtime;
        e->script_size = rt->events.size;
        e->script_crc = crc32_of(rt->events.data, rt->events.size);
        e->record_count = rt->record_count;
        e->clock = rt->clock; e->tick = rt->tick;
        e->pending_count = rt->pending_count;
        memcpy(e->pending, rt->pending, sizeof(e->pending));
        memcpy(e->timers, rt->deferred, sizeof(e->timers));
        memcpy(e->waits, rt->wait, sizeof(e->waits));
        out->event_runtime_present = true;
    }

    /*
     * --- script state -----------------------------------------------------
     *
     * A save that restores position and inventory but not which triggers have
     * fired drops the player into a level where every door they opened is shut
     * again — so this is copied, not skipped.
     */
    if (sim->events_ready && sim->event_rt.record_count > 0 &&
        sim->event_rt.flags) {
        u32 n = sim->event_rt.record_count;

        out->event_flags = (u8 *)malloc(n);
        if (!out->event_flags) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        memcpy(out->event_flags, sim->event_rt.flags, n);
        out->event_count = n;
    }

    /*
     * And the ITEM latches, which are as much "what has fired" as the record
     * bytes are: a one-shot item retires itself (0x80027498) inside a record
     * that may run again. Without them a record an ENABLE re-armed before the
     * save — LAB 0x6d4, three one-shot items — replays all three on load.
     */
    if (sim->events_ready && sim->event_rt.item_flags &&
        sim->event_rt.item_flags_size > 0) {
        u32 n = sim->event_rt.item_flags_size;

        out->event_item_flags = (u8 *)malloc(n);
        if (!out->event_item_flags) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        memcpy(out->event_item_flags, sim->event_rt.item_flags, n);
        out->event_item_count = n;
        if (sim->event_rt.object_flags) {
            out->event_object_flags = (u8 *)malloc(n);
            if (!out->event_object_flags) {
                q2_save_free(out);
                return Q2_ERR_NO_MEMORY;
            }
            memcpy(out->event_object_flags, sim->event_rt.object_flags, n);
            out->event_object_count = n;
        }
    }

    /*
     * --- trigger residency -------------------------------------------------
     *
     * A volume fires on ENTRY. Restoring into one with its bit clear re-fires
     * it, so a save made standing on a teleport pad would teleport on load.
     */
    if (sim->trigger_inside && sim->trigger_capacity > 0) {
        u32 n = sim->trigger_capacity;

        out->trigger_inside = (u8 *)malloc(n);
        if (!out->trigger_inside) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }

        memcpy(out->trigger_inside, sim->trigger_inside, n);
        out->trigger_count = n;
    }

    /* --- the breakables ---------------------------------------------------- */
    if (sim->breakable_count > 0) {
        u32 n = sim->breakable_count, i;

        out->breakables = (q2_save_breakable *)calloc(n, sizeof(*out->breakables));
        if (!out->breakables) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }
        out->breakable_count = n;

        for (i = 0; i < n; i++) {
            out->breakables[i].scene_node = sim->breakable[i].scene_node;
            out->breakables[i].health     = sim->breakable[i].health;
            out->breakables[i].broken     = sim->breakable[i].broken ? 1u : 0u;
        }
    }

    if (sim->explosives && sim->explosives->count) {
        u32 i, n = sim->explosives->count;
        out->explosives = calloc(n, sizeof(*out->explosives));
        if (!out->explosives) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }
        out->explosive_count = n;
        for (i = 0; i < n; i++) {
            const q2_explosive *e = &sim->explosives->items[i];
            out->explosives[i].item_offset = e->item_offset;
            out->explosives[i].health = e->health;
            out->explosives[i].destroyed = e->destroyed;
        }
    }

    /* --- the entity set ---------------------------------------------------- */
    if (sim->entities.count > 0 && sim->entities.ent) {
        u32 n = sim->entities.count;
        u32 i;

        out->entities = (q2_save_entity *)calloc(n, sizeof(q2_save_entity));
        if (!out->entities) {
            q2_save_free(out);
            return Q2_ERR_NO_MEMORY;
        }
        out->entity_count = n;

        for (i = 0; i < n; i++) {
            const q2_entity *e = &sim->entities.ent[i];
            q2_save_entity  *d = &out->entities[i];

            d->place_id   = e->place_id;
            d->in_use     = e->in_use ? 1u : 0u;
            d->hidden     = e->hidden ? 1u : 0u;
            d->taken      = taken_mask(e);
            d->think      = (u8)think_id(e->think);
            d->scale      = e->scale;
            d->health     = e->health;
            d->frame      = e->frame;
            d->spin       = e->angles[1];
            d->remove_in  = e->remove_in;
            d->respawn_at = e->respawn_at;
            d->pos[0]     = e->pos[0];
            d->pos[1]     = e->pos[1];
            d->pos[2]     = e->pos[2];

            /* These are transient presentation, as the save contract states.
             * Their callbacks/models are not Population places and cannot be
             * rebuilt by index. Keeping them live made a mid-explosion save
             * fail to load. Keep the slot empty so later item keys still map. */
            if (e->kind == Q2_ENT_KIND_MODEL || e->think == q2_gib_think)
                d->in_use = 0;
        }
    }

    /*
     * --- Population group lifetime and stable entity identity ------------
     *
     * The order is not reducible to a bitmap: BASE1 can call ShotgunRoom and
     * LiftRoom in either order, and each appends/reuses slots in that order.
     * The keys are needed for the reuse half — a pickup collected before the
     * call leaves a hole a deferred place can occupy.
     */
    if (sim->item_population_ready && sim->item_population.group_count > 0 &&
        sim->item_group_run && sim->item_group_order) {
        u32 n = sim->item_population.group_count;
        u32 i;

        out->item_state_present = true;
        out->item_population_group_count = n;
        out->item_group_order_count = sim->item_group_order_count;

        if (out->item_group_order_count > n) {
            q2_save_free(out);
            return Q2_ERR_BAD_FORMAT;
        }
        if (out->item_group_order_count) {
            out->item_group_order = (u32 *)malloc(
                (size_t)out->item_group_order_count * sizeof(u32));
            if (!out->item_group_order) {
                q2_save_free(out);
                return Q2_ERR_NO_MEMORY;
            }
            memcpy(out->item_group_order, sim->item_group_order,
                   (size_t)out->item_group_order_count * sizeof(u32));
        }

        out->item_key_count = out->entity_count;
        if (out->item_key_count) {
            out->item_keys = (q2_save_item_key *)calloc(
                out->item_key_count, sizeof(*out->item_keys));
            if (!out->item_keys) {
                q2_save_free(out);
                return Q2_ERR_NO_MEMORY;
            }

            for (i = 0; i < out->item_key_count; i++) {
                out->item_keys[i].group = sim->entities.ent[i].population_group;
                out->item_keys[i].slot  = sim->entities.ent[i].population_slot;
            }
        }
    }

    q2_save_default_label(out, out->label, (u32)sizeof(out->label));
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Apply                                                                      */
/* ------------------------------------------------------------------------- */
static q2_result rebuild_saved_item_roster(const q2_save *s, q2_sim *sim)
{
    q2_sim stage;
    q2_entity_set rebuilt;
    u8 *seen = NULL;
    u8 *used = NULL;
    u32 capacity, i;
    q2_result result = Q2_ERR_BAD_FORMAT;

    if (!s->item_state_present) {
        /* A Population-backed version-5 save without ITEM has lost the exact
         * startup/CREBATCH history needed to identify reused entity slots.
         * Refuse the whole restore; applying ENTS by index would silently put
         * one item's state on another. Utility sims without Population remain
         * valid because they have no such history to preserve. */
        if (sim->item_population_ready &&
            sim->item_population.group_count > 0) {
            Q2_ERROR("a version-%d+ save for a Population map has no ITEM "
                     "chunk", Q2_SAVE_VERSION_OLDEST);
            return Q2_ERR_BAD_FORMAT;
        }
        return Q2_OK;
    }

    if (!sim->item_population_ready || !sim->item_group_run ||
        !sim->item_group_order ||
        sim->entities.count > SAVE_MAX_ENTITIES ||
        (sim->entities.count && !sim->entities.ent) ||
        s->item_population_group_count != sim->item_population.group_count ||
        s->item_group_order_count > s->item_population_group_count ||
        s->item_key_count != s->entity_count ||
        (s->entity_count && (!s->entities || !s->item_keys)))
        return Q2_ERR_BAD_FORMAT;

    seen = (u8 *)calloc(s->item_population_group_count ?
                        s->item_population_group_count : 1, 1);
    if (!seen)
        return Q2_ERR_NO_MEMORY;

    /* The fresh load's startup selection must be an exact prefix. Everything
     * after it is a CREBATCH first-run, in the order the allocator saw it. */
    if (sim->item_group_order_count > s->item_group_order_count)
        goto done;
    for (i = 0; i < s->item_group_order_count; i++) {
        u32 gi = s->item_group_order[i];

        if (gi >= s->item_population_group_count || seen[gi])
            goto done;
        seen[gi] = 1;
        if (i < sim->item_group_order_count &&
            sim->item_group_order[i] != gi)
            goto done;
    }

    /* Pre-size the staged set for the full logical roster. Runtime activation
     * may reuse holes; the fresh stage has none, so this is deliberately an
     * upper bound and prevents a partial replay on realloc failure. */
    capacity = sim->entities.count;
    for (i = sim->item_group_order_count;
         i < s->item_group_order_count; i++) {
        q2_pop_group g;
        u32 slot;

        if (!q2_pop_get_group(&sim->item_population,
                              s->item_group_order[i], &g))
            goto done;
        for (slot = 0; ; slot++) {
            q2_pop_place place;

            if (!q2_pop_get_place(&sim->item_population, &g, slot, &place))
                break;
            if (!q2_item_find(sim->item_table, (s32)place.id))
                continue;
            if (capacity >= SAVE_MAX_ENTITIES)
                goto done;
            capacity++;
        }
    }

    stage = *sim;
    memset(&stage.entities, 0, sizeof(stage.entities));
    stage.item_group_run = NULL;
    stage.item_group_order = NULL;

    if (capacity) {
        stage.entities.ent = (q2_entity *)calloc(capacity,
                                                  sizeof(q2_entity));
        if (!stage.entities.ent) {
            result = Q2_ERR_NO_MEMORY;
            goto done;
        }
        if (sim->entities.count)
            memcpy(stage.entities.ent, sim->entities.ent,
                   (size_t)sim->entities.count * sizeof(q2_entity));
        stage.entities.count    = sim->entities.count;
        stage.entities.capacity = capacity;
    }

    stage.item_group_run = (u8 *)malloc(
        s->item_population_group_count ? s->item_population_group_count : 1);
    stage.item_group_order = (u32 *)calloc(
        s->item_population_group_count ? s->item_population_group_count : 1,
        sizeof(u32));
    if (!stage.item_group_run || !stage.item_group_order) {
        result = Q2_ERR_NO_MEMORY;
        goto stage_done;
    }
    memcpy(stage.item_group_run, sim->item_group_run,
           s->item_population_group_count);
    if (sim->item_group_order_count)
        memcpy(stage.item_group_order, sim->item_group_order,
               (size_t)sim->item_group_order_count * sizeof(u32));

    for (i = sim->item_group_order_count;
         i < s->item_group_order_count; i++) {
        q2_pop_group g;
        u32 before = stage.item_group_order_count;
        u32 gi = s->item_group_order[i];

        if (!q2_pop_get_group(&stage.item_population, gi, &g))
            goto stage_done;
        (void)q2_sim_activate_item_group(&stage, g.name);
        if (!stage.item_group_run[gi] ||
            stage.item_group_order_count != before + 1 ||
            stage.item_group_order[before] != gi)
            goto stage_done;
    }

    memset(&rebuilt, 0, sizeof(rebuilt));
    if (s->entity_count) {
        rebuilt.ent = (q2_entity *)calloc(s->entity_count,
                                          sizeof(q2_entity));
        used = (u8 *)calloc(stage.entities.count ? stage.entities.count : 1,
                            1);
        if (!rebuilt.ent || !used) {
            result = Q2_ERR_NO_MEMORY;
            goto rebuilt_done;
        }
        rebuilt.count = rebuilt.capacity = s->entity_count;
    }

    for (i = 0; i < s->entity_count; i++) {
        const q2_save_item_key *key = &s->item_keys[i];
        u32 j;

        q2_entity_init(&rebuilt.ent[i]);
        if (key->group >= 0) {
            for (j = 0; j < stage.entities.count; j++) {
                const q2_entity *e = &stage.entities.ent[j];

                if (used[j] || !e->in_use)
                    continue;
                if (e->population_group != key->group ||
                    e->population_slot != key->slot)
                    continue;
                rebuilt.ent[i] = *e;
                used[j] = 1;
                break;
            }
            if (j == stage.entities.count)
                goto rebuilt_done;
        } else if (s->entities[i].in_use) {
            /* Non-Population entities retain the legacy by-index rule. They
             * are not part of CREBATCH reconstruction, but rejecting a changed
             * slot is safer than attaching their state to an item. */
            if (i >= stage.entities.count || used[i] ||
                !stage.entities.ent[i].in_use ||
                stage.entities.ent[i].population_group >= 0)
                goto rebuilt_done;
            rebuilt.ent[i] = stage.entities.ent[i];
            used[i] = 1;
        }

        if (s->entities[i].in_use &&
            rebuilt.ent[i].place_id != s->entities[i].place_id)
            goto rebuilt_done;
    }

    /* All validation happened against private storage. Only now replace the
     * live roster and its two group-lifetime shadows. */
    q2_entity_set_free(&sim->entities);
    sim->entities = rebuilt;
    memset(&rebuilt, 0, sizeof(rebuilt));
    memcpy(sim->item_group_run, stage.item_group_run,
           s->item_population_group_count);
    if (stage.item_group_order_count)
        memcpy(sim->item_group_order, stage.item_group_order,
               (size_t)stage.item_group_order_count * sizeof(u32));
    sim->item_group_order_count = stage.item_group_order_count;
    result = Q2_OK;

rebuilt_done:
    q2_entity_set_free(&rebuilt);
    free(used);
stage_done:
    q2_entity_set_free(&stage.entities);
    free(stage.item_group_run);
    free(stage.item_group_order);
done:
    free(seen);
    return result;
}

q2_result q2_save_apply(const q2_save *s, q2_sim *sim, q2_inventory *inv,
                        const char *serial, const char *map)
{
    q2_result rebuild;

    if (!s || !sim)
        return Q2_ERR_INVALID_ARG;

    /* A save from a different release would restore coordinates into a
     * different map's geometry, because the level table and script offsets are
     * per-build. Refusing is the only safe answer. */
    if (serial && s->serial[0] && strcmp(serial, s->serial) != 0) {
        Q2_ERROR("save is for build %s, this disc is %s", s->serial, serial);
        return Q2_ERR_UNSUPPORTED;
    }

    /* The caller loads the map; this only restores into it. Applying a save to
     * the wrong map would put the player inside geometry. */
    if (map && strcmp(map, s->map) != 0) {
        Q2_ERROR("save is for map %s, but %s is loaded", s->map, map);
        return Q2_ERR_INVALID_ARG;
    }

    if (s->event_runtime_present &&
        (!sim->events_ready || !event_runtime_matches(&s->event_runtime, &sim->event_rt)))
        return Q2_ERR_BAD_FORMAT;

    rebuild = rebuild_saved_item_roster(s, sim);
    if (rebuild != Q2_OK)
        return rebuild;

    /*
     * The entity set is rebuilt by the caller's attach sequence, so its size is
     * a check on whether the same map data came back. A mismatch means the save
     * and the map disagree and applying per-index state would put one item's
     * "collected" flag on another.
     */
    if (s->entity_count && sim->entities.count &&
        s->entity_count != sim->entities.count) {
        Q2_ERROR("save has %u entities, this map spawned %u",
                 s->entity_count, sim->entities.count);
        return Q2_ERR_BAD_FORMAT;
    }

    /*
     * And the per-slot check, run in full BEFORE anything is written.
     *
     * A live slot whose place id does not match is a map whose population has
     * moved under the save. Catching that halfway through the loop would leave
     * the simulation half restored, which is worse than either outcome — so
     * every entity is validated first and the apply is all or nothing.
     */
    if (s->entities && sim->entities.ent) {
        u32 n = s->entity_count;
        u32 i;

        if (n > sim->entities.count)
            n = sim->entities.count;

        for (i = 0; i < n; i++) {
            if (!s->entities[i].in_use)
                continue;   /* the record was cleared when it was collected */
            if (sim->entities.ent[i].place_id != s->entities[i].place_id) {
                Q2_ERROR("save entity %u is place %u, the map spawned %u",
                         i, s->entities[i].place_id,
                         sim->entities.ent[i].place_id);
                return Q2_ERR_BAD_FORMAT;
            }
        }
    }

    /* --- the player ------------------------------------------------------- */
    sim->player[0] = s->player;

    /* --- the clock and the rules ------------------------------------------ */
    sim->level_time          = s->level_time;
    sim->tick_count          = s->tick_count;
    sim->dt_accum            = s->dt_accum;
    sim->gravity             = s->gravity;
    sim->env_flags           = s->env_flags;
    sim->cheats              = s->cheats;
    sim->current_node        = s->current_node;
    sim->multiplayer         = s->multiplayer != 0;
    sim->full_basis_movement = s->full_basis_movement != 0;
    sim->no_fall_damage      = s->no_fall_damage != 0;

    /*
     * dt_per_field is the build's field rate, adjusted by the GAME SPEED
     * slider. A save carries it so a game saved under a modified speed resumes
     * at that speed; zero would stall the clock, so a bad value is ignored
     * rather than honoured.
     */
    if (s->dt_per_field > 0)
        sim->dt_per_field = s->dt_per_field;

    /* --- combat ------------------------------------------------------------ */
    sim->combat.inv              = s->inventory;
    sim->combat.weapon_id        = s->weapon_id;
    sim->combat.next_fire        = s->next_fire;
    sim->combat.kick[0]          = s->kick[0];
    sim->combat.kick[1]          = s->kick[1];
    sim->combat.kick[2]          = s->kick[2];
    sim->combat.chaingun_bullets = s->chaingun_bullets;
    sim->combat.rng.state        = s->rng_state;
    sim->fx_rng.state            = s->fx_rng_state;

    /* Frame-driven fire can leave one report for the next view-weapon step.
     * VWPN records its consumer cursors; no projectile or damage is replayed. */
    memset(&sim->combat.last_shot, 0, sizeof(sim->combat.last_shot));
    sim->combat.shot_serial = s->viewweapon_present ? s->shot_serial : 0;
    if (s->viewweapon_present) {
        sim->combat.last_shot.fired = s->shot_fired;
        sim->combat.last_shot.dry = s->shot_dry;
        sim->combat.last_shot.sound = s->shot_sound;
    }

    {
        u32 i, live = 0;

        memcpy(sim->combat.projectiles.p, s->proj, sizeof(s->proj));
        for (i = 0; i < Q2_PROJ_MAX; i++) {
            if (!s->projectile_runtime_present &&
                sim->combat.projectiles.p[i].kind == Q2_PROJ_BOLT)
                sim->combat.projectiles.p[i].flags =
                    q2_projectile_flags_for_mod(sim->combat.projectiles.p[i].mod);
            /* A creature's actor does not outlive the zone it was built in. */
            sim->combat.projectiles.p[i].shooter = NULL;
            if (sim->combat.projectiles.p[i].in_use)
                live++;
        }
        sim->combat.projectiles.live = live;
    }

    if (inv)
        *inv = s->inventory;

    /* --- script state ------------------------------------------------------ */
    if (s->event_flags && sim->events_ready && sim->event_rt.flags) {
        u32 n = s->event_count;
        if (n > sim->event_rt.record_count)
            n = sim->event_rt.record_count;
        memcpy(sim->event_rt.flags, s->event_flags, n);
    }

    /* Item latches: replaced whole, so a latch the file does not hold is
     * clear rather than left over from whatever the runtime last ran. A file
     * with no EVIT (version 5) leaves the runtime's own, which on the fresh
     * load this is applied to are clear too. */
    if (s->event_item_flags && sim->events_ready && sim->event_rt.item_flags) {
        u32 n = s->event_item_count;
        if (n > sim->event_rt.item_flags_size)
            n = sim->event_rt.item_flags_size;
        memset(sim->event_rt.item_flags, 0, sim->event_rt.item_flags_size);
        memcpy(sim->event_rt.item_flags, s->event_item_flags, n);
    }

    if (s->event_object_flags && sim->events_ready && sim->event_rt.object_flags) {
        u32 n = s->event_object_count;
        if (n > sim->event_rt.item_flags_size) n = sim->event_rt.item_flags_size;
        memset(sim->event_rt.object_flags, 0, sim->event_rt.item_flags_size);
        memcpy(sim->event_rt.object_flags, s->event_object_flags, n);
    }

    if (s->event_runtime_present) {
        q2_event_rt *rt = &sim->event_rt;
        const q2_save_event_runtime *e = &s->event_runtime;
        u32 i;
        rt->clock = e->clock; rt->tick = e->tick;
        rt->pending_count = e->pending_count;
        memcpy(rt->pending, e->pending, sizeof(rt->pending));
        memcpy(rt->deferred, e->timers, sizeof(rt->deferred));
        memcpy(rt->wait, e->waits, sizeof(rt->wait));
        rt->deferred_count = 0;
        for (i = 0; i < Q2_EVENT_RT_TIMER_MAX; i++)
            if (rt->deferred[i].item) rt->deferred_count++;
        rt->abort_record = rt->disable_self = rt->defer_timer = false;
        rt->defer_ticks = rt->defer_fires = rt->defer_window = 0;
    }

    /* --- trigger residency -------------------------------------------------- */
    if (sim->trigger_inside && sim->trigger_capacity) {
        memset(sim->trigger_inside, 0, sim->trigger_capacity);
        if (s->trigger_inside) {
            u32 n = s->trigger_count;
            if (n > sim->trigger_capacity)
                n = sim->trigger_capacity;
            memcpy(sim->trigger_inside, s->trigger_inside, n);
        }
    }

    /*
     * --- the breakables --------------------------------------------------
     *
     * Matched by SCENE NODE, not by index: the registry is rebuilt from the map
     * on load and an ordinal is only stable while build order never changes.
     * A pane the file does not mention is left as the map built it, which is
     * what a save written before this chunk existed produces.
     */
    if (s->breakables) {
        u32 i, j;

        for (i = 0; i < s->breakable_count; i++) {
            for (j = 0; j < sim->breakable_count; j++) {
                if (sim->breakable[j].scene_node != s->breakables[i].scene_node)
                    continue;
                sim->breakable[j].health = s->breakables[i].health;
                sim->breakable[j].broken = s->breakables[i].broken != 0;
                break;
            }
        }
    }

    q2_sim_breakables_sync_solidity(sim);

    if (sim->explosives) {
        u32 i, j;
        for (i = 0; i < s->explosive_count; i++) {
            const q2_save_explosive *d = &s->explosives[i];
            int index = q2_explosive_find(sim->explosives, d->item_offset);
            q2_explosive *e;
            if (index < 0) continue;
            e = &sim->explosives->items[index];
            e->health = d->health;
            e->destroyed = d->destroyed;
            for (j = 0; j < sim->breakable_count; j++) {
                q2_breakable *b = &sim->breakable[j];
                if (b->kind == Q2_BREAKABLE_FXGROUP &&
                    b->item_offset == d->item_offset) {
                    b->health = d->health;
                    b->broken = d->destroyed != 0;
                }
            }
        }
    }

    q2_sim_scene_solids_update(sim, NULL);

    /* --- the entity set ------------------------------------------------------ */
    if (s->entities && sim->entities.ent) {
        u32 n = s->entity_count;
        u32 i;

        if (n > sim->entities.count)
            n = sim->entities.count;

        for (i = 0; i < n; i++) {
            const q2_save_entity *d = &s->entities[i];
            q2_entity            *e = &sim->entities.ent[i];
            int p;

            if (!d->in_use) {
                /* Collected in single player: the record was cleared and
                 * returned to the free list (0x8006D280). */
                q2_entity_remove(e);
                continue;
            }

            /* The place ids were checked in full above, so by here they line
             * up and nothing in this loop can fail. */
            e->in_use    = true;
            e->hidden    = d->hidden != 0;

            switch ((q2_save_think)d->think) {
            case Q2_SAVE_THINK_NONE:   e->think = NULL;                  break;
            case Q2_SAVE_THINK_ITEM:   e->think = q2_item_think;         break;
            case Q2_SAVE_THINK_SHRINK: e->think = q2_item_shrink_think;  break;
            default: /* OTHER: leave whatever the spawn installed. */    break;
            }

            e->scale     = d->scale;
            e->health    = d->health;
            e->frame     = d->frame;
            e->angles[1] = d->spin;
            e->remove_in = d->remove_in;
            e->respawn_at = d->respawn_at;
            e->pos[0]    = d->pos[0];
            e->pos[1]    = d->pos[1];
            e->pos[2]    = d->pos[2];

            for (p = 0; p < Q2_MAX_PLAYERS && p < 8; p++)
                e->taken[p] = (d->taken & (1u << p)) != 0;

            /* The draw origin follows the position, and the touch box follows
             * the draw origin — both are derived, so they are recomputed rather
             * than stored. */
            e->origin[0] = e->pos[0];
            e->origin[1] = e->pos[1] + Q2_EYE_BASE - e->model_offset;
            e->origin[2] = e->pos[2];
            q2_entity_set_bounds(e, Q2_ITEM_TOUCH_HALF);
        }
    }

    /* One clock: the entity world reads the same level time the sim runs, so
     * it is put back in step rather than left where the rebuild left it. */
    if (sim->entities_ready) {
        sim->ent_world.level_time   = sim->level_time;
        sim->ent_world.cheats       = sim->cheats;
        sim->ent_world.deathmatch   = sim->multiplayer;
        /* Same reason the cheat word is put back here: a restored deathmatch
         * would otherwise run one frame's worth of touches with WEAPON STAY
         * off before the client's per-frame apply catches up. */
        sim->ent_world.weapons_stay = sim->weapons_stay;
        q2_entity_world_move_player(&sim->ent_world, 0, sim->player[0].pos);
    }

    /* The owner restores its separate creature and mover sets with the
     * q2_save_apply_creatures / q2_save_apply_movers helpers. */

    q2_save_apply_game_rng(s);

    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Mission tallies and settings                                               */
/* ------------------------------------------------------------------------- */
void q2_save_capture_mission(q2_save *s, const struct q2_mission *m)
{
    int i;

    if (!s || !m)
        return;

    s->mission_unit = m->unit;
    for (i = 0; i < Q2_SAVE_MISSION_ROWS && i < Q2_MISSION_ROWS; i++) {
        snprintf(s->mission[i].name, sizeof(s->mission[i].name), "%s",
                 m->row[i].name);
        s->mission[i].secrets       = m->row[i].secrets;
        s->mission[i].secrets_total = m->row[i].secrets_total;
        s->mission[i].kills         = m->row[i].kills;
        s->mission[i].kills_total   = m->row[i].kills_total;
    }
}

void q2_save_apply_mission(const q2_save *s, struct q2_mission *m)
{
    int i;

    if (!s || !m)
        return;

    m->unit = s->mission_unit;
    for (i = 0; i < Q2_SAVE_MISSION_ROWS && i < Q2_MISSION_ROWS; i++)
        q2_mission_set_row(m, i, s->mission[i].name,
                           s->mission[i].secrets, s->mission[i].secrets_total,
                           s->mission[i].kills,   s->mission[i].kills_total);
}

/*
 * The live counters behind the mission row's first column.
 *
 * The console has one of these three: `0x800B29FC`, the secrets-found word
 * that `0x80022210` reloads out of the row. It needs no seen-list because its
 * INSECRET stamp is the script's, and no zone slots because it never rebuilds
 * the creature set inside a level. Both of the extras are the port's and are
 * carried here rather than reconstructed, because neither can be: the offsets
 * of the secrets already counted are not derivable from their number, and a
 * row's single kills byte cannot say which zones they were made in.
 */
void q2_save_set_level_counters(q2_save *s, const q2_save_level_counters *lc)
{
    if (!s)
        return;

    if (!lc) {
        memset(&s->level, 0, sizeof(s->level));
        return;
    }

    s->level = *lc;
    s->level.present = true;
    if (s->level.secret_seen_count > Q2_SAVE_SECRETS_SEEN)
        s->level.secret_seen_count = Q2_SAVE_SECRETS_SEEN;
}

bool q2_save_get_level_counters(const q2_save *s, q2_save_level_counters *out)
{
    if (!s || !out || !s->level.present)
        return false;

    *out = s->level;
    return true;
}

/*
 * The help computer's pair, 0x800218A8 / 0x80020F34.
 *
 * Marked present even when both keys are empty, because empty is what
 * `HelpComputer_Reset` leaves and it is a real answer: "this unit has had no
 * HELPCOMPUTER yet". Reading it back as "absent" would make a reset unit and a
 * pre-version-8 file indistinguishable, and only the second should fall back to
 * the session's pair.
 */
void q2_save_set_help_keys(q2_save *s, const char *orders_key,
                           const char *objective_key)
{
    if (!s)
        return;

    memset(&s->help, 0, sizeof(s->help));
    s->help.present = true;
    if (orders_key)
        snprintf(s->help.orders_key, sizeof(s->help.orders_key), "%s",
                 orders_key);
    if (objective_key)
        snprintf(s->help.objective_key, sizeof(s->help.objective_key), "%s",
                 objective_key);
}

bool q2_save_get_help_keys(const q2_save *s, q2_save_help *out)
{
    if (!s || !out || !s->help.present)
        return false;

    *out = s->help;
    return true;
}

void q2_save_set_settings(q2_save *s, const s16 *values, u32 count)
{
    u32 i;

    if (!s)
        return;

    if (!values || count == 0) {
        s->settings_count = 0;
        return;
    }

    if (count > Q2_SAVE_SETTINGS_MAX)
        count = Q2_SAVE_SETTINGS_MAX;

    for (i = 0; i < count; i++)
        s->settings[i] = values[i];
    s->settings_count = count;
}

u32 q2_save_get_settings(const q2_save *s, s16 *out, u32 count)
{
    u32 i, n;

    if (!s || !out || count == 0)
        return 0;

    n = s->settings_count;
    if (n > count)
        n = count;
    for (i = 0; i < n; i++)
        out[i] = s->settings[i];
    return n;
}

/* ------------------------------------------------------------------------- */
/* Serialising one chunk at a time                                            */
/* ------------------------------------------------------------------------- */
static void write_head(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_HEAD);

    w_str(w, s->serial, Q2_SAVE_SERIAL_LEN);
    w_str(w, s->map,    Q2_SAVE_MAP_LEN);
    w_s32(w, s->zone);
    w_str(w, s->label,  Q2_SAVE_LABEL_LEN);
    w_u32(w, s->timestamp);

    w_chunk_end(w, at);
}

static void read_head(rbuf *r, q2_save *s)
{
    r_str(r, s->serial, Q2_SAVE_SERIAL_LEN);
    r_str(r, s->map,    Q2_SAVE_MAP_LEN);
    s->zone = r_s32(r);
    r_str(r, s->label,  Q2_SAVE_LABEL_LEN);
    s->timestamp = r_u32(r);
}

static void write_move_ent(wbuf *w, const q2_move_ent *e)
{
    w_s32v(w, e->pos, 3);
    w_s16v(w, e->ground_normal, 3);
    w_s16v(w, e->last_normal, 3);
    w_u32(w, e->flags);
    w_s16(w, e->max_slope_ny);
    w_s32(w, e->node);
}

static void read_move_ent(rbuf *r, q2_move_ent *e)
{
    r_s32v(r, e->pos, 3);
    r_s16v(r, e->ground_normal, 3);
    r_s16v(r, e->last_normal, 3);
    e->flags        = r_u32(r);
    e->max_slope_ny = r_s16(r);
    e->node         = r_s32(r);
}

static void write_player(wbuf *w, const q2_save *s)
{
    const q2_player *p = &s->player;
    size_t at = w_chunk_begin(w, TAG_PLYR);

    w_s32v(w, p->pos, 3);
    w_s32v(w, p->vel, 3);
    w_s32(w, p->yaw);
    w_s32(w, p->pitch);
    w_s32(w, p->roll);
    w_s16v(w, p->wish, 3);
    w_s16(w, p->pitch_rate);
    w_s16(w, p->yaw_rate);
    w_s16v(w, p->impulse, 3);
    w_bool(w, p->impulse_armed);
    w_s16v(w, p->frame_delta, 3);
    w_s16(w, p->jump_hold);
    w_s32(w, p->view_height);
    w_bool(w, p->on_ground);
    w_bool(w, p->crouching);
    w_s32(w, p->ground_y);
    w_s16(w, p->fall_value);
    w_s32(w, p->fall_time);
    w_s32(w, p->footstep_time);
    w_s32(w, (s32)p->foot);
    w_s32(w, (s32)p->look_scheme);

    /*
     * The movement frame's remaining state. Every one of these is a deadline or
     * a latch that outlives a tick, so a save without them restores a player who
     * has forgotten which way they were leaning, whether their view was walking
     * itself level, and how long they have left before they can grunt again.
     */
    w_u32(w, p->ent2_flags);
    w_u8(w, p->look_hist);
    w_u8(w, p->recentring);
    w_s16(w, p->autocentre);
    w_s16v(w, p->kick, 3);
    w_s32(w, p->kick_time);
    w_s16v(w, p->hurt_kick, 2);
    w_s32(w, p->pain_time);
    w_s16(w, p->prev_health);
    w_s16(w, p->prev_armour);
    w_s32(w, p->wade);
    w_s32(w, p->water_air);
    w_s32(w, p->water_next);
    w_s32(w, p->splash_time);
    w_bool(w, p->water_voice);

    write_move_ent(w, &p->ent);

    w_chunk_end(w, at);
}

static void read_player(rbuf *r, q2_save *s)
{
    q2_player *p = &s->player;

    r_s32v(r, p->pos, 3);
    r_s32v(r, p->vel, 3);
    p->yaw   = r_s32(r);
    p->pitch = r_s32(r);
    p->roll  = r_s32(r);
    r_s16v(r, p->wish, 3);
    p->pitch_rate = r_s16(r);
    p->yaw_rate   = r_s16(r);
    r_s16v(r, p->impulse, 3);
    p->impulse_armed = r_bool(r);
    r_s16v(r, p->frame_delta, 3);
    p->jump_hold   = r_s16(r);
    p->view_height = r_s32(r);
    p->on_ground   = r_bool(r);
    p->crouching   = r_bool(r);
    p->ground_y    = r_s32(r);
    p->fall_value  = r_s16(r);
    p->fall_time   = r_s32(r);
    p->footstep_time = r_s32(r);
    p->foot          = (int)r_s32(r);
    p->look_scheme   = (int)r_s32(r);

    p->ent2_flags = r_u32(r);
    p->look_hist  = r_u8(r);
    p->recentring = r_u8(r) != 0;
    p->autocentre = r_s16(r);
    r_s16v(r, p->kick, 3);
    p->kick_time  = r_s32(r);
    r_s16v(r, p->hurt_kick, 2);
    p->pain_time  = r_s32(r);
    p->prev_health = r_s16(r);
    p->prev_armour = r_s16(r);
    p->wade        = r_s32(r);
    p->water_air   = r_s32(r);
    p->water_next  = r_s32(r);
    p->splash_time = r_s32(r);
    p->water_voice = r_bool(r);

    read_move_ent(r, &p->ent);
}

static void write_sim(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_SIMS);

    w_s32(w, s->level_time);
    w_u32(w, s->tick_count);
    w_s32(w, s->dt_accum);
    w_s32(w, s->dt_per_field);
    w_s32(w, s->gravity);
    w_u32(w, s->env_flags);
    w_u32(w, s->cheats);
    w_s32(w, s->current_node);
    w_u8(w, s->multiplayer);
    w_u8(w, s->full_basis_movement);
    w_u8(w, s->no_fall_damage);

    w_chunk_end(w, at);
}

static void read_sim(rbuf *r, q2_save *s)
{
    s->level_time          = r_s32(r);
    s->tick_count          = r_u32(r);
    s->dt_accum            = r_s32(r);
    s->dt_per_field        = r_s32(r);
    s->gravity             = r_s32(r);
    s->env_flags           = r_u32(r);
    s->cheats              = r_u32(r);
    s->current_node        = r_s32(r);
    s->multiplayer         = r_u8(r);
    s->full_basis_movement = r_u8(r);
    s->no_fall_damage      = r_u8(r);
}

static void write_inventory(wbuf *w, const q2_inventory *v)
{
    int i;

    w_s16(w, v->health);
    w_s16(w, v->health_max);
    w_s16(w, v->armour);
    w_u8(w, v->armour_class);
    for (i = 0; i < Q2_AMMO_COUNT; i++)
        w_s16(w, v->ammo[i]);
    w_u16(w, v->weapons);
    w_u8(w, (u8)v->current_weapon);
    w_u8(w, v->ammo_tier);
    w_u32(w, v->flags);
    w_s16(w, v->silencer_shots);
    w_u8(w, v->last_item);
    w_s32(w, v->item_name_until);
    w_s32(w, v->quad_until);
    w_s32(w, v->invuln_until);
    w_s32(w, v->enviro_until);
    w_s32(w, v->breather_until);
    w_s32(w, v->mega_health_next);
}

static void read_inventory(rbuf *r, q2_inventory *v)
{
    int i;

    v->health       = r_s16(r);
    v->health_max   = r_s16(r);
    v->armour       = r_s16(r);
    v->armour_class = r_u8(r);
    for (i = 0; i < Q2_AMMO_COUNT; i++)
        v->ammo[i] = r_s16(r);
    v->weapons        = r_u16(r);
    v->current_weapon = (s8)r_u8(r);
    v->ammo_tier      = r_u8(r);
    v->flags          = r_u32(r);
    v->silencer_shots = r_s16(r);
    v->last_item      = r_u8(r);
    v->item_name_until = r_s32(r);
    v->quad_until      = r_s32(r);
    v->invuln_until    = r_s32(r);
    v->enviro_until    = r_s32(r);
    v->breather_until  = r_s32(r);
    v->mega_health_next = r_s32(r);
}

static void write_combat(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_CMBT);

    w_s32(w, s->weapon_id);
    w_s32(w, s->next_fire);
    w_s16v(w, s->kick, 3);
    w_s32(w, s->chaingun_bullets);
    w_u32(w, s->rng_state);
    w_u32(w, s->fx_rng_state);

    w_chunk_end(w, at);
}

static void read_combat(rbuf *r, q2_save *s)
{
    s->weapon_id        = r_s32(r);
    s->next_fire        = r_s32(r);
    r_s16v(r, s->kick, 3);
    s->chaingun_bullets = r_s32(r);
    s->rng_state        = r_u32(r);
    s->fx_rng_state     = r_u32(r);
}

static void write_event_runtime(wbuf *w, const q2_save *s)
{
    const q2_save_event_runtime *e = &s->event_runtime;
    u32 i;
    size_t at;
    if (!s->event_runtime_present) return;
    at = w_chunk_begin(w, TAG_ERTS);
    w_u32(w, 1);
    w_u32(w, e->script_size); w_u32(w, e->script_crc); w_u32(w, e->record_count);
    w_s32(w, e->clock); w_s32(w, e->tick);
    w_u32(w, e->pending_count);
    for (i = 0; i < e->pending_count; i++) w_u32(w, e->pending[i]);
    w_u32(w, Q2_EVENT_RT_TIMER_MAX);
    for (i = 0; i < Q2_EVENT_RT_TIMER_MAX; i++) {
        const q2_event_timer *t = &e->timers[i];
        w_u32(w, t->item); w_u32(w, t->offset);
        w_u8(w, t->timer_index); w_u8(w, t->next_item);
        w_u16(w, t->window); w_u16(w, t->fires);
        w_s32(w, t->period); w_s32(w, t->due);
    }
    w_u32(w, Q2_EVENT_RT_WAIT_MAX);
    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        const q2_event_wait *v = &e->waits[i];
        w_bool(w, v->in_use); w_u32(w, v->offset);
        w_s32(w, v->stamp); w_u16(w, v->count);
    }
    w_chunk_end(w, at);
}

static bool read_event_runtime(rbuf *r, q2_save *s)
{
    q2_save_event_runtime *e = &s->event_runtime;
    u32 i;
    if (s->event_runtime_present || r_u32(r) != 1) return false;
    e->script_size = r_u32(r); e->script_crc = r_u32(r); e->record_count = r_u32(r);
    e->clock = r_s32(r); e->tick = r_s32(r);
    e->pending_count = r_u32(r);
    if (e->pending_count > Q2_EVENT_RT_PENDING_MAX) return false;
    for (i = 0; i < e->pending_count; i++) e->pending[i] = r_u32(r);
    if (r_u32(r) != Q2_EVENT_RT_TIMER_MAX) return false;
    for (i = 0; i < Q2_EVENT_RT_TIMER_MAX; i++) {
        q2_event_timer *t = &e->timers[i];
        t->item = r_u32(r); t->offset = r_u32(r);
        t->timer_index = r_u8(r); t->next_item = r_u8(r);
        t->window = r_u16(r); t->fires = r_u16(r);
        t->period = r_s32(r); t->due = r_s32(r);
    }
    if (r_u32(r) != Q2_EVENT_RT_WAIT_MAX) return false;
    for (i = 0; i < Q2_EVENT_RT_WAIT_MAX; i++) {
        q2_event_wait *v = &e->waits[i];
        v->in_use = r_bool(r); v->offset = r_u32(r);
        v->stamp = r_s32(r); v->count = r_u16(r);
    }
    s->event_runtime_present = true;
    return !r->bad && r->at == r->len && valid_event_runtime(e);
}

static bool valid_viewweapon(const q2_save *s)
{
    const q2_viewweapon *v = &s->viewweapon;
    return v->weapon >= 0 && v->weapon < Q2_VM_SLOTS &&
           v->pending >= 0 && v->pending < Q2_VM_SLOTS &&
           (u32)v->state < Q2_VM_STATES && v->total >= 0 &&
           v->left >= 0 && v->left <= v->total &&
           s->viewweapon_selected >= 0 && s->viewweapon_selected < Q2_VM_SLOTS;
}

q2_result q2_save_capture_viewweapon(q2_save *s, const q2_viewweapon *vw,
                                     int selected, u32 shown, u32 heard,
                                     bool cock_pending)
{
    if (!s || !vw) return Q2_ERR_INVALID_ARG;
    s->viewweapon = *vw;
    s->viewweapon.tab = NULL;
    s->viewweapon.model = NULL;
    s->viewweapon_selected = selected;
    s->shot_serial_shown = shown;
    s->shot_serial_heard = heard;
    s->cock_pending = cock_pending;
    if (!valid_viewweapon(s)) return Q2_ERR_BAD_FORMAT;
    s->viewweapon_present = true;
    return Q2_OK;
}

q2_result q2_save_apply_viewweapon(const q2_save *s, q2_viewweapon *vw,
                                   const q2_vm_tables *tables)
{
    const q2_vm_clip *clip;
    if (!s || !vw || !tables) return Q2_ERR_INVALID_ARG;
    if (!s->viewweapon_present) return Q2_OK;
    if (!valid_viewweapon(s)) return Q2_ERR_BAD_FORMAT;
    clip = q2_vm_clip_get(tables, s->viewweapon.weapon, s->viewweapon.state);
    if (!clip || !clip->count || s->viewweapon.frame >= clip->count)
        return Q2_ERR_BAD_FORMAT;
    *vw = s->viewweapon;
    vw->tab = tables;
    vw->model = NULL;
    return Q2_OK;
}

static void write_viewweapon(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 k;
    const q2_viewweapon *v = &s->viewweapon;
    if (!s->viewweapon_present) return;
    at = w_chunk_begin(w, TAG_VWPN);
    w_u32(w, 1); /* chunk version */
#define WRITE_VW(t, f) w_##t(w, (t)v->f);
#define WRITE_VW_ARRAY(t, f, n) for (k = 0; k < n; k++) w_##t(w, v->f[k]);
    VW_SCALARS(WRITE_VW)
    VW_ARRAYS(WRITE_VW_ARRAY)
#undef WRITE_VW
#undef WRITE_VW_ARRAY
    w_s32(w, s->viewweapon_selected);
    w_u32(w, s->shot_serial);
    w_u32(w, s->shot_serial_shown);
    w_u32(w, s->shot_serial_heard);
    w_bool(w, s->shot_fired); w_bool(w, s->shot_dry);
    w_s16(w, s->shot_sound); w_bool(w, s->cock_pending);
    w_chunk_end(w, at);
}

static bool read_viewweapon(rbuf *r, q2_save *s)
{
    u32 k;
    q2_viewweapon *v = &s->viewweapon;
    if (s->viewweapon_present || r_u32(r) != 1) return false;
    memset(v, 0, sizeof(*v));
#define READ_VW(t, f) v->f = r_##t(r);
#define READ_VW_ARRAY(t, f, n) for (k = 0; k < n; k++) v->f[k] = r_##t(r);
    VW_SCALARS(READ_VW)
    VW_ARRAYS(READ_VW_ARRAY)
#undef READ_VW
#undef READ_VW_ARRAY
    s->viewweapon_selected = r_s32(r);
    s->shot_serial = r_u32(r);
    s->shot_serial_shown = r_u32(r);
    s->shot_serial_heard = r_u32(r);
    s->shot_fired = r_bool(r); s->shot_dry = r_bool(r);
    s->shot_sound = r_s16(r); s->cock_pending = r_bool(r);
    s->viewweapon_present = true;
    return !r->bad && r->at == r->len && valid_viewweapon(s);
}

static void write_projectiles(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_PROJ);
    u32 i;

    w_u32(w, (u32)Q2_PROJ_MAX);
    for (i = 0; i < Q2_PROJ_MAX; i++) {
        const q2_projectile *p = &s->proj[i];

        w_bool(w, p->in_use);
        w_u8(w, (u8)p->kind);
        w_s32v(w, p->pos, 3);
        w_s32v(w, p->vel, 3);
        w_s16(w, p->damage);
        w_s16(w, p->mod);
        w_s16(w, p->splash_radius);
        w_s32(w, p->expires);
        w_s32(w, p->owner);
        w_bool(w, p->bounced);
        w_s32(w, p->node);
    }

    w_chunk_end(w, at);
    if (s->projectile_runtime_present) {
        at = w_chunk_begin(w, TAG_PRST);
        w_u32(w, 1);
        w_u32(w, Q2_PROJ_MAX);
        for (i = 0; i < Q2_PROJ_MAX; i++) {
            w_u16(w, s->proj[i].flags);
            w_u16(w, s->proj[i].beam_time);
        }
        w_chunk_end(w, at);
    }
}

static void read_projectiles(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    for (i = 0; i < n; i++) {
        q2_projectile scratch;
        q2_projectile *p = (i < Q2_PROJ_MAX) ? &s->proj[i] : &scratch;
        u16 flags = 0, beam_time = 0;

        /* Chunks may arrive in either order. PRST's state must survive the
         * base projectile reader's initialization when it appeared first. */
        if (s->projectile_runtime_present && i < Q2_PROJ_MAX) {
            flags = p->flags; beam_time = p->beam_time;
        }

        memset(p, 0, sizeof(*p));
        p->in_use = r_bool(r);
        p->kind   = (q2_proj_kind)r_u8(r);
        r_s32v(r, p->pos, 3);
        r_s32v(r, p->vel, 3);
        p->damage        = r_s16(r);
        p->mod           = r_s16(r);
        p->splash_radius = r_s16(r);
        p->expires       = r_s32(r);
        p->owner         = r_s32(r);
        p->bounced       = r_bool(r);
        p->node          = r_s32(r);
        p->flags = s->projectile_runtime_present ? flags :
                   (p->kind == Q2_PROJ_BOLT ? q2_projectile_flags_for_mod(p->mod) : 0);
        p->beam_time = beam_time;

        if (r->bad)
            return;
    }
}

static bool read_projectile_runtime(rbuf *r, q2_save *s)
{
    u32 i;
    if (s->projectile_runtime_present || r_u32(r) != 1 ||
        r_u32(r) != Q2_PROJ_MAX) return false;
    for (i = 0; i < Q2_PROJ_MAX; i++) {
        s->proj[i].flags = r_u16(r);
        s->proj[i].beam_time = r_u16(r);
    }
    s->projectile_runtime_present = true;
    return !r->bad && r->at == r->len;
}

static void write_entities(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_ENTS);
    u32 i;

    w_u32(w, s->entity_count);
    for (i = 0; i < s->entity_count; i++) {
        const q2_save_entity *e = &s->entities[i];

        w_u16(w, e->place_id);
        w_u8(w, e->in_use);
        w_u8(w, e->hidden);
        w_u8(w, e->taken);
        w_u8(w, e->think);
        w_s16(w, e->scale);
        w_s16(w, e->health);
        w_s32(w, e->frame);
        w_s32(w, e->spin);
        w_s32(w, e->remove_in);
        w_s32(w, e->respawn_at);
        w_s32v(w, e->pos, 3);
    }

    w_chunk_end(w, at);
}

static bool read_entities(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->entities);
    s->entities = (q2_save_entity *)calloc(n, sizeof(q2_save_entity));
    if (!s->entities)
        return false;
    s->entity_count = n;

    for (i = 0; i < n; i++) {
        q2_save_entity *e = &s->entities[i];

        e->place_id   = r_u16(r);
        e->in_use     = r_u8(r);
        e->hidden     = r_u8(r);
        e->taken      = r_u8(r);
        e->think      = r_u8(r);
        e->scale      = r_s16(r);
        e->health     = r_s16(r);
        e->frame      = r_s32(r);
        e->spin       = r_s32(r);
        e->remove_in  = r_s32(r);
        e->respawn_at = r_s32(r);
        r_s32v(r, e->pos, 3);

        if (r->bad)
            return false;
    }
    return true;
}

static void write_item_state(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 i;

    if (!s->item_state_present)
        return;

    at = w_chunk_begin(w, TAG_ITEM);
    w_u32(w, s->item_population_group_count);
    w_u32(w, s->item_group_order_count);
    for (i = 0; i < s->item_group_order_count; i++)
        w_u32(w, s->item_group_order[i]);

    w_u32(w, s->item_key_count);
    for (i = 0; i < s->item_key_count; i++) {
        w_s32(w, s->item_keys[i].group);
        w_u32(w, s->item_keys[i].slot);
    }
    w_chunk_end(w, at);
}

static bool read_item_state(rbuf *r, q2_save *s)
{
    u32 groups = r_u32(r);
    u32 order_count = r_u32(r);
    u32 i;

    if (r->bad || groups > SAVE_MAX_ITEM_GROUPS || order_count > groups)
        return false;

    free(s->item_group_order);
    s->item_group_order = NULL;
    s->item_group_order_count = 0;
    if (order_count) {
        s->item_group_order = (u32 *)calloc(order_count, sizeof(u32));
        if (!s->item_group_order)
            return false;
        for (i = 0; i < order_count; i++) {
            s->item_group_order[i] = r_u32(r);
            if (r->bad || s->item_group_order[i] >= groups)
                return false;
        }
        s->item_group_order_count = order_count;
    }

    s->item_key_count = r_u32(r);
    if (r->bad || s->item_key_count > SAVE_MAX_ENTITIES)
        return false;

    free(s->item_keys);
    s->item_keys = NULL;
    if (s->item_key_count) {
        s->item_keys = (q2_save_item_key *)calloc(s->item_key_count,
                                                  sizeof(*s->item_keys));
        if (!s->item_keys)
            return false;

        for (i = 0; i < s->item_key_count; i++) {
            s32 group = r_s32(r);
            u32 slot  = r_u32(r);

            if (r->bad || group < -1 ||
                (group >= 0 && (u32)group >= groups))
                return false;
            s->item_keys[i].group = group;
            s->item_keys[i].slot  = slot;
        }
    }
    s->item_population_group_count = groups;
    s->item_state_present = true;
    return true;
}

static void write_breakables(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_BRKS);
    u32 i;

    w_u32(w, s->breakable_count);
    for (i = 0; i < s->breakable_count; i++) {
        w_s32(w, s->breakables[i].scene_node);
        w_s16(w, s->breakables[i].health);
        w_u8(w, s->breakables[i].broken);
    }

    w_chunk_end(w, at);
}

static bool read_breakables(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->breakables);
    s->breakables = (q2_save_breakable *)calloc(n, sizeof(*s->breakables));
    if (!s->breakables)
        return false;
    s->breakable_count = n;

    for (i = 0; i < n; i++) {
        s->breakables[i].scene_node = r_s32(r);
        s->breakables[i].health     = r_s16(r);
        s->breakables[i].broken     = r_u8(r);
        if (r->bad)
            return false;
    }
    return true;
}

static void write_movers(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_MOVR);
    u32 i;

    w_u32(w, s->mover_count);
    for (i = 0; i < s->mover_count; i++) {
        const q2_save_mover *m = &s->movers[i];

        w_u32(w, m->item_offset);
        w_u8(w, m->seq);
        w_u8(w, m->state);
        w_u8(w, m->saved_state);
        w_u8(w, m->block_timer);
        w_u8(w, m->triggered);
        w_u8(w, m->announced);
        w_u16(w, m->delay_timer);
        w_u16(w, m->wait_timer);
        w_s32(w, m->offset);
    }

    w_chunk_end(w, at);
}

static bool read_movers(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->movers);
    s->movers = (q2_save_mover *)calloc(n, sizeof(*s->movers));
    if (!s->movers)
        return false;
    s->mover_count = n;

    for (i = 0; i < n; i++) {
        q2_save_mover *m = &s->movers[i];

        m->item_offset = r_u32(r);
        m->seq         = r_u8(r);
        m->state       = r_u8(r);
        m->saved_state = r_u8(r);
        m->block_timer = r_u8(r);
        m->triggered   = r_u8(r);
        m->announced   = r_u8(r);
        m->delay_timer = r_u16(r);
        m->wait_timer  = r_u16(r);
        m->offset      = r_s32(r);
        if (r->bad)
            return false;
    }
    return true;
}

/* Which occurrence of `item_offset` mover `index` is. A MOVER_C double door is
 * two leaves from one item, so the offset alone does not identify a leaf. */
static u8 mover_seq(const q2_mover_set *set, u32 index)
{
    u32 i, n = 0;

    for (i = 0; i < index && i < set->count; i++)
        if (set->movers[i].item_offset == set->movers[index].item_offset)
            n++;
    return (u8)(n > 255 ? 255 : n);
}

void q2_save_capture_movers(q2_save *s, const q2_mover_set *set)
{
    u32 i;

    if (!s || !set || set->count == 0 || !set->movers)
        return;

    free(s->movers);
    s->movers = (q2_save_mover *)calloc(set->count, sizeof(*s->movers));
    if (!s->movers)
        return;
    s->mover_count = set->count;

    for (i = 0; i < set->count; i++) {
        const q2_mover *m = &set->movers[i];
        q2_save_mover  *d = &s->movers[i];

        d->item_offset = m->item_offset;
        d->seq         = mover_seq(set, i);
        d->state       = m->state;
        d->saved_state = m->saved_state;
        d->block_timer = m->block_timer;
        d->triggered   = m->triggered;
        d->announced   = m->announced;
        d->delay_timer = m->delay_timer;
        d->wait_timer  = m->wait_timer;
        d->offset      = m->offset;
    }
}

void q2_save_apply_movers(const q2_save *s, q2_mover_set *set)
{
    u32 i, j;

    if (!s || !s->movers || !set || !set->movers)
        return;

    for (i = 0; i < s->mover_count; i++) {
        const q2_save_mover *d = &s->movers[i];

        for (j = 0; j < set->count; j++) {
            q2_mover *m = &set->movers[j];

            if (m->item_offset != d->item_offset)
                continue;
            if (mover_seq(set, j) != d->seq)
                continue;

            m->state       = d->state;
            m->saved_state = d->saved_state;
            m->block_timer = d->block_timer;
            m->triggered   = d->triggered;
            m->announced   = d->announced;
            m->delay_timer = d->delay_timer;
            m->wait_timer  = d->wait_timer;
            m->offset      = d->offset;
            break;
        }
    }
}

q2_result q2_save_capture_rotators(q2_save *s, const q2_rotator_set *set)
{
    q2_save_rotator *copy;
    u32 i;
    if (!s || !set || (set->count && !set->rotators)) return Q2_ERR_INVALID_ARG;
    copy = set->count ? calloc(set->count, sizeof(*copy)) : NULL;
    if (set->count && !copy) return Q2_ERR_NO_MEMORY;
    for (i = 0; i < set->count; i++) {
        const q2_rotator *r = &set->rotators[i];
        q2_save_rotator *d = &copy[i];
        d->node = r->node; d->kind = (u8)r->kind; d->axis = r->axis;
        d->accum = r->accum; d->angle = r->angle; d->state = r->state;
        d->step_pending = r->step_pending;
        d->delay = r->delay; d->hold = r->hold;
        d->motor_active = (r->loop_running ||
                           (r->loop_pending & Q2_ROTLOOP_WANT_START)) &&
                          !(r->loop_pending & Q2_ROTLOOP_WANT_STOP);
    }
    free(s->rotators);
    s->rotators = copy;
    s->rotator_count = set->count;
    s->rotators_present = true;
    return Q2_OK;
}

q2_result q2_save_apply_rotators(const q2_save *s, q2_rotator_set *set)
{
    u32 i;
    if (!s || !set) return Q2_ERR_INVALID_ARG;
    if (!s->rotators_present) return Q2_OK;
    if (s->rotator_count != set->count ||
        (set->count && (!set->rotators || !s->rotators))) return Q2_ERR_BAD_FORMAT;
    /* Validate the entire rebuilt roster before changing any member. */
    for (i = 0; i < set->count; i++) {
        const q2_save_rotator *d = &s->rotators[i];
        const q2_rotator *r = &set->rotators[i];
        if (d->node != r->node || d->kind != (u8)r->kind || d->axis != r->axis)
            return Q2_ERR_BAD_FORMAT;
    }
    for (i = 0; i < set->count; i++) {
        const q2_save_rotator *d = &s->rotators[i];
        q2_rotator *r = &set->rotators[i];
        r->accum = d->accum; r->angle = d->angle; r->state = d->state;
        r->step_pending = d->step_pending != 0;
        r->delay = d->delay; r->hold = d->hold;
        r->sound_pending = Q2_ROTSND_NONE;
        r->loop_running = false;
        r->loop_pending = d->motor_active ? Q2_ROTLOOP_WANT_START : 0;
        r->loop_voice = NULL;
        r->loop_serial = 0;
    }
    return Q2_OK;
}

static void write_rotators(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 i;
    if (!s->rotators_present) return;
    at = w_chunk_begin(w, TAG_ROTR);
    w_u32(w, 1); w_u32(w, s->rotator_count);
    for (i = 0; i < s->rotator_count; i++) {
        const q2_save_rotator *d = &s->rotators[i];
        w_s16(w, d->node); w_u8(w, d->kind); w_u8(w, d->axis);
        w_s32(w, d->accum); w_s16(w, d->angle); w_u8(w, d->state);
        w_u8(w, d->step_pending); w_u8(w, d->motor_active);
        w_u16(w, d->delay); w_u16(w, d->hold);
    }
    w_chunk_end(w, at);
}

static bool read_rotators(rbuf *r, q2_save *s)
{
    u32 version = r_u32(r), n = r_u32(r), i;
    if (r->bad || version != 1 || s->rotators_present ||
        n > SAVE_MAX_ENTITIES || n != (r->len - r->at) / 17) return false;
    s->rotators = n ? calloc(n, sizeof(*s->rotators)) : NULL;
    if (n && !s->rotators) return false;
    s->rotator_count = n;
    s->rotators_present = true;
    for (i = 0; i < n; i++) {
        q2_save_rotator *d = &s->rotators[i];
        d->node = r_s16(r); d->kind = r_u8(r); d->axis = r_u8(r);
        d->accum = r_s32(r); d->angle = r_s16(r); d->state = r_u8(r);
        d->step_pending = r_u8(r); d->motor_active = r_u8(r);
        d->delay = r_u16(r); d->hold = r_u16(r);
        if (r->bad || d->kind > Q2_ROT_SNAP || d->axis > 3 ||
            d->state > Q2_ROTST_HOLD || d->state == 4 ||
            d->step_pending > 1 || d->motor_active > 1) return false;
    }
    return r->at == r->len;
}

static void copy_ai_state(q2_monster *d, const q2_monster *m)
{
#define COPY_SCALAR(t, f) d->f = m->f;
#define COPY_ARRAY(t, f, n) memcpy(d->f, m->f, sizeof(d->f));
    AI_SCALARS(COPY_SCALAR)
    AI_ARRAYS(COPY_ARRAY)
#undef COPY_SCALAR
#undef COPY_ARRAY
}

static void write_ai_state(wbuf *w, const q2_monster *m)
{
    u32 k;
#define WRITE_SCALAR(t, f) w_##t(w, (t)m->f);
#define WRITE_ARRAY(t, f, n) for (k = 0; k < n; k++) w_##t(w, m->f[k]);
    AI_SCALARS(WRITE_SCALAR)
    AI_ARRAYS(WRITE_ARRAY)
#undef WRITE_SCALAR
#undef WRITE_ARRAY
}

static void read_ai_state(rbuf *r, q2_monster *m)
{
    u32 k;
#define READ_SCALAR(t, f) m->f = r_##t(r);
#define READ_ARRAY(t, f, n) for (k = 0; k < n; k++) m->f[k] = r_##t(r);
    AI_SCALARS(READ_SCALAR)
    AI_ARRAYS(READ_ARRAY)
#undef READ_SCALAR
#undef READ_ARRAY
}

/* -1 = no target, -2 = player proxy, -(3+i) = path corner, i >= 0 = monster. */
static bool ai_ref(const q2_creature_world *w, const q2_monster *m, s32 *out)
{
    u32 i;
    if (!m) { *out = -1; return true; }
    if (m == &w->sight) { *out = -2; return true; }
    for (i = 0; i < w->set.count; i++)
        if (m == &w->set.monsters[i]) { *out = (s32)i; return true; }
    for (i = 0; i < w->corner_count; i++)
        if (m == &w->corner[i]) { *out = -3 - (s32)i; return true; }
    return false;
}

static bool valid_ai_ref(s32 ref, u32 count, u32 corners)
{
    if (ref == -1 || ref == -2) return true;
    return ref >= 0 ? (u32)ref < count : -(s64)ref - 3 < (s64)corners;
}

static q2_monster *resolve_ai_ref(q2_creature_world *w, s32 ref)
{
    if (ref == -1) return NULL;
    if (ref == -2) return &w->sight;
    if (ref >= 0) return &w->set.monsters[ref];
    return &w->corner[-(s64)ref - 3];
}

static const q2_mmove *resolve_ai_move(const q2_creature_world *w,
                                       const save_ai_monster *d)
{
    u32 i, j;
    for (i = 0; i < w->mod_count; i++) {
        const q2_creature_module *mod = &w->mod[i];
        if (strcmp(mod->name, d->move_module)) continue;
        for (j = 0; j < mod->bind.move_count && j < mod->cre.move_count; j++)
            if (!mod->cre.move[j].clip_piece &&
                mod->cre.move[j].addr - mod->cre.base == d->move_offset)
                return &mod->bind.move[j];
    }
    return NULL;
}

bool q2_save_get_skill(const q2_save *s, s32 *skill)
{
    if (!s || !s->skill_present || !skill) return false;
    *skill = s->skill;
    return true;
}

void q2_save_apply_game_rng(const q2_save *s)
{
    if (s && s->game_rng_present) q2_random_seed(s->game_rng_state);
}

q2_result q2_save_capture_ai(q2_save *s, const q2_creature_world *w, u32 phase_us)
{
    struct q2_save_ai *a;
    u32 i, j, k;
    if (!s || !w || phase_us > 500000 || w->set.count > SAVE_MAX_ENTITIES ||
        w->corner_count > SAVE_MAX_ENTITIES ||
        (w->set.count && !w->set.monsters) || (w->corner_count && !w->corner))
        return Q2_ERR_INVALID_ARG;
    a = calloc(1, sizeof(*a));
    if (!a) return Q2_ERR_NO_MEMORY;
    a->count = w->set.count; a->corner_count = w->corner_count;
    a->phase_us = phase_us;
    a->monsters = a->count ? calloc(a->count, sizeof(*a->monsters)) : NULL;
    if (a->count && !a->monsters) { free_ai(a); return Q2_ERR_NO_MEMORY; }
    a->level = q2_level_state;
    if (!ai_ref(w, a->level.sight_client, &a->level_refs[0]) ||
        !ai_ref(w, a->level.sight_entity, &a->level_refs[1]) ||
        !ai_ref(w, a->level.sound_entity, &a->level_refs[2]) ||
        !ai_ref(w, a->level.sound2_entity, &a->level_refs[3])) goto bad;
    a->level.sight_client = a->level.sight_entity = NULL;
    a->level.sound_entity = a->level.sound2_entity = NULL;
    copy_ai_state(&a->sight, &w->sight);
    q2_trail_capture(&a->trail);
    for (i = 0; i < a->count; i++) {
        const q2_monster *m = &w->set.monsters[i];
        save_ai_monster *d = &a->monsters[i];
        copy_ai_state(&d->state, m);
        if (!ai_ref(w, m->enemy, &d->refs[0]) ||
            !ai_ref(w, m->oldenemy, &d->refs[1]) ||
            !ai_ref(w, m->goalentity, &d->refs[2]) ||
            !ai_ref(w, m->movetarget, &d->refs[3]) ||
            !ai_ref(w, m->owner, &d->refs[4])) goto bad;
        if (m->think && m->think != q2_M_MoveFrame) goto bad;
        d->thinking = m->think != NULL;
        if (!m->currentmove) continue;
        for (j = 0; j < w->mod_count && !d->has_move; j++) {
            const q2_creature_module *mod = &w->mod[j];
            for (k = 0; k < mod->bind.move_count && k < mod->cre.move_count; k++) {
                if (m->currentmove != &mod->bind.move[k]) continue;
                snprintf(d->move_module, sizeof(d->move_module), "%s", mod->name);
                d->move_offset = mod->cre.move[k].addr - mod->cre.base;
                d->has_move = 1;
                break;
            }
        }
        if (!d->has_move) goto bad;
    }
    free_ai(s->ai);
    s->ai = a;
    return Q2_OK;
bad:
    free_ai(a);
    return Q2_ERR_BAD_FORMAT;
}

q2_result q2_save_apply_ai(const q2_save *s, q2_creature_world *w, u32 *phase_us)
{
    const struct q2_save_ai *a;
    u32 i, j;
    if (!s || !w) return Q2_ERR_INVALID_ARG;
    if (!s->ai) return Q2_OK;
    a = s->ai;
    if (a->count != w->set.count || a->corner_count != w->corner_count ||
        (a->count && (!w->set.monsters || !a->monsters)) ||
        (a->corner_count && !w->corner) || a->trail.head >= Q2_TRAIL_LENGTH)
        return Q2_ERR_BAD_FORMAT;
    for (i = 0; i < 4; i++)
        if (!valid_ai_ref(a->level_refs[i], a->count, a->corner_count))
            return Q2_ERR_BAD_FORMAT;
    for (i = 0; i < a->count; i++) {
        const save_ai_monster *d = &a->monsters[i];
        const q2_monster *m = &w->set.monsters[i];
        if (m->pop_class_id != d->state.pop_class_id || m->group != d->state.group ||
            (d->has_move && !resolve_ai_move(w, d))) return Q2_ERR_BAD_FORMAT;
        for (j = 0; j < 5; j++)
            if (!valid_ai_ref(d->refs[j], a->count, a->corner_count))
                return Q2_ERR_BAD_FORMAT;
    }
    for (i = 0; i < a->count; i++) {
        const save_ai_monster *d = &a->monsters[i];
        q2_monster *m = &w->set.monsters[i];
        copy_ai_state(m, &d->state);
        m->currentmove = d->has_move ? resolve_ai_move(w, d) : NULL;
        m->think = d->thinking ? q2_M_MoveFrame : NULL;
        m->enemy = resolve_ai_ref(w, d->refs[0]);
        m->oldenemy = resolve_ai_ref(w, d->refs[1]);
        m->goalentity = resolve_ai_ref(w, d->refs[2]);
        m->movetarget = resolve_ai_ref(w, d->refs[3]);
        m->owner = resolve_ai_ref(w, d->refs[4]);
        if (m->corpse) {
            m->stand = m->idle = m->search = m->walk = m->run = NULL;
            m->attack = m->melee = NULL;
            m->dodge = NULL; m->sight = NULL; m->checkattack = NULL;
            m->bigturn = NULL;
        }
    }
    copy_ai_state(&w->sight, &a->sight);
    q2_level_state = a->level;
    q2_level_state.sight_client = resolve_ai_ref(w, a->level_refs[0]);
    q2_level_state.sight_entity = resolve_ai_ref(w, a->level_refs[1]);
    q2_level_state.sound_entity = resolve_ai_ref(w, a->level_refs[2]);
    q2_level_state.sound2_entity = resolve_ai_ref(w, a->level_refs[3]);
    q2_trail_restore(&a->trail);
    if (phase_us) *phase_us = a->phase_us;
    return Q2_OK;
}

static void write_ai(wbuf *w, const q2_save *s)
{
    const struct q2_save_ai *a = s->ai;
    size_t at;
    u32 i;
    if (!a) return;
    at = w_chunk_begin(w, TAG_CAIS);
    w_u32(w, 1); w_u32(w, a->count); w_u32(w, a->corner_count);
    w_u32(w, a->phase_us);
    w_s32(w, a->level.framenum); w_s32(w, a->level.time);
    w_s32(w, a->level.total_monsters); w_s32(w, a->level.killed_monsters);
    w_s32v(w, a->level_refs, 4);
    w_s32(w, a->level.sight_entity_framenum);
    w_s32(w, a->level.sound_entity_framenum);
    w_s32(w, a->level.sound2_entity_framenum);
    write_ai_state(w, &a->sight);
    w_u32(w, a->trail.head); w_u8(w, a->trail.active);
    for (i = 0; i < Q2_TRAIL_LENGTH; i++) {
        w_s32v(w, a->trail.spot[i].origin, 3);
        w_s32(w, a->trail.spot[i].timestamp); w_s16(w, a->trail.spot[i].yaw);
    }
    for (i = 0; i < a->count; i++) {
        const save_ai_monster *d = &a->monsters[i];
        write_ai_state(w, &d->state);
        w_s32v(w, d->refs, 5); w_str(w, d->move_module, sizeof(d->move_module));
        w_u32(w, d->move_offset); w_u8(w, d->has_move); w_u8(w, d->thinking);
    }
    w_chunk_end(w, at);
}

static bool read_ai(rbuf *r, q2_save *s)
{
    struct q2_save_ai *a;
    u32 version = r_u32(r), n = r_u32(r), i, j;
    if (r->bad || version != 1 || s->ai || n > SAVE_MAX_ENTITIES ||
        n > (r->len - r->at) / 128) return false;
    a = calloc(1, sizeof(*a));
    if (!a) return false;
    s->ai = a;
    a->count = n; a->corner_count = r_u32(r); a->phase_us = r_u32(r);
    if (a->corner_count > SAVE_MAX_ENTITIES || a->phase_us > 500000) return false;
    a->monsters = n ? calloc(n, sizeof(*a->monsters)) : NULL;
    if (n && !a->monsters) return false;
    a->level.framenum = r_s32(r); a->level.time = r_s32(r);
    a->level.total_monsters = r_s32(r); a->level.killed_monsters = r_s32(r);
    r_s32v(r, a->level_refs, 4);
    for (i = 0; i < 4; i++)
        if (!valid_ai_ref(a->level_refs[i], n, a->corner_count)) return false;
    a->level.sight_entity_framenum = r_s32(r);
    a->level.sound_entity_framenum = r_s32(r);
    a->level.sound2_entity_framenum = r_s32(r);
    read_ai_state(r, &a->sight);
    a->trail.head = r_u32(r); a->trail.active = r_u8(r) != 0;
    if (a->trail.head >= Q2_TRAIL_LENGTH) return false;
    for (i = 0; i < Q2_TRAIL_LENGTH; i++) {
        r_s32v(r, a->trail.spot[i].origin, 3);
        a->trail.spot[i].timestamp = r_s32(r); a->trail.spot[i].yaw = r_s16(r);
    }
    for (i = 0; i < n; i++) {
        save_ai_monster *d = &a->monsters[i];
        read_ai_state(r, &d->state);
        r_s32v(r, d->refs, 5);
        for (j = 0; j < sizeof(d->move_module); j++)
            d->move_module[j] = (char)r_u8(r);
        d->move_offset = r_u32(r); d->has_move = r_u8(r); d->thinking = r_u8(r);
        if (r->bad || d->has_move > 1 || d->thinking > 1 ||
            !memchr(d->move_module, 0, sizeof(d->move_module))) return false;
        for (j = 0; j < 5; j++)
            if (!valid_ai_ref(d->refs[j], n, a->corner_count)) return false;
    }
    return !r->bad && r->at == r->len;
}

static void write_creatures(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_CRES);
    u32 i;

    w_u32(w, s->creature_count);
    for (i = 0; i < s->creature_count; i++) {
        const q2_save_creature *c = &s->creatures[i];

        w_u8(w, c->in_use);
        w_u8(w, c->dead);
        w_s16(w, c->health);
        w_s16(w, c->frame);
        w_s16v(w, c->angles, 3);
        w_s32v(w, c->pos, 3);
    }

    w_chunk_end(w, at);
}

static bool read_creatures(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > SAVE_MAX_ENTITIES)
        return false;

    free(s->creatures);
    s->creatures = (q2_save_creature *)calloc(n, sizeof(*s->creatures));
    if (!s->creatures)
        return false;
    s->creature_count = n;

    for (i = 0; i < n; i++) {
        q2_save_creature *c = &s->creatures[i];

        c->in_use = r_u8(r);
        c->dead   = r_u8(r);
        c->health = r_s16(r);
        c->frame  = r_s16(r);
        r_s16v(r, c->angles, 3);
        r_s32v(r, c->pos, 3);
        if (r->bad)
            return false;
    }
    return true;
}

void q2_save_capture_creatures(q2_save *s, const q2_monster_set *set)
{
    u32 i;

    if (!s || !set || set->count == 0 || !set->monsters)
        return;

    free(s->creatures);
    s->creatures = (q2_save_creature *)calloc(set->count, sizeof(*s->creatures));
    if (!s->creatures)
        return;
    s->creature_count = set->count;

    for (i = 0; i < set->count; i++) {
        const q2_monster *m = &set->monsters[i];
        q2_save_creature *d = &s->creatures[i];
        int k;

        d->in_use = m->in_use ? 1u : 0u;
        d->dead   = m->dead ? 1u : 0u;
        d->health = m->health;
        d->frame  = m->frame;
        for (k = 0; k < 3; k++) {
            d->angles[k] = m->angles[k];
            d->pos[k]    = m->pos[k];
        }
    }
}

void q2_save_apply_creatures(const q2_save *s, q2_monster_set *set)
{
    u32 i;

    if (!s || !s->creatures || !set || !set->monsters)
        return;

    /*
     * The count must match. The set is rebuilt from the map's spawn records in
     * a fixed order, so a different count means a different population — a
     * save from another zone, or from a build whose spawn pass differs — and
     * restoring by index into that would put one creature's health on another.
     */
    if (s->creature_count != set->count)
        return;

    for (i = 0; i < set->count; i++) {
        const q2_save_creature *d = &s->creatures[i];
        q2_monster             *m = &set->monsters[i];
        int k;

        m->in_use = d->in_use != 0;
        m->dead   = d->dead != 0;
        m->health = d->health;
        m->frame  = d->frame;
        for (k = 0; k < 3; k++) {
            m->angles[k] = d->angles[k];
            m->pos[k]    = d->pos[k];
        }
    }
}

static void write_mission(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_MISN);
    int i;

    w_s32(w, s->mission_unit);
    w_u32(w, (u32)Q2_SAVE_MISSION_ROWS);
    for (i = 0; i < Q2_SAVE_MISSION_ROWS; i++) {
        w_str(w, s->mission[i].name, Q2_SAVE_MISSION_NAME + 1);
        w_u8(w, s->mission[i].secrets);
        w_u8(w, s->mission[i].secrets_total);
        w_u8(w, s->mission[i].kills);
        w_u8(w, s->mission[i].kills_total);
    }

    w_chunk_end(w, at);
}

static void read_mission(rbuf *r, q2_save *s)
{
    u32 n, i;

    s->mission_unit = r_s32(r);
    n = r_u32(r);

    for (i = 0; i < n; i++) {
        q2_save_level_stats scratch;
        q2_save_level_stats *row =
            (i < Q2_SAVE_MISSION_ROWS) ? &s->mission[i] : &scratch;

        r_str(r, row->name, Q2_SAVE_MISSION_NAME + 1);
        row->secrets       = r_u8(r);
        row->secrets_total = r_u8(r);
        row->kills         = r_u8(r);
        row->kills_total   = r_u8(r);

        if (r->bad)
            return;
    }
}

/*
 * LVLC. Written only when there is something to write, so a utility snapshot
 * that never touched a level does not claim counters it does not have — and so
 * `present` means what it says on the way back in.
 *
 * The two counts lead their arrays so a later version can widen either without
 * a new tag; the reader takes what it can hold and steps over the rest.
 */
static void write_level_counters(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 i;

    if (!s->level.present)
        return;

    at = w_chunk_begin(w, TAG_LVLC);
    w_u32(w, s->level.secrets_found);
    w_u32(w, s->level.secret_seen_count);
    for (i = 0; i < s->level.secret_seen_count &&
                i < Q2_SAVE_SECRETS_SEEN; i++)
        w_u32(w, s->level.secret_seen[i]);

    w_u32(w, (u32)Q2_SAVE_LEVEL_ZONES);
    for (i = 0; i < Q2_SAVE_LEVEL_ZONES; i++) {
        w_u32(w, s->level.zone_dead[i]);
        w_u32(w, s->level.zone_placed[i]);
    }
    w_chunk_end(w, at);
}

static void read_level_counters(rbuf *r, q2_save *s)
{
    u32 n, z, i;

    memset(&s->level, 0, sizeof(s->level));

    s->level.secrets_found = r_u32(r);
    n = r_u32(r);
    for (i = 0; i < n; i++) {
        u32 off = r_u32(r);

        if (r->bad)
            return;
        if (i < Q2_SAVE_SECRETS_SEEN)
            s->level.secret_seen[i] = off;
    }
    s->level.secret_seen_count =
        (n > Q2_SAVE_SECRETS_SEEN) ? Q2_SAVE_SECRETS_SEEN : n;

    z = r_u32(r);
    for (i = 0; i < z; i++) {
        u32 dead   = r_u32(r);
        u32 placed = r_u32(r);

        if (r->bad)
            return;
        if (i < Q2_SAVE_LEVEL_ZONES) {
            s->level.zone_dead[i]   = dead;
            s->level.zone_placed[i] = placed;
        }
    }

    s->level.present = !r->bad;
}

/*
 * HELP. Two fixed-width keys and nothing else — what the console's hub arms
 * copy is exactly twelve bytes apiece (0x800218B0..0x800218DC), and the width
 * here is thirteen only so the reader always has a terminator.
 */
static void write_help_keys(wbuf *w, const q2_save *s)
{
    size_t at;

    if (!s->help.present)
        return;

    at = w_chunk_begin(w, TAG_HELP);
    w_str(w, s->help.orders_key,    sizeof(s->help.orders_key));
    w_str(w, s->help.objective_key, sizeof(s->help.objective_key));
    w_chunk_end(w, at);
}

static void read_help_keys(rbuf *r, q2_save *s)
{
    memset(&s->help, 0, sizeof(s->help));
    r_str(r, s->help.orders_key,    sizeof(s->help.orders_key));
    r_str(r, s->help.objective_key, sizeof(s->help.objective_key));
    s->help.present = !r->bad;
}

static void write_settings(wbuf *w, const q2_save *s)
{
    size_t at = w_chunk_begin(w, TAG_SETT);
    u32 i;

    w_u32(w, s->settings_count);
    for (i = 0; i < s->settings_count; i++)
        w_s16(w, s->settings[i]);

    w_chunk_end(w, at);
}

static void read_settings(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r);
    u32 i;

    s->settings_count = (n > Q2_SAVE_SETTINGS_MAX) ? Q2_SAVE_SETTINGS_MAX : n;
    for (i = 0; i < n; i++) {
        s16 v = r_s16(r);
        if (i < Q2_SAVE_SETTINGS_MAX)
            s->settings[i] = v;
        if (r->bad)
            return;
    }
}

/* A run of bytes with a leading count — the event flags and the trigger
 * residency are both exactly that, so they share one pair. */
static void write_bytes_chunk(wbuf *w, u32 tag, const u8 *data, u32 count)
{
    size_t at = w_chunk_begin(w, tag);

    w_u32(w, count);
    if (count && data)
        w_bytes(w, data, count);

    w_chunk_end(w, at);
}

static bool read_bytes_chunk(rbuf *r, u8 **out, u32 *out_count, u32 limit)
{
    u32 n = r_u32(r);
    u32 i;

    if (r->bad)
        return false;
    if (n == 0)
        return true;
    if (n > limit || !r_need(r, n))
        return false;

    free(*out);
    *out = (u8 *)malloc(n);
    if (!*out)
        return false;

    for (i = 0; i < n; i++)
        (*out)[i] = r_u8(r);
    *out_count = n;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */
static void write_explosives(wbuf *w, const q2_save *s)
{
    u32 i;
    size_t at;
    if (!s->explosive_count) return;
    at = w_chunk_begin(w, TAG_EXPL);
    w_u32(w, s->explosive_count);
    for (i = 0; i < s->explosive_count; i++) {
        w_u32(w, s->explosives[i].item_offset);
        w_s16(w, s->explosives[i].health);
        w_u8(w, s->explosives[i].destroyed);
    }
    w_chunk_end(w, at);
}

static bool read_explosives(rbuf *r, q2_save *s)
{
    u32 i, n = r_u32(r);
    if (r->bad || n > SAVE_MAX_ENTITIES || s->explosives ||
        !r_need(r, (size_t)n * 7)) return false;
    if (n) {
        s->explosives = calloc(n, sizeof(*s->explosives));
        if (!s->explosives) return false;
    }
    s->explosive_count = n;
    for (i = 0; i < n; i++) {
        s->explosives[i].item_offset = r_u32(r);
        s->explosives[i].health = r_s16(r);
        s->explosives[i].destroyed = r_u8(r);
        if (s->explosives[i].destroyed > 1) return false;
    }
    return r->at == r->len;
}

static void write_event_history(wbuf *w, const q2_save *s)
{
    size_t at;
    u32 i;

    if (!s->event_history_present) return;
    at = w_chunk_begin(w, TAG_EVHS);
    w_u32(w, s->event_history.count);
    for (i = 0; i < s->event_history.count; i++) {
        const q2_event_block *b = &s->event_history.block[i];
        const q2_event_carry *c = &b->carry;
        w_str(w, b->map, Q2_EVENT_BLOCK_NAME_MAX);
        w_u32(w, c->size);
        w_u32(w, c->record_count);
        w_u32(w, c->identity);
        w_bytes(w, c->record_flags, c->record_count);
        w_bytes(w, c->item_flags, c->size);
        w_bytes(w, c->destroyed_items, c->size);
    }
    w_chunk_end(w, at);
}

static bool read_event_history(rbuf *r, q2_save *s)
{
    u32 n = r_u32(r), i;

    if (r->bad || n > SAVE_MAX_EVENT_MAPS || s->event_history_present)
        return false;
    s->event_history_present = true;
    for (i = 0; i < n; i++) {
        char map[Q2_EVENT_BLOCK_NAME_MAX];
        q2_event_carry value = {0};
        q2_event_carry *dst;

        if (!r_need(r, sizeof(map)) || r->p[r->at + sizeof(map) - 1])
            return false;
        r_str(r, map, sizeof(map));
        value.size = r_u32(r);
        value.record_count = r_u32(r);
        value.identity = r_u32(r);
        if (r->bad || !map[0] || !valid_event_carry(&value) ||
            q2_event_store_find(&s->event_history, map) ||
            !r_need(r, (size_t)value.record_count + 2u * value.size))
            return false;
        memcpy(value.record_flags, r->p + r->at, value.record_count);
        r->at += value.record_count;
        memcpy(value.item_flags, r->p + r->at, value.size);
        r->at += value.size;
        memcpy(value.destroyed_items, r->p + r->at, value.size);
        r->at += value.size;
        dst = q2_event_store_open(&s->event_history, map);
        if (!dst) return false;
        *dst = value;
    }
    return r->at == r->len;
}

static q2_result build_body(const q2_save *s, wbuf *w)
{
    u32 i;
    if ((s->skill_present && (s->skill < 0 || s->skill > 3)) ||
        (s->viewweapon_present && !valid_viewweapon(s)) ||
        (s->event_runtime_present && !valid_event_runtime(&s->event_runtime)) ||
        s->rotator_count > SAVE_MAX_ENTITIES ||
        (s->rotator_count && !s->rotators) ||
        s->event_object_count > SAVE_MAX_EVENTS ||
        (s->event_object_count && (!s->event_object_flags ||
                                  s->event_object_count != s->event_item_count)))
        return Q2_ERR_BAD_FORMAT;
    if (s->event_history_present) {
        if (s->event_history.count > SAVE_MAX_EVENT_MAPS ||
            (s->event_history.count && !s->event_history.block))
            return Q2_ERR_BAD_FORMAT;
        for (i = 0; i < s->event_history.count; i++)
            if (!valid_event_carry(&s->event_history.block[i].carry))
                return Q2_ERR_BAD_FORMAT;
    }
    write_head(w, s);
    write_player(w, s);
    write_sim(w, s);

    {
        size_t at = w_chunk_begin(w, TAG_INVN);
        write_inventory(w, &s->inventory);
        w_chunk_end(w, at);
    }

    write_combat(w, s);
    write_viewweapon(w, s);
    write_event_runtime(w, s);
    if (s->game_rng_present) {
        size_t at = w_chunk_begin(w, TAG_RAND);
        w_u32(w, s->game_rng_state);
        w_chunk_end(w, at);
    }
    write_projectiles(w, s);
    write_bytes_chunk(w, TAG_EVNT, s->event_flags, s->event_count);
    write_bytes_chunk(w, TAG_EVIT, s->event_item_flags, s->event_item_count);
    if (s->event_object_flags)
        write_bytes_chunk(w, TAG_EOBJ, s->event_object_flags, s->event_object_count);
    write_event_history(w, s);
    write_bytes_chunk(w, TAG_TRIG, s->trigger_inside, s->trigger_count);
    write_entities(w, s);
    write_item_state(w, s);
    write_breakables(w, s);
    write_explosives(w, s);
    write_movers(w, s);
    write_rotators(w, s);
    write_creatures(w, s);
    write_ai(w, s);
    if (s->skill_present) {
        size_t at = w_chunk_begin(w, TAG_SKIL);
        w_s32(w, s->skill);
        w_chunk_end(w, at);
    }
    write_mission(w, s);
    write_level_counters(w, s);
    write_help_keys(w, s);
    write_settings(w, s);

    return w->bad ? Q2_ERR_NO_MEMORY : Q2_OK;
}

q2_result q2_save_write(const q2_save *s, const char *path)
{
    wbuf w;
    FILE *f;
    u8 header[SAVE_FILE_HEADER];
    u32 crc;
    q2_result rc;

    if (!s || !path)
        return Q2_ERR_INVALID_ARG;

    memset(&w, 0, sizeof(w));
    rc = build_body(s, &w);
    if (rc != Q2_OK) {
        w_free(&w);
        return rc;
    }

    crc = crc32_of(w.p, w.len);

    memcpy(header, Q2_SAVE_MAGIC, 4);
    header[4]  = (u8)Q2_SAVE_VERSION;
    header[5]  = header[6] = header[7] = 0;
    header[8]  = (u8)w.len;
    header[9]  = (u8)(w.len >> 8);
    header[10] = (u8)(w.len >> 16);
    header[11] = (u8)(w.len >> 24);
    header[12] = (u8)crc;
    header[13] = (u8)(crc >> 8);
    header[14] = (u8)(crc >> 16);
    header[15] = (u8)(crc >> 24);

    f = fopen(path, "wb");
    if (!f) {
        w_free(&w);
        return Q2_ERR_IO;
    }

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        (w.len && fwrite(w.p, 1, w.len, f) != w.len)) {
        fclose(f);
        w_free(&w);
        return Q2_ERR_IO;
    }

    fclose(f);
    w_free(&w);
    return Q2_OK;
}

/* Read the whole file into memory. Saves are tens of kilobytes; streaming them
 * would buy nothing and would make the CRC a second pass. */
static q2_result slurp(const char *path, u8 **out, size_t *out_size)
{
    FILE *f;
    long size;
    u8 *data;

    f = fopen(path, "rb");
    if (!f)
        return Q2_ERR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return Q2_ERR_IO;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return Q2_ERR_IO;
    }

    data = (u8 *)malloc((size_t)size ? (size_t)size : 1);
    if (!data) {
        fclose(f);
        return Q2_ERR_NO_MEMORY;
    }

    if (size && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return Q2_ERR_IO;
    }

    fclose(f);
    *out      = data;
    *out_size = (size_t)size;
    return Q2_OK;
}

/*
 * Validate the fixed header and hand back the body. Shared by the full read and
 * the header-only listing, so a slot that lists cleanly is a slot that loads.
 */
static q2_result open_body(const u8 *data, size_t size, u32 *out_version,
                           const u8 **out_body, size_t *out_body_size)
{
    u32 version, body_size, crc;

    if (size < SAVE_FILE_HEADER)
        return Q2_ERR_BAD_FORMAT;
    if (memcmp(data, Q2_SAVE_MAGIC, 4) != 0)
        return Q2_ERR_BAD_FORMAT;

    version   = q2_rd_u32(data + 4);
    body_size = q2_rd_u32(data + 8);
    crc       = q2_rd_u32(data + 12);

    if (out_version)
        *out_version = version;

    if (version < Q2_SAVE_VERSION_OLDEST || version > Q2_SAVE_VERSION) {
        Q2_ERROR("save is version %u, this build reads versions %d to %d",
                 version, Q2_SAVE_VERSION_OLDEST, Q2_SAVE_VERSION);
        return Q2_ERR_UNSUPPORTED;
    }

    if (body_size > size - SAVE_FILE_HEADER)
        return Q2_ERR_BAD_FORMAT;

    if (crc32_of(data + SAVE_FILE_HEADER, body_size) != crc) {
        Q2_ERROR("save is corrupt: checksum mismatch");
        return Q2_ERR_BAD_FORMAT;
    }

    *out_body      = data + SAVE_FILE_HEADER;
    *out_body_size = body_size;
    return Q2_OK;
}

/*
 * Walk the chunks. Each is `tag, size, payload`, and a payload is read through
 * its own cursor so a chunk that is longer than its reader expects is skipped
 * cleanly rather than desynchronising everything after it — which is the whole
 * point of sizing them.
 */
static q2_result read_body(q2_save *out, const u8 *body, size_t body_size)
{
    rbuf top;

    memset(&top, 0, sizeof(top));
    top.p   = body;
    top.len = body_size;

    while (top.at + 8 <= top.len) {
        u32 tag  = r_u32(&top);
        u32 size = r_u32(&top);
        rbuf c;

        if (top.bad || size > top.len - top.at)
            return Q2_ERR_BAD_FORMAT;

        memset(&c, 0, sizeof(c));
        c.p   = body + top.at;
        c.len = size;

        switch (tag) {
        case TAG_HEAD: read_head(&c, out);    break;
        case TAG_PLYR: read_player(&c, out);  break;
        case TAG_SIMS: read_sim(&c, out);     break;
        case TAG_INVN: read_inventory(&c, &out->inventory); break;
        case TAG_CMBT: read_combat(&c, out);  break;
        case TAG_PROJ: read_projectiles(&c, out); break;
        case TAG_MISN: read_mission(&c, out); break;
        case TAG_LVLC: read_level_counters(&c, out); break;
        case TAG_HELP: read_help_keys(&c, out); break;
        case TAG_SETT: read_settings(&c, out); break;

        case TAG_EVNT:
            if (!read_bytes_chunk(&c, &out->event_flags, &out->event_count,
                                  SAVE_MAX_EVENTS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_EVIT:
            if (!read_bytes_chunk(&c, &out->event_item_flags,
                                  &out->event_item_count, SAVE_MAX_EVENTS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_EVHS:
            if (!read_event_history(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_EOBJ:
            if (out->event_object_flags ||
                !read_bytes_chunk(&c, &out->event_object_flags,
                                  &out->event_object_count, SAVE_MAX_EVENTS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_TRIG:
            if (!read_bytes_chunk(&c, &out->trigger_inside,
                                  &out->trigger_count, SAVE_MAX_TRIGGERS))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ENTS:
            if (!read_entities(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ITEM:
            if (!read_item_state(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_BRKS:
            if (!read_breakables(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_EXPL:
            if (!read_explosives(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_MOVR:
            if (!read_movers(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_CRES:
            if (!read_creatures(&c, out))
                return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ROTR:
            if (!read_rotators(&c, out)) return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_CAIS:
            if (!read_ai(&c, out)) return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_SKIL:
            if (out->skill_present || c.len != 4) return Q2_ERR_BAD_FORMAT;
            out->skill = r_s32(&c);
            if (out->skill < 0 || out->skill > 3) return Q2_ERR_BAD_FORMAT;
            out->skill_present = true;
            break;

        case TAG_RAND:
            if (out->game_rng_present || c.len != 4) return Q2_ERR_BAD_FORMAT;
            out->game_rng_state = r_u32(&c);
            out->game_rng_present = true;
            break;

        case TAG_VWPN:
            if (!read_viewweapon(&c, out)) return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_ERTS:
            if (!read_event_runtime(&c, out)) return Q2_ERR_BAD_FORMAT;
            break;

        case TAG_PRST:
            if (!read_projectile_runtime(&c, out)) return Q2_ERR_BAD_FORMAT;
            break;

        default:
            /* An unknown chunk from a later version. Skipping it is the
             * contract that makes the format extensible. */
            break;
        }

        if (c.bad)
            return Q2_ERR_BAD_FORMAT;

        top.at += size;
    }

    if (top.at != top.len || (out->event_object_count &&
                             out->event_object_count != out->event_item_count))
        return Q2_ERR_BAD_FORMAT;

    return Q2_OK;
}

/*
 * Version 5 to 6: a spent one-shot record, stored as the runtime that wrote
 * it left it (ONESHOT|HASRUN, 0x49 for a CAT_A record), gets the DISABLED bit
 * the current latch would have given it (0x800279A8..0x800279BC), or it loads
 * armed. Exact for every v5 byte — see the note on version 6 in save.h.
 */
static void migrate_v5_event_flags(u8 *flags, u32 count)
{
    const u8 spent = (u8)(Q2_EVREC_ONESHOT | Q2_EVREC_HASRUN);
    u32 i;

    for (i = 0; flags && i < count; i++) {
        if ((flags[i] & spent) == spent)
            flags[i] |= Q2_EVREC_DISABLED;
    }
}

q2_result q2_save_read(q2_save *out, const char *path)
{
    u8 *data = NULL;
    size_t size = 0;
    const u8 *body;
    size_t body_size;
    u32 version = 0;
    q2_result rc;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    rc = open_body(data, size, &version, &body, &body_size);
    if (rc != Q2_OK) {
        free(data);
        return rc;
    }

    rc = read_body(out, body, body_size);
    free(data);

    if (rc != Q2_OK)
        q2_save_free(out);
    else if (version < 6)       /* the version EVNT changed meaning at */
        migrate_v5_event_flags(out->event_flags, out->event_count);

    return rc;
}

q2_result q2_save_read_info(q2_save_info *out, const char *path)
{
    u8 *data = NULL;
    size_t size = 0;
    const u8 *body;
    size_t body_size;
    u32 version = 0;
    q2_result rc;
    rbuf top;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    rc = open_body(data, size, &version, &body, &body_size);
    out->version = version;
    if (rc != Q2_OK) {
        free(data);
        return rc;
    }

    /*
     * Only the chunks the row needs: identity, the clock, and enough of the
     * player's condition to say what state the save is in. Everything else is
     * skipped, so listing four slots reads four headers rather than four games.
     */
    memset(&top, 0, sizeof(top));
    top.p   = body;
    top.len = body_size;

    while (top.at + 8 <= top.len) {
        u32 tag  = r_u32(&top);
        u32 csize = r_u32(&top);
        rbuf c;

        if (top.bad || csize > top.len - top.at)
            break;

        memset(&c, 0, sizeof(c));
        c.p   = body + top.at;
        c.len = csize;

        if (tag == TAG_HEAD) {
            q2_save scratch;
            memset(&scratch, 0, sizeof(scratch));
            read_head(&c, &scratch);
            memcpy(out->serial, scratch.serial, sizeof(out->serial));
            memcpy(out->map,    scratch.map,    sizeof(out->map));
            memcpy(out->label,  scratch.label,  sizeof(out->label));
            out->zone      = scratch.zone;
            out->timestamp = scratch.timestamp;
        } else if (tag == TAG_SIMS) {
            out->level_time = r_s32(&c);
        } else if (tag == TAG_INVN) {
            q2_inventory inv;
            memset(&inv, 0, sizeof(inv));
            read_inventory(&c, &inv);
            out->health = inv.health;
        } else if (tag == TAG_CMBT) {
            out->weapon_id = r_s32(&c);
        }

        top.at += csize;
    }

    free(data);
    out->used = true;
    return Q2_OK;
}

/* ------------------------------------------------------------------------- */
/* Where saves live                                                           */
/* ------------------------------------------------------------------------- */
static char g_save_dir[512];
static bool g_save_dir_set;

void q2_save_set_dir(const char *dir)
{
    if (!dir || !*dir) {
        g_save_dir[0]  = '\0';
        g_save_dir_set = false;
        return;
    }
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", dir);
    g_save_dir_set = true;
}

const char *q2_save_dir(void)
{
    const char *env;

    if (g_save_dir_set)
        return g_save_dir;

#if defined(_WIN32)
    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s/Q2PSX-PC/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
#else
    env = getenv("XDG_DATA_HOME");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir), "%s/q2psx-pc/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
    env = getenv("HOME");
    if (env && *env) {
        snprintf(g_save_dir, sizeof(g_save_dir),
                 "%s/.local/share/q2psx-pc/saves", env);
        g_save_dir_set = true;
        return g_save_dir;
    }
#endif

    /* Nothing resolved — a bare relative directory beside the executable is
     * better than refusing to save at all. */
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", "saves");
    g_save_dir_set = true;
    return g_save_dir;
}

/* Create every component of `path`. Only called on the way to a write, so a
 * session that never saves leaves nothing behind. */
static bool ensure_dir(const char *path)
{
    char work[512];
    size_t i, n;

    snprintf(work, sizeof(work), "%s", path);
    n = strlen(work);
    if (!n)
        return false;

    for (i = 1; i <= n; i++) {
        char c = work[i];

        if (c != '/' && c != '\\' && c != '\0')
            continue;

        work[i] = '\0';
        /* A drive root ("C:") is not a directory anyone creates. */
        if (!(i == 2 && work[1] == ':'))
            (void)SAVE_MKDIR(work);
        work[i] = c;
    }

    return true;
}

q2_result q2_save_slot_path(int slot, char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return Q2_ERR_INVALID_ARG;
    if (slot < 0 || slot >= Q2_SAVE_SLOTS)
        return Q2_ERR_RANGE;

    snprintf(out, out_size, "%s/slot%d.q2s", q2_save_dir(), slot);
    return Q2_OK;
}

q2_result q2_save_slot_write(const q2_save *s, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;

    ensure_dir(q2_save_dir());
    return q2_save_write(s, path);
}

q2_result q2_save_slot_read(q2_save *out, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return q2_save_read(out, path);
}

q2_result q2_save_slot_info(q2_save_info *out, int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return q2_save_read_info(out, path);
}

q2_result q2_save_slot_delete(int slot)
{
    char path[512];
    q2_result rc = q2_save_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return (remove(path) == 0) ? Q2_OK : Q2_ERR_NOT_FOUND;
}

u32 q2_save_slots_scan(q2_save_info *out, u32 count)
{
    u32 i, used = 0;

    if (!out)
        return 0;
    if (count > Q2_SAVE_SLOTS)
        count = Q2_SAVE_SLOTS;

    for (i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        /*
         * A slot that fails to parse comes back unused rather than as an error:
         * the screen has four rows either way, and a row that cannot be loaded
         * should not be selectable.
         */
        if (q2_save_slot_info(&out[i], (int)i) == Q2_OK && out[i].used)
            used++;
        else
            out[i].used = false;
    }

    return used;
}

/* ------------------------------------------------------------------------- */
/* Row text                                                                   */
/* ------------------------------------------------------------------------- */
const char *q2_save_slot_row(const q2_save_info *info, int slot,
                             char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return "";

    /*
     * An empty slot is the EMPTY STRING, not "EMPTY". The selection bar tests
     * the label against the empty string, so an unfilled row draws nothing and
     * gets no bar (memcard.h) — writing a word there would give it both.
     */
    if (!info || !info->used) {
        out[0] = '\0';
        return out;
    }

    if (info->label[0])
        snprintf(out, out_size, "%d %s", slot + 1, info->label);
    else
        snprintf(out, out_size, "%d %s", slot + 1, info->map);

    return out;
}

/* ------------------------------------------------------------------------- */
/* Multiplayer settings slots                                                */

q2_result q2_settings_slot_path(int slot, char *out, u32 out_size)
{
    if (!out || out_size == 0)
        return Q2_ERR_INVALID_ARG;
    if (slot < 0 || slot >= Q2_SAVE_SLOTS)
        return Q2_ERR_RANGE;

    snprintf(out, out_size, "%s/settings%d.q2c", q2_save_dir(), slot);
    return Q2_OK;
}

q2_result q2_settings_slot_write(const q2_settings_blob *settings, int slot)
{
    u8 file[SAVE_FILE_HEADER + 4 + Q2_SETTINGS_VALUE_MAX * 2];
    u8 *body = file + SAVE_FILE_HEADER;
    u32 body_size, crc, i;
    char path[512];
    FILE *f;
    q2_result rc;

    if (!settings)
        return Q2_ERR_INVALID_ARG;
    if (settings->count > Q2_SETTINGS_VALUE_MAX)
        return Q2_ERR_RANGE;
    rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));
    if (rc != Q2_OK)
        return rc;

    body_size = 4 + settings->count * 2;
    q2_wr_u32(body, settings->count);
    for (i = 0; i < settings->count; i++)
        q2_wr_u16(body + 4 + i * 2, (u16)settings->value[i]);
    crc = crc32_of(body, body_size);

    memcpy(file, SETTINGS_MAGIC, 4);
    q2_wr_u32(file + 4, SETTINGS_VERSION);
    q2_wr_u32(file + 8, body_size);
    q2_wr_u32(file + 12, crc);

    ensure_dir(q2_save_dir());
    f = fopen(path, "wb");
    if (!f)
        return Q2_ERR_IO;
    if (fwrite(file, 1, SAVE_FILE_HEADER + body_size, f) !=
        SAVE_FILE_HEADER + body_size) {
        fclose(f);
        return Q2_ERR_IO;
    }
    fclose(f);
    return Q2_OK;
}

q2_result q2_settings_slot_read(q2_settings_blob *out, int slot)
{
    u8 *data = NULL;
    size_t size = 0;
    u32 version, body_size, crc, count, i;
    char path[512];
    q2_result rc;

    if (!out)
        return Q2_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));
    if (rc != Q2_OK)
        return rc;
    rc = slurp(path, &data, &size);
    if (rc != Q2_OK)
        return rc;

    if (size < SAVE_FILE_HEADER || memcmp(data, SETTINGS_MAGIC, 4) != 0) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }
    version   = q2_rd_u32(data + 4);
    body_size = q2_rd_u32(data + 8);
    crc       = q2_rd_u32(data + 12);
    if (version != SETTINGS_VERSION) {
        rc = Q2_ERR_UNSUPPORTED;
        goto done;
    }
    if (body_size > size - SAVE_FILE_HEADER || body_size < 4 ||
        crc32_of(data + SAVE_FILE_HEADER, body_size) != crc) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }

    count = q2_rd_u32(data + SAVE_FILE_HEADER);
    if (count > Q2_SETTINGS_VALUE_MAX || body_size != 4 + count * 2) {
        rc = Q2_ERR_BAD_FORMAT;
        goto done;
    }
    out->count = count;
    for (i = 0; i < count; i++)
        out->value[i] = (s16)q2_rd_u16(data + SAVE_FILE_HEADER + 4 + i * 2);
    rc = Q2_OK;

done:
    free(data);
    if (rc != Q2_OK)
        memset(out, 0, sizeof(*out));
    return rc;
}

q2_result q2_settings_slot_delete(int slot)
{
    char path[512];
    q2_result rc = q2_settings_slot_path(slot, path, (u32)sizeof(path));

    if (rc != Q2_OK)
        return rc;
    return (remove(path) == 0) ? Q2_OK : Q2_ERR_NOT_FOUND;
}

u32 q2_settings_slots_scan(bool *used, u32 count)
{
    u32 i, found = 0;

    if (!used)
        return 0;
    if (count > Q2_SAVE_SLOTS)
        count = Q2_SAVE_SLOTS;
    for (i = 0; i < count; i++) {
        q2_settings_blob probe;
        used[i] = (q2_settings_slot_read(&probe, (int)i) == Q2_OK);
        if (used[i])
            found++;
    }
    return found;
}

const char *q2_save_default_label(const q2_save *s, char *out, u32 out_size)
{
    s32 seconds, minutes;

    if (!out || out_size == 0)
        return "";
    if (!s) {
        out[0] = '\0';
        return out;
    }

    /* 300 ticks to the second — the level clock every duration in the game is
     * expressed in (sim.h). */
    seconds = s->level_time / 300;
    minutes = seconds / 60;
    seconds %= 60;

    snprintf(out, out_size, "%s %d  %d:%02d",
             s->map[0] ? s->map : "?", (int)s->zone,
             (int)minutes, (int)seconds);
    return out;
}
