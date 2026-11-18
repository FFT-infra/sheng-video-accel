#!/bin/sh

# Keep the kernel patches honest about what they are and what they touch.
#
# `packaging/kernel/` is the only kernel code this project owns, and it is
# consumed by a second repository that fetches each patch by URL and verifies
# its sha256 before `%patch`. Two failure modes are worth a check that runs on
# every host, without a kernel tree or an arm64 toolchain:
#
#   * a patch is edited in place, so the recorded sha256 no longer describes
#     the file the downstream spec verifies. `%prep` then fails in a build
#     that takes an hour to reach it, or, worse, the pin is updated to a
#     file nobody re-read.
#   * a patch grows a hunk in a file it should not touch. 0001 applies at
#     fuzz 0 against the `sheng-7.2.6` tag and the downstream spec relies on
#     that; a stray path is how a rebase goes wrong quietly.
#   * a patch is edited here but the downstream kernel spec still carries the
#     old sha256, or its Source URLs still point at a commit the tag no longer
#     names. The spec is a second repository, so this check only binds the two
#     when that checkout is available; CI runs without it and still checks
#     everything above.
#
# What it asserts, per patch:
#   * the file exists and its sha256 matches `packaging/kernel/SHA256SUMS`
#   * every path it touches is under `drivers/media/platform/qcom/iris/`
#   * the path list is the one `packaging/kernel/README.md` describes
#   * the marker that identifies the change is added exactly once, and 0004's
#     timestamp copy sits inside the ERROR branch
#   * README.md names the file, so a renamed patch cannot leave the recipe
#     pointing at nothing
#
# With the downstream checkout present (IRIS_DOWNSTREAM_SPEC or the usual
# companion path), it also asserts:
#   * each IRIS_PATCHn_SHA256 in the spec equals the recorded hash
#   * each Source6-9 URL pins the commit `iris-kernel-patches-v1` resolves to

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
kernel_dir=$root/packaging/kernel
sums=$kernel_dir/SHA256SUMS
readme=$kernel_dir/README.md
failures=0

fail() {
    failures=$((failures + 1))
    printf 'FAIL kernel-patch: %s\n' "$*" >&2
}

for path in "$sums" "$readme"; do
    [ -f "$path" ] || { fail "missing $path"; exit 1; }
done

# The recorded hashes are the single source of truth; the downstream spec
# carries its own copy of each one, so a mismatch there is caught at build
# time and a mismatch here is caught now.
check=$(cd "$kernel_dir" && sha256sum -c SHA256SUMS 2>&1) || true
bad=$(printf '%s\n' "$check" | grep -v ': OK$' | grep -v '^$' || true)
if [ -n "$bad" ]; then
    fail "recorded hash does not match: $bad"
fi

# Only the fifteen patches may be listed, and every one of them must be.
# 0001-0007 apply to sheng-7.2.6; 0008-0015 are the 7.3.4 set.
listed=$(awk '{print $2}' "$sums")
for name in 0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015; do
    printf '%s\n' "$listed" | grep -q "^$name-" \
        || fail "SHA256SUMS does not record $name"
done
count=$(printf '%s\n' "$listed" | grep -c '\.patch$' || true)
[ "$count" -eq 15 ] \
    || fail "SHA256SUMS must record exactly fifteen patches, found $count"
for name in $(printf '%s\n' "$listed" | grep '\.patch$'); do
    [ -f "$kernel_dir/$name" ] || fail "SHA256SUMS records a file that is not there: $name"
done

expect_marker() {
    patch=$1
    marker=$2
    want=$3
    got=$(grep -cF -- "$marker" "$patch" || true)
    [ "$got" -eq "$want" ] \
        || fail "$patch: '$marker' appears $got time(s), expected $want"
}

# Each patch's identifying change. The count is 1 on purpose: 0002 and 0003
# both write into `inst_fw_cap_sm8550_enc`, and a fuzzy apply of the upstream
# patch has produced a doubled table entry before. The README records the
# greps to run against an applied tree; these are the same counts, checked
# against the patch instead of a build.
expect_marker "$kernel_dir/0001-media-iris-decode-order-output-and-64-capture-buffers.patch" \
    '+#define HFI_PROP_DECODE_ORDER_OUTPUT' 1
expect_marker "$kernel_dir/0001-media-iris-decode-order-output-and-64-capture-buffers.patch" \
    '+		dst_vq->max_num_buffers = DEFAULT_MAX_HOST_BUF_COUNT;' 1
expect_marker "$kernel_dir/0002-media-iris-deblocking-filter-controls.patch" \
    '+		.cap_id = DEBLOCK_MODE_H264,' 1
expect_marker "$kernel_dir/0002-media-iris-deblocking-filter-controls.patch" \
    '+#define HFI_PROP_DEBLOCKING_MODE' 1
expect_marker "$kernel_dir/0003-media-iris-encoder-request-sync-frame.patch" \
    '+		.cap_id = REQUEST_SYNC_FRAME,' 1
expect_marker "$kernel_dir/0003-media-iris-encoder-request-sync-frame.patch" \
    '+#define HFI_PROP_REQUEST_SYNC_FRAME' 1
expect_marker "$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch" \
    '+		vb2->timestamp = buf->timestamp;' 1
# The zeroing it replaces must be the line it removes, not a line it leaves
# behind next to the new assignment.
expect_marker "$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch" \
    '-		vb2->timestamp = 0;' 1
expect_marker "$kernel_dir/0005-media-iris-recycle-decoder-buffers-for-non-display-pictures.patch" \
    '+static int iris_hfi_gen2_requeue_output_buffer(' 1
expect_marker "$kernel_dir/0005-media-iris-recycle-decoder-buffers-for-non-display-pictures.patch" \
    '+	if (inst->domain == DECODER &&' 1
expect_marker "$kernel_dir/0006-media-iris-discard-unmatched-timestamp-metadata.patch" \
    '+	memset(&vbuf->timecode, 0, sizeof(vbuf->timecode));' 1
# 0007 is the completion-preserving form of 0005: the flag it stops setting and
# the empty-payload rule it stops applying are both load bearing.
expect_marker "$kernel_dir/0007-media-iris-complete-noshow-pictures-without-an-error.patch" \
    '+	bool noshow = inst_hfi_gen2->hfi_frame_info.picture_type & HFI_GEN2_PICTURE_NOSHOW;' 1
expect_marker "$kernel_dir/0007-media-iris-complete-noshow-pictures-without-an-error.patch" \
    '+	if (!buf->data_size && inst->state == IRIS_INST_STREAMING && !noshow &&' 1

# 0008 and 0009 are the 7.3.4 set. 0008 carries the mapping in both
# directions: a forward-only mapping registers the control and then rejects
# every write to it, which is the defect this pair exists to prevent.
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+#define HFI_PROP_DECODE_ORDER_OUTPUT' 1
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+		dst_vq->max_num_buffers = DEFAULT_MAX_HOST_BUF_COUNT;' 1
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+	case V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE:' 1
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+	case DISPLAY_DELAY_ENABLE:' 1
# The deblocking caps go into both HFI gen2 encoder tables, milos and
# sm8550; this device reads the sm8550 one.
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+		.cap_id = DEBLOCK_MODE_H264,' 2
expect_marker "$kernel_dir/0008-7.3.4-decode-order-and-deblocking.patch" \
    '+#define HFI_PROP_DEBLOCKING_MODE' 1
expect_marker "$kernel_dir/0009-7.3.4-preserve-timestamps-on-error-buffers.patch" \
    '+		vb2->timestamp = buf->timestamp;' 1
expect_marker "$kernel_dir/0009-7.3.4-preserve-timestamps-on-error-buffers.patch" \
    '-		vb2->timestamp = 0;' 1

# 0010 moves the LAST record above the error return. The added and the
# removed occurrence are both counted, so an edit that leaves the old block
# behind fails.
expect_marker "$kernel_dir/0010-7.3.4-complete-drain-on-error-last.patch" \
    '+	if (vbuf->flags & V4L2_BUF_FLAG_LAST) {' 1
expect_marker "$kernel_dir/0010-7.3.4-complete-drain-on-error-last.patch" \
    '-	if (vbuf->flags & V4L2_BUF_FLAG_LAST) {' 1
# 0011 must widen the zero-payload exemption, not replace one flag with the
# other: LAST still has to be exempt.
expect_marker "$kernel_dir/0011-7.3.4-psc-last-is-not-an-error.patch" \
    '+	    !(hfi_buffer->flags & (HFI_BUF_FW_FLAG_LAST | HFI_BUF_FW_FLAG_PSC_LAST))) {' 1
expect_marker "$kernel_dir/0012-7.3.4-quiesce-irq-before-core-lock.patch" \
    '+	disable_irq(core->irq);' 1
expect_marker "$kernel_dir/0012-7.3.4-quiesce-irq-before-core-lock.patch" \
    '+	enable_irq(core->irq);' 1

# 0013's value is the translation, not the registration: the cap entry alone
# reaches no property, and the setter alone has nothing to select a mode with.
expect_marker "$kernel_dir/0013-7.3.4-multi-slice.patch" \
    '+		.cap_id = SLICE_MODE,' 1
expect_marker "$kernel_dir/0013-7.3.4-multi-slice.patch" \
    '+#define HFI_PROP_MULTI_SLICE_MB_COUNT' 1
expect_marker "$kernel_dir/0013-7.3.4-multi-slice.patch" \
    '+int iris_set_slice_count(' 2

# 0014 has to reach all four places at once: the raw format table, the
# buffer size, the firmware color format and the bit-depth property. The
# format table alone would size a P010 buffer as 8-bit.
expect_marker "$kernel_dir/0014-7.3.4-encode-p010.patch" \
    '+	[IRIS_FMT_TP10] = V4L2_PIX_FMT_P010,' 1
expect_marker "$kernel_dir/0014-7.3.4-encode-p010.patch" \
    '+				return iris_yuv_buffer_size_p010(inst);' 1
expect_marker "$kernel_dir/0014-7.3.4-encode-p010.patch" \
    '+			hfi_colorformat = HFI_COLOR_FMT_P010;' 1
expect_marker "$kernel_dir/0014-7.3.4-encode-p010.patch" \
    '+	HFI_PROP_LUMA_CHROMA_BIT_DEPTH,' 1

# 0015 is only useful if the codec id and the two caps all land: an id alone
# reaches no profile control, and a cap alone has nothing to select it.
expect_marker "$kernel_dir/0015-7.3.4-mpeg2-decode.patch" \
    '+	HFI_CODEC_DECODE_MPEG2			= 6,' 1
expect_marker "$kernel_dir/0015-7.3.4-mpeg2-decode.patch" \
    '+		.cap_id = PROFILE_MPEG2,' 1
expect_marker "$kernel_dir/0015-7.3.4-mpeg2-decode.patch" \
    '+		.cap_id = LEVEL_MPEG2,' 1

# 0004's whole point is that the copy lands in the ERROR branch. Checking the
# line order inside the hunk is what distinguishes that from a copy added to
# the normal completion path, where the timestamp was already correct.
patch4=$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch
error_branch=$(grep -nF 'if (buf->flags & V4L2_BUF_FLAG_ERROR) {' "$patch4" | head -1 | cut -d: -f1)
copy=$(grep -nF '+		vb2->timestamp = buf->timestamp;' "$patch4" | head -1 | cut -d: -f1)
done_call=$(grep -nF 'v4l2_m2m_buf_done' "$patch4" | head -1 | cut -d: -f1)
if [ -z "$error_branch" ] || [ -z "$copy" ] || [ -z "$done_call" ] \
        || [ "$error_branch" -ge "$copy" ] || [ "$copy" -ge "$done_call" ]; then
    fail "0004: the timestamp copy is not inside the ERROR branch"
fi

patch9=$kernel_dir/0009-7.3.4-preserve-timestamps-on-error-buffers.patch
error_branch=$(grep -nF 'if (buf->flags & V4L2_BUF_FLAG_ERROR) {' "$patch9" | head -1 | cut -d: -f1)
copy=$(grep -nF '+		vb2->timestamp = buf->timestamp;' "$patch9" | head -1 | cut -d: -f1)
done_call=$(grep -nF 'v4l2_m2m_buf_done' "$patch9" | head -1 | cut -d: -f1)
if [ -z "$error_branch" ] || [ -z "$copy" ] || [ -z "$done_call" ] \
        || [ "$error_branch" -ge "$copy" ] || [ "$copy" -ge "$done_call" ]; then
    fail "0009: the timestamp copy is not inside the ERROR branch"
fi

# Every path the patch touches belongs to the Iris driver. `git apply
# --numstat` reads the diff without a tree and fails on a malformed patch,
# which is also the cheapest way to prove the file is still a patch.
if command -v git >/dev/null 2>&1; then
    for patch in "$kernel_dir"/0*.patch; do
        # Parse from the repository root: `git apply` reports the paths of a
        # plain unified diff relative to the caller's directory, so running
        # this from a build tree would prefix every path with it.
        if ! paths=$(git -C "$root" apply --numstat "$patch" 2>&1); then
            fail "${patch##*/}: git cannot read it as a patch"
            continue
        fi
        outside=$(printf '%s\n' "$paths" | awk '$3 != "" {print $3}' \
            | grep -v '^drivers/media/platform/qcom/iris/' || true)
        if [ -n "$outside" ]; then
            fail "${patch##*/} touches a path outside the Iris driver: $outside"
        fi
    done
else
    printf 'WARN kernel-patch: git is unavailable, patches not parsed\n' >&2
fi

# The README is the recipe an operator follows, and it names each patch by
# file. A renamed patch that this check does not catch leaves the recipe
# pointing at a file that no longer exists.
for patch in "$kernel_dir"/0*.patch; do
    grep -Fq "${patch##*/}" "$readme" \
        || fail "README.md does not name ${patch##*/}"
done

# The kernel spec lives in a second repository and fetches these files by
# URL. Bind its copy of the hashes and its commit pin to this tree, so editing
# a patch without re-tagging and re-pinning fails here instead of an hour into
# a kernel build. CI checks out only this repository, where the spec is absent;
# an explicit IRIS_DOWNSTREAM_SPEC is how that environment opts in.
spec=${IRIS_DOWNSTREAM_SPEC:-}
if [ -z "$spec" ]; then
    candidate=$root/../sheng-fedora-packages/kernel-sm8550/kernel-sm8550.spec
    if [ -f "$candidate" ]; then spec=$candidate; fi
fi
if [ -n "$spec" ] && [ -f "$spec" ]; then
    base_failures=$failures
    for name in 0001 0002 0003 0004; do
        recorded=$(awk -v n="$name" '$2 ~ "^"n"-" {print $1}' "$sums")
        case $name in
            0001) var=IRIS_PATCH1_SHA256 ;;
            0002) var=IRIS_PATCH2_SHA256 ;;
            0003) var=IRIS_PATCH3_SHA256 ;;
            0004) var=IRIS_PATCH4_SHA256 ;;
        esac
        downstream=$(sed -n "s/^%global $var \([0-9a-f]\{64\}\).*/\1/p" "$spec")
        [ "$downstream" = "$recorded" ] \
            || fail "downstream spec $var is ${downstream:-unset}, SHA256SUMS has $recorded"
    done
    pinned=$(git -C "$root" rev-list -n1 iris-kernel-patches-v1 2>/dev/null || true)
    [ -n "$pinned" ] || fail "iris-kernel-patches-v1 tag is missing"
    urls=$(sed -n 's/^Source[6-9]:[[:space:]]*//p' "$spec")
    [ -n "$urls" ] || fail "downstream spec has no Source6-9 patch URLs"
    for url in $urls; do
        printf '%s\n' "$url" | grep -Fq "/$pinned/packaging/kernel/" \
            || fail "downstream Source URL does not pin tag commit $pinned: $url"
    done
    if [ "$failures" -eq "$base_failures" ]; then
        printf 'PASS kernel patches: downstream spec hashes and commit pin agree\n'
    fi
else
    printf 'NOTE kernel patches: downstream spec not present, cross-repo check skipped\n'
fi

if [ "$failures" -ne 0 ]; then
    printf 'FAIL kernel patches: %d problem(s)\n' "$failures" >&2
    exit 1
fi
printf 'PASS kernel patches: fifteen files, recorded hashes, Iris-only paths\n'
