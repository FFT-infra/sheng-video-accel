#!/bin/sh

# Verify that a post-build strip can refresh the staged manifest to the exact
# bytes shipped in a package. This is intentionally hardware-independent.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
scratch_base=${IRIS_TEST_TMP_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
work=$(mktemp -d "$scratch_base/iris-install-manifest-check.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15

artifact=$work/v4l2_drv_video.so
manifest=$work/install-manifest.txt
printf 'unstripped-payload\n' >"$artifact"
cat >"$manifest" <<'EOF'
schema=qualcomm-iris-vaapi/system-install-v1
artifact_sha256=0000000000000000000000000000000000000000000000000000000000000000
artifact_size=0
driver_path=/usr/lib64/dri/v4l2_drv_video.so
EOF

sh "$root/scripts/refresh-install-manifest.sh" "$manifest" "$artifact" >/dev/null
expected_hash=$(sha256sum "$artifact" | awk '{print $1}')
expected_size=$(wc -c <"$artifact" | tr -d '[:space:]')
[ "$(sed -n 's/^artifact_sha256=//p' "$manifest")" = "$expected_hash" ]
[ "$(sed -n 's/^artifact_size=//p' "$manifest")" = "$expected_size" ]

printf 'post-strip-payload\n' >>"$artifact"
sh "$root/scripts/refresh-install-manifest.sh" "$manifest" "$artifact" >/dev/null
expected_hash=$(sha256sum "$artifact" | awk '{print $1}')
expected_size=$(wc -c <"$artifact" | tr -d '[:space:]')
[ "$(sed -n 's/^artifact_sha256=//p' "$manifest")" = "$expected_hash" ]
[ "$(sed -n 's/^artifact_size=//p' "$manifest")" = "$expected_size" ]

echo 'PASS staged install manifest tracks final payload bytes'
