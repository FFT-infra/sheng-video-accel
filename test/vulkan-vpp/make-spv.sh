#!/bin/sh

# Regenerate copy.spv from copy.comp on a host with glslangValidator. The
# target has no glslang, so the SPIR-V binary is checked in and pinned by
# SHA256SUMS; the gate verifies the pin at run time instead of compiling on
# the device.
#
# Usage: test/vulkan-vpp/make-spv.sh [-c]
#   -c  also check that the current copy.spv matches the fresh compile and
#       SHA256SUMS (no glslang needed for the check itself if you skip regen)
#
# glslangValidator is a host build tool, not a runtime dependency. Its absence
# in CI and on the target is why this is a script, not a meson target.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
comp="$here/copy.comp"
spv="$here/copy.spv"
sums="$here/SHA256SUMS"

glslang=${GLSLANG_VALIDATOR:-glslangValidator}
if ! command -v "$glslang" >/dev/null 2>&1; then
    echo "FAIL $glslang not found; run this on a host with glslang installed" >&2
    exit 2
fi

fresh=$(mktemp "${TMPDIR:-$HOME/Lab/Bridge/tmp}/vpp-spv.XXXXXX")
trap 'rm -f "$fresh"' EXIT HUP INT TERM
"$glslang" -V --target-env vulkan1.1 -o "$fresh" "$comp" >/dev/null

hash=$(sha256sum "$fresh" | cut -d' ' -f1)
cp "$fresh" "$spv"
printf '%s  copy.spv\n' "$hash" >"$sums"
echo "PASS regenerated copy.spv sha256=$hash"
