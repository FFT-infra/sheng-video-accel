#!/bin/sh

# End-to-end VA video processor gate, run on the target under
# test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/scale_vaapi_gate.sh
#
# nv12_scale_gate.sh proved the compute kernel over two imported DMA-BUFs.
# This gate proves the VA surface path around it: a client opens VAProfileNone
# / VAEntrypointVideoProc, uploads an NV12 frame, runs scale_vaapi and
# downloads the result. Every output byte is compared with swscale's
# nearest-neighbour result on the same input and geometry. The driver samples
# at swscale's point positions, so the comparison is byte-exact at any ratio,
# not just at 2x.
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

# Ratios a client can ask for: 2x (the qualified geometry), fractional up and
# down, a result below the decoder node's minimum, the advertised floor, a
# mismatched aspect, and a real 720p frame each way. The driver samples at
# swscale's point positions, so `scale_vaapi=w=W:h=H` must be byte-identical
# to `scale=WxH:flags=neighbor` in every case. Equal geometry is not here:
# FFmpeg's scale_vaapi passes such a frame through without a VPP picture.
cases="64x64:128x128 64x64:96x96 64x64:32x32 128x128:16x16 128x128:200x150 62x34:46x26 1280x720:854x480 1280x720:1920x1080"

failed=0
for c in $cases; do
    src=${c%%:*}
    dst=${c#*:}
    sw=${src%x*}
    sh_=${src#*x}
    dw=${dst%x*}
    dh=${dst#*x}
    echo "== $src -> $dst"
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$src:rate=1" \
        -frames:v 1 -pix_fmt nv12 -f rawvideo "$work/src-$src.nv12"
    ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt nv12 -s "$src" \
        -i "$work/src-$src.nv12" -vf "scale=$dst:flags=neighbor" -f rawvideo "$work/ref-$src-$dst.nv12"
    ref_hash=$(sha256sum "$work/ref-$src-$dst.nv12" | cut -d' ' -f1)
    if ! env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
            LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
            timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
            -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s "$src" -i "$work/src-$src.nv12" \
            -vf "format=nv12,hwupload,scale_vaapi=w=$dw:h=$dh,hwdownload,format=nv12" \
            -f rawvideo "$work/hw-$src-$dst.nv12" 2>"$work/$src-$dst.trace"; then
        sed -n '1,40p' "$work/$src-$dst.trace" >&2 || true
        printf 'case=%s->%s exit=1 (scale_vaapi failed)\n' "$src" "$dst"
        failed=1
        continue
    fi
    hw_hash=$(sha256sum "$work/hw-$src-$dst.nv12" | cut -d' ' -f1)
    ends=$(grep -c 'entrypoint=videoproc' "$work/$src-$dst.trace" || true)
    rc=0
    cmp "$work/ref-$src-$dst.nv12" "$work/hw-$src-$dst.nv12" || rc=$?
    if [ "$ends" -lt 3 ]; then
        echo "only $ends videoproc records; expected create, begin and end" >&2
        rc=1
    fi
    if grep -Eq 'va error|Vulkan VPP scale failed' "$work/$src-$dst.trace"; then
        grep -E 'va error|Vulkan VPP scale failed' "$work/$src-$dst.trace" >&2
        rc=1
    fi
    printf 'case=%s->%s exit=%s ref=%s hw=%s videoproc=%s\n' "$src" "$dst" "$rc" "$ref_hash" "$hw_hash" "$ends"
    [ "$rc" -eq 0 ] || failed=1
done
[ "$failed" -eq 0 ] || fail "VPP output differs from swscale neighbor for at least one ratio"

echo "== opt-in is required: the same client is refused without V4L2_VA_VPP"
if env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s 64x64 -i "$work/src-64x64.nv12" \
        -vf 'format=nv12,hwupload,scale_vaapi=w=128:h=128,hwdownload,format=nv12' \
        -f rawvideo "$work/default.nv12" 2>"$work/default.trace"; then
    fail "scale_vaapi ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'entrypoint=videoproc' "$work/default.trace"; then
    fail "videoproc appeared without the opt-in"
fi

printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'cases=%s\n' "$(echo "$cases" | wc -w)"
printf 'oracle=swscale:flags=neighbor byte-identical for every case\n'
echo "PASS va-vpp ratios=$(echo "$cases" | wc -w) exact driver=$driver_hash"
