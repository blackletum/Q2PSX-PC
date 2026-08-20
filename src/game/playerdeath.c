/*
 * playerdeath.c — the death chain, transcribed. See playerdeath.h for where
 * every number in here comes from.
 */
#include "playerdeath.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* The moves                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * The ten names 0x8003C5F8..0x8003CBFC looks up, in the order it looks them up
 * — which is also the order they sit in the string pool at 0x800AC554, twelve
 * bytes apart. The globals they land in are NOT in that order (Stand goes to
 * gp+17288 and Run to gp+17252), which is why the table is keyed by name.
 */
static const char *const k_move_names[Q2_PMOVE_COUNT] = {
    "Stand",    /* 0x800AC554 -> 0x800B2988 */
    "Run",      /* 0x800AC560 -> 0x800B2964 */
    "Attak",    /* 0x800AC56C -> 0x800B2990 */
    "Death 1",  /* 0x800AC578 -> 0x800B299C */
    "Death 2",  /* 0x800AC584 -> 0x800B29A4 */
    "Death 3",  /* 0x800AC590 -> 0x800B29AC */
    "Jump",     /* 0x800AC59C -> 0x800B29B4 */
    "Pain 1",   /* 0x800AC5A8 -> 0x800B2970 */
    "Pain 2",   /* 0x800AC5B4 -> 0x800B2978 */
    "Pain 3"    /* 0x800AC5C0 -> 0x800B2980 */
};

const char *q2_player_move_name(q2_player_move m)
{
    if (m < 0 || m >= Q2_PMOVE_COUNT)
        return "";
    return k_move_names[m];
}

bool q2_player_move_is_death(q2_player_move m)
{
    return m >= Q2_PMOVE_DEATH1 && m <= Q2_PMOVE_DEATH3;
}

bool q2_player_move_is_pain(q2_player_move m)
{
    return m >= Q2_PMOVE_PAIN1 && m <= Q2_PMOVE_PAIN3;
}

q2_player_move q2_player_anim_pick(q2_player_anim want, q2_player_move cur,
                                   u16 animflags, u32 roll)
{
    q2_player_move pick = Q2_PMOVE_NONE;

    /*
     * A CORPSE ANIMATES NOTHING ELSE. The function says this twice — once when
     * it chooses (0x8003CEB4 and 0x8003CE78 both fall to `s0 = 0`) and once
     * when it installs (0x8003D158 sets the lock) — so neither a pain, a run
     * nor a second death displaces the move a player died in.
     */
    if (q2_player_move_is_death(cur))
        return Q2_PMOVE_NONE;

    switch (want) {
    case Q2_PANIM_STAND:
        /*
         * 0x8003D008: Stand does NOT interrupt an attack that has not yet
         * wrapped. Bit 0 of +0x102 is "the move ran past its end", raised by
         * 0x8003DF90, so an unwrapped Attak holds the frame.
         */
        if (cur == Q2_PMOVE_ATTAK && !(animflags & Q2_PDEATH_ANIM_WRAPPED))
            return Q2_PMOVE_NONE;
        pick = Q2_PMOVE_STAND;
        break;

    case Q2_PANIM_RUN:
        pick = Q2_PMOVE_RUN;
        break;

    case Q2_PANIM_JUMP:
        pick = Q2_PMOVE_JUMP;
        break;

    case Q2_PANIM_PAIN:
        /* 0x8003CF74: `rand() % 3` over Pain 1/2/3. */
        pick = (q2_player_move)(Q2_PMOVE_PAIN1 + (int)(roll % 3u));
        break;

    case Q2_PANIM_DEATH:
        /* 0x8003CEEC: the same roll over Death 1/2/3. */
        pick = (q2_player_move)(Q2_PMOVE_DEATH1 + (int)(roll % 3u));
        break;

    case Q2_PANIM_ATTACK:
        pick = Q2_PMOVE_ATTAK;
        break;

    default:
        return Q2_PMOVE_NONE;
    }

    /*
     * 0x8003D188..0x8003D1E4: a pain move holds until it has played out, and
     * only a DEATH request may cut it short. Being killed while flinching still
     * drops the body properly, and nothing else does.
     */
    if (q2_player_move_is_pain(cur) && want != Q2_PANIM_DEATH &&
        !(animflags & Q2_PDEATH_ANIM_WRAPPED))
        return Q2_PMOVE_NONE;

    /* Asking for the move already playing is not a request to restart it. */
    if (pick == cur)
        return Q2_PMOVE_NONE;

    return pick;
}

/* ------------------------------------------------------------------------- */
/* 0x8006FE3C — step a value toward a target and clamp at it                  */
/* ------------------------------------------------------------------------- */
static void approach(s16 *v, s16 target, s32 step)
{
    if (step < 0)
        step = -step;

    if (*v < target)
        *v = (s16)((*v + step < target) ? (*v + step) : target);
    else
        *v = (s16)((*v - step > target) ? (*v - step) : target);
}

/* ------------------------------------------------------------------------- */
/* The state                                                                  */
/* ------------------------------------------------------------------------- */
void q2_player_death_init(q2_player_death *d)
{
    if (!d)
        return;

    memset(d, 0, sizeof(*d));
    d->stage = Q2_PDEATH_ALIVE;
    /* 0x8003DE34. Not -1: 4 is "not a player", and the frag hook's own
     * `killer < 4` rejects it, so a fresh spawn owes nobody a frag. */
    d->killer        = Q2_PDEATH_NO_KILLER;
    d->mod           = 0;
    d->victim        = -1;
    d->gib_health    = 0;
    d->corpse_ticks  = 0;
    d->box_y         = 0;
    d->scale         = Q2_PDEATH_SCALE_ONE;
    d->move          = Q2_PMOVE_STAND;   /* 0x8003DE48 sets the Stand move */
    d->animflags     = 0;
    d->ent2          = 1u;               /* 0x8003B428 writes exactly 1    */
    d->linked_weapon = true;
    d->has_body      = true;             /* 0x8003B474 writes the entity   */
}

bool q2_player_should_die(s16 health, u32 ent2_flags)
{
    /* 0x8003ADC0 is `bgtz`, so zero health is dead; 0x8003ADD4 skips a body
     * whose corpse think has already raised the bit. */
    return health <= 0 && !(ent2_flags & Q2_PDEATH_DEAD_BIT);
}

bool q2_player_death_cries_out(s8 killer_field, s16 means_of_death)
{
    return q2_mp_attribute_kill((int)killer_field, (int)means_of_death) < 0;
}

void q2_player_die(q2_player_death *d, s8 killer_field, s16 means_of_death,
                   int victim, bool deathmatch, bool drowning,
                   q2_player_death_event *ev)
{
    q2_player_death_event local;

    if (!ev)
        ev = &local;
    memset(ev, 0, sizeof(*ev));

    if (!d || d->stage != Q2_PDEATH_ALIVE)
        return;

    d->mod    = means_of_death;
    d->victim = victim;

    /*
     * 0x800396CC, on the entity rather than in the scoring: acid and lava erase
     * the attacker outright, so everything that reads +222 afterwards — the
     * sound choice below included — sees a death with no killer.
     */
    d->killer = (s8)q2_mp_attribute_kill((int)killer_field, (int)means_of_death);

    /*
     * 0x80039728. Only a death with NO killer cries out, and `client+0x84`
     * picks which voice. A player shot by somebody dies silently here; what the
     * port used to do — raise pla_death4 from `update_pain` for every death —
     * made every death audible and both of them the same.
     */
    if (d->killer == -1) {
        ev->cried_out = true;
        ev->drowned   = drowning;
    }

    if (deathmatch) {
        /* 0x8003976C, then the SIGNED `killer < 4 && victim < 4` at 0x80039774
         * — a world kill at -1 passes it and reaches the module. */
        ev->body_recorded = true;
        if (d->killer < Q2_MP_MAX_PLAYERS && victim < Q2_MP_MAX_PLAYERS) {
            ev->frag_hook   = true;
            ev->frag_killer = d->killer;
            ev->frag_victim = victim;
        }
    } else {
        /* 0x8002059C: page 41, armed for 600. */
        ev->death_page    = true;
        /* 0x8003982C: only from game states 1 and 2, and once. */
        ev->abandon_armed = true;
    }

    /* The tail, 0x800397D4..0x80039814. */
    d->pitch         = 0;
    d->has_body      = false;          /* the player record forgets its body */
    d->linked_weapon = false;          /* released, and the field reused     */
    d->gib_health    = Q2_PDEATH_GIB_HEALTH;
    d->corpse_ticks  = Q2_PDEATH_CORPSE_TICKS;

    /* +0x3C becomes the corpse think, which is the whole of the state change:
     * the player think that got here is no longer installed, so the gate above
     * cannot fire twice. */
    d->stage = Q2_PDEATH_DYING;
}

void q2_player_death_anim_ended(q2_player_death *d)
{
    if (d)
        d->animflags |= Q2_PDEATH_ANIM_WRAPPED;
}

/* corpse_think and respawn_think open with the same gib test, 0x8003956C and
 * 0x8003E270 — `gib_health < health` is alive, so the boundary is inclusive. */
static bool gibbed(const q2_player_death *d, s16 health)
{
    return health <= d->gib_health;
}

static void go_gibbed(q2_player_death *d, q2_player_death_event *ev)
{
    d->stage = Q2_PDEATH_GIBBED;
    d->scale = 0;
    if (ev)
        ev->gibbed = true;
}

bool q2_player_death_tick(q2_player_death *d, s16 health, s32 dt,
                          bool deathmatch, u32 roll)
{
    if (!d)
        return false;

    if (dt < 0)
        dt = 0;

    switch (d->stage) {
    case Q2_PDEATH_ALIVE:
        return true;

    case Q2_PDEATH_DYING: {
        int i;

        if (gibbed(d, health)) {
            go_gibbed(d, NULL);
            return false;
        }

        /* 0x800395A0: friction on all three axes at dt * 5. */
        for (i = 0; i < 3; i++)
            approach(&d->velocity[i], 0, dt * Q2_PDEATH_FRICTION);

        if (!deathmatch) {
            /*
             * 0x8003968C. Single player raises DEAD on the first corpse tick
             * and does nothing else, ever: respawn_think is only reached
             * through the deathmatch branch, so the body lies where it fell
             * until the level is restarted.
             */
            d->ent2 |= Q2_PDEATH_DEAD_BIT;
            return true;
        }

        if (d->animflags == 0) {
            /* 0x80039628: keep asking for the death animation. The pick
             * refuses once one is playing, which is what makes this a hold
             * rather than a restart. */
            q2_player_move want = q2_player_anim_pick(Q2_PANIM_DEATH, d->move,
                                                      d->animflags, roll);
            if (want != Q2_PMOVE_NONE) {
                d->move      = want;
                d->animflags = 0;    /* 0x8003DFE4 clears the low two bits */
            }
            return true;
        }

        /* 0x80039638: the animation has played out. */
        d->ent2  |= Q2_PDEATH_DEAD_BIT | Q2_PDEATH_SETTLED_BIT;
        d->box_y  = Q2_PDEATH_BODY_BOX_Y;
        d->stage  = Q2_PDEATH_DOWN;
        return true;
    }

    case Q2_PDEATH_DOWN:
        /* 0x8003E270: the same gib test, so a rocket into a body that is
         * already down still takes it apart. */
        if (gibbed(d, health)) {
            go_gibbed(d, NULL);
            return false;
        }

        /* 0x8003E2CC. The subtraction is on a halfword and the test is on the
         * result sign-extended, so it expires at or below zero. */
        d->corpse_ticks = (s16)(d->corpse_ticks - dt);
        if (d->corpse_ticks <= 0) {
            d->corpse_ticks = 1;      /* 0x8003E2E0 */
            d->stage        = Q2_PDEATH_FADING;
        }
        return true;

    case Q2_PDEATH_FADING:
        /* 0x8005B36C: `dt << 4` off the scale each tick, and the model is
         * released when it runs out. */
        d->scale = (s16)(d->scale - dt * Q2_PDEATH_FADE_RATE);
        if (d->scale <= 0) {
            d->scale = 0;
            d->stage = Q2_PDEATH_GONE;
            return false;
        }
        return true;

    case Q2_PDEATH_GIBBED:
    case Q2_PDEATH_GONE:
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* The two endings                                                            */
/* ------------------------------------------------------------------------- */
bool q2_player_abandon_tick(s32 *ticks, s32 dt)
{
    if (!ticks || *ticks <= 0)     /* 0x80041D38: an unarmed deadline is 0 */
        return false;

    if (dt < 0)
        dt = 0;

    *ticks -= dt;
    if (*ticks > 0)
        return false;

    /* 0x80041DB8: request 8, and the deadline is cleared rather than left
     * negative, so it fires exactly once. */
    *ticks = 0;
    return true;
}

bool q2_player_spend_resupply(int *continues)
{
    if (!continues || *continues <= 0)
        return false;

    /* 0x8001FF0C. */
    *continues -= 1;
    return true;
}
