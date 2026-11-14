#!/bin/sh

# Crop through the VA video processor, on the target:
#
#   IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh -- sh test/vulkan-vpp/crop_vaapi_gate.sh
#
# A VA client crops by putting a surface_region in the pipeline buffer. No
# FFmpeg filter in 8.1 submits one (there is no crop_vaapi), so
# `iris-va-vpp-probe` is the client here: it uploads an NV12 frame, runs one
# VideoProc picture over a region, and downloads the result with vaGetImage.
#
# The oracle is `crop=W:H:X:Y,scale=DWxDH:flags=neighbor` on the same input.
# A crop is a pixel move and the kernel samples at swscale's point positions,
# so the two must agree byte for byte — including the frames where the crop is
# not the whole surface and the scale is not 1:1.
#
# The same client then runs one picture with filter_flags naming the bilinear
# method, which the driver advertises and implements. There is no byte-exact
# software reference for it here: swscale widens its support when it
# downsamples, so `flags=bilinear` is a different filter at these ratios. What
# this gate can prove about the method without a per-pixel model is that the
# picture runs and the driver reports the method it ran; the per-pixel model is
# nv12_scale_gate.sh's, over the kernel's own buffers.
#
# The last two cases ask for what the driver must refuse rather than round: an
# odd region, and the "advanced" interpolation method, which has no kernel.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

for tool in ffmpeg sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing target tool: $tool"
done

device=$(iris_resolve_device) || exit 2
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
[ -c "$render" ] || fail "no render node $render"

driver_env=''
if [ "${IRIS_VPP_INSTALLED:-0}" = 1 ]; then
    [ -z "${LIBVA_DRIVERS_PATH:-}" ] || fail "IRIS_VPP_INSTALLED=1 with LIBVA_DRIVERS_PATH set; unset it"
    driver_path=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
    [ -n "$driver_path" ] || fail "libva.pc does not report a driverdir"
else
    driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
    driver_env="LIBVA_DRIVERS_PATH=$driver_path"
fi
[ -f "$driver_path/v4l2_drv_video.so" ] || fail "no v4l2_drv_video.so in $driver_path; set LIBVA_DRIVERS_PATH"
driver_hash=$(sha256sum "$driver_path/v4l2_drv_video.so" | cut -d' ' -f1)

probe=${IRIS_VPP_PROBE:-}
if [ -z "$probe" ]; then
    for candidate in "$(dirname "$driver_path")/test/iris-va-vpp-probe" \
            "$(pwd)/build/test/iris-va-vpp-probe" "$(pwd)/build-local/test/iris-va-vpp-probe"; do
        if [ -x "$candidate" ]; then
            probe=$candidate
            break
        fi
    done
fi
[ -n "$probe" ] && [ -x "$probe" ] || fail "probe not built; set IRIS_VPP_PROBE"

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp)}/crop
mkdir -p "$work"

# src:  crop WxH+X+Y: dst
cases="128x72:96x40+16+16:86x48 64x32:32x16+16+8:128x64 128x128:64x64+32+32:32x32"

failed=0
for c in $cases; do
    src=${c%%:*}
    rest=${c#*:}
    crop=${rest%%:*}
    dst=${rest#*:}
    sw=${src%x*}
    sh_=${src#*x}
    dw=${dst%x*}
    dh=${dst#*x}
    cw=${crop%x*}
    ch=${crop#*x}
    ch=${ch%%+*}
    cx=$(echo "$crop" | cut -d+ -f2)
    cy=$(echo "$crop" | cut -d+ -f3)
    echo "== $src crop ${cw}x${ch}+$cx+$cy -> $dst"
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$src:rate=1" \
        -frames:v 1 -pix_fmt nv12 -f rawvideo "$work/src-$src.nv12"
    ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt nv12 -s "$src" \
        -i "$work/src-$src.nv12" -vf "crop=$cw:$ch:$cx:$cy,scale=$dst:flags=neighbor" \
        -f rawvideo "$work/ref-$src-$dst.nv12"
    rc=0
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" "$render" "$work/src-$src.nv12" "$sw" "$sh_" "$cw" "$ch" "$cx" "$cy" "$dw" "$dh" \
        "$work/hw-$src-$dst.nv12" >"$work/$src-$dst.log" 2>"$work/$src-$dst.trace" || rc=$?
    if [ "$rc" -eq 0 ]; then
        cmp "$work/ref-$src-$dst.nv12" "$work/hw-$src-$dst.nv12" || rc=$?
    fi
    ends=$(grep -c 'entrypoint=videoproc' "$work/$src-$dst.trace" || true)
    if [ "$ends" -lt 1 ]; then
        echo "no videoproc record in the trace" >&2
        rc=1
    fi
    printf 'case=%s crop=%s dst=%s exit=%s ref=%s hw=%s\n' "$src" "$crop" "$dst" "$rc" \
        "$(sha256sum "$work/ref-$src-$dst.nv12" 2>/dev/null | cut -d' ' -f1)" \
        "$(sha256sum "$work/hw-$src-$dst.nv12" 2>/dev/null | cut -d' ' -f1)"
    [ "$rc" -eq 0 ] || failed=1
done
[ "$failed" -eq 0 ] || fail "a cropped picture differs from crop,scale:flags=neighbor"

echo "== bilinear through the same client"
if ! env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" "$render" "$work/src-128x72.nv12" 128 72 128 72 0 0 86 48 \
        "$work/bilinear.nv12" --interp=bilinear >"$work/bilinear.log" 2>"$work/bilinear.trace"; then
    sed -n '1,20p' "$work/bilinear.log" >&2 || true
    fail "a bilinear picture failed"
fi
grep -q 'the caps name advanced interpolation' "$work/bilinear.log" \
    || fail "the caps do not name advanced interpolation"
printf 'case=bilinear caps=advertised picture=ran size=%s\n' "$(sha256sum "$work/bilinear.nv12" | cut -d' ' -f1)"

# Advanced is the driver's own pick: the area average when an axis shrinks and
# the cubic when it grows. It has to run, and it has to differ from the point
# map on a downscale, or "advanced" is a name for nothing.
echo "== the advanced method runs its own kernel"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" "$render" "$work/src-128x72.nv12" 128 72 128 72 0 0 86 48 \
        "$work/advanced.nv12" --interp=advanced >"$work/advanced.log" 2>"$work/advanced.trace"; then
    :
else
    sed -n '1,20p' "$work/advanced.log" >&2 || true
    fail "the driver did not run VA_FILTER_INTERPOLATION_ADVANCED"
fi
if cmp -s "$work/advanced.nv12" "$work/bilinear.nv12"; then
    fail "advanced and bilinear produced the same frame on a downscale"
fi
printf 'case=advanced size=%s\n' "$(sha256sum "$work/advanced.nv12" | cut -d' ' -f1)"

echo "== an odd region is refused, not rounded"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" "$render" "$work/src-128x72.nv12" 128 72 31 16 16 8 64 32 \
        "$work/refused.nv12" --expect-refusal >"$work/refused.log" 2>"$work/refused.trace"; then
    :
else
    sed -n '1,20p' "$work/refused.log" >&2 || true
    fail "the driver did not refuse an odd source region"
fi

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
echo "PASS va-vpp crops=$(echo "$cases" | wc -w) exact driver=$driver_hash"
