#!/bin/sh

# Generate short H.264 streams with different reference structures and compare
# VA-API output with an FFmpeg software framemd5 oracle. Run on the target
# tablet (or another host with the Iris V4L2 node and this driver).

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/../lib/iris-env.sh"

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_STRUCTURE_DIR:-$(iris_artifact_dir va-structure-matrix)}
frames=${IRIS_STRUCTURE_FRAMES:-48}
timeout_seconds=${IRIS_STRUCTURE_TIMEOUT_SECONDS:-180}
size=${IRIS_STRUCTURE_SIZE:-640x360}

mkdir -p "$root"

if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
    echo "FAIL ffmpeg/ffprobe is required" >&2
    exit 2
fi
if ! command -v timeout >/dev/null 2>&1; then
    echo "FAIL timeout(1) is required" >&2
    exit 2
fi

echo "Iris structure matrix device=$device size=$size frames=$frames root=$root"

make_h264() {
    name=$1
    gop=$2
    bframes=$3
    preset=$4
    ffmpeg -y -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=$size:rate=24" -frames:v "$frames" \
        -c:v libx264 -preset "$preset" -g "$gop" -bf "$bframes" \
        -x264-params "scenecut=0:keyint=$gop:min-keyint=$gop" -an \
        "$root/$name.mp4"
}

make_h264 all-i 1 0 ultrafast
make_h264 ip-g12 12 0 ultrafast
make_h264 b2-g48 48 2 medium
make_h264 b4-g48 48 4 medium

run_sample() {
    name=$1
    input="$root/$name.mp4"
    software="$root/$name.software.md5"
    hardware="$root/$name.vaapi.md5"
    trace="$root/$name.trace"

    expected=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "$input")
    profile=$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=profile,has_b_frames,r_frame_rate \
        -of default=noprint_wrappers=1 "$input" | tr '\n' ' ')
    ffmpeg -y -hide_banner -loglevel error -i "$input" -pix_fmt yuv420p \
        -f framemd5 "$software"

    set +e
    timeout "${timeout_seconds}s" env \
        LIBVA_DRIVER_NAME=v4l2 \
        LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" \
        V4L2_VA_TRACE=1 \
        V4L2_VA_SYNC_TIMEOUT_MS="${V4L2_VA_SYNC_TIMEOUT_MS:-2000}" \
        ffmpeg -y -hide_banner -loglevel error \
        -vaapi_device /dev/dri/renderD128 \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
        -vf "hwdownload,format=nv12,format=yuv420p" -f framemd5 "$hardware" \
        2>"$trace"
    rc=$?
    set -e

    actual=$(grep -c '^[0-9]' "$hardware" 2>/dev/null || true)
    misses=$(grep -c 'publish=drop' "$trace" 2>/dev/null || true)
    sync_timeouts=$(grep -c 'sync timeout token=' "$trace" 2>/dev/null || true)
    capture_errors=$(grep -Ec 'publish=error|capture error index=' "$trace" 2>/dev/null || true)
    if [ "$rc" -eq 124 ]; then
        echo "FAIL $name timeout expected=$expected actual=$actual profile=$profile" >&2
        return 1
    fi
    if [ "$rc" -ne 0 ] || [ "$actual" -ne "$expected" ]; then
        echo "FAIL $name rc=$rc expected=$expected actual=$actual profile=$profile" >&2
        return 1
    fi
    if [ "$misses" -ne 0 ] || [ "$sync_timeouts" -ne 0 ] || [ "$capture_errors" -ne 0 ]; then
        echo "FAIL $name misses=$misses sync_timeouts=$sync_timeouts capture_errors=$capture_errors" >&2
        return 1
    fi
    if ! diff -q "$software" "$hardware" >/dev/null 2>&1; then
        echo "FAIL $name frame-md5-mismatch expected=$expected actual=$actual profile=$profile" >&2
        return 1
    fi
    echo "PASS $name frames=$actual profile=$profile"
}

run_sample all-i
run_sample ip-g12
run_sample b2-g48
run_sample b4-g48
echo "PASS iris-structure-matrix"
