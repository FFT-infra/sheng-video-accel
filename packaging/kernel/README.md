# Iris Kernel Module Patches

Two patch sets live here. 0001-0007 target `sheng-7.2.6`
(`42f3b40c702a`): four qualified patches, plus three experimental ones.
0008-0013 target `sheng-7.3.4` (`87ac0f2347eb`), whose in-tree driver already
carries the request-sync-frame control and the stability fixes that 7.2.6
needs patches for, so the 7.3.4 set is smaller. 0008-0009 are qualified;
0010-0012 are drain and IRQ backports with one target run each, recorded in
`TEST-RESULTS.md`. 0013 adds the encoder multi-slice controls.

Only 0001-0004 make up the 7.2.6 module; on 7.3.4 the qualified set is
0008-0009. `TEST-RESULTS.md` records the hardware evidence for the qualified
combination, including the AV1 zero-copy Import run. None of the experimental
patches (0005-0007, 0010-0012) is part of a qualified baseline: 0006 had one
guarded run on `sheng` with a "no regression" verdict and no capability
change, while 0005 and 0007 were rejected on hardware. 0010-0012 come from
Project SunUEFI's media patch for the same HFI gen2 driver; each was built
into the 7.3.4 module and run on `sheng` once, and none is part of the
qualified baseline. The driver detects missing kernel controls
at runtime and degrades gracefully on stock kernels.

## Patch Summary

| Patch | Origin | Description | Purpose |
| --- | --- | --- | --- |
| `0001-media-iris-decode-order-output-and-64-capture-buffers.patch` | upstream Iris patches | Implements `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY` and expands CAPTURE queue to 64 buffers | Enables zero-copy Import decode path and dynamic resolution changes |
| `0002-media-iris-deblocking-filter-controls.patch` | upstream Iris patches | Exposes H.264 and HEVC loop-filter controls (`HFI_PROP_DEBLOCKING_MODE`) | Allows forwarding of client slice deblocking requests |
| `0003-media-iris-encoder-request-sync-frame.patch` | Qualcomm (Wangao Wang) | Implements `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` (`HFI_PROP_REQUEST_SYNC_FRAME`) | Enables direct hardware sync-frame requests |
| `0004-media-iris-preserve-timestamps-on-error-buffers.patch` | upstream Iris patches | Preserves buffer timestamp on `V4L2_BUF_FLAG_ERROR` completions | Allows immediate failure attribution for corrupted frames |
| `0005-media-iris-recycle-decoder-buffers-for-non-display-pictures.patch` | upstream Iris patches | Recycles non-display (NOSHOW) decoder buffers internally | **Experimental, rejected.** Requeues NOSHOW CAPTURE buffers in the kernel instead of completing them to userspace; this driver's AV1 path consumes those completions by decode order to publish hidden reference frames. Built into the module once: all three qualified AV1 clips failed under import, copy and direct, and native `v4l2av1dec` reached EOS on the same node. Never build it into a released module. |
| `0006-media-iris-discard-unmatched-timestamp-metadata.patch` | downstream timestamp-metadata fix | Discards unmatched timestamp metadata instead of borrowing | **Experimental, one guarded no-regression run only.** Built, installed with 0001-0004, rebooted and run through the four-codec matrix, long VP9 clip, AV1 real/DRC clips, EOS and slow-CAPTURE probes on `sheng`. The upstream defect was not reproduced on the 0001-0004 baseline, so the verdict is "no regression", not "fixed". It is not part of the qualified baseline and adds no codec, profile or publish-policy claim. |
| `0007-media-iris-complete-noshow-pictures-without-an-error.patch` | this repository | Completes non-display (NOSHOW) pictures without `V4L2_BUF_FLAG_ERROR` and without the picture-type flags, keeping the timestamp 0004 preserves | **Experimental, rejected.** The completion-preserving form of 0005: nothing is recycled in the kernel, the false error is simply not attached. Built with 0001-0004, installed and booted (module `f070ae18…`); the installed driver then failed all three AV1 clips under import, copy and direct, while the same driver with the unflagged-NOSHOW tolerance in the session decoded byte-identical output and matched the qualified baseline's counters. It therefore buys nothing over the qualified module. Never build it into a released module. |
| `0008-7.3.4-decode-order-and-deblocking.patch` | this repository | Exposes decode-order output (`HFI_PROP_DECODE_ORDER_OUTPUT`) and the six H.264/HEVC loop-filter controls (`HFI_PROP_DEBLOCKING_MODE`) on the 7.3.4 driver, and raises the decoder CAPTURE ceiling to `DEFAULT_MAX_HOST_BUF_COUNT` | **Required on 7.3.4.** Without it the driver opens in display-order mode and the Import zero-copy path is unavailable. Carries the control-id mapping in both directions: `iris_get_v4l2_id()` alone registers a control that `VIDIOC_S_CTRL` then rejects with EINVAL. |
| `0009-7.3.4-preserve-timestamps-on-error-buffers.patch` | this repository | Preserves the buffer timestamp on `V4L2_BUF_FLAG_ERROR` completions | **Required on 7.3.4.** Same change as 0004, rebased onto the 7.3.4 source. |
| `0010-7.3.4-complete-drain-on-error-last.patch` | Project SunUEFI media patch | Records `V4L2_BUF_FLAG_LAST` before the error return in `iris_vb2_buffer_done()` | **Experimental, one target run 11-18.** Built with 0008-0011 and run on the DRC clip and at stream end: the drain stopped, the visible output matched the software decode byte for byte (`84/84`, `drops=0`), and the encoder drained cleanly. The run does not isolate this patch from 0011, so the result is "no regression", not a demonstrated fix. Not part of the qualified baseline. |
| `0011-7.3.4-psc-last-is-not-an-error.patch` | Project SunUEFI media patch | Exempts `HFI_BUF_FW_FLAG_PSC_LAST` from the zero-payload error rule | **Experimental, one target run 11-18.** On the DRC clip the sequence-change completion carried `V4L2_BUF_FLAG_ERROR` on the qualified module (`flags=0x104040`) and none with this patch (`0x104000`). Visible output stayed byte-identical to the software decode, `submitted=84 completed=84 drops=0`, and the native `v4l2av1dec` client decoded the same clip without an error. The boundary hidden completion is no longer published as a NOSHOW reference, so this patch changes this driver's own invisible accounting; that is why it stays experimental. |
| `0012-7.3.4-quiesce-irq-before-core-lock.patch` | Project SunUEFI media patch | Disables the Iris IRQ around the `core->lock` section in `iris_core_deinit()` | **Experimental, one target run 11-18.** On the qualified module `modprobe -r qcom_iris` hung in `D` state with the module in `Unloading`, its stack at `disable_irq -> __synchronize_irq` from `iris_core_deinit`. With this patch the same unload and a following reload each completed in under a second and both nodes re-probed. Not part of the qualified baseline. |
| `0013-7.3.4-multi-slice.patch` | this repository | Exposes `V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE`, `..._MAX_BYTES` and `..._MAX_MB` on the 7.3.4 encoder and maps the selected mode to `HFI_PROP_MULTI_SLICE_MB_COUNT` / `..._BYTES_COUNT` | **New, target run pending.** Adds the three controls and a setter that rejects anything the firmware does not take: only CBR, up to 60 fps, within the slice geometry bounds. Without it every encoded frame is one slice and `VAConfigAttribEncMaxSlices` can only answer 1. |
## Integrity

`SHA256SUMS` records the authoritative hash for each patch file. Integrity is checked automatically via:
```sh
sh test/iris-kernel-patch-check.sh
```

The downstream kernel spec pins these patches by commit hash against the tag `iris-kernel-patches-v1`.

## Module Rebuild Guide

`build-module.sh` builds only `drivers/media/platform/qcom/iris` against the
running kernel. It does not install a module and does not reboot. The source
tree must match the running kernel, on a case-sensitive filesystem: a
case-insensitive checkout collides Linux's `xt_TCPMSS.c` / `xt_tcpmss.c` and
breaks the build. The target here is f2fs.

```sh
# 7.2.6
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
sh packaging/kernel/build-module.sh --source linux
# Experimental only: add 0006 to the four qualified patches, never 0005.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0001 --patch 0002 --patch 0003 --patch 0004 --patch 0006

# 7.3.4
git clone --depth 1 -b sheng-7.3.4 https://github.com/ianchb/sm8550-mainline linux
sh packaging/kernel/build-module.sh --source linux --patch 0008 --patch 0009
# Experimental only: add the drain and IRQ backports.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0008 --patch 0009 --patch 0010 --patch 0011 --patch 0012
```

On 7.3.4, `CONFIG_LOCALVERSION_AUTO` appends `-g<sha>` to the release, so a
tree that has the patches applied as uncommitted changes produces a vermagic
the running kernel does not accept. Commit the applied patches in the scratch
clone, or drop `LOCALVERSION_AUTO` from `.config`, before building.

The script takes `.config` from `/proc/config.gz` rather than a
`/boot/config-*` file: on `sheng`, those two files have different hashes and
there is no config named after the `-iris1` release. It derives `localversion`
from `uname -r`, regenerates the video-subsystem `Module.symvers` from
`/proc/kallsyms` (or takes one with `--symvers`), applies the
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
the rollback copy from `build-module.sh`.

```sh
work=${IRIS_KERNEL_BUILD_DIR:-/tmp/iris-kernel-module}
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

The twelve patch files are kernel fragments targeting GPL-2.0-only sources under
`drivers/media/platform/qcom/iris/`. They are not compiled into the MIT driver
package, so they have no SPDX header of their own; the patched files keep the
kernel's own identifier. `SHA256SUMS` pins each patch for the downstream
kernel spec.
