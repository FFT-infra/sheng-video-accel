#!/bin/sh

# Hardware gate for the encoder's raw packed header path: an HDR10 source
# carries mastering-display and content-light-level side data, FFmpeg's
# hevc_vaapi turns them into a prefix SEI and hands it to the driver as a
# VAEncPackedHeaderRawData buffer, and the driver splices that into the access
# unit. The stream that comes back must still carry both SEI messages.
#
# The encoder node writes the parameter sets itself and has no SEI input of
# its own, so this is the only path by which metadata enters the stream. A
# driver that drops the packed header still produces a decodable Main 10
# stream, which is why the check reads the SEI back rather than only the
# picture.
#
# What it asserts:
#   * the encode moved the Iris IRQ counter (it ran on the hardware);
#   * the output decodes to the requested frame count;
#   * ffprobe reports mastering display and content light level frame side
#     data on the output's frames;
#   * the same module still codes an 8-bit stream without the metadata.
#
# Usage: iris-encode-hdr-sei-gate.sh [render-node]
#
# Environment:
#   LIBVA_DRIVERS_PATH      directory holding the driver to measure
#                           (default <checkout>/build/src)
#   LIBVA_V4L2_ENCODER_PATH forced encoder node; resolved by name otherwise
#   IRIS_ENCODE_HDR_DIR     scratch directory (default $IRIS_LAB_ROOT or
#                           ~/iris-vaapi-lab/tmp)
#   IRIS_ENCODE_FRAMES      frames (default 60)
#   IRIS_ENCODE_SIZE        WxH (default 640x480)

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$root/test/lib/iris-env.sh"

scratch=${IRIS_ENCODE_HDR_DIR:-${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-encode-hdr-sei.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

frames=${IRIS_ENCODE_FRAMES:-60}
size=${IRIS_ENCODE_SIZE:-640x480}

fail() {
    echo "FAIL encode-hdr-sei: $*" >&2
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
        driver=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Driver name[[:space:]]*: *//p' | sed -n '1p')
        [ "$driver" = iris_driver ] || continue
        card=$(printf '%s\n' "$info" | sed -n 's/^[[:space:]]*Card type[[:space:]]*: *//p' | sed -n '1p')
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

echo "encoder=$encoder render=$render frames=$frames size=$size"

# An HDR10 source whose frames carry the two side-data blocks. libx265 writes
# both as SEI, and the software HEVC decoder turns them back into frame side
# data, which is what hevc_vaapi reads.
hdr_params='master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400'
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
    -frames:v "$frames" -pix_fmt yuv420p10le \
    -c:v libx265 -profile:v main10 -x265-params "$hdr_params" \
    -f hevc -y "$work/source.hevc" \
    || fail 'could not generate the HDR10 source'

# The source has to carry the metadata before the encode can be expected to
# pass it on: a test whose input is already bare would pass on any driver.
source_side=$(ffprobe -v error -select_streams v:0 -show_frames -read_intervals '%+#1' \
    -show_entries frame=side_data_list -of default=nw=1 "$work/source.hevc")
printf '%s\n' "$source_side" | grep -q 'Mastering display metadata' \
    || fail 'the source has no mastering display side data'
printf '%s\n' "$source_side" | grep -q 'Content light level metadata' \
    || fail 'the source has no content light level side data'
echo "PASS source: mastering display and content light level side data present"

# hevc_vaapi's default -sei covers both messages; the input is uploaded to a
# P010 surface for Main 10.
irq_before=$(irq_count)
LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path \
    ffmpeg -hide_banner -loglevel error \
    -vaapi_device "$render" -i "$work/source.hevc" \
    -vf 'format=p010,hwupload' -c:v hevc_vaapi -profile:v main10 -sei hdr \
    -y "$work/out.hevc" \
    || fail 'the VAAPI encode failed'
irq_after=$(irq_count)
[ "$irq_after" -gt "$irq_before" ] \
    || fail "the encode did not move the Iris IRQ counter (before=$irq_before after=$irq_after)"
echo "PASS encode: moved the Iris IRQ counter by $((irq_after - irq_before))"

decoded_frames=$(ffmpeg -hide_banner -loglevel error -i "$work/out.hevc" -f null - \
    -progress pipe:1 2>/dev/null | sed -n 's/^frame=//p' | tail -1)
[ "${decoded_frames:-0}" -eq "$frames" ] \
    || fail "the output decoded $decoded_frames frames, expected $frames"
echo "PASS output: $decoded_frames frames decode"

# The metadata is in the coded stream only if the driver spliced the client's
# raw packed header in. ffprobe reports it as frame side data on the frames
# that carry the prefix SEI.
out_side=$(ffprobe -v error -select_streams v:0 -show_frames -read_intervals '%+#1' \
    -show_entries frame=side_data_list -of default=nw=1 "$work/out.hevc")
printf '%s\n' "$out_side" | grep -q 'Mastering display metadata' \
    || fail 'the output has no mastering display side data: the packed header did not reach the stream'
printf '%s\n' "$out_side" | grep -q 'Content light level metadata' \
    || fail 'the output has no content light level side data: the packed header did not reach the stream'
echo "PASS output: mastering display and content light level SEI are in the stream"

# The raw format is per context and the packed header is per picture; the
# 8-bit lane must still encode and decode without the metadata.
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
    -frames:v "$frames" -pix_fmt nv12 -f rawvideo -y "$work/source.nv12" \
    || fail 'could not generate the 8-bit source'
LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path \
    ffmpeg -hide_banner -loglevel error \
    -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s "$size" -r 30 \
    -i "$work/source.nv12" -vf 'format=nv12,hwupload' -c:v hevc_vaapi -profile:v main \
    -y "$work/out8.hevc" \
    || fail 'the 8-bit lane failed'
ffmpeg -hide_banner -loglevel error -i "$work/out8.hevc" -f null - \
    || fail 'the 8-bit output does not decode'
echo "PASS 8-bit lane: still encodes and decodes"

echo "PASS encode-hdr-sei: raw packed headers reach the stream"
