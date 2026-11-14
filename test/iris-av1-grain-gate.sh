#!/bin/sh

# AV1 film grain gate for the target. Clients that ask for grain name two
# surfaces: the picture decodes into current_frame, and the grained picture
# the client shows is current_display_picture. Mesa's drivers blend it on the
# GPU; this driver blends it in software from the decode's completion
# (src/va/grain.cc). This gate is what says the blend is right: every frame's
# framemd5 must be byte-identical to libdav1d's own grained output.
#
# Usage:
#   IRIS_GRAIN_CLIP_DIR=DIR [IRIS_GRAIN_CLIP_SET=all|nv12|p010] \
#       sh test/iris-av1-grain-gate.sh
#
# The directory holds the clips and their software framemd5 baselines:
#   grain8.webm  + grain8-sw.md5   (8-bit NV12, every frame grained)
#   grain10.webm + grain10-sw.md5  (10-bit P010, every frame grained)
# Generate a baseline once with a decoder that applies grain itself:
#   ffmpeg -c:v libdav1d -i grain8.webm -f framemd5 grain8-sw.md5
# Do not generate it with this driver: a broken blend would bless its own
# output.
#
# The driver is chosen by the caller's environment. Leave LIBVA_DRIVERS_PATH
# unset to test the installed package, or point it at a build tree. A
# DESTDIR staging run is not an installation: record which one ran.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib/iris-env.sh"

clip_dir=${IRIS_GRAIN_CLIP_DIR:-}
clip_set=${IRIS_GRAIN_CLIP_SET:-all}
out_base=${IRIS_GRAIN_OUT:-$(iris_artifact_dir av1-grain-gate)}
publish=${IRIS_GRAIN_PUBLISH:-import}
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
driver_path=${LIBVA_DRIVERS_PATH:-}

[ -n "$clip_dir" ] || {
    echo 'FAIL IRIS_GRAIN_CLIP_DIR must name the directory holding the clips and their software framemd5 files' >&2
    exit 2
}
case $clip_set in all|nv12|p010) ;; *) echo "FAIL unknown IRIS_GRAIN_CLIP_SET=$clip_set" >&2; exit 2 ;; esac
[ -d "$clip_dir" ] || { echo "FAIL clip directory missing: $clip_dir" >&2; exit 2; }
[ -e "$render" ] || { echo "FAIL render node missing: $render" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo 'FAIL ffmpeg is required' >&2; exit 2; }
if [ -n "$driver_path" ]; then
    [ -f "$driver_path/v4l2_drv_video.so" ] \
        || { echo "FAIL no v4l2_drv_video.so in $driver_path" >&2; exit 2; }
fi

mkdir -p "$out_base"
failures=0

# The driver the run exercises: a build tree when the caller pointed
# LIBVA_DRIVERS_PATH at one, the installed package otherwise. Both variables
# go together; libva picks the built .so by name.
run_ffmpeg() {
    if [ -n "$driver_path" ]; then
        env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" "$@"
    else
        env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH "$@"
    fi
}

# The download: the planar form libdav1d writes. hwdownload yields the
# surface's own sample layout (NV12 or P010), so a P010 clip converts to
# yuv420p10le here; a mismatch in this chain fails the hash comparison for a
# reason that has nothing to do with the grain. The counts are the clip's
# picture structure, recorded from the reference run: `imports` is one per
# submitted picture (AV1 also submits frames it never shows), and `invisible`
# is the install half of each two-completion unit.
run_clip() {
    name=$1
    clip=$2
    frames=$3
    filter=$4
    imports=${5:-$frames}
    copies=${6:-0}
    invisible=${7:-0}

    [ -r "$clip_dir/$clip" ] || { echo "FAIL clip missing: $clip_dir/$clip" >&2; return 1; }
    [ -r "$clip_dir/$name-sw.md5" ] || {
        echo "FAIL software baseline missing: $clip_dir/$name-sw.md5" >&2
        return 1
    }
    out="$out_base/$name"
    mkdir -p "$out"

    if ! run_ffmpeg V4L2_VA_TRACE=1 V4L2_VA_PUBLISH="$publish" \
        ffmpeg -y -hide_banner -loglevel error -vaapi_device "$render" \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$clip_dir/$clip" \
        -vf "$filter" -f framemd5 "$out/va.md5" \
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
    # At least one blend per displayed picture. A hidden picture's blend can
    # be skipped when its display surface is reused before anyone asks for
    # it, which is why the count is not exact; a displayed picture that lost
    # its blend shows up as a hash mismatch above, not here.
    blends=$(grep -c '^va grain ' "$out/driver.trace" || true)
    [ "$blends" -ge "$frames" ] || {
        echo "FAIL $name: $blends grain blends, expected at least $frames" >&2
        return 1
    }
    printf 'PASS %s frames=%s blends=%s import=%s copy=%s invisible=%s\n' \
        "$name" "$frames" "$blends" "$imports" "$copies" "$invisible"
}

if [ "$clip_set" = all ] || [ "$clip_set" = nv12 ]; then
    # 320x240, film-grain-test=1: an 8-bit stream whose every visible frame
    # carries grain parameters, so the whole blend path is exercised rather
    # than a single salted frame.
    run_clip grain8 grain8.webm 20 'hwdownload,format=yuv420p' 21 0 11 || failures=$((failures + 1))
fi
if [ "$clip_set" = all ] || [ "$clip_set" = p010 ]; then
    # 352x290, 10-bit: the same blend over P010 storage, where the 10-bit
    # scaling curve and the right-aligned codes are the parts that can be off
    # by one and still look plausible.
    run_clip grain10 grain10.webm 2 'hwdownload,format=p010le,format=yuv420p10le' || failures=$((failures + 1))
fi

[ "$failures" -eq 0 ] || exit 1
echo "PASS AV1 film grain gate set=$clip_set publish=$publish driver=${driver_path:-installed}"
