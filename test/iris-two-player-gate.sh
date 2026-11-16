#!/bin/sh

# Hardware gate for two independent players decoding at the same time. The
# concurrency soak qualifies two 640x360 lanes for two hours; this gate fills
# the resolution gap with two 1920x1080 clients, each in its own process, each
# with its own VA display and driver trace. Both clients decode a finite clip
# under real-time pacing, finish, match the software framemd5 baseline byte for
# byte, and show no completion errors in either ledger.
#
# The driver is chosen by the caller's environment. Leave LIBVA_DRIVERS_PATH
# unset to test the installed package, or point it at a build tree.
#
# Usage:
#   IRIS_TWO_PLAYER_CLIP_A=FILE IRIS_TWO_PLAYER_CLIP_B=FILE \
#       sh test/iris-two-player-gate.sh
#
# Both clips must be 1920x1080 H.264. Each needs a software framemd5 baseline
# beside it, named <clip>.sw.md5, generated once by a software decoder.
#
# Environment:
#   IRIS_TWO_PLAYER_CLIP_A/B    two 1920x1080 clips (required)
#   IRIS_TWO_PLAYER_PUBLISH     publish override; empty (default) leaves the
#                               driver's own policy in place
#   IRIS_TWO_PLAYER_OUT         artifact directory
#   IRIS_RENDER_NODE            DRM render node (default /dev/dri/renderD128)

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
. "$here/lib/iris-env.sh"

clip_a=${IRIS_TWO_PLAYER_CLIP_A:-}
clip_b=${IRIS_TWO_PLAYER_CLIP_B:-}
publish=${IRIS_TWO_PLAYER_PUBLISH:-}
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
out=${IRIS_TWO_PLAYER_OUT:-$(iris_artifact_dir two-player-gate)}
# An empty override means the installed package: do not export
# LIBVA_DRIVERS_PATH at all, or libva would load a build tree the caller never
# named. A named override is verified to hold the driver before any player is
# started.
driver_path=${LIBVA_DRIVERS_PATH:-}

fail() {
    echo "FAIL two-player: $*" >&2
    exit 1
}

[ -n "$clip_a" ] && [ -n "$clip_b" ] || fail 'IRIS_TWO_PLAYER_CLIP_A and _B are required'
for clip in "$clip_a" "$clip_b"; do
    [ -r "$clip" ] || fail "clip missing: $clip"
    [ -r "$clip.sw.md5" ] || fail "software baseline missing: $clip.sw.md5"
    geometry=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height \
        -of csv=p=0 "$clip")
    [ "$geometry" = "1920,1080" ] || fail "clip is $geometry, expected 1920,1080: $clip"
done
[ -e "$render" ] || fail "render node missing: $render"
[ -z "$driver_path" ] || [ -f "$driver_path/v4l2_drv_video.so" ] \
    || fail "no v4l2_drv_video.so in $driver_path"
command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required'
command -v ffprobe >/dev/null 2>&1 || fail 'ffprobe is required'

mkdir -p "$out"
set_publish=''
[ -z "$publish" ] || set_publish="V4L2_VA_PUBLISH=$publish"

run_player() {
    lane=$1
    clip=$2
    # With no LIBVA_DRIVERS_PATH the caller is testing the installed package:
    # inherit the environment untouched, so libva resolves the system driver
    # with no override at all. Only a named build tree gets a driver name and
    # path, because that tree has no msm alias to resolve by DRM name.
    if [ -n "$driver_path" ]; then
        set_driver="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_path"
    else
        set_driver=''
    fi
    # shellcheck disable=SC2086
    env $set_driver V4L2_VA_TRACE=1 $set_publish \
        ffmpeg -y -nostdin -hide_banner -loglevel error -vaapi_device "$render" \
        -re -hwaccel vaapi -hwaccel_output_format vaapi -i "$clip" \
        -vf 'hwdownload,format=nv12,format=yuv420p' \
        -f framemd5 "$out/$lane.md5" 2>"$out/$lane.trace"
}

# Two independent processes, started together. Each has its own VA display,
# its own decoder fd and its own trace file, so a cross-process failure lands
# in one ledger and cannot be averaged away by the other.
run_player a "$clip_a" >"$out/a.stdout" 2>&1 &
pid_a=$!
run_player b "$clip_b" >"$out/b.stdout" 2>&1 &
pid_b=$!

rc=0
wait "$pid_a" || rc=1
wait "$pid_b" || rc=1
[ "$rc" -eq 0 ] || { cat "$out/a.stdout" "$out/b.stdout" >&2; fail 'a player exited non-zero'; }

for lane in a b; do
    trace="$out/$lane.trace"
    md5="$out/$lane.md5"
    if [ "$lane" = a ]; then baseline="$clip_a.sw.md5"; else baseline="$clip_b.sw.md5"; fi
    [ -r "$trace" ] || fail "$lane: no driver trace"
    frames=$(grep -c '^0,' "$md5" || true)
    [ "$frames" -gt 0 ] || fail "$lane: no frames decoded"
    grep -q 'session open' "$trace" || fail "$lane: trace does not prove the stateful driver ran"
    errors=$(grep -Ec 'publish=(error|drop|misplaced)|capture error index=|sync timeout token=|session failed|undefined symbol' \
        "$trace" || true)
    [ "$errors" -eq 0 ] || fail "$lane: $errors completion error(s) in $trace"
    if ! awk '
        FNR == NR { if (/^0,/) a[++n] = $0; next }
        /^0,/ { if (++m > n || $0 != a[m]) bad = 1 }
        END { if (m != n || bad) exit 1 }
    ' "$md5" "$baseline"; then
        fail "$lane: framemd5 differs from the software baseline"
    fi
    last=$(grep -c 'capture last' "$trace" || true)
    [ "$last" -gt 0 ] || fail "$lane: no terminal LAST in $trace"
    printf 'PASS %s frames=%s errors=0 framemd5=exact last=%s\n' "$lane" "$frames" "$last"
done

# Two ledgers are only two players if both sessions really overlapped. Each
# trace carries its own process's node and a session-open record; the guard
# wrapper records the wall-clock window in which both ran.
echo "PASS two-player codec=h264 geometry=1920x1080 publish=${publish:-auto}"
