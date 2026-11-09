#!/bin/sh

# Validate a V4L2 stateful trace against the generic EOS contract (also used by
# Chromium's video decoder tests): every submitted access unit must complete,
# followed by one terminal CAPTURE LAST marker per VA context after decoder
# STOP. FFmpeg may recreate its VA context at coded-size changes while keeping
# one decoder process alive.

set -eu

strict_sync=1
hidden_pictures=0
if [ "$#" -eq 3 ] && [ "$3" = "--allow-bounded-sync" ]; then
    strict_sync=0
elif [ "$#" -eq 3 ] && [ "$3" = "--hidden-pictures" ]; then
    # AV1 decodes alt-ref pictures that the stream displays later through
    # show_existing_frame. They are submitted and completed like any other
    # picture, so the trace holds more submissions than frames the client
    # outputs; only the output side can be compared frame for frame.
    hidden_pictures=1
elif [ "$#" -ne 2 ]; then
    echo "usage: $0 TRACE_LOG FRAME_COUNT [--allow-bounded-sync|--hidden-pictures]" >&2
    exit 2
fi

log=$1
expected=$2

case "$expected" in
    ''|*[!0-9]*)
        echo "FRAME_COUNT must be a non-negative integer" >&2
        exit 2
        ;;
esac

if [ ! -r "$log" ]; then
    echo "trace log is not readable: $log" >&2
    exit 2
fi

count() {
    grep -c "$1" "$log" || true
}

contexts=$(count 'va create_context id=')
[ "$contexts" -gt 0 ] || contexts=1

submitted=$(count 'submit token=')
# Every publish policy a capture can name: the trace prints how the pixels
# reached the client, and an Import capture is as much a published picture as
# a Direct or a copied one. Leaving `import` out made a fully successful
# Import run report capture=0.
capture=$(grep -E -c 'capture (noshow )?token=.*publish=(direct|import|copy-gpu|copy-cpu)' "$log" || true)
noshow=$(count 'capture noshow token=')
drain=$(count 'drain complete last=1')
capture_errors=$(grep -E -c 'publish=error|capture error index=' "$log" || true)
timeouts=$(count 'sync timeout token=')
drops=$(count 'publish=drop')

terminal_last=$(awk '
    /^capture / { last_cap = $0 }
    /^drain complete last=1/ { if (last_cap !~ /^capture last/) err++ }
    END { print err + 0 }
' "$log")

expected_capture=$((expected))

fail=0
check() {
    name=$1
    actual=$2
    wanted=$3
    if [ "$actual" -ne "$wanted" ]; then
        echo "FAIL $name=$actual (expected $wanted)" >&2
        fail=1
    fi
}

# Every submitted picture completes, and every picture the client can display
# is published at least once.
check_at_least() {
    name=$1
    actual=$2
    wanted=$3
    if [ "$actual" -lt "$wanted" ]; then
        echo "FAIL $name=$actual (expected at least $wanted)" >&2
        fail=1
    fi
}

if [ "$hidden_pictures" -eq 1 ]; then
    check_at_least submitted_frames "$submitted" "$expected"
    check_at_least capture "$capture" "$expected_capture"
    if [ "$noshow" -gt "$capture" ]; then
        echo "FAIL noshow=$noshow (more than capture=$capture)" >&2
        fail=1
    fi
else
    check submitted_frames "$submitted" "$expected"
    check capture "$capture" "$expected_capture"
    # A picture the firmware did not show is only legitimate for a codec
    # whose stream carries hidden references (AV1 alt-refs).
    check noshow_pictures "$noshow" 0
fi
check drain "$drain" "$contexts"
check capture_errors "$capture_errors" 0
check terminal_last "$terminal_last" 0
if [ "$strict_sync" -eq 1 ]; then
    check timeouts "$timeouts" 0
    check drops "$drops" 0
elif [ "$timeouts" -ne 0 ] || [ "$drops" -ne 0 ]; then
    echo "WARN bounded_sync timeouts=$timeouts drops=$drops" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "PASS frames=$expected contexts=$contexts submitted=$submitted capture=$capture drain=$drain timeouts=$timeouts drops=$drops noshow=$noshow"
