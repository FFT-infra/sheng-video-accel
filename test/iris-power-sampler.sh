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
#
# Options:
#   --clip PATH       Elementary H.264 stream to decode (required).
#   --seconds N       Sample each arm for N seconds (default 20).
#   --rounds N        Rounds, one VA and one software arm each (default 4).
#   --driver PATH     VA driver under test (default: the installed one).
#   --out DIR         Artifact directory (default: lab artifacts).
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

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clip) clip=${2:?}; shift 2 ;;
        --seconds) seconds=${2:?}; shift 2 ;;
        --rounds) rounds=${2:?}; shift 2 ;;
        --driver) driver=${2:?}; shift 2 ;;
        --out) out=${2:?}; shift 2 ;;
        -h|--help) sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

[ -n "$clip" ] || fail '--clip is required'
[ -r "$clip" ] || fail "clip not found: $clip"
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
fi
driver_sha=$(sha256sum ${driver:-$(sed -n 's/^driver_path=//p' /usr/local/share/iris-vaapi/install-manifest.txt 2>/dev/null | head -1)} 2>/dev/null | cut -d' ' -f1 || true)
node=$(iris_resolve_device 2>/dev/null || echo unknown)
boot_id=$(cat /proc/sys/kernel/random/boot_id)
[ -n "$out" ] || out=$(iris_artifact_dir power)/$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

echo "Iris power clip=$clip node=$node driver=${driver:-installed} sha256=${driver_sha:-NA} boot=$boot_id seconds=$seconds rounds=$rounds"
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
    if [ "$tag" = va ]; then
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

awk -v sz="$(basename "$clip")" '/^round /{n++; v[n]=$3; s[n]=$4; sub(/^va=/,"",v[n]); sub(/^sw=/,"",s[n]); sv+=v[n]; ss+=s[n]}
END {
    if (!n) { print "RESULT no rounds"; exit }
    mv=sv/n; ms=ss/n
    for (i=1;i<=n;i++){qv+=(v[i]-mv)^2; qs+=(s[i]-ms)^2}
    # Two-sided 95% t interval; small-n table, n-1 degrees of freedom.
    t = (n==2)?12.706:(n==3)?4.303:(n==4)?3.182:(n==5)?2.776:2.571
    printf "RESULT clip=%s n=%d va=%.0fuW(+/-%.0f) sw=%.0fuW(+/-%.0f) va-sw=%.0fuW\n", \
        sz, n, mv, sqrt(qv/(n-1))*t/sqrt(n), ms, sqrt(qs/(n-1))*t/sqrt(n), mv-ms
}' "$out/ab.txt"
