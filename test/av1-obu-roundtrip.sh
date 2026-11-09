#!/bin/sh

# Verify the AV1 OBU writer without involving the decoder hardware.
#
# src/av1_obu.cc rebuilds OBU temporal units from VA-API decode parameters,
# because VA hands the driver a parsed frame header plus bare tile payloads
# while Iris' stateful node needs a real bitstream. That reconstruction either
# reproduces the original stream's pixels or it does not, and that question is
# answerable with an independent software decoder.
#
# With V4L2_VA_DUMP pointing at a directory the driver writes every submitted
# access unit of a session into one file; for AV1 that is the rebuilt
# low-overhead OBU stream. This script decodes it with libdav1d and checks
# the result against a software decode of the source.
#
# Frame counts are expected to differ. AV1 repeats already-decoded frames with
# show_existing_frame, which carries no payload and never reaches a VA-API
# hardware accelerator, so the rebuilt stream contains only the frames that
# were actually decoded. What must hold is that every frame the writer does
# emit is bit-exact and in order, which is what the subsequence check below
# tests.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_AV1_ROUNDTRIP_DIR:-$(iris_artifact_dir av1-roundtrip)}
frames=${IRIS_AV1_ROUNDTRIP_FRAMES:-32}
mkdir -p "$root"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "FAIL ffmpeg is required" >&2
    exit 2
fi
if ! ffmpeg -hide_banner -decoders 2>/dev/null | grep -q libdav1d; then
    echo "SKIP av1-obu-roundtrip needs an ffmpeg with libdav1d" >&2
    exit 0
fi

source=${IRIS_AV1_ROUNDTRIP_SOURCE:-}
if [ -z "$source" ]; then
    source="$root/source.webm"
    ffmpeg -y -hide_banner -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=24" \
        -frames:v "$frames" -c:v libaom-av1 -cpu-used 8 -crf 40 -an "$source"
fi

dump="$root/dump"
rm -rf "$dump"
mkdir -p "$dump"

# The hardware result is checked elsewhere; this run exists to make the
# driver emit its reconstruction, so its own output is discarded.
LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" LIBVA_V4L2_VIDEO_PATH="$device" \
    V4L2_VA_DUMP="$dump" \
    ffmpeg -y -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 \
    -hwaccel vaapi -hwaccel_output_format vaapi -i "$source" -f null - \
    >"$root/driver.log" 2>&1 || true

rebuilt=$(ls -S "$dump"/iris-session-*.bin 2>/dev/null | head -1 || true)
if [ -z "$rebuilt" ] || [ ! -s "$rebuilt" ]; then
    echo "FAIL the driver produced no rebuilt OBU stream; see $root/driver.log" >&2
    exit 1
fi

ffmpeg -y -hide_banner -loglevel error -f obu -c:v libdav1d -i "$rebuilt" \
    -pix_fmt yuv420p -f framemd5 "$root/rebuilt.md5" 2>"$root/rebuilt.log"
ffmpeg -y -hide_banner -loglevel error -i "$source" \
    -pix_fmt yuv420p -f framemd5 "$root/source.md5"

grep -o '[0-9a-f]\{32\}' "$root/rebuilt.md5" > "$root/rebuilt.hashes"
grep -o '[0-9a-f]\{32\}' "$root/source.md5" > "$root/source.hashes"

rebuilt_count=$(wc -l < "$root/rebuilt.hashes")
source_count=$(wc -l < "$root/source.hashes")
if [ "$rebuilt_count" -eq 0 ]; then
    echo "FAIL the rebuilt stream decoded to no frames; see $root/rebuilt.log" >&2
    exit 1
fi

# Every rebuilt frame must appear in the source decode, in order, with nothing
# altered. diff reporting only additions from the source side is exactly that
# property; any change or deletion means a frame was reconstructed wrongly.
if diff "$root/rebuilt.hashes" "$root/source.hashes" \
    | grep -qE '^[0-9,]+[cd][0-9,]+'; then
    echo "FAIL rebuilt frames do not match the source decode" >&2
    diff "$root/rebuilt.hashes" "$root/source.hashes" | head -20 >&2
    exit 1
fi

printf 'PASS av1-obu-roundtrip rebuilt=%s source=%s all rebuilt frames bit-exact\n' \
    "$rebuilt_count" "$source_count"
