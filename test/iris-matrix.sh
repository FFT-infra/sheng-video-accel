#!/bin/sh

# Repeatable Qualcomm Iris codec matrix. Run on the target tablet (or another
# host with the Iris V4L2 node and a working VA-API stack). The decoder node is
# resolved by driver name and artifacts land in a durable lab directory; see
# test/lib/iris-env.sh.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_MATRIX_DIR:-$(iris_artifact_dir va-matrix)}
frames=${IRIS_MATRIX_FRAMES:-48}
mkdir -p "$root"

va_env="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path LIBVA_V4L2_VIDEO_PATH=$device V4L2_VA_SYNC_TIMEOUT_MS=${V4L2_VA_SYNC_TIMEOUT_MS:-1000}"

if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
    echo "FAIL ffmpeg/ffprobe is required" >&2
    exit 2
fi

formats=$(v4l2-ctl --list-formats-out-ext -d "$device" 2>/dev/null || true)
has_format() { printf '%s\n' "$formats" | grep -q "'$1'"; }

echo "Iris matrix device=$device frames=$frames root=$root"
echo "$formats" | sed -n '/\[[0-9][0-9]*\]:/p'

ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=24" \
    -frames:v "$frames" -c:v libx264 -preset ultrafast -tune zerolatency \
    -x264-params "keyint=24:min-keyint=24:scenecut=0" -an "$root/test-h264.mp4"
ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=24" \
    -frames:v "$frames" -c:v libvpx-vp9 -b:v 300k -an "$root/test-vp9.webm"
ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=24" \
    -frames:v "$frames" -c:v libx265 -preset ultrafast \
    -x265-params "keyint=24:min-keyint=24:scenecut=0" -an "$root/test-hevc.mkv"
ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=24" \
    -frames:v "$frames" -c:v libaom-av1 -cpu-used 8 -crf 40 -an \
    "$root/test-av1.webm"

run_va() {
    codec=$1
    input=$2
    extra=${3:-}
    eos_mode=${4:-strict}
    trace="$root/$codec-vaapi.trace"
    md5="$root/$codec-vaapi.md5"
    sw="$root/$codec-software.md5"
    ffmpeg -y -hide_banner -loglevel error -i "$input" -pix_fmt yuv420p -f framemd5 "$sw"
    # shellcheck disable=SC2086
    env $va_env $extra V4L2_VA_TRACE=1 ffmpeg -y -hide_banner -loglevel error \
        -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$input" -vf "hwdownload,format=nv12,format=yuv420p" -f framemd5 "$md5" \
        2>"$trace"
    if [ "$eos_mode" = allow-bounded-sync ]; then
        test/iris-eos-check.sh "$trace" "$frames" --allow-bounded-sync
    elif [ "$eos_mode" = hidden-pictures ]; then
        test/iris-eos-check.sh "$trace" "$frames" --hidden-pictures
    else
        test/iris-eos-check.sh "$trace" "$frames"
    fi
    echo "$codec frames=$(grep -c '^[0-9]' "$md5")"
    if diff -q "$sw" "$md5" >/dev/null 2>&1; then
        echo "PASS $codec content=frame-md5"
    else
        echo "FAIL $codec content=frame-md5-mismatch (see $md5)" >&2
        exit 1
    fi
}

run_native() {
    codec=$1
    demux=$2
    parser=$3
    decoder=$4
    input=$5
    log="$root/$codec-native.log"
    if ! command -v gst-launch-1.0 >/dev/null 2>&1 || ! gst-inspect-1.0 "$decoder" >/dev/null 2>&1; then
        echo "SKIP $codec native-gstreamer-unavailable"
        return
    fi
    gst-launch-1.0 -e filesrc location="$root/$input" ! "$demux" ! queue ! "$parser" ! \
        "$decoder" capture-io-mode=2 output-io-mode=2 ! fakesink sync=false >"$log" 2>&1
    grep -q 'Got EOS' "$log"
    echo "PASS $codec baseline=native-$decoder"
}

if has_format H264; then
    run_native h264 qtdemux h264parse v4l2h264dec test-h264.mp4
    run_va h264 "$root/test-h264.mp4"
else
    echo "SKIP h264 device-format-unavailable"
fi

if has_format VP90; then
    run_native vp9 matroskademux vp9parse v4l2vp9dec test-vp9.webm
    run_va vp9 "$root/test-vp9.webm"
else
    echo "SKIP vp9 device-format-unavailable"
fi

if has_format HEVC; then
    # HEVC timeout recovery drains trailing reorder pictures and emits one
    # terminal LAST marker. Keep this strict so regressions fail the matrix.
    run_va hevc "$root/test-hevc.mkv"
    # Run the native baseline after the VA session. Some Iris firmware builds
    # leave a reorder queue warm after GStreamer closes it, which can force a
    # VA caller's first sync to wait for future AUs that it cannot submit yet.
    run_native hevc matroskademux h265parse v4l2h265dec test-hevc.mkv
else
    echo "SKIP hevc device-format-unavailable"
fi

if has_format AV01; then
    if command -v gst-launch-1.0 >/dev/null 2>&1 && gst-inspect-1.0 v4l2av1dec >/dev/null 2>&1; then
        gst-launch-1.0 -e filesrc location="$root/test-av1.webm" ! matroskademux ! queue ! \
            av1parse ! v4l2av1dec capture-io-mode=2 output-io-mode=2 ! fakesink sync=false \
            >"$root/av1-native.log" 2>&1
        grep -q 'Got EOS' "$root/av1-native.log"
        echo "PASS av1 baseline=native-v4l2av1dec"
    else
        echo "SKIP av1 baseline-gstreamer-unavailable"
    fi
    # The stream carries alt-ref pictures, which the driver submits and
    # publishes but which the client displays through show_existing_frame, so
    # the trace has more pictures than output frames.
    run_va av1 "$root/test-av1.webm" "" hidden-pictures
else
    echo "SKIP av1 device-format-unavailable"
fi

if ! has_format MPEG2 && ! has_format VP80; then
    echo "SKIP mpeg2/vp8 device-formats-unavailable"
fi
