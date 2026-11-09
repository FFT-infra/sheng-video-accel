#!/bin/sh

# Node resolution for the Iris display, shared by iris-device-check and
# iris-verify.
#
# This file exists so there is exactly one copy of the rule. Both callers used
# to ask the browser launcher (--print-node / --print-encoder); that wrapper is
# gone, and the rule it held is what has to survive, not the wrapper.
#
# The rule, in one sentence: a node is found by the `iris_driver` name it
# reports, and the decoder and encoder are told apart by the card type, never
# by a /dev/video number. Sourcing scripts must not hardcode one either.
#
# A caller that sets LIBVA_V4L2_VIDEO_PATH or LIBVA_V4L2_ENCODER_PATH is
# honoured first, because that is the override the driver itself reads.
#
# IRIS_NODE_SEARCH_ROOT moves the search off /dev, and IRIS_NODE_IS_CHAR
# replaces the character-device test. Both exist for the host test only, which
# has no real nodes and may not create them; nothing in a shipped path sets
# either, and the defaults are the real /dev and the real -c test.

# Is this path a device node? A hook keeps the test able to answer without
# mknod privileges, which an unprivileged container does not have.
iris_node_is_char() {
    if [ -n "${IRIS_NODE_IS_CHAR:-}" ]; then
        "$IRIS_NODE_IS_CHAR" "$1"
        return
    fi
    [ -c "$1" ]
}

# iris_node_decoder: echo the decoder node path. Fails when there is none.
iris_node_decoder() {
    if [ -n "${LIBVA_V4L2_VIDEO_PATH:-}" ]; then
        iris_node_is_char "$LIBVA_V4L2_VIDEO_PATH" \
            || iris_node_fail "LIBVA_V4L2_VIDEO_PATH is not a character device: $LIBVA_V4L2_VIDEO_PATH"
        printf '%s\n' "$LIBVA_V4L2_VIDEO_PATH"
        return 0
    fi

    command -v v4l2-ctl >/dev/null 2>&1 \
        || iris_node_fail 'v4l2-ctl is required to resolve the iris_driver decoder dynamically'

    iris_node_decoder_path=''
    iris_node_fallback=''
    iris_node_count=0
    for iris_node_candidate in "${IRIS_NODE_SEARCH_ROOT:-/dev}"/video*; do
        iris_node_is_char "$iris_node_candidate" || continue
        iris_node_info=$(v4l2-ctl -d "$iris_node_candidate" --info 2>/dev/null || true)
        iris_node_driver=$(printf '%s\n' "$iris_node_info" \
            | sed -n 's/^[[:space:]]*Driver name[[:space:]]*:[[:space:]]*//p' \
            | sed -n '1p')
        [ "$iris_node_driver" = iris_driver ] || continue

        iris_node_count=$((iris_node_count + 1))
        iris_node_fallback=$iris_node_candidate
        iris_node_card=$(printf '%s\n' "$iris_node_info" \
            | sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' \
            | sed -n '1p')
        case "$iris_node_card" in
            *[Dd]ecoder*)
                [ -z "$iris_node_decoder_path" ] \
                    || iris_node_fail "multiple iris_driver decoder nodes found: $iris_node_decoder_path and $iris_node_candidate"
                iris_node_decoder_path=$iris_node_candidate
                ;;
        esac
    done

    if [ -n "$iris_node_decoder_path" ]; then
        printf '%s\n' "$iris_node_decoder_path"
        return 0
    fi
    if [ "$iris_node_count" -eq 1 ]; then
        printf '%s\n' "$iris_node_fallback"
        return 0
    fi
    if [ "$iris_node_count" -gt 1 ]; then
        iris_node_fail 'multiple iris_driver nodes found but none identifies itself as the decoder'
    fi
    iris_node_fail 'no /dev/video* node reports iris_driver; refusing a fixed-node fallback'
}

# iris_node_encoder: echo the encoder node path, or nothing when the display
# has none. An absent encoder is not an error: the driver probes it as
# non-fatal and advertises no encode entrypoint.
iris_node_encoder() {
    if [ -n "${LIBVA_V4L2_ENCODER_PATH:-}" ]; then
        iris_node_is_char "$LIBVA_V4L2_ENCODER_PATH" \
            || iris_node_fail "LIBVA_V4L2_ENCODER_PATH is not a character device: $LIBVA_V4L2_ENCODER_PATH"
        printf '%s\n' "$LIBVA_V4L2_ENCODER_PATH"
        return 0
    fi

    command -v v4l2-ctl >/dev/null 2>&1 || return 0

    for iris_node_candidate in "${IRIS_NODE_SEARCH_ROOT:-/dev}"/video*; do
        iris_node_is_char "$iris_node_candidate" || continue
        iris_node_info=$(v4l2-ctl -d "$iris_node_candidate" --info 2>/dev/null || true)
        iris_node_driver=$(printf '%s\n' "$iris_node_info" \
            | sed -n 's/^[[:space:]]*Driver name[[:space:]]*:[[:space:]]*//p' \
            | sed -n '1p')
        [ "$iris_node_driver" = iris_driver ] || continue
        iris_node_card=$(printf '%s\n' "$iris_node_info" \
            | sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' \
            | sed -n '1p')
        case "$iris_node_card" in
            *[Ee]ncoder*)
                printf '%s\n' "$iris_node_candidate"
                return 0
                ;;
        esac
    done
    return 0
}

# A caller may define its own message format before sourcing this file.
iris_node_fail() {
    if command -v iris_node_fail_hook >/dev/null 2>&1; then
        iris_node_fail_hook "$*"
    fi
    echo "iris-node: $*" >&2
    exit 1
}
