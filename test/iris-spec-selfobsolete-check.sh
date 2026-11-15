#!/bin/sh

# A package that both Provides and Obsoletes the same name, with an Obsoletes
# bound covering its own EVR, makes DNF schedule the package twice. That shape
# was measured in a downstream package spec.
# This spec replaces libva-v4l2 without providing that name. Guard that choice
# instead of relying on rpmspec -P, which parses macros but cannot see the
# solver behaviour, or on an aarch64 buildroot CI does not run.
#
#   sh iris-spec-selfobsolete-check.sh [--self-test] [ROOT]
#
# --self-test also writes a spec with the known-bad shape to scratch and
# requires the guard to reject it, so a guard that can no longer fail does not
# pass forever.

set -eu

root=${2:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
spec=$root/packaging/fedora/libva-v4l2-iris.spec

# Return 0 when a name in Provides is also named by Obsoletes. rpmspec would
# expand the EVR here; the fix keeps the two name sets disjoint, so comparing
# names is enough and needs no rpm tooling on a host or CI runner. Fixed-string
# matching keeps a name containing '.' or '+' from acting as a regex.
self_obsolete() {
    provides=$(sed -n 's/^Provides:[[:space:]]*\([A-Za-z0-9_.+-]*\).*/\1/p' "$1")
    obsoletes=$(sed -n 's/^Obsoletes:[[:space:]]*\([A-Za-z0-9_.+-]*\).*/\1/p' "$1")
    for name in $provides; do
        if printf '%s\n' "$obsoletes" | grep -Fxq "$name"; then
            return 0
        fi
    done
    return 1
}

if [ "${1:-}" = --self-test ]; then
    scratch_root=${IRIS_TEST_SCRATCH_ROOT:-$HOME/Lab/Bridge/tmp}
    mkdir -p "$scratch_root"
    work=$(mktemp -d "$scratch_root/iris-spec-selfobsolete.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    cat >"$work/bad.spec" <<'EOF'
Name:    libva-v4l2-iris
Version: 0.1.0
Release: 3%{?dist}
Provides:  libva-v4l2 = %{version}-%{release}
Obsoletes: libva-v4l2 < 1.2
EOF
    self_obsolete "$work/bad.spec" \
        || { echo 'FAIL self-obsolete guard misses the known-bad shape' >&2; exit 1; }
    self_obsolete "$spec" \
        && { echo "FAIL self-obsolete guard rejects the shipped spec: $spec" >&2; exit 1; }
    echo 'PASS self-obsolete guard: red on the bad shape, green on the shipped spec'
    exit 0
fi

[ -f "$spec" ] || { echo "FAIL missing $spec" >&2; exit 1; }
if self_obsolete "$spec"; then
    echo "FAIL $spec both Provides and Obsoletes one name; DNF can install it twice" >&2
    exit 1
fi
echo 'PASS spec does not obsolete a name it provides'
