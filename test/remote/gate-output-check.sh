#!/bin/sh

# Refuse a gate run whose guarded child produced no output. A guard can reach
# status=complete with exit=0 while the command never ran: a `sh -c` whose
# quoting was flattened by the runner, or a heredoc a backgrounded child never
# read. The 2026-11-17 misc-bound run was exactly that -- three missing PASS
# lines behind a green record.
#
# Usage: gate-output-check.sh <run-dir> [allow-empty]
#   allow-empty=1 opts out for a deliberately silent command.
#
# Exit: 0 real output (or explicit opt-out), 5 empty output, 2 bad usage.

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo 'usage: gate-output-check.sh <run-dir> [allow-empty]' >&2
    exit 2
fi
run_dir=$1
allow=${2:-0}
out=$run_dir/guard/command.stdout

[ -f "$out" ] || {
    echo "FAIL no guarded child output at $out" >&2
    exit 5
}
if [ -s "$out" ]; then
    exit 0
fi
[ "$allow" = 1 ] || {
    echo "FAIL gate produced no child output: $out" >&2
    echo '     the command may not have run; set IRIS_GATE_ALLOW_EMPTY_OUTPUT=1 only for a deliberately silent command' >&2
    exit 5
}
exit 0
