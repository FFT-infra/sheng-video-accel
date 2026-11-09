#!/bin/sh

# Install the built VA driver into libva's configured system driver directory,
# then install the diagnostic commands under /usr/local by default. Run as
# root, or set DESTDIR for an unprivileged packaging test.

set -eu

usage() {
    cat <<'EOF'
Usage: install-system.sh [BUILD_DIR]

Environment:
  IRIS_INSTALL_PREFIX  Launcher/data prefix (default: /usr/local)
  IRIS_VA_DRIVER_DIR   Explicit libva driver directory override when
                       libva.pc is unavailable (for packaging/staging)
  DESTDIR              Optional package staging root
  IRIS_SOURCE_COMMIT   Full source object id when .git is unavailable
  IRIS_TRACKED_SOURCE_SHA256
                       SHA-256 over tracked source paths when .git is unavailable
  IRIS_SOURCE_DIRTY    0 when the caller verified a clean source tree and .git
                       is unavailable; anything else records a dirty install

The driver directory is read from libva.pc. This script never configures a
global LIBVA_DRIVERS_PATH and never writes a fixed /dev/video number.
EOF
}

fail() {
    echo "install-system.sh: $*" >&2
    exit 1
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac
[ "$#" -le 1 ] || { usage >&2; exit 2; }

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-$root/build}
case "$build_dir" in
    /*) ;;
    *) build_dir=$PWD/$build_dir ;;
esac
prefix=${IRIS_INSTALL_PREFIX:-/usr/local}
destdir=${DESTDIR:-}

case "$prefix" in /*) ;; *) fail 'IRIS_INSTALL_PREFIX must be absolute' ;; esac
case "$destdir" in ''|/*) ;; *) fail 'DESTDIR must be empty or absolute' ;; esac
case "$destdir" in *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail 'DESTDIR must be normalized' ;; esac

if [ -z "$destdir" ] && [ "$(id -u)" -ne 0 ]; then
    fail 'run as root for a live system install, or set DESTDIR for staging'
fi

for command in pkg-config install ldd sha256sum date; do
    command -v "$command" >/dev/null 2>&1 || fail "$command is required"
done

source_commit=''
source_dirty=1
source_digest=''
if command -v git >/dev/null 2>&1 && git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    source_commit=$(git -C "$root" rev-parse HEAD)
    [ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ] && source_dirty=0
    source_digest=$(cd "$root" && git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')
else
    source_commit=${IRIS_SOURCE_COMMIT:-}
    source_digest=${IRIS_TRACKED_SOURCE_SHA256:-}
    # Without .git this used to record every install as dirty, including a
    # clean packaged build, which made the field carry no information. The
    # caller may state the source state it verified; anything but an explicit
    # 0 stays dirty, so a missing or malformed value never upgrades a claim.
    case "${IRIS_SOURCE_DIRTY:-}" in
        0) source_dirty=0 ;;
        ''|1) source_dirty=1 ;;
        *) fail 'IRIS_SOURCE_DIRTY must be 0 or 1' ;;
    esac
fi
case "$source_commit" in
    ????????????????????????????????????????|????????????????????????????????????????????????????????????????) ;;
    *) fail 'source commit must be a full 40- or 64-character object id (set IRIS_SOURCE_COMMIT when .git is unavailable)' ;;
esac
case "$source_digest" in
    ????????????????????????????????????????????????????????????????) ;;
    *) fail 'tracked source SHA-256 is unavailable (set IRIS_TRACKED_SOURCE_SHA256 when .git is unavailable)' ;;
esac

driver_dir=${IRIS_VA_DRIVER_DIR:-}
if [ -z "$driver_dir" ]; then
    driver_dir=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
    if [ -z "$driver_dir" ]; then
        libdir=$(pkg-config --variable=libdir libva 2>/dev/null || true)
        [ -n "$libdir" ] || fail 'libva.pc does not provide driverdir or libdir; set IRIS_VA_DRIVER_DIR for staging'
        driver_dir=$libdir/dri
    fi
fi
case "$driver_dir" in /*) ;; *) fail "libva driverdir is not absolute: $driver_dir" ;; esac

artifact=$build_dir/src/v4l2_drv_video.so
[ -f "$artifact" ] || fail "driver artifact not found: $artifact"
undefined=$(ldd -r "$artifact" 2>&1 | grep -i 'undefined symbol' || true)
[ -z "$undefined" ] || fail "driver has unresolved symbols:\n$undefined"

artifact_sha256=$(sha256sum "$artifact" | awk '{print $1}')
artifact_size=$(wc -c <"$artifact" | tr -d '[:space:]')
manifest_dir=$destdir$prefix/share/iris-vaapi
manifest=$manifest_dir/install-manifest.txt
backup=''
previous_sha256=''
if [ -z "$destdir" ] && [ -f "$destdir$driver_dir/v4l2_drv_video.so" ]; then
    previous_sha256=$(sha256sum "$destdir$driver_dir/v4l2_drv_video.so" | awk '{print $1}')
    backup=$manifest_dir/backup/v4l2_drv_video.so
    install -D -m 0755 "$destdir$driver_dir/v4l2_drv_video.so" "$backup"
fi

install -D -m 0755 "$artifact" "$destdir$driver_dir/v4l2_drv_video.so"
# libva resolves the driver by DRM driver name when LIBVA_DRIVER_NAME is unset,
# and this SoC reports DRIVER=msm_dpu, so it looks for msm_drv_video.so. This
# alias is what lets mpv, VLC, GStreamer, Chrome and anything else work
# without an environment variable or a wrapper.
alias_path=$destdir$driver_dir/msm_drv_video.so
if [ -e "$alias_path" ] && [ ! -L "$alias_path" ]; then
    fail "$alias_path exists and is not a symlink; refusing to replace it"
fi
ln -sfn v4l2_drv_video.so "$alias_path"
# The node rule is one shared file, installed where both commands look for it.
install -D -m 0644 "$root/scripts/lib/iris-node.sh" \
    "$destdir$prefix/lib/iris-vaapi/iris-node.sh"
install -D -m 0755 "$root/scripts/iris-device-check.sh" \
    "$destdir$prefix/bin/iris-device-check"
install -D -m 0755 "$root/scripts/iris-verify.sh" \
    "$destdir$prefix/bin/iris-verify"
install -D -m 0755 "$root/scripts/iris-configure-clients.sh" \
    "$destdir$prefix/bin/iris-configure-clients"

mkdir -p "$manifest_dir"
manifest_new=$manifest.new.$$
{
    printf 'schema=qualcomm-iris-vaapi/system-install-v1\n'
    printf 'installed_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'source_commit=%s\nsource_dirty=%s\ntracked_source_sha256=%s\n' "$source_commit" "$source_dirty" "$source_digest"
    printf 'artifact_sha256=%s\nartifact_size=%s\n' "$artifact_sha256" "$artifact_size"
    # Record where the files will live on the target, not where they were
    # staged. A DESTDIR build otherwise bakes the build root into the manifest,
    # which rpmbuild's check-buildroot rejects outright and which would point
    # uninstall-system.sh at a path that does not exist on the installed
    # system. Consumers prepend their own DESTDIR.
    printf 'driver_path=%s\nnode_lib_path=%s\n' "$driver_dir/v4l2_drv_video.so" "$prefix/lib/iris-vaapi/iris-node.sh"
    printf 'driver_alias_path=%s\n' "$driver_dir/msm_drv_video.so"
    printf 'previous_driver_backup=%s\nprevious_driver_sha256=%s\n' "${backup#"$destdir"}" "$previous_sha256"
} >"$manifest_new"
mv -f "$manifest_new" "$manifest"

printf 'PASS installed VA driver: %s\n' "$destdir$driver_dir/v4l2_drv_video.so"
printf 'PASS installed driver alias: %s\n' "$alias_path"
printf 'PASS installed node rule: %s\n' "$destdir$prefix/lib/iris-vaapi/iris-node.sh"
printf 'PASS installed diagnostics: %s %s %s\n' \
    "$destdir$prefix/bin/iris-device-check" \
    "$destdir$prefix/bin/iris-verify" \
    "$destdir$prefix/bin/iris-configure-clients"
printf 'PASS install manifest: %s\n' "$manifest"
