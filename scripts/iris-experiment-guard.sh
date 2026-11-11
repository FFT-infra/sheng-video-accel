#!/bin/sh

# Run one bounded Iris experiment while recording the boot, thermal and pack
# state. A reboot, missing thermal inventory, or temperature limit breach
# terminates the child and leaves a durable interrupted guard for operator
# review. Battery level is recorded, not enforced: discharge is a valid lab
# condition, and a short run at a low pack is evidence with a boundary, not a
# reason to refuse the run.

set -eu

usage() {
    echo "usage: $0 -- command [args ...]" >&2
    exit 2
}

[ "${1:-}" = -- ] || usage
shift
[ "$#" -gt 0 ] || usage

root=${IRIS_EXPERIMENT_DIR:-${TMPDIR:-/tmp}/iris-experiment}
guard_file=${IRIS_EXPERIMENT_GUARD_FILE:-$root/guard.state}
max_seconds=${IRIS_EXPERIMENT_TIMEOUT_SECONDS:-7200}
interval=${IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS:-30}
max_temperature=${IRIS_EXPERIMENT_MAX_TEMP_MILLIC:-85000}
allow_stale=${IRIS_EXPERIMENT_ALLOW_STALE_GUARD:-0}
thermal_root=${IRIS_EXPERIMENT_THERMAL_ROOT:-/sys/class/thermal}
battery_root=${IRIS_EXPERIMENT_BATTERY_ROOT:-/sys/class/power_supply}
boot_id_file=/proc/sys/kernel/random/boot_id

case "$max_seconds:$interval:$max_temperature" in
    *[!0-9:]*|0:*|*:0:*|*:0)
        echo 'FAIL experiment guard settings must be positive integers' >&2
        exit 2
        ;;
esac
[ -r "$boot_id_file" ] || { echo "FAIL boot-id guard is unavailable: $boot_id_file" >&2; exit 2; }
command -v timeout >/dev/null 2>&1 || { echo 'FAIL experiment guard requires timeout' >&2; exit 2; }

mkdir -p "$root" "$(dirname "$guard_file")"
if [ -f "$guard_file" ] && grep -q '^status=running$' "$guard_file" \
    && [ "$allow_stale" != 1 ]; then
    echo "FAIL previous experiment did not complete; inspect $guard_file (set IRIS_EXPERIMENT_ALLOW_STALE_GUARD=1 only after review)" >&2
    exit 1
fi

temperature_snapshot() {
    timestamp=$1
    found=0
    for zone in "$thermal_root"/thermal_zone*; do
        [ -r "$zone/temp" ] || continue
        value=$(tr -d '\n' <"$zone/temp")
        case "$value" in *[!0-9-]*|'') continue ;; esac
        if [ "$value" -gt -1000 ] && [ "$value" -lt 1000 ]; then value=$((value * 1000)); fi
        found=1
        printf '%s,%s,%s\n' "$timestamp" "$(basename "$zone")" "$value"
    done
    [ "$found" -eq 1 ]
}

# Record the pack state at the start, then sample it with the thermal loop.
# Missing or unreadable gauges are written as "unknown" so the evidence says
# what was measured instead of implying a healthy pack.
battery_snapshot() {
    timestamp=$1
    gauge=''
    for supply in "$battery_root"/*; do
        [ -r "$supply/capacity" ] || continue
        [ -r "$supply/status" ] || continue
        gauge=$supply
        break
    done
    if [ -z "$gauge" ]; then
        printf '%s,unknown,unknown,unknown,unknown,unknown,unknown\n' "$timestamp"
        return 0
    fi
    capacity=$(tr -d '\n' <"$gauge/capacity" 2>/dev/null || printf 'unknown')
    state=$(tr -d '\n' <"$gauge/status" 2>/dev/null || printf 'unknown')
    voltage=$(tr -d '\n' <"$gauge/voltage_now" 2>/dev/null || printf 'unknown')
    current=$(tr -d '\n' <"$gauge/current_now" 2>/dev/null || printf 'unknown')
    online=''
    for supply in "$battery_root"/*; do
        [ -r "$supply/online" ] || continue
        online=$(tr -d '\n' <"$supply/online")
        break
    done
    [ -n "$online" ] || online=unknown
    case "$capacity:$voltage:$current:$online" in
        *[!0-9:.-]*)
            capacity=unknown; voltage=unknown; current=unknown; online=unknown ;;
        *:*:*:*) : ;;
        *) capacity=unknown; voltage=unknown; current=unknown; online=unknown ;;
    esac
    printf '%s,%s,%s,%s,%s,%s,%s\n' \
        "$timestamp" "$(basename "$gauge")" "$capacity" "$state" \
        "$voltage" "$current" "$online"
}

now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
initial_boot_id=$(tr -d '\n' <"$boot_id_file")
if ! temperature_snapshot "$now" >"$root/temperature-initial.csv"; then
    echo 'FAIL no readable thermal zones; experiment guard would be empty' >&2
    exit 2
fi
initial_hottest=$(awk -F, 'BEGIN { max=-999999 } $3+0 > max { max=$3+0 } END { print max }' "$root/temperature-initial.csv")
[ "$initial_hottest" -le "$max_temperature" ] || {
    echo "FAIL initial temperature exceeds limit: $initial_hottest > $max_temperature millidegrees C" >&2
    exit 1
}
battery_snapshot "$now" >"$root/battery-initial.csv"
printf 'timestamp,gauge,capacity_percent,status,voltage_uv,current_ua,usb_online\n' \
    >"$root/battery.log"
cat "$root/battery-initial.csv" >>"$root/battery.log"

started_at=$now
write_guard() {
    status=$1
    finished=${2:-}
    {
        printf 'status=%s\n' "$status"
        printf 'boot_id=%s\n' "$initial_boot_id"
        printf 'timeout_seconds=%s\n' "$max_seconds"
        printf 'max_temperature_millic=%s\n' "$max_temperature"
        printf 'started_at=%s\n' "$started_at"
        [ -z "$finished" ] || printf 'finished_at=%s\n' "$finished"
    } >"$guard_file"
}

pid=''
completed=0
failure=''
stop_child() { [ -z "$pid" ] || kill "$pid" 2>/dev/null || true; }
cleanup() {
    rc=$?
    trap - 0 1 2 15
    stop_child
    if [ "$completed" -eq 1 ]; then
        write_guard complete "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    else
        write_guard interrupted "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        [ "$rc" -ne 0 ] || rc=1
    fi
    exit "$rc"
}
trap cleanup 0 1 2 15

write_guard running
timeout "$max_seconds" "$@" >"$root/command.stdout" 2>"$root/command.stderr" &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
    current_boot_id=$(tr -d '\n' <"$boot_id_file")
    if [ "$current_boot_id" != "$initial_boot_id" ]; then
        failure="boot-id changed from $initial_boot_id to $current_boot_id"
        echo "$failure" >"$root/failure"
        stop_child
        wait "$pid" 2>/dev/null || true
        exit 1
    fi
    stamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    if ! temperature_snapshot "$stamp" >"$root/temperature-current.csv"; then
        failure='thermal zones disappeared during experiment'
        echo "$failure" >"$root/failure"
        stop_child
        wait "$pid" 2>/dev/null || true
        exit 1
    fi
    hottest=$(awk -F, 'BEGIN { max=-999999 } $3+0 > max { max=$3+0 } END { print max }' "$root/temperature-current.csv")
    cat "$root/temperature-current.csv" >>"$root/temperature.log"
    battery_snapshot "$stamp" >"$root/battery-current.csv"
    cat "$root/battery-current.csv" >>"$root/battery.log"
    if [ "$hottest" -gt "$max_temperature" ]; then
        failure="temperature exceeded limit: $hottest > $max_temperature millidegrees C"
        echo "$failure" >"$root/failure"
        stop_child
        wait "$pid" 2>/dev/null || true
        exit 1
    fi
    sleep "$interval"
done
if wait "$pid"; then
    rc=0
else
    rc=$?
fi
[ "$rc" -eq 124 ] && { echo "experiment timeout after ${max_seconds}s" >"$root/failure"; exit 1; }
[ "$rc" -eq 0 ] || exit "$rc"
battery_snapshot "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$root/battery.log"
completed=1
echo "PASS experiment guard boot_id=$initial_boot_id root=$root"
