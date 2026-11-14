#!/bin/sh

set -eu
root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
work=$(mktemp -d "$scratch_base/iris-experiment-guard.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15
mkdir -p "$work/thermal/thermal_zone0" "$work/power/qcom-battmgr-bat" \
    "$work/power/qcom-battmgr-usb" "$work/empty-power"
printf 'test\n' >"$work/thermal/thermal_zone0/type"
printf '42000\n' >"$work/thermal/thermal_zone0/temp"
printf '19\n' >"$work/power/qcom-battmgr-bat/capacity"
printf 'Discharging\n' >"$work/power/qcom-battmgr-bat/status"
printf '3800000\n' >"$work/power/qcom-battmgr-bat/voltage_now"
printf -- '-2100000\n' >"$work/power/qcom-battmgr-bat/current_now"
printf '0\n' >"$work/power/qcom-battmgr-usb/online"
mkdir -p "$work/class/backlight/ktz8866-backlight" "$work/class/graphics/fb0" \
    "$work/class/drm/card0-DSI-1" "$work/net/docker0" "$work/net/wlp1s0"
printf '1474\n' >"$work/class/backlight/ktz8866-backlight/brightness"
printf '2047\n' >"$work/class/backlight/ktz8866-backlight/max_brightness"
printf '4\n' >"$work/class/graphics/fb0/blank"
printf 'connected\n' >"$work/class/drm/card0-DSI-1/status"
printf 'disabled\n' >"$work/class/drm/card0-DSI-1/enabled"
printf 'Off\n' >"$work/class/drm/card0-DSI-1/dpms"
printf '3048x2032\n3048x2032\n' >"$work/class/drm/card0-DSI-1/modes"
printf 'down\n' >"$work/net/docker0/operstate"
printf 'up\n' >"$work/net/wlp1s0/operstate"

IRIS_EXPERIMENT_DIR="$work/artifacts" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/thermal" \
IRIS_EXPERIMENT_BATTERY_ROOT="$work/power" \
IRIS_EXPERIMENT_DISPLAY_ROOT="$work/class" \
IRIS_EXPERIMENT_NET_ROOT="$work/net" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null
grep -Fxq 'status=complete' "$work/artifacts/guard.state"
grep -q '^boot_id=' "$work/artifacts/guard.state"
test -s "$work/artifacts/temperature-initial.csv"
grep -q '^timestamp,gauge,capacity_percent,status,voltage_uv,current_ua,usb_online$' \
    "$work/artifacts/battery.log"
grep -q 'qcom-battmgr-bat,19,Discharging,3800000,-2100000,0' \
    "$work/artifacts/battery.log"
# The display boundary a power run is measured under: connector state, the
# preferred mode string, brightness and the fbcon blank level.
grep -q '^timestamp,status,enabled,dpms,mode,brightness,max_brightness,blank$' \
    "$work/artifacts/display.log"
grep -q ',connected,disabled,Off,3048x2032,1474,2047,4$' "$work/artifacts/display.log"
grep -q '^timestamp,interfaces$' "$work/artifacts/network.log"
grep -q ',docker0=down;wlp1s0=up$' "$work/artifacts/network.log"
# The argv is the run's own identity; one argument per line keeps quoting.
printf 'sh\n-c\nexit 0\n' >"$work/expected-command.txt"
cmp -s "$work/expected-command.txt" "$work/artifacts/command.txt"
# A low, discharging pack is recorded, not refused: this is the same 19 %
# state with the guard still reaching complete.
test "$(grep -c 'status=complete' "$work/artifacts/guard.state")" -eq 1

# The opposite pack state must also stay a recorded condition, not a gate.
printf '100\n' >"$work/power/qcom-battmgr-bat/capacity"
printf 'Charging\n' >"$work/power/qcom-battmgr-bat/status"
printf '1\n' >"$work/power/qcom-battmgr-usb/online"
IRIS_EXPERIMENT_DIR="$work/charging" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/thermal" \
IRIS_EXPERIMENT_BATTERY_ROOT="$work/power" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null
grep -Fxq 'status=complete' "$work/charging/guard.state"
grep -q 'qcom-battmgr-bat,100,Charging,3800000,-2100000,1' \
    "$work/charging/battery.log"

# A machine with no readable gauge writes unknown rather than silently
# claiming a healthy pack; boot/thermal guards are unchanged.
IRIS_EXPERIMENT_DIR="$work/no-gauge" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/thermal" \
IRIS_EXPERIMENT_BATTERY_ROOT="$work/empty-power" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null
grep -Fxq 'status=complete' "$work/no-gauge/guard.state"
grep -q ',unknown,unknown,unknown,unknown,unknown,unknown$' \
    "$work/no-gauge/battery.log"

# A machine with no connector, backlight or network tree records unknown
# rather than failing a run that the thermal and boot guards accept.
IRIS_EXPERIMENT_DIR="$work/no-display" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/thermal" \
IRIS_EXPERIMENT_BATTERY_ROOT="$work/power" \
IRIS_EXPERIMENT_DISPLAY_ROOT="$work/absent-class" \
IRIS_EXPERIMENT_NET_ROOT="$work/absent-net" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null
grep -Fxq 'status=complete' "$work/no-display/guard.state"
grep -q ',unknown,unknown,unknown,unknown,unknown,unknown,unknown$' \
    "$work/no-display/display.log"
grep -q ',unknown$' "$work/no-display/network.log"

# The temperature ceiling is a runaway backstop, not a comfort line: a zone
# in the firmware's own throttle band (90 C here, above the old 85000 default)
# must not cost the run, while a zone past the default still stops it.
mkdir -p "$work/hot/thermal_zone0" "$work/too-hot/thermal_zone0"
printf 'test\n' >"$work/hot/thermal_zone0/type"
printf '90000\n' >"$work/hot/thermal_zone0/temp"
printf 'test\n' >"$work/too-hot/thermal_zone0/type"
printf '101000\n' >"$work/too-hot/thermal_zone0/temp"
IRIS_EXPERIMENT_DIR="$work/hot-run" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/hot" \
IRIS_EXPERIMENT_BATTERY_ROOT="$work/power" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'sleep 2' >/dev/null
grep -Fxq 'status=complete' "$work/hot-run/guard.state"
grep -q '^max_temperature_millic=100000$' "$work/hot-run/guard.state"

if IRIS_EXPERIMENT_DIR="$work/too-hot-run" \
    IRIS_EXPERIMENT_THERMAL_ROOT="$work/too-hot" \
    IRIS_EXPERIMENT_BATTERY_ROOT="$work/power" \
    IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
    IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
    sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null 2>&1; then
    echo 'FAIL the guard accepted a zone past its ceiling' >&2
    exit 1
fi

echo 'PASS experiment guard completion and evidence recording'
