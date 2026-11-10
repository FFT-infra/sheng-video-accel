#!/bin/sh

# Feed the clip checker a known-good trace and several broken ones. Host only:
# no Iris node, no FFmpeg, no framemd5 real decoder involved.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
checker="$root/test/iris-clip-trace-check.sh"
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-clip-trace-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

sw="$scratch/sw.md5"
va="$scratch/va.md5"
trace="$scratch/clean.trace"
cat >"$sw" <<'EOF'
#format: frame checksums
#stream#, dts, pts, duration, size, hash
0,          0,          0,        1,  1382400, bb06d790db755de375828b790749ce1b
0,          1,          1,        1,  1382400, ff383743f7a9fc6beaeed2977924b1de
0,          2,          2,        1,  1382400, 900e6372cf3e800c9681aab2e50ceedf
EOF
cp "$sw" "$va"

good_trace() {
    cat >"$trace" <<'EOF'
va create_surfaces count=1 1280x720 fourcc=NV12 imported=0 fd=-1
capture configure 1280x736 stride=1280 size=1413120 visible=1280x720 pool=32 memory=import
va end_picture ctx=6 surface=2 deferred
submit token=1 bytes=19198 output=0
capture token=1 index=0 flags=0x4011 publish=import
capture token=2 index=0 flags=0x4011 publish=import
capture token=3 index=0 flags=0x4011 publish=import
session finish submitted=3 completed=3 errors=0 drops=0 timeouts=0 drains=1 misplaced=0
EOF
}

expect_ok() {
    label=$1
    shift
    if ! "$@" >/dev/null 2>&1; then
        printf 'FAIL %s: checker rejected a valid trace\n' "$label" >&2
        exit 1
    fi
}

expect_fail() {
    label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        printf 'FAIL %s: checker accepted a broken trace\n' "$label" >&2
        exit 1
    fi
}

args="--trace $trace --va-md5 $va --sw-md5 $sw --frames 3 --import 3"

good_trace
# shellcheck disable=SC2086
expect_ok clean sh "$checker" $args

# A real AV1 clip that lost 87 of 120 frames but kept a green synthetic matrix.
good_trace
sed -i '$d' "$va"
printf '0,          3,          3,        1,  1382400, deadbeef\n' >"$va"
# shellcheck disable=SC2086
expect_fail truncated-framemd5 sh "$checker" $args

# Right frame count, wrong pixels.
cp "$sw" "$va"
sed -i 's/^0,          1,.*/0,          1,          1,        1,  1382400, 00000000000000000000000000000000/' "$va"
# shellcheck disable=SC2086
expect_fail pixel-mismatch sh "$checker" $args

cp "$sw" "$va"
good_trace
sed -i 's/errors=0/errors=2/; s/drops=0/drops=1/' "$trace"
# shellcheck disable=SC2086
expect_fail errors-and-drops sh "$checker" $args

good_trace
sed -i 's/completed=3/completed=2/' "$trace"
# shellcheck disable=SC2086
expect_fail lost-completion sh "$checker" $args

good_trace
sed -i '/capture token=1 /s/publish=import/publish=copy-gpu/' "$trace"
# shellcheck disable=SC2086
expect_fail unexpected-copy sh "$checker" $args

good_trace
echo 'sync timeout token=3 pending=1 queued=1 held=1 action=drain' >>"$trace"
# shellcheck disable=SC2086
expect_fail sync-bound sh "$checker" $args

# A DRC clip that declares its copy-gpu half and one invisible completion.
good_trace
sed -i 's/submitted=3/submitted=7/; s/completed=3/completed=7/; s/drains=1/drains=2/' "$trace"
sed -i '1i import rejected: client layout differs from the CAPTURE layout (client stride=1280; capture stride=896)' "$trace"
sed -i '1i capture configure 896x480 stride=896 size=647168 visible=854x480 pool=32 memory=mmap' "$trace"
sed -i '/^session finish /d' "$trace"
for i in 4 5 6 7; do echo "capture token=$i index=0 flags=0x4011 publish=copy-gpu" >>"$trace"; done
echo 'capture token=8 index=0 flags=0x104040 publish=invisible' >>"$trace"
echo 'session finish submitted=7 completed=7 errors=0 drops=0 timeouts=0 drains=2 misplaced=0' >>"$trace"
# shellcheck disable=SC2086
expect_fail unexpected-copy sh "$checker" $args
# shellcheck disable=SC2086
expect_ok declared-drc sh "$checker" $args --copy 4 --allow-invisible 1

# The DRC fallback is only correct when the client kept the old geometry. A
# trace without the layout-mismatch reason must not be excused.
sed -i '/^import rejected: client layout differs/d' "$trace"
# shellcheck disable=SC2086
expect_fail unexplained-drc sh "$checker" $args --copy 4 --allow-invisible 1

echo 'PASS clip trace checker (good, truncated, mismatched, lost, copy, sync, DRC)'
