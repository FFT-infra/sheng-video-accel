# Iris Kernel Module Patches

This directory provides four qualified kernel patches (0001-0004) plus two
experimental, unvalidated ones (0005-0006), targeting the `qcom-iris` driver on
`ianchb/sm8550-mainline` (branch `sheng-7.2.6`, commit `42f3b40c702a`).

Only 0001-0004 make up the module the driver's decode results in
TEST-RESULTS.md are qualified on, including the 2026-11-10 AV1 zero-copy
Import run. The driver detects missing kernel controls at runtime and degrades
gracefully on stock kernels.

## Patch Summary

| Patch | Upstream Reference | Description | Purpose |
| --- | --- | --- | --- |
| `0001-media-iris-decode-order-output-and-64-capture-buffers.patch` | Radxa / `qcom-va-driver` | Implements `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY` and expands CAPTURE queue to 64 buffers | Enables zero-copy Import decode path and dynamic resolution changes |
| `0002-media-iris-deblocking-filter-controls.patch` | Radxa / `qcom-va-driver` | Exposes H.264 and HEVC loop-filter controls (`HFI_PROP_DEBLOCKING_MODE`) | Allows forwarding of client slice deblocking requests |
| `0003-media-iris-encoder-request-sync-frame.patch` | Qualcomm (Wangao Wang) | Implements `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` (`HFI_PROP_REQUEST_SYNC_FRAME`) | Enables direct hardware sync-frame requests |
| `0004-media-iris-preserve-timestamps-on-error-buffers.patch` | Radxa / `qcom-va-driver` | Preserves buffer timestamp on `V4L2_BUF_FLAG_ERROR` completions | Allows immediate failure attribution for corrupted frames |
| `0005-media-iris-recycle-decoder-buffers-for-non-display-pictures.patch` | Radxa / `qcom-va-driver` | Recycles non-display (NOSHOW) decoder buffers internally | **Experimental, not validated with this driver.** Requeues NOSHOW CAPTURE buffers in the kernel instead of completing them to userspace; this driver's AV1 path consumes those completions by decode order to publish hidden reference frames, so loading 0005 with the current userspace loses the frames. Qualifying it needs its own translator/session rewrite plus hardware runs. |
| `0006-media-iris-discard-unmatched-timestamp-metadata.patch` | yzddmr6 / `linux-sm8750-piano` | Discards unmatched timestamp metadata instead of borrowing | **Experimental, not validated.** Has not been built or loaded on the target. |

## Verification & Integrity

`SHA256SUMS` records the authoritative hash for each patch file. Integrity is verified automatically via:
```sh
sh test/iris-kernel-patch-check.sh
```

Downstream packaging in `GhfunNiHe/sheng-fedora-packages` pins these patches by commit hash against the immutable tag `iris-kernel-patches-v1`.

## Module Rebuild Guide

To build and install the modified module on the target device:

```sh
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
cd linux
# Apply only the four qualified patches. 0005/0006 are experimental and
# change the NOSHOW/timestamp completion contract this userspace expects.
git am ../0001-*.patch ../0002-*.patch ../0003-*.patch ../0004-*.patch

cp /boot/config-$(uname -r) .config
echo "-sm8550-g42f3b40-iris1" > localversion
export LOCALVERSION=""
make ARCH=arm64 LLVM=1 olddefconfig modules_prepare

# Generate Module.symvers from active kernel exports
awk '$4 ~ /^\[/ && $2 ~ /[TDR]/ {gsub(/[\[\]]/,"",$4); print "0x00000000\t"$3"\t"$4"\tEXPORT_SYMBOL\t"}' /proc/kallsyms \
  | grep -E 'videobuf2|videodev|v4l2_mem2mem' > Module.symvers

make ARCH=arm64 LLVM=1 LOCALVERSION= KBUILD_MODPOST_WARN=1 M=drivers/media/platform/qcom/iris modules
```

### Installation

Compress and replace the installed kernel module:

```sh
zstd -19 -f drivers/media/platform/qcom/iris/qcom-iris.ko -o qcom-iris.ko.zst
sudo cp qcom-iris.ko.zst /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/
sudo depmod -a
sudo systemctl reboot
```

After reboot, verify active controls on the decoded and encoded nodes:
```sh
iris-device-check
```
The `[5] optional kernel controls` section resolves both nodes by the
`iris_driver` name and reports `display_delay`, `min_number_of_capture_buffers`,
`force_key_frame` and the loop-filter controls.
