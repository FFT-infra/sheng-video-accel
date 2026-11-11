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
| `0006-media-iris-discard-unmatched-timestamp-metadata.patch` | yzddmr6 / `linux-sm8750-piano` | Discards unmatched timestamp metadata instead of borrowing | **Experimental, not validated on hardware.** A module containing it was built on `sheng` (WP1, 2026-11-12) but has not been installed, loaded or run through a decode gate. |

## Verification & Integrity

`SHA256SUMS` records the authoritative hash for each patch file. Integrity is verified automatically via:
```sh
sh test/iris-kernel-patch-check.sh
```

Downstream packaging in `GhfunNiHe/sheng-fedora-packages` pins these patches by commit hash against the immutable tag `iris-kernel-patches-v1`.

## Module Rebuild Guide

`build-module.sh` builds only `drivers/media/platform/qcom/iris` against the
running kernel. It does not install a module and does not reboot. The source
tree must be `ianchb/sm8550-mainline` at `42f3b40c702a` (`sheng-7.2.6`), on a
case-sensitive filesystem: a case-insensitive checkout collides Linux's
`xt_TCPMSS.c` / `xt_tcpmss.c` and breaks the build. The target here is f2fs.

```sh
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
sh packaging/kernel/build-module.sh --source linux
# WP1 only: add 0006 to the four qualified patches, never 0005.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0001 --patch 0002 --patch 0003 --patch 0004 --patch 0006
```

The script takes `.config` from `/proc/config.gz` rather than a
`/boot/config-*` file: on `sheng`, those two files have different hashes and
there is no config named after the `-iris1` release. It derives `localversion`
from `uname -r`, regenerates the video-subsystem `Module.symvers` from
`/proc/kallsyms` (or takes a verified one with `--symvers`), applies the
numbered patches in order with `git apply`, and builds with `LLVM=1`.

`KBUILD_MODPOST_WARN=1` is expected: a regenerated symbol table carries only
the video subsystem exports, so modpost warns about core-kernel symbols.
The script checks every undefined symbol against the running kernel's
`__ksymtab` instead, verifies `modinfo -F vermagic` against `uname -r`, writes
a compressed module to the work directory, and compares a rollback copy with
the installed `qcom-iris.ko.zst`. `run-static-checks.sh` parses this script.

### Installation

Installation is a separate, explicit decision. Before replacing anything,
record the source commit, the installed driver hash, the kernel module hash,
boot ID, resolved decoder/encoder nodes, battery state and thermal zones. Keep
the verified rollback copy from `build-module.sh`.

```sh
work=~/Lab/Bridge/tmp/iris-kernel-module
sudo cp "$work/qcom-iris.ko.zst" \
    /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/
sudo depmod -a
sudo systemctl reboot
```

The reboot changes the boot ID by design. Record it before and after, and
start `scripts/iris-experiment-guard.sh` only after the reboot so its
"boot ID unchanged" rule measures the experiment, not the installation. On a
failure, restore `$work/rollback-qcom-iris.ko.zst`, run `depmod -a`, reboot,
and keep the record.

After reboot, verify active controls on the decoded and encoded nodes:
```sh
iris-device-check
```
The `[5] optional kernel controls` section resolves both nodes by the
`iris_driver` name and reports `display_delay`, `min_number_of_capture_buffers`,
`force_key_frame` and the loop-filter controls.

The six patch files are kernel fragments targeting GPL-2.0-only sources under
`drivers/media/platform/qcom/iris/`. They are not compiled into the MIT driver
package, so they have no SPDX header of their own; the patched files keep the
kernel's own identifier. `SHA256SUMS` pins each patch for the downstream
kernel spec.
