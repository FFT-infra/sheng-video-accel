#!/bin/sh

# Decode throughput through this driver against the native V4L2 decoders.
#
# Run on the target. Every size is decoded twice: once through VA-API with the
# frames left on the VA surface (-hwaccel_output_format vaapi, -f null) and
# once through {h264,hevc}_v4l2m2m. The VA path is what a browser does; the
# native path copies each frame out, so it is a lower bound, not a rival.
#
# The reported fps is frames over wall clock for the whole run, startup
# included. It is a regression guard, not a benchmark: compare two builds on
# the same boot, not two machines.
#
# Usage:
#   test/iris-throughput.sh [--sizes 720,1080,4k] [--frames N]
#                           [--codecs h264,hevc] [--driver PATH]
#
# Defaults: 720p and 1080p, 300 frames per codec, H.264 and HEVC.
# 4k adds a memory cost of a few hundred megabytes per process.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/iris-env.sh
. "$here/lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

sizes=720,1080
codecs=h264,hevc
frames=300
driver=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sizes) sizes=${2:?}; shift 2 ;;
        --codecs) codecs=${2:?}; shift 2 ;;
        --frames) frames=${2:?}; shift 2 ;;
        --driver) driver=${2:?}; shift 2 ;;
        -h|--help) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'
[ -f "$driver" ] || fail "driver not found: $driver"
driver=$(CDPATH='' cd -- "$(dirname -- "$driver")" && pwd)/$(basename -- "$driver")
driver_dir=$(dirname "$driver")
driver_sha=$(sha256sum "$driver" | cut -d' ' -f1)

render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}

node=$(iris_resolve_device) || exit 1
boot_id=$(cat /proc/sys/kernel/random/boot_id)
root=$(iris_artifact_dir throughput)/$(date +%Y%m%d-%H%M%S)
mkdir -p "$root"

# The decoder is told which node to use explicitly: the node number is not
# stable across boots and a silent fallback would measure a camera.
va_env="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_dir"

size_args() {
    case "$1" in
        720) echo 1280x720 ;;
        1080) echo 1920x1080 ;;
        4k) echo 3840x2160 ;;
        *) fail "unknown size: $1" ;;
    esac
}

# One clip per size and codec so both paths read the same bytes.
make_clip() {
    codec=$1
    size=$2
    geometry=$(size_args "$size")
    out=$root/$codec-$size
    if [ "$codec" = h264 ]; then
        ffmpeg -y -hide_banner -loglevel error -f lavfi \
            -i "testsrc2=size=$geometry:rate=30" -frames:v "$frames" \
            -c:v libx264 -preset ultrafast -qp 20 -pix_fmt yuv420p "$out.h264"
    else
        ffmpeg -y -hide_banner -loglevel error -f lavfi \
            -i "testsrc2=size=$geometry:rate=30" -frames:v "$frames" \
            -c:v libx265 -preset ultrafast -x265-params crf=22:keyint=60 \
            -pix_fmt yuv420p "$out.h265"
    fi
    echo "$out"
}

report() {
    printf '%-6s %-5s %-16s %8.3f %8s %8.1f\n' "$1" "$2" "$3" "$4" "$5" "$6"
}

printf 'Iris throughput driver=%s sha256=%s node=%s frames=%s boot=%s\n' \
    "$driver" "$driver_sha" "$node" "$frames" "$boot_id"
printf '%-6s %-5s %-16s %8s %8s %8s\n' codec size path seconds user fps

status=0
old_ifs=$IFS
for codec in $(echo "$codecs" | tr ',' ' '); do
    for size in $(echo "$sizes" | tr ',' ' '); do
        clip=$(make_clip "$codec" "$size")
        ext=h264
        [ "$codec" = hevc ] && ext=h265

        va_log=$clip.va.log
        # shellcheck disable=SC2086
        env $va_env LIBVA_V4L2_VIDEO_PATH="$node" ffmpeg -hide_banner \
            -nostats -loglevel info -benchmark \
            -hwaccel vaapi -hwaccel_device "$render" \
            -hwaccel_output_format vaapi -i "$clip.$ext" -f null - \
            >"$va_log" 2>&1 || fail "$codec $size: VA decode failed, see $va_log"
        va_rt=$(sed -n 's/.*rtime=\([0-9.]*\)s.*/\1/p' "$va_log" | tail -1)
        va_ut=$(sed -n 's/.*utime=\([0-9.]*\)s.*/\1/p' "$va_log" | tail -1)
        [ -n "$va_rt" ] || fail "$codec $size: no benchmark line in $va_log"
        report "$codec" "$size" va-ffmpeg "$va_rt" "$va_ut" \
            "$(awk -v f="$frames" -v t="$va_rt" 'BEGIN { printf "%.1f", f / t }')"

        native_log=$clip.native.log
        start=$(date +%s%N)
        ffmpeg -hide_banner -nostats -loglevel error \
            -c:v "${codec}_v4l2m2m" -i "$clip.$ext" -f null - \
            >"$native_log" 2>&1 || fail "$codec $size: native decode failed, see $native_log"
        end=$(date +%s%N)
        native_rt=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", (b - a) / 1e9 }')
        report "$codec" "$size" native-v4l2m2m "$native_rt" '-' \
            "$(awk -v f="$frames" -v t="$native_rt" 'BEGIN { printf "%.1f", f / t }')"

        # Content: the VA frames must equal the native ones. Software on the
        # same pixel format is the reference so both sides hash the same
        # layout; a VA-only run that decodes nothing would still print fps.
        ffmpeg -hide_banner -loglevel error -i "$clip.$ext" -vf format=nv12 \
            -f framemd5 -y "$root/$codec-$size-sw.md5" || fail "$codec $size: software decode failed"
        # shellcheck disable=SC2086
        env $va_env LIBVA_V4L2_VIDEO_PATH="$node" ffmpeg -hide_banner -loglevel error \
            -hwaccel vaapi -hwaccel_device "$render" \
            -hwaccel_output_format vaapi -i "$clip.$ext" \
            -vf hwdownload,format=nv12 -f framemd5 -y "$root/$codec-$size-va.md5" \
            || fail "$codec $size: VA content decode failed"
        va_frames=$(grep -vc '^#' "$root/$codec-$size-va.md5")
        if diff "$root/$codec-$size-sw.md5" "$root/$codec-$size-va.md5" >/dev/null; then
            echo "PASS $codec $size content frames=$va_frames frame-md5"
        else
            echo "FAIL $codec $size content differs from the software decode"
            status=1
        fi
    done
done
IFS=$old_ifs

echo "artifacts: $root"
exit "$status"
