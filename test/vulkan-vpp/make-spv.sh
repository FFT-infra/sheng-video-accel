#!/bin/sh

# Regenerate the checked-in SPIR-V binaries from their .comp sources on a host
# with glslangValidator, then rewrite SHA256SUMS. The target has no glslang,
# so every module is checked in and pinned by hash; the target gates verify
# the pin at run time instead of compiling on the device.
#
#   copy.spv          flat read/write prereq (prereq.sh / probe.c)
#   nv12_scale.spv    NV12 nearest scale with crop and rotation
#                     (nv12_scale_gate.sh / scale_probe.c, crop_vaapi_gate.sh,
#                     transpose_vaapi_gate.sh through the driver)
#   p010_scale.spv    the same kernel for P010 surfaces
#                     (p010_scale_gate.sh / scale_probe.c)
#   tonemap.spv       HDR10 P010 to SDR NV12 (tonemap_gate.sh / tonemap_probe.c)
#   overlay.spv       global-alpha compositing of two surfaces into one output
#                     (overlay_gate.sh / the probe's overlay mode)
#   overlay_rgb.spv   the same composite with a packed BGRA/RGBA overlay
#                     (overlay_gate.sh's rgb cases)
#
# Usage: test/vulkan-vpp/make-spv.sh [-c]
#   -c  check that every current .spv file matches a fresh compile and
#       SHA256SUMS, without writing (glslang is still needed for the compile)
#
# glslangValidator is a host build tool, not a runtime dependency. Its absence
# in CI and on the target is why this is a script, not a meson target.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
sums="$here/SHA256SUMS"
check=${1:-}

glslang=${GLSLANG_VALIDATOR:-glslangValidator}
if ! command -v "$glslang" >/dev/null 2>&1; then
    echo "FAIL $glslang not found; run this on a host with glslang installed" >&2
    exit 2
fi

scratch=${TMPDIR:-$HOME/Lab/Bridge/tmp}
work=$(mktemp -d "$scratch/vpp-spv.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

names="copy nv12_scale p010_scale tonemap overlay overlay_rgb"
for name in $names; do
    "$glslang" -V --target-env vulkan1.1 -o "$work/$name.spv" "$here/$name.comp" >/dev/null
done

if [ "$check" = "-c" ]; then
    for name in $names; do
        a=$(sha256sum "$work/$name.spv" | cut -d' ' -f1)
        b=$(sha256sum "$here/$name.spv" | cut -d' ' -f1)
        [ "$a" = "$b" ] || { echo "FAIL $name.spv does not match its .comp source" >&2; exit 1; }
        grep -q "^$a  $name.spv\$" "$sums" || { echo "FAIL SHA256SUMS missing $name.spv pin" >&2; exit 1; }
    done
    echo "PASS checked-in SPIR-V matches sources and SHA256SUMS"
    exit 0
fi

: >"$sums"
for name in $names; do
    hash=$(sha256sum "$work/$name.spv" | cut -d' ' -f1)
    cp "$work/$name.spv" "$here/$name.spv"
    chmod 644 "$here/$name.spv" # mktemp's 0600 would keep the tracked binary private
    printf '%s  %s.spv\n' "$hash" "$name" >>"$sums"
    echo "PASS regenerated $name.spv sha256=$hash"
done
