#!/bin/sh

# Exercise deploy.sh with command shims.  Unsafe roots must fail before any
# ssh/rsync process is started because the real command uses rsync --delete.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
deploy="$root/test/remote/deploy.sh"
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-deploy-path-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

calls="$scratch/calls"
mkdir -p "$scratch/bin"
: >"$calls"

for command in ssh rsync; do
    script="$scratch/bin/$command"
    sed "s|@COMMAND@|$command|g; s|@CALLS@|$calls|g" >"$script" <<'SHIM'
#!/bin/sh
printf '%s\n' '@COMMAND@' >>'@CALLS@'
exit 73
SHIM
    chmod +x "$script"
done

assert_rejected() {
    label=$1
    value=$2
    : >"$calls"
    if PATH="$scratch/bin:$PATH" IRIS_REMOTE_HOST=shim IRIS_REMOTE_ROOT="$value" sh "$deploy" >"$scratch/out" 2>"$scratch/err"; then
        echo "FAIL unsafe root accepted: $label" >&2
        exit 1
    fi
    if [ -s "$calls" ]; then
        echo "FAIL unsafe root reached an external command: $label" >&2
        cat "$calls" >&2
        exit 1
    fi
    grep -Fq 'FAIL unsafe IRIS_REMOTE_ROOT' "$scratch/err" \
        || { echo "FAIL unsafe root had the wrong error: $label" >&2; exit 1; }
}

assert_rejected empty ''
assert_rejected dot '.'
assert_rejected root '/'
assert_rejected dotdot '..'
assert_rejected option-like '-delete-target'
assert_rejected absolute '/home/user/Lab/iris'
assert_rejected parent-prefix '../Lab/iris'
assert_rejected parent-middle 'iris/../tools'
assert_rejected parent-suffix 'iris/tools/..'
assert_rejected dot-middle 'Lab/./iris'
assert_rejected double-slash 'Lab//iris'
assert_rejected trailing-slash 'Lab/iris/'
assert_rejected single-quote "Lab/iris'bad"
assert_rejected double-quote 'Lab/iris"bad'
assert_rejected whitespace 'Lab/iris bad'
assert_rejected newline "$(printf 'Lab/iris\nbad')"
assert_rejected single-segment '.'

: >"$calls"
if PATH="$scratch/bin:$PATH" \
    IRIS_REMOTE_HOST=shim \
    IRIS_REMOTE_ROOT='Projects/iris-vaapi-lab/src' \
    IRIS_SOURCE_COMMIT=0000000000000000000000000000000000000000 \
    IRIS_TRACKED_SOURCE_SHA256=1111111111111111111111111111111111111111111111111111111111111111 \
    sh "$deploy" \
    >"$scratch/out" 2>"$scratch/err"; then
    echo 'FAIL command shim unexpectedly let deploy complete' >&2
    exit 1
fi
if ! grep -Fxq ssh "$calls"; then
    echo 'FAIL safe relative root did not reach ssh after validation' >&2
    exit 1
fi

echo 'PASS deploy root validation rejects destructive paths before ssh/rsync'
