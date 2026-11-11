#!/bin/sh

# Sync this checkout to the V4L2 target, verify that every tracked file
# arrived with the content this checkout holds, rebuild the driver from
# scratch, and refuse to hand back a library that cannot resolve its own
# symbols.
#
# This exists because an earlier debugging session lost most of a day to a
# stale incremental build: rsync preserved the original mtimes, Ninja decided
# the changed translation units were already up to date, and the resulting
# .so linked with undefined symbols. Every runtime symptom observed after that
# point was an artifact of the broken library, not of the driver logic. The
# clean rebuild plus the `ldd -r` gate below make that failure mode impossible
# to reach silently.
#
# Environment:
#   IRIS_REMOTE_HOST   ssh destination                (required)
#   IRIS_REMOTE_ROOT   remote checkout directory, relative to the
#                      remote home (default iris-vaapi-lab/src)
#   IRIS_DEPLOY_INCREMENTAL=1  skip the clean step (still refreshes mtimes)
#   IRIS_SOURCE_COMMIT full source object id (required only without local .git)

set -eu

host=${IRIS_REMOTE_HOST:-}
if [ -z "$host" ]; then
    echo 'FAIL IRIS_REMOTE_HOST is required for remote deployment' >&2
    exit 2
fi
if [ "${IRIS_REMOTE_ROOT+x}" = x ]; then
    remote_root=$IRIS_REMOTE_ROOT
else
    remote_root=iris-vaapi-lab/src
fi
local_root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)

fail_remote_root() {
    echo "FAIL unsafe IRIS_REMOTE_ROOT: '$remote_root'" >&2
    exit 1
}

# rsync --delete must only ever target a plain relative path below the remote
# user's home.  A character whitelist also rejects shell metacharacters,
# quotes, whitespace and newlines before they can reach ssh or rsync.
case "$remote_root" in
    ''|.|..|-*|/*|./*|../*|*/./*|*/.|*/../*|*/..|*//*|*/|*[!A-Za-z0-9_./-]*)
        fail_remote_root
        ;;
esac

for tool in rsync sha256sum python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "FAIL $tool is required to deploy" >&2
        exit 1
    fi
done

# The remote source tree intentionally excludes .git, so capture the source
# identity before rsync and inject it into the remote build explicitly. The
# digest is over the current contents of every tracked path, not merely the
# commit tree, so tracked local edits are distinguishable from a clean build.
source_commit=${IRIS_SOURCE_COMMIT:-}
source_dirty=1
incremental=${IRIS_DEPLOY_INCREMENTAL:-}
case "$incremental" in ''|1) ;; *) echo "FAIL IRIS_DEPLOY_INCREMENTAL must be empty or 1" >&2; exit 1 ;; esac
has_git=0
if command -v git >/dev/null 2>&1 \
    && git -C "$local_root" rev-parse --git-dir >/dev/null 2>&1; then
    has_git=1
fi
if [ "$has_git" -eq 1 ]; then
    source_commit=$(git -C "$local_root" rev-parse HEAD)
    if [ -z "$(git -C "$local_root" status --porcelain --untracked-files=all)" ]; then
        source_dirty=0
    fi
else
    case "$source_commit" in
        ''|*[!0-9A-Fa-f]*)
            echo "FAIL IRIS_SOURCE_COMMIT must inject a full object id when .git is absent" >&2
            exit 1
            ;;
    esac
fi
case "$source_commit" in
    ????????????????????????????????????????|????????????????????????????????????????????????????????????????) ;;
    *) echo "FAIL source commit must be a full 40- or 64-character object id" >&2; exit 1 ;;
esac

if [ "$has_git" -eq 1 ]; then
    source_manifest=$(cd "$local_root" && git ls-files -z | xargs -0 sha256sum)
    source_digest=$(printf '%s\n' "$source_manifest" | sha256sum | awk '{print $1}')
else
    # Without .git there is no file list to verify against; the caller's
    # digest is still recorded, but it cannot be checked on the target.
    source_manifest=
    source_digest=${IRIS_TRACKED_SOURCE_SHA256:-}
fi
case "$source_digest" in
    ????????????????????????????????????????????????????????????????) ;;
    *) echo "FAIL tracked source SHA-256 is unavailable" >&2; exit 1 ;;
esac

echo "== sync $local_root -> $host:$remote_root"
ssh -- "$host" "IRIS_REMOTE_ROOT='$remote_root' sh -s" <<'REMOTE_PREP'
set -eu
set -f
current=$HOME
old_ifs=$IFS
IFS=/
for component in $IRIS_REMOTE_ROOT; do
    [ -n "$component" ] || { echo "FAIL empty remote path component" >&2; exit 1; }
    current=$current/$component
    if [ -L "$current" ]; then
        echo "FAIL remote deploy path contains a symlink: $current" >&2
        exit 1
    fi
    if [ -e "$current" ]; then
        [ -d "$current" ] || { echo "FAIL remote deploy path is not a directory: $current" >&2; exit 1; }
    else
        mkdir -- "$current"
    fi
done
IFS=$old_ifs
REMOTE_PREP

# --checksum instead of the default size/mtime heuristic: the whole point of
# this script is that mtimes lie after a sync.
# The directory patterns end in a slash so they match directories only: the
# plain `build*` this replaced also dropped the tracked files whose names
# begin with "build", and the digest check below then had nothing to verify
# them against.
rsync -rlv --checksum --delete \
    --exclude '.git' \
    --exclude 'build*/' \
    --exclude 'output/' \
    --exclude 'chromium/' \
    --exclude '.playwright-cli/' \
    --exclude '*.mp4' \
    --exclude '*.ivf' \
    --exclude '.DS_Store' \
    -- "$local_root/" "$host:$remote_root/" | tail -20

# rsync is trusted to copy, not to have copied. The provenance gate below
# verifies the built library against an identity this host injected, so a
# sync that silently delivered an older tree would still produce a manifest
# that verifies. Re-hash every tracked file where it landed: the digest the
# manifest records is then a measured fact about the target, not a claim
# about the sender. A tracker-free tree has no file list, and says so.
if [ -n "$source_manifest" ]; then
    echo "== verify the synced tree against $source_digest"
    if ! printf '%s\n' "$source_manifest" \
        | ssh -- "$host" "cd \"\$HOME/$remote_root\" && sha256sum -c --quiet -"; then
        echo 'FAIL the synced tree does not match the tracked source digest' >&2
        exit 1
    fi
    echo 'PASS synced tree matches the tracked source digest'
fi

echo "== rebuild"
ssh -- "$host" "IRIS_REMOTE_ROOT='$remote_root' IRIS_SOURCE_COMMIT='$source_commit' IRIS_TRACKED_SOURCE_SHA256='$source_digest' IRIS_SOURCE_DIRTY='$source_dirty' IRIS_DEPLOY_INCREMENTAL='$incremental' sh -s" <<'REMOTE'
set -eu
cd "$HOME/$IRIS_REMOTE_ROOT"

# Defeat any mtime that survived the sync. Ninja compares mtimes, and a
# restored-from-backup or checksum-synced tree can carry timestamps older than
# the objects built from a previous revision.
find src include -type f \( -name '*.cc' -o -name '*.h' \) -exec touch {} +

if [ ! -f build/build.ninja ]; then
    meson setup build
else
    meson setup --reconfigure build >/dev/null
fi

if [ -z "${IRIS_DEPLOY_INCREMENTAL:-}" ]; then
    ninja -C build -t clean >/dev/null
fi
ninja -C build

so=build/src/v4l2_drv_video.so
if [ ! -f "$so" ]; then
    echo "FAIL driver was not produced at $so" >&2
    exit 1
fi

# ldd -r resolves data and function relocations. Anything reported here means
# the link succeeded but the library would fail at dlopen time inside libva,
# which surfaces as a silent fallback to software decoding.
undefined=$(ldd -r "$so" 2>&1 | grep -i 'undefined symbol' || true)
if [ -n "$undefined" ]; then
    echo "FAIL driver has undefined symbols:" >&2
    printf '%s\n' "$undefined" >&2
    exit 1
fi

manifest=build/iris-vaapi-provenance.json
python3 test/remote/provenance.py generate \
    --source-commit "$IRIS_SOURCE_COMMIT" \
    --tracked-source-sha256 "$IRIS_TRACKED_SOURCE_SHA256" \
    --source-dirty "$IRIS_SOURCE_DIRTY" \
    --build-dir build --artifact "$so" --output "$manifest"
python3 test/remote/provenance.py verify \
    --manifest "$manifest" --artifact "$so" \
    --expected-source-commit "$IRIS_SOURCE_COMMIT" \
    --expected-tracked-source-sha256 "$IRIS_TRACKED_SOURCE_SHA256" \
    --expected-source-dirty "$IRIS_SOURCE_DIRTY"

printf 'PASS driver built  sha256=%s  manifest=%s/%s  path=%s/%s\n' \
    "$(sha256sum "$so" | cut -d' ' -f1)" "$PWD" "$manifest" "$PWD" "$so"
REMOTE
