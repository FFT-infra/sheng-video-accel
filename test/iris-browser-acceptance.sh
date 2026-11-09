#!/bin/sh

# Browser hardware-decode acceptance for one clip on the Iris target.
#
# Launches the browser directly, with no wrapper and no LIBVA_* variable, and
# reports the evidence the release gate requires: the decoder Chrome itself
# selected, the platform flag, frame/drop counts over a window, and an
# independent check that a Chrome process holds the Iris decoder node. "The
# video played" is not a result; software decode plays too.
#
# The direct launch is deliberate. libva resolves this driver through the
# msm_drv_video.so alias, so a client that sets nothing still reaches it; the
# project used to ship a launcher that exported the node path, and this
# script is what proves the launcher is not needed.
#
# Usage:
#   test/iris-browser-acceptance.sh --clip PATH [--browser chrome|chromium]
#       [--driver PATH] [--seconds N] [--out DIR] [--label NAME]
#
# Exit status is 0 only when the selected decoder is VaapiVideoDecoder with
# kIsPlatformVideoDecoder=true, the window decoded frames, and a Chrome
# process held the resolved Iris node during playback.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/iris-env.sh
. "$here/lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
}

clip=''
browser=chrome
driver=''
seconds=30
out=''
label=''
port=9337
while [ "$#" -gt 0 ]; do
    case "$1" in
        --clip) clip=${2:?}; shift 2 ;;
        --browser) browser=${2:?}; shift 2 ;;
        --driver) driver=${2:?}; shift 2 ;;
        --seconds) seconds=${2:?}; shift 2 ;;
        --out) out=${2:?}; shift 2 ;;
        --label) label=${2:?}; shift 2 ;;
        --port) port=${2:?}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; fail "unknown argument: $1" ;;
    esac
done
[ -n "$clip" ] || { usage >&2; fail '--clip is required'; }
[ -f "$clip" ] || fail "clip not found: $clip"
clip=$(CDPATH='' cd -- "$(dirname -- "$clip")" && pwd)/$(basename -- "$clip")
[ -n "$label" ] || label=$(basename "$clip" | sed 's/\.[^.]*$//')
[ -n "$out" ] || out=$(iris_artifact_dir browser-acceptance)/$label-$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

page=$here/playback.html
[ -f "$page" ] || fail "playback page missing: $page"
cdp=$here/remote/cdp.py

node=$(iris_resolve_device) || exit 1
boot_id=$(cat /proc/sys/kernel/random/boot_id)

# Record which driver bytes the GPU process will map. A build-tree override is
# allowed for development but is recorded as such; a release claim needs the
# installed path.
if [ -n "$driver" ]; then
    [ -f "$driver" ] || fail "driver not found: $driver"
    driver=$(CDPATH='' cd -- "$(dirname -- "$driver")" && pwd)/$(basename -- "$driver")
    driver_dir=$(dirname "$driver")
    driver_source=build-tree
else
    driver_dir=$(pkg-config --variable=driverdir libva 2>/dev/null || echo /usr/lib64/dri)
    driver=$driver_dir/v4l2_drv_video.so
    [ -f "$driver" ] || fail "installed driver not found: $driver"
    driver_source=installed
fi
driver_sha=$(sha256sum "$driver" | cut -d' ' -f1)

ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,profile,width,height,r_frame_rate,nb_frames,has_b_frames \
    -of json "$clip" >"$out/clip-probe.json" 2>/dev/null || true

profile_root=$out/profile
rm -rf "$profile_root"
chrome_args="--remote-debugging-port=$port --remote-allow-origins=* --kiosk
    --autoplay-policy=no-user-gesture-required --no-first-run
    --no-default-browser-check --disable-background-networking
    --disable-component-update --no-pings --disable-sync"

case "$browser" in
    chrome) real_browser=$(command -v google-chrome-stable || command -v google-chrome) ;;
    chromium) real_browser=$(command -v chromium || command -v chromium-browser) ;;
    *) fail "unsupported browser family: $browser" ;;
esac
[ -n "$real_browser" ] || fail "no $browser binary on PATH"

# A build-tree run is a development override and is recorded as one. It is the
# only case that sets LIBVA_DRIVERS_PATH; the installed path must reach the
# driver through libva's own resolution, which is the property being tested.
driver_env=''
if [ "$driver_source" = build-tree ]; then
    driver_env="LIBVA_DRIVERS_PATH=$driver_dir"
fi

# Keep every experiment switch out of the environment: this is the default
# path the package ships. LIBVA_DRIVER_NAME and both node overrides are
# dropped so the run cannot inherit them from the session and silently pass
# for the wrong reason. Diagnostics may be enabled by the caller through
# IRIS_ACCEPTANCE_ENV="V4L2_VA_TRACE=1", which is passed through verbatim.
# shellcheck disable=SC2086
env -u V4L2_VA_ZERO_COPY -u V4L2_VA_ZERO_COPY_CONTRACT -u V4L2_VA_BATCH_SIZE \
    -u V4L2_VA_COPY_SURFACES -u V4L2_VA_CAPTURE_SCHEDULED \
    -u LIBVA_DRIVER_NAME -u LIBVA_V4L2_VIDEO_PATH -u LIBVA_V4L2_ENCODER_PATH \
    $driver_env \
    ${IRIS_ACCEPTANCE_ENV:-} \
    "$real_browser" --user-data-dir="$profile_root" \
    --ozone-platform=wayland $chrome_args about:blank \
    >"$out/browser.log" 2>&1 &
browser_pid=$!
trap 'kill "$browser_pid" 2>/dev/null || true; wait "$browser_pid" 2>/dev/null || true' EXIT

# Independent of the browser's self-report: which Chrome process holds the
# Iris node, and which one mapped the driver. Software decode never opens the
# node. Compare the full symlink target so /dev/video1 cannot be mistaken for
# /dev/video11.
#
# This has to sample while the decode is in flight. Sampling after playback
# stops finds a GPU process that has already closed the node and unmapped the
# driver, which is how an earlier revision of this script recorded
# driver_mapped_in_gpu_process=null on a hardware run.
: >"$out/iris-node-holders.txt"
mapped_file=$out/driver-mapped.txt
: >"$mapped_file"
sampler_pid=''
sample() {
    for pid in $(pgrep -f -- "--user-data-dir=$profile_root" 2>/dev/null || true); do
        [ -d "/proc/$pid" ] || continue
        type=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -o -- '--type=[a-z-]*' | head -1)
        for fd in "/proc/$pid/fd"/*; do
            [ -e "$fd" ] || continue
            [ "$(readlink "$fd" 2>/dev/null || true)" = "$node" ] || continue
            printf '%s %s\n' "$pid" "$type" >>"$out/iris-node-holders.txt"
            break
        done
        m=$(grep -o '[^ ]*v4l2_drv_video\.so' "/proc/$pid/maps" 2>/dev/null | sort -u | head -1)
        # An if, not `[ -n ] && printf`: the empty case must not be the
        # function's exit status, or `set -e` kills the sampler on the first
        # second before playback has mapped anything.
        if [ -n "$m" ]; then
            printf '%s %s\n' "$pid" "$m" >>"$mapped_file"
        fi
    done
    return 0
}
# One sample a second for the length of the window, in the background.
(
    n=0
    while [ "$n" -lt "$((seconds + 10))" ]; do
        sample || true
        n=$((n + 1))
        sleep 1
    done
) &
sampler_pid=$!

rc=0
python3 "$cdp" --port "$port" run \
    --url "file://$page?src=file://$clip" \
    --warmup-seconds 5 \
    --measure-seconds "$seconds" \
    --out "$out/cdp.json" >"$out/cdp.log" 2>&1 || rc=$?

kill "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true
# Collapse the per-second samples into one record each, deduplicated.
sort -u "$out/iris-node-holders.txt" -o "$out/iris-node-holders.txt"
sort -u "$mapped_file" -o "$mapped_file"
mapped=''
[ -s "$mapped_file" ] && mapped=$(awk '{print $2; exit}' "$mapped_file")

kill "$browser_pid" 2>/dev/null || true
wait "$browser_pid" 2>/dev/null || true
trap - EXIT

boot_id_after=$(cat /proc/sys/kernel/random/boot_id)

commit=$(git -C "$here" rev-parse HEAD 2>/dev/null || echo unknown)
python3 - "$out" "$node" "$boot_id" "$boot_id_after" "$driver" "$driver_sha" \
    "$driver_source" "$mapped" "$browser" "$label" "$rc" "$commit" <<'PY'
import json, os, subprocess, sys
out, node, boot, boot_after, driver, sha, source, mapped, browser, label, rc, commit = sys.argv[1:13]
report = {}
try:
    with open(os.path.join(out, "cdp.json")) as f:
        report = json.load(f)
except Exception as e:
    report = {"error": f"cdp.json unreadable: {e}"}
holders = open(os.path.join(out, "iris-node-holders.txt")).read().split()
holder_types = [h for h in holders if h.startswith("--type=")]
kernel = subprocess.run(["uname", "-r"], capture_output=True, text=True).stdout.strip()
summary = {
    "schema_version": 1,
    "label": label,
    "source_commit": commit,
    "browser_family": browser,
    "browser": report.get("browser"),
    "kernel": kernel,
    "boot_id": boot,
    "boot_unchanged": boot == boot_after,
    "node": node,
    "driver_path": driver,
    "driver_sha256": sha,
    "driver_source": source,
    "driver_mapped_in_gpu_process": mapped or None,
    "decoder_name": report.get("decoder_name"),
    "is_platform_video_decoder": report.get("is_platform_video_decoder"),
    "window_seconds": report.get("measure_seconds"),
    "window_decoded_frames": report.get("window_decoded_frames"),
    "window_dropped_frames": report.get("window_dropped_frames"),
    "iris_node_holders": holder_types,
    "cdp_exit": int(rc),
}
hw = (summary["decoder_name"] == "VaapiVideoDecoder"
      and summary["is_platform_video_decoder"] is True
      and (summary["window_decoded_frames"] or 0) > 0
      and bool(holder_types)
      and summary["boot_unchanged"])
summary["hardware_decode"] = hw
with open(os.path.join(out, "summary.json"), "w") as f:
    json.dump(summary, f, indent=2, sort_keys=True)
    f.write("\n")
verdict = "PASS" if hw else "FAIL"
print(f"{verdict} {label} browser={summary['browser']} decoder={summary['decoder_name']} "
      f"platform={summary['is_platform_video_decoder']} frames={summary['window_decoded_frames']} "
      f"dropped={summary['window_dropped_frames']} holders={','.join(holder_types) or 'none'} "
      f"driver={source}:{sha[:12]} boot_unchanged={summary['boot_unchanged']}")
print(f"artifacts: {out}")
sys.exit(0 if hw else 1)
PY
