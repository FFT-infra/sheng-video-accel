#!/bin/sh

# Real AV1 clip gate for the target. The 48-frame synthetic matrix passed on a
# build that decoded only 33 of 120 frames of a real clip, so a real clip is a
# separate gate, not a longer version of the matrix.
#
# Usage:
#   IRIS_REAL_CLIP_DIR=DIR [IRIS_REAL_CLIP_SET=all|plain|drc] \
#       sh test/iris-real-clip-gate.sh
#
# The directory holds the clips and their software framemd5 baselines:
#   av1-720.webm     + av1-720-sw.md5     (120 frames, all Import)
#   resize-av1.mkv   + resize-av1-sw.md5 (80 frames, DRC: 42 Import, 42 copy)
# Generate the baselines once with a software decoder; they are not produced
# here so a broken VA driver cannot bless its own output.
#
# The driver is chosen by the caller's environment. Leave LIBVA_DRIVERS_PATH
# unset to test the installed package, or point it at a build tree.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
. "$here/lib/iris-env.sh"

clip_dir=${IRIS_REAL_CLIP_DIR:-}
clip_set=${IRIS_REAL_CLIP_SET:-all}
out_base=${IRIS_REAL_CLIP_OUT:-$(iris_artifact_dir real-clip-gate)}
publish=${IRIS_REAL_CLIP_PUBLISH:-import}
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}

[ -n "$clip_dir" ] || {
    echo 'FAIL IRIS_REAL_CLIP_DIR must name the directory holding the clips and software framemd5 files' >&2
    exit 2
}
case $clip_set in all|plain|drc) ;; *) echo "FAIL unknown IRIS_REAL_CLIP_SET=$clip_set" >&2; exit 2 ;; esac
[ -d "$clip_dir" ] || { echo "FAIL clip directory missing: $clip_dir" >&2; exit 2; }
[ -e "$render" ] || { echo "FAIL render node missing: $render" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo 'FAIL ffmpeg is required' >&2; exit 2; }

mkdir -p "$out_base"
failures=0

run_clip() {
    name=$1
    clip=$2
    frames=$3
    imports=$4
    copies=$5
    invisible=$6

    [ -r "$clip_dir/$clip" ] || { echo "FAIL clip missing: $clip_dir/$clip" >&2; return 1; }
    [ -r "$clip_dir/$name-sw.md5" ] || {
        echo "FAIL software baseline missing: $clip_dir/$name-sw.md5" >&2
        return 1
    }
    out="$out_base/$name"
    mkdir -p "$out"

    # The VA decode writes both framemd5 and the driver trace. V4L2_VA_TRACE
    # is the driver's own ledger; the checker reads it rather than trusting
    # ffmpeg's exit status alone.
    if ! env -u LIBVA_DRIVER_NAME V4L2_VA_TRACE=1 V4L2_VA_PUBLISH="$publish" \
        ffmpeg -y -hide_banner -loglevel error -vaapi_device "$render" \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$clip_dir/$clip" \
        -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$out/va.md5" \
        2>"$out/driver.trace"; then
        echo "FAIL $name: ffmpeg VA decode failed" >&2
        return 1
    fi
    if ! sh "$here/iris-clip-trace-check.sh" \
        --trace "$out/driver.trace" --va-md5 "$out/va.md5" \
        --sw-md5 "$clip_dir/$name-sw.md5" --frames "$frames" \
        --import "$imports" --copy "$copies" --allow-invisible "$invisible"; then
        echo "FAIL $name: clip contract failed (artifacts in $out)" >&2
        return 1
    fi
    printf 'PASS %s frames=%s import=%s copy=%s invisible=%s\n' \
        "$name" "$frames" "$imports" "$copies" "$invisible"
}

if [ "$clip_set" = all ] || [ "$clip_set" = plain ]; then
    run_clip av1-720 av1-720.webm 120 120 0 0 || failures=$((failures + 1))
fi
if [ "$clip_set" = all ] || [ "$clip_set" = drc ]; then
    # The DRC clip's client keeps exporting surfaces at 1280x720 while the
    # CAPTURE pool is rebuilt at 896x480. The second half is copy-gpu by
    # design; the checker demands the recorded layout-mismatch reason.
    run_clip resize-av1 resize-av1.mkv 80 42 42 1 || failures=$((failures + 1))
fi

[ "$failures" -eq 0 ] || exit 1
echo "PASS real AV1 clip gate set=$clip_set publish=$publish"
