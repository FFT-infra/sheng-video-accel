#!/bin/sh

# Exercise the shared node rule without a real /dev/video node, a driver or a
# desktop session.
#
# The rule used to live in the browser launcher, and this file used to drive
# that launcher. The launcher is gone; the rule moved to
# scripts/lib/iris-node.sh and the cases that mattered moved with it.
#
# Two seams make this testable on an unprivileged build host, which cannot
# mknod: IRIS_NODE_SEARCH_ROOT moves the scan to a scratch directory, and
# IRIS_NODE_IS_CHAR replaces the character-device test. Both default to the
# real thing, so a shipped path exercises the real rule.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
node_lib="$root/scripts/lib/iris-node.sh"
[ -r "$node_lib" ] || { echo "FAIL node rule missing: $node_lib" >&2; exit 1; }

scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-node-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

dev=$scratch/dev
mkdir -p "$dev" "$scratch/bin"
# Every node number a case uses, as an empty file. video99 is a file the
# predicate below refuses, so "a regular file is not a node" stays a real
# distinction rather than a tautology.
for n in 2 3 4 5 6 7 9 13 99; do
    : >"$dev/video$n"
done
cat >"$scratch/is-char" <<'EOF'
#!/bin/sh
case "${1##*/}" in
    video99) exit 1 ;;
    video[0-9]*) exit 0 ;;
esac
exit 1
EOF
chmod +x "$scratch/is-char"

# A fake v4l2-ctl that answers from the case's table and exits non-zero for
# anything the table does not name, the way the real tool does for a node that
# is not a simple video device.
cat >"$scratch/bin/v4l2-ctl" <<'EOF'
#!/bin/sh
node=$2
while IFS='|' read -r path driver card; do
    [ "$path" = "$node" ] || continue
    printf 'Driver Info:\n\tDriver name      : %s\n\tCard type        : %s\n' "$driver" "$card"
    exit 0
done <"${IRIS_FAKE_TABLE:?}"
exit 1
EOF
chmod +x "$scratch/bin/v4l2-ctl"

# One resolution in a subshell. $1 = function, $2 = table (paths relative to
# the scratch dev directory, e.g. "video4|iris_driver|Iris Decoder"), rest =
# extra NAME=VALUE exports.
resolve() {
    fn=$1
    table=$2
    shift 2
    : >"$scratch/table"
    printf '%s\n' "$table" | while IFS= read -r line; do
        case "$line" in
            ''|\#*) continue ;;
            *) printf '%s/%s\n' "$dev" "$line" >>"$scratch/table" ;;
        esac
    done
    (
        PATH="$scratch/bin:$PATH"
        IRIS_FAKE_TABLE="$scratch/table"
        IRIS_NODE_SEARCH_ROOT="$dev"
        IRIS_NODE_IS_CHAR="$scratch/is-char"
        export PATH IRIS_FAKE_TABLE IRIS_NODE_SEARCH_ROOT IRIS_NODE_IS_CHAR
        # shellcheck source=lib/iris-node.sh
        . "$node_lib"
        for assignment in "$@"; do
            eval "export $assignment"
        done
        "$fn"
    )
}

pass=0
expect() {
    name=$1 expected=$2 actual=$3
    if [ "$actual" = "$expected" ]; then
        printf 'ok   %s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL %s: expected [%s], got [%s]\n' "$name" "$expected" "$actual" >&2
        exit 1
    fi
}

expect_fail() {
    name=$1 table=$2
    if resolve iris_node_decoder "$table" >"$scratch/out" 2>"$scratch/err"; then
        printf 'FAIL %s: expected a refusal, got [%s]\n' "$name" "$(cat "$scratch/out")" >&2
        exit 1
    fi
    grep -Fq 'iris-node:' "$scratch/err" \
        || { printf 'FAIL %s: refusal carried no diagnostic\n' "$name" >&2; exit 1; }
    printf 'ok   %s\n' "$name"
    pass=$((pass + 1))
}

# The node numbers are deliberately not 0 and 1, and a camera sits between
# them: the rule must tell nodes apart by card type, never by number.
two='video13|iris_driver|Qualcomm Camera Subsystem
video7|iris_driver|Iris Encoder
video4|iris_driver|Iris Decoder'

expect 'decoder by card type' "$dev/video4" "$(resolve iris_node_decoder "$two")"
expect 'encoder by card type' "$dev/video7" "$(resolve iris_node_encoder "$two")"

# A display with only a decoder: the encoder lookup prints nothing and still
# succeeds, because decode-only is a supported configuration.
out=$(resolve iris_node_encoder 'video2|iris_driver|Iris Decoder') \
    || { echo 'FAIL no-encoder: exited non-zero' >&2; exit 1; }
expect 'no encoder prints nothing' '' "$out"

# One iris node that does not name itself a decoder: unambiguous, so it is
# the decoder.
expect 'single unnamed node' "$dev/video9" \
    "$(resolve iris_node_decoder 'video9|iris_driver|Iris Video')"

# Two iris nodes, neither identifying as the decoder: refuse rather than pick.
expect_fail 'ambiguous nodes refused' \
    'video5|iris_driver|Iris Video
video6|iris_driver|Iris Video'

# Two iris nodes both claiming to be the decoder: refuse.
expect_fail 'two decoders refused' \
    'video5|iris_driver|Iris Decoder
video6|iris_driver|Iris Decoder'

# Nothing reports iris_driver: refuse, never fall back to a node number.
expect_fail 'no iris node refused' 'video3|uvcvideo|Webcam'

# A regular file named like a node is not a node.
expect_fail 'regular file ignored' 'video99|iris_driver|Iris Decoder'

# The explicit overrides win over probing, because they are what the driver
# itself reads.
expect 'video path override' "$dev/video4" \
    "$(resolve iris_node_decoder "$two" "LIBVA_V4L2_VIDEO_PATH=$dev/video4")"
expect 'encoder path override' "$dev/video7" \
    "$(resolve iris_node_encoder "$two" "LIBVA_V4L2_ENCODER_PATH=$dev/video7")"

# A non-device override is refused with its own diagnostic, not silently
# ignored and not replaced by a probe.
if resolve iris_node_decoder "$two" "LIBVA_V4L2_VIDEO_PATH=$scratch/table" \
        >"$scratch/out" 2>"$scratch/err"; then
    echo 'FAIL non-device override accepted' >&2
    exit 1
fi
grep -Fq 'not a character device' "$scratch/err" \
    || { echo 'FAIL non-device override: wrong diagnostic' >&2; exit 1; }
printf 'ok   non-device override refused\n'
pass=$((pass + 1))

# Neither diagnostic may carry its own copy of the rule, and neither may name
# a node by number.
for helper in iris-device-check iris-verify; do
    script="$root/scripts/$helper.sh"
    [ -f "$script" ] || { echo "FAIL missing $helper" >&2; exit 1; }
    grep -Fq 'iris-node.sh' "$script" \
        || { echo "FAIL $helper does not source the shared rule" >&2; exit 1; }
    if grep -Eq '/dev/video[0-9]+' "$script"; then
        echo "FAIL $helper names a node by number" >&2
        exit 1
    fi
    printf 'ok   %s sources the rule and names no node\n' "$helper"
    pass=$((pass + 1))
done

printf 'PASS node rule: %d cases, resolution, refusals and single source\n' "$pass"
