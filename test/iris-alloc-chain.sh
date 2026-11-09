#!/bin/sh

# The stable-buffer allocation chain on real hardware (docs/architecture.md
# §3.4): MSM GEM cached-coherent, MSM GEM write-combined, system DMA heap.
# The first two rungs need an MSM render node, and the last is only reached
# when they are refused, so this runs test/iris-alloc-probe under
# test/alloc-fault.so, which refuses the MSM allocations the probe asks it to.
#
# Usage: test/iris-alloc-chain.sh [render-node] [build-dir]
#   render-node  default /dev/dri/renderD128
#   build-dir    default build, the meson build directory with the probe in it
#
# Every mode is a separate process, because the fault mode is read once per
# allocation and the chain must be walked from the top each time.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
render=${1:-${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}}
build=${2:-$root/build}

probe="$build/test/iris-alloc-probe"
shim="$build/test/alloc-fault.so"

for required in "$probe" "$shim"; do
    if [ ! -e "$required" ]; then
        echo "FAIL missing $required; build the tree first (ninja -C $build)" >&2
        exit 2
    fi
done
if [ ! -c "$render" ]; then
    echo "FAIL $render is not a character device" >&2
    exit 2
fi

scratch=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch"
work=$(mktemp -d "$scratch/iris-alloc-chain.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

failures=0

# The unfaulted run has no expected log: which MSM rung answers depends on
# whether this kernel accepts MSM_BO_CACHED_COHERENT, and both are documented.
printf '== no-fault\n'
"$probe" "$render" no-fault || failures=$((failures + 1))

for mode in coherent both coherent-enomem; do
    printf '== %s\n' "$mode"
    IRIS_TEST_ALLOC_FAULT=$mode IRIS_TEST_ALLOC_LOG="$work/$mode.log" LD_PRELOAD=$shim \
        "$probe" "$render" "$mode" || failures=$((failures + 1))
done

# A probe that cannot tell an injected chain from a real one would report a
# pass for the wrong reason. Both of these must be refused (exit 2), never
# reported as PASS.
printf '== negative controls\n'
if "$probe" "$render" coherent 2>/dev/null; then
    echo "FAIL the probe ran an injected mode without the shim" >&2
    failures=$((failures + 1))
else
    printf 'PASS an injected mode is refused without the shim\n'
fi
if IRIS_TEST_ALLOC_FAULT= IRIS_TEST_ALLOC_LOG="$work/unset.log" LD_PRELOAD=$shim \
    "$probe" "$render" coherent 2>/dev/null; then
    echo "FAIL the probe ran an injected mode with the shim's fault mode unset" >&2
    failures=$((failures + 1))
else
    printf 'PASS an injected mode is refused when the shim was not told which rung to refuse\n'
fi

if [ "$failures" -ne 0 ]; then
    echo "FAIL alloc-chain render=$render failures=$failures" >&2
    exit 1
fi
printf 'PASS alloc-chain render=%s modes=no-fault,coherent,both,coherent-enomem\n' "$render"
