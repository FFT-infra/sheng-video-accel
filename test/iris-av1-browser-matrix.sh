#!/bin/sh

# Complete AV1 browser hardware-decode matrix on the Iris target.
#
# One clip per invocation, two browsers (Firefox via WebDriver BiDi, Chrome via
# CDP), plus the driver's own V4L2_VA_TRACE ledger. A browser decode counts as
# hardware only when the media surface reports the selected decoder and the
# window reports decoded/dropped frames. No LIBVA_*, publish or node override:
# the installed package is what is being tested.
#
# Usage:
#   IRIS_BROWSER_CLIP=FILE IRIS_BROWSER_CLIPS_DIR=DIR \
#       sh test/iris-av1-browser-matrix.sh [--local-only | --sites-only]
#
# IRIS_BROWSER_CLIPS_DIR is the directory holding the generated clips for the
# per-clip sweep; a single --clip run can pass just IRIS_BROWSER_CLIP. Local
# playback uses test/playback.html. Live sites need the local relay on
# 127.0.0.1:8899 (scripts/iris-experiment-guard.sh does not provide it).
#
# Artifacts land under IRIS_BROWSER_OUT or iris_artifact_dir(browser-av1).

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$here/lib/iris-env.sh"

clip=${IRIS_BROWSER_CLIP:-}
clips_dir=${IRIS_BROWSER_CLIPS_DIR:-}
out_base=${IRIS_BROWSER_OUT:-$(iris_artifact_dir browser-av1)}
seconds=${IRIS_BROWSER_SECONDS:-30}
mode=${1:-all}
# The usage line spells the modes with a leading `--`; accept both forms so
# the documented invocation works without a second spelling to remember.
mode=${mode#--}
case $mode in all|local-only|sites-only) ;; *) echo "FAIL unknown mode: $mode" >&2; exit 2 ;; esac
[ -n "$clip" ] || [ -n "$clips_dir" ] || { echo 'FAIL IRIS_BROWSER_CLIP or IRIS_BROWSER_CLIPS_DIR is required' >&2; exit 2; }

command -v firefox >/dev/null 2>&1 || { echo 'FAIL firefox is required' >&2; exit 2; }
command -v google-chrome >/dev/null 2>&1 || { echo 'FAIL google-chrome is required' >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo 'FAIL python3 is required' >&2; exit 2; }
[ -f "$here/playback.html" ] || { echo 'FAIL test/playback.html missing' >&2; exit 2; }

node=$(iris_resolve_device) || exit 1
boot_before=$(cat /proc/sys/kernel/random/boot_id)
driver=$(pkg-config --variable=driverdir libva 2>/dev/null || echo /usr/lib64/dri)
driver_sha=$(sha256sum "$driver/v4l2_drv_video.so" | cut -d' ' -f1)
mkdir -p "$out_base"

# Wayland is mandatory on this tablet. The browser is launched directly so no
# launcher can inject a node or publish override.
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}

port_base=${IRIS_BROWSER_PORT_BASE:-9500}
run_index=0
cases=0

firefox_profile() {
    profile=$1
    cat >"$profile/user.js" <<'EOF'
user_pref("media.hardware-video-decoding.force-enabled", true);
user_pref("media.hardware-video-decoding.enabled", true);
user_pref("media.ffvpx-hw.enabled", true);
user_pref("media.rdd-ffmpeg.enabled", true);
user_pref("media.autoplay.default", 0);
user_pref("media.autoplay.blocking_policy", 0);
user_pref("media.volume.scale", "0");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("browser.shell.checkDefaultBrowser", false);
EOF
    case ${FF_AV1_ENABLED:-true} in false) printf 'user_pref("media.av1.enabled", false);\n' >>"$profile/user.js" ;; esac
    case ${IRIS_BROWSER_PROXY:-} in 1)
        cat >>"$profile/user.js" <<'EOF'
user_pref("network.proxy.type", 1);
user_pref("network.proxy.http", "127.0.0.1");
user_pref("network.proxy.http_port", 8899);
user_pref("network.proxy.ssl", "127.0.0.1");
user_pref("network.proxy.ssl_port", 8899);
user_pref("network.proxy.no_proxies_on", "");
EOF
        ;;
    esac
}

# Scan every Firefox process by cmdline during playback. Child processes drop
# -profile from argv, so matching the profile path alone misses the RDD/utility
# process that actually holds /dev/videoN.
sample_holders() {
    out=$1
    for d in /proc/[0-9]*; do
        [ -r "$d/cmdline" ] || continue
        c=$(tr '\0' ' ' <"$d/cmdline" 2>/dev/null || true)
        case "$c" in *firefox*) ;; *) continue ;; esac
        p=${d#/proc/}
        t=$(printf '%s' "$c" | grep -o -- '--type=[a-z-]*' | head -1)
        for fd in "$d"/fd/*; do
            [ -e "$fd" ] || continue
            target=$(readlink "$fd" 2>/dev/null || true)
            case "$target" in
                /dev/video*|*v4l2_drv_video.so*|*msm_drv_video.so*) printf '%s %s %s\n' "$p" "$t" "$target" >>"$out/holders.txt" ;;
            esac
        done
    done
}

run_firefox() {
    label=$1
    url=$2
    out=$out_base/$label
    rm -rf "$out"
    mkdir -p "$out"
    profile=$out/profile
    mkdir -p "$profile"
    firefox_profile "$profile"
    port=$((port_base + run_index)); run_index=$((run_index + 1))
    log=$out/moz.log
    V4L2_VA_TRACE=1 MOZ_ENABLE_WAYLAND=1 \
    MOZ_LOG='PlatformDecoderModule:5,FFmpegVideo:5,MediaFormatReader:4,MediaDecoderStateMachine:3' \
    MOZ_LOG_FILE=$log MOZ_DISABLE_RDD_SANDBOX=1 \
    env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH -u LIBVA_V4L2_VIDEO_PATH \
        -u LIBVA_V4L2_ENCODER_PATH -u V4L2_VA_PUBLISH -u V4L2_VA_ZERO_COPY \
        -u V4L2_VA_COPY_SURFACES -u V4L2_VA_SYNC_TIMEOUT_MS \
        firefox -profile "$profile" --no-remote --kiosk \
        --remote-debugging-port="$port" --remote-allow-origins='*' "$url" \
        >"$out/browser.log" 2>&1 &
    pid=$!
    cleanup() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
    trap cleanup EXIT
    : >"$out/holders.txt"
    ( for i in $(seq 1 80); do sample_holders "$out"; sleep 1; done ) & sampler=$!
    sleep 12
    python3 -I "$here/iris-firefox-media.py" "$port" "$seconds" "$out/media.json" \
        >"$out/media.stdout" 2>&1 || : >"$out/FAILED"
    kill "$sampler" 2>/dev/null || true
    cleanup; trap - EXIT
    python3 -I - "$out" "$label" <<'PY' || : >"$out/FAILED"
import json, os, sys
out, label = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(os.path.join(out, "media.json")))
except Exception as e:
    print(f"FAIL {label}: no BiDi report ({e})"); sys.exit(1)
dec, drop = d.get("window_decoded_frames"), d.get("window_dropped_frames")
first, second = d.get("stats_first") or {}, d.get("stats_second") or {}
import time
hw = False
# MOZ_LOG child files are flushed lazily; allow the media decoder's init line
# to land before deciding the run was software.
for _ in range(20):
    logs = "".join(open(os.path.join(out, f), errors="replace").read()
                   for f in os.listdir(out) if f.endswith(".moz_log"))
    if "VA-API FFmpeg init successful" in logs or "IsHardwareAccelerated=true" in logs:
        hw = True
        break
    time.sleep(0.5)
trace = open(os.path.join(out, "browser.log"), errors="replace").read()
direct = trace.count("publish=direct")
imports = trace.count("publish=import")
published = direct + imports
errors = (trace.count("publish=error") + trace.count("publish=misplaced")
          + trace.count("publish=drop") + trace.count("session failed"))
print(f"  firefox {label}: decoded={dec} dropped={drop} first={first.get('decoded')} second={second.get('decoded')} hw_log={hw} direct={direct} import={imports} errors={errors}")
# MOZ_LOG is process-wide; a VA-API init line alone does not prove this player's
# decode. Require that this process published the window's frames through the
# installed driver with no driver-side error, misplacement or drop.
if not isinstance(dec, int) or dec <= 0 or not hw or published < dec or errors > 0:
    print(f"FAIL {label}: no hardware AV1 window or driver error"); sys.exit(1)
print(f"PASS firefox {label}")
PY
    printf '  holders: %s\n' "$(sort -u "$out/holders.txt" | head -3 | tr '\n' ' ')"
}

run_chrome() {
    label=$1
    url=$2
    out=$out_base/$label
    rm -rf "$out"
    mkdir -p "$out"
    profile=$out/profile
    mkdir -p "$profile"
    port=$((port_base + run_index)); run_index=$((run_index + 1))
    proxy=''
    case ${IRIS_BROWSER_PROXY:-} in 1) proxy='--proxy-server=http://127.0.0.1:8899' ;; esac
    V4L2_VA_TRACE=1 env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH \
        -u LIBVA_V4L2_VIDEO_PATH -u LIBVA_V4L2_ENCODER_PATH -u V4L2_VA_PUBLISH \
        -u V4L2_VA_ZERO_COPY -u V4L2_VA_COPY_SURFACES \
        google-chrome --user-data-dir="$profile" --ozone-platform=wayland \
        --remote-debugging-port="$port" --remote-allow-origins='*' --kiosk \
        --autoplay-policy=no-user-gesture-required --no-first-run \
        --no-default-browser-check --disable-component-update --no-pings \
        --disable-sync --disable-background-networking \
        --password-store=basic --use-mock-keychain $proxy "$url" \
        >"$out/browser.log" 2>&1 &
    pid=$!
    cleanup() { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
    trap cleanup EXIT
    sleep 12
    python3 -I "$here/iris-chrome-media.py" "$port" "$seconds" "$out/media.json" \
        >"$out/media.stdout" 2>&1 || : >"$out/FAILED"
    cleanup; trap - EXIT
    python3 -I - "$out" "$label" <<'PY' || : >"$out/FAILED"
import json, os, sys
out, label = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(os.path.join(out, "media.json")))
except Exception as e:
    print(f"FAIL {label}: no CDP report ({e})"); sys.exit(1)
dec, drop = d.get("window_decoded_frames"), d.get("window_dropped_frames")
name, platform = d.get("decoder_name"), d.get("is_platform_video_decoder")
print(f"  chrome {label}: decoded={dec} dropped={drop} decoder={name} platform={platform}")
if name != "VaapiVideoDecoder" or platform is not True or not isinstance(dec, int) or dec <= 0:
    print(f"FAIL {label}: no VaapiVideoDecoder window"); sys.exit(1)
print(f"PASS chrome {label}")
PY
}

run_pair() {
    name=$1
    clip_path=$2
    url="file://$here/playback.html?src=file://$clip_path"
    cases=$((cases + 2))
    run_firefox "ff-$name" "$url"
    run_chrome "chrome-$name" "$url"
}

if [ "$mode" != sites-only ]; then
    if [ -n "$clip" ]; then
        run_pair "$(basename "$clip" | sed 's/\.[^.]*$//')" "$clip"
    fi
    if [ -n "$clips_dir" ]; then
        for f in "$clips_dir"/av1-*.mkv; do
            [ -f "$f" ] || continue
            run_pair "$(basename "$f" .mkv)" "$f"
        done
    fi
    [ "$cases" -gt 0 ] || {
        echo "FAIL local-only mode ran no clips: check IRIS_BROWSER_CLIP and IRIS_BROWSER_CLIPS_DIR" >&2
        exit 2
    }
fi

if [ "$mode" != local-only ]; then
    IRIS_BROWSER_PROXY=1
    cases=$((cases + 5))
    run_firefox ff-youtube 'https://www.youtube.com/watch?v=dQw4w9WgXcQ'
    run_chrome chrome-youtube 'https://www.youtube.com/watch?v=dQw4w9WgXcQ'
    run_firefox ff-bilibili 'https://www.bilibili.com/video/BV1GJ411x7h7/'
    run_chrome chrome-bilibili 'https://www.bilibili.com/video/BV1GJ411x7h7/'
    FF_AV1_ENABLED=false
    run_firefox ff-youtube-noav1 'https://www.youtube.com/watch?v=dQw4w9WgXcQ'
fi

failures=$(find "$out_base" -name FAILED -type f | wc -l | tr -d ' ')
printf 'node=%s driver=%s boot=%s seconds=%s cases=%s failures=%s out=%s\n' \
    "$node" "$driver_sha" "$boot_before" "$seconds" "$cases" "$failures" "$out_base"
[ "$cases" -gt 0 ] || { echo "FAIL no browser cases ran" >&2; exit 1; }
[ "$failures" -eq 0 ] || exit 1
echo "PASS AV1 browser matrix mode=$mode cases=$cases"
