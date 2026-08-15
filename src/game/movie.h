/*
 * movie.h — playing a `.STX`: the schedule, the audio and the skip.
 *
 * `stx.h` decodes ONE frame and says so: who schedules frames, against which
 * clock, and what happens to the audio riding in the same file is not the
 * decoder's business. This is that part.
 *
 * ---------------------------------------------------------------------------
 * The clock is the container's, not the host's
 * ---------------------------------------------------------------------------
 * A `.STX` is an 8-sector interleave: slots 0-6 carry video and slot 7 carries
 * CD-XA ADPCM. One XA sector is 2016 stereo frames at 37800 Hz = 53.333 ms, and
 * the file gives one of those per eight sectors, so eight sectors of film is
 * 53.333 ms of sound and seven of picture. Two 320x192 frames fit in those
 * seven video sectors on the disc's cadence (6,5,5,5 sectors per frame), which
 * puts the picture at exactly 25.000 fps.
 *
 * That number is FORCED by the container and is not a choice this port makes.
 * A player therefore advances the film on its own accumulated time and not on
 * the game's tick, which is 30 Hz — driving one from the other would drop or
 * double every fifth frame.
 *
 * ---------------------------------------------------------------------------
 * What the audio needs that the music path does not
 * ---------------------------------------------------------------------------
 * The XA in a movie is the same codec as the streamed music (`xa.h`) with a
 * different multiplex: music is one channel in four of a `.XAI`, a movie is one
 * sector in eight of a `.STX`. The sound-group layout, the 2304-of-2324 trap
 * and the persistent per-channel history are all shared, so this uses `xa.h`'s
 * decoder and only replaces the sector selection.
 *
 * ---------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------------
 * The three films are 15, 20 and 30 MB. They are read from the disc in a
 * WINDOW rather than whole — a ring of sectors refilled as the cursor advances
 * — because a 30 MB allocation to play a two-minute clip is the sort of thing
 * that works on a development machine and fails on someone else's.
 */
#ifndef Q2PSX_GAME_MOVIE_H
#define Q2PSX_GAME_MOVIE_H

#include "disc.h"
#include "q2psx.h"
#include "stx.h"
#include "xa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exactly 25.000 fps, forced by the interleave — see the header comment. */
#define Q2_MOVIE_FPS 25.0

/* How many sectors the reader holds at once. One frame is at most 6 video
 * sectors and they can straddle an audio slot, so 32 is several frames of
 * slack and 64 KB of memory. */
#define Q2_MOVIE_WINDOW 32u

typedef struct q2_movie {
    const disc *disc;
    u32   first_lba;
    u32   sector_count;
    u32   cursor;             /* next sector to consider, file-relative     */

    u8    window[Q2_MOVIE_WINDOW * Q2_STX_SECTOR_SIZE];
    u32   window_base;        /* file-relative sector index of window[0]    */
    u32   window_have;        /* sectors valid in the window                */

    q2_stx_frame frame;       /* the frame most recently demuxed            */
    bool  frame_valid;
    u32   frames_shown;

    double clock;             /* seconds of film presented so far           */
    bool   finished;

    /* Audio: the same decoder the music uses, over a different multiplex. */
    q2_xa_decoder xa;
    u32   audio_cursor;       /* next sector to consider for audio          */
} q2_movie;

/*
 * Open `path` (e.g. "Q2DATA/MOVIES/OUTRO1P.STX") on `d`.
 *
 * Fails if the file is not there or is not a `.STX`. Nothing is decoded here;
 * the first frame arrives on the first `q2_movie_advance`.
 */
bool q2_movie_open(q2_movie *m, const disc *d, const char *path);

/*
 * Advance the film by `dt` seconds and decode whatever frame that lands on.
 *
 * Returns true when a NEW frame was decoded into `rgb` (which must hold
 * `Q2_STX_WIDTH * Q2_STX_HEIGHT * 3` bytes). Returns false when the film has
 * not moved on to the next frame yet — in which case the caller keeps showing
 * what it has — or when the film has ended, which `q2_movie_finished` reports.
 *
 * Frames are decoded at most one per call: a caller that stalls for a second
 * does not then burn twenty-five decodes in one frame, it drops the interval
 * and carries on. That is what the console does too, since the drive keeps
 * streaming whether or not the MDEC kept up.
 */
bool q2_movie_advance(q2_movie *m, double dt, u8 *rgb);

/* Has the film run out of frames? */
Q2PSX_INLINE bool q2_movie_finished(const q2_movie *m) { return m->finished; }

/*
 * Pull up to `max_samples` of interleaved stereo PCM16 out of the audio slots,
 * returning how many were written. Call it as often as the audio device needs
 * feeding; it is independent of the video clock, because on the console the two
 * are independent — the SPU plays the XA the drive delivers whatever the MDEC
 * is doing.
 */
u32 q2_movie_audio(q2_movie *m, s16 *out, u32 max_samples);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_GAME_MOVIE_H */
