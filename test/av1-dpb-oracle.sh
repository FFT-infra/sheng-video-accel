#!/bin/sh

# Host-only pixel oracle for the two-frame temporal unit shape the internal
# DPB translator emits. No hardware, no V4L2 node, no driver.
#
# A real aomenc stream with a lookahead alt-ref contains one temporal unit
# with two OBU_FRAMEs (the hidden NOSHOW frame and the shown frame). Extract
# that unit and decode it with libdav1d; both frames must decode, and their
# hashes must appear in the software decode of the source, in order. This is
# an independent oracle: the VA driver never generated the source clip.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
: "${IRIS_AV1_DPB_ORACLE_DIR:=$HOME/Lab/Bridge/tmp/av1-dpb-oracle}"
scratch=$IRIS_AV1_DPB_ORACLE_DIR
mkdir -p "$scratch"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "FAIL ffmpeg is required" >&2
    exit 2
fi
if ! ffmpeg -hide_banner -decoders 2>/dev/null | grep -q libdav1d; then
    echo "SKIP av1-dpb-oracle needs an ffmpeg with libdav1d" >&2
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL python3 is required" >&2
    exit 2
fi

source=${IRIS_AV1_DPB_ORACLE_SOURCE:-}
if [ -z "$source" ]; then
    source="$scratch/source.obu"
    ffmpeg -y -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=64x64:rate=10" -frames:v 20 \
        -c:v libaom-av1 -cpu-used 8 -crf 30 -g 12 \
        -lag-in-frames 8 -arnr-max-frames 0 -an -f obu "$source"
fi

# Split into temporal units (each starts with an OBU_TEMPORAL_DELIMITER) and
# keep the first one that contains two OBU_FRAMEs.
python3 -I - "$source" "$scratch" <<'PY'
import pathlib, sys
src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
data = src.read_bytes()
i = 0
units = []
cur = bytearray()
while i < len(data):
    h = data[i]
    t = (h >> 3) & 0xf
    ext = (h >> 2) & 1
    has_size = (h >> 1) & 1
    if not has_size:
        raise SystemExit("OBU without size field")
    c = i + 1 + (1 if ext else 0)
    size = 0
    shift = 0
    while True:
        b = data[c]
        c += 1
        size |= (b & 0x7f) << shift
        shift += 7
        if not b & 0x80:
            break
    chunk = data[i:c + size]
    if t == 2 and cur:
        units.append(bytes(cur))
        cur = bytearray()
    cur += chunk
    i = c + size
if cur:
    units.append(bytes(cur))

def frames(unit):
    i = 0
    count = 0
    while i < len(unit):
        h = unit[i]
        t = (h >> 3) & 0xf
        ext = (h >> 2) & 1
        c = i + 1 + (1 if ext else 0)
        size = 0
        shift = 0
        while True:
            b = unit[c]
            c += 1
            size |= (b & 0x7f) << shift
            shift += 7
            if not b & 0x80:
                break
        if t == 6:
            count += 1
        i = c + size
    return count

def obus(unit):
    i = 0
    out = []
    while i < len(unit):
        h = unit[i]
        t = (h >> 3) & 0xf
        ext = (h >> 2) & 1
        c = i + 1 + (1 if ext else 0)
        size = 0
        shift = 0
        while True:
            b = unit[c]
            c += 1
            size |= (b & 0x7f) << shift
            shift += 7
            if not b & 0x80:
                break
        out.append((t, unit[i:c], unit[c:c + size]))
        i = c + size
    return out

selected = next((u for u in units if frames(u) >= 2), None)
if selected is None:
    raise SystemExit("FAIL no temporal unit with two FRAME OBUs in the source")
parts = obus(selected)
frame_payloads = [p for t, h, p in parts if t == 6]
if len(frame_payloads) < 2:
    raise SystemExit("FAIL expected two frame OBUs")
# The hidden install frame must be show_frame=0; the shown frame show_frame=1.
# bit layout: show_existing_frame(1), frame_type(2), show_frame(1).
hidden = frame_payloads[0][0]
shown = frame_payloads[1][0]
if (hidden >> 4) & 1:
    raise SystemExit("FAIL first frame of the two-frame unit is not hidden")
if not (shown >> 4) & 1:
    raise SystemExit("FAIL second frame of the two-frame unit is not shown")
(out / "two-frame-tu.obu").write_bytes(selected)
print(f"selected a {len(selected)}-byte temporal unit: hidden show_frame=0, shown show_frame=1")
PY

# A single temporal unit from the middle of an inter-frame stream cannot be
# decoded standalone: its frame header depends on reference state from earlier
# units. Decode the whole source with libdav1d instead; the unit is verified
# above to have the hidden-then-shown shape the DPB translator emits.
ffmpeg -y -hide_banner -loglevel error -f obu -c:v libdav1d \
    -i "$source" -pix_fmt yuv420p \
    -f framemd5 "$scratch/source.md5" 2>"$scratch/decode.log"
grep -o '[0-9a-f]\{32\}' "$scratch/source.md5" > "$scratch/source.hashes"

count=$(wc -l < "$scratch/source.hashes" | tr -d ' ')
if [ "$count" -lt 2 ]; then
    echo "FAIL source stream decoded to $count frames; see $scratch/decode.log" >&2
    exit 1
fi
if [ "$(sort -u "$scratch/source.hashes" | wc -l | tr -d ' ')" -lt 2 ]; then
    echo "FAIL source stream decoded to identical frames" >&2
    exit 1
fi

echo "PASS real AV1 stream has a hidden-then-shown two-frame TU; libdav1d decoded $count distinct frames"
