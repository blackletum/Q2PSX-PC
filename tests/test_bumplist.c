/*
 * The frame's actor list (sim.h, `bump_next`) against the console's own code.
 *
 * fixtures/bump_list_reference.h was made by executing the PAL and US
 * InitEveryLevel list setup (0x80055288), InitialiseBumpListFrame (0x8005525C)
 * and AddCreToNextBumpList (0x800552B4) over scripted frames, and
 * LineIntersectCreCylinder (0x800544EC) over two-body lists in both orders
 * (generate_bump_list_reference.py). Here the sim runs the same frames: its
 * tick swaps the list and appends the player, and the test appends the
 * creatures the script appended. Then a creature round is fired through the
 * sim with the two bodies appended each way round.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "combat.h"
#include "objslot.h"
#include "sim.h"
#include "fixtures/bump_list_reference.h"

static int g_failures;
static int g_checks;

#define CHECK(x) do { g_checks++; if (!(x)) { g_failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define CREATURES 40

static q2_move_body g_body[CREATURES];
static q2_actor     g_actor[CREATURES];
static q2_actor    *g_target[CREATURES];

static void setup(q2_sim *sim)
{
    static const s32 feet[3] = { 0, 0, 0 };
    u32 i;

    q2_obj_slots_reset();
    q2_sim_init(sim, NULL, 50);
    q2_sim_spawn(sim, feet, 0);
    memset(g_body, 0, sizeof(g_body));
    for (i = 0; i < CREATURES; i++) {
        g_body[i].id = (s32)i;
        g_body[i].pos[0] = 50000 + 1000 * (s32)i;
        g_body[i].radius = Q2_ACTOR_BODY_RADIUS;
        g_body[i].sweep_radius = 286;
        g_body[i].height = 600;
        g_body[i].solid = true;
        q2_actor_init(&g_actor[i]);
        g_actor[i].origin[0] = g_body[i].pos[0];
        g_actor[i].radius = 286;
        g_actor[i].height = 600;
        g_target[i] = &g_actor[i];
    }
    q2_sim_set_bodies(sim, g_body, CREATURES);
    q2_sim_set_targets(sim, g_target, CREATURES);
    q2_sim_set_body_actors(sim, g_target, CREATURES);
}

/* The swap, the lag, the order, the duplicates and the 32 cap. */
static void test_frames(void)
{
    q2_sim *sim = (q2_sim *)calloc(1, sizeof(*sim));
    q2_input input;
    u32 f;
    int i;

    if (!sim) exit(2);
    memset(&input, 0, sizeof(input));
    setup(sim);
    for (f = 0; f < sizeof(native_bump_frames) / sizeof(native_bump_frames[0]); f++) {
        const native_bump_frame *n = &native_bump_frames[f];
        bool same;

        /* The tick swaps (0x8005525C at the top of the frame) and the player,
         * slot 0, appends through its move: the script's first append. */
        q2_sim_tick(sim, &input, 12);
        same = (int)sim->body_count == n->read_count;
        for (i = 0; same && i < n->read_count; i++)
            same = sim->bodies[i].id ==
                   (n->read[i] == 0 ? -2 : n->read[i] - 1);
        if (!same)
            fprintf(stderr, "frame %u: %u entries, native %d\n", f,
                    sim->body_count, n->read_count);
        CHECK(same);
        CHECK(n->append_count >= 1 && n->appends[0] == 0);
        for (i = 1; i < n->append_count; i++)
            q2_sim_bump_append(sim, n->appends[i] == 0 ? -2 : n->appends[i] - 1,
                               -1);
        CHECK(sim->bump_next_count ==
              (u32)(n->append_count < 32 ? n->append_count : 32));
    }
    q2_sim_free(sim);
    free(sim);
}

/* Which body a round stops in, and where, depends on the list's order. */
static void test_orders(void)
{
    q2_sim *sim = (q2_sim *)calloc(1, sizeof(*sim));
    q2_input input;
    u32 c, changed = 0, picked = 0, matched = 0;
    int order, j, k;

    if (!sim) exit(2);
    memset(&input, 0, sizeof(input));
    setup(sim);
    for (c = 0; c < sizeof(native_bump_orders) / sizeof(native_bump_orders[0]); c++) {
        const native_bump_order *n = &native_bump_orders[c];
        s32 hit[2], at[2][3];

        for (order = 0; order < 2; order++) {
            s32 start[3], dir[3];

            for (j = 0; j < 2; j++) {
                for (k = 0; k < 3; k++) {
                    g_body[j].pos[k] = n->centre[j][k];
                    g_actor[j].origin[k] = n->centre[j][k];
                }
                g_actor[j].radius = n->radius[j];
                g_actor[j].height = (s16)n->height[j];
                g_actor[j].health = 100;
            }
            /* Appended this frame, read by the next one's queries. */
            q2_sim_bump_append(sim, order ? 1 : 0, -1);
            q2_sim_bump_append(sim, order ? 0 : 1, -1);
            q2_sim_tick(sim, &input, 12);
            for (k = 0; k < 3; k++) {
                start[k] = n->start[k];
                dir[k] = n->delta[k];
            }
            hit[order] = q2_sim_creature_hitscan(sim, NULL, start, dir, 0,
                                                 Q2_MOD_BULLET, false,
                                                 at[order]);
        }
        {
            bool ab = hit[0] == n->hit_ab &&
                      (n->hit_ab < 0 || !memcmp(at[0], n->point_ab, sizeof(at[0])));
            bool ba = hit[1] == n->hit_ba &&
                      (n->hit_ba < 0 || !memcmp(at[1], n->point_ba, sizeof(at[1])));

            if (!ab || !ba)
                fprintf(stderr, "case %u: %d (%d,%d,%d) / %d (%d,%d,%d), native "
                        "%d (%d,%d,%d) / %d (%d,%d,%d)\n", c,
                        hit[0], at[0][0], at[0][1], at[0][2],
                        hit[1], at[1][0], at[1][1], at[1][2],
                        n->hit_ab, n->point_ab[0], n->point_ab[1], n->point_ab[2],
                        n->hit_ba, n->point_ba[0], n->point_ba[1], n->point_ba[2]);
            matched += ab && ba;
        }
        changed += hit[0] != hit[1] || memcmp(at[0], at[1], sizeof(at[0])) != 0;
        picked += hit[0] != hit[1];
    }
    printf("  %u of %u two-body crossings match both orders; %u land "
           "elsewhere when the order flips, %u pick another body\n",
           matched, c, changed, picked);
    CHECK(matched == c);
    CHECK(changed == NATIVE_BUMP_ORDER_CHANGED);
    CHECK(picked == NATIVE_BUMP_ORDER_PICKED);
    q2_sim_free(sim);
    free(sim);
}

/*
 * What only an append puts there: a body that did not append is not in the
 * next frame's list, and a smashed corpse's entry outlives its record until
 * the slot is handed out again (0x8007F740, then the free at 0x8007D140).
 */
static void test_membership(void)
{
    q2_sim *sim = (q2_sim *)calloc(1, sizeof(*sim));
    q2_actor *list[Q2_BUMP_LIST_MAX];
    q2_input input;
    s32 slot, n;

    if (!sim) exit(2);
    memset(&input, 0, sizeof(input));
    setup(sim);
    slot = q2_obj_slot_alloc(Q2_OBJ_CREATURE, 3);
    CHECK(slot >= 0);

    q2_sim_tick(sim, &input, 12);
    q2_sim_bump_append(sim, 3, slot);
    q2_sim_tick(sim, &input, 12);
    /* The player (its first tick's move) and creature 3; creature 4, which
     * appended nothing, is in nobody's list. */
    CHECK(sim->body_count == 2 && sim->bodies[0].id == -2 &&
          sim->bodies[1].id == 3);
    n = q2_sim_bump_actors(sim, NULL, list, Q2_BUMP_LIST_MAX);
    CHECK(n == 2 && list[0] == &sim->combat.self && list[1] == &g_actor[3]);

    /* Appended, then smashed and freed on the same tick. */
    q2_sim_bump_append(sim, 3, slot);
    q2_obj_slot_free(slot);
    g_actor[3].absent = true;
    g_body[3].solid = false;
    q2_sim_tick(sim, &input, 12);
    CHECK(sim->body_count == 2 && sim->bodies[1].id == 3 &&
          !sim->bodies[1].solid);
    n = q2_sim_bump_actors(sim, NULL, list, Q2_BUMP_LIST_MAX);
    CHECK(n == 2 && list[1] != &g_actor[3] && !list[1]->absent &&
          list[1]->origin[0] == g_actor[3].origin[0]);

    /* Its record handed out again: the entry no longer names a body. */
    CHECK(q2_obj_slot_alloc(Q2_OBJ_ENTITY, 0) == slot);
    n = q2_sim_bump_actors(sim, NULL, list, Q2_BUMP_LIST_MAX);
    CHECK(n == 1 && list[0] == &sim->combat.self);
    q2_sim_body_refresh(sim, &g_body[3]);
    CHECK(!sim->bodies[1].listed);

    /* Next frame it is gone; the free left nothing to append. */
    q2_sim_tick(sim, &input, 12);
    CHECK(sim->body_count == 1 && sim->bodies[0].id == -2);
    q2_sim_free(sim);
    free(sim);
}

int main(void)
{
    printf("actor list\n\n");
    test_frames();
    test_orders();
    test_membership();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
