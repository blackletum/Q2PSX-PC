/*
 * test_shock.c — the DualShock engine: SetShockData (0x80040800),
 * ProcessShockData (0x80040B28) and the sixteen patterns at 0x8009D420.
 *
 * Every expected byte here is worked by hand from the instructions shock.c
 * cites, at the PAL field step of 6 ticks unless a test says otherwise.
 */
#include <stdio.h>
#include <string.h>

#include "shock.h"

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what)
{
    g_checks++;
    if (!condition) {
        printf("  FAIL  %s\n", what);
        g_failures++;
    }
}

static void check_eq_s32(s32 got, s32 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %d, want %d\n", what, (int)got, (int)want);
        g_failures++;
    }
}

static void test_table(void)
{
    u32 i, n = 0;

    puts("ShockDataAvailable");

    for (i = 0; q2_shock_available[i]; i++)
        n++;
    check_eq_s32((s32)n, Q2_SHOCK_PATTERNS, "sixteen records, NULL-ended");

    /* The list is in record order, not id order. */
    check_eq_s32(q2_shock_available[6]->id, Q2_SHOCK_ROCKETL,
                 "the seventh record is the rocket launcher's, id 10");
    check(q2_shock_find(Q2_SHOCK_ROCKETL) == q2_shock_available[6],
          "find walks to it");
    check(q2_shock_find(Q2_SHOCK_MACHINEGUN)->addr == 0x8009D450u,
          "id 5 is ShockData_MachineGun");
    check(strcmp(q2_shock_find(Q2_SHOCK_MACHINEGUN)->name,
                 "ShockData_MachineGun") == 0, "and carries its name");
    check(q2_shock_find(0) == NULL && q2_shock_find(17) == NULL,
          "ids outside 1..16 find nothing");

    for (i = 0; i < Q2_SHOCK_PATTERNS; i++) {
        const q2_shock_data *d = q2_shock_available[i];
        bool stacks = d->id == Q2_SHOCK_STOMP ||
                      d->id == Q2_SHOCK_IMPACT_DISTINCT;
        check(((d->flags & Q2_SHOCK_FLAG_MERGE) == 0) == stacks,
              "only Stomp and Impact_Distinct stack");
        check(d->big && d->little, "every record has both envelopes");
    }
}

static void test_machinegun(void)
{
    q2_shock s;
    u8 out[2];

    puts("the machinegun's kick");

    memset(&s, 0, sizeof(s));
    check(q2_shock_set(&s, Q2_SHOCK_MACHINEGUN, Q2_SHOCK_FULL, NULL, NULL,
                       0, 0), "QPlayerFireMachineGun's request is taken");

    /* big {1536,16}{1536,0}: 1536 * 255 >> 12 = 95 for t = 0, 6, 12.
     * little {4096,8}{4096,0}: on for t = 0 and 6, nothing at 12. */
    q2_shock_process(&s, 6, true, out);
    check_eq_s32(out[0], 95, "t=0 big");
    check_eq_s32(out[1], 1,  "t=0 little on");
    q2_shock_process(&s, 6, true, out);
    check_eq_s32(out[0], 95, "t=6 big");
    check_eq_s32(out[1], 1,  "t=6 little on");
    q2_shock_process(&s, 6, true, out);
    check_eq_s32(out[0], 95, "t=12 big");
    check_eq_s32(out[1], 0,  "t=12 little spent");
    check(q2_shock_active(&s), "the big envelope still holds the slot");
    q2_shock_process(&s, 6, true, out);
    check_eq_s32(out[0], 0, "t=18 big spent");
    check_eq_s32(out[1], 0, "t=18 little spent");
    check(!q2_shock_active(&s), "and the slot is freed");

    /* 255 lands it on 255 (on, -> -1), 255 again on 254 (on, -> -2), and the
     * two spent samples add nothing. */
    check_eq_s32((s16)s.little_accum, -2, "sigma-delta carries its residue");
}

static void test_merge_and_stack(void)
{
    q2_shock s;
    u8 out[2];
    int used, i;

    puts("restart versus stack");

    memset(&s, 0, sizeof(s));
    q2_shock_set(&s, Q2_SHOCK_MACHINEGUN, Q2_SHOCK_FULL, NULL, NULL, 0, 0);
    q2_shock_process(&s, 6, true, out);
    q2_shock_process(&s, 6, true, out);
    q2_shock_set(&s, Q2_SHOCK_MACHINEGUN, Q2_SHOCK_FULL, NULL, NULL, 0, 0);
    for (used = 0, i = 0; i < Q2_SHOCK_SLOTS; i++)
        used += s.slot[i].data != NULL;
    check_eq_s32(used, 1, "a second machinegun shot restarts the first");
    for (i = 0; i < Q2_SHOCK_SLOTS; i++)
        if (s.slot[i].data)
            check_eq_s32(s.slot[i].time, 0, "from the top");

    /* Merge with all four slots free takes the LAST free one the scan saw. */
    memset(&s, 0, sizeof(s));
    q2_shock_set(&s, Q2_SHOCK_BLASTER, Q2_SHOCK_FULL, NULL, NULL, 0, 0);
    check(s.slot[3].data != NULL, "a mergeable request lands in slot 3");

    memset(&s, 0, sizeof(s));
    for (i = 0; i < 5; i++)
        check(q2_shock_set(&s, Q2_SHOCK_STOMP, Q2_SHOCK_FULL, NULL, NULL,
                           0, 0) == (i < 4), "stomps stack, four deep");
    check(s.slot[0].data && s.slot[3].data, "first free slot each time");

    /* Four stomps at full strength sum past 4096 and clamp. */
    q2_shock_process(&s, 6, true, out);
    check_eq_s32(out[0], 255, "the clamp: 4096 * 255 >> 12");
}

static void test_dead_and_hold(void)
{
    q2_shock s;
    u8 out[2];
    int i;

    puts("a dead player, and the terminator's single sample");

    memset(&s, 0, sizeof(s));
    q2_shock_set(&s, Q2_SHOCK_MACHINEGUN, Q2_SHOCK_FULL, NULL, NULL, 0, 0);
    q2_shock_process(&s, 6, false, out);
    check(out[0] == 0 && out[1] == 0, "a dead player's pad is still");
    check_eq_s32(s.slot[3].time, 6, "but the slot advanced");

    /* GrenadeOpen big {1024,128}{2048,0}: at exactly t=128 the terminator's
     * 2048 is reported once. At an 8-tick step that is the seventeenth
     * sample. The little envelope (0 for 144) keeps the slot alive. */
    memset(&s, 0, sizeof(s));
    q2_shock_set(&s, Q2_SHOCK_GRENADE_OPEN, Q2_SHOCK_FULL, NULL, NULL, 0, 0);
    for (i = 0; i < 16; i++)
        q2_shock_process(&s, 8, true, out);
    check_eq_s32(out[0], (s32)(((1024 + 1024 * 120 / 128) * 255) >> 12),
                 "t=120 interpolates toward 2048");
    q2_shock_process(&s, 8, true, out);
    check_eq_s32(out[0], (2048 * 255) >> 12, "t=128 holds the terminator");
    q2_shock_process(&s, 8, true, out);
    check_eq_s32(out[0], 0, "t=136 reads nothing");
}

static void test_position(void)
{
    q2_shock s;
    const s32 at[3] = { 0, 0, 0 };
    s32 ear[3] = { 2560, 0, 0 };

    puts("SetShockData's falloff — the train's arguments");

    /* 0x8002C778: id 13 at 1024, near 2048, far 4096. */
    memset(&s, 0, sizeof(s));
    check(q2_shock_set(&s, Q2_SHOCK_EXPLOSION, 1024, at, ear, 2048, 4096),
          "2560 away is inside far");
    check_eq_s32(s.slot[3].strength, 1024 * (4096 - 2560) / 2048,
                 "linear between the radii");

    memset(&s, 0, sizeof(s));
    ear[0] = 3072;
    check(!q2_shock_set(&s, Q2_SHOCK_EXPLOSION, 1024, at, ear, 2048, 4096),
          "3072 away falls to 512, under the 513 floor");

    memset(&s, 0, sizeof(s));
    ear[0] = 1000;
    q2_shock_set(&s, Q2_SHOCK_EXPLOSION, 1024, at, ear, 2048, 4096);
    check_eq_s32(s.slot[3].strength, 1024, "inside near is full strength");

    memset(&s, 0, sizeof(s));
    ear[0] = 0; ear[2] = 4097;
    check(!q2_shock_set(&s, Q2_SHOCK_EXPLOSION, 4096, at, ear, 2048, 4096),
          "one axis past far rejects");

    memset(&s, 0, sizeof(s));
    check(!q2_shock_set(&s, Q2_SHOCK_EXPLOSION, 512, NULL, NULL, 0, 0),
          "the floor applies without a position too");
}

static void test_sigma_delta(void)
{
    q2_shock s;
    u8 out[2];
    int on = 0, i;

    puts("the little motor at half strength");

    /* Hyperblaster little is 4096 for 16 ticks: at strength 2048 the byte is
     * 127, so the accumulator runs 127 (off), 254 (on, -> -2), 125 (off)... */
    memset(&s, 0, sizeof(s));
    q2_shock_set(&s, Q2_SHOCK_HYPERBLASTER, 2048, NULL, NULL, 0, 0);
    for (i = 0; i < 3; i++) {
        q2_shock_process(&s, 6, true, out);
        on += out[1];
    }
    check_eq_s32(on, 1, "off, on, off: the duty settles toward half");
    check_eq_s32((s16)s.little_accum, 125, "127, then 254 -> -2, then 125");
}

int main(void)
{
    test_table();
    test_machinegun();
    test_merge_and_stack();
    test_dead_and_hold();
    test_position();
    test_sigma_delta();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
