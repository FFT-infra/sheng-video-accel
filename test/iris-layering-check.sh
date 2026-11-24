#!/bin/sh

# The include direction docs/architecture.md §5 states, and its module list.
#
# src/ is four layers, each including only the ones below it:
# util <- codec <- iris <- va. iris/ takes one header from codec/, the
# Annex-B splitter the encoder session reassembles access units with, and
# nothing else: a translator reaching into the session, or the session into
# a translator, is how the two stop being testable apart.
#
# §5 lists every file under src/ by name. A file the list does not name is a
# module nobody was told about.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
src="$root/src"
doc="$root/docs/architecture.md"
status=0

fail() {
    echo "FAIL $*" >&2
    status=1
}

[ -d "$src" ] || { echo "FAIL $src is not a directory" >&2; exit 2; }
[ -r "$doc" ] || { echo "FAIL $doc is not readable" >&2; exit 2; }

# One line per quoted include: "<file> <layer of file> <layer of target> <path>".
# A path that names no layer after its leading ../ segments stays in its own.
edges=$(find "$src" -type f \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) | LC_ALL=C sort | while read -r file; do
    rel=${file#"$src"/}
    layer=${rel%%/*}
    sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$file" | while read -r path; do
        target=$path
        while :; do
            case $target in
                ../*) target=${target#../} ;;
                *) break ;;
            esac
        done
        case $target in
            util/*|codec/*|iris/*|va/*) to=${target%%/*} ;;
            *) to=$layer; target=$path ;;
        esac
        printf '%s %s %s %s\n' "$rel" "$layer" "$to" "$target"
    done
done)

[ -n "$edges" ] || fail "no quoted include found under src/; the scan matched nothing"

bad=$(printf '%s\n' "$edges" | awk '
    $2 == $3 { next }
    $2 == "va" && ($3 == "util" || $3 == "codec" || $3 == "iris") { next }
    $2 == "iris" && $3 == "util" { next }
    $2 == "iris" && $4 == "codec/annexb.h" { next }
    $2 == "codec" && $3 == "util" { next }
    { print "  " $1 " includes " $4 }')
if [ -n "$bad" ]; then
    fail "an include goes against util <- codec <- iris <- va:"
    printf '%s\n' "$bad" >&2
fi

# The scan has to be able to fail. iris/encode.h is the one iris -> codec
# edge, so it must be seen; if it is not, the pattern above stopped matching.
printf '%s\n' "$edges" | grep -Fqx 'iris/encode.h iris codec codec/annexb.h' \
    || fail "the scan no longer sees iris/encode.h including codec/annexb.h"

layout=$(sed -n '/^## 5\. Module layout/,/^## 6\. /p' "$doc")
[ -n "$layout" ] || fail "docs/architecture.md has no '## 5. Module layout' section"
missing=$(find "$src" -type f \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) | LC_ALL=C sort | while read -r file; do
    name=$(basename "$file")
    stem=${name%.*}
    # Either spelling: the full name, or the stem in front of a {h,cc} group.
    printf '%s\n' "$layout" | grep -Fq -e "$name" -e "$stem.{" || echo "  ${file#"$root"/}"
done)
if [ -n "$missing" ]; then
    fail "docs/architecture.md §5 does not name:"
    printf '%s\n' "$missing" >&2
fi

[ "$status" -eq 0 ] || exit 1
echo "PASS layering and module list"
