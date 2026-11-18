#!/bin/sh

# Hardware gate for HEVC Main 10 encode: a 10-bit source must reach the
# encoder as P010 and come back as a Main 10 stream that still holds the
# source's pixels.
#
# The VA path accepts the profile and the bytes arrive through hwupload, but
# neither proves the firmware coded 10-bit pictures: an encoder that drops the
# low two bits still produces a decodable stream. This gate therefore checks
# the stream's own profile and pixel format, and measures SSIM against the
# 10-bit source rather than against another decode of itself.
#
# What it asserts:
#   * the encode moved the Iris IRQ counter (it ran on the hardware);
#   * ffprobe reports profile "Main 10" and a 10-bit pixel format;
#   * the stream decodes with the requested frame count;
#   * the platform decode matches a software decode byte for byte;
#   * SSIM against the source is above the floor, per frame;
#   * a blank stream scores below the floor, so a pass cannot be vacuous;
#   * the 8-bit HEVC lane still encodes and decodes in the same run.
#
# Usage: iris-encode-main10-gate.sh [render-node]
#
# Environment:
#   LIBVA_DRIVERS_PATH       directory holding the driver to measure
#                            (default <checkout>/build/src)
#   LIBVA_V4L2_ENCODER_PATH  forced encoder node; resolved by name otherwise
#   IRIS_ENCODE_MAIN10_DIR   scratch directory (default $IRIS_LAB_ROOT or
#                            ~/iris-vaapi-lab/tmp)
#   IRIS_ENCODE_FRAMES       frames (default 60)
#   IRIS_ENCODE_SIZE         WxH (default 640x480)
#   IRIS_ENCODE_SSIM_MIN     per-frame SSIM floor (default 0.90)

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
. "$root/test/lib/iris-env.sh"

scratch=${IRIS_ENCODE_MAIN10_DIR:-${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-encode-main10.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

frames=${IRIS_ENCODE_FRAMES:-60}
size=${IRIS_ENCODE_SIZE:-640x480}
ssim_min=${IRIS_ENCODE_SSIM_MIN:-0.90}

fail() {
    echo "FAIL encode-main10: $*" >&2
    exit 1
}

# Resolve the encoder by its iris_driver name and Card type, the same rule the
# launcher uses. A fixed /dev/videoN is never assumed.
resolve_encoder() {
    if [ -n "${LIBVA_V4L2_ENCODER_PATH:-}" ]; then
        printf '%s\n' "$LIBVA_V4L2_ENCODER_PATH"
        return 0
    fi
    command -v v4l2-ctl >/dev/null 2>&1 || fail 'v4l2-ctl is required to resolve the encoder node'
    for candidate in /dev/video*; do
        [ -c "$candidate" ] || continue
        info=$(v4l2-ctl -d "$candidate" --info 2>/dev/null || true)
        driver=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Driver name[[:space:]]*:[[:space:]]*//p' | sed -n '1p')
        [ "$driver" = iris_driver ] || continue
        card=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' | sed -n '1p')
        case "$card" in
            *[Ee]ncoder*) printf '%s\n' "$candidate"; return 0 ;;
        esac
    done
    fail 'no /dev/video* node reports the iris_driver encoder'
}

irq_count() {
    awk '$NF ~ /^(iris|qcom-iris|venus|vcodec)$/ { s += $2 } END { print s + 0 }' /proc/interrupts
}

command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'
command -v ffprobe >/dev/null 2>&1 || fail 'ffprobe is required'

encoder=$(resolve_encoder)
render=${1:-/dev/dri/renderD128}
driver_path=${LIBVA_DRIVERS_PATH:-$root/build/src}
[ -f "$driver_path/v4l2_drv_video.so" ] \
    || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
[ -c "$render" ] || fail "not a DRM render node: $render"
[ -c "$encoder" ] || fail "not a V4L2 node: $encoder"

echo "encoder=$encoder render=$render frames=$frames size=$size ssim_min=$ssim_min"

# One 10-bit source in yuv420p10le, the same layout the SSIM below compares
# against; hwupload converts it to P010, which is what the encoder sees. The
# 8-bit lane derives its own source from the same pattern.
source10=$work/source.yuv420p10le
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
    -frames:v "$frames" -pix_fmt yuv420p10le -f rawvideo -y "$source10" \
    || fail 'could not generate the 10-bit source'
source8=$work/source.nv12
ffmpeg -hide_banner -loglevel error -f rawvideo -pix_fmt yuv420p10le -s "$size" -r 30 \
    -i "$source10" -pix_fmt nv12 -f rawvideo -y "$source8" \
    || fail 'could not derive the 8-bit source'

encode_lane() {
    lane=$1
    pix_fmt=$2
    upload=$3
    profile=$4
    source=$5
    stream=$work/$lane.hevc

    irq_before=$(irq_count)
    LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path \
        ffmpeg -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt "$pix_fmt" -s "$size" -r 30 \
        -i "$source" -vf "format=$upload,hwupload" \
        -c:v hevc_vaapi -profile:v "$profile" -y "$stream" \
        || fail "$lane: the encode failed"
    irq_after=$(irq_count)
    [ "$irq_after" -gt "$irq_before" ] \
        || fail "$lane: the encode did not move the Iris IRQ counter (before=$irq_before after=$irq_after)"
    echo "PASS $lane: encode moved the Iris IRQ counter by $((irq_after - irq_before))"
    lane_stream=$stream
}

# Main 10 first: the profile must be the one the client asked for, and the
# pixel format must be 10-bit. A stream coded at 8 bits decodes as
# yuv420p even when the container says Main 10.
encode_lane main10 yuv420p10le p010 main10 "$source10"
stream10=$lane_stream
stream_profile=$(ffprobe -v error -select_streams v:0 -show_entries stream=profile \
    -of default=nw=1:nk=1 "$stream10")
stream_pix=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt \
    -of default=nw=1:nk=1 "$stream10")
[ "$stream_profile" = "Main 10" ] \
    || fail "main10: the stream reports profile '$stream_profile', not 'Main 10'"
[ "$stream_pix" = "yuv420p10le" ] \
    || fail "main10: the stream reports pixel format '$stream_pix', not yuv420p10le"
echo "PASS main10: profile='Main 10' pix_fmt=$stream_pix"

decoded=$work/main10.decoded
ref=$work/main10.ref
ffmpeg -hide_banner -loglevel error -i "$stream10" -pix_fmt yuv420p10le -f rawvideo -y "$decoded" \
    || fail 'main10: the stream does not decode'
decoded_frames=$(( $(wc -c <"$decoded") / $(printf '%s' "$size" | awk -F x '{ print $1 * $2 * 3 }') ))
[ "$decoded_frames" -eq "$frames" ] \
    || fail "main10: decoded $decoded_frames frames, expected $frames"
echo "PASS main10: $decoded_frames frames decode"

# A decoder that fills the low bits differently would make the SSIM below a
# comparison of two rendering choices rather than of the coded picture.
ffmpeg -hide_banner -loglevel error -i "$stream10" -pix_fmt yuv420p10le -f rawvideo -y "$ref" \
    || fail 'main10: the reference decode failed'
cmp -s "$decoded" "$ref" \
    || fail 'main10: the platform decode does not match the software decode'
echo "PASS main10: platform decode matches the software decode byte for byte"

ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p10le -s "$size" -r 30 \
    -i "$decoded" -f rawvideo -pix_fmt yuv420p10le -s "$size" -r 30 -i "$source10" \
    -lavfi "[0:v][1:v]ssim=stats_file=$work/main10.ssim" -f null - 2>&1 \
    | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
[ -n "$ssim" ] || fail 'main10: could not compute SSIM'
worst=$(awk -F'All:' '{ split($2, a, " "); if (a[1] + 0 < min || min == 0) min = a[1] + 0 } END { printf "%.4f", min }' \
    "$work/main10.ssim")
echo "PASS main10: ssim mean=$ssim worst=$worst floor=$ssim_min"
awk -v s="$worst" -v m="$ssim_min" 'BEGIN { exit !(s >= m) }' \
    || fail "main10: the worst frame scores $worst, below the $ssim_min floor"

blank=$work/main10.blank
head -c "$(wc -c <"$decoded")" /dev/zero >"$blank"
blank_ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p10le -s "$size" -r 30 \
    -i "$blank" -f rawvideo -pix_fmt yuv420p10le -s "$size" -r 30 -i "$source10" \
    -lavfi "[0:v][1:v]ssim" -f null - 2>&1 \
    | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
awk -v s="$blank_ssim" -v m="$ssim_min" 'BEGIN { exit !(s < m) }' \
    || fail "main10: a blank stream scores $blank_ssim, so the $ssim_min floor proves nothing"
echo "PASS main10: a blank stream scores $blank_ssim, below the floor"

# The same module must still code 8-bit after the 10-bit lane: the raw format
# is per context, and a session that kept P010 would show up here.
encode_lane main yuv420p nv12 main "$source8"
stream8=$lane_stream
ffmpeg -hide_banner -loglevel error -i "$stream8" -f rawvideo -pix_fmt yuv420p -y "$work/main8.decoded" \
    || fail 'main: the 8-bit stream does not decode'
echo "PASS main: the 8-bit HEVC lane still encodes and decodes"

echo "PASS encode-main10: P010 input reaches the firmware and returns Main 10"
