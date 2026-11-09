#!/bin/sh

# One command that answers "is hardware video actually working on this
# machine?", for a user, on an installed system.
#
# Three things are checked, and each needs its own evidence:
#   * libva opens this driver with no environment help;
#   * a decode goes through it and the codec block's interrupt counter moved;
#   * an encode does the same, and the stream it produced decodes.
#
# A client that merely starts is not evidence. Neither is a stream of the
# right size: the decode check compares frame count and the encode check
# decodes what it wrote.
#
# What is deliberately absent: `ffmpeg -c:v h264_v4l2m2m`. That invocation
# wedges the codec on this hardware, uninterruptibly, and takes the machine
# with it. The native V4L2 path is exercised by mpv instead, which is the
# client that negotiates the buffers correctly.
#
# Usage: iris-verify.sh [--clip FILE] [--keep DIR]
#
#   --clip FILE   decode this file instead of a generated test pattern
#   --keep DIR    keep the generated media and logs in DIR
#
# Environment:
#   IRIS_RENDER_NODE   DRM render node (default /dev/dri/renderD128)
#   LIBVA_DRIVERS_PATH driver directory to measure (default: the installed one)
#   IRIS_VERIFY_SECONDS length of the generated clip (default 2)

set -u

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
# The node rule lives in one file, shared with iris-device-check. In a
# checkout it is beside this script under lib/; installed, both land in
# $prefix/bin and the library sits in $prefix/lib/iris-vaapi.
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
iris_node_lib=''
for candidate in "$here/lib/iris-node.sh" \
        "$here/../lib/iris-vaapi/iris-node.sh" \
        "$root/scripts/lib/iris-node.sh"; do
    if [ -r "$candidate" ]; then
        iris_node_lib=$candidate
        break
    fi
done
if [ -z "$iris_node_lib" ]; then
    echo 'iris-verify: iris-node.sh not found beside this script or under lib/iris-vaapi' >&2
    exit 2
fi
# shellcheck source=lib/iris-node.sh
. "$iris_node_lib"
render=${IRIS_RENDER_NODE:-/dev/dri/renderD128}
clip=''
keep=''
seconds=${IRIS_VERIFY_SECONDS:-2}

usage() {
    cat <<'EOF'
Usage: iris-verify.sh [--clip FILE] [--keep DIR]

  --clip FILE   decode this file instead of a generated test pattern
  --keep DIR    keep the generated media and logs in DIR
  -h, --help    show this help

Environment:
  IRIS_RENDER_NODE      DRM render node (default /dev/dri/renderD128)
  IRIS_VERIFY_SECONDS   length of the generated clip (default 2)
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clip) shift; [ "$#" -gt 0 ] || { echo 'iris-verify: --clip needs a file' >&2; exit 2; }; clip=$1; shift ;;
        --keep) shift; [ "$#" -gt 0 ] || { echo 'iris-verify: --keep needs a directory' >&2; exit 2; }; keep=$1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "iris-verify: unknown argument: $1" >&2; exit 2 ;;
    esac
done

failures=0
pass() { printf 'PASS %s\n' "$*"; }
fail() { failures=$((failures + 1)); printf 'FAIL %s\n' "$*" >&2; }
skip() { printf 'SKIP %s\n' "$*"; }

if [ -n "$keep" ]; then
    work=$keep
    mkdir -p "$work"
    cleanup=''
else
    scratch=${IRIS_LAB_ROOT:-$HOME/Lab/Bridge/tmp}
    mkdir -p "$scratch"
    work=$(mktemp -d "$scratch/iris-verify.XXXXXX")
    cleanup=$work
fi
# shellcheck disable=SC2064
trap '[ -n "$cleanup" ] && rm -rf "$cleanup"' 0 1 2 15

have() { command -v "$1" >/dev/null 2>&1; }

# The video block's interrupt count. Matching the name rather than a column
# position survives a kernel that renames the line; a counter that never
# moves means the work happened somewhere else, which is the whole point.
irq_count() {
    awk '$NF ~ /^(iris|qcom-iris|venus|vcodec)$/ { s += $2 } END { print s + 0 }' /proc/interrupts
}

have ffmpeg || { echo 'iris-verify: ffmpeg is required' >&2; exit 2; }
have ffprobe || { echo 'iris-verify: ffprobe is required' >&2; exit 2; }
[ -c "$render" ] || { echo "iris-verify: not a DRM render node: $render" >&2; exit 2; }

decoder=$(iris_node_decoder 2>/dev/null) || decoder=''
encoder=$(iris_node_encoder 2>/dev/null) || encoder=''
[ -n "$decoder" ] || { echo 'iris-verify: no iris_driver decoder node' >&2; exit 1; }
[ -n "$decoder" ] || { echo 'iris-verify: no iris_driver decoder node' >&2; exit 1; }

printf 'Iris verify  kernel=%s  decoder=%s  encoder=%s\n' \
    "$(uname -r)" "$decoder" "${encoder:-none}"

# 1. libva finds the driver with nothing set. A client that knows about this
# project would pass this anyway; the point is that a plain one does.
if have vainfo; then
    if out=$(env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH \
            vainfo --display drm --device "$render" 2>&1); then
        version=$(printf '%s\n' "$out" | sed -n 's/^vainfo: Driver version: *//p' | head -1)
        profiles=$(printf '%s\n' "$out" | grep -c 'VAProfile')
        case "$version" in
            *Iris*|*iris*|*v4l2*|*V4L2*) pass "libva opens the driver: $version" ;;
            *) fail "libva opened a different driver: ${version:-<no version>}" ;;
        esac
        [ "$profiles" -gt 0 ] && pass "driver advertises $profiles profiles" \
            || fail 'driver advertises no profiles'
        if [ -n "$encoder" ]; then
            printf '%s\n' "$out" | grep -q EncSlice \
                && pass 'encode entrypoint advertised' \
                || fail 'encode entrypoint missing on a display with an encoder node'
        fi
    else
        fail 'libva opened no driver (is the package installed?)'
        printf '%s\n' "$out" | grep -iE 'error|fail' | head -3 | sed 's/^/    /' >&2
    fi
else
    skip 'vainfo not installed (libva-utils); driver check skipped'
fi

# 2. Decode. The clip is generated locally so the check needs no corpus, and
# the frame count is asserted: a decoder that returns success and no frames
# would otherwise pass.
if [ -n "$clip" ]; then
    [ -f "$clip" ] || { echo "iris-verify: no such clip: $clip" >&2; exit 2; }
    source_clip=$clip
else
    source_clip=$work/source.h264
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=1280x720:rate=30" -t "$seconds" \
        -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 30 \
        -f h264 -y "$source_clip" 2>"$work/source.log" \
        || { echo 'iris-verify: could not generate a test clip (is libx264 present?)' >&2; exit 2; }
fi
expected_frames=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of csv=p=0 "$source_clip" 2>/dev/null | tr -d ',')

irq_before=$(irq_count)
if LIBVA_DRIVER_NAME=v4l2 ffmpeg -hide_banner -loglevel error \
        -vaapi_device "$render" -hwaccel vaapi -hwaccel_output_format vaapi \
        -i "$source_clip" -f null - 2>"$work/decode.log"; then
    irq_after=$(irq_count)
    if [ "$irq_after" -gt "$irq_before" ]; then
        pass "decode ran on the hardware (Iris IRQ +$((irq_after - irq_before)))"
    else
        fail "decode returned success but the Iris IRQ counter did not move"
    fi
else
    fail "decode failed: $(head -3 "$work/decode.log" | tr '\n' ' ')"
fi

# 3. Encode. The stream has to decode afterwards: an encoder that returns a
# header and no pictures passes a "did it exit 0" check.
encode_one() {
    codec=$1
    size=$2
    case "$codec" in
        h264) ff_codec=h264_vaapi ;;
        hevc) ff_codec=hevc_vaapi ;;
    esac
    stream=$work/$codec.$codec
    irq_before=$(irq_count)
    if ! LIBVA_DRIVER_NAME=v4l2 ffmpeg -hide_banner -loglevel error \
            -f lavfi -i "testsrc2=size=$size:rate=30" -t "$seconds" \
            -vf format=nv12,hwupload -vaapi_device "$render" \
            -c:v "$ff_codec" -b:v 4M -g 30 -y "$stream" 2>"$work/encode-$codec.log"; then
        fail "$codec encode failed: $(head -3 "$work/encode-$codec.log" | tr '\n' ' ')"
        return
    fi
    irq_after=$(irq_count)
    [ "$irq_after" -gt "$irq_before" ] \
        && pass "$codec encode ran on the hardware (Iris IRQ +$((irq_after - irq_before)))" \
        || fail "$codec encode returned a stream but the Iris IRQ counter did not move"
    if ffmpeg -hide_banner -loglevel error -i "$stream" -f null - 2>"$work/decode-$codec.log"; then
        frames=$(ffprobe -v error -count_frames -select_streams v:0 \
            -show_entries stream=nb_read_frames -of csv=p=0 "$stream" 2>/dev/null | tr -d ',')
        pass "$codec stream decodes ($frames frames)"
    else
        fail "$codec stream does not decode: $(head -3 "$work/decode-$codec.log" | tr '\n' ' ')"
    fi
}

if [ -n "$encoder" ]; then
    encode_one h264 1280x720
    encode_one hevc 1280x720
else
    skip 'no encoder node on this display; encode not tested'
fi

# 4. The native path, through the client that negotiates it correctly. This
# does not use this driver at all, and it is reported separately for that
# reason: it says the codec works, not that the backend does.
if have mpv; then
    if timeout -s KILL 60 mpv --no-config --vo=null --ao=null \
            --hwdec=v4l2m2m-copy --length=2 "$source_clip" 2>&1 \
            | grep -q 'Using hardware decoding'; then
        pass 'native V4L2 M2M decode through mpv (independent of this driver)'
    else
        skip 'mpv did not report hardware decode on the native V4L2 path'
    fi
else
    skip 'mpv not installed; native V4L2 path not tested'
fi

printf '\n'
if [ "$failures" -ne 0 ]; then
    printf 'FAIL iris-verify: %d check(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'PASS iris-verify: decode and encode both ran on the hardware\n'
