#!/bin/sh
# Opt-in target regression: HEVC scaling_list_enabled_flag requires a VA IQ
# buffer even when the original SPS uses the standard default scaling lists.
set -eu
. "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/../lib/iris-env.sh"
device=$(iris_resolve_device)
root=${IRIS_SCALING_DIR:-$(iris_artifact_dir hevc-scaling)}
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
frames=${IRIS_SCALING_FRAMES:-48}
mkdir -p "$root"
ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=320x180:rate=24 \
    -frames:v "$frames" -c:v libx265 -preset ultrafast \
    -x265-params 'scaling-list=default:keyint=24:min-keyint=24:scenecut=0:pools=2:frame-threads=2' \
    -an "$root/scaling.mkv" 2>"$root/encode.log"
ffmpeg -y -xerror -hide_banner -loglevel error -i "$root/scaling.mkv" \
    -pix_fmt nv12 -f framemd5 "$root/software.md5"
if ! timeout 60 env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
    LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
    ffmpeg -y -xerror -hide_banner -loglevel error \
    -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -hwaccel_output_format vaapi \
    -i "$root/scaling.mkv" -vf hwdownload,format=nv12 -f framemd5 "$root/hardware.md5" \
    2>"$root/decode.trace"; then
    echo "FAIL HEVC IQ-matrix decode (see $root/decode.trace)" >&2
    exit 1
fi
actual=$(grep -c '^[0-9]' "$root/hardware.md5" || true)
[ "$actual" -eq "$frames" ] || { echo "FAIL HEVC IQ frames=$actual expected=$frames" >&2; exit 1; }
diff -u "$root/software.md5" "$root/hardware.md5"
if grep -Eq 'publish=error|capture error index=|sync timeout token=|publish=drop|publish=misplaced|Failed to upload decode parameters' "$root/decode.trace"; then
    echo 'FAIL HEVC IQ decoder diagnostics' >&2
    exit 1
fi
echo "PASS HEVC IQ-matrix frames=$actual exact-software-hashes"
