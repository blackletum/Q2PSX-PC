/*
 * cmd_death.c — check the player death chain against the executable.
 *
 * playerdeath.[ch] is a transcription, and a transcription is worth exactly as
 * much as the reader's confidence that it says what the machine code says. This
 * command reads the disc's own SLES_015.34 and compares, instruction by
 * instruction, every number the reconstruction carries: the gib threshold, the
 * corpse timer, the page's arm, the walk-back deadline, the fade rate, the
 * friction, the two guards on the frag hook, and the ten move names the player's
 * animation set is built from.
 *
 * The comparison is against the ENCODED WORD, not against a disassembly — an
 * `addiu v0, zero, 1500` is 0x240205DC and nothing else, so a mismatch means
 * either a different build of the game or a wrong constant in the port, and the
 * command says which address disagreed.
 */
#include "cmd_death.h"

#include <stdio.h>
#include <string.h>

#include "exe.h"
#include "playerdeath.h"

static int g_fail;

/* ------------------------------------------------------------------------- */

static void check_word(const q2_exe *e, u32 addr, u32 want, const char *what)
{
    u32 got = 0;

    if (!q2_exe_u32(e, addr, &got)) {
        printf("  %-46s %08X  NOT IN THE SEGMENT\n", what, addr);
        g_fail++;
        return;
    }
    if (got != want) {
        printf("  %-46s %08X  %08X, expected %08X  MISMATCH\n",
               what, addr, got, want);
        g_fail++;
        return;
    }
    printf("  %-46s %08X  %08X  ok\n", what, addr, got);
}

static void check_string(const q2_exe *e, u32 addr, const char *want,
                         const char *what)
{
    const u8 *p = (const u8 *)q2_exe_ptr(e, addr, (u32)strlen(want) + 1);

    if (!p) {
        printf("  %-46s %08X  NOT IN THE SEGMENT\n", what, addr);
        g_fail++;
        return;
    }
    if (memcmp(p, want, strlen(want) + 1) != 0) {
        printf("  %-46s %08X  \"%.16s\", expected \"%s\"  MISMATCH\n",
               what, addr, (const char *)p, want);
        g_fail++;
        return;
    }
    {
        /* Escaped, because one of these strings ends in a newline and an
         * unescaped one would break the column. */
        char   shown[80];
        size_t at = 0, k;

        for (k = 0; want[k] && at + 3 < sizeof(shown); k++) {
            if (want[k] == '\n') {
                shown[at++] = '\\';
                shown[at++] = 'n';
            } else {
                shown[at++] = want[k];
            }
        }
        shown[at] = '\0';
        printf("  %-46s %08X  \"%s\"  ok\n", what, addr, shown);
    }
}

/* ------------------------------------------------------------------------- */

int cmd_death(const disc *d)
{
    q2_exe exe;
    int    i;

    if (q2_exe_load(&exe, d, NULL) != Q2_OK) {
        fprintf(stderr, "cannot read the executable\n");
        return 1;
    }

    g_fail = 0;

    printf("The player death chain, checked against SLES_015.34\n\n");

    printf("the five functions, and who installs whom\n");
    printf("  0x8003A1C8  the player think, installed at spawn (0x8003B3EC)\n");
    printf("  0x800396AC  player_die     — installs the corpse think, so it\n");
    printf("                               runs once and only once\n");
    printf("  0x80039550  corpse_think   — the body, every tick\n");
    printf("  0x8003E238  respawn_think  — the corpse timer, deathmatch only\n");
    printf("  0x8005B358  body_fade      — the body shrinking out of the world\n");
    printf("  0x8003CE14  player_anim    — which move plays\n");
    printf("  0x8003DDF8  spawn_player   — the ONLY respawn, and the engine\n");
    printf("                               never calls it: QMULTI.C does\n\n");

    printf("the gate, 0x8003ADB8\n");
    /* `bgtz v0, 0x8003ADF8` — zero health is dead. */
    check_word(&exe, 0x8003ADC0u, 0x1C40000Du, "health > 0 skips the handler");
    /* `lui v1, 0x8` — the DEAD bit, entity+0x10C & 0x00080000. */
    check_word(&exe, 0x8003ADC4u, 0x3C030008u, "the DEAD bit is 0x00080000");
    check_word(&exe, 0x8003ADDCu, 0x0C00E5ABu, "and then jal 0x800396AC");
    /* `addiu a1, zero, 4` in the delay slot of the player_anim call. */
    check_word(&exe, 0x8003ADECu, 0x24050004u, "player_anim(self, 4) = DEATH");
    printf("\n");

    printf("player_die, 0x800396AC\n");
    check_word(&exe, 0x800396CCu, 0x2442FFF7u, "mod - 9 ...");
    check_word(&exe, 0x800396D0u, 0x2C420002u, "... < 2 erases the killer");
    check_word(&exe, 0x800396DCu, 0xA20200DEu, "... by writing -1 to entity+222");
    check_word(&exe, 0x80039724u, 0x2402FFFFu, "the death cry tests killer ...");
    check_word(&exe, 0x80039728u, 0x1622000Bu, "... != -1 and SKIPS the sound");
    check_word(&exe, 0x80039774u, 0x2A220004u, "the frag hook wants killer < 4");
    check_word(&exe, 0x8003977Cu, 0x2A420004u, "and victim < 4 (both SIGNED)");
    check_word(&exe, 0x800397FCu, 0x2402FFD8u, "gib_health = -40");
    check_word(&exe, 0x80039810u, 0x240205DCu, "the corpse timer = 1500");
    check_word(&exe, 0x80039818u, 0x3C028004u, "self->think = 0x80039550 ...");
    check_word(&exe, 0x80039824u, 0x24429550u, "... the corpse think");
    check_word(&exe, 0x8003984Cu, 0x244204B0u, "the walk-back deadline = +1200");
    printf("\n");

    printf("single player, 0x8002059C\n");
    check_word(&exe, 0x8001D73Cu, 0x24040029u, "the page it opens is 41");
    check_word(&exe, 0x800205B0u, 0x24020258u, "armed for 600");
    check_string(&exe, 0x800AC3E8u, "Continues %d\n",
                 "and it prints the resupply count");
    check_word(&exe, 0x8001FF0Cu, 0x2484FFFFu, "RESUPPLY spends one, 0x8001FF00");
    check_string(&exe, 0x800AB32Cu, "gRESUPPLY AND RESTART (0 LEFT)",
                 "the greyed row");
    check_string(&exe, 0x800AB34Cu, "RESUPPLY AND RESTART (%d LEFT)",
                 "and the live one");
    printf("\n");

    printf("corpse_think, 0x80039550\n");
    /* `sll a2, v0, 2` then `addu a2, a2, v0` — the friction step is dt * 5. */
    check_word(&exe, 0x800395A0u, 0x00023080u, "the friction step is dt * 4 ...");
    check_word(&exe, 0x800395A4u, 0x00C23021u, "... + dt, so dt * 5");
    check_word(&exe, 0x8003962Cu, 0x24050004u, "it keeps asking for DEATH");
    check_word(&exe, 0x8003964Cu, 0x2402008Fu, "the settled body's box is 143");
    check_word(&exe, 0x80039650u, 0xA602006Eu, "written to entity+0x6E ...");
    check_word(&exe, 0x80039670u, 0xAE02007Cu, "... and its bound at +0x7C");
    check_word(&exe, 0x80039658u, 0x2442E238u, "then think = 0x8003E238");
    printf("\n");

    printf("respawn_think 0x8003E238, and the body's end 0x8005B358\n");
    check_word(&exe, 0x8003E2BCu, 0x960200F4u, "the timer is entity+0xF4 ...");
    check_word(&exe, 0x8003E2CCu, 0x00431023u, "... and dt comes off it");
    check_word(&exe, 0x8003E2E8u, 0x2442B358u, "then think = 0x8005B358");
    check_word(&exe, 0x8005B368u, 0x948200FCu,
               "which takes the scale at +0xFC ...");
    check_word(&exe, 0x8005B36Cu, 0x00031900u, "... down by dt << 4");
    printf("\n");

    printf("the animation, 0x8003CE14 and 0x8003DFE4\n");
    check_word(&exe, 0x8003E000u, 0x3042FFFCu,
               "installing a move clears +0x102 & 3");
    check_word(&exe, 0x8003DF90u, 0x34420001u,
               "and running past the end sets bit 0");
    check_word(&exe, 0x80039618u, 0x86020102u,
               "which is what the corpse think waits for");
    printf("\n");

    printf("the player's ten moves, 12 bytes apart from 0x800AC554\n");
    for (i = 0; i < Q2_PMOVE_COUNT; i++) {
        char label[32];

        snprintf(label, sizeof(label), "  move %d", i);
        check_string(&exe, 0x800AC554u + (u32)i * 12u,
                     q2_player_move_name((q2_player_move)i), label);
    }
    printf("\n");

    printf("the spawn, 0x8003B250 and 0x8003DDF8\n");
    check_word(&exe, 0x8003B284u, 0x240600E0u, "the client stride is 224 ...");
    check_word(&exe, 0x8003B298u, 0x24637C60u, "... based at 0x800C7C60 ...");
    check_word(&exe, 0x800396E4u, 0x24637C60u,
               "... which is the base player_die subtracts ...");
    check_word(&exe, 0x80039720u, 0x00029143u,
               "... before the >> 5 that finishes /224");
    check_word(&exe, 0x8003B2B0u, 0x24020027u, "a player's entity kind is 39");
    check_word(&exe, 0x8003B2B8u, 0x24020064u, "and it starts at 100 health");
    check_word(&exe, 0x8003DE24u, 0x24020004u,
               "a fresh spawn's killer byte is 4");
    check_word(&exe, 0x8003DECCu, 0x0C00F77Eu,
               "and the mode gate is its only caller");
    printf("\n");

    q2_exe_free(&exe);

    if (g_fail) {
        printf("%d check%s FAILED\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("every check passed\n");
    return 0;
}
