#!/bin/sh

# Long-running concurrent H.264/HEVC VA decode qualification for Iris. The production
# defaults run each selected scenario for two wall-clock hours. Shorter values
# are accepted for development, but are labelled SHORTENED in the artifacts.
#
# Usage:
#   ./test/iris-concurrency-soak.sh [all|dual-h264|mixed]

set -eu

. "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

scenario=${1:-all}
device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_SOAK_DIR:-$(iris_artifact_dir concurrency-soak)}
duration=${IRIS_SOAK_SECONDS:-7200}
fps=${IRIS_SOAK_FPS:-24}
temperature_interval=${IRIS_SOAK_TEMPERATURE_INTERVAL_SECONDS:-30}
# Same backstop as scripts/iris-experiment-guard.sh: below the zones'
# 110000 millidegree critical trip, not at a comfort line.
max_temperature=${IRIS_SOAK_MAX_TEMP_MILLIC:-100000}
grace=${IRIS_SOAK_EXIT_GRACE_SECONDS:-300}
guard_file=${IRIS_SOAK_GUARD_FILE:-$(dirname "$root")/iris-concurrency-soak.guard}
allow_stale_guard=${IRIS_SOAK_ALLOW_STALE_GUARD:-0}

case "$scenario" in
    all|dual-h264|mixed) ;;
    *) echo "usage: $0 [all|dual-h264|mixed]" >&2; exit 2 ;;
esac
case "$duration:$fps:$temperature_interval:$max_temperature:$grace" in
    *[!0-9:]*|0:*|*:0:*|*:0)
        echo "FAIL soak numeric settings must be positive integers" >&2
        exit 2
        ;;
esac

for tool in ffmpeg v4l2-ctl timeout awk sha256sum ps; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL required soak tool is missing: $tool" >&2
        exit 2
    fi
done
if [ ! -e "$device" ]; then
    echo "FAIL Iris decoder device is missing: $device" >&2
    exit 2
fi

mkdir -p "$root" "$(dirname "$guard_file")"
boot_id_file=/proc/sys/kernel/random/boot_id
if [ ! -r "$boot_id_file" ]; then
    echo "FAIL boot-id guard is unavailable: $boot_id_file" >&2
    exit 2
fi
initial_boot_id=$(tr -d '\n' <"$boot_id_file")
if [ -f "$guard_file" ] && grep -q '^status=running$' "$guard_file" \
    && [ "$allow_stale_guard" != 1 ]; then
    echo "FAIL previous soak did not complete; inspect $guard_file (set IRIS_SOAK_ALLOW_STALE_GUARD=1 only after review)" >&2
    exit 1
fi

temperature_snapshot() {
    timestamp=$1
    found=0
    for zone in /sys/class/thermal/thermal_zone*; do
        [ -r "$zone/temp" ] || continue
        name=$(basename "$zone")
        kind=$(tr '\n,' '__' <"$zone/type" 2>/dev/null || printf unknown)
        value=$(tr -d '\n' <"$zone/temp")
        case "$value" in *[!0-9-]*|'') continue ;; esac
        if [ "$value" -gt -1000 ] && [ "$value" -lt 1000 ]; then
            value=$((value * 1000))
        fi
        found=1
        printf '%s,%s,%s,%s\n' "$timestamp" "$name" "$kind" "$value"
    done
    [ "$found" -eq 1 ]
}

if ! temperature_snapshot "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$root/temperature-initial.csv"; then
    echo "FAIL no readable thermal zones; temperature guard would be empty" >&2
    exit 2
fi
initial_hottest=$(awk -F, 'BEGIN { max=-999999 } $4+0 > max { max=$4+0 } END { print max }' \
    "$root/temperature-initial.csv")
if [ "$initial_hottest" -gt "$max_temperature" ]; then
    echo "FAIL initial temperature exceeds limit: $initial_hottest > $max_temperature millidegrees C" >&2
    exit 1
fi

cat_guard() {
    status=$1
    finished=${2:-}
    {
        printf 'status=%s\n' "$status"
        printf 'boot_id=%s\n' "$initial_boot_id"
        printf 'scenario=%s\n' "$scenario"
        printf 'duration_seconds=%s\n' "$duration"
        printf 'source_commit=%s\n' "${IRIS_SOURCE_COMMIT:-unknown}"
        printf 'started_at=%s\n' "$started_at"
        [ -z "$finished" ] || printf 'finished_at=%s\n' "$finished"
    } >"$guard_file"
}

started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat_guard running
pid_file="$root/worker-pids"
monitor_pid=''
completed=0

terminate_workers() {
    if [ -f "$pid_file" ]; then
        while IFS= read -r pid; do
            case "$pid" in *[!0-9]*|'') continue ;; esac
            terminate_process_tree "$pid"
        done <"$pid_file"
    fi
}

terminate_process_tree() {
    parent=$1
    for child in $(ps -o pid= --ppid "$parent" 2>/dev/null); do
        terminate_process_tree "$child"
    done
    kill "$parent" 2>/dev/null || true
}

cleanup() {
    rc=$?
    trap - 0 1 2 15
    terminate_workers
    [ -z "$monitor_pid" ] || kill "$monitor_pid" 2>/dev/null || true
    if [ "$completed" -eq 1 ]; then
        cat_guard complete "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    else
        cat_guard interrupted "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        [ "$rc" -ne 0 ] || rc=1
    fi
    exit "$rc"
}
trap cleanup 0 1 2 15

formats=$(v4l2-ctl --list-formats-out-ext -d "$device" 2>/dev/null) || {
    echo "FAIL cannot inventory compressed formats on $device" >&2
    exit 1
}
required_formats=H264
if [ "$scenario" = all ] || [ "$scenario" = mixed ]; then
    required_formats='H264 HEVC'
fi
for required in $required_formats; do
    if ! printf '%s\n' "$formats" | grep -q "'$required'"; then
        echo "FAIL required soak format is not advertised: $required" >&2
        exit 1
    fi
done

media_dir="$root/media"
mkdir -p "$media_dir"
frames=48
ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=$fps" \
    -frames:v "$frames" -c:v "${IRIS_H264_ENCODER:-libx264}" -preset ultrafast \
    -tune zerolatency -g "$fps" -bf 0 -pix_fmt yuv420p -an "$media_dir/h264.mp4"
if [ "$scenario" = all ] || [ "$scenario" = mixed ]; then
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=$fps" \
        -frames:v "$frames" -c:v "${IRIS_HEVC_ENCODER:-libx265}" -preset ultrafast \
        -x265-params "keyint=$fps:min-keyint=$fps:scenecut=0" -pix_fmt yuv420p -an \
        "$media_dir/hevc.mkv"
    hevc_soak_frames=$((duration * fps))
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=$fps" \
        -frames:v "$hevc_soak_frames" -c:v "${IRIS_HEVC_ENCODER:-libx265}" -preset ultrafast \
        -x265-params "keyint=$fps:min-keyint=$fps:scenecut=0" -pix_fmt yuv420p -an \
        "$media_dir/hevc-soak.mkv"
fi

va_decode() {
    input=$1
    output=$2
    progress=$3
    trace=$4
    timeout "$((duration + grace))" env \
        LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        ffmpeg -nostdin -y -hide_banner -loglevel warning -nostats -progress "$progress" \
        -stream_loop -1 -re -vaapi_device /dev/dri/renderD128 \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" -t "$duration" \
        -vf 'hwdownload,format=nv12' -f null "$output" 2>"$trace"
}

# HEVC reorder needs a continuous POC/reference timeline. Generate one input
# spanning the requested duration and decode it in a single long-lived context
# instead of repeating independent files without an EOS boundary.
va_decode_once() {
    once_input=$1
    once_output=$2
    once_progress=$3
    once_trace=$4
    timeout "$((duration + grace))" env \
        LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        ffmpeg -nostdin -y -hide_banner -loglevel warning -nostats \
        -progress "$once_progress" -re -vaapi_device /dev/dri/renderD128 \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$once_input" -t "$duration" \
        -vf 'hwdownload,format=nv12' -f null "$once_output" 2>"$once_trace"
}

preflight() {
    codec=$1
    input=$2
    software="$root/preflight-$codec-software.md5"
    hardware="$root/preflight-$codec-va.md5"
    trace="$root/preflight-$codec.trace"
    ffmpeg -y -hide_banner -loglevel error -i "$input" -pix_fmt yuv420p -f framemd5 "$software"
    timeout "$grace" env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
        LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
        ffmpeg -y -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 \
        -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
        -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$hardware" 2>"$trace"
    test/iris-eos-check.sh "$trace" "$frames"
    if ! cmp -s "$software" "$hardware"; then
        echo "FAIL $codec preflight pixels differ from software output" >&2
        exit 1
    fi
    printf 'PASS %s preflight exact-frame-md5\n' "$codec"
}

preflight h264 "$media_dir/h264.mp4"
if [ "$scenario" = all ] || [ "$scenario" = mixed ]; then
    preflight hevc "$media_dir/hevc.mkv"
fi

monitor_environment() {
    log=$1
    failure=$2
    : >"$log"
    while :; do
        now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
        current_boot_id=$(tr -d '\n' <"$boot_id_file")
        uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null || printf unknown)
        printf '%s,boot_id,%s,uptime,%s\n' "$now" "$current_boot_id" "$uptime" >>"$log"
        if [ "$current_boot_id" != "$initial_boot_id" ]; then
            echo "boot-id changed from $initial_boot_id to $current_boot_id" >"$failure"
            terminate_workers
            return 1
        fi
        temperatures="$root/temperature-current.csv"
        if ! temperature_snapshot "$now" >"$temperatures"; then
            echo "thermal zones disappeared during soak" >"$failure"
            terminate_workers
            return 1
        fi
        cat "$temperatures" >>"$log"
        hottest=$(awk -F, 'BEGIN { max=-999999 } $4+0 > max { max=$4+0 } END { print max }' "$temperatures")
        if [ "$hottest" -gt "$max_temperature" ]; then
            echo "temperature exceeded limit: $hottest > $max_temperature millidegrees C" >"$failure"
            terminate_workers
            return 1
        fi
        sleep "$temperature_interval"
    done
}

validate_job() {
    validate_name=$1
    validate_progress="$root/$validate_name.progress"
    validate_trace="$root/$validate_name.trace"
    if ! grep -q '^progress=end$' "$validate_progress"; then
        echo "FAIL $validate_name did not report progress=end" >&2
        return 1
    fi
    validate_frames=$(awk -F= '$1 == "frame" { value=$2 } END { print value + 0 }' "$validate_progress")
    validate_minimum=$((duration * fps * 8 / 10))
    [ "$validate_minimum" -gt 0 ] || validate_minimum=1
    if [ "$validate_frames" -lt "$validate_minimum" ]; then
        echo "FAIL $validate_name frames=$validate_frames minimum=$validate_minimum" >&2
        return 1
    fi
    if ! grep -q 'session open' "$validate_trace"; then
        echo "FAIL $validate_name trace does not prove the stateful driver ran" >&2
        return 1
    fi
    if grep -Eqi 'publish=drop|publish=error|publish=misplaced|capture error index=|sync timeout token=|session failed|undefined symbol' "$validate_trace"; then
        echo "FAIL $validate_name trace contains a stateful decode error" >&2
        return 1
    fi
    test/iris-ending-check.sh "$validate_trace"
    printf 'PASS %s frames=%s duration=%ss\n' "$validate_name" "$validate_frames" "$duration"
}

start_job() {
    job_name=$1
    job_input=$2
    job_status="$root/$job_name.status"
    rm -f "$job_status"
    (
        set +e
        va_decode "$job_input" - "$root/$job_name.progress" "$root/$job_name.trace"
        job_rc=$?
        printf '%s\n' "$job_rc" >"$job_status"
        exit "$job_rc"
    ) &
    printf '%s\n' "$!" >>"$pid_file"
}

start_once_job() {
    once_job_name=$1
    once_job_input=$2
    once_job_status="$root/$once_job_name.status"
    rm -f "$once_job_status"
    (
        set +e
        va_decode_once "$once_job_input" - \
            "$root/$once_job_name.progress" "$root/$once_job_name.trace"
        once_job_rc=$?
        printf '%s\n' "$once_job_rc" >"$once_job_status"
        exit "$once_job_rc"
    ) &
    printf '%s\n' "$!" >>"$pid_file"
}

# Media generation and the preflight decodes run the CPU encoder flat out and
# leave the big cores well above their idle temperature. The thermal guard is
# there to catch heat the *decoder* produces, so let the SoC settle below the
# limit with margin before a scenario's first sample, instead of failing a
# soak on the encoder's tail.
wait_for_cooldown() {
    settle=$((max_temperature - 15000))
    deadline=$(( $(date +%s) + ${IRIS_SOAK_COOLDOWN_MAX_SECONDS:-180} ))
    while :; do
        hottest=$(temperature_snapshot "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            | awk -F, 'BEGIN { max=-999999 } $4+0 > max { max=$4+0 } END { print max }')
        [ "$hottest" -gt "$settle" ] || break
        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "FAIL SoC did not cool below $settle millidegrees C before the scenario (hottest=$hottest)" >&2
            return 1
        fi
        sleep 5
    done
    printf 'cooldown settled hottest=%s limit=%s\n' "$hottest" "$max_temperature"
}

run_scenario() {
    scenario_name=$1
    rm -f "$pid_file" "$root/$scenario_name-environment-failure"
    : >"$pid_file"
    wait_for_cooldown
    case "$scenario_name" in
        dual-h264)
            scenario_jobs='dual-h264-a dual-h264-b'
            scenario_job_count=2
            start_job dual-h264-a "$media_dir/h264.mp4"
            start_job dual-h264-b "$media_dir/h264.mp4"
            ;;
        mixed)
            scenario_jobs='mixed-h264 mixed-hevc'
            scenario_job_count=2
            start_job mixed-h264 "$media_dir/h264.mp4"
            start_once_job mixed-hevc "$media_dir/hevc-soak.mkv"
            ;;
    esac

    monitor_environment "$root/$scenario_name-environment.csv" "$root/$scenario_name-environment-failure" &
    monitor_pid=$!
    scenario_failed=0
    while :; do
        scenario_complete_count=0
        for scenario_job in $scenario_jobs; do
            scenario_status="$root/$scenario_job.status"
            if [ -f "$scenario_status" ]; then
                scenario_complete_count=$((scenario_complete_count + 1))
                scenario_rc=$(sed -n '1p' "$scenario_status")
                if [ "$scenario_rc" -ne 0 ]; then
                    echo "FAIL $scenario_job exited rc=$scenario_rc" >&2
                    scenario_failed=1
                fi
            fi
        done
        if [ "$scenario_failed" -ne 0 ] || [ -f "$root/$scenario_name-environment-failure" ]; then
            terminate_workers
            break
        fi
        [ "$scenario_complete_count" -eq "$scenario_job_count" ] && break
        sleep 2
    done

    while IFS= read -r pid; do
        wait "$pid" || scenario_failed=1
    done <"$pid_file"
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
    monitor_pid=''
    # All recorded children have been reaped. Do not retain their numeric PIDs
    # until the EXIT trap, where a reused PID could name an unrelated process.
    : >"$pid_file"
    if [ -f "$root/$scenario_name-environment-failure" ]; then
        echo "FAIL $scenario_name environment guard: $(cat "$root/$scenario_name-environment-failure")" >&2
        return 1
    fi
    [ "$scenario_failed" -eq 0 ] || return 1
    for scenario_job in $scenario_jobs; do
        validate_job "$scenario_job" || return 1
    done
    printf 'PASS scenario=%s duration=%ss boot_id=%s\n' "$scenario_name" "$duration" "$initial_boot_id"
}

profile=production
[ "$duration" -eq 7200 ] || profile=SHORTENED
echo "Iris concurrency soak scenario=$scenario duration=${duration}s profile=$profile max_temp=${max_temperature}mC root=$root"
if [ "$scenario" = all ] || [ "$scenario" = dual-h264 ]; then
    run_scenario dual-h264
fi
if [ "$scenario" = all ] || [ "$scenario" = mixed ]; then
    run_scenario mixed
fi

final_boot_id=$(tr -d '\n' <"$boot_id_file")
if [ "$final_boot_id" != "$initial_boot_id" ]; then
    echo "FAIL boot-id changed during soak" >&2
    exit 1
fi
completed=1
echo "PASS concurrency-soak scenario=$scenario duration=${duration}s profile=$profile boot_id=$initial_boot_id root=$root"
