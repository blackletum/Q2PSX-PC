#include "shock.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/*
 * The envelopes, 0x8009D258..0x8009D41F, transcribed point for point with the
 * address each run starts at and MAIN.SYM's name for it. Every word on the disc
 * is `duration << 16 | level`; a zero duration ends the run.
 *
 * Several are not one shape but a rhythm on the little motor — the shotgun's
 * 44 on, 8 off, 12 on, 52 off, 8 on is the pump — written as pairs of one-tick
 * spans that step the level between 4096 and 0 so the interpolation has
 * nothing to smooth.
 */
static const q2_shock_point k_blaster_big[] = {           /* 0x8009D258 */
    { 2048, 48 }, { 1024, 0 } };
static const q2_shock_point k_blaster_little[] = {        /* 0x8009D260 */
    { 2560, 32 }, { 1024, 0 } };
static const q2_shock_point k_shotgun_big[] = {           /* 0x8009D268 */
    { 3072, 28 }, { 4096, 0 } };
static const q2_shock_point k_shotgun_little[] = {        /* 0x8009D270 */
    { 4096, 44 }, { 4096, 1 }, { 0, 8 }, { 0, 1 }, { 4096, 12 },
    { 4096, 1 }, { 0, 52 }, { 0, 1 }, { 4096, 8 }, { 4096, 0 } };
static const q2_shock_point k_sshotgun_big[] = {          /* 0x8009D298 */
    { 4096, 44 }, { 4096, 0 } };
static const q2_shock_point k_sshotgun_little[] = {       /* 0x8009D2A0 */
    { 4096, 44 }, { 4096, 1 }, { 0, 72 }, { 0, 1 }, { 4096, 12 },
    { 4096, 1 }, { 0, 60 }, { 0, 1 }, { 4096, 12 }, { 3072, 16 },
    { 0, 0 } };
static const q2_shock_point k_machinegun_big[] = {        /* 0x8009D2CC */
    { 1536, 16 }, { 1536, 0 } };
static const q2_shock_point k_machinegun_little[] = {     /* 0x8009D2D4 */
    { 4096, 8 }, { 4096, 0 } };
static const q2_shock_point k_chaingun_big[] = {          /* 0x8009D2DC */
    { 2560, 16 }, { 2560, 0 } };
static const q2_shock_point k_chaingun_little[] = {       /* 0x8009D2E4 */
    { 4096, 12 }, { 4096, 0 } };
static const q2_shock_point k_hyperblaster_big[] = {      /* 0x8009D2EC */
    { 4096, 16 }, { 4096, 0 } };
static const q2_shock_point k_hyperblaster_little[] = {   /* 0x8009D2F4 */
    { 4096, 16 }, { 4096, 0 } };
static const q2_shock_point k_rocketl_big[] = {           /* 0x8009D2FC */
    { 4096, 64 }, { 4096, 0 } };
static const q2_shock_point k_rocketl_little[] = {        /* 0x8009D304 */
    { 4096, 16 }, { 4096, 40 }, { 0, 32 }, { 0, 1 }, { 4096, 16 },
    { 4096, 1 }, { 0, 128 }, { 0, 1 }, { 4096, 16 }, { 4096, 0 } };
static const q2_shock_point k_grenadel_big[] = {          /* 0x8009D32C */
    { 4096, 32 }, { 4096, 0 } };
static const q2_shock_point k_grenadel_little[] = {       /* 0x8009D334 */
    { 4096, 64 }, { 4096, 1 }, { 0, 80 }, { 0, 1 }, { 4096, 16 },
    { 4096, 1 }, { 0, 32 }, { 0, 1 }, { 4096, 16 }, { 4096, 0 } };
static const q2_shock_point k_grenadeopen_big[] = {       /* 0x8009D35C */
    { 1024, 128 }, { 2048, 0 } };
static const q2_shock_point k_grenadeopen_little[] = {    /* 0x8009D364 */
    { 0, 144 }, { 0, 1 }, { 4096, 32 }, { 4096, 1 }, { 0, 0 } };
static const q2_shock_point k_grenadethrow_big[] = {      /* 0x8009D378 */
    { 2048, 128 }, { 4096, 0 } };
static const q2_shock_point k_grenadethrow_little[] = {   /* 0x8009D380 */
    { 4096, 128 }, { 2048, 0 } };
static const q2_shock_point k_bfg_big[] = {               /* 0x8009D388 */
    { 1536, 208 }, { 2560, 1 }, { 4096, 96 }, { 4096, 0 } };
static const q2_shock_point k_bfg_little[] = {            /* 0x8009D398 */
    { 0, 208 }, { 0, 1 }, { 4096, 96 }, { 4096, 1 }, { 0, 74 },
    { 0, 1 }, { 4096, 40 }, { 4096, 0 } };
static const q2_shock_point k_railgun_big[] = {           /* 0x8009D3B8 */
    { 4096, 64 }, { 4096, 0 } };
static const q2_shock_point k_railgun_little[] = {        /* 0x8009D3C0 */
    { 4096, 24 }, { 4096, 1 }, { 0, 104 }, { 0, 1 }, { 4096, 8 },
    { 4096, 1 }, { 0, 200 }, { 0, 1 }, { 4096, 16 }, { 4096, 0 } };
static const q2_shock_point k_explosion_big[] = {         /* 0x8009D3E8 */
    { 2048, 128 }, { 4096, 0 } };
static const q2_shock_point k_explosion_little[] = {      /* 0x8009D3F0 */
    { 4096, 128 }, { 2048, 0 } };
static const q2_shock_point k_stomp_big[] = {             /* 0x8009D3F8 */
    { 4096, 32 }, { 4096, 32 }, { 0, 0 } };
static const q2_shock_point k_stomp_little[] = {          /* 0x8009D404 */
    { 4096, 32 }, { 4096, 0 } };
static const q2_shock_point k_impact_big[] = {            /* 0x8009D40C */
    { 4096, 16 }, { 4096, 16 }, { 0, 0 } };
static const q2_shock_point k_impact_little[] = {         /* 0x8009D418 */
    { 4096, 16 }, { 4096, 0 } };

/* The sixteen records, 0x8009D420 + 12*i. The two impacts share both runs and
 * differ only in the merge flag. */
#define REC(n, b, l, f, i, a) \
    static const q2_shock_data k_##n = { b, l, f, i, #n, a }
REC(ShockData_Blaster,          k_blaster_big,      k_blaster_little,      1, Q2_SHOCK_BLASTER,          0x8009D420u);
REC(ShockData_HyperBlaster,     k_hyperblaster_big, k_hyperblaster_little, 1, Q2_SHOCK_HYPERBLASTER,     0x8009D42Cu);
REC(ShockData_Shotgun,          k_shotgun_big,      k_shotgun_little,      1, Q2_SHOCK_SHOTGUN,          0x8009D438u);
REC(ShockData_SShotgun,         k_sshotgun_big,     k_sshotgun_little,     1, Q2_SHOCK_SSHOTGUN,         0x8009D444u);
REC(ShockData_MachineGun,       k_machinegun_big,   k_machinegun_little,   1, Q2_SHOCK_MACHINEGUN,       0x8009D450u);
REC(ShockData_ChainGun,         k_chaingun_big,     k_chaingun_little,     1, Q2_SHOCK_CHAINGUN,         0x8009D45Cu);
REC(ShockData_RocketL,          k_rocketl_big,      k_rocketl_little,      1, Q2_SHOCK_ROCKETL,          0x8009D468u);
REC(ShockData_GrenadeL,         k_grenadel_big,     k_grenadel_little,     1, Q2_SHOCK_GRENADEL,         0x8009D474u);
REC(ShockData_GrenadeOpen,      k_grenadeopen_big,  k_grenadeopen_little,  1, Q2_SHOCK_GRENADE_OPEN,     0x8009D480u);
REC(ShockData_GrenadeThrow,     k_grenadethrow_big, k_grenadethrow_little, 1, Q2_SHOCK_GRENADE_THROW,    0x8009D48Cu);
REC(ShockData_RailGun,          k_railgun_big,      k_railgun_little,      1, Q2_SHOCK_RAILGUN,          0x8009D498u);
REC(ShockData_BFG,              k_bfg_big,          k_bfg_little,          1, Q2_SHOCK_BFG,              0x8009D4A4u);
REC(ShockData_Explosion,        k_explosion_big,    k_explosion_little,    1, Q2_SHOCK_EXPLOSION,        0x8009D4B0u);
REC(ShockData_Stomp,            k_stomp_big,        k_stomp_little,        0, Q2_SHOCK_STOMP,            0x8009D4BCu);
REC(ShockData_Impact_Distinct,  k_impact_big,       k_impact_little,       0, Q2_SHOCK_IMPACT_DISTINCT,  0x8009D4C8u);
REC(ShockData_Impact_Mergeable, k_impact_big,       k_impact_little,       1, Q2_SHOCK_IMPACT_MERGEABLE, 0x8009D4D4u);
#undef REC

/* ShockDataAvailable, 0x8009D4E0: record order, which is not id order — the
 * rocket launcher (10) sits seventh and the grenade pair (7, 8) ninth and
 * tenth. */
const q2_shock_data *const q2_shock_available[Q2_SHOCK_PATTERNS + 1] = {
    &k_ShockData_Blaster,     &k_ShockData_HyperBlaster, &k_ShockData_Shotgun,
    &k_ShockData_SShotgun,    &k_ShockData_MachineGun,   &k_ShockData_ChainGun,
    &k_ShockData_RocketL,     &k_ShockData_GrenadeL,     &k_ShockData_GrenadeOpen,
    &k_ShockData_GrenadeThrow, &k_ShockData_RailGun,     &k_ShockData_BFG,
    &k_ShockData_Explosion,   &k_ShockData_Stomp,
    &k_ShockData_Impact_Distinct, &k_ShockData_Impact_Mergeable,
    NULL
};

const q2_shock_data *q2_shock_find(int id)
{
    u32 i;

    /* 0x800409EC..0x80040A20: first match wins, NULL ends the walk. */
    for (i = 0; q2_shock_available[i]; i++)
        if ((s16)q2_shock_available[i]->id == (s16)id)
            return q2_shock_available[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* SquareRootNR (0x80055CBC): a seed, then two Newton steps that each subtract
 * `(x*x - n) / (2x)` with the console's truncating divide. */
static s32 shock_sqrt(s32 n)
{
    u32 v = (u32)n, r = 0, bit = 1u << 30;
    s32 x;
    int step;

    if (n <= 0)
        return 0;

    /* The seed: an exact floor root standing in for SquareRoot0's table. */
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    x = (s32)r;

    for (step = 0; step < 2 && x != 0; step++)
        x -= (s32)((s64)x * x - n) / (2 * x);
    return x;
}

static s32 iabs32(s32 v) { return v < 0 ? -v : v; }

bool q2_shock_set(q2_shock *s, int id, s32 strength,
                  const s32 pos[3], const s32 listener[3],
                  s16 near_r, s16 far_r)
{
    const q2_shock_data *data;
    s32 str = strength;
    int i, use = Q2_SHOCK_SLOTS;

    if (!s)
        return false;

    if (pos && listener) {
        s32 dx = iabs32(pos[0] - listener[0]);
        s32 dy = iabs32(pos[1] - listener[1]);
        s32 dz = iabs32(pos[2] - listener[2]);
        s32 d2, far2, near2;

        /* 0x80040884..0x800408F4: any axis past `far` rejects outright, then
         * the squared distance against `far` squared — 32-bit products, as
         * there. */
        if (dx > far_r || dy > far_r || dz > far_r)
            return false;
        d2   = (s32)((u32)dx * (u32)dx + (u32)dy * (u32)dy + (u32)dz * (u32)dz);
        far2 = (s32)far_r * far_r;
        if (far2 < d2)
            return false;

        near2 = (s32)near_r * near_r;
        if (near2 < d2) {
            /* 0x80040960..0x800409DC: the linear falloff, never above the
             * request, and a negative result is no request at all. */
            s32 span = (s32)far_r - near_r;
            s32 d    = shock_sqrt(d2);
            s32 att  = span ? (s32)((s64)strength * (far_r - d) / span) : 0;

            if (strength < att)
                att = strength;
            str = att < 0 ? 0 : att;
        }
    }

    if (str < Q2_SHOCK_MIN_STRENGTH)
        return false;

    data = q2_shock_find(id);
    if (!data)
        return false;

    if (data->flags & Q2_SHOCK_FLAG_MERGE) {
        /*
         * 0x80040A44..0x80040A98: a slot already playing this id is restarted.
         * Failing that, the LAST free slot the scan passed — `t1` is
         * overwritten on every empty slot, not just the first.
         */
        int free_slot = Q2_SHOCK_SLOTS;

        for (i = 0; i < Q2_SHOCK_SLOTS; i++) {
            const q2_shock_data *d = s->slot[i].data;

            if (d && (s16)d->id == (s16)data->id) {
                use = i;
                break;
            }
            if (!d)
                free_slot = i;
        }
        if (use == Q2_SHOCK_SLOTS)
            use = free_slot;
    } else {
        /* 0x80040A9C..0x80040AC4: the first free slot. */
        for (i = 0; i < Q2_SHOCK_SLOTS; i++)
            if (!s->slot[i].data) {
                use = i;
                break;
            }
    }

    if (use >= Q2_SHOCK_SLOTS)
        return false;

    s->slot[use].data     = data;
    s->slot[use].time     = 0;
    s->slot[use].strength = (s16)str;
    return true;
}

/*
 * One envelope at `time`, and whether it is still running.
 *
 * 0x80040B84..0x80040C2C. The walk subtracts each span's duration until the
 * time falls inside one; inside, it interpolates toward the next point with a
 * truncating divide. Reaching the terminator stops the walk, and the
 * terminator's level is only reported when the time left over is EXACTLY
 * zero — otherwise the envelope reads nothing. Neither case keeps the slot
 * alive.
 */
static s32 envelope_level(const q2_shock_point *p, s32 time, bool *running)
{
    s32 t = time;

    for (;;) {
        if (t < p->duration)
            break;
        if (p->duration == 0)
            return t == 0 ? p->level : 0;
        t -= p->duration;
        p++;
    }

    *running = true;
    return p->level + (s32)((s32)(p[1].level - p->level) * t) / p->duration;
}

/* `x * 4096 >> 12` with `bgez` / `addiu 4095` in front: a divide that rounds
 * toward zero. */
static s32 div4096(s32 v)
{
    return (v < 0 ? v + 4095 : v) >> 12;
}

void q2_shock_process(q2_shock *s, s32 dt, bool alive, u8 out[2])
{
    s32 big = 0, little = 0, v;
    int i;
    u16 acc;
    u8 motor[2];

    if (!s || !out)
        return;

    for (i = 0; i < Q2_SHOCK_SLOTS; i++) {
        q2_shock_slot *sl = &s->slot[i];
        bool running = false;

        if (!sl->data)
            continue;

        /* A NULL envelope pointer skips that motor (0x80040B7C, 0x80040C6C);
         * every record on the disc has both. */
        if (sl->data->big)
            big += div4096(envelope_level(sl->data->big, sl->time, &running) *
                           sl->strength);
        if (sl->data->little)
            little += div4096(envelope_level(sl->data->little, sl->time,
                                             &running) * sl->strength);

        /* 0x80040D48..0x80040D74: alive slots advance by PalNtscRate, a spent
         * one is freed. */
        if (running)
            sl->time = (s16)(u16)((u16)sl->time + (u16)dt);
        else
            sl->data = NULL;
    }

    /* 0x80040D88..0x80040DB0: clamp the sum, then `* 255 >> 12`. */
    if (big > Q2_SHOCK_FULL)
        big = Q2_SHOCK_FULL;
    motor[0] = (u8)div4096((big << 8) - big);

    /* 0x80040DB4..0x80040E28: the little motor's sigma-delta. */
    if (little > Q2_SHOCK_FULL)
        little = Q2_SHOCK_FULL;
    v   = div4096((little << 8) - little);
    acc = (u16)(s->little_accum + v);
    s->little_accum = acc;
    if ((s16)acc >= 129) {
        s->little_accum = (u16)(acc - 256);
        motor[1] = 1;
    } else {
        motor[1] = 0;
    }

    /* 0x80040E4C..0x80040E90: no window player, or a dead one, and the pad
     * is told to stop — after the slots have advanced. */
    if (!alive) {
        out[0] = out[1] = 0;
        return;
    }
    out[0] = motor[0];
    out[1] = motor[1];
}

void q2_shock_kill(q2_shock *s)
{
    int i;

    if (!s)
        return;
    for (i = 0; i < Q2_SHOCK_SLOTS; i++)
        s->slot[i].data = NULL;
}

bool q2_shock_active(const q2_shock *s)
{
    int i;

    if (!s)
        return false;
    for (i = 0; i < Q2_SHOCK_SLOTS; i++)
        if (s->slot[i].data)
            return true;
    return false;
}

/* ------------------------------------------------------------------------- */
static q2_shock_sink_fn g_sink;
static void            *g_sink_user;

void q2_shock_attach(q2_shock_sink_fn fn, void *user)
{
    g_sink      = fn;
    g_sink_user = fn ? user : NULL;
}

void q2_shock_request(int player, int id, s32 strength, const s32 pos[3],
                      s16 near_r, s16 far_r)
{
    if (g_sink)
        g_sink(g_sink_user, player, id, strength, pos, near_r, far_r);
}

void q2_shock_explosion(const s32 at[3], s32 size)
{
    q2_shock_request(Q2_SHOCK_ALL_PLAYERS, Q2_SHOCK_EXPLOSION, size, at,
                     (s16)(size / 2), (s16)size);
}
