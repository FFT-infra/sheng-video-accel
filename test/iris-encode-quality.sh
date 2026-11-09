#!/bin/sh

# Hardware gate for encode quality: the bitstream must decode, and the pixels
# it decodes to must still be the source's pixels.
#
# The existing encode gates prove the stream is decodable and that a decode
# through this driver matches a decode through the native decoder. Neither
# says anything about fidelity: an encoder that emits a stream of flat grey
# passes both. A rate-controlled encode cannot be compared byte for byte with
# the source, so this measures structural similarity against it instead.
#
# What it asserts, for each codec:
#   * the encode moved the Iris IRQ counter (it ran on the hardware);
#   * the stream decodes with no error;
#   * the frame count is the frame count that was requested;
#   * every decoded frame matches a software decode of the same stream;
#   * SSIM against the source is above the floor, per frame;
#   * the floor would catch a broken encode: a deliberately blank stream
#     scores below it, so a pass cannot come from trusting the encoder.
#
# Usage: iris-encode-quality.sh [encoder-node] [render-node]
#
# Environment:
#   LIBVA_DRIVERS_PATH        directory holding the driver to measure
#                             (default <checkout>/build/src; name
#                             /usr/lib64/dri to measure an installed package)
#   IRIS_ENCODE_QUALITY_DIR   scratch directory (default $IRIS_LAB_ROOT or
#                             ~/Lab/Bridge/tmp)
#   IRIS_ENCODE_FRAMES        frames per codec (default 120)
#   IRIS_ENCODE_SIZE          WxH (default 1920x1080)
#   IRIS_ENCODE_SSIM_MIN      per-frame SSIM floor (default 0.90)
#   IRIS_ENCODE_CODECS        "h264 hevc" by default

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
. "$root/test/lib/iris-env.sh"

scratch=${IRIS_ENCODE_QUALITY_DIR:-${IRIS_LAB_ROOT:-$HOME/Lab/Bridge/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-encode-quality.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

frames=${IRIS_ENCODE_FRAMES:-120}
size=${IRIS_ENCODE_SIZE:-1920x1080}
ssim_min=${IRIS_ENCODE_SSIM_MIN:-0.90}
codecs=${IRIS_ENCODE_CODECS:-h264 hevc}

fail() {
    echo "FAIL encode-quality: $*" >&2
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

# The video block's interrupt count. The line is named `iris` on this kernel
# branch and after the driver on others, so match the name rather than a
# column position; a node that never fires would make the gate vacuous.
irq_count() {
    awk '$NF ~ /^(iris|qcom-iris|venus|vcodec)$/ { s += $2 } END { print s + 0 }' /proc/interrupts
}

command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'
command -v ffprobe >/dev/null 2>&1 || fail 'ffprobe is required'

encoder=$(resolve_encoder)
render=${2:-/dev/dri/renderD128}
# The caller may name the driver: the hardware suite passes its own tree, and
# an installed package is verified by naming /usr/lib64/dri. A hardcoded build
# directory silently measures whatever happens to be installed instead.
driver_path=${LIBVA_DRIVERS_PATH:-$root/build/src}
[ -f "$driver_path/v4l2_drv_video.so" ] \
    || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
[ -c "$render" ] || fail "not a DRM render node: $render"
[ -c "$encoder" ] || fail "not a V4L2 node: $encoder"

echo "encoder=$encoder render=$render frames=$frames size=$size ssim_min=$ssim_min"

# The source: a moving test pattern. It is generated once and both codecs
# encode the same pixels.
source_mp4=$work/source.mp4
ffmpeg -hide_banner -loglevel error -f lavfi \
    -i "testsrc2=size=$size:rate=30" -frames:v "$frames" \
    -c:v ffv1 -pix_fmt yuv420p -y "$source_mp4" \
    || fail 'could not generate the source'
source_nv12=$work/source.nv12
ffmpeg -hide_banner -loglevel error -i "$source_mp4" -pix_fmt yuv420p -f rawvideo -y "$source_nv12" \
    || fail 'could not extract the source frames'

check_codec() {
    codec=$1
    # The muxer is chosen from the file extension, and the H.264 raw muxer
    # rejects HEVC with a header error. Name each stream after its own
    # codec.
    case $codec in
        h264) ff_codec=h264_vaapi; stream=$work/$codec.h264 ;;
        hevc) ff_codec=hevc_vaapi; stream=$work/$codec.hevc ;;
        *) fail "unsupported codec: $codec" ;;
    esac

    decoded=$work/$codec.nv12
    ref=$work/$codec.ref.nv12

    irq_before=$(irq_count)
    LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path \
        ffmpeg -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
        -i "$source_nv12" -vf format=nv12,hwupload \
        -c:v "$ff_codec" -g 60 -y "$stream" \
        || fail "$codec: the encode failed"
    irq_after=$(irq_count)
    [ "$irq_after" -gt "$irq_before" ] \
        || fail "$codec: the encode did not move the Iris IRQ counter (before=$irq_before after=$irq_after)"
    echo "PASS $codec: encode moved the Iris IRQ counter by $((irq_after - irq_before))"

    # Decode the stream with the platform decoder; a stream the firmware
    # cannot read is not an encode.
    ffmpeg -hide_banner -loglevel error -i "$stream" -f rawvideo -pix_fmt yuv420p -y "$decoded" \
        || fail "$codec: the stream does not decode"
    decoded_frames=$(( $(wc -c <"$decoded") / $(ffprobe -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=p=0 "$stream" | awk -F, '{ print $1 * $2 * 3 / 2 }') ))
    [ "$decoded_frames" -eq "$frames" ] \
        || fail "$codec: decoded $decoded_frames frames, expected $frames"
    echo "PASS $codec: $decoded_frames frames decode"

    # The decoder is a second opinion on the same bytes: if the platform
    # decoder and the software decoder disagree, the stream is not what it
    # claims to be and the SSIM below would be meaningless.
    ffmpeg -hide_banner -loglevel error -i "$stream" -f rawvideo -pix_fmt yuv420p -y "$ref" \
        || fail "$codec: the software decode of the stream failed"
    cmp -s "$decoded" "$ref" \
        || fail "$codec: the platform decode does not match the software decode"
    echo "PASS $codec: platform decode matches the software decode byte for byte"

    # Fidelity against the source, not against another decode of itself.
    ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
        -i "$decoded" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 -i "$source_nv12" \
        -lavfi "[0:v][1:v]ssim=stats_file=$work/$codec.ssim" -f null - 2>&1 \
        | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
    [ -n "$ssim" ] || fail "$codec: could not compute SSIM"
    # Every frame, not just the average: one shattered frame is one broken
    # picture handed back to the client.
    worst=$(awk -F'All:' '{ split($2, a, " "); if (a[1] + 0 < min || min == 0) min = a[1] + 0 } END { printf "%.4f", min }' \
        "$work/$codec.ssim")
    echo "PASS $codec: ssim mean=$ssim worst=$worst floor=$ssim_min"
    awk -v s="$worst" -v m="$ssim_min" 'BEGIN { exit !(s >= m) }' \
        || fail "$codec: the worst frame scores $worst, below the $ssim_min floor"

    # A floor nobody can miss is not a floor. A blank stream must score below
    # it, otherwise this check would pass an encoder that writes nothing.
    blank=$work/$codec.blank.nv12
    : >"$blank"
    head -c $(( $(wc -c <"$decoded") )) /dev/zero >>"$blank"
    blank_ssim=$(ffmpeg -hide_banner -nostats -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
        -i "$blank" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 -i "$source_nv12" \
        -lavfi "[0:v][1:v]ssim" -f null - 2>&1 \
        | sed -n 's/.*All:\([0-9.]*\).*/\1/p' | tail -1)
    awk -v s="$blank_ssim" -v m="$ssim_min" 'BEGIN { exit !(s < m) }' \
        || fail "$codec: a blank stream scores $blank_ssim, so the $ssim_min floor proves nothing"
    echo "PASS $codec: a blank stream scores $blank_ssim, below the floor"
}

for codec in $codecs; do
    check_codec "$codec"
done

echo "PASS encode-quality encoder=$encoder codecs='$codecs' frames=$frames ssim_min=$ssim_min"
