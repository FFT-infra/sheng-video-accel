#!/bin/sh

# Read-only report: does this device have the Iris codec, what does the driver
# expose on it, and which path should the reader take?
#
# Every answer comes from the running system. Nothing is written, no node is
# opened for streaming, and no node number is assumed: both nodes are resolved
# by the `iris_driver` name they report, so a boot that renumbers /dev/video*
# changes nothing here.
#
# Exit status is 0 when an Iris codec was found, 1 when it was not. A missing
# encoder node is reported, not failed: a display without one still decodes.

set -u

fail() {
    echo "iris-device-check: $*" >&2
    exit 2
}

have() { command -v "$1" >/dev/null 2>&1; }

# The node rule lives in one file, shared with iris-verify. In a checkout it is
# beside this script under lib/; installed, both land in $prefix/bin and the
# library sits in $prefix/lib/iris-vaapi.
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
for candidate in "$here/lib/iris-node.sh" \
        "$here/../lib/iris-vaapi/iris-node.sh" \
        "$(dirname -- "$here")/lib/iris-vaapi/iris-node.sh"; do
    if [ -r "$candidate" ]; then
        iris_node_lib=$candidate
        break
    fi
done
[ -n "${iris_node_lib:-}" ] || fail 'iris-node.sh not found beside this script or under lib/iris-vaapi'
# shellcheck source=lib/iris-node.sh
. "$iris_node_lib"
iris_node_fail_hook() { fail "$*"; }

hr() { printf '%s\n' '--------------------------------------------------------'; }

echo "Iris device check - $(date '+%F %T')"
echo "kernel: $(uname -r)   machine: $(uname -m)"
hr

# 1. The codec device in the device tree, and the driver bound to it. The
# IOMMU group type decides whether the codec shares an address space with the
# GPU, which is what makes a GPU blit between them cheap; it is also the first
# thing to look at when a copy path works but zero-copy does not.
echo '[1] codec platform device'
codec_devices=''
for d in /sys/bus/platform/devices/*video-codec*; do
    [ -e "$d" ] || continue
    codec_devices="$codec_devices $d"
    driver=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
    iommu=$(cat "$d/iommu_group/type" 2>/dev/null)
    printf '    %-34s driver=%s  iommu_group_type=%s\n' \
        "$(basename "$d")" "${driver:-<unbound>}" "${iommu:-<none>}"
done
[ -n "$codec_devices" ] || echo '    no *video-codec* platform device found'

# 2. The kernel side: is the driver built, is it the loaded module, and which
# module file is on disk. A patched module and a stock one both answer
# `qcom-iris`; the hash is what tells them apart in a bug report.
echo
echo '[2] kernel driver'
cfg=''
for f in /proc/config.gz "/boot/config-$(uname -r)" "/lib/modules/$(uname -r)/config"; do
    case "$f" in
        *.gz) [ -r "$f" ] && cfg="zcat $f" && break ;;
        *) [ -r "$f" ] && cfg="cat $f" && break ;;
    esac
done
if [ -n "$cfg" ]; then
    $cfg 2>/dev/null | grep -E '^CONFIG_VIDEO_QCOM_(IRIS|VENUS)' | sed 's/^/    /' \
        || echo '    CONFIG_VIDEO_QCOM_IRIS is not set in this kernel config'
else
    echo '    no kernel config readable'
fi
lsmod 2>/dev/null | awk '/^qcom_iris|^qcom-iris/ { printf "    loaded: %s (%s bytes, %s users)\n", $1, $2, $3 }'
module=$(find "/lib/modules/$(uname -r)" -name 'qcom-iris.ko*' 2>/dev/null | head -1)
if [ -n "$module" ]; then
    printf '    module: %s\n' "$module"
    if [ -r "$module" ]; then
        printf '    sha256: %s\n' "$(sha256sum "$module" | awk '{print $1}')"
    fi
else
    echo '    no qcom-iris.ko* under /lib/modules'
fi

# 3. Firmware. The device tree names the file the driver will request, and
# the name differs by SoC (this target asks for vpu30_4v.mbn, not the vpu35
# of a newer part). Read the name rather than guessing a pattern; then say
# whether the file is there, because a missing one is the difference between
# "no firmware" and "wrong firmware".
echo
echo '[3] firmware'
firmware_name=''
for node in /sys/bus/platform/devices/*video-codec*/of_node; do
    [ -r "$node/firmware-name" ] || continue
    firmware_name=$(tr -d '\0' <"$node/firmware-name")
    break
done
if [ -n "$firmware_name" ]; then
    printf '    device tree asks for: %s\n' "$firmware_name"
    if [ -r "/lib/firmware/$firmware_name" ]; then
        printf '    present: /lib/firmware/%s (%s bytes)\n' "$firmware_name" \
            "$(wc -c <"/lib/firmware/$firmware_name" | tr -d '[:space:]')"
    else
        printf '    MISSING: /lib/firmware/%s is not there\n' "$firmware_name"
    fi
else
    echo '    no firmware-name in the codec device tree node'
fi
found=$(find /lib/firmware -iname 'vpu*.mbn' 2>/dev/null | head -6)
if [ -n "$found" ]; then
    printf '%s\n' "$found" | sed 's/^/    vpu image: /'
else
    echo '    no vpu*.mbn anywhere under /lib/firmware'
fi
venus=$(find /lib/firmware -iname 'venus*.mbn' 2>/dev/null | head -3)
[ -n "$venus" ] && printf '%s\n' "$venus" | sed 's/^/    venus: /'

# 4. The two nodes, resolved by driver name. The decoder carries the coded
# formats on OUTPUT and the encoder carries them on CAPTURE; that is the only
# reliable way to tell them apart, and it is the rule the driver uses too.
echo
echo '[4] V4L2 nodes (resolved by iris_driver name)'
if ! have v4l2-ctl; then
    echo '    v4l2-ctl is not installed; install v4l-utils for the format inventory'
else
    decoder=$(iris_node_decoder 2>/dev/null) || decoder=''
    encoder=$(iris_node_encoder 2>/dev/null) || encoder=''
    if [ -n "$decoder" ]; then
        card=$(v4l2-ctl -d "$decoder" --info 2>/dev/null | sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' | sed -n '1p')
        coded=$(v4l2-ctl -d "$decoder" --list-formats-out 2>/dev/null | grep -oE "'[A-Z0-9]{4}'" | tr '\n' ' ')
        raw=$(v4l2-ctl -d "$decoder" --list-formats 2>/dev/null | grep -oE "'[A-Z0-9]{4}'" | tr '\n' ' ')
        printf '    decoder: %s  %s\n' "$decoder" "${card:-<no card type>}"
        printf '             decodes: %s\n' "${coded:-<none reported>}"
        printf '             outputs: %s\n' "${raw:-<none reported>}"
    else
        echo '    decoder: none found'
    fi
    if [ -n "$encoder" ]; then
        card=$(v4l2-ctl -d "$encoder" --info 2>/dev/null | sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' | sed -n '1p')
        coded=$(v4l2-ctl -d "$encoder" --list-formats 2>/dev/null | grep -oE "'[A-Z0-9]{4}'" | tr '\n' ' ')
        raw=$(v4l2-ctl -d "$encoder" --list-formats-out 2>/dev/null | grep -oE "'[A-Z0-9]{4}'" | tr '\n' ' ')
        printf '    encoder: %s  %s\n' "$encoder" "${card:-<no card type>}"
        printf '             encodes: %s\n' "${coded:-<none reported>}"
        printf '             accepts: %s\n' "${raw:-<none reported>}"
    else
        echo '    encoder: none found (decode-only display)'
    fi
fi

# 5. The kernel changes this project's gates lean on. Their absence is not a
# failure: the driver detects it and takes the slower path. Which one is
# present changes what a trace means, so report all four.
echo
echo '[5] optional kernel controls'
if [ -n "${decoder:-}" ] && have v4l2-ctl; then
    for control in display_delay min_number_of_capture_buffers; do
        if v4l2-ctl -d "$decoder" --list-ctrls 2>/dev/null | grep -q "$control"; then
            echo "    decoder $control: present"
        else
            echo "    decoder $control: absent"
        fi
    done
fi
if [ -n "${encoder:-}" ] && have v4l2-ctl; then
    if v4l2-ctl -d "$encoder" --list-ctrls 2>/dev/null | grep -q force_key_frame; then
        echo '    encoder force_key_frame: present'
    else
        echo '    encoder force_key_frame: absent'
    fi
    # The loop-filter controls are six, named per codec, and their exact
    # spellings differ from the kernel names a reader may expect
    # (`hevc_loop_filter`, not `hevc_loop_filter_mode`). List what is there
    # rather than testing a guessed name.
    loop_filter=$(v4l2-ctl -d "$encoder" --list-ctrls 2>/dev/null \
        | sed -n 's/^[[:space:]]*\([a-z0-9_]*loop_filter[a-z0-9_]*\).*/\1/p' | tr '\n' ' ')
    if [ -n "$loop_filter" ]; then
        printf '    encoder loop-filter controls: %s\n' "$loop_filter"
    else
        echo '    encoder loop-filter controls: none (firmware default deblocking only)'
    fi
fi
[ -n "${decoder:-}" ] || echo '    (no decoder node, controls not queried)'

# 6. Is the VA driver installed, and does libva find it without help? This is
# the question a client asks, so ask it the same way: no LIBVA_DRIVER_NAME,
# no LIBVA_DRIVERS_PATH.
echo
echo '[6] VA driver'
if have vainfo; then
    if out=$(env -u LIBVA_DRIVER_NAME -u LIBVA_DRIVERS_PATH \
            vainfo --display drm --device /dev/dri/renderD128 2>&1); then
        printf '%s\n' "$out" | grep -E 'vainfo: Driver version|va_openDriver' | sed 's/^/    /'
        profiles=$(printf '%s\n' "$out" | grep -c 'VAProfile')
        echo "    profiles: $profiles"
        printf '%s\n' "$out" | grep -q EncSlice \
            && echo '    encode entrypoint: advertised' \
            || echo '    encode entrypoint: not advertised'
    else
        echo '    libva opened no driver; the package is not installed'
        printf '%s\n' "$out" | grep -iE 'error|fail' | head -3 | sed 's/^/    /'
    fi
else
    echo '    vainfo is not installed; install libva-utils to check'
fi

# 7. The verdict. Say which path this device has, not just what was found.
echo
hr
echo 'VERDICT'
if [ -z "$codec_devices" ]; then
    echo '    No Qualcomm video codec platform device. Either the SoC has none,'
    echo '    or this kernel does not describe it yet. Nothing in this project applies.'
    exit 1
fi
case "$(for d in $codec_devices; do basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null; done | head -1)" in
    qcom-iris)
        echo '    Iris (HFI gen2) is present and the driver is bound.'
        echo '    - VA-API decode: this driver, installed through the package.'
        echo '    - VA-API encode: H.264 and HEVC when the encoder node above exists.'
        echo '    - Native V4L2 M2M: mpv and Firefox reach the same codec with no'
        echo '      backend at all (`--hwdec=v4l2m2m-copy` in mpv). That path is'
        echo '      independent of this driver and works either way.'
        ;;
    qcom-venus)
        echo '    Venus (the generation before Iris) is bound.'
        echo '    This backend targets Iris; use the distro libva package or'
        echo '    mpv --hwdec=v4l2m2m-copy, which needs no backend.'
        exit 1
        ;;
    *)
        echo '    A codec device exists but no supported driver is bound to it.'
        echo '    Check `dmesg | grep -iE "iris|venus|vcodec"` and the kernel config above.'
        exit 1
        ;;
esac
hr
