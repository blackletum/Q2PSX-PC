/*
 * test_explosive.c — opcode 0x08, the `func_explosive`.
 *
 * These pin the branches a plausible-looking implementation gets wrong, which
 * is a different set from the ones that are merely fiddly:
 *
 *   - the hit burst runs BEFORE the survival test and is NOT suppressed by the
 *     group surviving (0x80026820 precedes the `bgtz` at 0x80026830);
 *   - `destroy` is SIGNED and negative means detonate, with the piece count the
 *     one's complement rather than the absolute value (`nor`, 0x80026944);
 *   - a negative slot in array A skips the hide as well as the effects, but the
 *     matching array B slot is processed anyway (0x80026890 jumps to 0x80026990,
 *     not past it);
 *   - the load-time suppression flag costs the effects and KEEPS the swap
 *     (0x8002692C);
 *   - a script call — damage 0 — destroys the group outright (0x80026808).
 *
 * The item bytes are built here rather than read off a disc, so this runs
 * without one. `q2psx-inspect explosives` is the half that needs the disc.
 */
#include <stdio.h>
#include <string.h>

#include "explosive.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n",
               what, (long long)got, (long long)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* A hand-built Events chunk holding one record with one opcode-0x08 item.    */
/* ------------------------------------------------------------------------- */
static u8 g_chunk[64];

static void put16(u8 *p, s16 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

/*
 * The container: `u32 count`, a directory terminator, then the records. See
 * events.h — a record is `u16 size; u8 n_items; u8 flags` and an item is
 * `u8 op; u8 len; payload`.
 */
static bool build_chunk(q2_events *out,
                        const s16 node[4], const s16 rubble[4], s16 reveal,
                        s16 health, u8 hit_pieces, s8 destroy)
{
    u8 *rec = g_chunk + 8;
    u8 *it  = rec + 4;
    int k;

    memset(g_chunk, 0, sizeof(g_chunk));
    put32(g_chunk + 0, 1);      /* one record            */
    put32(g_chunk + 4, 0);      /* directory terminator  */

    put16(rec + 0, 4 + Q2_EXPLOSIVE_ITEM_LEN);   /* record size */
    rec[2] = 1;                                  /* one item    */
    rec[3] = 0x10;                               /* the loader's default flags */

    it[0] = 0x08;
    it[1] = Q2_EXPLOSIVE_ITEM_LEN;
    put32(it + 2, 0x00800000u);                  /* the unread word */
    for (k = 0; k < 4; k++) {
        put16(it + 6 + 2 * k, node[k]);
        put16(it + 14 + 2 * k, rubble[k]);
    }
    put16(it + 22, health);
    it[24] = hit_pieces;
    it[25] = (u8)destroy;
    put16(it + 26, reveal);

    {
        q2_common_file dummy;
        (void)dummy;
    }

    out->data         = g_chunk;
    out->size         = 8 + 4 + Q2_EXPLOSIVE_ITEM_LEN;
    out->record_count = 1;
    out->dir_count    = 0;
    out->dir_offset   = 4;
    out->first_record = 8;
    return true;
}

static bool build_one(q2_explosive_set *set,
                      const s16 node[4], const s16 rubble[4], s16 reveal,
                      s16 health, u8 hit_pieces, s8 destroy)
{
    q2_events ev;

    build_chunk(&ev, node, rubble, reveal, health, hit_pieces, destroy);
    if (q2_explosives_build(set, &ev, NULL, NULL) != Q2_OK)
        return false;
    return set->count == 1;
}

static bool res_hides(const q2_explosive_result *r, s16 node)
{
    u32 i;
    for (i = 0; i < r->hide_count; i++)
        if (r->hide[i] == node)
            return true;
    return false;
}

static bool res_shows(const q2_explosive_result *r, s16 node)
{
    u32 i;
    for (i = 0; i < r->show_count; i++)
        if (r->show[i] == node)
            return true;
    return false;
}

/* ------------------------------------------------------------------------- */
static void test_decode(void)
{
    static const s16 node[4]   = { 10, 11, -1, -1 };
    static const s16 rubble[4] = { 20, -1, -1, 21 };
    q2_explosive_set set;

    puts("the 28-byte item decodes (0x80026A20)");

    check(build_one(&set, node, rubble, 30, 100, 7, -13),
          "one item builds one entry");
    if (set.count == 1) {
        const q2_explosive *e = &set.items[0];

        check_eq(e->node[0], 10, "intact slot 0");
        check_eq(e->node[1], 11, "intact slot 1");
        check_eq(e->node[2], -1, "an empty intact slot stays empty");
        check_eq(e->part_count, 2, "two intact parts, not four");
        check_eq(e->rubble[0], 20, "rubble slot 0");
        check_eq(e->rubble[3], 21, "rubble slot 3 - a hole is not a terminator");
        check_eq(e->reveal, 30, "the +26 reveal node");
        check_eq(e->health, 100, "health");
        check_eq(e->hit_pieces, 7, "the per-hit debris count");
        check_eq(e->destroy, -13, "the destroy byte keeps its sign");
        check(e->damageable, "non-zero health installs a damage callback");
    }
    q2_explosives_free(&set);
}

static void test_initial_visibility(void)
{
    static const s16 node[4]   = { 10, -1, -1, -1 };
    static const s16 rubble[4] = { 20, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result vis;

    puts("the constructor shows the intact geometry and hides the wreckage");

    check(build_one(&set, node, rubble, 30, 100, 0, 5), "built");
    q2_explosive_initial_vis(&set, 0, &vis);

    check(res_shows(&vis, 10), "the intact node is shown (0x80026B84)");
    check(res_hides(&vis, 20), "the rubble node is hidden (0x80026C60)");
    check(res_hides(&vis, 30), "the reveal node is hidden too (0x80026ACC)");
    check(!vis.destroyed, "and nothing is destroyed by building it");

    q2_explosives_free(&set);
}

static void test_hit_burst_survives(void)
{
    static const s16 node[4]   = { 10, -1, -1, -1 };
    static const s16 none[4]   = { -1, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result res;

    puts("a non-fatal hit still throws debris (0x80026820 before 0x80026830)");

    check(build_one(&set, node, none, -1, 100, 9, 5), "built");

    check(!q2_explosive_damage(&set, 0, 0, 40, false, NULL, &res),
          "40 of 100 does not destroy it");
    check_eq(set.items[0].health, 60, "the hit points come off the item");
    check_eq(res.hit_pieces, 9, "and the burst ran anyway");
    check_eq(res.hit_node, 10, "out of the part that was hit");
    check_eq(res.burst_count, 0, "no destruction burst yet");
    check(!res.destroyed, "not destroyed");

    /* A second hit finishes it, and the counter is cumulative. */
    check(q2_explosive_damage(&set, 0, 0, 60, false, NULL, &res),
          "a second hit of 60 destroys it");
    check_eq(res.hit_pieces, 9, "the fatal hit throws its burst too");
    check_eq(res.burst_count, 1, "and one destruction burst");
    check(res.destroyed, "destroyed");

    check(!q2_explosive_damage(&set, 0, 0, 100, false, NULL, &res),
          "a third hit finds nothing left");

    q2_explosives_free(&set);
}

static void test_destroy_sign(void)
{
    static const s16 node[4] = { 10, -1, -1, -1 };
    static const s16 none[4] = { -1, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result res;

    puts("the destroy byte's SIGN selects the explosion (0x80026938)");

    /* Positive: debris only, count taken as-is. */
    check(build_one(&set, node, none, -1, 10, 0, 12), "built, destroy +12");
    check(q2_explosive_damage(&set, 0, 0, 10, false, NULL, &res), "destroyed");
    check_eq(res.burst_count, 1, "one burst");
    check(!res.burst[0].explode, "a positive count does not detonate");
    check_eq(res.burst[0].pieces, 12, "and throws exactly that many");
    q2_explosives_free(&set);

    /* Negative: detonate, count is the ONE'S COMPLEMENT. */
    check(build_one(&set, node, none, -1, 10, 0, -13), "built, destroy -13");
    check(q2_explosive_damage(&set, 0, 0, 10, false, NULL, &res), "destroyed");
    check(res.burst[0].explode, "a negative count detonates");
    check_eq(res.burst[0].pieces, 12,
             "and throws ~(-13) == 12 pieces, not 13");
    q2_explosives_free(&set);

    /* -1 is the edge: detonate and throw nothing at all. */
    check(build_one(&set, node, none, -1, 10, 0, -1), "built, destroy -1");
    check(q2_explosive_damage(&set, 0, 0, 10, false, NULL, &res), "destroyed");
    check(res.burst[0].explode, "-1 detonates");
    check_eq(res.burst[0].pieces, 0, "and throws no debris");
    q2_explosives_free(&set);
}

static void test_geometry_swap(void)
{
    static const s16 node[4]   = { 10, 11, -1, -1 };
    static const s16 rubble[4] = { 20, 21, -1, 23 };
    q2_explosive_set set;
    q2_explosive_result res;

    puts("destroying it swaps the two node groups");

    check(build_one(&set, node, rubble, 30, 10, 0, 4), "built");
    check(q2_explosive_damage(&set, 0, 0, 10, false, NULL, &res), "destroyed");

    check(res_hides(&res, 10), "intact node 0 hidden (0x80068818)");
    check(res_hides(&res, 11), "intact node 1 hidden");
    check(res_shows(&res, 20), "rubble node 0 shown (0x800269D0)");
    check(res_shows(&res, 21), "rubble node 1 shown");
    check(res_shows(&res, 30), "the reveal node shown (0x80026868)");
    check_eq(res.burst_count, 2, "one burst per intact part");

    /*
     * SLOT 3 HAS NO INTACT NODE AND ITS WRECKAGE STILL APPEARS. The `bltz` at
     * 0x80026890 jumps to 0x80026990, which is array B's arm — not past it.
     */
    check(res_shows(&res, 23),
          "rubble slot 3 is shown even though intact slot 3 is empty");

    q2_explosives_free(&set);
}

static void test_suppression(void)
{
    static const s16 node[4]   = { 10, -1, -1, -1 };
    static const s16 rubble[4] = { 20, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result res;

    puts("gp+0x4234 costs the effects and KEEPS the swap (0x8002692C)");

    check(build_one(&set, node, rubble, 30, 10, 0, -20), "built");
    check(q2_explosive_damage(&set, 0, 0, 10, true, NULL, &res),
          "still destroyed under suppression");

    check_eq(res.burst_count, 0, "no debris and no explosion");
    check(res_hides(&res, 10), "but the intact node is still hidden");
    check(res_shows(&res, 20), "and the wreckage still appears");
    check(res_shows(&res, 30), "and so does the reveal node");

    q2_explosives_free(&set);
}

static void test_script_call(void)
{
    static const s16 node[4] = { 10, -1, -1, -1 };
    static const s16 none[4] = { -1, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result res;
    int idx;

    puts("a script call carries no damage and destroys it outright"
         " (0x80026808)");

    /* Authored health zero: not shootable at all, but a script still gets it. */
    check(build_one(&set, node, none, -1, 0, 0, -5), "built with health 0");
    check(!set.items[0].damageable,
          "health 0 means no damage callback (0x80026B10)");

    idx = q2_explosive_find(&set, set.items[0].item_offset);
    check_eq(idx, 0, "the item offset is the identity");

    check(q2_explosive_trigger_item(&set, set.items[0].item_offset, false,
                                    NULL, &res),
          "a script destroys it");
    check(res.destroyed, "destroyed");
    check_eq(res.hit_node, -1, "no part took a hit");
    check_eq(res.hit_pieces, 0, "so no per-hit burst");
    check_eq(res.burst_count, 1, "but the destruction burst ran");
    check(res.burst[0].explode, "and it detonated");

    check(!q2_explosive_trigger_item(&set, set.items[0].item_offset, false,
                                     NULL, &res),
          "running it twice does nothing the second time");

    q2_explosives_free(&set);
}

static void test_survives_healthy_group_with_damage_zero(void)
{
    static const s16 node[4] = { 10, -1, -1, -1 };
    static const s16 none[4] = { -1, -1, -1, -1 };
    q2_explosive_set set;
    q2_explosive_result res;

    puts("damage 0 skips the counter even on a group with hit points left");

    check(build_one(&set, node, none, -1, 500, 3, 8), "built with 500 hp");
    check(q2_explosive_damage(&set, 0, -1, 0, false, NULL, &res),
          "damage 0 destroys it regardless of the 500 hit points");
    check_eq(set.items[0].health, 500,
             "and the hit points are untouched - the branch skipped them");

    q2_explosives_free(&set);
}

/* ------------------------------------------------------------------------- */
int main(void)
{
    puts("opcode 0x08 - the destroyable brush groups\n");

    test_decode();
    test_initial_visibility();
    test_hit_burst_survives();
    test_destroy_sign();
    test_geometry_swap();
    test_suppression();
    test_script_call();
    test_survives_healthy_group_with_damage_zero();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
