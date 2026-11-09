#!/bin/sh

# Compare this driver against libavcodec on the HEVC conformance vectors that
# Phase 7 recorded as pixel-different, using normalised output.
#
# A raw framemd5 hashes the decoder's frame layout as well as its pixels, so a
# stride difference makes identical pictures hash differently. Both sides are
# normalised to the same pixel format before hashing here.
#
# Usage:
#   test/iris-hevc-conformance.sh [--resources DIR] [--driver PATH]
#
# --resources defaults to $IRIS_LAB_ROOT/tools/fluster/resources/JCT-VC-HEVC_V1

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=lib/iris-env.sh
. "$here/lib/iris-env.sh"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

lab=${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab}
resources=$lab/tools/fluster/resources/JCT-VC-HEVC_V1
driver=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --resources) resources=${2:?}; shift 2 ;;
        --driver) driver=${2:?}; shift 2 ;;
        -h|--help) sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[ -d "$resources" ] || fail "conformance resources not found: $resources"
[ -f "$driver" ] || fail "driver not found: $driver"
driver=$(CDPATH='' cd -- "$(dirname -- "$driver")" && pwd)/$(basename -- "$driver")
driver_dir=$(dirname "$driver")
render=${IRIS_RENDER_DEVICE:-/dev/dri/renderD128}
out=$(iris_artifact_dir hevc-conformance)/$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

va_env="LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=$driver_dir"

check() {
    name=$1
    format=$2
    bit=$resources/$name/$name/$name.bit
    [ -f "$bit" ] || { echo "SKIP $name (no $bit)"; return 0; }
    # `format=` normalises libavcodec's frame to the same layout the VA path
    # downloads into, so the hash covers pixels only.
    ffmpeg -v error -i "$bit" -vf "format=$format" -pix_fmt "$format" \
        -f framemd5 -y "$out/$name-sw.md5"
    # shellcheck disable=SC2086
    env $va_env ffmpeg -v error -hwaccel vaapi -hwaccel_device "$render" \
        -hwaccel_output_format vaapi -i "$bit" -vf "hwdownload,format=$format" \
        -f framemd5 -y "$out/$name-va.md5" || fail "$name: VA decode failed"

    frames=$(grep -vc '^#' "$out/$name-sw.md5" || true)
    if diff "$out/$name-sw.md5" "$out/$name-va.md5" >/dev/null; then
        echo "PASS $name frames=$frames format=$format md5-identical"
    else
        echo "FAIL $name frames=$frames format=$format"
        return 1
    fi
}

printf 'Iris HEVC conformance driver=%s sha256=%s boot=%s\n' \
    "$driver" "$(sha256sum "$driver" | cut -d' ' -f1)" \
    "$(cat /proc/sys/kernel/random/boot_id)"

status=0
check DELTAQP_A_BRCM_4 nv12 || status=1
check PMERGE_E_TI_3 nv12 || status=1
check INITQP_B_Main10_Sony_1 p010le || status=1
echo "artifacts: $out"
exit "$status"
