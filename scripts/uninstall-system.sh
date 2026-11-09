#!/bin/sh

# Remove an installation created by install-system.sh and restore the driver
# that was present before it. Refuses to remove a driver whose hash changed.

set -eu

prefix=${IRIS_INSTALL_PREFIX:-/usr/local}
destdir=${DESTDIR:-}
manifest=${IRIS_INSTALL_MANIFEST:-$destdir$prefix/share/iris-vaapi/install-manifest.txt}
fail() { echo "uninstall-system.sh: $*" >&2; exit 1; }
[ -f "$manifest" ] || fail "install manifest not found: $manifest"
get() { awk -F= -v key="$1" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$manifest"; }
schema=$(get schema)
[ "$schema" = qualcomm-iris-vaapi/system-install-v1 ] || fail "unsupported manifest schema: $schema"
# The manifest records target paths without DESTDIR so a packaged build does
# not carry its build root; a staged uninstall re-applies its own.
driver=$destdir$(get driver_path)
alias_path=$destdir$(get driver_alias_path)
expected=$(get artifact_sha256)
backup=$(get previous_driver_backup)
[ -n "$backup" ] && backup=$destdir$backup
[ -n "$driver" ] || fail 'manifest has no driver path'
case "$driver" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe driver path: $driver" ;;
esac
case "$driver" in /*/v4l2_drv_video.so) ;; *) fail "unexpected driver path: $driver" ;; esac
case "$alias_path" in
    ''|/*/msm_drv_video.so) ;;
    *) fail "unexpected driver alias path: $alias_path" ;;
esac

# Two layouts exist. The current one records the shared node rule; manifests
# written before the browser launcher was removed record a launcher path and a
# desktop directory instead. Both have to uninstall, or a machine that already
# has the old layout can never be cleaned up by the new script.
node_lib=$(get node_lib_path)
launcher=$(get launcher_path)
desktop_dir=$(get desktop_dir)
if [ -n "$node_lib" ]; then
    node_lib=$destdir$node_lib
    case "$node_lib" in
        *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe node rule path: $node_lib" ;;
    esac
    case "$node_lib" in /*/iris-vaapi/iris-node.sh) ;; *) fail "unexpected node rule path: $node_lib" ;; esac
elif [ -n "$launcher" ]; then
    launcher=$destdir$launcher
    desktop_dir=$destdir$desktop_dir
    case "$launcher" in
        *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe launcher path: $launcher" ;;
    esac
    case "$launcher" in /*/iris-vaapi-browser) ;; *) fail "unexpected launcher path: $launcher" ;; esac
    case "$desktop_dir" in
        ''|/*/applications) ;;
        *) fail "unexpected desktop directory: $desktop_dir" ;;
    esac
else
    fail 'manifest has neither a node rule path nor a launcher path'
fi
case "$backup" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe backup path: $backup" ;;
esac
case "$backup" in ''|/*/backup/v4l2_drv_video.so) ;; *) fail "unexpected backup path: $backup" ;; esac
[ -f "$driver" ] || fail "installed driver is missing: $driver"
[ -L "$driver" ] && fail "refusing to remove symlinked driver: $driver"

# The same layout now also ships as an RPM and an Arch package, and those
# install the manifest too. Deleting the files behind the package manager
# leaves it convinced they are still there, so the next upgrade or verify
# reports a corrupt package and reinstalling does not obviously fix it.
# Whoever owns the file gets to remove it.
if command -v rpm >/dev/null 2>&1 && rpm -qf "$driver" >/dev/null 2>&1; then
    fail "$driver belongs to $(rpm -qf "$driver"); remove it with dnf or rpm"
fi
if command -v pacman >/dev/null 2>&1 && pacman -Qo "$driver" >/dev/null 2>&1; then
    fail "$driver belongs to $(pacman -Qoq "$driver"); remove it with pacman"
fi

actual=$(sha256sum "$driver" | awk '{print $1}')
[ "$actual" = "$expected" ] || fail "refusing to remove modified driver (expected $expected, got $actual)"
# The alias is ours only while it points at our driver.
if [ -n "$alias_path" ] && [ -L "$alias_path" ] \
    && [ "$(readlink "$alias_path")" = v4l2_drv_video.so ]; then
    rm -f "$alias_path"
fi
rm -f "$driver"
if [ -n "$node_lib" ]; then
    # Current layout: the diagnostics sit in <prefix>/bin and the node rule in
    # <prefix>/lib/iris-vaapi, one directory each.
    for helper in iris-device-check iris-verify iris-configure-clients; do
        rm -f "$(dirname "$(dirname "$node_lib")")/bin/$helper" 2>/dev/null || true
    done
    rm -f "$node_lib"
    rmdir "$(dirname "$node_lib")" 2>/dev/null || true
else
    # Legacy layout: everything sat beside the launcher.
    rm -f "$launcher"
    for helper in iris-device-check iris-verify iris-configure-clients; do
        rm -f "$(dirname "$launcher")/$helper"
    done
    for desktop in "$desktop_dir"/*.desktop; do
        [ -f "$desktop" ] || continue
        case "$desktop" in
            *chromium-iris-v4l2.desktop|*google-chrome-iris-v4l2.desktop|*chromium-iris-vulkan-webgpu.desktop|*google-chrome-iris-vulkan-webgpu.desktop) rm -f "$desktop" ;;
        esac
    done
    rmdir "$desktop_dir" 2>/dev/null || true
fi
if [ -n "$backup" ] && [ -f "$backup" ]; then
    install -D -m 0755 "$backup" "$driver"
    rm -f "$backup"
fi
rm -f "$manifest"
rmdir "$(dirname "$manifest")/backup" "$(dirname "$manifest")" 2>/dev/null || true
printf 'PASS system installation removed; previous driver restored when recorded\n'
