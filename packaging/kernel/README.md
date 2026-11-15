# Iris Kernel Module Patches

This directory provides four qualified kernel patches (0001-0004) plus three
experimental ones (0005-0007), targeting the `qcom-iris` driver on
`ianchb/sm8550-mainline` (branch `sheng-7.2.6`, commit `42f3b40c702a`).

Only 0001-0004 make up the module the driver's decode results in
TEST-RESULTS.md are qualified on, including the 2026-11-10 AV1 zero-copy
Import run. The lab module `b90bd823` also carries 0006; its guarded WP1 run
found no frame effect, so no capability claim rests on that extra patch. The
driver detects missing kernel controls at runtime and degrades gracefully on
stock kernels. None of the experimental patches is part of
that qualified baseline; 0006 has one guarded WP1 run on `sheng` recorded in
TEST-RESULTS.md, with a "no regression" verdict and no capability change, and
0007 was built, installed and run on 2026-11-12 with a "no reason to ship"
verdict (TEST-RESULTS.md, "The completion-preserving 0005 form").

## Patch Summary

| Patch | Origin | Description | Purpose |
| --- | --- | --- | --- |
| `0001-media-iris-decode-order-output-and-64-capture-buffers.patch` | upstream Iris patches | Implements `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY` and expands CAPTURE queue to 64 buffers | Enables zero-copy Import decode path and dynamic resolution changes |
| `0002-media-iris-deblocking-filter-controls.patch` | upstream Iris patches | Exposes H.264 and HEVC loop-filter controls (`HFI_PROP_DEBLOCKING_MODE`) | Allows forwarding of client slice deblocking requests |
| `0003-media-iris-encoder-request-sync-frame.patch` | Qualcomm (Wangao Wang) | Implements `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` (`HFI_PROP_REQUEST_SYNC_FRAME`) | Enables direct hardware sync-frame requests |
| `0004-media-iris-preserve-timestamps-on-error-buffers.patch` | upstream Iris patches | Preserves buffer timestamp on `V4L2_BUF_FLAG_ERROR` completions | Allows immediate failure attribution for corrupted frames |
| `0005-media-iris-recycle-decoder-buffers-for-non-display-pictures.patch` | upstream Iris patches | Recycles non-display (NOSHOW) decoder buffers internally | **Experimental, measured on `sheng` and rejected.** Requeues NOSHOW CAPTURE buffers in the kernel instead of completing them to userspace; this driver's AV1 path consumes those completions by decode order to publish hidden reference frames. Built into the module once (2026-11-12): all three qualified AV1 clips failed under import, copy and direct, and native `v4l2av1dec` reached EOS on the same node (`TEST-RESULTS.md` §"0005 on the target"). Never build it into a released module. |
| `0006-media-iris-discard-unmatched-timestamp-metadata.patch` | downstream timestamp-metadata fix | Discards unmatched timestamp metadata instead of borrowing | **Experimental, one guarded WP1 run only.** Built, installed with 0001-0004, rebooted and run through the four-codec matrix, long VP9 clip, AV1 real/DRC clips, EOS and slow-CAPTURE probes on `sheng` (2026-11-12). The upstream defect was not reproduced on the 0001-0004 baseline, so the verdict is "no regression", not "fixed". It is not part of the qualified baseline and adds no codec, profile or publish-policy claim. |
| `0007-media-iris-complete-noshow-pictures-without-an-error.patch` | this repository | Completes non-display (NOSHOW) pictures without `V4L2_BUF_FLAG_ERROR` and without the picture-type flags, keeping the timestamp 0004 preserves | **Experimental, measured on `sheng` and rejected.** The completion-preserving form of 0005: nothing is recycled in the kernel, the false error is simply not attached. Built with 0001-0004, installed and booted (module `f070ae18…`); the installed driver then failed all three AV1 clips under import, copy and direct, while the same driver with the unflagged-NOSHOW tolerance in the session decoded byte-identical output and matched the qualified baseline's counters. It therefore buys nothing over the qualified module. Never build it into a released module. |

## Verification & Integrity

`SHA256SUMS` records the authoritative hash for each patch file. Integrity is verified automatically via:
```sh
sh test/iris-kernel-patch-check.sh
```

The downstream kernel spec pins these patches by commit hash against the tag `iris-kernel-patches-v1`.

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

Installation is a separate, explicit decision. WP1's installation, reboot and
guarded gates are recorded in `TEST-RESULTS.md` §"WP1: 0006 loaded on sheng";
the three interrupted guard attempts are retained as audit records. Before
replacing anything, record the source commit, the installed driver hash, the
kernel module hash, boot ID, resolved decoder/encoder nodes, battery state and
thermal zones. Keep the verified rollback copy from `build-module.sh`.

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

The seven patch files are kernel fragments targeting GPL-2.0-only sources under
`drivers/media/platform/qcom/iris/`. They are not compiled into the MIT driver
package, so they have no SPDX header of their own; the patched files keep the
kernel's own identifier. `SHA256SUMS` pins each patch for the downstream
kernel spec.
