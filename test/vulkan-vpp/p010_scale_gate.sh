#!/bin/sh

# P010 scale primitive gate, run on the target under test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/p010_scale_gate.sh
#
# nv12_scale_gate.sh proves the 8-bit kernel. This one proves the 10-bit
# sibling: p010_scale.spv over two imported DMA-BUFs laid out like the Iris
# CAPTURE P010 buffers (256-byte pitch, one 16-bit word per sample, chroma
# behind the luma plane). The oracle decodes the same 16-bit words, so the
# comparison covers the whole sample, not just the top eight bits. It runs the
# same ratio, crop and rotation matrix, and both DMA-BUF origins.
#
# The crop cases matter more here than in the 8-bit gate: a chroma pair spans
# two luma columns and four bytes in P010, so the region origin is at byte
# 2 * x of the chroma row. A crop at the origin cannot tell that apart from a
# pair at two bytes, which is why every crop case here starts away from it.
#
# This is evidence for the VPP slice's kernel only. It advertises nothing.
# Target prerequisites match nv12_scale_gate.sh.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

for tool in cc pkg-config sha256sum spirv-val; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL missing target tool: $tool" >&2; exit 2; }
done
pkg-config --exists vulkan libdrm || { echo "FAIL missing vulkan/libdrm pkg-config files" >&2; exit 2; }
[ -r /dev/dma_heap/system ] || { echo "FAIL /dev/dma_heap/system is not available" >&2; exit 2; }

(cd "$here" && sha256sum -c SHA256SUMS) || { echo "FAIL SPIR-V does not match SHA256SUMS" >&2; exit 2; }
spv_hash=$(sha256sum "$here/p010_scale.spv" | cut -d' ' -f1)

echo "== validate SPIR-V"
spirv-val "$here/p010_scale.spv" || { echo "FAIL p010_scale.spv is not valid SPIR-V" >&2; exit 1; }

render=${IRIS_RENDER_DEVICE:-}
if [ -z "$render" ]; then
    for node in /dev/dri/renderD*; do
        [ -c "$node" ] || continue
        render=$node
        break
    done
fi
[ -n "$render" ] && [ -c "$render" ] || { echo "FAIL no MSM render node (set IRIS_RENDER_DEVICE)" >&2; exit 2; }

scratch=${IRIS_TEST_SCRATCH_ROOT:-$HOME/Lab/Bridge/tmp}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/vpp-p010.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "== compile probe"
cc -std=c11 -O2 -Wall -Wextra -x c "$here/scale_probe.c" \
    $(pkg-config --cflags libdrm) -lvulkan $(pkg-config --libs libdrm) \
    -o "$work/probe"

# `src:dst:crop:rot[:mirror]`, crop `-` when the whole source is sampled and
# the mirror field optional. Same matrix as the 8-bit gate, shortened where the
# map is already covered there.
cases="
64x32:128x64:-:0
64x32:96x48:-:0
128x72:86x48:-:0
128x72:96x40:96x40+16+16:0
64x32:96x48:48x24+8+4:0
64x32:32x64:-:1
64x32:64x64:-:2
128x72:96x48:-:3
128x72:96x48:96x40+16+16:1
64x32:128x64:-:0:1
128x72:96x48:-:1:2
64x32:96x48:48x24+8+4:0:3
"

failed=0
printf 'spv_sha256=%s\n' "$spv_hash"
printf 'render_node=%s\n' "$render"
for origin in heap msm; do
    echo "== origin: $origin"
    for c in $cases; do
        old_ifs=$IFS
        IFS=:
        set -- $c
        IFS=$old_ifs
        src=$1 dst=$2 crop=$3 rot=$4 mirror=${5:-0}
        set -- "$here/p010_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --p010 || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s exit=%s\n' "$origin" "$src" "$dst" "$crop" "$rot" \
            "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

if [ "$failed" -ne 0 ]; then
    echo "FAIL p010-scale cases are not all byte-exact" >&2
    exit 1
fi
echo "PASS p010-scale cases=$(echo $cases | wc -w) origins=2 pixels_exact spv=$spv_hash render=$render"
