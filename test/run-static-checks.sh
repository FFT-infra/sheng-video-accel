#!/bin/sh

# Everything that can be verified without a V4L2 device or a running browser.
# This is the entry point used by CI and the one to run before every commit.
#
# Hardware-dependent suites (iris-matrix.sh, iris-structure-matrix.sh,
# iris-hardware-suite.sh) and the runtime log analysers (iris-eos-check.sh,
# iris-ending-check.sh, iris-context-retirement-check.sh) are deliberately not
# invoked here: the first group needs the decoder node, the second needs a
# trace captured from a real playback.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
test_dir="$root/test"
failures=0

run() {
    name=$1
    shift
    if "$@"; then
        return 0
    fi
    echo "FAIL $name" >&2
    failures=$((failures + 1))
}

# A shell script that does not parse is a check that silently never ran. One
# of the trace checkers shipped broken for a while because a typo made it grep
# a path instead of a file, so parse every script we own on every run.
echo "== shell syntax"
for script in "$test_dir"/*.sh "$test_dir"/remote/*.sh "$root"/scripts/*.sh; do
    [ -f "$script" ] || continue
    run "syntax $(basename "$script")" sh -n "$script"
done

echo "== node resolution invariants"
node_lib="$root/scripts/lib/iris-node.sh"
run node-lib-syntax sh -n "$node_lib"
run node-lib-dynamic-node grep -Fq 'iris_driver' "$node_lib"
run node-lib-no-fixed-node sh -c '! grep -Eq "/dev/video[0-9]+" "$1"' sh "$node_lib"
run node-lib-refuses-guess grep -Fq 'refusing a fixed-node fallback' "$node_lib"
# The rule is one file. Both diagnostics must source it rather than carrying
# a second copy, and neither may name a node by number.
for helper in "$root"/scripts/iris-device-check.sh "$root"/scripts/iris-verify.sh; do
    run "no-fixed-node $(basename "$helper")" sh -c \
        '! grep -Eq "/dev/video[0-9]+" "$1"' sh "$helper"
    run "sources-node-rule $(basename "$helper")" grep -Fq 'iris-node.sh' "$helper"
done
# The browser launcher and its desktop entry are gone: the driver needs no
# wrapper, because libva resolves it through the msm_drv_video.so alias and a
# client keeps its own profile. Reintroducing either must fail here.
run no-browser-launcher sh -c '! test -e "$1"' sh "$root/scripts/iris-vaapi-browser"
run no-desktop-entry sh -c '! ls "$1"/*.desktop >/dev/null 2>&1' sh "$root/data"
# Chrome must not be given a video-decoder feature override anywhere we ship:
# on Chrome 151 forcing AcceleratedVideoDecodeLinuxZeroCopyGL off made the
# pipeline build VaapiVideoDecoder, hit the ImageProcessor path and fall back
# to software. With no launcher there is no place for one to hide, so assert
# it across the whole shipped tree.
run no-video-feature-override sh -c \
    '! grep -rFq -- "AcceleratedVideoDecode" "$1/scripts" "$1/data"' sh "$root"
run iris-node-check sh "$test_dir/iris-node-check.sh" "$root"

echo "== source invariants"
run iris-source-invariants sh "$test_dir/iris-source-invariants.sh" "$root"
run iris-env-doc-check sh "$test_dir/iris-env-doc-check.sh" "$root"
run iris-codec-matrix-check sh "$test_dir/iris-codec-matrix-check.sh" "$root"
run iris-experiment-guard-self-test sh "$test_dir/iris-experiment-guard-self-test.sh" "$root"
run iris-eos-check-self-test sh "$test_dir/iris-eos-check-self-test.sh" "$root"
run iris-setup-path-check sh "$test_dir/iris-setup-path-check.sh" "$root"
run iris-packaging-check sh "$test_dir/iris-packaging-check.sh" "$root"
run iris-kernel-patch-check sh "$test_dir/iris-kernel-patch-check.sh" "$root"
run iris-install-manifest-check sh "$test_dir/iris-install-manifest-check.sh" "$root"
run provenance-python-syntax python3 -c \
    'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text())' \
    "$test_dir/remote/provenance.py"
for helper in cdp; do
    run "$helper-python-syntax" python3 -c \
        'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text())' \
        "$test_dir/remote/$helper.py"
done
run provenance-self-test sh "$test_dir/provenance-self-test.sh" "$root"

if [ "$failures" -ne 0 ]; then
    printf 'FAIL static checks: %d failing\n' "$failures" >&2
    exit 1
fi
echo "PASS static checks"
