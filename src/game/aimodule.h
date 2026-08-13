/*
 * aimodule.h — reading animation data out of a relocated creature module.
 *
 * A creature module is half data and half code. The code — the per-frame think
 * functions — cannot be executed by a native port and has to be reimplemented
 * creature by creature. The data can simply be read, and that is what this does.
 *
 * The split matters because it decides how much work remains. Every animation
 * on the disc (frame ranges, per-frame movement distances, which verb drives
 * each frame) is data and comes across automatically. What has to be written by
 * hand is only the behaviour behind each `think` index.
 *
 * ---------------------------------------------------------------------------
 * Finding the move tables
 * ---------------------------------------------------------------------------
 * There is no directory. Moves are found by scanning the relocated image for
 * records that satisfy every structural constraint at once:
 *
 *     s32 first_frame     >= 0 and <= last_frame
 *     s32 last_frame      - first_frame + 1 frames, a plausible count
 *     u32 frames          points inside the image, and the frames there decode
 *     u32 endfunc         points inside the image, or is 0
 *
 * A candidate is only accepted if the frame array it points at ALSO validates:
 * every ai byte in {0..5} or {0x80..0x85}, and the array fits in the image.
 *
 * ---------------------------------------------------------------------------
 * THIS SCAN OVER-MATCHES. Measured, not suspected.
 * ---------------------------------------------------------------------------
 * A disassembly-based census of the disc found roughly 160 moves and 2,675
 * frames across 22 modules. This scan, over only the 13 modules in COMMON.DAT,
 * returns 251 moves and 4,428 frames — more moves from fewer modules, so a
 * substantial fraction are false positives. Tightening the bounds to 48 frames
 * and a first frame under 256 brings it to 200 / 2,887, which is closer and
 * still too many.
 *
 * So this is a LEAD, not a solution, and it is labelled that way rather than
 * quietly shipped: 250 of 251 candidates contain a run of two or more frames
 * sharing a verb, which means the false positives look plausible and cannot be
 * filtered by inspecting them alone.
 *
 * The correct route is not a better heuristic. Each module registers a
 * descriptor through its exported header pointers, and that descriptor holds
 * the real move table. Following it is deterministic and needs no guessing.
 * That is the next piece of work; this scanner exists so the animation data can
 * be examined in the meantime, and so the eventual deterministic reader has
 * something to be checked against.
 *
 * ---------------------------------------------------------------------------
 * One explanation ruled out, so nobody spends a day on it
 * ---------------------------------------------------------------------------
 * A creature's spawn routine resolves its animations BY NAME at load and writes
 * the results into module BSS, which suggests the obvious theory that the
 * first/last frame fields are empty on disc and my scanner was validating
 * placeholder zeros.
 *
 * That is wrong. Measured across the 13 COMMON.DAT modules: of the records with
 * a valid frames pointer, 252 carry a non-zero frame range and exactly 0 carry
 * a zero one. The ranges are genuinely present in the file.
 *
 * So the over-matching is ordinary false positives from a permissive filter,
 * not a misunderstanding of where the data lives. The name resolution at spawn
 * must be doing something else — most likely binding animation NAMES to those
 * already-populated records rather than filling them in.
 */
#ifndef Q2PSX_AIMODULE_H
#define Q2PSX_AIMODULE_H

#include "monster.h"
#include "q2psx.h"
#include "reloc.h"

/* A move recovered from a module, with its frames already validated. */
typedef struct q2_ai_move {
    u32 offset;         /* module-relative offset of the move record */
    s32 first_frame;
    s32 last_frame;
    u32 frame_count;
    u32 frames_offset;
    u32 endfunc_offset; /* 0 when the move loops */
} q2_ai_move;

typedef struct q2_ai_moves {
    q2_ai_move *moves;
    u32         count;
    u32         capacity;
    u32         total_frames;
} q2_ai_moves;

/*
 * Scan a relocated module image for move tables.
 *
 * `base` is the address the image was relocated to, needed to turn a relocated
 * pointer back into a module-relative offset.
 */
q2_result q2_ai_moves_scan(q2_ai_moves *out, const u8 *image, size_t size,
                           u32 base);
void      q2_ai_moves_free(q2_ai_moves *m);

/* Read frame `index` of a move. */
bool q2_ai_move_frame(const q2_ai_moves *set, u32 move_index, u32 frame_index,
                      const u8 *image, size_t size, q2_mframe *out);

/* Longest run of consecutive frames whose ai verb is the same, which is a
 * cheap way to sanity-check that a recovered move looks like an animation
 * rather than a random window. */
u32 q2_ai_move_verb_run(const q2_ai_moves *set, u32 move_index,
                        const u8 *image, size_t size);

#endif /* Q2PSX_AIMODULE_H */
