#!/bin/sh

# Real AV1 clip gate for the target. The 48-frame synthetic matrix passed on a
# build that decoded only 33 of 120 frames of a real clip, so a real clip is a
# separate gate, not a longer version of the matrix.
#
# Usage:
#   IRIS_REAL_CLIP_DIR=DIR [IRIS_REAL_CLIP_SET=all|plain|drc|4k] \
#       sh test/iris-real-clip-gate.sh
#
# The directory holds the clips, their software framemd5 baselines and a
# SHA256SUMS that pins both:
#   av1-720.webm     + av1-720-sw.md5     (120 frames, all Import)
#   resize-av1.mkv   + resize-av1-sw.md5 (80 frames, DRC: 40 Import, 40 copy)
#   av1-4k.mkv       + av1-4k-sw.md5     (30 frames, 3840x2160, all Import)
# Generate the baselines once with a software decoder; they are not produced
# here so a broken VA driver cannot bless its own output. Generate the pins
# with `(cd DIR && sha256sum *.webm *.mkv *.md5 > SHA256SUMS)`.
#
# The expected publish counts live in expected_counts() below, keyed by clip
# hash rather than by lane, because a count is a property of one exact file.
# A clip the table does not know is refused: measuring it against another
# file's numbers is how a replaced clip reads as a driver regression.
#
# The 4K lane is opt-in: IRIS_REAL_CLIP_SET=4k runs only that clip. It is a
# short Import capability probe, not a thermal, power or long-window result.
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
driver_path=${LIBVA_DRIVERS_PATH:-}

[ -n "$clip_dir" ] || {
    echo 'FAIL IRIS_REAL_CLIP_DIR must name the directory holding the clips and software framemd5 files' >&2
    exit 2
}
case $clip_set in all|plain|drc|4k) ;; *) echo "FAIL unknown IRIS_REAL_CLIP_SET=$clip_set" >&2; exit 2 ;; esac
[ -d "$clip_dir" ] || { echo "FAIL clip directory missing: $clip_dir" >&2; exit 2; }
[ -e "$render" ] || { echo "FAIL render node missing: $render" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo 'FAIL ffmpeg is required' >&2; exit 2; }
iris_require_media_pin "$clip_dir" || exit 2
if [ -n "$driver_path" ]; then
    [ -f "$driver_path/v4l2_drv_video.so" ] \
        || { echo "FAIL no v4l2_drv_video.so in $driver_path" >&2; exit 2; }
fi

mkdir -p "$out_base"
failures=0

# The driver the run exercises: a build tree when the caller pointed
# LIBVA_DRIVERS_PATH at one, the installed package otherwise. libva picks the
# built .so by driver name, so both variables have to travel together.
run_ffmpeg() {
    if [ -n "$driver_path" ]; then
        env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" "$@"
    else
        env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH "$@"
    fi
}

# The expected counts are a property of one exact file, not of the lane. Each
# row is "<sha256> <frames> <import> <copy> <invisible> <why>". A clip whose
# hash is absent is refused instead of being measured against another file's
# numbers: a replaced clip silently changed the recorded invisible count,
# which reads as a driver regression.
expected_counts() {
    sha=$1
    case $sha in
    # 1280x720 AV1, 120 frames. The internal-DPB translator replays 15
    # uncommitted pictures as hidden install frames; each contributes a
    # second, invisible completion under the same token, and the 120 visible
    # publications remain Import.
    722a6190610730ca36b43bf250ea0fb5814125e42d178584ce157b1dd2f19e79)
        echo '120 120 0 15' ;;
    # 80 frames, 1280x720 for the first 40 and 854x480 for the second. The
    # switch drains and reconfigures CAPTURE; the 40 pictures the client kept
    # exporting at 1280x720 are the recorded layout-mismatch fallback, and one
    # hidden install half lands between them.
    bd9a90e3211d8d9d59f240b470cededdfc11f7457ce1b6b218ab673789fbd67a)
        echo '80 40 40 1' ;;
    # 3840x2160, 30 frames, every picture Import.
    e1d565cbb0aee9ee8fe7019ffad05b84e0d53a56b41de48e346cb2bfa3187454)
        echo '30 30 0 0' ;;
    esac
}

run_clip() {
    name=$1
    clip=$2

    [ -r "$clip_dir/$clip" ] || { echo "FAIL clip missing: $clip_dir/$clip" >&2; return 1; }
    sha=$(iris_media_sha "$clip_dir" "$clip")
    [ -n "$sha" ] || { echo "FAIL $name: cannot hash $clip_dir/$clip" >&2; return 1; }
    counts=$(expected_counts "$sha")
    if [ -z "$counts" ]; then
        echo "FAIL $name: clip $clip (sha256 $sha) has no recorded counts." >&2
        echo "      Measure it once, then add a row to expected_counts() in this script" >&2
        echo "      and the file's hash to $clip_dir/SHA256SUMS." >&2
        return 1
    fi
    set -- $counts
    frames=$1 imports=$2 copies=$3 invisible=$4
    [ -r "$clip_dir/$name-sw.md5" ] || {
        echo "FAIL software baseline missing: $clip_dir/$name-sw.md5" >&2
        return 1
    }
    out="$out_base/$name"
    mkdir -p "$out"

    # The VA decode writes both framemd5 and the driver trace. V4L2_VA_TRACE
    # is the driver's own ledger; the checker reads it rather than trusting
    # ffmpeg's exit status alone.
    if ! run_ffmpeg V4L2_VA_TRACE=1 V4L2_VA_PUBLISH="$publish" \
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
    run_clip av1-720 av1-720.webm || failures=$((failures + 1))
fi
if [ "$clip_set" = all ] || [ "$clip_set" = drc ]; then
    # The DRC clip's client keeps exporting surfaces at 1280x720 while the
    # CAPTURE pool is rebuilt at 896x480. The second half is copy-gpu by
    # design; the checker demands the recorded layout-mismatch reason.
    run_clip resize-av1 resize-av1.mkv || failures=$((failures + 1))
fi
if [ "$clip_set" = 4k ]; then
    # A 4K AV1 clip is a capability probe, not part of the default three-clip
    # gate: run it only when the operator asks for that lane.
    run_clip av1-4k av1-4k.mkv || failures=$((failures + 1))
fi

[ "$failures" -eq 0 ] || exit 1
echo "PASS real AV1 clip gate set=$clip_set publish=$publish"
