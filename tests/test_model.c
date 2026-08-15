/*
 * test_model.c — block D's move table, and the rule that ties it to block C.
 *
 * The one thing worth pinning here is a unit. Block D counts TWO units per
 * animation frame, so a move spanning `start..end` is `(end - start) / 2 + 1`
 * frames. That is not a guess: on every model on the disc carrying both a move
 * table and a clip chain — 34 of them, across 25 zones — move `i`'s frame count
 * by this rule equals clip `i`'s length exactly, in list order.
 *
 * The values below are transcribed from BASE1 model 15, whose 31 clips read
 * 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 ... and whose moves
 * are Death1 0..214, Pain1 216..244, Pain2 246..286 and so on.
 *
 * A disc is not available to the test suite, so this pins the arithmetic rather
 * than the correspondence. If someone later "simplifies" the divisor, or drops
 * the inclusive +1, these fail.
 */
#include <stdio.h>
#include <string.h>

#include "model.h"

static int g_failures;
static int g_checks;

static void check_eq(s64 got, s64 want, const char *what)
{
    g_checks++;
    if (got != want) {
        printf("  FAIL  %s: got %lld, want %lld\n", what, (long long)got,
               (long long)want);
        g_failures++;
    }
}

static u32 frames_of(u16 start, u16 end)
{
    q2_model_move mv;
    memset(&mv, 0, sizeof(mv));
    mv.start = start;
    mv.end   = end;
    return q2_model_move_frames(&mv);
}

/* Real records from BASE1 model 15, against that model's real clip lengths. */
static void test_span_matches_clip_length(void)
{
    check_eq(frames_of(0, 214),      108, "Death1  0..214  -> clip 0  (108)");
    check_eq(frames_of(216, 244),     15, "Pain1   216..244 -> clip 1  (15)");
    check_eq(frames_of(246, 286),     21, "Pain2   246..286 -> clip 2  (21)");
    check_eq(frames_of(288, 394),     54, "Pain3   288..394 -> clip 3  (54)");
    check_eq(frames_of(396, 496),     51, "Pain4   396..496 -> clip 4  (51)");
    check_eq(frames_of(498, 532),     18, "Attack4 498..532 -> clip 5  (18)");
    check_eq(frames_of(534, 850),    159, "Death4  534..850 -> clip 6  (159)");
    check_eq(frames_of(1950, 2008),   30, "Walk   1950..2008 -> clip 13 (30)");
    check_eq(frames_of(2292, 2296),    3, "Fire 1 Ready      -> clip 17 (3)");
}

/* A single-frame move is start == end, not a zero-length one. */
static void test_degenerate_spans(void)
{
    check_eq(frames_of(100, 100), 1, "start == end is ONE frame, not zero");
    check_eq(frames_of(100,  98), 0, "end before start is rejected");
    check_eq(q2_model_move_frames(NULL), 0, "NULL is rejected");
}

/*
 * The two-unit gap between consecutive moves is exactly one frame, which is
 * what makes the moves tile the clip chain end to end with nothing left over.
 */
static void test_moves_tile_without_gaps(void)
{
    /* Death1 0..214, Pain1 216..244, Pain2 246..286: 108 + 15 + 21 = 144. */
    u32 total = frames_of(0, 214) + frames_of(216, 244) + frames_of(246, 286);
    check_eq(total, 144, "three consecutive moves total their three clips");
    check_eq(216 - 214, Q2_MODEL_BLOCKD_PER_FRAME, "the gap is one frame");
}


/*
 * The position formula, against the Arachner's real block-D table.
 *
 * Its `Walk` record is 360..418 in block-D units, so `start` is 360 and the
 * animation begins at position 360 * 5 = 1800. Its AI walk is frames 16..24, so
 * frame 16 sits at 1800 and each AI frame after adds 30.
 *
 * The x5 and the 30 are the two units meeting: block D counts 2 per animation
 * frame, the position counts 10, and an AI frame is three animation frames.
 * Getting either wrong is a silent mis-pose rather than a failure, which is why
 * this pins the arithmetic directly rather than through a lookup.
 */
static void test_position_formula(void)
{
    /* start * 5 + 30 * (f - first), computed the long way. */
    struct { s32 start, first, f, want; } k[] = {
        { 360, 16, 16, 1800 },          /* the move's own first frame */
        { 360, 16, 17, 1830 },          /* one AI frame on */
        { 360, 16, 24, 2040 },          /* its last, eight frames in */
        {   0,  0,  0,    0 },          /* a move at the timeline's start */
        {   0,  0, 10,  300 },          /* ten AI frames = 30 model frames */
    };
    u32 i;

    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        s32 got = k[i].start * 5
                + (k[i].f - k[i].first) * Q2_MODEL_POS_PER_MOVE_FRAME;
        check_eq(got, k[i].want, "position = start*5 + 30*(f - first)");
    }

    /* The two units, stated so a change to either is caught here. */
    check_eq(Q2_MODEL_POS_PER_MOVE_FRAME, 30, "30 position units per AI frame");
    check_eq(Q2_MODEL_BLOCKD_PER_FRAME,    2, "block D counts 2 per frame");
    check_eq(Q2_MODEL_TICKS_PER_FRAME,    10, "the position counts 10");
}

/* ------------------------------------------------------------------------- */
/* Walking off the end of the timeline                                        */
/* ------------------------------------------------------------------------- */
/*
 * `q2_model_anim_at` is the ENGINE'S loop and nothing more: `position <
 * clip->frames`, subtract, advance, with NO end-of-chain test anywhere
 * (0x8006B924's loop is four instructions and 0x80070188, its advance, is
 * `*cursor += delta`). It relies on the position always landing inside.
 *
 * It does not always land inside, and the reason is not arithmetic. An AI
 * move's length and its animation's extent are different quantities authored
 * independently (#63): the Enforcer's `Duck` is a 30-frame AI move whose
 * animation begins at model frame 1287 of a 1302-frame timeline. Five AI frames
 * of animation, twenty-five frames of nothing after it, and at `into = 5` the
 * position is frame 1302 — one past the last.
 *
 * `q2_model_anim_at_held` holds the last frame there. That is this port's
 * divergence and not the console's, so the two must stay distinguishable —
 * anything that quietly turned the first into the second would make the
 * divergence invisible. Before the hold, 678 of BASE2's 1000 poses fell back to
 * matching a clip by LENGTH, which is something the disc never does.
 *
 * The chain here is synthetic because these tests carry no disc: three clips of
 * 4, 6 and 5 frames, laid out exactly as `anim_read` reads them — u16 frames,
 * u16 flags, u32 delta to the next, then `frames` four-byte keys.
 */
#define TL_CLIPS 3

static u8 g_timeline[512];

static void build_timeline(q2_model *m)
{
    static const u16 k_frames[TL_CLIPS] = { 4, 6, 5 };
    /* The chain starts at 16, not 0: `anim_span` treats ofs_block_c == 0 as
     * "this model has no block C" and refuses. */
    u32 at = 16;
    int i;

    memset(g_timeline, 0, sizeof(g_timeline));
    memset(m, 0, sizeof(*m));

    for (i = 0; i < TL_CLIPS; i++) {
        u32 size = 8u + (u32)k_frames[i] * 4u;
        u32 next = (i + 1 < TL_CLIPS) ? size : 0;

        g_timeline[at + 0] = (u8)(k_frames[i] & 0xFF);
        g_timeline[at + 1] = (u8)(k_frames[i] >> 8);
        g_timeline[at + 4] = (u8)(next & 0xFF);
        g_timeline[at + 5] = (u8)((next >> 8) & 0xFF);
        at += size;
    }

    m->base              = g_timeline;
    m->size              = sizeof(g_timeline);
    m->hdr.ofs_block_c   = 16;
    m->hdr.ofs_block_d   = at;   /* block C ends where block D begins */
}

static void test_anim_at_holds_the_last_frame(void)
{
    q2_model m;
    q2_model_anim a;
    u32 within = 0;
    bool held = true;
    const u32 total = 4 + 6 + 5;

    build_timeline(&m);

    /* Inside: both walks agree and neither reports a hold. */
    {
        q2_model_anim plain;
        u32 pw = 0;

        check_eq(q2_model_anim_at(&m, 7, &plain, &pw), 1,
                 "frame 7 resolves on the engine's walk");
        check_eq(q2_model_anim_at_held(&m, 7, &a, &within, &held), 1,
                 "...and on the holding one");
        check_eq(held, 0, "frame 7 is not a hold");
        check_eq(within, pw, "both give the same offset into the clip");
        check_eq(within, 3, "which is 7 - 4, inside the six-frame clip");
    }

    /* The timeline's own last frame is inside it, not a hold. */
    check_eq(q2_model_anim_at_held(&m, total - 1, &a, &within, &held), 1,
             "the last frame of the timeline resolves");
    check_eq(held, 0, "and is the real last frame, not a hold");
    check_eq(within, 4, "at offset 4 of the five-frame clip");

    /* One past: the engine's walk fails, the holding walk holds. */
    check_eq(q2_model_anim_at(&m, total, &a, &within), 0,
             "one past the end fails the engine's own walk");
    check_eq(q2_model_anim_at_held(&m, total, &a, &within, &held), 1,
             "and is held by the port's");
    check_eq(held, 1, "which says so, so the divergence is visible");
    check_eq(a.frames, 5, "on the last clip");
    check_eq(within, 4, "at its last frame, not its first");

    /* Far past: the same place. A hold does not drift. */
    check_eq(q2_model_anim_at_held(&m, total + 5000, &a, &within, &held), 1,
             "far past the end still resolves");
    check_eq(held, 1, "and is held");
    check_eq(within, 4, "at the same frame — a hold does not drift");
}

int main(void)
{
    puts("block D: the move table");
    puts("=======================");

    test_span_matches_clip_length();
    test_degenerate_spans();
    test_moves_tile_without_gaps();
    test_position_formula();
    test_anim_at_holds_the_last_frame();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
