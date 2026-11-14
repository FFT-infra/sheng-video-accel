#!/bin/sh

# The Fedora spec and the Arch PKGBUILD both consume the tarball that
# scripts/make-dist.sh builds. A README instruction is not a test: derive one
# archive from a clean copy of HEAD and assert the prefix the two consumers
# unpack, the files their build reads, and the dirty-tree refusal. Working in
# a throwaway fixture keeps the developer's own uncommitted changes out of it.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
scratch_root=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-$HOME/Lab/Bridge/tmp}}
mkdir -p "$scratch_root"
work=$(mktemp -d "$scratch_root/iris-make-dist.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

fail() {
    echo "FAIL make-dist: $*" >&2
    exit 1
}

src=$work/src
out=$work/out
mkdir -p "$src" "$out"
# Copy the working tree, not HEAD: an edit to make-dist.sh that is not yet
# committed must still be the script this check exercises. Skip the build tree
# and other untracked/generated files so the fixture stays a clean git tree.
(cd "$root" && git ls-files -z | xargs -0 -I{} cp --parents "{}" "$src")
git -C "$src" init -q
git -C "$src" add -A
git -C "$src" -c user.name=make-dist-check \
    -c user.email=make-dist-check@localhost commit -qm 'archive fixture'

version=$(sed -n 's/^Version:[[:space:]]*//p' \
    "$src/packaging/fedora/libva-v4l2-iris.spec" | head -n 1)
[ -n "$version" ] || fail 'no Version: in the spec'
pkgver=$(sed -n 's/^pkgver=//p' "$src/packaging/arch/PKGBUILD" | head -n 1)
[ "$version" = "$pkgver" ] \
    || fail "spec Version $version and PKGBUILD pkgver $pkgver disagree"
name=libva-v4l2-iris-$version
prefix=$name/

archive=$(sh "$src/scripts/make-dist.sh" --out "$out")
[ -f "$archive" ] || fail "make-dist.sh did not create $archive"
case "$(basename -- "$archive")" in
    "$name.tar.gz") ;;
    *) fail "archive name does not name the spec version: $archive" ;;
esac

entries=$work/entries
tar tzf "$archive" >"$entries"
[ -s "$entries" ] || fail 'archive is empty'
grep -q "^$prefix\$" "$entries" || fail "archive has no $prefix root"
if grep -v "^$prefix" "$entries" | grep -q .; then
    fail 'an entry escapes the $prefix root'
fi
if grep -qE "^$prefix(\.git/|build[^/]*/|.*\.tar\.gz\$)" "$entries"; then
    fail 'archive carries git metadata, build output or another archive'
fi

# The archive is a source artifact, so CI, agent instructions and experiment
# records stay in the repository. .gitattributes carries the exclusions and
# git archive applies them. The negation matters: a removed or misspelled rule
# would otherwise ship these files with no failing check.
for path in \
    .gitattributes \
    .gitignore \
    .github/workflows/ci.yml \
    AGENTS.md \
    docs/experiments/av1-noshow-dpb-spike.md; do
    if grep -Fxq "$prefix$path" "$entries"; then
        fail "repository-only file leaked into the source archive: $path"
    fi
done

# Distribution packaging and the trace vocabulary stay in: the package's own
# %check runs these gates against the unpacked tree, so excluding either
# would make the tarball fail its own tests on an aarch64 build root.
for path in \
    meson_options.txt \
    debian/rules \
    docs/architecture.md; do
    grep -Fxq "$prefix$path" "$entries" || fail "archive is missing $path"
done

# Exactly the inputs the two package builds need. A missing one turns into a
# build failure only on an aarch64 build root, which is why it is checked here.
for path in \
    meson.build \
    data/iris-codec-capabilities.json \
    scripts/install-system.sh \
    scripts/refresh-install-manifest.sh \
    scripts/iris-device-check.sh \
    scripts/iris-verify.sh \
    scripts/iris-configure-clients.sh \
    scripts/lib/iris-node.sh \
    test/lib/iris-env.sh \
    packaging/fedora/libva-v4l2-iris.spec \
    packaging/arch/PKGBUILD \
    COPYING COPYING.LGPL COPYING.MIT \
    README.md README.zh-CN.md AUTHORS CREDITS; do
    grep -Fxq "$prefix$path" "$entries" || fail "archive is missing $path"
done

# A second build from the same commit must be byte-identical: the version and
# the file list come from the tree, not from the clock or the local checkout.
second=$work/second
mkdir -p "$second"
archive2=$(sh "$src/scripts/make-dist.sh" --out "$second")
cmp -s "$archive" "$archive2" || fail 'two builds from one commit differ'

# The refusal is the other half of the contract: packaging an uncommitted
# change under a commit it does not contain would make the provenance lie.
: >"$src/uncommitted"
if sh "$src/scripts/make-dist.sh" --out "$work/dirty" >"$work/dirty.out" 2>&1; then
    fail 'a dirty tree was packaged'
fi
grep -q 'working tree is dirty' "$work/dirty.out" \
    || fail 'dirty-tree failure did not name its cause'

echo "PASS make-dist: prefix, contents, repository-only exclusions, reproducibility and dirty refusal"
