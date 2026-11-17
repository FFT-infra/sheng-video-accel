#!/bin/sh

# Battery-side power of a decode, this driver against software.
#
# The tablet's only energy source userspace can read is the fuel gauge at
# /sys/class/power_supply/qcom-battmgr-bat: voltage_now x |current_now|. It
# measures the whole device (screen, SoC, radio), so every number here is an
# increment over a pre/post baseline, never decoder-package power. There is no
# per-block telemetry on this SoC.
#
# One round is: 10 s pre-baseline, N s of the workload, 10 s post-baseline,
# sampled at 1 Hz. The VA and software arms alternate every round, so a slow
# drift in background power cancels instead of loading one arm. The first 4 s
# of each window are dropped: a decode ramps CPU and GPU for a moment before
# it settles.
#
# Run on the target, with the screen off. A lit desktop costs more than the
# decode itself and makes the gauge noise larger than the signal.
#
# Usage:
#   test/iris-power-sampler.sh [--clip PATH] [--seconds N] [--rounds N]
#                              [--driver PATH] [--out DIR]
#   test/iris-power-sampler.sh --workload encode --codec h264|hevc
#                              [--size WxH] [--bitrate B] [--preset P] ...
#
# Options:
#   --clip PATH       Elementary H.264 stream to decode (required for the
#                     decode workload).
#   --workload W      decode (default) or encode.
#   --codec C         h264 or hevc, for the encode workload (default h264).
#   --size WxH        encode source size (default 1920x1080).
#   --bitrate B       encode target bitrate (default 4M).
#   --preset P        software encode preset (default veryfast).
#   --seconds N       Sample each arm for N seconds (default 20).
#   --rounds N        Rounds, one VA and one software arm each (default 4).
#   --driver PATH     VA driver under test (default: the installed one).
#   --out DIR         Artifact directory (default: lab artifacts).
#
# The encode workload re-encodes a synthetic source back to back in each arm:
# `testsrc2` through `h264_vaapi`/`hevc_vaapi`, and the same source through
# libx264/libx265 at the same size, rate, GOP and bitrate. It answers what a
# decode-only measurement cannot: whether hardware encoding saves energy at
# all, and how much of the difference is the client CPU.
#
# The workload re-decodes the clip back to back, so the sampling window is
# always full. A paced `-re` run leaves the device idle most of the time and
# its increment is smaller than the gauge noise; do not read a paced result
# as a decode-power measurement.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/iris-env.sh
. "$here/lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

battery=/sys/class/power_supply/qcom-battmgr-bat
backlight=/sys/class/backlight/ktz8866-backlight
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
clip=''
seconds=20
rounds=4
driver=''
out=''
workload=decode
codec=h264
size=1920x1080
bitrate=4M
preset=veryfast

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clip) clip=${2:?}; shift 2 ;;
        --seconds) seconds=${2:?}; shift 2 ;;
        --rounds) rounds=${2:?}; shift 2 ;;
        --driver) driver=${2:?}; shift 2 ;;
        --out) out=${2:?}; shift 2 ;;
        --workload) workload=${2:?}; shift 2 ;;
        --codec) codec=${2:?}; shift 2 ;;
        --size) size=${2:?}; shift 2 ;;
        --bitrate) bitrate=${2:?}; shift 2 ;;
        --preset) preset=${2:?}; shift 2 ;;
        -h|--help) sed -n '3,42p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

case "$workload" in
    decode)
        [ -n "$clip" ] || fail '--clip is required'
        [ -r "$clip" ] || fail "clip not found: $clip"
        ;;
    encode)
        case "$codec" in
            h264) sw_codec=x264 ;;
            hevc) sw_codec=x265 ;;
            *) fail "--codec must be h264 or hevc" ;;
        esac
        # A missing software encoder exits the loop on its first iteration and
        # leaves the gauge sampling an idle device, which reads as a hardware
        # win. Check it before the first round, not after the last one.
        ffmpeg -hide_banner -encoders 2>/dev/null | grep -q " lib$sw_codec " ||
            fail "ffmpeg has no lib$sw_codec; the software arm cannot run"
        ;;
    *) fail "--workload must be decode or encode" ;;
esac
[ -r "$battery/voltage_now" ] && [ -r "$battery/current_now" ] || fail "no battery gauge at $battery"

status=$(cat "$battery/status" 2>/dev/null || echo unknown)
case "$status" in
    Charging|Full) fail "battery is $status; unplug before measuring" ;;
esac

if [ -r "$backlight/brightness" ] && [ "$(cat "$backlight/brightness")" != 0 ]; then
    echo "WARN backlight is $(cat "$backlight/brightness"); screen-off numbers need it at 0" >&2
fi

va_env=''
if [ -n "$driver" ]; then
    [ -f "$driver" ] || fail "driver not found: $driver"
    driver=$(CDPATH='' cd -- "$(dirname -- "$driver")" && pwd)/$(basename -- "$driver")
    va_env="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$(dirname "$driver")"
elif [ -n "${LIBVA_DRIVERS_PATH:-}" ]; then
    # The caller chose a build tree through the environment. Name that
    # driver in the record; reporting "installed" would name a library the
    # arms never loaded.
    driver=$LIBVA_DRIVERS_PATH/v4l2_drv_video.so
    [ -f "$driver" ] || fail "LIBVA_DRIVERS_PATH has no v4l2_drv_video.so: $LIBVA_DRIVERS_PATH"
fi
driver_sha=$(sha256sum ${driver:-$(sed -n 's/^driver_path=//p' /usr/local/share/iris-vaapi/install-manifest.txt 2>/dev/null | head -1)} 2>/dev/null | cut -d' ' -f1 || true)
if [ "$workload" = encode ]; then
    node=$(iris_resolve_encoder 2>/dev/null || echo unknown)
else
    node=$(iris_resolve_device 2>/dev/null || echo unknown)
fi
boot_id=$(cat /proc/sys/kernel/random/boot_id)
[ -n "$out" ] || out=$(iris_artifact_dir power)/$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

if [ "$workload" = encode ]; then
    echo "Iris power workload=encode codec=$codec node=$node driver=${driver:-installed} sha256=${driver_sha:-NA} boot=$boot_id size=$size bitrate=$bitrate preset=$preset seconds=$seconds rounds=$rounds"
else
    echo "Iris power clip=$clip node=$node driver=${driver:-installed} sha256=${driver_sha:-NA} boot=$boot_id seconds=$seconds rounds=$rounds"
fi
echo "Iris power artifacts: $out"

sample() {
    uv=$(cat "$battery/voltage_now")
    ua=$(cat "$battery/current_now")
    # current_now is negative while discharging; power is its magnitude.
    case "$ua" in -*) ua=${ua#-} ;; esac
    echo $((uv * ua / 1000000))
}
mean() { awk '{s+=$1;n++} END{if(n) printf "%.0f", s/n; else print 0}' "$1"; }
# The first 4 s of a window are the ramp, not steady state.
steady() { tail -n +5 "$1" | awk '{s+=$1;n++} END{if(n) printf "%.0f", s/n; else print 0}'; }

collect() {
    n=0
    : > "$2"
    while [ "$n" -lt "$1" ]; do
        sample >> "$2"
        n=$((n+1))
        sleep 1
    done
}

arm() {
    tag=$1
    pre=$out/$tag-pre.txt; run=$out/$tag-run.txt; post=$out/$tag-post.txt
    collect 10 "$pre"
    if [ "$workload" = encode ]; then
        # One encode per iteration. -f null discards the stream, so the arm
        # measures the encoder and not the disk. The source is regenerated
        # by lavfi each time, which is the same CPU cost in both arms.
        if [ "$tag" = va ]; then
            # shellcheck disable=SC2086
            env $va_env sh -c '
                while :; do
                    ffmpeg -hide_banner -loglevel error -vaapi_device "$1" \
                        -f lavfi -i "testsrc2=size=$2:rate=30" -frames:v 120 \
                        -vf format=nv12,hwupload -c:v "${3}_vaapi" \
                        -b:v "$4" -g 60 -f null - || break
                done' sh "$render" "$size" "$codec" "$bitrate" > "$out/$tag-workload.log" 2>&1 &
        else
            sh -c '
                while :; do
                    ffmpeg -hide_banner -loglevel error \
                        -f lavfi -i "testsrc2=size=$1:rate=30" -frames:v 120 \
                        -c:v "lib$5" -preset "$3" -b:v "$4" -pix_fmt yuv420p \
                        -g 60 -f null - || break
                done' sh "$size" "$codec" "$preset" "$bitrate" "$sw_codec" > "$out/$tag-workload.log" 2>&1 &
        fi
    elif [ "$tag" = va ]; then
        # shellcheck disable=SC2086
        env $va_env LIBVA_V4L2_VIDEO_PATH="$node" sh -c '
            while :; do
                ffmpeg -hide_banner -loglevel error -hwaccel vaapi \
                    -hwaccel_device "$1" -hwaccel_output_format vaapi \
                    -i "$2" -f null - || break
            done' sh "$render" "$clip" > "$out/$tag-workload.log" 2>&1 &
    else
        sh -c 'while :; do ffmpeg -hide_banner -loglevel error -i "$1" -f null - || break; done' \
            sh "$clip" > "$out/$tag-workload.log" 2>&1 &
    fi
    pid=$!
    collect "$seconds" "$run"
    # A loop that already exited sampled an idle device for part of the
    # window. The increment would then be a small positive number that reads
    # as an efficient arm, so the round is refused instead of averaged.
    if ! kill -0 "$pid" 2>/dev/null; then
        fail "the $tag workload stopped before the window closed: $(head -1 "$out/$tag-workload.log" 2>/dev/null)"
    fi
    # The loop runs one ffmpeg at a time; stop the loop and its current child.
    pkill -P "$pid" 2>/dev/null || true
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    sleep 1
    collect 10 "$post"
    r=$(steady "$run")
    b=$(( ($(mean "$pre") + $(mean "$post")) / 2 ))
    echo $((r - b))
}

: > "$out/ab.txt"
round=0
while [ "$round" -lt "$rounds" ]; do
    round=$((round + 1))
    va=$(arm va)
    sw=$(arm sw)
    printf 'round %s va=%s sw=%s\n' "$round" "$va" "$sw" >> "$out/ab.txt"
    echo "round $round: va=${va}uW sw=${sw}uW"
done

if [ "$workload" = encode ]; then
    label="encode-$codec-$size-$bitrate"
else
    label=$(basename "$clip")
fi
awk -v sz="$label" '/^round /{n++; v[n]=$3; s[n]=$4; sub(/^va=/,"",v[n]); sub(/^sw=/,"",s[n]); sv+=v[n]; ss+=s[n]}
END {
    if (!n) { print "RESULT no rounds"; exit 1 }
    mv=sv/n; ms=ss/n
    # One round is a usable measurement without a spread; an interval needs a
    # second round, and dividing by n-1 without one was a crash after a
    # complete measurement.
    if (n < 2) { printf "RESULT clip=%s n=1 va=%.0fuW sw=%.0fuW va-sw=%.0fuW (one round, no interval)\n", \
        sz, mv, ms, mv-ms; exit }
    for (i=1;i<=n;i++){qv+=(v[i]-mv)^2; qs+=(s[i]-ms)^2}
    # Two-sided 95% t interval; small-n table, n-1 degrees of freedom.
    t = (n==2)?12.706:(n==3)?4.303:(n==4)?3.182:(n==5)?2.776:2.571
    printf "RESULT clip=%s n=%d va=%.0fuW(+/-%.0f) sw=%.0fuW(+/-%.0f) va-sw=%.0fuW\n", \
        sz, n, mv, sqrt(qv/(n-1))*t/sqrt(n), ms, sqrt(qs/(n-1))*t/sqrt(n), mv-ms
}' "$out/ab.txt"
