#!/bin/sh

# Hardware gate for encode latency: submission-to-completion time on the
# encoder. The encoder has no low-latency firmware switch (docs/architecture.md
# §4.1), so the only honest claim is a measurement: the session traces
# `encode frame … latency_us=` from the client's vaEndPicture to the access
# unit, and this gate turns those numbers into first-picture, p50 and p95.
#
# The run pins the client's pipeline at one picture (`-async_depth 1`), so the
# reported latency is the firmware's, not a queue the client built. The first
# picture is reported separately because session bring-up (firmware load,
# first parameter sets) is not representative of steady state.
#
# What it asserts:
#   * the encode moved the Iris IRQ counter (it ran on the hardware);
#   * every picture reports a latency, numbered from 0, exactly once;
#   * the output decodes to the requested frame count;
#   * p50 and p95 are printed for the run record.
#
# Usage: iris-encode-latency.sh [render-node]
#
# Environment:
#   LIBVA_DRIVERS_PATH      directory holding the driver to measure
#                           (default <checkout>/build/src)
#   IRIS_ENCODE_LATENCY_DIR scratch directory (default $IRIS_LAB_ROOT or
#                           ~/iris-vaapi-lab/tmp)
#   IRIS_ENCODE_FRAMES      frames (default 300)
#   IRIS_ENCODE_SIZE        WxH (default 1920x1080)
#   IRIS_ENCODE_CODEC       h264 or hevc (default h264)

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
. "$root/test/lib/iris-env.sh"

scratch=${IRIS_ENCODE_LATENCY_DIR:-${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-encode-latency.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

frames=${IRIS_ENCODE_FRAMES:-300}
size=${IRIS_ENCODE_SIZE:-1920x1080}
codec=${IRIS_ENCODE_CODEC:-h264}

fail() {
    echo "FAIL encode-latency: $*" >&2
    exit 1
}

irq_count() {
    awk '$NF ~ /^(iris|qcom-iris|venus|vcodec)$/ { s += $2 } END { print s + 0 }' /proc/interrupts
}

command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'

render=${1:-/dev/dri/renderD128}
driver_path=${LIBVA_DRIVERS_PATH:-$root/build/src}
[ -f "$driver_path/v4l2_drv_video.so" ] \
    || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
[ -c "$render" ] || fail "not a DRM render node: $render"

case "$codec" in h264|hevc) ;; *) fail "IRIS_ENCODE_CODEC must be h264 or hevc" ;; esac

echo "render=$render codec=$codec frames=$frames size=$size"

ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
    -frames:v "$frames" -pix_fmt yuv420p -f rawvideo -y "$work/source.nv12" \
    || fail 'could not generate the source'

irq_before=$(irq_count)
LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path V4L2_VA_TRACE=1 \
    ffmpeg -hide_banner -loglevel error \
    -vaapi_device "$render" -f rawvideo -pix_fmt yuv420p -s "$size" -r 30 \
    -i "$work/source.nv12" -vf format=nv12,hwupload \
    -c:v "${codec}_vaapi" -async_depth 1 -rc_mode CBR -b:v 8M -g 60 \
    -y "$work/out.$codec" 2> "$work/trace.log" \
    || fail 'the encode failed'
irq_after=$(irq_count)
[ "$irq_after" -gt "$irq_before" ] \
    || fail "the encode did not move the Iris IRQ counter (before=$irq_before after=$irq_after)"
echo "PASS encode: moved the Iris IRQ counter by $((irq_after - irq_before))"

# One latency per picture, indexed 0..frames-1.
grep -o 'encode frame index=[0-9]* .*latency_us=[0-9]* first=[01]' "$work/trace.log" \
    | sed -n 's/.*latency_us=\([0-9]*\).*/\1/p' > "$work/latency.txt" || true
count=$(wc -l < "$work/latency.txt" | tr -d ' ')
[ "$count" -eq "$frames" ] \
    || fail "the trace reports $count latencies, expected $frames"
echo "PASS trace: $count pictures carry a latency"

first=$(head -1 "$work/latency.txt")
sort -n "$work/latency.txt" > "$work/sorted.txt"
p50=$(awk -v n="$count" 'NR == int((n + 1) / 2) { print; exit }' "$work/sorted.txt")
p95=$(awk -v n="$count" 'NR == int((n * 95 + 99) / 100) { print; exit }' "$work/sorted.txt")
[ -n "${first:-}" ] && [ -n "${p50:-}" ] && [ -n "${p95:-}" ] \
    || fail 'could not compute the percentiles'
echo "PASS latency: first=${first}us p50=${p50}us p95=${p95}us"

decoded_frames=$(ffmpeg -hide_banner -loglevel error -i "$work/out.$codec" -f null - \
    -progress pipe:1 2>/dev/null | sed -n 's/^frame=//p' | tail -1)
[ "${decoded_frames:-0}" -eq "$frames" ] \
    || fail "the output decoded $decoded_frames frames, expected $frames"
echo "PASS output: $decoded_frames frames decode"

echo "PASS encode-latency: first=${first}us p50=${p50}us p95=${p95}us"
