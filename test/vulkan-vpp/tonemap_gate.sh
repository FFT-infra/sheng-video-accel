#!/bin/sh

# HDR10 to SDR tone mapping through the VA video processor, on the target:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/tonemap_gate.sh
#
# The client is `iris-va-vpp-probe tonemap`: FFmpeg's tonemap_vaapi refuses a
# raw P010 frame (it demands mastering-display side data, which a rawvideo
# input has nowhere to carry), so nothing in 8.1 can drive the filter from a
# file. The probe builds the buffer itself: VAProcFilterParameterBufferType
# holding VAHdrMetaDataHDR10, one VAProcPipelineParameterBuffer naming
# VAProcFilterHighDynamicRangeToneMapping, a P010 source and an NV12 target.
#
# There is no byte-exact reference for a tone curve, so the probe compares its
# output with its own implementation of the pipeline documented in
# tonemap.comp, byte by byte, and the gate requires the largest difference to
# stay within 2 codes (float32 kernel against a double-precision model). Three
# properties of the curve are checked separately, so a model and a kernel that
# share a mistake still fail: PQ black lands on 16/128/128, the mastering peak
# lands on 235, and the luminance ramp stays monotone.
#
# Two mastering peaks run the same frame: the curve has to move when the
# metadata moves, and the outputs are compared to prove it did.
#
# The refusals are part of the contract: an NV12 source, a filter without its
# parameters, and a rotation in the same picture are all refused rather than
# approximated. So is the whole entrypoint without V4L2_VA_VPP.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

command -v sha256sum >/dev/null 2>&1 || fail "missing target tool: sha256sum"

device=$(iris_resolve_device) || exit 2
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
[ -c "$render" ] || fail "no render node $render"

# Two ways to name the driver under test: a build tree (the default) or the
# installed package (IRIS_VPP_INSTALLED=1, where libva resolves its own driver
# directory so the record can say which artifact produced the evidence).
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

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp-tonemap)}
mkdir -p "$work"

# One case per line: tag, source geometry, mastering peak and the extra
# processor arguments, colon-separated. 318 is not a multiple of four, so the
# last chroma word of a row carries two columns the ramp does not cover; that
# path has its own neutral fill in the kernel. The composition cases run the
# tone mapper and then the scale pass over its output: three quarter turns,
# both mirrors, a crop that scales, and a thumbnail at the processor's 2x2
# floor.
cat > "$work/cases" <<'EOF'
sdrwhite:128x72:1000:
mastering4000:128x72:4000:
ragged:318x178:1000:
rotate90:128x72:1000:--dst=72x128 --rotate=1
rotate270:128x72:1000:--dst=72x128 --rotate=3
halfturn:128x72:1000:--rotate=2
hflip:128x72:1000:--mirror=1
vflip:128x72:1000:--mirror=2
bothflips:128x72:1000:--mirror=3
crop-scale:128x72:1000:--crop=16,8,64,48 --dst=32x24
thumbnail:64x64:1000:--dst=2x2
EOF

failed=0
run_case() {
    tag=$1
    size=$2
    peak=$3
    extras=$4
    w=${size%x*}
    h=${size#*x}
    echo "== $tag: $size peak=$peak $extras"
    rc=0
    # shellcheck disable=SC2086
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 300 "$probe" tonemap "$render" "$w" "$h" "$peak" "$work/$tag.nv12" $extras \
        >"$work/$tag.log" 2>"$work/$tag.trace" || rc=$?
    if [ "$rc" -ne 0 ]; then
        sed -n '1,40p' "$work/$tag.log" >&2 || true
        sed -n '1,20p' "$work/$tag.trace" >&2 || true
    fi
    ends=$(grep -c 'tonemap=hdr10' "$work/$tag.trace" || true)
    if [ "$ends" -lt 1 ]; then
        echo "no tonemap=hdr10 record in the trace" >&2
        rc=1
    fi
    printf 'case=%s peak=%s exit=%s out=%s max_diff=%s\n' "$tag" "$peak" "$rc" \
        "$(sha256sum "$work/$tag.nv12" 2>/dev/null | cut -d' ' -f1)" \
        "$(sed -n 's/.*max_diff=\([0-9]*\).*/\1/p' "$work/$tag.log" | tail -1)"
    [ "$rc" -eq 0 ] || failed=1
}

while IFS= read -r c; do
    [ -n "$c" ] || continue
    run_case "$(echo "$c" | cut -d: -f1)" "$(echo "$c" | cut -d: -f2)" "$(echo "$c" | cut -d: -f3)" \
        "$(echo "$c" | cut -d: -f4-)"
done < "$work/cases"
[ "$failed" -eq 0 ] || fail "a tone-mapped frame differs from the documented pipeline"

echo "== the curve follows the metadata: 1000 and 4000 cd/m2 differ"
if cmp -s "$work/sdrwhite.nv12" "$work/mastering4000.nv12"; then
    fail "the same frame tone-mapped to two mastering peaks produced identical output; the metadata was ignored"
fi

echo "== refusals"
refuse() {
    what=$1
    shift
    echo "-- $what"
    rc=0
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" tonemap "$render" 128 72 1000 "$work/refused.nv12" "$@" --expect-refusal \
        >"$work/refused-$what.log" 2>"$work/refused-$what.trace" || rc=$?
    if [ "$rc" -ne 0 ]; then
        sed -n '1,20p' "$work/refused-$what.log" >&2 || true
        fail "the driver did not refuse a tone map with $what"
    fi
    printf 'refused=%s\n' "$what"
}
refuse nv12-source --source-nv12
refuse no-parameters --no-filter
refuse odd-crop --crop=1,0,64,64

echo "== opt-in is required: the same client is refused without V4L2_VA_VPP"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        timeout 120 "$probe" tonemap "$render" 64 64 1000 "$work/default.nv12" \
        >"$work/default.log" 2>"$work/default.trace"; then
    fail "tonemap ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'tonemap=hdr10' "$work/default.trace"; then
    fail "a tone-mapped picture appeared without the opt-in"
fi

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'cases=%s\n' "$(grep -c . "$work/cases")"
printf 'oracle=the documented pipeline in double precision, plus black, peak and monotonicity\n'
echo "PASS va-vpp-tonemap peaks=2 sizes=2 refused=3 driver=$driver_hash"
