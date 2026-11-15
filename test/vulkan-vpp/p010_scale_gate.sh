#!/bin/sh

# P010 scale primitive gate, run on the target under test/remote/gate.sh:
#
#   IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh -- sh test/vulkan-vpp/p010_scale_gate.sh
#
# nv12_scale_gate.sh proves the 8-bit kernel. This one proves the 10-bit
# companion: p010_scale.spv over two imported DMA-BUFs laid out like the Iris
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
# The second pass runs the same matrix with the four-tap bilinear method
# (--interp 1), compared within one code of a double-precision model of the
# blend, plus the two invariants: a constant 10-bit picture stays that constant
# at any ratio, and a 1:1 scale reproduces the point map byte for byte. The
# clamp is the 10-bit one here, so a kernel that reused the 8-bit saturation
# would fail on any highlight sample above 255.
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

bilinear_cases="
64x32:128x64:-:0:0
64x32:96x48:-:0:0
128x72:86x48:-:0:0
128x72:96x40:96x40+16+16:0:0
64x32:96x48:48x24+8+4:0:0
64x32:32x64:-:1:0
128x72:96x48:-:3:2
64x32:64x64:-:0:1
64x64:2x2:-:0:0
"

# The area average (shrinking), the cubic (enlarging) and the driver's own
# per-axis pick. The 10-bit codes go through the same taps; what differs is
# that the blend runs on the code rather than on the padded word, which the
# probe's model checks byte for byte.
area_cases="
64x32:32x16:-:0:0
64x32:42x22:-:0:0
128x72:86x48:-:0:0
128x72:96x48:-:3:2
64x32:96x48:48x24+8+4:0:0
64x64:2x2:-:0:0
"
cubic_cases="
64x32:128x64:-:0:0
64x32:96x48:-:0:0
2x2:64x64:-:0:0
64x32:64x64:-:0:1
128x72:96x48:-:3:2
"
advanced_cases="
64x32:32x16:-:0:0
64x32:128x64:-:0:0
128x72:86x48:-:0:0
128x72:96x48:-:3:2
64x32:96x48:48x24+8+4:0:0
"

# Bit-depth conversion: the source is eight bits and the destination is this
# kernel's ten. The conversion is `code << 2` before any filtering, the exact
# conversion swscale applies in that direction, so the point map stays
# byte-exact and the weighted methods agree with the same double model. An
# eight-bit source halves the source pitch and chroma pair size, which is what
# the address path has to get right.
up_cases="
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
        set -- "$here/p010_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --p010 || rc=$?
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
        set -- "$here/p010_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 1 --p010 || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s interp=bilinear exit=%s\n' "$origin" "$src" "$dst" \
            "$crop" "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

# 1023 is the largest 10-bit code; a constant picture of it proves both the
# weights and the clamp in one case.
for origin in heap msm; do
    rc=0
    "$work/probe" "$here/p010_scale.spv" "$origin" "$render" 64x32 200x100 --interp 1 --constant 1023 --p010 \
        || rc=$?
    printf 'case=%s 64x32->200x100 constant=1023 interp=bilinear exit=%s\n' "$origin" "$rc"
    [ "$rc" -eq 0 ] || failed=1
done

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
            set -- "$here/p010_scale.spv" "$origin" "$render" "$src" "$dst"
            [ "$crop" = - ] || set -- "$@" "$crop"
            rc=0
            case $method in
                area) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 2 --p010 || rc=$? ;;
                cubic) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 3 --p010 || rc=$? ;;
                advanced) "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 4 --p010 || rc=$? ;;
            esac
            printf 'case=%s %s->%s crop=%s rot=%s mirror=%s interp=%s exit=%s\n' "$origin" "$src" "$dst" \
                "$crop" "$rot" "$mirror" "$method" "$rc"
            [ "$rc" -eq 0 ] || failed=1
        done
    done
done

for origin in heap msm; do
    for interp in 2 3 4; do
        rc=0
        "$work/probe" "$here/p010_scale.spv" "$origin" "$render" 64x32 200x100 --interp "$interp" \
            --constant 1023 --p010 || rc=$?
        printf 'case=%s 64x32->200x100 constant=1023 interp=%s exit=%s\n' "$origin" "$interp" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
done

# An eight-bit source into this kernel's ten-bit destination: the same matrix
# under the point map and the bilinear blend, so both the exact and the
# weighted path see the conversion. The constant case is 255, the top of the
# source's range, which must come out as 1020 — the largest 10-bit code `code
# << 2` can produce.
for origin in heap msm; do
    for c in $up_cases; do
        old_ifs=$IFS
        IFS=:
        set -- $c
        IFS=$old_ifs
        src=$1 dst=$2 crop=$3 rot=$4 mirror=${5:-0}
        set -- "$here/p010_scale.spv" "$origin" "$render" "$src" "$dst"
        [ "$crop" = - ] || set -- "$@" "$crop"
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --up || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s up exit=%s\n' "$origin" "$src" "$dst" "$crop" \
            "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
        rc=0
        "$work/probe" "$@" --rot "$rot" --mirror "$mirror" --interp 1 --up || rc=$?
        printf 'case=%s %s->%s crop=%s rot=%s mirror=%s up interp=bilinear exit=%s\n' "$origin" "$src" \
            "$dst" "$crop" "$rot" "$mirror" "$rc"
        [ "$rc" -eq 0 ] || failed=1
    done
    rc=0
    "$work/probe" "$here/p010_scale.spv" "$origin" "$render" 64x32 200x100 --interp 1 --constant 255 --up \
        || rc=$?
    printf 'case=%s 64x32->200x100 constant=255 up interp=bilinear exit=%s\n' "$origin" "$rc"
    [ "$rc" -eq 0 ] || failed=1
done

if [ "$failed" -ne 0 ]; then
    echo "FAIL p010-scale cases are not all byte-exact" >&2
    exit 1
fi
echo "PASS p010-scale cases=$(echo $cases | wc -w) bilinear=$(echo $bilinear_cases | wc -w) area=$(echo $area_cases | wc -w) cubic=$(echo $cubic_cases | wc -w) advanced=$(echo $advanced_cases | wc -w) up=$(echo $up_cases | wc -w) origins=2 pixels_exact spv=$spv_hash render=$render"
