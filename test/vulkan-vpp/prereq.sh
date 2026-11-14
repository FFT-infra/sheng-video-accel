#!/bin/sh

# VPP prerequisite gate, run on the target under test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/prereq.sh
#
# It proves the two facts a zero-copy Vulkan video processor needs before any
# VAProfileNone/VideoProc capability is advertised (docs/architecture.md):
#   * a compute pipeline from the checked-in SPIR-V runs with exact output;
#   * a DMA-BUF, from both allocators src/iris/alloc.cc uses, imports as
#     Vulkan external memory and is read by that pipeline.
#
# It does not deploy or rebuild the driver: the probe is pure userspace and
# needs no V4L2 node. gate.sh records the boot, kernel, module and installed
# driver hashes, and the decoder/encoder names resolved by driver name.
#
# Target prerequisites (present on sheng, absent in CI): a Vulkan 1.x ICD,
# libvulkan, libdrm + msm_drm.h, vulkan headers, /dev/dma_heap/system, an MSM
# render node, cc, pkg-config, sha256sum. CI has no GPU, so this stays out of
# meson test and run-static-checks.sh only checks the SPIR-V pin and syntax.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

for tool in cc pkg-config sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL missing target tool: $tool" >&2; exit 2; }
done
pkg-config --exists vulkan libdrm || { echo "FAIL missing vulkan/libdrm pkg-config files" >&2; exit 2; }
[ -r /dev/dma_heap/system ] || { echo "FAIL /dev/dma_heap/system is not available" >&2; exit 2; }

# The checked-in binary is the source of truth; a stale or hand-edited copy.spv
# must fail here rather than produce a pass the shader source cannot reproduce.
(cd "$here" && sha256sum -c SHA256SUMS) || { echo "FAIL copy.spv does not match SHA256SUMS" >&2; exit 2; }
spv_hash=$(sha256sum "$here/copy.spv" | cut -d' ' -f1)

# Resolve the MSM render node; a numbered default would go stale across boots.
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
work=$(mktemp -d "$scratch/vpp-prereq.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "== compile probe"
cc -std=c11 -O2 -Wall -Wextra -x c "$here/probe.c" \
    $(pkg-config --cflags libdrm) -lvulkan $(pkg-config --libs libdrm) \
    -o "$work/probe"

heap_rc=0
msm_rc=0
echo "== origin: system DMA heap"
"$work/probe" "$here/copy.spv" heap || heap_rc=$?
echo "== origin: MSM GEM PRIME fd"
"$work/probe" "$here/copy.spv" msm "$render" || msm_rc=$?

printf 'spv_sha256=%s\n' "$spv_hash"
printf 'render_node=%s\n' "$render"
printf 'case=heap exit=%s\n' "$heap_rc"
printf 'case=msm exit=%s\n' "$msm_rc"

if [ "$heap_rc" -ne 0 ] || [ "$msm_rc" -ne 0 ]; then
    echo "FAIL vpp-prereq heap=$heap_rc msm=$msm_rc" >&2
    exit 1
fi
echo "PASS vpp-prereq heap=compute_exact msm=compute_exact spv=$spv_hash render=$render"
