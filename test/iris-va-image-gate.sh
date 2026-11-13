#!/bin/sh

# The upload half of the image API, on the target:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/iris-va-image-gate.sh
#
# A client that brings its own pixels has two routes into a surface: an image
# it creates and hands over with vaPutImage, or a derived image it maps and
# writes. Either way vaGetImage must be able to read the result; before this
# gate existed the second route failed with "surface has no decoded pixels".
#
# The probe covers the API contract, including the regions and the refusals.
# FFmpeg then runs the same two routes a real client uses
# (hwupload,hwdownload), so the check is not only the probe agreeing with
# itself.
#
# Driver selection matches test/vulkan-vpp/scale_vaapi_gate.sh: `build/src`
# unless LIBVA_DRIVERS_PATH says otherwise, and IRIS_GATE_INSTALLED=1 to run
# the same cases against the installed package.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

for tool in ffmpeg sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing target tool: $tool"
done

device=$(iris_resolve_device) || exit 2
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
[ -c "$render" ] || fail "no render node $render"

driver_env=''
if [ "${IRIS_GATE_INSTALLED:-0}" = 1 ]; then
    [ -z "${LIBVA_DRIVERS_PATH:-}" ] || fail "IRIS_GATE_INSTALLED=1 with LIBVA_DRIVERS_PATH set; unset it"
    driver_path=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
    [ -n "$driver_path" ] || fail "libva.pc does not report a driverdir"
else
    driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
    driver_env="LIBVA_DRIVERS_PATH=$driver_path"
fi
[ -f "$driver_path/v4l2_drv_video.so" ] || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
driver_hash=$(sha256sum "$driver_path/v4l2_drv_video.so" | cut -d' ' -f1)

probe=${IRIS_IMAGE_PROBE:-$(pwd)/build-local/test/iris-va-image-probe}
[ -x "$probe" ] || fail "probe not built: $probe"

work=${IRIS_IMAGE_DIR:-$(iris_artifact_dir va-image)}
mkdir -p "$work"

echo "== the image API contracts"
env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" timeout 120 "$probe" "$render"

echo "== hwupload,hwdownload round trips"
failed=0
# Four shapes: a plain 64x64, an odd-height 128x72 (chroma rows land on a
# half row, which is where an off-by-one in the plane arithmetic shows), a
# 10-bit P010 frame, and a small 32x32 that sits under the decoder node's
# floor but not under the image API's.
cases="nv12:64x64 nv12:128x72 p010:64x64 nv12:32x32"
for c in $cases; do
    fmt=${c%%:*}
    size=${c#*:}
    w=${size%x*}
    h=${size#*x}
    echo "== $fmt $size"
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=1" -frames:v 1 \
        -pix_fmt "$fmt" -f rawvideo "$work/src-$fmt-$size.raw"
    if ! env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" \
            timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
            -vaapi_device "$render" -f rawvideo -pix_fmt "$fmt" -s "$size" \
            -i "$work/src-$fmt-$size.raw" \
            -vf "format=$fmt,hwupload,hwdownload,format=$fmt" \
            -f rawvideo "$work/back-$fmt-$size.raw" 2>"$work/$fmt-$size.trace"; then
        sed -n '1,30p' "$work/$fmt-$size.trace" >&2 || true
        printf 'case=%s-%s exit=1 (upload/read-back failed)\n' "$fmt" "$size"
        failed=1
        continue
    fi
    rc=0
    cmp "$work/src-$fmt-$size.raw" "$work/back-$fmt-$size.raw" || rc=$?
    printf 'case=%s-%s exit=%s sha=%s\n' "$fmt" "$size" "$rc" \
        "$(sha256sum "$work/back-$fmt-$size.raw" | cut -d' ' -f1)"
    [ "$rc" -eq 0 ] || failed=1
done
[ "$failed" -eq 0 ] || fail "a frame did not survive upload and read-back unchanged"

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
echo "PASS va-image upload and read-back driver=$driver_hash"
