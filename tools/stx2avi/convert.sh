#!/bin/sh
#
# convert.sh — the disc's three films to .avi, .mp4, or both.
#
# stx2avi does the half ffmpeg cannot: it demuxes this port's 8-sector
# interleave and decodes MDEC video and CD-XA audio through the SAME code the
# game plays them with. ffmpeg does the half we have no reason to write: the
# container. Video arrives on a pipe as RGB24 and the audio as a .wav beside it.
#
# ---------------------------------------------------------------------------
# Why the two formats are encoded differently on purpose
# ---------------------------------------------------------------------------
# The .avi is the ARCHIVE and the .mp4 is the COPY YOU WATCH. They are not the
# same job and encoding them the same way would do one of them badly.
#
#   .avi — FFV1 and PCM, both lossless, audio at the disc's own 37800 Hz. The
#          source is already lossy MDEC, so a second lossy pass would put the
#          CONVERTER's artefacts in the archive next to the disc's. A frame out
#          of this file is the frame the decoder produced, bit for bit. About
#          1 MB per second.
#
#   .mp4 — H.264 and AAC, because that is what plays everywhere. Two losses are
#          unavoidable here and are accepted deliberately:
#
#          1. AAC CANNOT STORE 37800 Hz. Its sample rates are a fixed table —
#             48000, 44100, 32000, ... — and the disc's rate is not in it, so
#             the audio must be resampled. soxr at precision 28 does it, which
#             is far finer than 4-bit ADPCM was ever carrying.
#          2. yuv420p subsamples chroma, and the decoder's output is RGB.
#
#          Both are inaudible/invisible next to the MDEC source, and neither is
#          allowed anywhere near the .avi.
#
# Usage: convert.sh [disc] [outdir] [--retail] [--format=avi|mp4|both]
#
# --retail cuts each film where the console's player cuts it (see movie.h);
# without it the whole video region is converted, including the frames the
# retail game never reaches.
#
set -e

DISC="ref/extracted/Quake II (Europe).cue"
OUTDIR=".tmp/cinematic-convert"
RETAIL=
FORMAT=both
positional=0

for arg in "$@"; do
    case "$arg" in
        --retail)   RETAIL=--retail ;;
        --format=*) FORMAT=${arg#--format=} ;;
        *)
            positional=$((positional + 1))
            [ "$positional" = 1 ] && DISC=$arg
            [ "$positional" = 2 ] && OUTDIR=$arg
            ;;
    esac
done

case "$FORMAT" in
    avi|mp4|both) ;;
    *) echo "convert.sh: --format must be avi, mp4 or both" >&2; exit 2 ;;
esac

STX2AVI=${STX2AVI:-./build/bin/stx2avi.exe}
FFMPEG=${FFMPEG:-ffmpeg}

command -v "$FFMPEG" >/dev/null 2>&1 || {
    for c in /c/ffmpeg/bin/ffmpeg.exe /usr/bin/ffmpeg; do
        [ -x "$c" ] && FFMPEG=$c && break
    done
}

mkdir -p "$OUTDIR"

# The .avi output: exact, and tagged with nothing it does not know.
avi_out() {
    echo "-map 0:v -map 1:a -c:v ffv1 -level 3 -c:a pcm_s16le -y $1.avi"
}

#
# The .mp4 output.
#
# The colour matrix is FORCED to match the tag rather than left to swscale's
# guess. Tagging bt709 while the conversion used bt601 is the classic way to
# ship a file whose colours are subtly wrong in every player at once — it costs
# one scale filter to make the two agree, so they agree.
#
mp4_out() {
    echo "-map 0:v -map 1:a \
-vf scale=out_color_matrix=bt709:out_range=tv \
-c:v libx264 -preset veryslow -crf 16 -pix_fmt yuv420p -profile:v high \
-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv \
-c:a aac -b:a 192k -af aresample=48000:resampler=soxr:precision=28 \
-movflags +faststart -y $1.mp4"
}

for film in TAKE1BP OUTRO1P ROGUEINP; do
    wav="$OUTDIR/$film.wav"
    stem="$OUTDIR/$film"
    outs=

    echo "=== $film.STX"
    "$STX2AVI" "$DISC" audio "$film.STX" "$wav" $RETAIL

    if [ "$FORMAT" = avi ] || [ "$FORMAT" = both ]; then
        outs="$outs $(avi_out "$stem")"
    fi
    if [ "$FORMAT" = mp4 ] || [ "$FORMAT" = both ]; then
        outs="$outs $(mp4_out "$stem")"
    fi

    # One decode feeds every requested container: the film is demuxed and
    # decoded once and ffmpeg writes both outputs off the same pipe. The video
    # is piped rather than staged because 320x192x3 over a couple of thousand
    # frames is a ~450 MB intermediate nobody needs on disk.
    "$STX2AVI" "$DISC" video "$film.STX" $RETAIL | \
        "$FFMPEG" -hide_banner -loglevel warning \
            -f rawvideo -pixel_format rgb24 -video_size 320x192 -framerate 25 \
            -i - -i "$wav" $outs

    rm -f "$wav"
done

echo
ls -la "$OUTDIR"
