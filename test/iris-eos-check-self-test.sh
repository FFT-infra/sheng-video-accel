#!/bin/sh

# A single FFmpeg process may replace its VA context at a coded-size change.
# Each context must still have an independent strict STOP/LAST drain contract.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-eos-check-self-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
log="$scratch/multi-context.trace"

cat >"$log" <<'EOF'
va create_context id=0
submit token=1
submit token=2
capture token=1 index=0 flags=0 publish=direct
capture token=2 index=1 flags=0 publish=direct
capture last
drain complete last=1
va create_context id=1
submit token=3
submit token=4
capture token=3 index=0 flags=0 publish=direct
capture token=4 index=1 flags=0 publish=direct
capture last
drain complete last=1
EOF

sh "$root/test/iris-eos-check.sh" "$log" 4 || exit 1

cat >"$log" <<'EOF'
va create_context id=0
submit token=1
capture token=1 index=0 flags=0 publish=direct
capture last
EOF
if sh "$root/test/iris-eos-check.sh" "$log" 1 2>/dev/null; then
    echo "FAIL allowed missing drain" >&2
    exit 1
fi

cat >"$log" <<'EOF'
va create_context id=0
submit token=1
submit token=2
capture token=1 index=0 flags=0 publish=direct
capture last
drain complete last=1
EOF
if sh "$root/test/iris-eos-check.sh" "$log" 2 2>/dev/null; then
    echo "FAIL allowed missing completion" >&2
    exit 1
fi

cat >"$log" <<'EOF'
va create_context id=0
submit token=1
capture token=1 index=0 flags=0 publish=error
capture last
drain complete last=1
EOF
if sh "$root/test/iris-eos-check.sh" "$log" 1 2>/dev/null; then
    echo "FAIL allowed capture error" >&2
    exit 1
fi

# An AV1 alt-ref is submitted, completes without a timestamp, and is
# published: more pictures than output frames, and not an error.
cat >"$log" <<'EOF'
va create_context id=0
submit token=1
submit token=2
submit token=3
capture token=1 index=0 flags=0x4009 publish=direct
capture noshow token=0 index=1 flags=0x4041 publish=direct
capture token=3 index=2 flags=0x4011 publish=direct
capture last
drain complete last=1
EOF
sh "$root/test/iris-eos-check.sh" "$log" 2 --hidden-pictures || exit 1
if sh "$root/test/iris-eos-check.sh" "$log" 2 2>/dev/null; then
    echo "FAIL allowed a hidden picture in a strict trace" >&2
    exit 1
fi

# An Import publish is a completed picture like any other: the firmware wrote
# straight into the client's buffer. Counting only direct and copy made a
# fully successful Import run report capture=0.
cat >"$log" <<'EOF'
va create_context id=0
submit token=1
submit token=2
capture token=1 index=0 flags=0x4010 publish=import
capture token=2 index=0 flags=0x4010 publish=import
capture last
drain complete last=1
EOF
sh "$root/test/iris-eos-check.sh" "$log" 2 || exit 1

echo 'PASS multi-context EOS checker'
