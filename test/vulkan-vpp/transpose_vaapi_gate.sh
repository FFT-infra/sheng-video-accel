#!/bin/sh

# Rotation through the VA video processor, run on the target under
# test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/transpose_vaapi_gate.sh
#
# nv12_scale_gate.sh and p010_scale_gate.sh prove the kernels' rotation map
# over imported DMA-BUFs. This gate proves the VA surface path around it: a
# client opens VAProfileNone / VAEntrypointVideoProc, uploads a frame, runs
# transpose_vaapi and downloads the result. Every output byte is compared with
# FFmpeg's software transpose on the same input. VA orders rotation before
# scaling, and transpose_vaapi asks for a quarter turn onto a transposed
# output, so the comparison is byte-exact for all three directions the driver
# advertises.
#
# Mirroring is not implemented and the verdict is visible two ways: the
# pipeline caps advertise no mirror flag, and a client that sends one anyway
# is refused rather than getting unmirrored pixels. dir=clock_flip (rotation
# plus a vertical mirror) is the case that proves the refusal.
#
# The entrypoint is opt-in. The same client must be refused without
# V4L2_VA_VPP, which is what keeps a VPP-capable build from changing the
# decode/encode surface an installed client already qualified against.
#
# Target prerequisites: ffmpeg built with --enable-vaapi (transpose_vaapi,
# hwupload, hwdownload), libvulkan, and a driver .so built with the Vulkan
# backend. By default the driver under test is `build/src` (LIBVA_DRIVERS_PATH
# is exported to it); IRIS_VPP_INSTALLED=1 runs the same cases against the
# installed package, letting libva resolve its own driver directory so the
# evidence record can name which artifact produced it. CI has no GPU, so this
# runs only here.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

for tool in ffmpeg sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing target tool: $tool"
done
filters=$(ffmpeg -hide_banner -filters 2>/dev/null || true)
for filter in transpose_vaapi transpose hwupload hwdownload; do
    printf '%s\n' "$filters" | grep -Eq "[[:space:]]$filter[[:space:]]" \
        || fail "ffmpeg has no $filter filter"
done
# The software transpose filter lost its named half-turn option in 8.x; a
# half-turn is hflip followed by vflip, which is the same bytes. The hardware
# filter still names it, so the gate says so plainly when it does not.
ffmpeg -hide_banner -h filter=transpose_vaapi 2>/dev/null | grep -q 'reversal' \
    || fail "transpose_vaapi has no reversal direction in this ffmpeg"

# The software chain that produces the same pixels for each direction the
# driver advertises.
software_chain() {
    case $1 in
        clock) echo 'transpose=clock' ;;
        cclock) echo 'transpose=cclock' ;;
        reversal) echo 'hflip,vflip' ;;
    esac
}

device=$(iris_resolve_device) || exit 2
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
[ -c "$render" ] || fail "no render node $render"
# Two ways to name the driver under test: a build tree (the default, selected
# through LIBVA_DRIVERS_PATH) or the installed package (IRIS_VPP_INSTALLED=1,
# where libva resolves its own driver directory and no LIBVA_DRIVERS_PATH is
# exported, so the record can say which one produced the evidence).
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

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp-transpose)}
mkdir -p "$work"

# A square frame catches a swapped direction, a wide frame catches a swapped
# axis, and 128x72 is the geometry the other VPP gates use. Each direction the
# driver advertises runs in both sample widths.
cases="
nv12:64x64:clock
nv12:64x64:cclock
nv12:64x64:reversal
nv12:128x72:clock
nv12:128x72:cclock
nv12:128x72:reversal
p010le:128x72:clock
p010le:128x72:cclock
p010le:128x72:reversal
"

failed=0
run_case() {
    pix=$1
    src=$2
    dir=$3
    tag=$pix-$src-$dir
    echo "== $pix $src dir=$dir"
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$src:rate=1" \
        -frames:v 1 -pix_fmt "$pix" -f rawvideo "$work/src-$pix-$src.raw"
    # Software transpose swaps the dimensions too, so the reference and the
    # hardware output have the same size by construction.
    ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt "$pix" -s "$src" \
        -i "$work/src-$pix-$src.raw" -vf "$(software_chain "$dir")" -f rawvideo "$work/ref-$tag.raw"
    ref_hash=$(sha256sum "$work/ref-$tag.raw" | cut -d' ' -f1)
    if ! env LIBVA_DRIVER_NAME=v4l2 $driver_env \
            LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
            timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
            -vaapi_device "$render" -f rawvideo -pix_fmt "$pix" -s "$src" -i "$work/src-$pix-$src.raw" \
            -vf "format=$pix,hwupload,transpose_vaapi=dir=$dir,hwdownload,format=$pix" \
            -f rawvideo "$work/hw-$tag.raw" 2>"$work/$tag.trace"; then
        sed -n '1,40p' "$work/$tag.trace" >&2 || true
        printf 'case=%s exit=1 (transpose_vaapi failed)\n' "$tag"
        failed=1
        return
    fi
    hw_hash=$(sha256sum "$work/hw-$tag.raw" | cut -d' ' -f1)
    ends=$(grep -c 'entrypoint=videoproc' "$work/$tag.trace" || true)
    rc=0
    cmp "$work/ref-$tag.raw" "$work/hw-$tag.raw" || rc=$?
    if [ "$ends" -lt 3 ]; then
        echo "only $ends videoproc records; expected create, begin and end" >&2
        rc=1
    fi
    if grep -Eq 'va error|Vulkan VPP scale failed' "$work/$tag.trace"; then
        grep -E 'va error|Vulkan VPP scale failed' "$work/$tag.trace" >&2
        rc=1
    fi
    printf 'case=%s exit=%s ref=%s hw=%s videoproc=%s\n' "$tag" "$rc" "$ref_hash" "$hw_hash" "$ends"
    [ "$rc" -eq 0 ] || failed=1
}

for c in $cases; do
    run_case "${c%%:*}" "$(echo "$c" | cut -d: -f2)" "${c##*:}"
done
[ "$failed" -eq 0 ] || fail "VPP transpose differs from the software transpose for at least one direction"

echo "== mirroring is refused, not ignored"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s 64x64 -i "$work/src-nv12-64x64.raw" \
        -vf 'format=nv12,hwupload,transpose_vaapi=dir=clock_flip,hwdownload,format=nv12' \
        -f rawvideo "$work/mirror.nv12" 2>"$work/mirror.trace"; then
    fail "transpose_vaapi dir=clock_flip ran; the driver does not mirror and must say so"
fi

echo "== opt-in is required: the same client is refused without V4L2_VA_VPP"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" -f rawvideo -pix_fmt nv12 -s 64x64 -i "$work/src-nv12-64x64.raw" \
        -vf 'format=nv12,hwupload,transpose_vaapi=dir=clock,hwdownload,format=nv12' \
        -f rawvideo "$work/default.nv12" 2>"$work/default.trace"; then
    fail "transpose_vaapi ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'entrypoint=videoproc' "$work/default.trace"; then
    fail "videoproc appeared without the opt-in"
fi

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'cases=%s\n' "$(echo "$cases" | wc -w)"
printf 'oracle=ffmpeg software transpose (half-turn as hflip,vflip) byte-identical for every direction\n'
echo "PASS va-vpp-transpose directions=3 formats=2 exact driver=$driver_hash"
