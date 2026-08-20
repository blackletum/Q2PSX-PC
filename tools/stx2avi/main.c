/*
 * stx2avi — the disc's three films out of `.STX` and into something that plays.
 *
 * There is no off-the-shelf converter for these. ffmpeg has a `psxstr` demuxer,
 * but it wants 2352-byte raw CD sectors carrying Sony's channel multiplex, and
 * a `.STX` is this port's own 8-sector interleave (slots 0-6 MDEC video, slot 7
 * CD-XA ADPCM) reached through the ISO filesystem. So the DEMUX and the DECODE
 * are ours — they already exist, verified, in stx.c / xa.c / movie.c — and only
 * the CONTAINER is ffmpeg's. This tool is the seam: it turns a film into the two
 * raw streams ffmpeg can mux, and does not attempt to write an AVI itself.
 *
 * A converter that re-implemented the decoder would be a SECOND decoder, and a
 * second decoder is a second answer to "what does this frame look like". This
 * one drives `q2_movie` — the same player the game uses — so the picture in the
 * .avi is the picture the port shows, or the bug is in both.
 *
 *   stx2avi <disc> info
 *   stx2avi <disc> video <FILM.STX> [--retail]     -> RGB24 to stdout
 *   stx2avi <disc> audio <FILM.STX> <out.wav> [--retail]
 *
 * `--retail` stops where the console's player stops: the second argument of
 * play() is a frame limit and all three films are cut short by it, so the disc
 * holds more picture than anyone playing it has seen. Without it the whole
 * video region is converted, which is what an archive of the disc wants.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "disc.h"
#include "movie.h"
#include "q2psx.h"
#include "stx.h"
#include "xa.h"

#define RGB_BYTES (Q2_STX_WIDTH * Q2_STX_HEIGHT * 3u)

/* One sector of XA is 2016 stereo frames; pull a few at a time. */
#define PCM_CHUNK (XA_FRAMES_PER_SECTOR * 2u * 8u)

static const char *const k_film[] = {
    "Q2DATA/MOVIES/TAKE1BP.STX",
    "Q2DATA/MOVIES/OUTRO1P.STX",
    "Q2DATA/MOVIES/ROGUEINP.STX"
};
#define FILM_COUNT (sizeof(k_film) / sizeof(k_film[0]))

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;

        if (ca != cb)
            return 0;
        a++;
        b++;
    }

    return !*a && !*b;
}

static const char *film_base(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

/* Accept either "ROGUEINP.STX" or the full disc path. */
static const char *film_resolve(const char *name)
{
    size_t i;

    if (!name || !*name)
        return NULL;

    for (i = 0; i < FILM_COUNT; i++)
        if (name_eq(name, film_base(k_film[i])) || name_eq(name, k_film[i]))
            return k_film[i];

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* WAV                                                                        */
/* ------------------------------------------------------------------------- */
static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void put_u16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

/* Canonical 44-byte PCM header. `data_bytes` is patched in on close. */
static void wav_header(u8 *h, u32 data_bytes)
{
    const u32 rate  = XA_SAMPLE_RATE;
    const u16 chans = XA_CHANNELS;
    const u16 bits  = 16;
    const u16 align = (u16)(chans * (bits / 8));

    memcpy(h + 0, "RIFF", 4);
    put_u32(h + 4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16u);              /* fmt chunk size */
    put_u16(h + 20, 1u);               /* PCM            */
    put_u16(h + 22, chans);
    put_u32(h + 24, rate);
    put_u32(h + 28, rate * align);     /* byte rate      */
    put_u16(h + 32, align);
    put_u16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

/* ------------------------------------------------------------------------- */
static int cmd_info(const disc *d)
{
    static q2_movie m;
    static u8 rgb[RGB_BYTES];
    size_t i;

    printf("%-14s %10s %8s %8s %8s\n",
           "film", "bytes", "sectors", "frames", "retail");

    for (i = 0; i < FILM_COUNT; i++) {
        const disc_file *f = disc_find(d, k_film[i]);
        const char *base   = film_base(k_film[i]);
        u32 frames = 0;

        if (!f) {
            printf("%-14s %10s\n", base, "absent");
            continue;
        }

        if (q2_movie_open(&m, d, k_film[i]))
            while (!q2_movie_finished(&m))
                if (q2_movie_advance(&m, 1.0 / Q2_MOVIE_FPS, rgb))
                    frames++;

        printf("%-14s %10u %8u %8u %8u\n", base, f->size,
               (unsigned)((f->size + Q2_STX_SECTOR_SIZE - 1) /
                          Q2_STX_SECTOR_SIZE),
               frames, q2_movie_retail_length(base));
    }

    return 0;
}

static int cmd_video(const disc *d, const char *path, bool retail)
{
    static q2_movie m;
    static u8 rgb[RGB_BYTES];
    u32 frames = 0;

    if (!q2_movie_open(&m, d, path)) {
        fprintf(stderr, "stx2avi: cannot open %s\n", path);
        return 1;
    }
    if (retail)
        m.frame_limit = q2_movie_retail_length(film_base(path));

    while (!q2_movie_finished(&m)) {
        if (!q2_movie_advance(&m, 1.0 / Q2_MOVIE_FPS, rgb))
            continue;
        if (fwrite(rgb, 1, RGB_BYTES, stdout) != RGB_BYTES) {
            fprintf(stderr, "stx2avi: short write on frame %u\n", frames);
            return 1;
        }
        frames++;
    }

    fflush(stdout);
    fprintf(stderr, "stx2avi: %s — %u frames of video\n",
            film_base(path), frames);
    return 0;
}

static int cmd_audio(const disc *d, const char *path, const char *out,
                     bool retail)
{
    static q2_movie m;
    static s16 pcm[PCM_CHUNK];
    u8    header[44];
    FILE *fp;
    u64   total_samples = 0;
    u64   cap = 0;

    if (!q2_movie_open(&m, d, path)) {
        fprintf(stderr, "stx2avi: cannot open %s\n", path);
        return 1;
    }

    /*
     * Under --retail the picture stops early, so the sound must too, or the
     * .avi carries seconds of audio over a film that has ended. The cut is the
     * limit's own duration: the limit is a frame NUMBER and the frame carrying
     * it is not shown, so the film is (limit - 1) frames at 25 fps.
     */
    if (retail) {
        u32 limit = q2_movie_retail_length(film_base(path));

        if (limit)
            cap = (u64)((double)(limit - 1u) / Q2_MOVIE_FPS *
                        XA_SAMPLE_RATE) * XA_CHANNELS;
    }

    fp = fopen(out, "wb");
    if (!fp) {
        fprintf(stderr, "stx2avi: cannot write %s\n", out);
        return 1;
    }

    memset(header, 0, sizeof(header));
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return 1;
    }

    for (;;) {
        u32 got = q2_movie_audio(&m, pcm, PCM_CHUNK);

        if (!got)
            break;
        if (cap && total_samples + got > cap)
            got = (u32)(cap - total_samples);
        if (!got)
            break;

        if (fwrite(pcm, sizeof(s16), got, fp) != got) {
            fprintf(stderr, "stx2avi: short write on audio\n");
            fclose(fp);
            return 1;
        }
        total_samples += got;
        if (cap && total_samples >= cap)
            break;
    }

    wav_header(header, (u32)(total_samples * sizeof(s16)));
    if (fseek(fp, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "stx2avi: cannot patch the WAV header\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    fprintf(stderr, "stx2avi: %s — %llu stereo frames of audio (%.2f s)\n",
            film_base(path),
            (unsigned long long)(total_samples / XA_CHANNELS),
            (double)total_samples / XA_CHANNELS / XA_SAMPLE_RATE);
    return 0;
}

/* ------------------------------------------------------------------------- */
static void usage(void)
{
    fprintf(stderr,
            "usage: stx2avi <disc> info\n"
            "       stx2avi <disc> video <FILM.STX> [--retail]\n"
            "       stx2avi <disc> audio <FILM.STX> <out.wav> [--retail]\n"
            "\n"
            "<disc> is a .cue, .bin, .iso or an extracted Q2DATA directory.\n"
            "video writes RGB24 %ux%u at %u fps to stdout, for ffmpeg.\n",
            Q2_STX_WIDTH, Q2_STX_HEIGHT, Q2_STX_FPS);
}

int main(int argc, char **argv)
{
    disc *d = NULL;
    q2_result rc;
    const char *cmd;
    bool retail = false;
    int i, ret;

    if (argc < 3) {
        usage();
        return 2;
    }

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--retail") == 0)
            retail = true;

#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    rc = disc_open(&d, argv[1]);
    if (rc != Q2_OK) {
        fprintf(stderr, "stx2avi: %s: %s\n", argv[1], q2_result_str(rc));
        return 1;
    }

    cmd = argv[2];
    if (strcmp(cmd, "info") == 0) {
        ret = cmd_info(d);
    } else if (strcmp(cmd, "video") == 0 && argc >= 4) {
        const char *path = film_resolve(argv[3]);

        if (!path) {
            fprintf(stderr, "stx2avi: %s is not one of the disc's films\n",
                    argv[3]);
            ret = 2;
        } else {
            ret = cmd_video(d, path, retail);
        }
    } else if (strcmp(cmd, "audio") == 0 && argc >= 5) {
        const char *path = film_resolve(argv[3]);

        if (!path) {
            fprintf(stderr, "stx2avi: %s is not one of the disc's films\n",
                    argv[3]);
            ret = 2;
        } else {
            ret = cmd_audio(d, path, argv[4], retail);
        }
    } else {
        usage();
        ret = 2;
    }

    disc_close(d);
    return ret;
}
