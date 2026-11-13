#!/bin/sh

# The packed-overlay path through the real client, on the target:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/overlay_vaapi_gate.sh
#
# overlay_gate.sh drives a probe that submits what overlay_vaapi submits. This
# gate drives overlay_vaapi itself, because two things about the real client
# are invisible from the probe:
#
#   - FFmpeg sets VA_BLEND_PREMULTIPLIED_ALPHA whenever its overlay input
#     carries an alpha channel (vf_overlay_vaapi.c: have_alpha_planar), so a
#     BGRA overlay always arrives premultiplied. A driver that accepts only
#     the global-alpha flag refuses the picture the client actually builds.
#   - the overlay surface is a VA_RT_FORMAT_RGB32 surface FFmpeg uploads a
#     BGRA frame into, so the RGB32 surface, image and blend paths all run.
#
# The oracle does not come from the driver or from FFmpeg. Both frames are a
# single colour, so `out = overlay + (1 - alpha) * main` on the normalised
# BT.709 codes is one number per plane, computed here in awk:
#
#   alpha 0, a black overlay   every byte equals the main frame (cmp)
#   alpha 1                   every byte equals the BT.709 codes of the
#                             overlay colour, which is the conversion itself
#   alpha 0.25                the mix, from the main frame's own codes
#
# The entrypoint stays opt-in: the same graph must be refused without
# V4L2_VA_VPP.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

fail() { echo "FAIL $*" >&2; exit 1; }

for tool in ffmpeg sha256sum od awk; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing target tool: $tool"
done
filter_list=$(ffmpeg -hide_banner -filters 2>/dev/null || true)
for filter in overlay_vaapi hwupload hwdownload; do
    printf '%s\n' "$filter_list" | grep -Eq "[[:space:]]$filter[[:space:]]" \
        || fail "ffmpeg has no $filter filter (needs --enable-vaapi)"
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

work=${IRIS_VPP_DIR:-$(iris_artifact_dir va-vpp-overlay-vaapi)}
mkdir -p "$work"

width=128
height=72
ow=64
oh=48

# One plane's codes as the BT.709 limited-range triple, rounded the way the
# kernel rounds (floor(v + 0.5)).
yuv_codes() {
    awk -v r="$1" -v g="$2" -v b="$3" 'BEGIN {
        kr = 0.2126; kb = 0.0722; kg = 1 - kr - kb;
        y = kr * r / 255 + kg * g / 255 + kb * b / 255;
        printf "%d %d %d", int(16 + 219 * y + 0.5),
            int(128 + 224 * ((b / 255) - y) / (2 * (1 - kb)) + 0.5),
            int(128 + 224 * ((r / 255) - y) / (2 * (1 - kr)) + 0.5);
    }'
}

# The stored-alpha mix in the order overlay_rgb.comp documents: convert the
# overlay colour to normalised Y'/Cb'/Cr', scale the main frame's own code by
# (1 - alpha), add, and encode once at the end. Plane 0 is luma, 1 is Cb and
# 2 is Cr; n and s are that plane's neutral code and span.
mix_norm() {
    awk -v r="$1" -v g="$2" -v b="$3" -v plane="$4" -v m="$5" -v n="$6" -v s="$7" -v a="$8" 'BEGIN {
        kr = 0.2126; kb = 0.0722; kg = 1 - kr - kb;
        r /= 255; g /= 255; b /= 255;
        y = kr * r + kg * g + kb * b;
        if (plane == 0)
            o = y;
        else if (plane == 1)
            o = (b - y) / (2 * (1 - kb));
        else
            o = (r - y) / (2 * (1 - kr));
        v = o + (1 - a) * ((m - n) / s);
        printf "%d", int(n + s * v + 0.5);
    }'
}

# The largest difference between a downloaded frame and one constant code per
# plane, over every byte of both planes.
frame_error() {
    od -An -tu1 -v "$1" | awk -v w="$2" -v h="$3" -v ey="$4" -v eu="$5" -v ev="$6" '
        { for (i = 1; i <= NF; i++) b[n++] = $i }
        END {
            max = 0;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    d = b[y * w + x] - ey; if (d < 0) d = -d; if (d > max) max = d;
                }
            base = w * h;
            for (cy = 0; cy < h / 2; cy++)
                for (cx = 0; cx < w / 2; cx++) {
                    d = b[base + cy * w + cx * 2] - eu; if (d < 0) d = -d; if (d > max) max = d;
                    d = b[base + cy * w + cx * 2 + 1] - ev; if (d < 0) d = -d; if (d > max) max = d;
                }
            print max;
        }'
}

# The code at one sample of a packed NV12 frame.
code_at() {
    od -An -tu1 -j "$2" -N 1 -v "$1" | tr -d ' '
}

band() {
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=$1:s=${width}x${height}:d=1" \
        -frames:v 1 -pix_fmt nv12 -f rawvideo "$2"
}

overlay_band() {
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "color=c=$1:s=${ow}x${oh}:d=1" \
        -frames:v 1 -pix_fmt bgra -f rawvideo "$2"
}

# One composite through overlay_vaapi, downloaded as the main frame's format.
# `alpha` is the filter's global alpha; the rectangle covers the whole output.
composite() {
    alpha=$1
    overlay=$2
    out=$3
    trace=$4
    env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_VPP=1 V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error \
        -vaapi_device "$render" \
        -f rawvideo -pix_fmt nv12 -s "${width}x${height}" -i "$work/main.nv12" \
        -f rawvideo -pix_fmt bgra -s "${ow}x${oh}" -i "$overlay" \
        -filter_complex "[0:v]format=nv12,hwupload=extra_hw_frames=4[m];[1:v]format=bgra,hwupload=extra_hw_frames=4[o];[m][o]overlay_vaapi=x=0:y=0:w=${width}:h=${height}:alpha=${alpha}[v];[v]hwdownload,format=nv12[out]" \
        -map "[out]" -frames:v 1 -f rawvideo "$out" 2>"$trace"
}

band 0x4070b0 "$work/main.nv12"
overlay_band black "$work/ovl-black.bgra"
overlay_band red "$work/ovl-red.bgra"
ls -l "$work/main.nv12" "$work/ovl-black.bgra" "$work/ovl-red.bgra" >/dev/null

main_y=$(code_at "$work/main.nv12" 0)
main_cb=$(code_at "$work/main.nv12" $((width * height)))
main_cr=$(code_at "$work/main.nv12" $((width * height + 1)))
printf 'main=luma %s chroma %s,%s\n' "$main_y" "$main_cb" "$main_cr"

failed=0

echo "== alpha 0 with a black overlay is the main frame, byte for byte"
rc=0
composite 0.0 "$work/ovl-black.bgra" "$work/alpha0.nv12" "$work/alpha0.trace" || rc=$?
if [ "$rc" -ne 0 ]; then
    sed -n '1,40p' "$work/alpha0.trace" >&2 || true
    failed=1
fi
if ! grep -q 'premultiplied' "$work/alpha0.trace"; then
    echo "the client did not ask for premultiplied alpha; the gate is not testing what it claims" >&2
    failed=1
fi
if ! grep -q 'source=bgra' "$work/alpha0.trace"; then
    echo "no packed-overlay record in the trace" >&2
    failed=1
fi
if [ "$rc" -eq 0 ]; then
    cmp "$work/alpha0.nv12" "$work/main.nv12" || failed=1
    printf 'alpha0=main-bytes exit=%s\n' "$rc"
fi

echo "== alpha 1 is the overlay colour's BT.709 codes"
# The colour is the frame's own first pixel rather than a number written here:
# lavfi builds its band in yuv420p, so `color=c=red` reaches the driver as
# (254, 0, 0). Every pixel has to match that first one, or the one-number
# oracle below is not about this frame at all.
set -- $(od -An -tu1 -v "$work/ovl-red.bgra" | awk '
    { for (i = 1; i <= NF; i++) v[n++] = $i }
    END {
        uniform = 1;
        for (i = 0; i < n; i++) if (v[i] != v[i % 4]) uniform = 0;
        printf "%d %d %d %d", v[2], v[1], v[0], uniform;
    }')
ov_r=$1 ov_g=$2 ov_b=$3 uniform=$4
[ "$uniform" -eq 1 ] || fail "the overlay band is not one colour; the gate's oracle assumes it is"
set -- $(yuv_codes "$ov_r" "$ov_g" "$ov_b")
red_y=$1 red_cb=$2 red_cr=$3
printf 'overlay=rgb(%s,%s,%s) bt709=%s,%s,%s\n' "$ov_r" "$ov_g" "$ov_b" "$red_y" "$red_cb" "$red_cr"
rc=0
composite 1.0 "$work/ovl-red.bgra" "$work/alpha1.nv12" "$work/alpha1.trace" || rc=$?
if [ "$rc" -ne 0 ]; then
    sed -n '1,40p' "$work/alpha1.trace" >&2 || true
    failed=1
else
    err=$(frame_error "$work/alpha1.nv12" "$width" "$height" "$red_y" "$red_cb" "$red_cr")
    [ "$err" -eq 0 ] || failed=1
    printf 'alpha1=max_diff=%s\n' "$err"
fi

echo "== alpha 0.25 is the documented stored-alpha mix of the two frames"
rc=0
composite 0.25 "$work/ovl-red.bgra" "$work/alpha25.nv12" "$work/alpha25.trace" || rc=$?
if [ "$rc" -ne 0 ]; then
    sed -n '1,40p' "$work/alpha25.trace" >&2 || true
    failed=1
else
    want_y=$(mix_norm "$ov_r" "$ov_g" "$ov_b" 0 "$main_y" 16 219 0.25)
    want_cb=$(mix_norm "$ov_r" "$ov_g" "$ov_b" 1 "$main_cb" 128 224 0.25)
    want_cr=$(mix_norm "$ov_r" "$ov_g" "$ov_b" 2 "$main_cr" 128 224 0.25)
    err=$(frame_error "$work/alpha25.nv12" "$width" "$height" "$want_y" "$want_cb" "$want_cr")
    [ "$err" -eq 0 ] || failed=1
    printf 'alpha25=expected %s,%s,%s max_diff=%s\n' "$want_y" "$want_cb" "$want_cr" "$err"
fi

echo "== the overlay actually changed the frame"
if cmp -s "$work/alpha1.nv12" "$work/main.nv12"; then
    echo "alpha 1 left the main frame in place; the overlay never ran" >&2
    failed=1
fi
[ "$failed" -eq 0 ] || fail "overlay_vaapi did not produce the documented composite"

echo "== opt-in is required: the same graph is refused without V4L2_VA_VPP"
if env LIBVA_DRIVER_NAME=v4l2 $driver_env LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        timeout 120 ffmpeg -y -xerror -hide_banner -loglevel error -vaapi_device "$render" \
        -f rawvideo -pix_fmt nv12 -s "${width}x${height}" -i "$work/main.nv12" \
        -f rawvideo -pix_fmt bgra -s "${ow}x${oh}" -i "$work/ovl-red.bgra" \
        -filter_complex "[0:v]format=nv12,hwupload[m];[1:v]format=bgra,hwupload[o];[m][o]overlay_vaapi=x=0:y=0:w=${width}:h=${height}:alpha=1.0[v];[v]hwdownload,format=nv12[out]" \
        -map "[out]" -frames:v 1 -f rawvideo "$work/default.nv12" 2>"$work/default.trace"; then
    fail "overlay_vaapi ran without V4L2_VA_VPP; the entrypoint must stay opt-in"
fi
if grep -q 'entrypoint=videoproc' "$work/default.trace"; then
    fail "videoproc appeared without the opt-in"
fi

printf 'driver_path=%s\n' "$driver_path"
printf 'driver_source=%s\n' "$([ -n "$driver_env" ] && echo build-tree || echo installed-package)"
printf 'driver_sha256=%s\n' "$driver_hash"
printf 'decoder=%s\n' "$device"
printf 'cases=3\n'
printf 'oracle=BT.709 limited-range codes of the overlay colour, the main frame byte for byte at alpha 0, and the stored-alpha mix in awk\n'
echo "PASS va-vpp-overlay-vaapi cases=3 client=ffmpeg driver=$driver_hash"
