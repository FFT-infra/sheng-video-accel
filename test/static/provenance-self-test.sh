#!/bin/sh

# Hardware-independent generate/verify/tamper regression for the deployment
# provenance format.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}
scratch_base=${IRIS_TEST_TMP_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
work=$(mktemp -d "$scratch_base/iris-provenance-self-test.XXXXXX")
manifest="$work/provenance.json"
artifact="$work/v4l2_drv_video.so"
commit=0000000000000000000000000000000000000000
digest=1111111111111111111111111111111111111111111111111111111111111111

trap 'rm -rf "$work"' 0 1 2 15
cp "$root/test/remote/provenance.py" "$artifact"

python3 "$root/test/remote/provenance.py" generate \
    --source-commit "$commit" --tracked-source-sha256 "$digest" --source-dirty 1 \
    --build-dir "$root" --artifact "$artifact" --output "$manifest" >/dev/null
python3 "$root/test/remote/provenance.py" verify \
    --manifest "$manifest" --artifact "$artifact" \
    --expected-source-commit "$commit" \
    --expected-tracked-source-sha256 "$digest" --expected-source-dirty 1 >/dev/null

printf 'tamper\n' >>"$artifact"
if python3 "$root/test/remote/provenance.py" verify \
    --manifest "$manifest" --artifact "$artifact" >/dev/null 2>&1; then
    echo "FAIL provenance verification accepted a modified artifact" >&2
    exit 1
fi

echo "PASS provenance generation, identity verification and tamper rejection"
