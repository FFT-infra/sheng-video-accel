#!/bin/sh

# Run one command on the Iris target under the experiment guard and write a
# run record beside its logs.
#
# Every target gate in this tree already guards its own experiment and prints
# its own evidence, but the evidence lives wherever the operator pointed the
# guard, and the identity that makes it mean something (boot, module hash,
# driver hash, resolved nodes) was re-collected by hand for every run. This
# runner puts both in one directory: the command's stdout and stderr, the
# guard's own state, and a summary.json that names the source commit the run
# came from.
#
# It does not deploy. `test/remote/deploy.sh` syncs and rebuilds; a run that
# skips it runs against whatever the target currently holds, and the record
# says which driver and module hashes that was.
#
# Usage:
#   IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh [--out DIR] -- command [args ...]
#
# The command runs on the target inside the remote checkout. It is landed in a
# per-run script under the run directory and read by the guard's child from
# there, because a backgrounded `sh -s` would get /dev/null for stdin and
# silently run nothing. Only the command text is copied to the target.
#
# Environment:
#   IRIS_REMOTE_HOST   ssh destination (required)
#   IRIS_REMOTE_ROOT   remote checkout, relative to the remote home
#                      (default iris-vaapi-lab/src)
#   IRIS_REMOTE_RUNS   remote directory for run artifacts, relative to the
#                      remote home (default iris-vaapi-lab/runs)
#
# Exit status is the command's own, except that a guard which stopped the run
# (reboot, thermal trip, timeout) is reported as 3 and a missing identity as 4.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=../lib/iris-env.sh
. "$here/../lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

usage() {
    sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'
}

host=${IRIS_REMOTE_HOST:-}
out=''
while [ $# -gt 0 ]; do
    case $1 in
        --out) out=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[ -n "$host" ] || fail "IRIS_REMOTE_HOST is required"
[ "$#" -gt 0 ] || { usage; fail "no command given after --"; }

remote_root=${IRIS_REMOTE_ROOT:-iris-vaapi-lab/src}
remote_runs=${IRIS_REMOTE_RUNS:-iris-vaapi-lab/runs}
case $remote_root in /*|*..*) fail "IRIS_REMOTE_ROOT must be a relative path without '..'" ;; esac
case $remote_runs in /*|*..*) fail "IRIS_REMOTE_RUNS must be a relative path without '..'" ;; esac

name=run-$(date -u +%Y%m%dT%H%M%SZ)
[ -n "$out" ] || out=$(iris_artifact_dir "$name")
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
name=$(basename -- "$out")
remote_run="$remote_runs/$name"

printf '%s\n' "$*" >"$out/command.txt"
commit=$(git -C "$here/../.." rev-parse HEAD 2>/dev/null || echo unknown)
dirty=0
[ -z "$(git -C "$here/../.." status --porcelain 2>/dev/null)" ] || dirty=1

# Identity comes from the target, by driver name. A node number is not stable
# across boots, so it is recorded only beside the name v4l2-ctl reports for it.
ssh -o ConnectTimeout=10 "$host" "cd ~/$remote_root && sh -s" >"$out/identity.txt" <<'IDENTITY' || fail "cannot read identity from $host"
set -eu
. ./test/lib/iris-env.sh
decoder=$(iris_resolve_device)
encoder=$(iris_resolve_encoder || true)
printf 'boot=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
printf 'kernel=%s\n' "$(uname -r)"
printf 'module=%s\n' "$(sha256sum "/lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/qcom-iris.ko.zst" 2>/dev/null | cut -d' ' -f1)"
printf 'driver=%s\n' "$(sha256sum /usr/lib64/dri/v4l2_drv_video.so 2>/dev/null | cut -d' ' -f1)"
printf 'decoder_node=%s\n' "$decoder"
printf 'decoder_name=%s\n' "$(v4l2-ctl -d "$decoder" --info 2>/dev/null | sed -n 's/.*Card type *: *//p')"
printf 'encoder_node=%s\n' "${encoder:-none}"
printf 'encoder_name=%s\n' "${encoder:+"$(v4l2-ctl -d "$encoder" --info 2>/dev/null | sed -n 's/.*Card type *: *//p')"}"
IDENTITY
cat "$out/identity.txt"

ssh -o ConnectTimeout=10 "$host" "mkdir -p ~/$remote_run/guard" || fail "cannot create ~/$remote_run/guard"

# Land the command in a file rather than feeding it to `sh -s` on stdin. The
# guard backgrounds its child (timeout ... &), and a backgrounded async list
# in a non-interactive POSIX shell gets /dev/null for stdin: the heredoc was
# silently dropped, nothing ran, and the guard still reported exit 0. A
# command file gives the child a real script to read and keeps the quoting-safe
# heredoc transport. Verified with an echo marker, which must appear in
# guard/command.stdout for a run to be real.
remote_cmd="$remote_run/command.sh"
ssh -o ConnectTimeout=10 "$host" "cat > \"\$HOME/$remote_cmd\"" <<EOF || fail "cannot write command file on $host"
$*
EOF

started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rc=0
ssh -o ConnectTimeout=10 "$host" "cd ~/$remote_root && IRIS_EXPERIMENT_DIR=\"\$HOME/$remote_run/guard\" \
    sh scripts/iris-experiment-guard.sh -- sh \"\$HOME/$remote_cmd\"" >"$out/command.stdout" 2>"$out/command.stderr" || rc=$?
finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)

guard_status=absent
if ssh -o ConnectTimeout=10 "$host" "test -f \"\$HOME/$remote_run/guard/guard.state\"" 2>/dev/null; then
    guard_status=$(ssh -o ConnectTimeout=10 "$host" \
        "sed -n 's/^status=//p' \"\$HOME/$remote_run/guard/guard.state\" | head -1")
    [ -n "$guard_status" ] || guard_status=unknown
    scp -q -r "$host:./$remote_run/guard" "$out/guard" 2>/dev/null || true
fi

# summary.json is written by hand rather than through a serialiser, so a run
# record stays readable on a target with no Python. Values here are hashes,
# identifiers and a command line; the escaping is for quotes and backslashes
# in the command only.
escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}
{
    printf '{\n'
    printf '  "schema": "qualcomm-iris-vaapi/target-run-v1",\n'
    printf '  "host": "%s",\n' "$(escape "$host")"
    printf '  "source_commit": "%s",\n' "$commit"
    printf '  "source_dirty": %s,\n' "$dirty"
    printf '  "started": "%s",\n' "$started"
    printf '  "finished": "%s",\n' "$finished"
    printf '  "exit_status": %s,\n' "$rc"
    printf '  "guard_status": "%s",\n' "$guard_status"
    printf '  "command": "%s",\n' "$(escape "$*")"
    printf '  "identity": {\n'
    awk -F= 'NF >= 2 { key = $1; value = substr($0, length(key) + 2); entries[++n] = key; vals[n] = value }
        END { for (i = 1; i <= n; i++) printf "    \"%s\": \"%s\"%s\n", entries[i], vals[i], (i < n ? "," : "") }' \
        "$out/identity.txt"
    printf '  }\n'
    printf '}\n'
} >"$out/summary.json"

printf 'run_record=%s guard=%s exit=%s\n' "$out/summary.json" "$guard_status" "$rc"
case $guard_status in
    complete|absent) ;;
    *) printf 'FAIL guard stopped the run: %s\n' "$guard_status" >&2; exit 3 ;;
esac
[ "$rc" -eq 0 ] || exit "$rc"
printf 'PASS %s\n' "$*"
