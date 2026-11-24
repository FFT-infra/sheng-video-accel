#!/bin/sh

# Check a real-clip decode against its software framemd5 baseline and the
# session's completion contract. Pure text: the same file is used after a
# target run and by the host self-test with synthetic traces.
#
# The 48-frame synthetic matrix passed on a build that decoded only 33 of 120
# frames of a real AV1 clip. This checker is the gate that run was missing:
# every frame hash must match, every submitted picture must complete, and the
# publish counts must be the qualified policy's counts.

set -eu

usage() {
    cat >&2 <<'EOF'
usage: iris-clip-trace-check.sh --trace FILE --va-md5 FILE --sw-md5 FILE \
           --frames N --import N [--copy N] [--allow-invisible N]
EOF
    exit 2
}

trace=''
va_md5=''
sw_md5=''
frames=''
imports=''
copies=0
invisible=0

while [ "$#" -gt 0 ]; do
    case $1 in
        --trace) trace=$2; shift 2 ;;
        --va-md5) va_md5=$2; shift 2 ;;
        --sw-md5) sw_md5=$2; shift 2 ;;
        --frames) frames=$2; shift 2 ;;
        --import) imports=$2; shift 2 ;;
        --copy) copies=$2; shift 2 ;;
        --allow-invisible) invisible=$2; shift 2 ;;
        *) usage ;;
    esac
done

[ -n "$trace" ] && [ -n "$va_md5" ] && [ -n "$sw_md5" ] \
    && [ -n "$frames" ] && [ -n "$imports" ] || usage
for value in "$frames" "$imports" "$copies" "$invisible"; do
    case $value in ''|*[!0-9]*) usage ;; esac
done

failures=0
fail() {
    printf 'FAIL clip-check: %s\n' "$*" >&2
    failures=$((failures + 1))
}

for file in "$trace" "$va_md5" "$sw_md5"; do
    [ -r "$file" ] || { fail "not readable: $file"; exit 1; }
done

count() {
    grep -c "$1" "$trace" || true
}

# Frame hashes: the number of frames and the exact bytes of every hash line.
# A header-only or truncated decode must not pass on a matching count alone.
actual_frames=$(grep -c '^0,' "$va_md5" || true)
[ "$actual_frames" -eq "$frames" ] \
    || fail "framemd5 has $actual_frames frames, expected $frames"
sw_frames=$(grep -c '^0,' "$sw_md5" || true)
[ "$sw_frames" -eq "$frames" ] \
    || fail "software baseline has $sw_frames frames, expected $frames"
# framemd5 headers carry the libavformat version, which differs between the
# host that generated the baseline and the target that decoded it. Compare
# the frame hashes in order, not the tool that wrote the file. One awk over
# both files avoids process substitution and a temporary file.
if ! awk '
    FNR == NR { if (/^0,/) a[++n] = $0; next }
    /^0,/ { if (++m > n || $0 != a[m]) bad = 1 }
    END { if (m != n || bad) exit 1 }
' "$va_md5" "$sw_md5"; then
    fail "framemd5 differs from the software baseline"
fi

# One terminal session summary. This is the driver's own ledger, not a count
# of log lines that a truncated trace could satisfy by accident.
finish=$(grep '^session finish ' "$trace" | tail -1 || true)
[ -n "$finish" ] || { fail "no session finish line"; finish=''; }
field() {
    printf '%s\n' "$finish" | sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p"
}
for name in errors drops timeouts misplaced; do
    value=$(field "$name")
    [ "$value" = 0 ] || fail "$name=${value:-unset}, expected 0"
done
submitted=$(field submitted)
completed=$(field completed)
[ -n "$submitted" ] && [ -n "$completed" ] \
    || fail "session finish lacks submitted/completed"
[ "$submitted" = "$completed" ] \
    || fail "submitted=$submitted completed=$completed do not agree"
[ "${completed:-0}" -ge "$frames" ] \
    || fail "completed=${completed:-0} is below the $frames decoded frames"

# Publish policy: Import is the qualified path. A DRC clip may name an
# explicit number of copy-gpu publications for pictures the client kept
# exporting at the old geometry; anything beyond that is a regression.
# Trace lines end with a timestamp (`t=<ms>`), so match the field followed
# by whitespace or end of line rather than anchoring at $.
actual_imports=$(grep -E -c 'publish=import([[:space:]]|$)' "$trace" || true)
[ "$actual_imports" -eq "$imports" ] \
    || fail "publish=import is $actual_imports, expected $imports"
actual_copies=$(grep -E -c 'publish=copy-(gpu|cpu)([[:space:]]|$)' "$trace" || true)
[ "$actual_copies" -eq "$copies" ] \
    || fail "publish=copy-* is $actual_copies, expected $copies"
if [ "$copies" -gt 0 ]; then
    # The copy half of an in-context AV1 DRC is the correct fallback only
    # when the client kept exporting a buffer at the old CAPTURE geometry.
    # A copy without that reason is a silent Import regression.
    n=$(count '^import rejected: client layout differs from the CAPTURE layout')
    [ "$n" -gt 0 ] || fail "copy publications without a recorded layout mismatch"
fi
actual_invisible=$(grep -E -c 'publish=invisible([[:space:]]|$)' "$trace" || true)
[ "$actual_invisible" -eq "$invisible" ] \
    || fail "publish=invisible is $actual_invisible, expected $invisible"
for bad in 'publish=error$' 'publish=drop$' 'publish=misplaced' 'capture error index='; do
    n=$(count "$bad")
    [ "$n" -eq 0 ] || fail "$n occurrence(s) of $bad"
done

# A session sync timeout means the client never saw a completion. The default
# AV1 path has no held tail, but the session still emits this record for any
# codec when its bounded wait expires.
n=$(count 'sync timeout token=')
[ "$n" -eq 0 ] || fail "$n session sync timeout(s) at EOS"

[ "$failures" -eq 0 ] || exit 1
printf 'PASS clip-check frames=%s submitted=%s completed=%s import=%s copy=%s invisible=%s\n' \
    "$frames" "${submitted:-?}" "${completed:-?}" "$imports" "$copies" "$invisible"
