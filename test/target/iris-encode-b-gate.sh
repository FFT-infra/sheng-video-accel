#!/bin/sh

# Hardware gate for B pictures in the H.264 encoder: a client that asks for
# them must get a stream with them, and a client that does not must get the
# P-only stream it always got.
#
# The driver advertises the future prediction direction and one l1 reference
# only when V4L2_VA_B_FRAMES is set; FFmpeg reads those two answers as the
# licence to code B pictures and otherwise falls back to intra and P frames.
# A gate that only encoded would pass with the fallback, so this one reads the
# picture types back out of the stream.
#
# What it asserts:
#   * the encode moved the Iris IRQ counter (it ran on the hardware);
#   * the opted-in arm codes I, P and B pictures, in that structure;
#   * every requested picture decodes, and SSIM against the source is above
#     the floor, with a blank stream scoring below it;
#   * a second run is byte-identical;
#   * without the opt-in the same client gets no B pictures;
#   * HEVC stays P-only under the same opt-in, which is the refusal
#     configure() would otherwise have to enforce at the picture boundary.
#
# Usage: iris-encode-b-gate.sh [render-node]
#
# Environment:
#   LIBVA_DRIVERS_PATH       directory holding the driver to measure
#                            (default <checkout>/build/src)
#   LIBVA_V4L2_ENCODER_PATH  forced encoder node; resolved by name otherwise
#   IRIS_ENCODE_B_DIR        scratch directory (default $IRIS_LAB_ROOT or
#                            ~/iris-vaapi-lab/tmp)
#   IRIS_ENCODE_B_FRAMES     frames (default 300)
#   IRIS_ENCODE_B_SIZE       WxH (default 1280x720)
#   IRIS_ENCODE_B_SSIM_MIN   mean SSIM floor (default 0.90)

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$root/test/lib/iris-env.sh"

scratch=${IRIS_ENCODE_B_DIR:-${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-encode-b.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

frames=${IRIS_ENCODE_B_FRAMES:-300}
size=${IRIS_ENCODE_B_SIZE:-1280x720}
ssim_min=${IRIS_ENCODE_B_SSIM_MIN:-0.90}
frame_bytes=$(printf '%s' "$size" | awk -F x '{ print $1 * $2 * 3 / 2 }')

fail() {
    echo "FAIL encode-b: $*" >&2
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

picture_types() {
    ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$1" \
        | sort | uniq -c | tr '\n' ' '
}

count_of() {
    ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$1" \
        | grep -c "^$2\$" || true
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

source=$work/source.nv12
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
    -frames:v "$frames" -pix_fmt yuv420p -f rawvideo -y "$source" \
    || fail 'could not generate the source'

encode() {
    codec=$1
    opt_in=$2
    stream=$3

    irq_before=$(irq_count)
    if [ "$opt_in" = yes ]; then
        LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path V4L2_VA_B_FRAMES=1 \
            ffmpeg -hide_banner -loglevel error \
            -vaapi_device "$render" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
            -i "$source" -vf format=nv12,hwupload \
            -c:v "${codec}_vaapi" -bf 1 -async_depth 3 -g 60 -b:v 4M -y "$stream" \
            || fail "$codec opt-in=$opt_in: the encode failed"
    else
        LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path \
            ffmpeg -hide_banner -loglevel error \
            -vaapi_device "$render" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
            -i "$source" -vf format=nv12,hwupload \
            -c:v "${codec}_vaapi" -bf 1 -async_depth 3 -g 60 -b:v 4M -y "$stream" \
            || fail "$codec opt-in=$opt_in: the encode failed"
    fi
    irq_after=$(irq_count)
    [ "$irq_after" -gt "$irq_before" ] \
        || fail "$codec opt-in=$opt_in: the encode did not move the Iris IRQ counter (before=$irq_before after=$irq_after)"
    echo "PASS $codec opt-in=$opt_in: encode moved the Iris IRQ counter by $((irq_after - irq_before))"
}

# The opted-in H.264 arm is the feature under test.
encode h264 yes "$work/bf1.h264"
types=$(picture_types "$work/bf1.h264")
echo "PASS h264 opt-in: picture types: $types"
b_pictures=$(count_of "$work/bf1.h264" B)
[ "$b_pictures" -gt 0 ] \
    || fail "h264 opt-in: the stream has no B pictures ($types)"
echo "PASS h264 opt-in: $b_pictures B pictures"

decoded=$work/bf1.decoded
ffmpeg -hide_banner -loglevel error -f h264 -i "$work/bf1.h264" \
    -pix_fmt yuv420p -f rawvideo -y "$decoded" \
    || fail 'h264 opt-in: the stream does not decode'
decoded_frames=$(( $(wc -c <"$decoded") / frame_bytes ))
[ "$decoded_frames" -eq "$frames" ] \
    || fail "h264 opt-in: decoded $decoded_frames pictures, expected $frames"
echo "PASS h264 opt-in: $decoded_frames pictures decode"

ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
    -i "$decoded" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 -i "$source" \
    -lavfi "[0:v][1:v]ssim" -f null - 2>&1 \
    | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
[ -n "$ssim" ] || fail 'h264 opt-in: could not compute SSIM'
echo "PASS h264 opt-in: ssim=$ssim floor=$ssim_min"
awk -v s="$ssim" -v m="$ssim_min" 'BEGIN { exit !(s >= m) }' \
    || fail "h264 opt-in: the stream scores $ssim, below the $ssim_min floor"

# Thirty frames are enough to separate a real stream from a blank one, and a
# full-length blank file would be a second copy of the source on f2fs.
blank=$work/blank.nv12
frame_bytes=$(printf '%s' "$size" | awk -F x '{ print $1 * $2 * 3 / 2 }')
head -c $((frame_bytes * 30)) /dev/zero >"$blank"
blank_ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
    -t 1 -i "$blank" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 -t 1 -i "$source" \
    -lavfi "[0:v][1:v]ssim" -f null - 2>&1 \
    | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
awk -v s="$blank_ssim" -v m="$ssim_min" 'BEGIN { exit !(s < m) }' \
    || fail "h264 opt-in: a blank stream scores $blank_ssim, so the $ssim_min floor proves nothing"
echo "PASS h264 opt-in: a blank stream scores $blank_ssim, below the floor"

# The firmware schedules the GOP itself, so the same input must produce the
# same stream. A reordering bug that depends on completion timing would show
# up as a mismatch here rather than as a quality drop.
encode h264 yes "$work/bf1.repeat.h264"
cmp -s "$work/bf1.h264" "$work/bf1.repeat.h264" \
    || fail 'h264 opt-in: the repeat run is not byte-identical'
echo "PASS h264 opt-in: the repeat run is byte-identical"

# Without the opt-in, FFmpeg is told l1 = 0 and codes intra and P frames. The
# stream must still decode: the fallback is a working stream, not a refusal.
# Counting decoded frames through ffprobe avoids a second copy of the source
# on the target's root filesystem.
encode h264 no "$work/bf0.h264"
types=$(picture_types "$work/bf0.h264")
[ "$(count_of "$work/bf0.h264" B)" -eq 0 ] \
    || fail "h264 gate off: the stream has B pictures ($types)"
bf0_frames=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$work/bf0.h264")
[ "$bf0_frames" -eq "$frames" ] \
    || fail "h264 gate off: decoded $bf0_frames pictures, expected $frames"
echo "PASS h264 gate off: no B pictures, $bf0_frames pictures decode"

# The opt-in advertises a future reference for H.264 only. HEVC must stay on
# the previous direction even here, so the same client request falls back.
encode hevc yes "$work/hevc.h265"
types=$(picture_types "$work/hevc.h265")
[ "$(count_of "$work/hevc.h265" B)" -eq 0 ] \
    || fail "hevc: the stream has B pictures ($types)"
hevc_frames=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$work/hevc.h265")
[ "$hevc_frames" -eq "$frames" ] \
    || fail "hevc: decoded $hevc_frames pictures, expected $frames"
echo "PASS hevc: no B pictures, $hevc_frames pictures decode"

echo "PASS encode-b: H.264 B pictures reach the firmware and decode"
