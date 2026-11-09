# Iris kernel module patches

Four patches. The first two are from
[radxa-pkg/qcom-va-driver](https://github.com/radxa-pkg/qcom-va-driver)
(Radxa, Xilin Wu; the repository was `strongtz/libva-v4l2` before its
2026-10-26 rename), the third is the encoder half of an upstream commit
(Wangao Wang), and the fourth is also Radxa's (Xilin Wu), all
rebased onto `ianchb/sm8550-mainline` branch `sheng-7.2.6` (commit
`42f3b40c702a`). Upstream now carries five patches (its 0005 recycles
non-display decoder buffers for AV1); this tree's mapping is 0001+0002 /
0003 / upstream `6f62dcefd249` / 0004, unchanged:

| Patch | Upstream | What it changes | Why the driver wants it |
| --- | --- | --- | --- |
| `0001-media-iris-decode-order-output-and-64-capture-buffers.patch` | `patches/0001` + `patches/0002` | Decode-order output through `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY{,_ENABLE}`; CAPTURE pool up to 64 | Chrome's zero-copy (Import) path, and the VP9 dynamic-resolution qualification |
| `0002-media-iris-deblocking-filter-controls.patch` | `patches/0003` | H.264 loop filter mode/alpha/beta and the HEVC equivalents, sent to firmware as `HFI_PROP_DEBLOCKING_MODE` | The driver forwards a client's slice deblocking request instead of refusing a non-default one |
| `0003-media-iris-encoder-request-sync-frame.patch` | `6f62dcefd249` | `REQUEST_SYNC_FRAME` in `inst_fw_cap_sm8550_enc`, so `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` exists and reaches firmware as `HFI_PROP_REQUEST_SYNC_FRAME` | A requested IDR becomes a firmware sync frame instead of a GOP move, and the driver prefixes no parameter sets of its own |
| `0004-media-iris-preserve-timestamps-on-error-buffers.patch` | `patches/0004` | An ERROR CAPTURE completion keeps the firmware's timestamp instead of being zeroed | The driver can name the failed picture and fail that surface at once, instead of leaving it pending until the sync bound |

All four modify GPL-2.0 kernel code (`drivers/media/platform/qcom/iris/`)
and are therefore GPL-2.0, unlike the driver itself. Upstream authors are
credited in each patch's trailer and in the driver's README. The driver works
without any of them: no display-delay control means the display-order fallback
and a GPU copy per Chrome frame; no loop-filter controls means the firmware's
enabled, 0/0 defaults and `VA_STATUS_ERROR_UNIMPLEMENTED` for any other
deblocking request; a zeroed error timestamp means the failed surface is
reported only after a drain or a sync timeout; no sync-frame control means a
requested IDR moves the intra period and the driver puts the last SPS/PPS in
front of it.

`SHA256SUMS` records the hash of each patch. Both the module recipe below and
the downstream package that fetches these files by URL verify against it, so a
patch edited in place fails a check instead of reaching a build.
`test/iris-kernel-patch-check.sh` compares the files to it, asserts every path
a patch touches is under `drivers/media/platform/qcom/iris/`, and pins the
one-line marker of each change — the counts it checks are the same greps the
"check these by hand" list below gives an operator. The script resolves the
downstream spec in exactly one of two ways: the explicit
`IRIS_DOWNSTREAM_SPEC` override, or the sibling
`../sheng-fedora-packages/kernel-sm8550/kernel-sm8550.spec` checkout. A stale
clone under `~/Lab/Bridge/tmp/` is deliberately not a candidate: it can pin an
older commit and produce four false URL-pin failures. With the spec present,
the same script also asserts that its four `IRIS_PATCHn_SHA256` values equal
`SHA256SUMS`, that at least one `Source6-9` URL exists, and that every such URL
pins the commit `iris-kernel-patches-v1` resolves to. Editing a patch without
re-tagging and re-pinning then fails on the host instead of an hour into a
kernel build. CI checks out only this repository, so it skips that half and
still checks everything above.

`rpmbuild -bp --nodebuginfo --nodeps kernel-sm8550/kernel-sm8550.spec` was run
against the downstream spec on 2026-11-10: `spectool -g -R` fetched all nine
sources, `%prep` verified all four hashes and applied all four patches at
`--fuzz=0` (0004 at its recorded −3-line offset), and the applied tree carries
the decode-order define, the deblocking and sync-frame capabilities in
`enum platform_inst_fw_cap_type`, and the ERROR-branch timestamp copy. A full
`-ba` was not run: it needs ten BuildRequires absent on this host and a
30–90 minute aarch64 kernel build. `%prep` is the stage that verifies this
directory's payload, so the unbuilt half is the RPM's own compile, not the
patch contract.

These patches are consumed by
[`GhfunNiHe/sheng-fedora-packages`](https://github.com/GhfunNiHe/sheng-fedora-packages)
(the branch carrying them is in the `FFT-infra` fork), whose
`kernel-sm8550/kernel-sm8550.spec` fetches each one by commit-pinned URL from
this repository and checks the recorded hash before `%patch`. A change to a
patch here therefore breaks that build until the pin and the hash move with
it.

The URLs pin commit `5a0fcb5`, the first commit that carries all four files
and this directory's `SHA256SUMS`. The `iris-kernel-patches-v1` tag points at
it, and a tag does not move, so the URLs survive the deletion of any branch
that happens to contain the commit. Pinning a commit that only a branch or a
pull request head keeps alive is the failure to avoid: GitHub collects an
unreachable commit, and the next kernel build fails at `spectool` with a 404
that says nothing about the patches being fine.

When a patch changes, tag a new commit and repoint the spec in the same
change; leaving the old tag in place keeps previously built packages
fetchable.

Rebuild only the module, on the target, against the running kernel:

```sh
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
cd linux && git am ../0001-*.patch ../0002-*.patch ../0003-*.patch ../0004-*.patch
```

`git am` applies all four cleanly on `sheng-7.2.6`. On a differently shaped
Iris tree expect fuzz, and check these by hand before building:

- The capability IDs belong in `enum platform_inst_fw_cap_type` in
  `iris_platform_common.h`, not in the neighbouring
  `enum platform_inst_fw_cap_flags`. A fuzzy 0001 apply has put
  `DISPLAY_DELAY_ENABLE`/`DISPLAY_DELAY` into the flags enum (seen on
  `sheng-7.2.2`).
- The deblocking patch (0002 here, `patches/0003` upstream) is written
  against a tree with two gen2 encoder tables (`milos_enc` and
  `sm8550_enc`). `sheng-7.2.6` has only `inst_fw_cap_sm8550_enc`, so both of
  the patch's table hunks land in the same array under fuzz and the six
  DEBLOCK entries appear twice. This rebased patch adds them once, after
  `ENTROPY_MODE` in `inst_fw_cap_sm8550_enc`. Verify with
  `grep -c 'cap_id = DEBLOCK_MODE_H264' drivers/media/platform/qcom/iris/iris_hfi_gen2.c`
  — it must print 1.
- The same upstream patch's second hunk also adds the same control to the
  gen1 encoder table; `sheng-7.2.6` has no gen1 encoder on this platform, so
  this rebase carries only the gen2 entry.
- The sync-frame patch (0003 here, upstream `6f62dcefd249`) needs two
  checks after applying.
  `grep -c 'cap_id = REQUEST_SYNC_FRAME' drivers/media/platform/qcom/iris/iris_hfi_gen2.c`
  must print 1, and
  `grep -c 'V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME' drivers/media/platform/qcom/iris/iris_ctrls.c`
  must print 2 (one case in each direction of the control/capability map).
  The same grep on `iris_venc.c` and `iris_vidc.c` must print 0: the control
  is created from the capability table, not named again at the node.
- The error-timestamp patch (0004 here, `patches/0004` upstream) must leave
  exactly one `vb2->timestamp = buf->timestamp;` inside the
  `V4L2_BUF_FLAG_ERROR` branch of `iris_vb2_buffer_done()`:
  `grep -c 'vb2->timestamp = buf->timestamp' drivers/media/platform/qcom/iris/iris_buffer.c`
  must print 2 (the error branch and the normal completion).

```sh
cp /boot/config-$(uname -r) .config
# The release string must match the running kernel exactly or the module
# will not load. The config's CONFIG_LOCALVERSION is empty and
# LOCALVERSION_AUTO is off, so the "-sm8550-g42f3b40-iris1" part of the
# running release is in neither the config nor git: put it in a
# `localversion` file in the source root instead.
cat /boot/config-$(uname -r) | grep -E '^CONFIG_LOCALVERSION'
uname -r                                    # e.g. 7.2.6-sm8550-g42f3b40-iris1
echo '-sm8550-g42f3b40-iris1' > localversion
export LOCALVERSION=""                      # keep the "+" off the release string
make ARCH=arm64 LLVM=1 LOCALVERSION= olddefconfig modules_prepare
cat include/config/kernel.release           # must equal uname -r
# Module.symvers is not shipped; derive the dependency symbols from the
# running kernel so modpost records "depends:" correctly.
awk '$4 ~ /^\[/ && $2 ~ /[TDR]/ {gsub(/[\[\]]/,"",$4); print "0x00000000\t"$3"\t"$4"\tEXPORT_SYMBOL\t"}' /proc/kallsyms \
  | grep -E 'videobuf2|videodev|v4l2_mem2mem' > Module.symvers
make ARCH=arm64 LLVM=1 LOCALVERSION= KBUILD_MODPOST_WARN=1 \
     M=drivers/media/platform/qcom/iris modules
llvm-readelf -n drivers/media/platform/qcom/iris/qcom-iris.ko | grep -E 'PAC|BTI'
modinfo drivers/media/platform/qcom/iris/qcom-iris.ko | grep -E 'vermagic|^depends'
```

`KCFLAGS=-mbranch-protection=pac-ret+bti` is required only when the target's
config sets `CONFIG_ARM64_BTI_KERNEL`, which Kconfig drops under clang ≥ 21
and whose absence oopses on load (`Oops - BTI`). The packaged
`kernel-sheng-7.2.6-1` does not: it sets `CONFIG_ARM64_BTI` without the
kernel option, and its module's note is `aarch64 feature: PAC`. Match the
stock module: build with the plain command above, then compare its
`llvm-readelf -n` note with the stock `.ko`'s before installing anything.
Adding the KCFLAGS there would give the module BTI landing pads the running
kernel's own code does not have.

Install by replacing the file and rebooting. Do not `modprobe -r qcom_iris`
on a booted system: unloading hangs in `iris_vpu_power_off` while the VPU
is powered.

```sh
zstd -19 -f qcom-iris.ko -o qcom-iris.ko.zst
sudo cp qcom-iris.ko.zst /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/
sudo depmod -a && sudo systemctl reboot
v4l2-ctl -d /dev/videoN --list-ctrls | grep display_delay   # present = decode-order
v4l2-ctl -d /dev/videoN --list-ctrls | grep loop_filter     # present = deblocking forwarded
v4l2-ctl -d /dev/videoN --list-ctrls | grep force_key_frame # present = direct IDR request
```

Keep the stock `qcom-iris.ko.zst` beside it for rollback.
