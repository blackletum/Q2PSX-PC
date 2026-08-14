#include "ai.h"

#include <stdlib.h>
#include <string.h>

#include "aimove.h"
#include "trig.h"

/* ------------------------------------------------------------------------- */
/* The stand-in world                                                         */
/* ------------------------------------------------------------------------- */
/*
 * Everything is visible, nothing blocks, and there is always ground. That is a
 * deliberate choice of failure: monsters that see through walls are obvious on
 * sight, where monsters that are silently blind look like the AI itself is
 * broken. Installing a real world through q2_ai_set_world replaces all three.
 */
static void open_trace(void *user, const s32 start[3], const s16 mins[3],
                       const s16 maxs[3], const s32 end[3],
                       const q2_monster *ignore, u32 mask, q2_ai_trace *out)
{
    (void)user; (void)mins; (void)maxs; (void)ignore; (void)mask;

    out->allsolid   = false;
    out->startsolid = false;
    out->ent        = NULL;

    /*
     * A floor, and nothing else.
     *
     * SV_movestep probes downward from a step height above where it wants to
     * be to a step height below, and treats a probe that runs the whole way as
     * having walked off a ledge. So a world that answers "clear" to everything
     * is not an empty world — it is a world with no ground, in which no walker
     * can take a single step. The stand-in therefore stops a downward probe
     * halfway, which is a flat floor at exactly the height the creature is
     * already standing at.
     *
     * Y points down, so a downward probe is one whose end is greater.
     */
    if (end[1] > start[1]) {
        out->fraction  = Q2_TRACE_ONE / 2;
        out->endpos[0] = end[0];
        out->endpos[1] = (start[1] + end[1]) / 2;
        out->endpos[2] = end[2];
        return;
    }

    out->fraction   = Q2_TRACE_ONE;
    out->endpos[0]  = end[0];
    out->endpos[1]  = end[1];
    out->endpos[2]  = end[2];
}

static bool open_los(void *user, const s32 a[3], const s32 b[3])
{
    (void)user; (void)a; (void)b;
    return true;
}

static bool open_bottom(void *user, const q2_monster *m)
{
    (void)user; (void)m;
    return true;
}

static const q2_ai_world g_open_world = {
    NULL, open_trace, open_los, open_bottom
};

static q2_ai_world g_world = { NULL, open_trace, open_los, open_bottom };

void q2_ai_set_world(const q2_ai_world *w)
{
    if (!w) {
        g_world = g_open_world;
        return;
    }

    g_world       = *w;
    if (!g_world.trace)         g_world.trace         = open_trace;
    if (!g_world.line_of_sight) g_world.line_of_sight = open_los;
    if (!g_world.check_bottom)  g_world.check_bottom  = open_bottom;
}

const q2_ai_world *q2_ai_get_world(void)
{
    return &g_world;
}

/* ------------------------------------------------------------------------- */
/* The globals ai_checkattack publishes — gp+17868..17880                     */
/* ------------------------------------------------------------------------- */
bool          q2_enemy_vis;
s16           q2_enemy_yaw;
bool          q2_enemy_infront;
q2_range_band q2_enemy_range;

/* ------------------------------------------------------------------------- */
/* Fixed-point helpers                                                        */
/* ------------------------------------------------------------------------- */
/* 0x8005C7B8 — one instruction. */
s32 q2_anglemod(s32 a)
{
    return a & 0xFFF;
}

/*
 * 0x8005F8E8 — yaw from an offset.
 *
 * The ground plane is XZ, so this is atan2(x, z) and not id's atan2(y, x). The
 * zero case tests z then x, in that order, exactly as the original does.
 */
s16 q2_vectoyaw(const s32 v[3])
{
    s32 best = 0;
    s64 best_dot;
    s32 a;

    if (!v)
        return 0;

    if (v[2] == 0 && v[0] == 0)
        return 0;

    /*
     * The original reaches a table-driven atan2 here. Searching the same sine
     * table for the angle whose direction best matches gives the same answer
     * to the unit and keeps every intermediate in fixed point, which matters
     * because the result is compared for exact equality in M_ChangeYaw.
     */
    best_dot = INT64_MIN;
    for (a = 0; a < Q2_ANGLE_360; a += 16) {
        s64 dot = (s64)q2_sin12(a) * v[0] + (s64)q2_cos12(a) * v[2];
        if (dot > best_dot) {
            best_dot = dot;
            best = a;
        }
    }
    for (a = best - 16; a <= best + 16; a++) {
        s64 dot = (s64)q2_sin12(a) * v[0] + (s64)q2_cos12(a) * v[2];
        if (dot > best_dot) {
            best_dot = dot;
            best = a;
        }
    }

    return (s16)q2_anglemod(best);
}

/* Integer square root, the shape 0x8008A7E8 has. */
static s32 isqrt64(s64 n)
{
    s64 x, y;

    if (n <= 0)
        return 0;

    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return (s32)x;
}

/*
 * 0x8005C4E8 / 0x8005C59C — length and length squared.
 *
 * Both refuse any component whose magnitude reaches 26755 (0xB504, the square
 * root of 2^31) and return that same 0xB504 rather than overflowing. That
 * saturation is observable: two entities far enough apart compare equal in
 * range regardless of how far apart they actually are.
 */
#define Q2_LEN_LIMIT 26755

s32 q2_vector_length(const s32 v[3])
{
    if (!v)
        return 0;

    if (llabs((long long)v[0]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[1]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[2]) >= Q2_LEN_LIMIT)
        return 0xB504;

    return isqrt64((s64)v[0] * v[0] + (s64)v[1] * v[1] + (s64)v[2] * v[2]);
}

s64 q2_vector_length_sq(const s32 v[3])
{
    if (!v)
        return 0;

    if (llabs((long long)v[0]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[1]) >= Q2_LEN_LIMIT ||
        llabs((long long)v[2]) >= Q2_LEN_LIMIT)
        return 0xB504;

    return (s64)v[0] * v[0] + (s64)v[1] * v[1] + (s64)v[2] * v[2];
}

/* 0x8005C634 — normalise in place to 1.12, returning the old length. */
s32 q2_vector_normalize(s32 v[3])
{
    s32 len;

    if (!v)
        return 0;

    len = q2_vector_length(v);
    if (len <= 0) {
        v[0] = v[1] = v[2] = 0;
        return 0;
    }

    v[0] = (s32)(((s64)v[0] << 12) / len);
    v[1] = (s32)(((s64)v[1] << 12) / len);
    v[2] = (s32)(((s64)v[2] << 12) / len);

    return len;
}

/*
 * 0x8005BB58 — forward, right and up from a yaw.
 *
 * Only yaw is used by anything the AI does; the creature's pitch and roll stay
 * zero. Forward is (sin, 0, cos) to match M_walkmove's move vector, and right
 * is forward turned a quarter circle so that the corner peek's +192 lands on
 * the creature's right hand.
 */
void q2_angle_vectors(const s16 angles[3], s32 forward[3], s32 right[3],
                      s32 up[3])
{
    s32 yaw = angles ? q2_anglemod(angles[2]) : 0;
    s32 s   = q2_sin12(yaw);
    s32 c   = q2_cos12(yaw);

    if (forward) {
        forward[0] = s;
        forward[1] = 0;
        forward[2] = c;
    }
    if (right) {
        right[0] =  c;
        right[1] =  0;
        right[2] = -s;
    }
    if (up) {
        up[0] =  0;
        up[1] = -4096;      /* Y points down, so up is negative Y */
        up[2] =  0;
    }
}

/*
 * 0x8005F5FC — a local (forward, up, right) triple placed in the world.
 *
 * The offset is three s16 in the original, written straight onto the stack by
 * the caller, and its components are in that order: ai_run fills
 * (d2, 0, ±192) to peek a distance ahead and a body's width to one side.
 */
void q2_project_source(const s32 point[3], const s16 distance[3],
                       const s32 forward[3], const s32 right[3],
                       s32 out[3])
{
    int i;

    if (!point || !distance || !out)
        return;

    for (i = 0; i < 3; i++) {
        s64 v = (s64)point[i] << 12;
        if (forward) v += (s64)forward[i] * distance[0];
        if (right)   v += (s64)right[i]   * distance[2];
        out[i] = (s32)((v + 2048) >> 12);
    }
}

/* ------------------------------------------------------------------------- */
/* visible — 0x8005B950                                                       */
/* ------------------------------------------------------------------------- */
bool q2_visible(const q2_monster *self, const q2_monster *other)
{
    s32 spot1[3], spot2[3];

    if (!self || !other)
        return false;

    spot1[0] = self->pos[0];
    spot1[1] = self->pos[1] + self->view_height;
    spot1[2] = self->pos[2];

    spot2[0] = other->pos[0];
    spot2[1] = other->pos[1] + other->view_height;
    spot2[2] = other->pos[2];

    return g_world.line_of_sight(g_world.user, spot1, spot2);
}

/* ------------------------------------------------------------------------- */
/* The player's breadcrumb trail — 0x800D517C, head at gp+17892              */
/* ------------------------------------------------------------------------- */
static q2_trail_spot g_trail[Q2_TRAIL_LENGTH];
static int  g_trail_head;
static bool g_trail_active;

#define TRAIL_NEXT(i) (((i) + 1) & (Q2_TRAIL_LENGTH - 1))
#define TRAIL_PREV(i) (((i) - 1) & (Q2_TRAIL_LENGTH - 1))

void q2_trail_init(void)
{
    memset(g_trail, 0, sizeof(g_trail));
    g_trail_head   = 0;
    g_trail_active = false;
}

void q2_trail_add(const s32 origin[3], s16 yaw)
{
    q2_trail_spot *s;

    if (!origin)
        return;

    g_trail_head = TRAIL_NEXT(g_trail_head);
    s = &g_trail[g_trail_head];

    s->origin[0] = origin[0];
    s->origin[1] = origin[1];
    s->origin[2] = origin[2];
    s->timestamp = q2_level_state.time;
    s->yaw       = yaw;
    s->valid     = true;

    g_trail_active = true;
}

/*
 * 0x80060BBC — the oldest spot the creature has not already been sent to.
 *
 * Walks forward from the head until it finds a spot newer than the creature's
 * own trail_time, then prefers that one if it can see it, the one before it if
 * it can see that instead, and falls back to the first either way. The
 * fallback is not a mistake: a creature that can see neither still needs
 * somewhere to walk.
 */
const q2_trail_spot *q2_trail_pick_first(q2_monster *self)
{
    int marker, n, prev;

    if (!g_trail_active || !self)
        return NULL;

    marker = g_trail_head;
    for (n = Q2_TRAIL_LENGTH; n; n--) {
        if (g_trail[marker].timestamp <= self->trail_time)
            marker = TRAIL_NEXT(marker);
        else
            break;
    }

    /* The original tests `visible(self, trail[marker])` against a real entity;
     * with spots rather than entities that reduces to a point sight check from
     * the creature's eye. */
    {
        s32 eye[3];
        eye[0] = self->pos[0];
        eye[1] = self->pos[1] + self->view_height;
        eye[2] = self->pos[2];

        if (g_trail[marker].valid
            && g_world.line_of_sight(g_world.user, eye, g_trail[marker].origin))
            return &g_trail[marker];

        prev = TRAIL_PREV(marker);
        if (g_trail[prev].valid
            && g_world.line_of_sight(g_world.user, eye, g_trail[prev].origin))
            return &g_trail[prev];
    }

    return g_trail[marker].valid ? &g_trail[marker] : NULL;
}

/*
 * 0x80060F90 — the next spot along, or NULL when the trail runs out.
 *
 * The original bails immediately when the trail is inactive, which is the only
 * part of it that survives in the image as a distinct function; the walk
 * itself shares PickFirst's loop.
 */
const q2_trail_spot *q2_trail_pick_next(q2_monster *self)
{
    int marker, n;

    if (!g_trail_active || !self)
        return NULL;

    marker = g_trail_head;
    for (n = Q2_TRAIL_LENGTH; n; n--) {
        if (g_trail[marker].timestamp <= self->trail_time)
            marker = TRAIL_NEXT(marker);
        else
            break;
    }

    if (!g_trail[marker].valid)
        return NULL;

    return &g_trail[marker];
}

/* ------------------------------------------------------------------------- */
/* random() — the game's 0..1 in fixed point                                  */
/* ------------------------------------------------------------------------- */
/*
 * The original computes `(rand() & 0x7fff) * n / 32767` with a magic-number
 * division, which is `random() * n` written for a machine with no divider.
 * Keeping the same form keeps the same distribution and the same endpoints:
 * a full 0x7fff yields exactly n.
 */
static s32 rand15(void)
{
    return (s32)(rand() & 0x7FFF);
}

static s32 random_scaled(s32 n)
{
    return (s32)(((s64)rand15() * n) / 32767);
}

/* ------------------------------------------------------------------------- */
/* AttackFinished — 0x800622D0                                                */
/* ------------------------------------------------------------------------- */
void q2_attack_finished(q2_monster *self, s32 t)
{
    if (self)
        self->attack_finished = q2_level_state.time + t;
}

/* ------------------------------------------------------------------------- */
/* HuntTarget / FoundTarget — inline at 0x8005DE8C, and 0x8005D104            */
/* ------------------------------------------------------------------------- */
void q2_hunt_target(q2_monster *self)
{
    s32 v[3];

    if (!self || !self->enemy)
        return;

    self->goalentity = self->enemy;

    if (self->aiflags & Q2_AI_STAND_GROUND) {
        if (self->stand)
            self->stand(self);
    } else {
        if (self->run)
            self->run(self);
    }

    v[0] = self->enemy->pos[0] - self->pos[0];
    v[1] = self->enemy->pos[1] - self->pos[1];
    v[2] = self->enemy->pos[2] - self->pos[2];
    self->ideal_yaw = q2_vectoyaw(v);

    /* Wait a beat before the first attack, so a monster that has just turned
     * around does not fire in the same tick. */
    if (!(self->aiflags & Q2_AI_STAND_GROUND))
        q2_attack_finished(self, Q2_AI_SECONDS(1));
}

/* The enemy's position as the AI records it: a client reports its own, which
 * is the branch on entity+0x3C at 0x8005D150. */
static void enemy_position(const q2_monster *e, s32 out[3])
{
    out[0] = e->pos[0];
    out[1] = e->pos[1];
    out[2] = e->pos[2];
}

void q2_found_target(q2_monster *self)
{
    if (!self || !self->enemy)
        return;

    /* Let other monsters see this monster for a while. */
    if (self->enemy->client) {
        q2_level_state.sight_entity          = self;
        q2_level_state.sight_entity_framenum = q2_level_state.framenum;
    }

    self->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);

    enemy_position(self->enemy, self->last_sighting);
    self->trail_time = q2_level_state.time;

    if (self->blindfire) {
        self->blind_target[0] = self->enemy->pos[0];
        self->blind_target[1] = self->enemy->pos[1];
        self->blind_target[2] = self->enemy->pos[2];
        self->spawnflags &= ~0xFFu;
    }

    if (self->combattarget <= 0) {
        q2_hunt_target(self);
        return;
    }

    self->movetarget = q2_pick_target(self->combattarget);
    self->goalentity = self->movetarget;

    if (!self->movetarget) {
        self->goalentity = self->enemy;
        self->movetarget = self->enemy;
        q2_hunt_target(self);
        return;
    }

    /* A combat point is a one-shot deal: claim it, clear the name so no other
     * creature takes it, and drop the pause so standing does not resume. */
    self->combattarget = 0;
    self->aiflags |= Q2_AI_COMBAT_POINT;
    self->movetarget->targetname = 0;
    self->pausetime = 0;

    if (self->run)
        self->run(self);
}

/* ------------------------------------------------------------------------- */
/* FindTarget — 0x8005D334                                                    */
/* ------------------------------------------------------------------------- */
/* The class id of the noise entity a player's own footsteps spawn. Read as the
 * equality at 0x8005D784. */
#define Q2_CLASS_PLAYER_NOISE 201
/* The class id of a scripted actor, which a good guy will not shoot. */
#define Q2_CLASS_ACTOR        204

bool q2_find_target(q2_monster *self)
{
    q2_monster *client;
    bool heardit = false;
    q2_range_band r;
    s32 temp[3];

    if (!self)
        return false;

    if (self->aiflags & Q2_AI_GOOD_GUY) {
        /* A friendly follows its goal and never picks a fight on its own. The
         * actor test is present in the original and both arms return false;
         * it is kept because its shape is what identifies the class id. */
        if (self->goalentity && q2_ent_inuse(self->goalentity)
            && self->goalentity->class_id != 0) {
            if (self->goalentity->class_id == Q2_CLASS_ACTOR)
                return false;
        }
        return false;
    }

    if (self->aiflags & Q2_AI_COMBAT_POINT)
        return false;

    /*
     * Three ways to be alerted, tried in order of how recently they happened.
     * The ambush spawnflag suppresses the two that are second-hand: an ambush
     * monster wakes for a loud noise but not for another monster's sighting
     * and not for a quiet one.
     */
    if (q2_level_state.sight_entity_framenum >= q2_level_state.framenum - 1
        && !(self->spawnflags & Q2_SPAWNFLAG_AMBUSH)) {
        client = q2_level_state.sight_entity;
        if (!client)
            return false;
        if (client->enemy == self->enemy)
            return false;
    } else if (q2_level_state.sound_entity_framenum
               >= q2_level_state.framenum - 1) {
        client  = q2_level_state.sound_entity;
        heardit = true;
    } else if (!self->enemy
               && q2_level_state.sound2_entity_framenum
                  >= q2_level_state.framenum - 1
               && !(self->spawnflags & Q2_SPAWNFLAG_AMBUSH)) {
        client  = q2_level_state.sound2_entity;
        heardit = true;
    } else {
        client = q2_level_state.sight_client;
        if (!client)
            return false;
    }

    if (!q2_ent_inuse(client))
        return false;

    if (client == self->enemy)
        return true;            /* already got one */

    if (client->client) {
        if (client->flags & Q2_FL_NOTARGET)
            return false;
    } else if (client->svflags & Q2_SVF_MONSTER) {
        if (!client->enemy)
            return false;
        if (client->enemy->flags & Q2_FL_NOTARGET)
            return false;
    } else if (!heardit) {
        return false;
    }

    if (!heardit) {
        r = q2_range(self, client);
        if (r == Q2_RANGE_FAR)
            return false;

        if (!q2_visible(self, client))
            return false;

        if (r == Q2_RANGE_NEAR) {
            /* Close by, a monster that has recently been hostile is noticed
             * even from behind — that is what show_hostile buys. */
            if (client->show_hostile < q2_level_state.time
                && !q2_infront(self, client))
                return false;
        } else if (r == Q2_RANGE_MID) {
            if (!q2_infront(self, client))
                return false;
        }

        self->enemy = client;

        if (self->enemy->class_id != Q2_CLASS_PLAYER_NOISE) {
            self->aiflags &= ~(u32)Q2_AI_SOUND_TARGET;
            if (!self->enemy->client) {
                self->enemy = self->enemy->enemy;
                if (!self->enemy || !self->enemy->client) {
                    self->enemy = NULL;
                    return false;
                }
            }
        }
    } else {
        /* Heard rather than saw. An ambush monster still needs line of sight
         * even to a noise, which is the whole point of the flag. */
        if ((self->spawnflags & Q2_SPAWNFLAG_AMBUSH)
            && !q2_visible(self, client))
            return false;

        temp[0] = client->pos[0] - self->pos[0];
        temp[1] = client->pos[1] - self->pos[1];
        temp[2] = client->pos[2] - self->pos[2];

        if (q2_vector_length_sq(temp)
            > (s64)Q2_RANGE_MID_DIST * Q2_RANGE_MID_DIST)
            return false;

        self->ideal_yaw = q2_vectoyaw(temp);
        q2_M_ChangeYaw(self);

        self->enemy = client;
        self->aiflags |= Q2_AI_SOUND_TARGET;
    }

    q2_found_target(self);

    if (!(self->aiflags & Q2_AI_SOUND_TARGET) && self->sight)
        self->sight(self, self->enemy);

    return true;
}

/* ------------------------------------------------------------------------- */
/* FacingIdeal — inline at 0x8005E274                                         */
/* ------------------------------------------------------------------------- */
bool q2_facing_ideal(const q2_monster *m)
{
    s32 delta;

    if (!m)
        return false;

    delta = q2_anglemod(m->angles[2] - m->ideal_yaw);

    /* 513..3583 is "more than 45 degrees either way". The bounds are the
     * original's own, produced by an unsigned compare against 3071. */
    if (delta >= 513 && delta <= 3583)
        return false;

    return true;
}

/* ------------------------------------------------------------------------- */
/* ai_run_missile / ai_run_melee — inline in ai_checkattack                    */
/* ------------------------------------------------------------------------- */
void q2_ai_run_missile(q2_monster *m)
{
    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    if (q2_facing_ideal(m)) {
        if (m->attack)
            m->attack(m);
        m->attack_state = Q2_AS_STRAIGHT;
    }
}

void q2_ai_run_melee(q2_monster *m)
{
    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    if (q2_facing_ideal(m)) {
        if (m->melee)
            m->melee(m);
        m->attack_state = Q2_AS_STRAIGHT;
    }
}

/* ------------------------------------------------------------------------- */
/* ai_run_slide — inline in ai_run at 0x8005E460                              */
/* ------------------------------------------------------------------------- */
void q2_ai_run_slide(q2_monster *m, s32 dist)
{
    s32 ofs;

    if (!m)
        return;

    m->ideal_yaw = q2_enemy_yaw;
    q2_M_ChangeYaw(m);

    ofs = m->lefty ? Q2_ANGLE_90 : -Q2_ANGLE_90;

    if (q2_M_walkmove(m, m->ideal_yaw + ofs, dist))
        return;

    /* Blocked on that hand, so try the other and remember which way to lead
     * with next time. */
    m->lefty = !m->lefty;
    q2_M_walkmove(m, m->ideal_yaw - ofs, dist);
}

/* ------------------------------------------------------------------------- */
/* ai_checkattack — 0x8005DCFC                                                */
/* ------------------------------------------------------------------------- */
#define Q2_PAUSE_FOREVER 1000000000

static void note_sighting(q2_monster *self)
{
    enemy_position(self->enemy, self->last_sighting);
    self->aiflags &= ~(u32)Q2_AI_LOST_SIGHT;
    self->trail_time = q2_level_state.time;

    if (self->blindfire) {
        self->blind_target[0] = self->enemy->pos[0];
        self->blind_target[1] = self->enemy->pos[1];
        self->blind_target[2] = self->enemy->pos[2];
        self->spawnflags &= ~0xFFu;
    }
}

bool q2_ai_checkattack(q2_monster *m, s32 dist)
{
    s32 temp[3];
    bool hes_dead_jim;

    if (!m)
        return false;

    (void)dist;

    /* Running blindly to a combat point: do not fire on the way. */
    if (m->goalentity) {
        if (m->aiflags & Q2_AI_COMBAT_POINT)
            return false;

        if (m->aiflags & Q2_AI_SOUND_TARGET) {
            if (m->enemy
                && q2_level_state.time - m->enemy->teleport_time > 50) {
                /* The noise is stale. Drop it and go back to what we were
                 * doing, releasing any temporary stand-ground with it. */
                if (m->goalentity == m->enemy)
                    m->goalentity = m->movetarget ? m->movetarget : NULL;

                m->aiflags &= ~(u32)Q2_AI_SOUND_TARGET;
                if (m->aiflags & Q2_AI_TEMP_STAND_GROUND)
                    m->aiflags &= ~(u32)(Q2_AI_STAND_GROUND
                                       | Q2_AI_TEMP_STAND_GROUND);
            } else {
                m->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);
                return false;
            }
        }
    }

    q2_enemy_vis = false;

    /* See if the enemy is dead. */
    hes_dead_jim = false;
    if (!m->enemy || !q2_ent_inuse(m->enemy)) {
        hes_dead_jim = true;
    } else if (m->aiflags & Q2_AI_MEDIC) {
        /* A medic's "enemy" is a corpse to raise; it stops being interesting
         * the moment it has health again. */
        if (m->enemy->health > 0) {
            hes_dead_jim = true;
            m->aiflags &= ~(u32)Q2_AI_MEDIC;
        }
    } else if (m->aiflags & Q2_AI_BRUTAL) {
        if (m->enemy->health <= -80)
            hes_dead_jim = true;
    } else {
        if (m->enemy->health <= 0)
            hes_dead_jim = true;
    }

    if (hes_dead_jim) {
        m->enemy = NULL;

        if (m->oldenemy && m->oldenemy->health > 0) {
            m->enemy    = m->oldenemy;
            m->oldenemy = NULL;
            q2_hunt_target(m);
        } else {
            if (m->movetarget) {
                m->goalentity = m->movetarget;
                if (m->walk)
                    m->walk(m);
            } else {
                /*
                 * The pause is what keeps a monster with nothing to do from
                 * wandering: without it ai_stand falls straight through to
                 * walking and the creature hunts the world entity.
                 */
                m->pausetime = q2_level_state.time + Q2_PAUSE_FOREVER;
                if (m->stand)
                    m->stand(m);
            }
            return true;
        }
    }

    if (!m->enemy)
        return true;

    m->show_hostile = q2_level_state.time + Q2_AI_SECONDS(1);   /* wake others */

    q2_enemy_vis = q2_visible(m, m->enemy);
    if (q2_enemy_vis) {
        m->search_time = q2_level_state.time + Q2_AI_SECONDS(5);
        note_sighting(m);
    }

    q2_enemy_infront = q2_infront(m, m->enemy);
    q2_enemy_range   = q2_range(m, m->enemy);

    temp[0] = m->enemy->pos[0] - m->pos[0];
    temp[1] = m->enemy->pos[1] - m->pos[1];
    temp[2] = m->enemy->pos[2] - m->pos[2];
    q2_enemy_yaw = q2_vectoyaw(temp);

    if (m->attack_state == Q2_AS_MISSILE) {
        q2_ai_run_missile(m);
        return true;
    }
    if (m->attack_state == Q2_AS_MELEE) {
        q2_ai_run_melee(m);
        return true;
    }

    /* Never attack something that cannot be seen. */
    if (!q2_enemy_vis)
        return false;

    if (!m->checkattack)
        return false;

    return m->checkattack(m);
}

/* ------------------------------------------------------------------------- */
/* ai_move — 0x8005ED44                                                       */
/* ------------------------------------------------------------------------- */
void q2_ai_move(q2_monster *m, s32 dist)
{
    /* Pure animation-driven movement, no turning and no thinking: used by
     * pain and death, which is why it must not run FindTarget. */
    if (m)
        q2_M_walkmove(m, m->angles[2], dist);
}

/* ------------------------------------------------------------------------- */
/* ai_stand — 0x8005CDA8                                                      */
/* ------------------------------------------------------------------------- */
void q2_ai_stand(q2_monster *m, s32 dist)
{
    s32 v[3];

    if (!m)
        return;

    if (dist)
        q2_M_walkmove(m, m->angles[2], dist);

    if (m->aiflags & Q2_AI_STAND_GROUND) {
        if (m->enemy) {
            bool acted;

            v[0] = m->enemy->pos[0] - m->pos[0];
            v[1] = m->enemy->pos[1] - m->pos[1];
            v[2] = m->enemy->pos[2] - m->pos[2];
            m->ideal_yaw = q2_vectoyaw(v);

            /*
             * A creature that only stopped because it was told to will start
             * moving again the moment it has to turn to keep facing — which is
             * how a guard peels off its post when you walk past it.
             */
            if (m->angles[2] != m->ideal_yaw
                && (m->aiflags & Q2_AI_TEMP_STAND_GROUND)) {
                m->aiflags &= ~(u32)(Q2_AI_STAND_GROUND
                                   | Q2_AI_TEMP_STAND_GROUND);
                if (m->run)
                    m->run(m);
            }

            if (!(m->aiflags & Q2_AI_MANUAL_STEERING))
                q2_M_ChangeYaw(m);

            acted = q2_ai_checkattack(m, 0);

            if (m->enemy && (m->enemy->spawnflags & Q2_SVFLAG_INUSE)
                && q2_visible(m, m->enemy)) {
                note_sighting(m);
            } else if (!acted) {
                q2_find_target(m);
            }
        } else {
            q2_find_target(m);
        }
        return;
    }

    if (q2_find_target(m))
        return;

    if (m->pausetime < q2_level_state.time) {
        if (m->walk)
            m->walk(m);
        return;
    }

    /*
     * Idle chatter. The first call only arms the timer, so a monster does not
     * grunt the instant the level starts; every one after it also plays the
     * sound. 15 to 30 seconds, in tenths.
     */
    if (!(m->spawnflags & Q2_SPAWNFLAG_AMBUSH) && m->idle
        && m->idle_time < q2_level_state.time) {
        if (m->idle_time) {
            m->idle(m);
            m->idle_time = q2_level_state.time + 150 + random_scaled(150);
        } else {
            m->idle_time = q2_level_state.time + random_scaled(150);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ai_walk — 0x8005ED6C                                                       */
/* ------------------------------------------------------------------------- */
void q2_ai_walk(q2_monster *m, s32 dist)
{
    if (!m)
        return;

    q2_M_MoveToGoal(m, dist);

    /* Check for noticing a player. */
    if (q2_find_target(m))
        return;

    /* Walking uses `search`, where standing uses `idle` — two different
     * sounds, and mixing them up makes a patrolling monster sound bored. */
    if (m->search && m->idle_time < q2_level_state.time) {
        if (m->idle_time) {
            m->search(m);
            m->idle_time = q2_level_state.time + 150 + random_scaled(150);
        } else {
            m->idle_time = q2_level_state.time + random_scaled(150);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ai_charge — 0x8005EE84                                                     */
/* ------------------------------------------------------------------------- */
void q2_ai_charge(q2_monster *m, s32 dist)
{
    s32 v[3];

    if (!m)
        return;

    if (m->blindfire && m->enemy && q2_visible(m, m->enemy)) {
        m->blind_target[0] = m->enemy->pos[0];
        m->blind_target[1] = m->enemy->pos[1];
        m->blind_target[2] = m->enemy->pos[2];
    }

    if (!(m->aiflags & Q2_AI_MANUAL_STEERING) && m->enemy) {
        v[0] = m->enemy->pos[0] - m->pos[0];
        v[1] = m->enemy->pos[1] - m->pos[1];
        v[2] = m->enemy->pos[2] - m->pos[2];
        m->ideal_yaw = q2_vectoyaw(v);
    }

    q2_M_ChangeYaw(m);

    /* Charging commits: it keeps facing the target and does not re-path. */
    if (dist)
        q2_M_walkmove(m, m->angles[2], dist);
}

/* ------------------------------------------------------------------------- */
/* ai_run — 0x8005E350                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The longest function in the AI, and the one that carries the behaviour
 * people remember: a monster that loses you does not give up, it walks to
 * where it last saw you, then follows your footsteps, then leans around the
 * corner to look.
 */
void q2_ai_run(q2_monster *m, s32 dist)
{
    s32 v[3];
    q2_monster *save;
    q2_monster tempgoal;
    bool isnew = false;
    const q2_trail_spot *marker = NULL;

    if (!m)
        return;

    /* If we are going to a combat point, just proceed. */
    if (m->aiflags & Q2_AI_COMBAT_POINT) {
        q2_M_MoveToGoal(m, dist);
        return;
    }

    if (m->aiflags & Q2_AI_SOUND_TARGET) {
        if (!m->enemy)
            return;

        v[0] = m->pos[0] - m->enemy->pos[0];
        v[1] = m->pos[1] - m->enemy->pos[1];
        v[2] = m->pos[2] - m->enemy->pos[2];

        /* Close enough to the noise: stop and look around rather than walking
         * on top of it. 768 units, id's 64 at this scale. */
        if (q2_vector_length_sq(v) < 768LL * 768LL) {
            m->aiflags |= (Q2_AI_STAND_GROUND | Q2_AI_TEMP_STAND_GROUND);
            if (m->stand)
                m->stand(m);
            return;
        }

        q2_M_MoveToGoal(m, dist);

        if (!q2_find_target(m))
            return;
    }

    if (q2_ai_checkattack(m, dist))
        return;

    if (m->attack_state == Q2_AS_SLIDING) {
        q2_ai_run_slide(m, dist);
        return;
    }

    if (q2_enemy_vis) {
        q2_M_MoveToGoal(m, dist);
        note_sighting(m);
        return;
    }

    /* Give up on a search that has run long enough. */
    if (m->search_time
        && q2_level_state.time > m->search_time + Q2_AI_SECONDS(20)) {
        q2_M_MoveToGoal(m, dist);
        m->search_time = 0;
        return;
    }

    /*
     * From here the creature is chasing a memory. It borrows a scratch entity
     * as its goal so that M_MoveToGoal has something with an origin to walk
     * at, and puts the real one back before returning.
     */
    save = m->goalentity;
    q2_monster_init(&tempgoal);
    tempgoal.in_use      = true;
    tempgoal.spawnflags |= Q2_SVFLAG_INUSE;
    m->goalentity = &tempgoal;

    if (!(m->aiflags & Q2_AI_LOST_SIGHT)) {
        /* Just lost sight of the player: decide where to go first. */
        m->aiflags |= (Q2_AI_LOST_SIGHT | Q2_AI_PURSUIT_LAST_SEEN);
        m->aiflags &= ~(u32)(Q2_AI_PURSUE_NEXT | Q2_AI_PURSUE_TEMP);
        isnew = true;
    }

    if (m->aiflags & Q2_AI_PURSUE_NEXT) {
        m->aiflags &= ~(u32)Q2_AI_PURSUE_NEXT;

        /* Getting this far earns more search time. */
        m->search_time = q2_level_state.time + Q2_AI_SECONDS(5);

        if (m->aiflags & Q2_AI_PURSUE_TEMP) {
            /* We were at a corner-peek waypoint; resume the real goal. */
            m->aiflags &= ~(u32)Q2_AI_PURSUE_TEMP;
            marker = NULL;
            m->last_sighting[0] = m->saved_goal[0];
            m->last_sighting[1] = m->saved_goal[1];
            m->last_sighting[2] = m->saved_goal[2];
            isnew = true;
        } else if (m->aiflags & Q2_AI_PURSUIT_LAST_SEEN) {
            m->aiflags &= ~(u32)Q2_AI_PURSUIT_LAST_SEEN;
            marker = q2_trail_pick_first(m);
        } else {
            marker = q2_trail_pick_next(m);
        }

        if (marker) {
            m->last_sighting[0] = marker->origin[0];
            m->last_sighting[1] = marker->origin[1];
            m->last_sighting[2] = marker->origin[2];
            m->trail_time  = marker->timestamp;
            m->angles[2]   = marker->yaw;
            m->ideal_yaw   = marker->yaw;
            isnew = true;
        }
    }

    v[0] = m->pos[0] - m->last_sighting[0];
    v[1] = m->pos[1] - m->last_sighting[1];
    v[2] = m->pos[2] - m->last_sighting[2];

    /* Arrived at the waypoint — within one frame's step of it — so ask for the
     * next one on the way back through. */
    if ((s64)dist * dist >= q2_vector_length_sq(v)) {
        m->aiflags |= Q2_AI_PURSUE_NEXT;
        dist = q2_vector_length(v);
    }

    tempgoal.pos[0] = m->last_sighting[0];
    tempgoal.pos[1] = m->last_sighting[1];
    tempgoal.pos[2] = m->last_sighting[2];

    if (isnew) {
        q2_ai_trace tr;

        g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                      m->last_sighting, m, Q2_MASK_PLAYERSOLID, &tr);

        if (tr.fraction < Q2_TRACE_ONE) {
            /*
             * The straight line is blocked. Look a body's width to each side
             * at roughly half the remaining distance and walk toward whichever
             * has more room — which reads, from outside, as peeking round the
             * corner.
             */
            s32 d1, d2, center, left, right;
            s32 v_forward[3], v_right[3];
            s32 left_target[3], right_target[3];
            s16 off[3];

            v[0] = m->goalentity->pos[0] - m->pos[0];
            v[1] = m->goalentity->pos[1] - m->pos[1];
            v[2] = m->goalentity->pos[2] - m->pos[2];
            d1 = q2_vector_length(v);

            center = tr.fraction;
            d2 = (s32)(((s64)d1 * ((center + Q2_TRACE_ONE) / 2)) >> 12);

            m->ideal_yaw = q2_vectoyaw(v);
            m->angles[2] = m->ideal_yaw;

            q2_angle_vectors(m->angles, v_forward, v_right, NULL);

            off[0] = (s16)d2;  off[1] = 0;  off[2] = -192;
            q2_project_source(m->pos, off, v_forward, v_right, left_target);
            g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                          left_target, m, Q2_MASK_PLAYERSOLID, &tr);
            left = tr.fraction;

            off[0] = (s16)d2;  off[1] = 0;  off[2] = 192;
            q2_project_source(m->pos, off, v_forward, v_right, right_target);
            g_world.trace(g_world.user, m->pos, m->mins, m->maxs,
                          right_target, m, Q2_MASK_PLAYERSOLID, &tr);
            right = tr.fraction;

            center = d2 ? (s32)(((s64)d1 * center) / d2) : center;

            if (left >= center && left > right) {
                if (left < Q2_TRACE_ONE) {
                    off[0] = (s16)(((s64)d2 * left) >> 13);
                    off[1] = 0;
                    off[2] = -192;
                    q2_project_source(m->pos, off, v_forward, v_right,
                                      left_target);
                }
                m->saved_goal[0] = m->last_sighting[0];
                m->saved_goal[1] = m->last_sighting[1];
                m->saved_goal[2] = m->last_sighting[2];
                m->aiflags |= Q2_AI_PURSUE_TEMP;
                tempgoal.pos[0] = left_target[0];
                tempgoal.pos[1] = left_target[1];
                tempgoal.pos[2] = left_target[2];
                m->last_sighting[0] = left_target[0];
                m->last_sighting[1] = left_target[1];
                m->last_sighting[2] = left_target[2];
                v[0] = tempgoal.pos[0] - m->pos[0];
                v[1] = tempgoal.pos[1] - m->pos[1];
                v[2] = tempgoal.pos[2] - m->pos[2];
                m->ideal_yaw = q2_vectoyaw(v);
                m->angles[2] = m->ideal_yaw;
            } else if (right >= center && right > left) {
                if (right < Q2_TRACE_ONE) {
                    off[0] = (s16)(((s64)d2 * right) >> 13);
                    off[1] = 0;
                    off[2] = 192;
                    q2_project_source(m->pos, off, v_forward, v_right,
                                      right_target);
                }
                m->saved_goal[0] = m->last_sighting[0];
                m->saved_goal[1] = m->last_sighting[1];
                m->saved_goal[2] = m->last_sighting[2];
                m->aiflags |= Q2_AI_PURSUE_TEMP;
                tempgoal.pos[0] = right_target[0];
                tempgoal.pos[1] = right_target[1];
                tempgoal.pos[2] = right_target[2];
                m->last_sighting[0] = right_target[0];
                m->last_sighting[1] = right_target[1];
                m->last_sighting[2] = right_target[2];
                v[0] = tempgoal.pos[0] - m->pos[0];
                v[1] = tempgoal.pos[1] - m->pos[1];
                v[2] = tempgoal.pos[2] - m->pos[2];
                m->ideal_yaw = q2_vectoyaw(v);
                m->angles[2] = m->ideal_yaw;
            }
        }
    }

    q2_M_MoveToGoal(m, dist);

    m->goalentity = save;
}

/* ------------------------------------------------------------------------- */
/* The verb table, in the executable's slot order — 0x800D561C               */
/* ------------------------------------------------------------------------- */
const q2_ai_verb_fn q2_ai_verbs[8] = {
    NULL,               /* 0 — the original stores zero here                 */
    q2_ai_stand,        /* 1 — 0x8005CDA8                                    */
    q2_ai_walk,         /* 2 — 0x8005ED6C                                    */
    q2_ai_run,          /* 3 — 0x8005E350                                    */
    q2_ai_charge,       /* 4 — 0x8005EE84                                    */
    q2_ai_move,         /* 5 — 0x8005ED44                                    */
    NULL,               /* 6, 7 — the shared do-nothing handler              */
    NULL
};
