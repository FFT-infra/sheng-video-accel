#!/bin/sh

# Create the source archive the Fedora spec and the Arch PKGBUILD consume.
#
#   sh scripts/make-dist.sh [--out DIR]
#
# The version comes from the Fedora spec, so the archive name and %autosetup
# cannot drift from the package. The archive is built from HEAD, not from the
# working tree: a source tarball should name a commit, and the install
# provenance manifest records that commit separately. A dirty tree is refused
# rather than silently packaging something other than the commit it names.
#
# .gitattributes marks CI, agent instructions and experiment records
# export-ignore, and git archive honours those rules for the tracked tree.
# Distribution packaging stays in: test/iris-packaging-check.sh checks all
# three formats from the unpacked tree, so a tarball without them would fail
# its own gate. test/iris-make-dist-check.sh asserts both halves.

set -eu

usage() {
    cat <<'EOF'
Usage: make-dist.sh [--out DIR]

Environment:
  (none)

The version is read from packaging/fedora/libva-v4l2-iris.spec. The default
output directory is packaging/arch, where makepkg expects the archive; pass
--out for a different consumer, for example:

  sh scripts/make-dist.sh --out ~/rpmbuild/SOURCES
EOF
}

fail() {
    echo "make-dist.sh: $*" >&2
    exit 1
}

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=$root/packaging/arch

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            out=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

spec=$root/packaging/fedora/libva-v4l2-iris.spec
[ -f "$spec" ] || fail "missing $spec"
version=$(sed -n 's/^Version:[[:space:]]*//p' "$spec" | head -n 1)
[ -n "$version" ] || fail 'the spec has no Version: field'
case "$version" in
    *[!0-9A-Za-z._+~-]*) fail "spec version is not archive-safe: $version" ;;
esac

git -C "$root" rev-parse --git-dir >/dev/null 2>&1 \
    || fail 'a git checkout is required to name the commit the archive contains'
[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ] \
    || fail 'the working tree is dirty; commit the change first'

case "$out" in
    /*) ;;
    *) out=$PWD/$out ;;
esac

name=libva-v4l2-iris-$version
mkdir -p "$out"
archive=$out/$name.tar.gz
git -C "$root" archive --format=tar.gz --prefix="$name/" -o "$archive" HEAD
printf '%s\n' "$archive"
