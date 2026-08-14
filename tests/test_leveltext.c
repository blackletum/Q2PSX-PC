/*
 * test_leveltext.c — the `Strings` chunk, built by hand and read back.
 *
 * The record is {char name[12]; u32 offset}, and the trap in it is that the
 * name is a fixed field rather than a C string: `FoundASecret` fills all twelve
 * bytes with no terminator. A decoder that calls strncpy-and-hope reads the
 * next record's offset bytes as part of the name and every lookup after it
 * misses. So the first fixture below is deliberately a full-width name.
 */
#include "leveltext.h"

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

/* BASE0's chunk, rebuilt: the directory, a zero record, then the text. */
static u8 g_chunk[0x200];
static u32 g_size;

static void put_record(u32 index, const char *name, u32 offset)
{
    u8 *r = g_chunk + index * 16;
    size_t n = strlen(name);

    memset(r, 0, 16);
    memcpy(r, name, n > 12 ? 12 : n);
    r[12] = (u8)(offset & 0xFF);
    r[13] = (u8)((offset >> 8) & 0xFF);
    r[14] = (u8)((offset >> 16) & 0xFF);
    r[15] = (u8)((offset >> 24) & 0xFF);
}

static u32 put_text(u32 at, const char *s)
{
    size_t n = strlen(s) + 1;

    memcpy(g_chunk + at, s, n);
    return at + (u32)n;
}

static void build(void)
{
    u32 t = 0xA0;

    memset(g_chunk, 0, sizeof(g_chunk));

    put_record(0, "MapTitle",     t);          t = put_text(t, "Strogg Outpost");
    put_record(1, "FoundASecret", t);          t = put_text(t, "You have found a secret.");
    put_record(2, "Unit1Miss1",   t);          t = put_text(t, "Establish Communication Link to Command Ship.");
    put_record(3, "Unit1Curr0",   t);          t = put_text(t, "Find entrance to Strogg Base.");
    put_record(4, "Unit2CurrA",   t);          t = put_text(t, "Locate Security Grid entrance beneath the Pyramid.");
    put_record(5, "Default",      t);          t = put_text(t, "Base0*");
    /* record 6 is left all zero — that is the terminator */
    g_size = t;
}

static void test_parse(void)
{
    q2_leveltext tx;

    build();
    CHECK(q2_leveltext_parse(&tx, g_chunk, g_size) == Q2_OK, "it parses");
    CHECK(tx.count == 6, "six records before the zero one, got %u", tx.count);

    /* The twelve-character name is the one that breaks a naive reader. */
    CHECK(strcmp(tx.entry[1].name, "FoundASecret") == 0,
          "a full-width name reads back whole, got \"%s\"", tx.entry[1].name);
    CHECK(strlen(tx.entry[1].name) == 12, "and is exactly twelve characters");

    CHECK(strcmp(q2_leveltext_find(&tx, "MapTitle"), "Strogg Outpost") == 0,
          "MapTitle");
    CHECK(strcmp(q2_leveltext_find(&tx, "Default"), "Base0*") == 0, "Default");
    CHECK(q2_leveltext_find(&tx, "NoSuchKey") == NULL,
          "a missing key is NULL, not a wrong answer");
    CHECK(q2_leveltext_find(&tx, "MapTitl") == NULL,
          "and a prefix does not match");
}

static void test_keys(void)
{
    char key[Q2_LEVELTEXT_NAME_LEN + 1];
    q2_leveltext tx;

    build();
    q2_leveltext_parse(&tx, g_chunk, g_size);

    q2_leveltext_key_objective(key, 1);
    CHECK(strcmp(key, "Unit1Miss1") == 0, "the objective key, got \"%s\"", key);
    CHECK(q2_leveltext_find(&tx, key) != NULL, "and it resolves");

    q2_leveltext_key_orders(key, 1, 0);
    CHECK(strcmp(key, "Unit1Curr0") == 0, "the orders key, got \"%s\"", key);

    /*
     * The step is hex. SECURITY runs Unit2Curr1..Unit2CurrA, so step 10 has to
     * format as `A` — with %d it becomes "Unit2Curr10", which finds nothing and
     * reads as "this map has no orders" rather than as a bug.
     */
    q2_leveltext_key_orders(key, 2, 10);
    CHECK(strcmp(key, "Unit2CurrA") == 0, "step 10 is A, got \"%s\"", key);
    CHECK(q2_leveltext_find(&tx, key) != NULL, "and that one resolves too");

    /* Every key must fit the field, or it would be truncated on the disc. */
    q2_leveltext_key_orders(key, 9, 15);
    CHECK(strlen(key) <= Q2_LEVELTEXT_NAME_LEN,
          "the widest key still fits twelve bytes: \"%s\"", key);
}

static void test_rejects_bad(void)
{
    q2_leveltext tx;

    build();

    /* An offset past the end must be refused, not followed. */
    g_chunk[12] = 0xFF; g_chunk[13] = 0xFF;
    CHECK(q2_leveltext_parse(&tx, g_chunk, g_size) != Q2_OK,
          "an out-of-range offset is refused");

    /* Text that runs off the end of the chunk must be refused too — this is
     * the one that would otherwise walk the heap on the first printf. */
    build();
    memset(g_chunk + 0xA0, 'x', g_size - 0xA0);
    CHECK(q2_leveltext_parse(&tx, g_chunk, g_size) != Q2_OK,
          "unterminated text is refused");

    CHECK(q2_leveltext_parse(NULL, g_chunk, g_size) != Q2_OK, "NULL out");
    CHECK(q2_leveltext_parse(&tx, NULL, 0) != Q2_OK, "NULL data");
}

int main(void)
{
    test_parse();
    test_keys();
    test_rejects_bad();

    if (g_fail) {
        printf("\n%d leveltext check%s failed\n", g_fail, g_fail == 1 ? "" : "s");
        return 1;
    }
    printf("leveltext: all checks passed\n");
    return 0;
}
