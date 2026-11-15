#!/bin/sh

# Verify that one AV1 temporal unit with two OBU_FRAMEs is legal and decodes
# to two frames with an independent software decoder. This is the container
# shape another backend's hidden reference frame needs; the C++ half of the
# test checks the rebuilt unit's structure, this half checks the shape against
# libdav1d on a real aomenc fixture.
#
# No hardware, no driver, no V4L2 node. The fixture is a tiny lossless-ish
# testsrc2 clip encoded with libaom-av1; it is checked into the repo so the
# check never generates its own oracle.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
: "${IRIS_AV1_INSTALL_DIR:=$HOME/Lab/Bridge/tmp/av1-install-tu}"
scratch=$IRIS_AV1_INSTALL_DIR
mkdir -p "$scratch"
fixture="$root/test/fixtures/av1-two-frame-tu.obu"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "FAIL ffmpeg is required" >&2
    exit 2
fi
if ! ffmpeg -hide_banner -decoders 2>/dev/null | grep -q libdav1d; then
    echo "FAIL av1-install-tu-roundtrip needs an ffmpeg with libdav1d" >&2
    exit 2
fi
if [ ! -s "$fixture" ]; then
    echo "FAIL missing AV1 two-frame fixture: $fixture" >&2
    exit 1
fi

ffmpeg -y -hide_banner -loglevel error -f obu -c:v libdav1d -i "$fixture" \
    -pix_fmt yuv420p -f framemd5 "$scratch/fixture.md5" 2>"$scratch/fixture.log"
grep -o '[0-9a-f]\{32\}' "$scratch/fixture.md5" > "$scratch/fixture.hashes"

count=$(wc -l < "$scratch/fixture.hashes" | tr -d ' ')
if [ "$count" -ne 2 ]; then
    echo "FAIL expected 2 decoded frames from the two-frame temporal unit, got $count" >&2
    exit 1
fi

# The two frame hashes must differ: a one-frame decode would fail the count
# check above, and duplicated frames would prove the unit was not really two.
if [ "$(sort -u "$scratch/fixture.hashes" | wc -l | tr -d ' ')" -ne 2 ]; then
    echo "FAIL the two-frame temporal unit decoded two identical frames" >&2
    exit 1
fi

echo "PASS two-frame AV1 temporal unit decodes to two distinct frames"
