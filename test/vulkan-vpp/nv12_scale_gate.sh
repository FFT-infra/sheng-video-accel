#!/bin/sh

# NV12 scale primitive gate, run on the target under test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=sheng test/remote/gate.sh -- sh test/vulkan-vpp/nv12_scale_gate.sh
#
# The flat read/write prereq (prereq.sh) proved a DMA-BUF can be a compute
# source and destination. It did not prove the NV12 plane math a real VPP
# output needs: pitch-aligned stride, a chroma plane behind the luma plane,
# 2x horizontal/vertical subsampling, and a crop. This gate dispatches the
# checked-in nv12_scale.spv over two imported DMA-BUFs laid out exactly like
# the Iris CAPTURE NV12 buffers (src/iris/alloc.cc:stable_layout ->
# plane_layout) and checks EVERY destination pixel against a CPU oracle that
# uses swscale's point positions and VA's rotation order, over a matrix of
# ratios, crops and rotations, for both DMA-BUF origins the driver allocates.
# A wrong stride, offset, subsample, rotation or sample position lands on a
# different pattern byte and fails here rather than producing a
# plausible-but-wrong frame. p010_scale_gate.sh runs the same map for 10-bit
# surfaces.
#
# A second pass over a smaller matrix runs the same kernel with the four-tap
# bilinear method (--interp 1), which the driver advertises as
# VA_FILTER_INTERPOLATION_BILINEAR. There the oracle is the same blend in
# double, compared within one code: GLSL does not pin the rounding of the
# four-tap sum, and one code is far below the distance a wrong tap, a missing
# weight or a transposed plane would produce. Two cases bracket it from the
# other side: a constant picture must come out that constant at every ratio
# (the weights sum to one), and a 1:1 scale must reproduce the nearest map
# byte for byte: at a sample centre there is nothing to blend, so the probe
# drops its one-code tolerance for exactly that geometry.
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
cc -std=c11 -O2 -Wall -Wextra -x c "$here/scale_probe.c" \
    $(pkg-config --cflags libdrm) -lvulkan $(pkg-config --libs libdrm) \
    -o "$work/probe"

# Every case is `src:dst:crop:rot`, with the crop `-` when the whole source is
# sampled. Ratios a client can ask for: upscale, downscale, identity, integral
# and fractional steps. Crops are what a VA surface region means: the sampled
# rectangle starts at X,Y inside the source. Rotation is VA's clockwise
# quarter turn, applied before the scale, so a quarter turn transposes the
# sampled rectangle, and mirroring flips that rectangle on its own rotated
# axes. Every case is `src:dst:crop:rot[:mirror]` with the mirror field
# optional. The oracle is the same integer map swscale uses, so a green case is
# a byte-level equivalence with the software filters.
cases="
64x32:128x64:-:0
64x32:96x48:-:0
64x32:32x16:-:0
64x32:42x22:-:0
64x32:200x100:-:0
62x34:62x34:-:0
128x72:86x48:-:0
64x32:128x64:32x16+0+0:0
64x32:96x48:48x24+8+4:0
64x32:32x16:16x16+16+8:0
128x72:86x48:96x40+16+16:0
64x32:32x64:-:1
32x64:64x32:-:3
64x32:64x32:-:2
128x72:96x48:-:1
128x72:56x96:-:3
128x72:96x48:96x40+16+16:1
64x32:32x16:32x16+16+8:3
64x32:128x64:-:0:1
64x32:64x32:-:0:2
128x72:96x48:-:1:1
128x72:56x96:-:3:2
64x32:96x48:48x24+8+4:0:3
62x34:62x34:-:0:1
64x64:2x2:-:0
64x64:2x2:-:1
2x2:64x64:-:0
"

# Bilinear cases: the same ratios, crops, rotations and mirrors, plus the two
# invariants the weights have to satisfy. `constant` is the code a
# `--constant` case fills the source with.
bilinear_cases="
64x32:128x64:-:0:0
64x32:96x48:-:0:0
64x32:42x22:-:0:0
128x72:86x48:-:0:0
64x32:32x16:-:0:0
64x32:96x48:48x24+8+4:0:0
128x72:96x48:96x40+16+16:1:0
128x72:56x96:-:3:2
64x32:64x32:-:0:1
64x32:128x64:-:0:3
64x64:2x2:-:0:0
62x34:62x34:-:0:0
"

# The area average and the cubic, plus the driver's own per-axis pick. Area is
# exact when shrinking, so its cases are downscales; the cubic is for
# enlargement. `--interp 4` is what VA_FILTER_INTERPOLATION_ADVANCED maps to
# and must match whichever of the two the ratio selects.
area_cases="
64x32:32x16:-:0:0
64x32:42x22:-:0:0
128x72:86x48:-:0:0
128x72:56x96:-:3:2
64x32:96x48:48x24+8+4:0:0
64x64:2x2:-:0:0
"
cubic_cases="
64x32:128x64:-:0:0
64x32:96x48:-:0:0
2x2:64x64:-:0:0
64x32:128x64:-:0:3
128x72:56x96:-:3:2
"
advanced_cases="
64x32:32x16:-:0:0
64x32:128x64:-:0:0
128x72:86x48:-:0:0
128x72:56x96:-:3:2
64x32:96x48:48x24+8+4:0:0
"

# Bit-depth conversion: the source is ten bits and the destination is this
# kernel's eight. The conversion is `code >> 2` before any filtering, which is
# the exact conversion swscale applies, so the point map stays byte-exact and
# the weighted methods still agree with the same double model. The crop origin
# stays even, and it is a luma origin on both ends: a chroma pair spans two
# luma columns whether the samples are one byte or two.
down_cases="
64x32:128x64:-:0:0
64x32:32x16:-:0:0
128x72:86x48:-:0:0
64x32:96x48:48x24+8+4:0:0
64x32:64x64:-:2:0
128x72:96x48:-:1:1
128x72:56x96:-:3:2
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
        set -- "$here/nv12_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s exit=%s\n' "$origin" "$src" "$dst" "$crop" "$rot" \
            "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

echo "== bilinear"
for origin in heap msm; do
    echo "== origin: $origin"
    for c in $bilinear_cases; do
        old_ifs=$IFS
        IFS=:
        set -- $c
        IFS=$old_ifs
        src=$1 dst=$2 crop=$3 rot=$4 mirror=${5:-0}
        set -- "$here/nv12_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 1 || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s interp=bilinear exit=%s\n' "$origin" "$src" "$dst" \
            "$crop" "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

# A constant picture, at a ratio whose weights are all non-trivial: 200 from
# 64 is not an integer step, so every destination sample sits between two
# sources.
for origin in heap msm; do
    rc=0
    "$work/probe" "$here/nv12_scale.spv" "$origin" "$render" 64x32 200x100 --interp 1 --constant 77 || rc=$?
    printf 'case=%s 64x32->200x100 constant=77 interp=bilinear exit=%s\n' "$origin" "$rc"
    [ "$rc" -eq 0 ] || failed=1
done

# The area average, the cubic and the driver's own pick, each against the same
# double-precision model the bilinear cases use.
for method in area cubic advanced; do
    echo "== $method"
    eval "list=\$${method}_cases"
    for origin in heap msm; do
        echo "== origin: $origin"
        for c in $list; do
            old_ifs=$IFS
            IFS=:
            set -- $c
            IFS=$old_ifs
            src=$1 dst=$2 crop=$3 rot=$4 mirror=${5:-0}
            set -- "$here/nv12_scale.spv" "$origin" "$render" "$src" "$dst"
            [ "$crop" = - ] || set -- "$@" "$crop"
            rc=0
            case $method in
                area) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 2 || rc=$? ;;
                cubic) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 3 || rc=$? ;;
                advanced) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 4 || rc=$? ;;
            esac
            printf 'case=%s %s->%s crop=%s rot=%s mirror=%s interp=%s exit=%s\n' "$origin" "$src" "$dst" \
                "$crop" "$rot" "$mirror" "$method" "$rc"
            [ "$rc" -eq 0 ] || failed=1
        done
    done
done

# A constant picture under every method: the weights sum to one, so the
# picture has to come through unchanged whatever the taps are.
for origin in heap msm; do
    for interp in 2 3 4; do
        rc=0
        "$work/probe" "$here/nv12_scale.spv" "$origin" "$render" 64x32 200x100 --interp "$interp" \
            --constant 77 || rc=$?
        printf 'case=%s 64x32->200x100 constant=77 interp=%s exit=%s\n' "$origin" "$interp" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

# A ten-bit source into this kernel's eight-bit destination: the same ratio,
# crop, rotation and mirror matrix under the point map and the bilinear blend,
# so the conversion is exercised on both the exact and the weighted path. The
# constant case is the widest 10-bit code whose low two bits are clear, which
# is the only kind that survives `code >> 2` without rounding.
for origin in heap msm; do
    for c in $down_cases; do
        old_ifs=$IFS
        IFS=:
        set -- $c
        IFS=$old_ifs
        src=$1 dst=$2 crop=$3 rot=$4 mirror=${5:-0}
        set -- "$here/nv12_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --down || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s down exit=%s\n' "$origin" "$src" "$dst" "$crop" \
            "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 1 --down || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s down interp=bilinear exit=%s\n' "$origin" "$src" \
            "$dst" "$crop" "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
    rc=0
    "$work/probe" "$here/nv12_scale.spv" "$origin" "$render" 64x32 200x100 --interp 1 --constant 1020 --down \
        || rc=$?
    printf 'case=%s 64x32->200x100 constant=1020 down interp=bilinear exit=%s\n' "$origin" "$rc"
    [ "$rc" -eq 0 ] || failed=1
done

if [ "$failed" -ne 0 ]; then
    echo "FAIL nv12-scale cases are not all byte-exact" >&2
    exit 1
fi
echo "PASS nv12-scale cases=$(echo $cases | wc -w) bilinear=$(echo $bilinear_cases | wc -w) area=$(echo $area_cases | wc -w) cubic=$(echo $cubic_cases | wc -w) advanced=$(echo $advanced_cases | wc -w) down=$(echo $down_cases | wc -w) origins=2 pixels_exact spv=$spv_hash render=$render"
