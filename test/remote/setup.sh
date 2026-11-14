#!/bin/sh

# Prepare a durable test environment on the V4L2 target.
#
# The previous layout kept the build tree, the Fluster corpus and Qualcomm's
# v4l-video-test-app under a scratch directory that is wiped on a timer, so a
# full corpus run could not be reproduced a day later. Everything this script
# creates lives outside that directory and is safe to keep.
#
# Idempotent: re-running it updates the external checkouts and leaves existing
# corpus downloads alone.
#
# Environment:
#   IRIS_REMOTE_HOST   ssh destination   (required)
#   IRIS_LAB_ROOT      remote lab root, relative to the remote home
#                      (default iris-vaapi-lab)
#   IRIS_LEGACY_TOOLS  optional existing tools directory on the remote host
#                      whose v4l-video-test-app / fluster checkouts are moved
#                      into the lab instead of cloned again

set -eu

host=${IRIS_REMOTE_HOST:-}
if [ -z "$host" ]; then
    echo 'FAIL IRIS_REMOTE_HOST is required for remote setup' >&2
    exit 2
fi
lab_root=${IRIS_LAB_ROOT:-iris-vaapi-lab}
fail_lab_root() {
    echo "FAIL unsafe IRIS_LAB_ROOT: '$lab_root'" >&2
    exit 1
}
case "$lab_root" in
    ''|.|..|-*|/*|./*|../*|*/./*|*/.|*/../*|*/..|*//*|*/|*[!A-Za-z0-9_./-]*)
        fail_lab_root
        ;;
esac
# The lab-root whitelist above makes this deliberate local interpolation safe.
# shellcheck disable=SC2087
ssh "$host" "sh -s" <<REMOTE
set -eu
lab="\$HOME/$lab_root"
mkdir -p "\$lab/src" "\$lab/artifacts" "\$lab/media" "\$lab/tools"

missing=''
for tool in meson ninja ffmpeg gst-launch-1.0 v4l2-ctl v4l2-compliance git python3 rsync; do
    command -v "\$tool" >/dev/null 2>&1 || missing="\$missing \$tool"
done
if [ -n "\$missing" ]; then
    echo "FAIL missing required tools:\$missing" >&2
    exit 1
fi
echo "PASS toolchain present"

# Adopt existing checkouts from IRIS_LEGACY_TOOLS instead of cloning again:
# the Fluster resources tree alone is >100 MB of downloaded conformance
# vectors, so a move (same filesystem, no copy) is cheaper than a fresh
# clone, and the caller may already have them somewhere durable.
adopt() {
    name=\$1
    legacy="\$IRIS_LEGACY_TOOLS/\$name"
    target="\$lab/tools/\$name"
    if [ -n "\$IRIS_LEGACY_TOOLS" ] && [ ! -e "\$target" ] && [ -d "\$legacy" ]; then
        mv "\$legacy" "\$target"
        printf 'PASS adopted %s from legacy tools directory\n' "\$name"
    fi
}
adopt v4l-video-test-app
adopt fluster

# Qualcomm's own V4L2 decoder client. It exercises the kernel interface
# directly, so a failure here separates firmware behaviour from this driver.
if [ ! -d "\$lab/tools/v4l-video-test-app/.git" ]; then
    git clone --quiet https://github.com/quic/v4l-video-test-app.git \
        "\$lab/tools/v4l-video-test-app"
fi
printf 'PASS v4l-video-test-app %s\n' \
    "\$(git -C "\$lab/tools/v4l-video-test-app" rev-parse --short HEAD)"

# Fluster drives the public conformance corpora. Its resources/ tree holds the
# downloaded vectors; never delete it to "clean up".
if [ ! -d "\$lab/tools/fluster/.git" ]; then
    git clone --quiet https://github.com/fluendo/fluster.git "\$lab/tools/fluster"
fi
printf 'PASS fluster %s (%s of vectors cached)\n' \
    "\$(git -C "\$lab/tools/fluster" rev-parse --short HEAD)" \
    "\$(du -sh "\$lab/tools/fluster/resources" 2>/dev/null | cut -f1 || echo none)"

free_kb=\$(df -Pk "\$HOME" | awk 'NR==2 {print \$4}')
if [ "\$free_kb" -lt 2097152 ]; then
    printf 'WARN only %s KiB free on the target; corpus runs may fail\n' "\$free_kb" >&2
fi

# The decoder node moved between boots on this device (an earlier session
# recorded /dev/video17, which is now the camera subsystem). Resolve it by
# driver name instead of hardcoding a number.
node=''
for candidate in /dev/video*; do
    [ -c "\$candidate" ] || continue
    if v4l2-ctl -d "\$candidate" --info 2>/dev/null | grep -q 'iris_driver'; then
        node="\$candidate"
        break
    fi
done
if [ -z "\$node" ]; then
    echo "FAIL no /dev/video* node reports iris_driver" >&2
    exit 1
fi
printf 'PASS decoder node %s\n' "\$node"
printf '%s\n' "\$node" > "\$lab/decoder-node"

printf 'PASS lab ready at %s\n' "\$lab"
REMOTE
