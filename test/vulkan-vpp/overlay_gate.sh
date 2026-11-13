#!/bin/sh

# Global-alpha compositing through the VA video processor, on the target:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/overlay_gate.sh
#
# The client is `iris-va-vpp-probe overlay`, which submits what FFmpeg's
# overlay_vaapi submits: a main pass over the whole output, then the overlay
# surface into one rectangle with a VABlendState naming a global alpha. Unlike
# the tone mapper there is an exact reference for the mix, so this gate
# compares every output byte with the probe's own double-precision model and
# separately with the input frames themselves:
#
#   alpha 0 over the whole surface   == the main frame, byte for byte
#   alpha 1 over the whole surface   == the overlay frame, byte for byte
#
# A kernel that copied the wrong plane, transposed a sample or ignored the
# alpha fails one of those three comparisons, not just the tolerance.
#
# Two more shapes run the same mix. A premultiplied source stores its colours
# already scaled by the global alpha, so only the main surface is scaled by
# (1 - alpha); the gate proves that by running the same picture twice, once
# with a premultiplied frame and once with the straight one, and requiring the
# two outputs to agree. A packed BGRA/RGBA overlay is a surface of a different
# format: the kernel converts each sampled pixel to the output's Y'CbCr matrix
# before mixing, and the model in the probe does the same conversion, so a
# swapped channel, a wrong byte order or a wrong matrix fails as a pixel
# difference. The straight-alpha premultiplied equivalence is the check that
# does not come from the model.
#
# The refusals are part of the contract: luma keying, a third pass, a blend
# before the pass it blends onto, an odd or out-of-bounds rectangle, a crop of
# the main surface and an alpha outside 0..1 are all refused rather than
# approximated.
# So is the whole entrypoint without V4L2_VA_VPP.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

command -v sha256sum >/dev/null 2>&1 || fail "missing target tool: sha256sum"

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

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp-overlay)}
mkdir -p "$work"

run_probe() {
    tag=$1
    shift
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 300 "$probe" overlay "$render" "$@" >"$work/$tag.log" 2>"$work/$tag.trace"
}

# Main size, overlay size, rectangle, alpha, then any probe flags; one case
# per line so a case may name more than one flag.
#   half    128x72 main, a same-size overlay at half alpha: the mix everywhere
#   opaque  160x96 main, 64x48 overlay at 1:1 into a 64x48 rectangle
#   scale   128x72 main, a 64x48 overlay halved into 32x24, and an alpha that
#           is not a dyadic fraction, so the mix rounds a tie in float32
#   p010    the 10-bit path, overlay and output both P010
#   ragged  318x178: a row whose last destination word carries two columns the
#           output does not cover
#   padded  the main surface 16 columns wider than the picture, as a decoder
#           allocates it, so the main pass copies a rectangle rather than the
#           whole surface
#   tonemap the main pass is an HDR10 P010 surface the tone mapper converts
#           into the NV12 the overlay is mixed into
#   premult*, rgbpremult  the stored-alpha form of the same mix, on both bit
#           depths and on the packed overlay
#   rgb, rgba  a packed BGRA and a packed RGBA overlay, so the two byte orders
#           are each read the way VA names them
#   rgb601  the same composite with the BT.601 matrix the picture names
#   rgbtonemap  a packed overlay onto a tone-mapped main surface
cat > "$work/cases" <<'EOF'
half:128x72:128x72:0:0:128:72:0.50
opaque:160x96:64x48:16:16:64:48:1.00
scale:128x72:64x48:34:12:32:24:0.60
p010:128x72:32x24:64:48:32:24:0.50:--p010
ragged:318x178:64x48:100:50:64:48:0.25
padded:128x72:64x48:16:16:64:48:0.35:--pad=16
tonemap:128x72:64x48:32:8:64:48:0.50:--tonemap=1000
premult:128x72:64x48:16:16:64:48:0.50:--premultiplied
premult-p010:128x72:32x24:64:48:32:24:0.50:--p010 --premultiplied
rgb:128x72:64x48:32:8:64:48:0.50:--rgb
rgba:128x72:64x48:32:8:64:48:0.50:--rgb=rgba
rgbpremult:128x72:64x48:32:8:64:48:0.50:--rgb --premultiplied
rgbp010:128x72:32x24:64:48:32:24:0.50:--rgb --p010
rgb601:128x72:64x48:32:8:64:48:0.50:--rgb --standard=601
rgbtonemap:128x72:64x48:32:8:64:48:0.50:--rgb --tonemap=1000
EOF

failed=0
count=0
while IFS= read -r entry; do
    [ -n "$entry" ] || continue
    tag=${entry%%:*}
    spec=${entry#*:}
    # The fields are positional and colon-separated; everything after the
    # alpha is a probe flag, one or more of them. Globbing is off so a flag
    # that contains `=` or `*` is not expanded on the way in.
    set -f
    set -- $(printf '%s\n' "$spec" | tr ':' ' ')
    set +f
    w=${1%x*} h=${1#*x}
    ow=${2%x*} oh=${2#*x}
    rx=$3 ry=$4 rw=$5 rh=$6 alpha=$7
    shift 7
    flags="$*"
    count=$((count + 1))
    echo "== $tag ${w}x${h} overlay ${ow}x${oh} rect ${rw}x${rh}+$rx,$ry alpha=$alpha $flags"
    rc=0
    run_probe "$tag" "$w" "$h" "$ow" "$oh" "$rx" "$ry" "$rw" "$rh" "$alpha" "$work/$tag.raw" $flags || rc=$?
    if [ "$rc" -ne 0 ]; then
        sed -n '1,40p' "$work/$tag.log" >&2 || true
        sed -n '1,20p' "$work/$tag.trace" >&2 || true
    fi
    ends=$(grep -c 'overlay=' "$work/$tag.trace" || true)
    if [ "$ends" -lt 1 ]; then
        echo "no overlay= record in the trace" >&2
        rc=1
    fi
    printf 'case=%s rect=%sx%s+%s,%s alpha=%s exit=%s out=%s max_diff=%s\n' "$tag" "$rw" "$rh" "$rx" "$ry" \
        "$alpha" "$rc" "$(sha256sum "$work/$tag.raw" 2>/dev/null | cut -d' ' -f1)" \
        "$(sed -n 's/.*max_diff=\([0-9]*\).*/\1/p' "$work/$tag.log" | tail -1)"
    [ "$rc" -eq 0 ] || failed=1
done < "$work/cases"
[ "$failed" -eq 0 ] || fail "a composited frame differs from the documented mix"

echo "== the endpoints are the input frames, not a plausible approximation"
# A full-surface composite at alpha 0 is the main frame and at alpha 1 is the
# overlay frame, byte for byte: the kernel's copy path and its sample path
# each have to reproduce an input exactly.
run_probe copy 128 72 128 72 0 0 128 72 0.00 "$work/copy.raw" || fail "alpha 0 composite failed"
if ! cmp -s "$work/copy.raw" "$work/copy.raw.main"; then
    fail "alpha 0 did not reproduce the main frame"
fi
run_probe over 128 72 128 72 0 0 128 72 1.00 "$work/over.raw" || fail "alpha 1 composite failed"
if ! cmp -s "$work/over.raw" "$work/over.raw.overlay"; then
    fail "alpha 1 did not reproduce the overlay frame"
fi
# And the half-alpha case is neither of them.
if cmp -s "$work/half.raw" "$work/half.raw.main" || cmp -s "$work/half.raw" "$work/half.raw.overlay"; then
    fail "alpha 0.5 produced one of its inputs; the mix did not happen"
fi
echo "copy=byte-exact over=byte-exact half=neither-input"

# A premultiplied frame is the straight frame's colours already scaled by the
# global alpha, so the two runs have to land on the same picture. This is the
# one composite check whose answer does not come from the probe's model.
echo "== stored alpha is the same picture as straight alpha"
run_probe premult-straight 128 72 64 48 32 8 64 48 0.50 "$work/premult-straight.raw" --rgb \
    || fail "straight-alpha composite failed"
if ! cmp -s "$work/rgbpremult.raw" "$work/premult-straight.raw"; then
    differ=$(cmp -l "$work/rgbpremult.raw" "$work/premult-straight.raw" | awk '
        function oct(s, i, c, v) { v = 0; for (i = 1; i <= length(s); i++) { c = substr(s, i, 1); v = v * 8 + c } return v }
        { d = oct($3) - oct($2); if (d < 0) d = -d; if (d > m) m = d }
        END { print m + 0 }')
    [ "$differ" -le 2 ] || fail "a premultiplied overlay differs from the same picture at straight alpha by $differ codes"
    echo "premultiplied=straight-within-$differ"
else
    echo "premultiplied=straight-byte-exact"
fi

# A packed overlay that never reached the kernel would leave the main frame in
# place, which is what an alpha of 0 looks like; the two have to differ inside
# the rectangle.
if cmp -s "$work/rgb.raw" "$work/rgb.raw.main"; then
    fail "the packed overlay did not change the output; the frame is the main surface"
fi

echo "== refusals"
refuse() {
    what=$1
    shift
    echo "-- $what"
    # The probe takes nine positional fields and then its flags, with the
    # output path between them; the call sites name the flags last, so split
    # the two here instead of relying on their order.
    fields=''
    flags=''
    n=0
    for argument in "$@"; do
        n=$((n + 1))
        if [ "$n" -le 9 ]; then
            fields="$fields $argument"
        else
            flags="$flags $argument"
        fi
    done
    rc=0
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 "$probe" overlay "$render" $fields "$work/refused.raw" $flags --expect-refusal \
        >"$work/refused-$what.log" 2>"$work/refused-$what.trace" || rc=$?
    if [ "$rc" -ne 0 ]; then
        sed -n '1,20p' "$work/refused-$what.log" >&2 || true
        fail "the driver did not refuse a composite with $what"
    fi
    printf 'refused=%s\n' "$what"
}
refuse luma-key 128 72 64 48 16 16 64 48 0.50 --luma-key
refuse third-pass 128 72 64 48 16 16 64 48 0.50 --third
refuse blend-first 128 72 64 48 16 16 64 48 0.50 --blend-first
refuse outside 128 72 64 48 112 16 32 48 0.50
refuse odd-region 128 72 64 48 17 16 32 48 0.50
refuse main-crop 128 72 64 48 16 16 64 48 0.50 --main-crop
refuse alpha-range 128 72 64 48 16 16 64 48 1.50

echo "== opt-in is required: the same client is refused without V4L2_VA_VPP"
rc=0
env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
    timeout 120 "$probe" overlay "$render" 64 64 32 32 16 16 32 32 0.50 "$work/default.raw" \
    >"$work/default.log" 2>"$work/default.trace" || rc=$?
if [ "$rc" -eq 0 ]; then
    fail "a composite ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'overlay=' "$work/default.trace"; then
    fail "a composited picture appeared without the opt-in"
fi

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'cases=%s\n' "$count"
printf 'oracle=the documented mix in double precision, both endpoint frames byte for byte, and straight against stored alpha\n'
echo "PASS va-vpp-overlay cases=$count endpoints=2 refused=7 driver=$driver_hash"
