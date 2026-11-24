#!/bin/sh

# Regression for the gate runner's "no output is not a pass" contract. A guard
# can reach status=complete with exit=0 while the child command never ran --
# a `sh -c LIBVA_DRIVER_NAME=v4l2 ...` with its quoting flattened left the
# probe unexecuted behind a green record. This is a host-side test: it builds
# fake run directories rather
# than ssh-ing to a target.

set -eu
root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}
checker=$root/test/remote/gate-output-check.sh
runner=$root/test/remote/gate.sh
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
work=$(mktemp -d "$scratch_base/iris-gate-output.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15
run=$work/run
mkdir -p "$run/guard"

expect_ok() {
    label=$1
    shift
    "$@" >/dev/null 2>&1 || {
        printf 'FAIL %s: checker refused a real run\n' "$label" >&2
        exit 1
    }
}

expect_fail() {
    label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        printf 'FAIL %s: checker accepted a run that proved nothing\n' "$label" >&2
        exit 1
    fi
}

# An empty child stdout is the false-green case. Without an opt-out it must
# fail; this is the assertion the old gate.sh could not make.
: >"$run/guard/command.stdout"
expect_fail empty-output sh "$checker" "$run"
expect_fail empty-output-no-arg sh "$checker" "$run" 0

# A deliberately silent command may opt out explicitly.
expect_ok empty-output-opt-out sh "$checker" "$run" 1

# A run that actually printed something passes, with and without the flag.
printf 'PASS three probes\n' >"$run/guard/command.stdout"
expect_ok real-output sh "$checker" "$run"
expect_ok real-output-opt-out sh "$checker" "$run" 1

# A missing output file is not silently treated as a pass either.
rm -f "$run/guard/command.stdout"
expect_fail missing-output sh "$checker" "$run"

# The runner must actually call the checker and pass its opt-out through;
# a checker that exists but is not wired in would leave the hole open.
grep -Fq 'gate-output-check.sh' "$runner"
grep -Fq 'IRIS_GATE_ALLOW_EMPTY_OUTPUT' "$runner"

echo "PASS gate output contract: empty and missing output refuse, opt-out and real output pass"
