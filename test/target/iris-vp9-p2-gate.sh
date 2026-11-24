#!/bin/sh

# VP9 Profile 2 RT-format and decode gate for the target.
#
# Profile 2 has no legal 8-bit stream: libvpx refuses to encode one and
# src/codec/vp9.cc requires bit_depth == 10. The config therefore advertises
# VA_RT_FORMAT_YUV420_10 only (0x100), and a client that opens the config with
# no RT-format attribute -- FFmpeg does this for a P2 clip -- must still land
# on P010 surfaces. test/unit/va-config-attributes-test.cc pins the attribute
# answer and the createConfig default; this gate proves the real decode path
# still produces P010 frames byte-identical to a software decode.
#
# The clip is generated here from testsrc2, so the gate needs no pinned media
# and no pre-recorded baseline: both arms decode the very same file, and the
# software framemd5 is produced by the target's own software decoder, not by
# this driver. A broken driver cannot bless its own output.
#
# Usage:
#   sh test/target/iris-vp9-p2-gate.sh
# The driver is chosen by the caller's environment. Leave LIBVA_DRIVERS_PATH
# unset to test the installed package, or point it at a build tree. A DESTDIR
# staging run is not an installation: record which one ran.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/../lib/iris-env.sh"

out_base=${IRIS_VP9_P2_OUT:-$(iris_artifact_dir vp9-p2-gate)}
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
driver_path=${LIBVA_DRIVERS_PATH:-}
publish=import
frames=30
size=640x360

[ -e "$render" ] || { echo "FAIL render node missing: $render" >&2; exit 2; }
for tool in ffmpeg ffprobe; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL $tool is required" >&2; exit 2; }
done
if [ -n "$driver_path" ]; then
    [ -f "$driver_path/v4l2_drv_video.so" ] \
        || { echo "FAIL no v4l2_drv_video.so in $driver_path" >&2; exit 2; }
fi

mkdir -p "$out_base"
clip="$out_base/p2-10bit.webm"

run_ffmpeg() {
    if [ -n "$driver_path" ]; then
        env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" "$@"
    else
        env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH "$@"
    fi
}

# Deterministic synthetic source; -frames:v keeps the run short. Profile 2 is
# 10/12-bit only, so yuv420p10le is the shallowest legal sample format. If an
# 8-bit P2 stream ever becomes encodable, this command must fail loudly rather
# than quietly exercise a format we do not advertise.
if ! ffmpeg -y -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=$size:rate=30" -frames:v "$frames" \
        -c:v libvpx-vp9 -profile:v 2 -pix_fmt yuv420p10le -b:v 1M "$clip" \
        2>"$out_base/encode.log"; then
    echo "FAIL cannot generate a 10-bit VP9 Profile 2 clip (see $out_base/encode.log)" >&2
    exit 1
fi
ffprobe -v error -select_streams v:0 \
    -show_entries stream=profile,pix_fmt -of default=nw=1 "$clip" \
    >"$out_base/ffprobe.txt" 2>&1 || {
    echo "FAIL ffprobe could not describe the generated clip" >&2
    exit 1
}
grep -q 'profile=Profile 2' "$out_base/ffprobe.txt" || {
    echo "FAIL generated clip is not VP9 Profile 2" >&2
    exit 1
}
grep -q 'pix_fmt=yuv420p10le' "$out_base/ffprobe.txt" || {
    echo "FAIL generated clip is not 10-bit" >&2
    exit 1
}

# Software baseline, produced by the target's own software VP9 decoder.
ffmpeg -y -hide_banner -loglevel error -i "$clip" \
    -pix_fmt p010le -f framemd5 "$out_base/sw.md5" 2>"$out_base/sw.log" || {
    echo "FAIL software decode failed (see $out_base/sw.log)" >&2
    exit 1
}

# VA decode through the driver under test, with a trace so the session and
# publish contract can be checked. hwdownload yields P010, which -pix_fmt
# keeps unmodified on both arms.
if ! run_ffmpeg V4L2_VA_TRACE=1 V4L2_VA_PUBLISH="$publish" \
        ffmpeg -y -hide_banner -loglevel error -vaapi_device "$render" \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$clip" \
        -vf 'hwdownload,format=p010le' -f framemd5 "$out_base/va.md5" \
        2>"$out_base/driver.trace"; then
    echo "FAIL ffmpeg VA decode failed (see $out_base/driver.trace)" >&2
    exit 1
fi

if ! sh "$here/iris-clip-trace-check.sh" \
        --trace "$out_base/driver.trace" --va-md5 "$out_base/va.md5" \
        --sw-md5 "$out_base/sw.md5" --frames "$frames" \
        --import "$frames" --copy 0 --allow-invisible 0; then
    echo "FAIL VP9 P2 clip contract failed (artifacts in $out_base)" >&2
    exit 1
fi

# A client that passes no RT-format attribute (FFmpeg on a P2 clip) must still
# get P010 surfaces: this is the observable proof that createConfig's default
# is 10-bit only, matching getConfigAttributes' 0x100 answer.
surface_lines=$(grep -c '^va create_surfaces .*fourcc=P010' "$out_base/driver.trace" || true)
[ "$surface_lines" -gt 0 ] || {
    echo "FAIL no P010 surfaces were created; the config default is not 10-bit" >&2
    exit 1
}
nv12_lines=$(grep -c '^va create_surfaces .*fourcc=NV12' "$out_base/driver.trace" || true)
[ "$nv12_lines" -eq 0 ] || {
    echo "FAIL $nv12_lines NV12 surface(s) on a Profile 2 clip" >&2
    exit 1
}

printf 'PASS VP9 Profile 2 gate frames=%s p010_surfaces=%s publish=%s driver=%s\n' \
    "$frames" "$surface_lines" "$publish" "${driver_path:-installed}"
