#!/bin/sh

# Validate the browser-facing stateful teardown trace. Chrome may retire VA
# surfaces while an access unit is still in flight, but that must not turn into
# a bounded session sync timeout or an unmatched timestamp during surface destruction.

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

timeouts=$(grep -c 'sync timeout token=' "$log" || true)
drain=$(grep -c 'drain complete last=1' "$log" || true)

fail=0
if [ "$timeouts" -ne 0 ]; then
    echo "FAIL teardown_timeouts=$timeouts (expected 0)" >&2
    fail=1
fi
if [ "$drain" -eq 0 ]; then
    echo "FAIL stateful_drain_complete=0 (expected terminal LAST drain)" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "PASS teardown_timeouts=0 stateful_drain_complete=$drain"
