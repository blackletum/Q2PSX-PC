/*
 * test_movie.c — the BS v2 bitstream, checked without a disc.
 *
 * The three films are the user's, so a test that needs them is a test that
 * does not run. What CAN be checked here is the thing that was wrong for four
 * passes and that no picture would have caught: the Huffman table's shape.
 *
 * Three things are checked, and it is worth being precise about which of them
 * would have caught which historical error, because one of them would not:
 *
 *   1. PREFIX-FREEDOM. No code may be a prefix of another. An earlier table put
 *      an 8-bit group under the 5-bit `00101` and a 10-bit group under the
 *      ESCAPE; both decode most blocks and desynchronise on the rest, which
 *      reads like bad data rather than a bad table. This check found both in
 *      seconds and is the one with the best return on its size.
 *
 *   2. KRAFT. SUM 2^-len over every code plus EOB and the escape must not
 *      exceed 1, and for B.14 it is exactly 1 - 2^-12: the branch under twelve
 *      leading zeros is unused, which is why a decoder that loses alignment
 *      reports its unmatched lookaheads with exactly twelve leading zeros.
 *
 *      Kraft would NOT have caught the error that actually shipped. The group
 *      of sixteen 13-bit codes had been written as eight 12-bit ones, and
 *      8 x 2^-12 equals 16 x 2^-13 — the same code space, so the sum is
 *      identical. Saying so is the point: a check that passes on the known bug
 *      is worth keeping for the bugs it does catch and worth not overselling.
 *
 *   3. GROUP SHAPE. One code of 2 bits, one of 3, two of 4 ... sixteen each at
 *      12, 13, 14, 15 and 16. THIS is what catches the shipped error, and it is
 *      the least clever of the three.
 *
 * None of them needs the disc, a reference table, or a frame.
 */
#include "stx.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* The table exposes lengths and run/level, not the codes themselves, so the
 * codes are rebuilt the way the decoder matches them: a 17-bit lookahead whose
 * top `len` bits are the code. `q2_stx_code_at` gives the rest. */
int main(void)
{
    u32 n = q2_stx_code_count();
    u32 i;
    double kraft = 0.0;
    u32 by_len[20];
    u32 max_run = 0, max_level = 0;

    memset(by_len, 0, sizeof(by_len));

    printf("AC table: %u codes\n", n);
    CHECK(n == 111, "MPEG-1 Table B.14 has 111 entries, this has %u", n);

    for (i = 0; i < n; i++) {
        u32 len = 0, run = 0, level = 0;

        CHECK(q2_stx_code_at(i, &len, &run, &level), "entry %u unreadable", i);
        CHECK(len >= 2 && len <= 16, "entry %u has length %u", i, len);
        CHECK(level >= 1, "entry %u has level 0, which is the EOB's job", i);
        if (len < 20)
            by_len[len]++;
        if (run > max_run)     max_run = run;
        if (level > max_level) max_level = level;
        kraft += 1.0 / (double)(1u << len);
    }

    /*
     * EOB is two bits and the escape is six, and both are matched ahead of the
     * table rather than being in it — so they are added by hand.
     */
    kraft += 1.0 / 4.0;    /* EOB    `10`     */
    kraft += 1.0 / 64.0;   /* ESCAPE `000001` */

    printf("  lengths:");
    for (i = 0; i < 20; i++)
        if (by_len[i])
            printf("  %u-bit x%u", i, by_len[i]);
    printf("\n  Kraft sum (with EOB and escape): %.6f\n", kraft);
    printf("  largest run %u, largest level %u\n", max_run, max_level);

    /*
     * 1 - 2^-12 exactly. Over 1 is impossible — it means two codes overlap —
     * and the deficit is not slack in the table: it is the branch under twelve
     * leading zeros, which B.14 leaves unused. A decoder that has lost
     * alignment lands there, which is why "unmatched, twelve leading zeros" was
     * the signature of the end-of-frame lookahead bug and not of a missing row.
     */
    {
        double want = 1.0 - 1.0 / 4096.0;

        CHECK(kraft <= 1.0 + 1e-9,
              "Kraft sum is %.7f > 1: two codes overlap", kraft);
        CHECK(kraft > want - 1e-9 && kraft < want + 1e-9,
              "Kraft sum is %.7f, B.14's is %.7f", kraft, want);
    }

    /* B.14's shape, group by group. */
    CHECK(by_len[2]  == 1,  "2-bit codes: %u",  by_len[2]);
    CHECK(by_len[3]  == 1,  "3-bit codes: %u",  by_len[3]);
    CHECK(by_len[4]  == 2,  "4-bit codes: %u",  by_len[4]);
    CHECK(by_len[5]  == 3,  "5-bit codes: %u",  by_len[5]);
    CHECK(by_len[6]  == 4,  "6-bit codes: %u",  by_len[6]);
    CHECK(by_len[7]  == 4,  "7-bit codes: %u",  by_len[7]);
    CHECK(by_len[8]  == 8,  "8-bit codes: %u",  by_len[8]);
    CHECK(by_len[9]  == 0,  "there is no 9-bit group: %u", by_len[9]);
    CHECK(by_len[10] == 8,  "10-bit codes: %u", by_len[10]);
    CHECK(by_len[11] == 0,  "there is no 11-bit group: %u", by_len[11]);
    CHECK(by_len[12] == 16, "12-bit codes: %u", by_len[12]);
    CHECK(by_len[13] == 16, "13-bit codes: %u — this is the group that was "
                            "written as eight twelves", by_len[13]);
    CHECK(by_len[14] == 16, "14-bit codes: %u", by_len[14]);
    CHECK(by_len[15] == 16, "15-bit codes: %u", by_len[15]);
    CHECK(by_len[16] == 16, "16-bit codes: %u", by_len[16]);

    /* A run of 31 skips to coefficient 32 and a level of 40 is the largest the
     * table carries; anything past those is a transcription slip that Kraft
     * cannot see, because it does not touch the lengths. */
    CHECK(max_run == 31, "largest run is %u, B.14's is 31", max_run);
    CHECK(max_level == 40, "largest level is %u, B.14's is 40", max_level);

    /*
     * PREFIX-FREEDOM, over every pair including EOB and the escape.
     *
     * Two codes collide when the shorter one equals the longer one's leading
     * bits. Both historical collisions in this table were of that shape and
     * neither was visible in the output.
     */
    {
        u32 code[128], len[128], count = 0;
        u32 collisions = 0, a, b;

        for (i = 0; i < n && count < 126; i++) {
            u32 l = 0, r = 0, v = 0;

            q2_stx_code_at(i, &l, &r, &v);
            code[count] = q2_stx_code_bits(i);
            len[count]  = l;
            count++;
        }
        code[count] = Q2_STX_EOB_BITS;    len[count] = Q2_STX_EOB_LEN;    count++;
        code[count] = Q2_STX_ESCAPE_BITS; len[count] = Q2_STX_ESCAPE_LEN; count++;

        for (a = 0; a < count; a++) {
            for (b = 0; b < count; b++) {
                if (a == b || len[a] > len[b])
                    continue;
                if ((code[b] >> (len[b] - len[a])) == code[a]) {
                    printf("  COLLIDE: %u-bit 0x%X is a prefix of "
                           "%u-bit 0x%X\n", len[a], code[a], len[b], code[b]);
                    collisions++;
                }
            }
        }
        CHECK(collisions == 0, "%u prefix collisions", collisions);
    }

    /*
     * Every (run, level) pair distinct. A duplicate is unreachable — the
     * decoder takes the first match — and would mean a row was copied.
     */
    {
        u32 dupes = 0;

        for (i = 0; i < n; i++) {
            u32 li = 0, ri = 0, vi = 0, j;

            q2_stx_code_at(i, &li, &ri, &vi);
            for (j = i + 1; j < n; j++) {
                u32 lj = 0, rj = 0, vj = 0;

                q2_stx_code_at(j, &lj, &rj, &vj);
                if (ri == rj && vi == vj)
                    dupes++;
            }
        }
        CHECK(dupes == 0, "%u duplicated (run, level) pairs", dupes);
    }

    if (g_fail)
        printf("\n%d check(s) failed\n", g_fail);
    else
        printf("\nall movie table checks passed\n");
    return g_fail ? 1 : 0;
}
