#!/bin/sh

# Hardware gate for the 8K geometry. The node advertises 96x96 - 8192x8192,
# but 7680x4320 NV12 is 47.5 MiB per CAPTURE frame: a fixed 32-frame pool asks
# the device for 1.5 GiB and STREAMON fails with ENOMEM. The session sizes the
# pool against a memory budget; this gate proves the budget actually lets all
# four qualified codecs decode at 8K, byte for byte, with a terminal LAST. The
# budget and the pool cap follow the module: the stock contiguous carve-out
# grants 8 frames, the capture_sg module parameter raises the budget to 720 MiB
# and the cap to 15.
#
# The driver is chosen by the caller's environment, which this gate inherits
# untouched. Leave LIBVA_DRIVERS_PATH unset to test the installed package, or
# name a build tree and set LIBVA_DRIVER_NAME=v4l2 so libva loads that driver
# instead of the installed alias. The gate never unsets the driver name itself:
# doing so made a build-tree run fail to initialise libva.
#
# Usage:
#   IRIS_8K_DIR=DIR sh test/iris-8k-gate.sh
#
# The directory holds one finite 8K clip per codec and its software framemd5
# baseline: h264.mkv + h264-sw.md5, hevc.mkv + hevc-sw.md5,
# vp9.webm + vp9-sw.md5, av1.webm + av1-sw.md5. Generate the baselines once
# with a software decoder; a broken VA driver must not bless its own output.
# Every clip must be 7680x4320.
#
# Environment:
#   IRIS_8K_DIR       directory holding the clips and software baselines
#   IRIS_8K_CODECS    "h264 hevc vp9 av1" by default
#   IRIS_8K_PUBLISH   publish override; empty (default) leaves the driver's
#                     own policy in place
#   IRIS_8K_OUT       artifact directory (default <lab>/artifacts/8k-gate)
#   IRIS_RENDER_NODE  DRM render node (default /dev/dri/renderD128)

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
. "$here/lib/iris-env.sh"

clip_dir=${IRIS_8K_DIR:-}
codecs=${IRIS_8K_CODECS:-h264 hevc vp9 av1}
publish=${IRIS_8K_PUBLISH:-}
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
out_base=${IRIS_8K_OUT:-$(iris_artifact_dir 8k-gate)}

fail() {
    echo "FAIL 8k-gate: $*" >&2
    exit 1
}

[ -n "$clip_dir" ] || fail 'IRIS_8K_DIR must name the directory holding 8K clips and software framemd5 files'
[ -d "$clip_dir" ] || fail "clip directory missing: $clip_dir"
[ -e "$render" ] || fail "render node missing: $render"
command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'
command -v ffprobe >/dev/null 2>&1 || fail 'ffprobe is required'

mkdir -p "$out_base"
failures=0
set_publish=''
[ -z "$publish" ] || set_publish="V4L2_VA_PUBLISH=$publish"

run_clip() {
    name=$1
    clip=$2
    [ -r "$clip_dir/$clip" ] || { echo "FAIL $name: clip missing: $clip_dir/$clip" >&2; return 1; }
    [ -r "$clip_dir/$name-sw.md5" ] || {
        echo "FAIL $name: software baseline missing: $clip_dir/$name-sw.md5" >&2
        return 1
    }
    geometry=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$clip_dir/$clip")
    [ "$geometry" = "7680,4320" ] || {
        echo "FAIL $name: clip is $geometry, expected 7680,4320" >&2
        return 1
    }
    sw_frames=$(grep -c '^0,' "$clip_dir/$name-sw.md5" || true)
    [ "$sw_frames" -gt 0 ] || { echo "FAIL $name: software baseline has no frames" >&2; return 1; }

    out="$out_base/$name"
    mkdir -p "$out"
    trace="$out/driver.trace"
    md5="$out/va.md5"

    # shellcheck disable=SC2086
    if ! env V4L2_VA_TRACE=1 $set_publish \
        ffmpeg -y -hide_banner -loglevel error -vaapi_device "$render" \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$clip_dir/$clip" \
        -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$md5" \
        2>"$trace"; then
        echo "FAIL $name: ffmpeg VA decode failed" >&2
        return 1
    fi

    # The pool that was granted must be the memory-budgeted one. A 32-frame
    # pool at this geometry is the failure this gate exists to catch, whether
    # or not STREAMON happened to succeed on a particular boot. The budget is
    # 384 MiB of contiguous capture memory (eight 8K frames), or 720 MiB when
    # the patched module allocates CAPTURE from system memory (capture_sg=1,
    # fifteen frames, the Direct headroom). The cap is read from the module
    # parameter because that is what the driver read.
    pool_cap=8
    if [ "$(cat /sys/module/qcom_iris/parameters/capture_sg 2>/dev/null || true)" = Y ]; then
        pool_cap=15
    fi
    pool=$(sed -n 's/.*capture configure .*pool=\([0-9]*\) memory=.*/\1/p' "$trace" | head -1)
    [ -n "$pool" ] || { echo "FAIL $name: no capture configure record in $trace" >&2; return 1; }
    [ "$pool" -le "$pool_cap" ] || {
        echo "FAIL $name: CAPTURE pool=$pool exceeds the 8K memory budget ($pool_cap)" >&2
        return 1
    }
    firmware_min=$(sed -n 's/.*firmware-min=\([0-9]*\).*/\1/p' "$trace" | head -1)
    [ -n "$firmware_min" ] || { echo "FAIL $name: no firmware-min in $trace" >&2; return 1; }
    [ "$pool" -ge "$firmware_min" ] || {
        echo "FAIL $name: CAPTURE pool=$pool is below the firmware minimum $firmware_min" >&2
        return 1
    }
    printf 'PASS %s geometry=7680x4320 pool=%s firmware-min=%s\n' "$name" "$pool" "$firmware_min"

    # Every frame hash must equal the software decode, in order, with no
    # missing frames. framemd5 headers carry the tool version, so compare
    # frame lines only.
    if ! awk '
        FNR == NR { if (/^0,/) a[++n] = $0; next }
        /^0,/ { if (++m > n || $0 != a[m]) bad = 1 }
        END { if (m != n || bad) exit 1 }
    ' "$md5" "$clip_dir/$name-sw.md5"; then
        echo "FAIL $name: framemd5 differs from the software baseline" >&2
        return 1
    fi
    printf 'PASS %s frames=%s framemd5=exact\n' "$name" "$sw_frames"

    # A finite clip drains: one terminal LAST and one completed drain. No
    # dropped, errored or misplaced completion, no failed session.
    errors=$(grep -Ec 'publish=(error|drop|misplaced)|capture error index=|sync timeout token=|session failed|undefined symbol' \
        "$trace" || true)
    [ "$errors" -eq 0 ] || { echo "FAIL $name: $errors completion error(s) in $trace" >&2; return 1; }
    grep -q 'capture last' "$trace" || { echo "FAIL $name: no terminal LAST" >&2; return 1; }
    grep -q 'drain complete last=1' "$trace" || { echo "FAIL $name: no completed drain" >&2; return 1; }
    printf 'PASS %s errors=0 last=1 drain=1\n' "$name"
}

for codec in $codecs; do
    case $codec in
        h264) clip=h264.mkv ;;
        hevc) clip=hevc.mkv ;;
        vp9)  clip=vp9.webm ;;
        av1)  clip=av1.webm ;;
        *) fail "unsupported codec: $codec" ;;
    esac
    run_clip "$codec" "$clip" || failures=$((failures + 1))
done

[ "$failures" -eq 0 ] || fail "$failures codec(s) failed"
echo "PASS 8k-gate codecs='$codecs' publish=${publish:-auto}"
