#!/bin/sh

# Shared environment resolution for the Iris test scripts. Source it as:
#
#   . "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"
#
# Two things used to be copy-pasted (and drift) across every script: the
# decoder node and the scratch directory.
#
# The node number is not stable. This device enumerated the decoder as
# /dev/video17 on one boot and /dev/video0 on the next, with the camera
# subsystem taking over the old number, so a hardcoded default silently points
# a "hardware decode" test at a camera. Resolve by driver name instead.
#
# The scratch directory used to default under a trash path that is wiped on a
# timer, which made corpus runs unreproducible a day later.

# iris_resolve_device: echo the decoder node.
# Honours an explicit override, otherwise probes for the iris driver. Fail
# closed when no Iris node is found: /dev/video0 may be a camera after reboot.
iris_resolve_device() {
    if [ -n "${LIBVA_V4L2_VIDEO_PATH:-}" ]; then
        printf '%s\n' "$LIBVA_V4L2_VIDEO_PATH"
        return 0
    fi
    if [ -n "${V4L2_DEVICE:-}" ]; then
        printf '%s\n' "$V4L2_DEVICE"
        return 0
    fi
    if command -v v4l2-ctl >/dev/null 2>&1; then
        for candidate in /dev/video*; do
            [ -c "$candidate" ] || continue
            if v4l2-ctl -d "$candidate" --info 2>/dev/null | grep -q 'iris_driver'; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done
    fi
    echo 'FAIL no /dev/video* node reports iris_driver; set LIBVA_V4L2_VIDEO_PATH only after checking the node' >&2
    return 1
}

# iris_resolve_encoder: echo the encoder node, or fail.
#
# The encoder is a second iris_driver node. A display without one still
# decodes, so a caller that only needs decode must not treat its absence as
# an error: check the exit status and skip instead. It is told apart from the
# decoder by the Card type, which is the only name the kernel gives the two.
iris_resolve_encoder() {
    if [ -n "${LIBVA_V4L2_ENCODER_PATH:-}" ]; then
        printf '%s\n' "$LIBVA_V4L2_ENCODER_PATH"
        return 0
    fi
    if command -v v4l2-ctl >/dev/null 2>&1; then
        for candidate in /dev/video*; do
            [ -c "$candidate" ] || continue
            info=$(v4l2-ctl -d "$candidate" --info 2>/dev/null || true)
            printf '%s\n' "$info" | grep -q 'Driver name.*iris_driver' || continue
            printf '%s\n' "$info" | grep -Eq 'Card type.*[Ee]ncoder' || continue
            printf '%s\n' "$candidate"
            return 0
        done
    fi
    echo 'FAIL no /dev/video* node reports the iris_driver encoder; set LIBVA_V4L2_ENCODER_PATH only after checking the node' >&2
    return 1
}

# iris_artifact_dir <name>: echo a durable directory for logs and media.
iris_artifact_dir() {
    lab=${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab}
    printf '%s/artifacts/%s\n' "$lab" "$1"
}
