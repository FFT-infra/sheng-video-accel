#!/bin/sh

# Build only drivers/media/platform/qcom/iris against the running kernel.
# Reversible: it never installs a module and never reboots. The compressed
# module and a verified copy of the running module are left in the work
# directory for the operator to install after a separate decision.
#
#   sh packaging/kernel/build-module.sh --source /path/to/linux --patch 0006
#
# The default patch set is the four qualified patches. See README.md for the
# source tree, symbol-table and rollback rules this script enforces.

set -eu

usage() {
    cat >&2 <<'EOF'
usage: build-module.sh [--source DIR] [--work DIR] [--config FILE]
                       [--symvers FILE] [--patch NNNN]...
  --source    prepared ianchb/sm8550-mainline tree (default: $WORK/linux)
  --work      scratch root for logs and artifacts (default:
              ${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab}/tmp/iris-kernel-module)
  --config    running kernel config (default: /proc/config.gz, decompressed)
  --symvers   verified Module.symvers (default: regenerate from /proc/kallsyms)
  --patch     apply one numbered patch; repeatable, replaces the default set
EOF
    exit 2
}

kernel_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
work=${IRIS_KERNEL_BUILD_DIR:-${IRIS_LAB_ROOT:-$HOME/iris-vaapi-lab}/tmp/iris-kernel-module}
source_dir=
config=
symvers=
patches=

while [ "$#" -gt 0 ]; do
    case $1 in
        --source) [ "$#" -ge 2 ] || usage; source_dir=$2; shift 2 ;;
        --work) [ "$#" -ge 2 ] || usage; work=$2; shift 2 ;;
        --config) [ "$#" -ge 2 ] || usage; config=$2; shift 2 ;;
        --symvers) [ "$#" -ge 2 ] || usage; symvers=$2; shift 2 ;;
        --patch) [ "$#" -ge 2 ] || usage; patches="$patches $2"; shift 2 ;;
        --help|-h) usage ;;
        *) printf 'FAIL build-module: unknown argument %s\n' "$1" >&2; usage ;;
    esac
done

[ -n "$source_dir" ] || source_dir=$work/linux
[ -n "$patches" ] || patches='0001 0002 0003 0004'
mkdir -p "$work"
log=$work/build.log
: >"$log"
say() { printf '%s\n' "$*" | tee -a "$log"; }

[ -f "$source_dir/Makefile" ] || { echo "FAIL no kernel source tree at $source_dir" >&2; exit 2; }
command -v make >/dev/null || { echo 'FAIL make is unavailable' >&2; exit 2; }
command -v zstd >/dev/null || { echo 'FAIL zstd is unavailable' >&2; exit 2; }

iris_dir=$source_dir/drivers/media/platform/qcom/iris
[ -d "$iris_dir" ] || { echo "FAIL no Iris driver under $source_dir" >&2; exit 2; }
# git apply is not idempotent. A dirty Iris directory means the operator may
# be about to apply a second patch set on top of an unknown first one.
if dirty=$(git -C "$source_dir" status --porcelain -- drivers/media/platform/qcom/iris 2>/dev/null) \
        && [ -n "$dirty" ]; then
    echo "FAIL Iris source tree is already modified; start from a clean checkout" >&2
    printf '%s\n' "$dirty" >&2
    exit 1
fi

cd "$source_dir"
say "source_commit=$(git rev-parse HEAD)"
say "kernel_release=$(uname -r)"

# The running config is the only one that matches this module's vermagic. The
# /boot/config-* files on sheng differ from /proc/config.gz and there is no
# config named after the -iris1 release, so they are not a substitute.
if [ -n "$config" ]; then
    cp "$config" .config
else
    zcat /proc/config.gz > .config
fi
base=$(make -s kernelversion)
release=$(uname -r)
suffix=${release#"$base"}
[ "$suffix" != "$release" ] || {
    echo "FAIL running release $release does not start with source version $base" >&2
    exit 1
}
printf '%s' "$suffix" > localversion
say "localversion=$suffix"

if [ -n "$symvers" ]; then
    cp "$symvers" Module.symvers
    say "symvers=provided $symvers"
else
    awk '$4 ~ /^\[/ && $2 ~ /[TDR]/ {gsub(/[\[\]]/,"",$4); print "0x00000000\t"$3"\t"$4"\tEXPORT_SYMBOL\t"}' \
        /proc/kallsyms | grep -E 'videobuf2|videodev|v4l2_mem2mem' > Module.symvers
    say "symvers=regenerated from /proc/kallsyms ($(wc -l < Module.symvers) entries)"
fi
[ -s Module.symvers ] || { echo 'FAIL empty Module.symvers' >&2; exit 1; }

for n in $patches; do
    set -- "$kernel_dir"/"$n"-*.patch
    [ -f "$1" ] || { echo "FAIL no patch numbered $n in $kernel_dir" >&2; exit 2; }
    for p in "$@"; do
        say "apply $(basename "$p")"
        git apply "$p" >>"$log" 2>&1 || { echo "FAIL git apply $p" >&2; tail -20 "$log" >&2; exit 1; }
    done
done
say "patch_markers:"
if grep -rlF 'iris_hfi_gen2_requeue_output_buffer' "$iris_dir" >/dev/null 2>&1; then
    say '  0005 present (experimental; not part of the qualified module)'
else
    say '  0005 absent'
fi

LOCALVERSION=
export LOCALVERSION
say "build=olddefconfig modules_prepare"
make ARCH=arm64 LLVM=1 olddefconfig >>"$log" 2>&1
make ARCH=arm64 LLVM=1 -j"$(getconf _NPROCESSORS_ONLN)" modules_prepare >>"$log" 2>&1
say "build=M=drivers/media/platform/qcom/iris modules"
# KBUILD_MODPOST_WARN=1 is needed because the regenerated Module.symvers only
# carries the video subsystem exports; the remaining kernel exports are
# checked against the running kernel below instead.
make ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 \
    M=drivers/media/platform/qcom/iris modules >>"$log" 2>&1 \
    || { tail -40 "$log" >&2; exit 1; }

ko=$iris_dir/qcom-iris.ko
[ -f "$ko" ] || { echo "FAIL build produced no $ko" >&2; exit 1; }
vermagic=$(modinfo -F vermagic "$ko")
case $vermagic in
    "$release "*) say "vermagic=$vermagic" ;;
    *) echo "FAIL vermagic '$vermagic' does not match running release '$release'" >&2; exit 1 ;;
esac

# modpost's warnings are expected with a partial Module.symvers. What matters
# is that every symbol the module needs is really exported by this kernel.
if command -v llvm-nm >/dev/null; then nm_tool='llvm-nm'; else nm_tool='nm'; fi
"$nm_tool" -u "$ko" | awk '{print $2}' | sort -u > "$work/undefined.txt"
awk '$3 ~ /^__ksymtab_/ {sub(/^__ksymtab_/,"",$3); print $3}' /proc/kallsyms \
    | sort -u > "$work/exported.txt"
comm -23 "$work/undefined.txt" "$work/exported.txt" > "$work/missing-exports.txt"
if [ -s "$work/missing-exports.txt" ]; then
    echo 'FAIL undefined symbols are not exported by the running kernel:' >&2
    cat "$work/missing-exports.txt" >&2
    exit 1
fi
say "undefined_symbols=$(wc -l < "$work/undefined.txt") all exported by running kernel"

# A rollback copy that has never been compared against the installed module
# is not a rollback path.
installed=$(modinfo -F filename qcom-iris)
cp -f "$installed" "$work/rollback-qcom-iris.ko.zst"
cmp -s "$installed" "$work/rollback-qcom-iris.ko.zst" \
    || { echo 'FAIL rollback copy does not match the installed module' >&2; exit 1; }
say "installed_module_sha256=$(sha256sum "$installed" | cut -d' ' -f1)"
say "rollback_copy=verified"

zstd -19 -f "$ko" -o "$work/qcom-iris.ko.zst" >>"$log" 2>&1
say "built_module_sha256=$(sha256sum "$ko" | cut -d' ' -f1)"
say "built_module_zst_sha256=$(sha256sum "$work/qcom-iris.ko.zst" | cut -d' ' -f1)"
say "artifacts=$work"
say "NOT INSTALLED; no depmod, no reboot. Read README.md before installing."
