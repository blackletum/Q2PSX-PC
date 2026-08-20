#!/bin/sh
#
# convert.sh — the disc's three films to lossless .avi.
#
# stx2avi does the half ffmpeg cannot: it demuxes this port's 8-sector
# interleave and decodes MDEC video and CD-XA audio through the SAME code the
# game plays them with. ffmpeg does the half we have no reason to write: the
# container. Video arrives on a pipe as RGB24 and the audio as a .wav beside it.
#
# FFV1 rather than MJPEG or an uncompressed AVI, for one reason: the source is
# already lossy MDEC, and a second lossy pass over it would put artefacts in
# the archive that are the CONVERTER'S and not the disc's. FFV1 is exact, so a
# frame out of the .avi is the frame the decoder produced, bit for bit — which
# is what makes these files usable as a reference and not just as something to
# watch. The cost is size: about 1 MB per second at 320x192.
#
# Usage: convert.sh [disc] [outdir] [--retail]
#
# --retail cuts each film where the console's player cuts it (see movie.h);
# without it the whole video region is converted, including the frames the
# retail game never reaches.
#
set -e

DISC=${1:-"ref/extracted/Quake II (Europe).cue"}
OUTDIR=${2:-".tmp/cinematic-convert"}
RETAIL=${3:-}

STX2AVI=${STX2AVI:-./build/bin/stx2avi.exe}
FFMPEG=${FFMPEG:-ffmpeg}

command -v "$FFMPEG" >/dev/null 2>&1 || {
    for c in /c/ffmpeg/bin/ffmpeg.exe /usr/bin/ffmpeg; do
        [ -x "$c" ] && FFMPEG=$c && break
    done
}

mkdir -p "$OUTDIR"

for film in TAKE1BP OUTRO1P ROGUEINP; do
    wav="$OUTDIR/$film.wav"
    avi="$OUTDIR/$film.avi"

    echo "=== $film.STX"
    "$STX2AVI" "$DISC" audio "$film.STX" "$wav" $RETAIL

    # The video is piped rather than staged: 320x192x3 over a couple of
    # thousand frames is a ~450 MB intermediate nobody needs on disk.
    "$STX2AVI" "$DISC" video "$film.STX" $RETAIL | \
        "$FFMPEG" -hide_banner -loglevel warning \
            -f rawvideo -pixel_format rgb24 -video_size 320x192 -framerate 25 \
            -i - -i "$wav" \
            -c:v ffv1 -level 3 -c:a pcm_s16le \
            -y "$avi"

    rm -f "$wav"
done

echo
ls -la "$OUTDIR"
