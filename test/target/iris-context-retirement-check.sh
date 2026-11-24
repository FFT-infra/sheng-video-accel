#!/bin/sh

# Validate that browser decoder contexts are reclaimed after their surfaces
# have been released, rather than remaining active until vaTerminate().

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 CLIENT_TRACE_LOG" >&2
    exit 2
fi

log=$1
if [ ! -r "$log" ]; then
    echo "trace log is not readable: $log" >&2
    exit 2
fi

destroyed=$(grep -a -c 'va destroy_context id=' "$log" || true)
finished=$(grep -a -c 'session finish' "$log" || true)

if [ "$destroyed" -eq 0 ]; then
    echo "FAIL retired_contexts_destroyed=0 (expected a context teardown)" >&2
    exit 1
fi
if [ "$finished" -lt "$destroyed" ]; then
    echo "FAIL retired_contexts_destroyed=$destroyed finished=$finished (expected every destroyed context logged session finish)" >&2
    exit 1
fi

echo "PASS retired_contexts_destroyed=$destroyed finished=$finished"
