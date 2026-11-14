#!/bin/sh

# Exercise 640x360 <-> 1280x720 sequence changes on the Iris stateful decoder.
# A codec passes only when native V4L2/GStreamer decodes the complete
# changing-resolution stream, one FFmpeg VA process matches software, and
# fresh VA decoder contexts repeatedly decode each geometry.
#
# VP9 was withdrawn here on the old tree: its context recreation and source
# changes rebooted the tablet. Phase 5 re-enabled it on the decode-order
# module, where neither reproduced (100 recreations plus 100 in-stream
# switches, pixels equal to software), and `IRIS_DYNAMIC_CODECS=vp9` has run
# since. Under `V4L2_VA_PUBLISH=import` the resolution change also rebuilds an
# Import pool, which is a different path from that run.
# Missing formats, elements or advertised support are failures, never SKIPs.

set -eu

. "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_DYNAMIC_DIR:-$(iris_artifact_dir dynamic-resolution)}
switches=${IRIS_DYNAMIC_SWITCHES:-100}
frames_per_segment=${IRIS_DYNAMIC_FRAMES_PER_SEGMENT:-2}
timeout_seconds=${IRIS_DYNAMIC_TIMEOUT_SECONDS:-600}
codecs=${IRIS_DYNAMIC_CODECS:-h264}

case "$switches:$frames_per_segment:$timeout_seconds" in
    *[!0-9:]*|0:*|*:0:*|*:0)
        echo "FAIL dynamic numeric settings must be positive integers" >&2
        exit 2
        ;;
esac

for tool in ffmpeg ffprobe gst-launch-1.0 gst-inspect-1.0 v4l2-ctl timeout cmp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL required dynamic-resolution tool is missing: $tool" >&2
        exit 2
    fi
done
if [ ! -e "$device" ]; then
    echo "FAIL Iris decoder device is missing: $device" >&2
    exit 2
fi

mkdir -p "$root"
formats=$(v4l2-ctl --list-formats-out-ext -d "$device" 2>/dev/null) || {
    echo "FAIL cannot inventory compressed formats on $device" >&2
    exit 1
}
segment_count=$((switches + 1))
expected_frames=$((segment_count * frames_per_segment))
va_env="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path LIBVA_V4L2_VIDEO_PATH=$device V4L2_VA_TRACE=1"

require_format() {
    fourcc=$1
    codec=$2
    if ! printf '%s\n' "$formats" | grep -q "'$fourcc'"; then
        echo "FAIL $codec is required but $device does not advertise $fourcc" >&2
        exit 1
    fi
}

require_element() {
    element=$1
    if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
        echo "FAIL required native baseline element is missing: $element" >&2
        exit 1
    fi
}

frame_count() {
    awk -F, '/^[0-9]+,/ { count++ } END { print count + 0 }' "$1"
}

check_frame_count() {
    file=$1
    expected=$2
    label=$3
    actual=$(frame_count "$file")
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL $label frames=$actual expected=$expected" >&2
        exit 1
    fi
}

check_exact_context_count() {
    trace=$1
    label=$2
    expected=$3
    contexts=$(grep -c 'va create_context id=' "$trace" || true)
    if [ "$contexts" -ne "$expected" ]; then
        echo "FAIL $label created $contexts VA contexts; expected=$expected" >&2
        exit 1
    fi
}

check_stream_switches() {
    stream=$1
    codec=$2
    dimensions_raw="$root/$codec-dimensions-raw.txt"
    dimensions="$root/$codec-dimensions.txt"
    timeout "$timeout_seconds" ffprobe -v error -select_streams v:0 \
        -show_entries frame=width,height -of csv=p=0 "$stream" >"$dimensions_raw"
    # Some ffprobe builds append an empty side-data column to keyframes. Only
    # width and height define a geometry transition; retaining the trailing
    # comma makes every keyframe look like a fake resolution change.
    awk -F, 'NF >= 2 { print $1 "," $2 }' "$dimensions_raw" >"$dimensions"
    actual_frames=$(wc -l <"$dimensions" | tr -d ' ')
    actual_switches=$(awk 'NR == 1 { previous=$0; next } $0 != previous { changes++; previous=$0 } END { print changes + 0 }' "$dimensions")
    invalid=$(awk -F, '$0 != "640,360" && $0 != "1280,720" { count++ } END { print count + 0 }' "$dimensions")
    if [ "$actual_frames" -ne "$expected_frames" ] || [ "$actual_switches" -ne "$switches" ] || [ "$invalid" -ne 0 ]; then
        echo "FAIL $codec assembled-stream frames=$actual_frames/$expected_frames switches=$actual_switches/$switches invalid_dimensions=$invalid" >&2
        exit 1
    fi
}

make_concat_stream() {
    codec=$1
    encoder=$2
    extension=$3
    encoder_args=$4
    low="$root/$codec-low.$extension"
    high="$root/$codec-high.$extension"
    list="$root/$codec-concat.txt"
    dynamic="$root/$codec-dynamic.mkv"

    # shellcheck disable=SC2086
    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x360:rate=24" -frames:v "$frames_per_segment" \
        -c:v "$encoder" $encoder_args -an "$low"
    # shellcheck disable=SC2086
    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=1280x720:rate=24" -frames:v "$frames_per_segment" \
        -c:v "$encoder" $encoder_args -an "$high"

    : >"$list"
    index=0
    while [ "$index" -lt "$segment_count" ]; do
        if [ $((index % 2)) -eq 0 ]; then
            printf "file '%s-low.%s'\n" "$codec" "$extension" >>"$list"
        else
            printf "file '%s-high.%s'\n" "$codec" "$extension" >>"$list"
        fi
        index=$((index + 1))
    done
    (cd "$root" && timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
        -f concat -safe 1 -i "$(basename "$list")" -map 0:v:0 -c copy "$(basename "$dynamic")")
    check_stream_switches "$dynamic" "$codec"
}

run_native_baseline() {
    codec=$1
    parser=$2
    decoder=$3
    extension=$4
    stream="$root/$codec-dynamic.mkv"
    low="$root/$codec-low.$extension"
    high="$root/$codec-high.$extension"
    low_raw="$root/$codec-low-reference.yuv"
    high_raw="$root/$codec-high-reference.yuv"
    software_raw="$root/$codec-software.yuv"
    native_raw="$root/$codec-native.yuv"
    native_log="$root/$codec-native.log"

    require_element "$decoder"
    # rawvideo has no per-frame geometry metadata, so FFmpeg locks one output
    # stream to the first frame's size when a decoded stream changes geometry.
    # Decode the two source segments independently and assemble the known
    # alternating reference instead of silently scaling every high segment.
    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error -i "$low" \
        -pix_fmt yuv420p -fps_mode passthrough -f rawvideo "$low_raw"
    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error -i "$high" \
        -pix_fmt yuv420p -fps_mode passthrough -f rawvideo "$high_raw"
    : >"$software_raw"
    index=0
    while [ "$index" -lt "$segment_count" ]; do
        if [ $((index % 2)) -eq 0 ]; then
            cat "$low_raw" >>"$software_raw"
        else
            cat "$high_raw" >>"$software_raw"
        fi
        index=$((index + 1))
    done
    timeout "$timeout_seconds" gst-launch-1.0 -e filesrc location="$stream" ! \
        matroskademux ! "$parser" ! "$decoder" capture-io-mode=2 output-io-mode=2 ! \
        videoconvert ! video/x-raw,format=I420 ! filesink location="$native_raw" \
        >"$native_log" 2>&1
    if ! grep -q 'Got EOS' "$native_log"; then
        echo "FAIL $codec native baseline did not reach EOS" >&2
        exit 1
    fi
    if ! cmp -s "$software_raw" "$native_raw"; then
        echo "FAIL $codec native baseline pixels differ from software output" >&2
        exit 1
    fi
    printf 'PASS %s native-baseline frames=%s switches=%s\n' "$codec" "$expected_frames" "$switches"
}

run_va_single_process() {
    codec=$1
    stream="$root/$codec-dynamic.mkv"
    software_md5="$root/$codec-software.md5"
    va_md5="$root/$codec-va-single-process.md5"
    trace="$root/$codec-va-single-process.trace"

    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error -i "$stream" \
        -pix_fmt yuv420p -f framemd5 "$software_md5"
    # shellcheck disable=SC2086
    timeout "$timeout_seconds" env $va_env ffmpeg -y -hide_banner -loglevel error \
        -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$stream" -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$va_md5" \
        2>"$trace"
    check_frame_count "$va_md5" "$expected_frames" "$codec VA single-process"
    test/iris-eos-check.sh "$trace" "$expected_frames"
    contexts=$(grep -c 'va create_context id=' "$trace" || true)
    if [ "$contexts" -lt 1 ]; then
        echo "FAIL $codec VA single-process did not create a VA context" >&2
        exit 1
    fi
    init_failures=$(grep -c 'session failed:' "$trace" || true)
    if [ "$init_failures" -ne 0 ]; then
        echo "FAIL $codec VA single-process context initialization failures=$init_failures" >&2
        exit 1
    fi
    # FFmpeg recreates its VA context at every coded-size change, so this
    # scenario exercises context churn, not the in-context reconfigure path
    # (that is covered by the session unit test and by a Chromium client).
    # Report the count for the record without gating on it.
    reconfigures=$(grep -c 'session reconfigure -> ' "$trace" || true)
    if ! cmp -s "$software_md5" "$va_md5"; then
        echo "FAIL $codec VA single-process pixels differ from software output" >&2
        exit 1
    fi
    printf 'PASS %s va-single-process frames=%s switches=%s contexts=%s reconfigures=%s\n' \
        "$codec" "$expected_frames" "$switches" "$contexts" "$reconfigures"
}

run_va_new_contexts() {
    codec=$1
    extension=$2
    low="$root/$codec-low.$extension"
    high="$root/$codec-high.$extension"
    low_reference="$root/$codec-low-reference.md5"
    high_reference="$root/$codec-high-reference.md5"
    output="$root/$codec-va-new-context.md5"
    trace="$root/$codec-va-new-context.trace"

    ffmpeg -y -hide_banner -loglevel error -i "$low" -pix_fmt yuv420p -f framemd5 "$low_reference"
    ffmpeg -y -hide_banner -loglevel error -i "$high" -pix_fmt yuv420p -f framemd5 "$high_reference"
    index=0
    while [ "$index" -lt "$segment_count" ]; do
        if [ $((index % 2)) -eq 0 ]; then
            input=$low
            reference=$low_reference
        else
            input=$high
            reference=$high_reference
        fi
        # A separate FFmpeg process creates a fresh VA config/context/surface
        # set. Repeating every transition catches teardown/recreate leaks.
        # shellcheck disable=SC2086
        timeout "$timeout_seconds" env $va_env ffmpeg -y -hide_banner -loglevel error \
            -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -hwaccel_output_format vaapi \
            -i "$input" -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$output" \
            2>"$trace"
        check_frame_count "$output" "$frames_per_segment" "$codec VA new-context iteration=$index"
        test/iris-eos-check.sh "$trace" "$frames_per_segment"
        check_exact_context_count "$trace" "$codec VA new-context iteration=$index" 1
        if ! cmp -s "$reference" "$output"; then
            echo "FAIL $codec VA new-context pixels differ at iteration=$index" >&2
            exit 1
        fi
        index=$((index + 1))
    done
    printf 'PASS %s va-new-context contexts=%s transitions=%s\n' "$codec" "$segment_count" "$switches"
}

echo "Iris dynamic-resolution device=$device switches=$switches frames_per_segment=$frames_per_segment root=$root"
for codec in $codecs; do
    case "$codec" in
        h264)
            require_format H264 h264
            make_concat_stream h264 "${IRIS_H264_ENCODER:-libx264}" mkv \
                '-preset ultrafast -tune zerolatency -g 1 -bf 0 -pix_fmt yuv420p'
            run_native_baseline h264 h264parse v4l2h264dec mkv
            run_va_single_process h264
            run_va_new_contexts h264 mkv
            ;;
        vp9)
            require_format VP90 vp9
            make_concat_stream vp9 "${IRIS_VP9_ENCODER:-libvpx-vp9}" webm \
                '-b:v 300k -g 1 -lag-in-frames 0 -auto-alt-ref 0 -pix_fmt yuv420p'
            run_native_baseline vp9 vp9parse v4l2vp9dec webm
            run_va_single_process vp9
            run_va_new_contexts vp9 webm
            ;;
        *)
            echo "FAIL unknown IRIS_DYNAMIC_CODECS entry: $codec" >&2
            exit 2
            ;;
    esac
done

echo "PASS dynamic-resolution codecs=$codecs switches=$switches root=$root"
