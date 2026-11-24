#!/bin/sh

# The Fedora spec and the Arch PKGBUILD both consume the tarball that
# scripts/make-dist.sh builds. A README instruction is not a test: derive one
# archive from a clean copy of HEAD and assert the prefix the two consumers
# unpack, the files their build reads, and the dirty-tree refusal. Working in
# a throwaway fixture keeps the developer's own uncommitted changes out of it.
# An RPM build has already unpacked that archive, so the repository-side half
# of this check is skipped there; the packaging checks still run from it.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}

# %autosetup produces a tree with no .git, and the spec has no BuildRequires:
# git. Check the root itself rather than trusting rev-parse alone: inside a
# parent repository it would "succeed" against the wrong checkout.
if [ ! -e "$root/.git" ] || ! git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    printf 'NOTE make-dist: no git checkout at %s, archive self-check skipped\n' "$root"
    exit 0
fi

scratch_root=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-$HOME/iris-vaapi-lab/tmp}}
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

# The archive is a source artifact, so CI metadata and agent instructions stay
# in the repository. .gitattributes carries the exclusions and git archive
# applies them. The negation matters: a removed or misspelled rule would
# otherwise ship these files with no failing check.
for path in \
    .gitattributes \
    .gitignore \
    .github/workflows/ci.yml \
    AGENTS.md; do
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

# The archive we just built is exactly what %autosetup unpacks: no .git. Prove
# its own checker skips cleanly there. A failure would take the whole RPM
# %check down; a skip that also fires in a real checkout would hide the
# assertions above, so the two shapes are both exercised.
nogit=$work/nogit
mkdir -p "$nogit"
tar xzf "$archive" -C "$nogit"
unpacked=$nogit/$name
if ! sh "$unpacked/test/static/iris-make-dist-check.sh" "$unpacked" >"$work/nogit.out" 2>&1; then
    fail 'archive self-check did not skip the unpacked tarball'
fi
grep -Fq 'archive self-check skipped' "$work/nogit.out" \
    || fail 'no-git skip did not report its reason'

echo "PASS make-dist: prefix, contents, repository-only exclusions, reproducibility and dirty refusal"
