#!/bin/sh

# Hardware gate for UBWC (Q08C) decode surfaces. The driver can ask the node
# for the four-plane compressed CAPTURE format and hand the buffer out as a
# QCOM_COMPRESSED dma-buf, but nothing in this tree can read UBWC: EGL does
# not, and the CPU paths have no such format. The GPU can, through
# VK_EXT_image_drm_format_modifier. So the evidence is a read-back: decode the
# same stream twice (one linear capture, one compressed), read the compressed
# buffer through Turnip, and require every luma and chroma sample to equal the
# linear decode's.
#
# The gate generates its own 640x480 streams: an access-unit-delimited
# Annex-B stream per H.264/HEVC codec (the probe splits on the delimiter) and
# all-intra VP9/AV1 IVF (the probe feeds raw access units, so a stream whose
# pictures reference the driver's AV1 install units is out of scope here). It
# then runs test/iris-decode-stream-probe twice per codec and
# test/vk-ubwc-read over the dumps.
#
# The VP9 stream also feeds test/iris-va-ubwc-probe, which asks for the
# modifier through vaCreateSurfaces2, decodes, and exports: the grant, the
# four-plane descriptor and the image-entry-point refusals come from the
# public API. VP9 is the codec there because its whole frame is one slice
# buffer, so the probe is a decode client without synthesising parameter
# sets. It runs once with V4L2_VA_UBWC=1 and once without, where a list that
# names only the compressed modifier must be refused and a list carrying a
# LINEAR fallback must get LINEAR.
#
# Usage:
#   sh test/iris-ubwc-gate.sh
#
# Environment:
#   IRIS_UBWC_CODECS  "h264 hevc vp9 av1" by default
#   IRIS_UBWC_WIDTH   stream width (default 640)
#   IRIS_UBWC_HEIGHT  stream height (default 480)
#   IRIS_UBWC_FRAMES  frames per stream (default 12)
#   IRIS_UBWC_OUT     artifact directory (default <lab>/artifacts/ubwc-gate)
#   IRIS_UBWC_DUMP    dump every frame instead of the first and the last
#   IRIS_RENDER_NODE  DRM render node for the VA half (default /dev/dri/renderD128)
#
# V4L2_VA_UBWC does not apply: this probe drives the session directly, the way
# the VA layer does once a client asks for the modifier, and the switch only
# decides whether the VA surface attribute is accepted.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
. "$here/lib/iris-env.sh"

codecs=${IRIS_UBWC_CODECS:-h264 hevc vp9 av1}
width=${IRIS_UBWC_WIDTH:-640}
height=${IRIS_UBWC_HEIGHT:-480}
frames=${IRIS_UBWC_FRAMES:-12}
out_base=${IRIS_UBWC_OUT:-$(iris_artifact_dir ubwc-gate)}
dump_all=${IRIS_UBWC_DUMP:-}

probe="$root/build/test/iris-decode-stream-probe"
reader="$root/build/test/vk-ubwc-read"
q08c_modifier=0x500000000000001

fail() {
    echo "FAIL ubwc-gate: $*" >&2
    exit 1
}

[ -x "$probe" ] || fail "build test/iris-decode-stream-probe first (ninja -C build)"
[ -x "$reader" ] || fail "test/vk-ubwc-read is missing: this build found no Vulkan"
command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required to generate the streams'
node=$(iris_resolve_device) || fail 'no Iris decoder node'

mkdir -p "$out_base"
media="$out_base/media"
frames_dir="$out_base/frames"
mkdir -p "$media" "$frames_dir"

encode() {
    out=$1
    shift
    [ -s "$out" ] && return 0
    ffmpeg -hide_banner -loglevel error -y -f lavfi \
        -i "testsrc2=size=${width}x${height}:rate=10" -frames:v "$frames" "$@" "$out" \
        || fail "ffmpeg could not generate $out"
}

case " $codecs " in
*" h264 "*) encode "$media/h264.h264" -c:v libx264 -preset veryfast -bsf:v h264_metadata=aud=insert -f h264 ;;
esac
case " $codecs " in
*" hevc "*) encode "$media/hevc.h265" -c:v libx265 -preset ultrafast -bsf:v hevc_metadata=aud=insert -f hevc ;;
esac
case " $codecs " in
*" vp9 "*) encode "$media/vp9.ivf" -c:v libvpx-vp9 -b:v 1M -f ivf ;;
esac
case " $codecs " in
*" av1 "*) encode "$media/av1.ivf" -c:v libaom-av1 -cpu-used 8 -g 1 -b:v 1M -f ivf ;;
esac

# Stream, capture name and, for the Annex-B codecs, the codec and geometry:
# the probe cannot read geometry out of an elementary stream.
probe_args() {
    case $1 in
    h264) echo "$media/h264.h264 $2 h264 $width $height" ;;
    hevc) echo "$media/hevc.h265 $2 hevc $width $height" ;;
    vp9) echo "$media/vp9.ivf $2" ;;
    av1) echo "$media/av1.ivf $2" ;;
    esac
}

failures=0
for codec in $codecs; do
    linear_dir="$frames_dir/$codec-nv12"
    compressed_dir="$frames_dir/$codec-q08c"
    rm -rf "$linear_dir" "$compressed_dir"
    mkdir -p "$linear_dir" "$compressed_dir"
    log="$out_base/$codec.log"

    # shellcheck disable=SC2086
    if ! IRIS_PROBE_DUMP="$compressed_dir" "$probe" "$node" $(probe_args "$codec" Q08C) >"$log" 2>&1; then
        echo "FAIL $codec: the compressed decode did not complete" >&2
        sed 's/^/  /' "$log" >&2
        failures=$((failures + 1))
        continue
    fi
    if ! grep -q "modifier=$q08c_modifier" "$log"; then
        echo "FAIL $codec: the CAPTURE pool is not Q08C" >&2
        sed 's/^/  /' "$log" >&2
        failures=$((failures + 1))
        continue
    fi
    # shellcheck disable=SC2086
    if ! IRIS_PROBE_DUMP="$linear_dir" "$probe" "$node" $(probe_args "$codec" NV12) >>"$log" 2>&1; then
        echo "FAIL $codec: the linear decode did not complete" >&2
        failures=$((failures + 1))
        continue
    fi

    last=$((frames - 1))
    verdict=0
    if [ -n "$dump_all" ]; then
        indices=$(seq 0 "$last")
    else
        indices="0 $last"
    fi
    for index in $indices; do
        if ! "$reader" "$linear_dir/frame-$index.bin" "$compressed_dir/frame-$index.bin" \
            "$width" "$height" >>"$log" 2>&1; then
            echo "FAIL $codec frame $index: the read-back differs from the linear decode" >&2
            tail -3 "$log" >&2
            verdict=1
        fi
    done
    if [ "$verdict" -ne 0 ]; then
        failures=$((failures + 1))
        continue
    fi
    echo "PASS $codec: the compressed frames read back exactly as the linear decode"
done

if [ "$failures" -ne 0 ]; then
    fail "$failures codec(s) failed; logs in $out_base"
fi

# The VA half: the API a browser or compositor calls.
va_probe="$root/build/test/iris-va-ubwc-probe"
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
[ -x "$va_probe" ] || fail "build test/iris-va-ubwc-probe first (needs libva-drm)"
case " $codecs " in
*" vp9 "*) ;;
*) fail 'the VA half needs vp9 in IRIS_UBWC_CODECS' ;;
esac
[ -c "$render" ] || fail "no render node $render"
driver_path=${LIBVA_DRIVERS_PATH:-$root/build/src}
[ -f "$driver_path/v4l2_drv_video.so" ] || fail "no v4l2_drv_video.so in $driver_path"
va_log="$out_base/va-probe.log"
if ! LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" "$va_probe" "$render" "$media/vp9.ivf" "$width" "$height" \
    >"$va_log" 2>&1; then
    echo 'FAIL va: the probe did not pass without the opt-in' >&2
    sed 's/^/  /' "$va_log" >&2
    fail 'the VA half failed'
fi
if ! V4L2_VA_UBWC=1 LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" "$va_probe" "$render" "$media/vp9.ivf" \
    "$width" "$height" >>"$va_log" 2>&1; then
    echo 'FAIL va: the probe did not pass with V4L2_VA_UBWC=1' >&2
    tail -20 "$va_log" >&2
    fail 'the VA half failed'
fi
if ! grep -q 'PASS the VA layer grants and exports a UBWC surface' "$va_log"; then
    echo 'FAIL va: the opt-in run did not reach its PASS line' >&2
    tail -20 "$va_log" >&2
    fail 'the VA half failed'
fi

echo "PASS ubwc-gate: $codecs va=pass"
