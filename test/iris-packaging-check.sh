#!/bin/sh

# Keep the distribution packaging honest about what actually gets installed.
#
# This project has already shipped the two failure modes this check exists to
# prevent. Desktop entries ended up in two directories at once because two
# install paths disagreed, and an entry for an absent browser stayed in the
# menu because nothing compared the packaged list against reality. RPM only
# catches an unpackaged file at build time, on a machine with the toolchain
# and the hardware arch; this runs everywhere, in CI, in a second.
#
# What it asserts:
#   * every path scripts/install-system.sh installs appears in the spec %files
#   * both manifests delegate to that script instead of open-coding a layout
#   * the runtime dependency the node rule cannot work without is declared
#   * the PKGBUILD parses as bash

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
spec=$root/packaging/fedora/libva-v4l2-iris.spec
pkgbuild=$root/packaging/arch/PKGBUILD
installer=$root/scripts/install-system.sh
refresh_manifest=$root/scripts/refresh-install-manifest.sh
failures=0

fail() {
    failures=$((failures + 1))
    printf 'FAIL packaging: %s\n' "$*" >&2
}

for path in "$spec" "$pkgbuild" "$installer" "$refresh_manifest"; do
    [ -f "$path" ] || { fail "missing $path"; exit 1; }
done

# The installer is the single source of truth for the layout. Read the paths
# out of it rather than restating them here, so a change there fails this
# check instead of silently diverging from the packaging.
#
# The patterns are the installer's literal source text, so the variables in
# them must stay unexpanded.
# shellcheck disable=SC2016
grep -Fq 'destdir$driver_dir/v4l2_drv_video.so' "$installer" \
    || fail 'install-system.sh no longer installs the driver where this check expects'
# shellcheck disable=SC2016
grep -Fq 'destdir$prefix/lib/iris-vaapi/iris-node.sh' "$installer" \
    || fail 'install-system.sh no longer installs the shared node rule where this check expects'
# libva resolves the driver by DRM name when the variable is unset, so the
# alias is a shipped file, not a convenience: both the installer and the spec
# must carry it or clients such as mpv silently fall back to software.
# shellcheck disable=SC2016
grep -Fq 'msm_drv_video.so' "$installer" \
    || fail 'install-system.sh no longer installs the msm alias libva resolves by DRM name'

expect_in_spec() {
    grep -Fq "$1" "$spec" || fail "spec %files does not list $1"
}

expect_in_spec '%{_libdir}/dri/v4l2_drv_video.so'
expect_in_spec '%{_libdir}/dri/msm_drv_video.so'
expect_in_spec '%{_prefix}/lib/iris-vaapi/iris-node.sh'
expect_in_spec '%{_datadir}/iris-vaapi/install-manifest.txt'

# The diagnostics source the shared node rule. A package that ships the rule
# without them leaves the documented `iris-verify` command missing, which is a
# worse outcome than shipping neither: the README tells the reader to run it.
for helper in iris-device-check iris-verify iris-configure-clients; do
    grep -Fq "\$prefix/bin/$helper" "$installer" \
        || fail "install-system.sh no longer installs $helper"
    expect_in_spec "%{_bindir}/$helper"
done

# No desktop entry is shipped. The driver needs no wrapper: libva resolves it
# through the msm_drv_video.so alias, and a client started from the desktop
# uses its own profile. A desktop entry reappearing here means someone is
# reintroducing a launcher, so fail loudly rather than silently packaging it.
for desktop in "$root"/data/*.desktop; do
    [ -f "$desktop" ] || continue
    fail "unexpected desktop entry: ${desktop##*/}; the driver ships no launcher"
done
# Nothing may ship the removed launcher either. The uninstaller is allowed to
# name it: it has to clean up a manifest written before the removal, and that
# branch is the only place the name may appear.
if grep -rFq 'iris-vaapi-browser' "$root/data" "$root/packaging" \
        || grep -Fq 'iris-vaapi-browser' "$root/scripts/install-system.sh"; then
    fail 'the removed browser launcher is still shipped by data, packaging or the installer'
fi

# Both manifests must delegate rather than open-code the layout.
grep -Fq 'scripts/install-system.sh' "$spec" \
    || fail 'spec does not install through scripts/install-system.sh'
grep -Fq 'scripts/install-system.sh' "$pkgbuild" \
    || fail 'PKGBUILD does not install through scripts/install-system.sh'
grep -Fq 'scripts/refresh-install-manifest.sh' "$spec" \
    || fail 'spec does not refresh the manifest after ELF post-processing'
grep -Fq 'scripts/refresh-install-manifest.sh' "$pkgbuild" \
    || fail 'PKGBUILD does not refresh the manifest after ELF post-processing'
grep -Fq '%{__brp_strip}' "$spec" \
    || fail 'spec does not model Fedora ELF stripping before manifest refresh'
grep -Fq '%{__brp_strip_lto}' "$spec" \
    || fail 'spec does not model Fedora LTO stripping before manifest refresh'
grep -Fq '%global debug_package %{nil}' "$spec" \
    || fail 'spec can generate an empty debugsource package after staged stripping'
grep -Fq "options=('!strip')" "$pkgbuild" \
    || fail 'PKGBUILD allows makepkg to strip after manifest refresh'
grep -Fq 'strip -g' "$pkgbuild" \
    || fail 'PKGBUILD does not explicitly strip before manifest refresh'
grep -Fq 'IRIS_VA_DRIVER_DIR' "$spec" \
    || fail 'spec does not pin the libva driver directory'
grep -Fq 'IRIS_VA_DRIVER_DIR' "$pkgbuild" \
    || fail 'PKGBUILD does not pin the libva driver directory'

# The node rule exits rather than guessing a node number when v4l2-ctl is
# missing, so a package without v4l-utils installs diagnostics that cannot
# run.
grep -Eq '^Requires:[[:space:]]+v4l-utils' "$spec" \
    || fail 'spec does not require v4l-utils'
grep -Fq 'v4l-utils' "$pkgbuild" \
    || fail 'PKGBUILD does not depend on v4l-utils'

if command -v bash >/dev/null 2>&1; then
    bash -n "$pkgbuild" || fail 'PKGBUILD does not parse as bash'
else
    printf 'WARN packaging: bash is unavailable, PKGBUILD not parsed\n' >&2
fi

if [ "$failures" -ne 0 ]; then
    printf 'FAIL packaging manifests: %d problem(s)\n' "$failures" >&2
    exit 1
fi
printf 'PASS packaging manifests cover the driver, the node rule and one install path\n'
