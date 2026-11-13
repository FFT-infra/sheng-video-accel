#!/bin/sh

# End-to-end VA video processor gate, run on the target under
# test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/scale_vaapi_gate.sh
#
# nv12_scale_gate.sh proved the compute kernel over two imported DMA-BUFs.
# This gate proves the VA surface path around it: a client opens VAProfileNone
# / VAEntrypointVideoProc, uploads a 128x128 NV12 frame, runs scale_vaapi to
# 256x256 and downloads it. Every output byte is compared with swscale's
# nearest-neighbour result on the same input. The kernel scales exactly 2x
# with nearest sampling, so this geometry is both what the driver advertises
# and what swscale can reproduce byte-for-byte; another ratio would not be a
# like-for-like oracle.
#
# The entrypoint is opt-in. The same client must be refused without
# V4L2_VA_VPP, which is what keeps a VPP-capable build from changing the
# decode/encode surface an installed client already qualified against.
#
# Target prerequisites: ffmpeg built with --enable-vaapi (scale_vaapi,
# hwupload, hwdownload), libvulkan, and a driver .so built with the Vulkan
# backend. CI has no GPU, so this runs only here.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

for tool in ffmpeg sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing target tool: $tool"
done
filters=$(ffmpeg -hide_banner -filters 2>/dev/null || true)
for filter in scale_vaapi hwupload hwdownload; do
    printf '%s\n' "$filters" | grep -Eq "[[:space:]]$filter[[:space:]]" \
        || fail "ffmpeg has no $filter filter (needs --enable-vaapi)"
done

device=$(iris_resolve_device) || exit 2
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
[ -c "$render" ] || fail "no render node $render"
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
[ -f "$driver_path/v4l2_drv_video.so" ] || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
driver_hash=$(sha256sum "$driver_path/v4l2_drv_video.so" | cut -d' ' -f1)

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp)}
mkdir -p "$work"

echo "== build the 128x128 NV12 input once"
ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=128x128:rate=1 \
    -frames:v 1 -pix_fmt nv12 -f rawvideo "$work/src.nv12"

echo "== software oracle: swscale nearest-neighbour 2x"
ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt nv12 -s 128x128 -i "$work/src.nv12" \
    -vf scale=256:256:flags=neighbor -f rawvideo "$work/ref.nv12"
ref_hash=$(sha256sum "$work/ref.nv12" | cut -d' ' -f1)

echo "== opt-in: scale_vaapi end to end"
if ! env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s 128x128 -i "$work/src.nv12" \
        -vf 'format=nv12,hwupload,scale_vaapi=w=256:h=256,hwdownload,format=nv12' \
        -f rawvideo "$work/hw.nv12" 2>"$work/vpp.trace"; then
    sed -n '1,40p' "$work/vpp.trace" >&2 || true
    fail "scale_vaapi failed with V4L2_VA_VPP=1 (is the driver built with Vulkan?)"
fi
cmp "$work/ref.nv12" "$work/hw.nv12" || fail "VPP output differs from swscale neighbor 2x"
hw_hash=$(sha256sum "$work/hw.nv12" | cut -d' ' -f1)

echo "== the picture actually ran through the VideoProc entrypoint"
grep -q 'entrypoint=videoproc' "$work/vpp.trace" || {
    sed -n '1,40p' "$work/vpp.trace" >&2 || true
    fail "no videoproc trace record; the client did not reach this backend"
}
ends=$(grep -c 'entrypoint=videoproc' "$work/vpp.trace" || true)
if grep -Eq 'va error|Vulkan VPP scale failed' "$work/vpp.trace"; then
    grep -E 'va error|Vulkan VPP scale failed' "$work/vpp.trace" >&2
    fail "VPP diagnostics in the trace"
fi
[ "$ends" -ge 3 ] || fail "only $ends videoproc records; expected create, begin and end"

echo "== opt-in is required: the same client is refused without V4L2_VA_VPP"
if env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s 128x128 -i "$work/src.nv12" \
        -vf 'format=nv12,hwupload,scale_vaapi=w=256:h=256,hwdownload,format=nv12' \
        -f rawvideo "$work/default.nv12" 2>"$work/default.trace"; then
    fail "scale_vaapi ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'entrypoint=videoproc' "$work/default.trace"; then
    fail "videoproc appeared without the opt-in"
fi

printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'oracle=swscale:flags=neighbor 128x128->256x256 ref=%s\n' "$ref_hash"
printf 'vpp=hwdownload 256x256 nv12=%s\n' "$hw_hash"
printf 'trace_videoproc_records=%s\n' "$ends"
echo "PASS va-vpp 2x-exact driver=$driver_hash"
