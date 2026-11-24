#!/bin/sh

# Zero-copy qualification: a client that exports surfaces only after they
# are decoded (FFmpeg, GStreamer, mpv) must receive the CAPTURE slot itself
# (publish=direct) on every frame, with no copy engine involved, and the
# pixels must equal a software decode. docs/architecture.md §3.3.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/../lib/iris-env.sh"

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_ZERO_COPY_DIR:-$(iris_artifact_dir zero-copy-suite)}
frames=${IRIS_ZERO_COPY_FRAMES:-24}
timeout_seconds=${IRIS_ZERO_COPY_TIMEOUT_SECONDS:-90}

mkdir -p "$root"

input="$root/h264.mp4"
software="$root/software.md5"
hardware="$root/hardware.md5"
trace="$root/trace.log"

ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x360:rate=24" -frames:v "$frames" \
    -c:v libx264 -preset ultrafast -an "$input"

timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
    -i "$input" -pix_fmt yuv420p -f framemd5 "$software"

timeout "$timeout_seconds" env \
    LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
    LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
    ffmpeg -y -hide_banner -loglevel error \
    -vaapi_device /dev/dri/renderD128 \
    -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
    -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$hardware" \
    2>"$trace"

captures=$(grep -c 'capture token=' "$trace" || true)
direct=$(grep -c 'capture token=.*publish=direct' "$trace" || true)
copies=$(grep -Ec 'publish=copy-gpu|publish=copy-cpu' "$trace" || true)

if [ "$captures" -eq 0 ]; then
    echo "FAIL zero captures" >&2
    exit 1
fi

if [ "$direct" -ne "$captures" ]; then
    echo "FAIL not all captures were direct (captures=$captures, direct=$direct)" >&2
    exit 1
fi

if [ "$copies" -ne 0 ]; then
    echo "FAIL copies occurred (copies=$copies)" >&2
    exit 1
fi

if ! cmp -s "$software" "$hardware"; then
    echo "FAIL framemd5 differs from software" >&2
    exit 1
fi

echo "PASS zero-copy suite direct=$direct"
