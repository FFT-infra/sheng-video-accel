#!/bin/sh

# GStreamer VA-API gate: decode and encode through libgstva on the Iris target.
#
# libgstva checks the VA driver's vendor string before it builds an element, so
# on this machine `gst-inspect-1.0 va` lists 0 features and `vaav1dec` does not
# exist. `GST_VA_ALL_DRIVERS=1` lifts that filter and the plugin lists seven
# elements. Nothing else is set: libva reaches this driver through its
# msm_drv_video.so alias, the same way the browser gate reaches it.
#
# The decode comparison does not use `fdsink`. Raw frames written by it start
# at the buffer base and ignore GstVideoMeta's plane offset, so each dumped
# picture is rotated inside its buffer: on the 120-frame clip the first frames
# matched the software decode only after a per-frame circular shift (mean
# absolute difference 93/255 unshifted, 0.25 after the shift, six bytes above
# 8 once shifted). Muxing the I420 the element produced and hashing that file
# after a software decode maps through GstVideoFrame and compares exactly,
# which is what this gate does. A mismatch on the muxed path is a decode
# difference; a mismatch on a raw dump is not.
#
# Usage:
#   test/iris-gstreamer-gate.sh --clip PATH [--out DIR] [--frames N]
#
# Exit status is 0 only when the elements register with the override, the
# decode is frame-identical to the software decode, and both encoder elements
# return a stream that decodes back to the requested frame count.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/iris-env.sh
. "$here/lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

usage() {
    sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
}

clip=''
out=''
frames=60
while [ $# -gt 0 ]; do
    case $1 in
        --clip) clip=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --frames) frames=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[ -n "$clip" ] || { usage; fail "--clip is required"; }
[ -r "$clip" ] || fail "clip not readable: $clip"
clip=$(CDPATH='' cd -- "$(dirname -- "$clip")" && pwd)/$(basename -- "$clip")
[ -n "$out" ] || out=$(iris_artifact_dir gstreamer-gate)
mkdir -p "$out"

command -v gst-launch-1.0 >/dev/null 2>&1 || fail "gst-launch-1.0 is not installed"
command -v gst-inspect-1.0 >/dev/null 2>&1 || fail "gst-inspect-1.0 is not installed"

# A private registry, so a cached scan cannot stand in for this run.
export GST_REGISTRY=$out/registry.bin
export GST_DEBUG_NO_COLOR=1
unset GST_VA_ALL_DRIVERS || true
unset LIBVA_DRIVER_NAME LIBVA_V4L2_VIDEO_PATH LIBVA_V4L2_ENCODER_PATH || true

without_override=$(gst-inspect-1.0 va 2>/dev/null | sed -n 's/^ *\([0-9][0-9]*\) features:.*/\1/p' | head -1)
export GST_VA_ALL_DRIVERS=1
listed=$out/va-elements.txt
gst-inspect-1.0 va >"$listed" 2>&1 || fail "gst-inspect-1.0 va failed with GST_VA_ALL_DRIVERS=1"
with_override=$(sed -n 's/^  +-- \([0-9][0-9]*\) elements/\1/p' "$listed" | head -1)
for element in vaav1dec vah264enc vah265enc; do
    grep -q "^  $element: " "$listed" || fail "$element does not register even with GST_VA_ALL_DRIVERS=1"
done

printf 'boot=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
printf 'driver=%s\n' "$(sha256sum /usr/lib64/dri/v4l2_drv_video.so 2>/dev/null | cut -d' ' -f1)"
printf 'module=%s\n' "$(sha256sum "/lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/qcom-iris.ko.zst" 2>/dev/null | cut -d' ' -f1)"
printf 'va_plugin=%s\n' "$(sed -n 's/^ *Version *//p' "$listed" | head -1)"
printf 'features_without_override=%s elements_with_override=%s\n' \
    "${without_override:-0}" "${with_override:-0}"

status=0

# Decode: the element's I420 output against the software decode of the clip.
gst-launch-1.0 -e filesrc location="$clip" ! matroskademux ! av1parse ! vaav1dec \
    ! videoconvert ! video/x-raw,format=I420 ! matroskamux \
    ! filesink location="$out/decode.mkv" >"$out/decode.log" 2>&1 \
    || { fail "vaav1dec pipeline failed; see $out/decode.log"; }
ffmpeg -y -v error -i "$clip" -pix_fmt yuv420p -f framemd5 "$out/decode-sw.md5"
ffmpeg -y -v error -i "$out/decode.mkv" -pix_fmt yuv420p -f framemd5 "$out/decode-gst.md5"
grep -v '^#' "$out/decode-sw.md5" >"$out/decode-sw.body"
grep -v '^#' "$out/decode-gst.md5" >"$out/decode-gst.body"
sw_frames=$(wc -l <"$out/decode-sw.body")
gst_frames=$(wc -l <"$out/decode-gst.body")
decode_same=no
cmp -s "$out/decode-sw.body" "$out/decode-gst.body" && decode_same=yes
printf 'decode frames sw=%s gst=%s identical=%s\n' "$sw_frames" "$gst_frames" "$decode_same"
[ "$decode_same" = yes ] || status=1

# Encode: the element's stream has to decode back to the frames it was given.
for element in vah264enc vah265enc; do
    case $element in
        vah264enc) parse=h264parse ;;
        *) parse=h265parse ;;
    esac
    gst-launch-1.0 -e videotestsrc num-buffers="$frames" \
        ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
        ! "$element" ! "$parse" ! filesink location="$out/$element.out" \
        >"$out/$element.log" 2>&1 || { fail "$element failed; see $out/$element.log"; }
    decoded=$(ffmpeg -v error -i "$out/$element.out" -f framemd5 - 2>/dev/null | grep -vc '^#' || true)
    size=$(stat -c %s "$out/$element.out" 2>/dev/null || echo 0)
    printf 'encode %s requested=%s decoded=%s size=%s\n' "$element" "$frames" "$decoded" "$size"
    [ "$decoded" = "$frames" ] || status=1
done

[ "$status" -eq 0 ] || fail "GStreamer VA gate did not pass; artifacts in $out"
printf 'PASS GStreamer VA decode and encode through the Iris driver\n'
