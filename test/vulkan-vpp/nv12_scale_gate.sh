#!/bin/sh

# NV12 scale primitive gate, run on the target under test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/nv12_scale_gate.sh
#
# The flat read/write prereq (prereq.sh) proved a DMA-BUF can be a compute
# source and destination. It did not prove the NV12 plane math a real VPP
# output needs: pitch-aligned stride, a chroma plane behind the luma plane,
# and 2x horizontal/vertical subsampling. This gate dispatches the checked-in
# nv12_scale.spv over two imported DMA-BUFs laid out exactly like the Iris
# CAPTURE NV12 buffers (src/iris/alloc.cc:stable_layout -> plane_layout) and
# checks EVERY destination byte against a CPU oracle that uses swscale's point
# positions, over a matrix of ratios, for both DMA-BUF origins the driver
# allocates. A wrong stride, offset, subsample or sample position lands on a
# different pattern byte and fails here rather than producing a
# plausible-but-wrong scaled frame.
#
# This is evidence for the VPP slice's load-bearing kernel only. It does not
# advertise VAProfileNone/VAEntrypointVideoProc, wire a VPP context, or touch
# data/iris-codec-capabilities.json. Target prerequisites match prereq.sh
# (Vulkan ICD, libvulkan, libdrm + msm_drm.h, Vulkan headers, system DMA heap,
# MSM render node, cc, pkg-config, sha256sum, spirv-val). CI has no GPU, so
# run-static-checks.sh only pins the SPIR-V binary and checks script syntax.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

for tool in cc pkg-config sha256sum spirv-val; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL missing target tool: $tool" >&2; exit 2; }
done
pkg-config --exists vulkan libdrm || { echo "FAIL missing vulkan/libdrm pkg-config files" >&2; exit 2; }
[ -r /dev/dma_heap/system ] || { echo "FAIL /dev/dma_heap/system is not available" >&2; exit 2; }

# The checked-in binary is the source of truth; a stale or hand-edited .spv
# must fail here rather than change what the gate measures. Same pin the host
# static check enforces, now including the scale shader.
(cd "$here" && sha256sum -c SHA256SUMS) || { echo "FAIL SPIR-V does not match SHA256SUMS" >&2; exit 2; }
spv_hash=$(sha256sum "$here/nv12_scale.spv" | cut -d' ' -f1)

echo "== validate SPIR-V"
spirv-val "$here/nv12_scale.spv" || { echo "FAIL nv12_scale.spv is not valid SPIR-V" >&2; exit 1; }

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
work=$(mktemp -d "$scratch/vpp-nv12.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "== compile probe"
cc -std=c11 -O2 -Wall -Wextra -x c "$here/nv12_scale_probe.c" \
    $(pkg-config --cflags libdrm) -lvulkan $(pkg-config --libs libdrm) \
    -o "$work/probe"

# Ratios a client can ask for: upscale, downscale, identity, integral and
# fractional steps. The oracle is the same integer map swscale uses, so a
# green case is a byte-level equivalence with `scale=WxH:flags=neighbor`.
cases="64x32:128x64 64x32:96x48 64x32:32x16 64x32:42x22 64x32:200x100 62x34:62x34 128x72:86x48"

failed=0
printf 'spv_sha256=%s\n' "$spv_hash"
printf 'render_node=%s\n' "$render"
for origin in heap msm; do
    echo "== origin: $origin"
    for c in $cases; do
        src=${c%%:*}
        dst=${c#*:}
        rc=0
        "$work/probe" "$here/nv12_scale.spv" "$origin" "$render" "$src" "$dst" || rc=$?
        printf 'case=%s %s->%s exit=%s\n' "$origin" "$src" "$dst" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

if [ "$failed" -ne 0 ]; then
    echo "FAIL nv12-scale ratios are not all byte-exact" >&2
    exit 1
fi
echo "PASS nv12-scale ratios=$(echo $cases | wc -w) origins=2 pixels_exact spv=$spv_hash render=$render"
