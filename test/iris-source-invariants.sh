#!/bin/sh

# Cheap guards for the hardware test scripts themselves: they run on hosts
# without V4L2 hardware and keep the oracles fail-closed. Driver behaviour is
# covered by unit tests and the hardware gates, not by grepping src/.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
env_helper="$root/test/lib/iris-env.sh"
matrix="$root/test/iris-matrix.sh"
structure_matrix="$root/test/iris-structure-matrix.sh"
concurrency_soak="$root/test/iris-concurrency-soak.sh"
dynamic_resolution="$root/test/iris-dynamic-resolution.sh"
eightk_gate="$root/test/iris-8k-gate.sh"
two_player_gate="$root/test/iris-two-player-gate.sh"
encode_quality="$root/test/iris-encode-quality.sh"

# The two new target gates must stay exact pixel oracles and must refuse a
# pool that would reintroduce the 8K ENOMEM. These are text guards for the
# scripts themselves: the driver's pool sizing has a FakeDevice unit test.
if ! grep -Fq 'framemd5 differs from the software baseline' "$eightk_gate" \
    || ! grep -Fq 'CAPTURE pool=$pool exceeds the 8K memory budget' "$eightk_gate"; then
    echo "FAIL 8K gate no longer checks both exact pixels and the pool budget" >&2
    exit 1
fi
if ! grep -Fq 'framemd5 differs from the software baseline' "$two_player_gate" \
    || ! grep -Fq 'run_player a' "$two_player_gate" \
    || ! grep -Fq 'run_player b' "$two_player_gate"; then
    echo "FAIL two-player gate no longer runs two independent exact-pixel clients" >&2
    exit 1
fi
# An installed-package run must not be silently redirected to a build tree.
# The default has to stay empty and the driver export has to be conditional.
if grep -Eq 'driver_path=\$\{LIBVA_DRIVERS_PATH:-[^}]*build' "$two_player_gate" \
    || ! grep -Fq "set_driver=''" "$two_player_gate"; then
    echo "FAIL two-player gate forces a build-tree driver on installed-package runs" >&2
    exit 1
fi

if grep -Eq 'printf .* /dev/video0|echo .* /dev/video0' "$env_helper" \
    || ! grep -q 'no /dev/video\* node reports iris_driver' "$env_helper"; then
    echo "FAIL Iris test device resolver can silently fall back to a camera node" >&2
    exit 1
fi

# The positive hardware matrix is an exact pixel oracle. A decoder that exits
# successfully but emits different frames must fail the run rather than leave
# a warning in otherwise-green output.
if grep -q 'WARN .*content=frame-md5-mismatch' "$matrix" \
    || ! grep -q 'FAIL \$codec content=frame-md5-mismatch' "$matrix"; then
    echo "FAIL Iris matrix does not fail closed on pixel mismatches" >&2
    exit 1
fi

# Normal CAPTURE completion traces include error=0. Do not let a broad
# "capture.*error" matcher reject every successfully decoded frame.
if grep -q "capture\.\*error\\\\|V4L2_BUF_FLAG_ERROR" "$structure_matrix" \
    || ! grep -q 'capture error index=' "$structure_matrix"; then
    echo "FAIL Iris structure matrix treats successful CAPTURE logs as errors" >&2
    exit 1
fi

if grep -q "capture\[\^:\]\*error|undefined symbol" "$concurrency_soak" \
    || ! grep -q 'capture error index=' "$concurrency_soak"; then
    echo "FAIL concurrency soak treats successful CAPTURE logs as errors" >&2
    exit 1
fi
if grep -Eq 'mixed-vp9|preflight vp9|VP90' "$concurrency_soak"; then
    echo "FAIL concurrency qualification still exercises withdrawn VP9 VA" >&2
    exit 1
fi
if grep -Eq 'start_job mixed-hevc|start_repeated_job mixed-hevc' "$concurrency_soak" \
    || ! grep -q 'start_once_job mixed-hevc .*hevc-soak\.mkv' "$concurrency_soak"; then
    echo "FAIL mixed HEVC soak does not use one continuous duration-sized stream" >&2
    exit 1
fi
if ! grep -Fq 'hevc_soak_frames=$((duration * fps))' "$concurrency_soak" \
    || ! grep -q '"$media_dir/hevc-soak.mkv"' "$concurrency_soak"; then
    echo "FAIL mixed HEVC media does not span the requested soak duration" >&2
    exit 1
fi
scenario_block=$(sed -n '/run_scenario()/,/^}/p' "$concurrency_soak")
if printf '%s\n' "$scenario_block" | grep -q '^    name=\$1$' \
    || ! printf '%s\n' "$scenario_block" | grep -q '^    scenario_name=\$1$'; then
    echo "FAIL concurrency scenario label can be clobbered by POSIX shell function variables" >&2
    exit 1
fi

if ! grep -Fq "awk -F, 'NF >= 2 { print \$1 \",\" \$2 }'" "$dynamic_resolution"; then
    echo "FAIL dynamic-resolution switch count does not normalize ffprobe side-data columns" >&2
    exit 1
fi
if ! grep -Fq 'low-reference.yuv' "$dynamic_resolution" \
    || ! grep -Fq 'cat "$high_raw" >>"$software_raw"' "$dynamic_resolution"; then
    echo "FAIL dynamic-resolution native oracle uses a fixed-size rawvideo reference" >&2
    exit 1
fi
if grep -q 'va-same-context\|same-context path' "$dynamic_resolution" \
    || ! grep -q 'va-single-process' "$dynamic_resolution"; then
    echo "FAIL dynamic-resolution gate claims FFmpeg reuses one VA context" >&2
    exit 1
fi

# The encode-quality gate must free each codec's multi-MiB raw frames before
# the next codec allocates its own, or the target's small free space fills up.
if ! grep -Fq 'rm -f "$stream" "$decoded" "$ref" "$blank"' "$encode_quality"; then
    echo "FAIL encode-quality gate no longer releases each codec's scratch frames" >&2
    exit 1
fi
# Each large encode lane writes one 50 MiB frame at a time and must check free
# space before it starts, and its oracle must be the frame hashes rather than
# a whole-file compare: the two framemd5 writers differ in their header. The
# lane is parameterized (8K plus IRIS_ENCODE_EXTRA_GEOM), so match the guard
# inside check_large_lane, not an 8K-specific message.
if ! grep -Fq 'check_large_lane()' "$encode_quality" \
    || ! grep -Fq 'needs 512 MiB free in $work' "$encode_quality" \
    || ! grep -Fq "FNR == NR { if (/^0,/) a[++n] = \$0; next }" "$encode_quality"; then
    echo "FAIL encode-quality large lane no longer checks free space and frame hashes" >&2
    exit 1
fi

# A gate whose oracle is a hardcoded publish count is a statement about one
# exact media file. Without a pin the file can be replaced and the count goes
# stale silently: a replaced clip changed the recorded invisible count, which
# read as a driver regression. Both counting gates must
# verify the media identity and look the counts up by clip hash.
real_clip="$root/test/iris-real-clip-gate.sh"
grain_gate="$root/test/iris-av1-grain-gate.sh"
for gate in "$real_clip" "$grain_gate"; do
    if ! grep -Fq 'iris_require_media_pin "$clip_dir"' "$gate"; then
        echo "FAIL $(basename "$gate") does not pin its media directory" >&2
        exit 1
    fi
    if ! grep -Fq 'expected_counts()' "$gate" || ! grep -Fq 'iris_media_sha' "$gate"; then
        echo "FAIL $(basename "$gate") hardcodes counts instead of keying them by clip hash" >&2
        exit 1
    fi
done

echo "PASS source invariants"
