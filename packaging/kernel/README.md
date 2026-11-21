# Iris Kernel Module Patches

Two patch sets live here. 0001-0007 target `sheng-7.2.6`
(`42f3b40c702a`): four qualified patches, plus three experimental ones.
0008-0017 target `sheng-7.3.4` (`87ac0f2347eb`), whose in-tree driver already
carries the request-sync-frame control and the stability fixes that 7.2.6
needs patches for, so the 7.3.4 set is smaller. 0008-0009 are qualified;
0010-0012 are drain and IRQ backports with one target run each, recorded in
`TEST-RESULTS.md`. 0013 adds the encoder multi-slice controls, 0014 adds
P010 input for HEVC Main 10 encode, 0015 is an MPEG-2 decode experiment,
0016 allocates the decoder CAPTURE pool from system memory behind a module
parameter, the 8K Direct experiment, and 0017 is the encoder ROI metadata
experiment.

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
| `0013-7.3.4-multi-slice.patch` | this repository | Exposes `V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE`, `..._MAX_BYTES` and `..._MAX_MB` on the 7.3.4 encoder and maps the selected mode to `HFI_PROP_MULTI_SLICE_MB_COUNT` / `..._BYTES_COUNT` | **Experimental, one target run 11-18.** The controls appear on the encoder node, and `test/iris-encode-slice-probe` through the build-tree driver coded 4 slices per picture for H.264 and HEVC at 640x480, with `max_slices=128` and the arbitrary-macroblock structure advertised (TEST-RESULTS.md, Encode). The setter rejects anything the firmware does not take: only CBR or CQP, up to 60 fps, within the slice geometry bounds. Without it every encoded frame is one slice and `VAConfigAttribEncMaxSlices` can only answer 1. |
| `0014-7.3.4-encode-p010.patch` | this repository | Lists `V4L2_PIX_FMT_P010` on the 7.3.4 encoder's raw queue with a 256-aligned byte pitch, sizes the input with `iris_yuv_buffer_size_p010()`, maps P010/QC10C to their HFI color formats, sends `HFI_PROP_LUMA_CHROMA_BIT_DEPTH` for an encoder and passes the 10-bit flag to the line/scratch/VPSS helpers | **Experimental, one target run 11-18.** Applies after 0013. FFmpeg's `hevc_vaapi -profile:v main10` encoded 60 frames through P010 at SSIM 0.9995 mean (0.9973 worst), the stream reports Main 10 / yuv420p10le, and the 8-bit lane still passes in the same run (TEST-RESULTS.md, Encode). Without it the encoder's raw queue accepts NV12 only, so no VA client can feed a 10-bit picture and `VAProfileHEVCMain10` is not advertised. |
| `0015-7.3.4-mpeg2-decode.patch` | this repository | Registers `V4L2_PIX_FMT_MPEG2` on the SM8550 decoder, maps it to HFI codec 6, and adds the MPEG-2 profile/level caps, port-settings-change list, output subscription and response mapping; internal buffers reuse the H.264 sizes | **Experimental.** Applies after 0014. The SM8550 firmware blob contains MPEG-2 decoder code and Qualcomm's platform data for this HFI generation lists the MPEG-2 tables, but the SM8550 vendor driver has no MPEG-2 codec, so the codec allow-list of this firmware build is unknown. **One target run 11-18: refused by the firmware.** The node enumerates `MPG2` and the profile/level controls appear, but opening a session with codec 6 raises `received system error of type 0x5000003`, the core goes to its error state and the firmware reloads; MPEG-2 is not enabled in this firmware build, and no codec, profile or format is claimed. TEST-RESULTS.md records the run. |
| `0016-7.3.4-capture-sg.patch` | this repository | Adds a `capture_sg` module parameter that selects `vb2_dma_sg_memops` for the decoder's CAPTURE queue and takes the buffer's device address from the mapped scatter list at QBUF; the encoder queues, the OUTPUT queue and every internal buffer keep the contiguous allocator | **Experimental, one target run 11-19.** The video-codec node is behind the SMMU (it is in IOMMU group 9 at boot), so a CAPTURE frame does not have to be contiguous; with the parameter on, the 8K pool is no longer limited to the 384 MiB CMA budget. Built with 0008-0014 and `capture_sg=1`, the build-tree driver granted `pool=15 memory=mmap` at 7680x4320 and all four codecs decoded 30/30 framemd5-exact under Direct and Import; the 48-frame codec matrix and the encode gate (including the 8K lane) passed on the same boot (TEST-RESULTS.md, 8K). A CAPTURE buffer whose DMA mapping is not one segment is refused at QBUF, so a device tree without `iommus` cannot make the firmware write past the first address; a second boot with that guard (module `d65b745e…`) repeated the 8K run with `pool=15 memory=mmap`, Direct publication on all 30 H.264 frames, and Import 30/30 with `memory=import`. Default off: without the parameter the CAPTURE pool still comes from the contiguous carve-out and 8K publishes Copy. |
| `0017-7.3.4-encode-roi.patch` | this repository, after the LKML ROI v10 series (iris half) | Adds a per-picture ROI delta-QP map to the encoder: a module-local `V4L2_CID_CODEC_BASE + 658` (`0x00990b92`) control shaped as a `V4L2_CTRL_TYPE_INTEGER` dynamic s32 array, `BUF_ROIMB_DELTAQP` metadata buffers carrying `HFI_PROP_ROI_INFO` (`0x03000173`), and the `HFI_CMD_DELIVERY_MODE` (`0x0100000A`) subscription made while the session starts; each control write fills the next free metadata buffer with s16 entries `BIT(11) | ((delta & 0x3f) << 4)`, rows aligned to 8 LCUs, and the buffer is attached to the next queued picture | **Experimental, one target run 11-19.** The upstream v10 series patches `v4l2-core` for `V4L2_CTRL_TYPE_S8` and a new control id; this port keeps the upstream id but uses the in-tree `V4L2_CTRL_TYPE_INTEGER` dynamic array, so only `qcom-iris.ko` changes. On the running 7.3.4 kernel (boot `79f5d3e1…`, module `1323d58a…`) the control appears on the encoder node; with the build-tree driver (artifact `e16dd8f9…`, source commit `6c216ba`) the s32 map reaches the module (`elems=8160` for 1920x1080 H.264, delta −31 at both ends), the delivery subscription returns success before the session starts, the metadata buffer is queued with its picture and completed by the firmware without an error, and the coded picture is **byte-identical** to the run without ROI — H.264 and HEVC, ROI on every picture at delta −31, under CBR and CQP alike (TEST-RESULTS.md, Encode). The firmware accepts the metadata and ignores its contents, so the driver advertises no ROI regions unless `V4L2_VA_ROI` is set. |


Patches 0010-0012 were in every module of the 11-19 `capture_sg` series
(`cc34da90…`, `ca7aa394…`, `d65b745e…`). The codec matrix, the 8K runs, AV1
film grain, resolution change, the video processor, two concurrent clients,
the encode gates and the GStreamer gate all passed on those modules, and
`modprobe -r` followed by `modprobe` completed. Their rows above cite the
earlier runs; the sg series is the wider no-regression evidence for the set.

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
# Experimental only: encoder multi-slice controls and P010 input (apply in order).
sh packaging/kernel/build-module.sh --source linux \
    --patch 0008 --patch 0009 --patch 0013 --patch 0014
# Experimental only: MPEG-2 decode, on top of the set above.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0008 --patch 0009 --patch 0013 --patch 0014 --patch 0015
# Experimental only: system-memory CAPTURE allocation for 8K Direct.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0008 --patch 0009 --patch 0013 --patch 0014 --patch 0016
# Experimental only: encoder ROI metadata, on top of the set above.
sh packaging/kernel/build-module.sh --source linux \
    --patch 0008 --patch 0009 --patch 0013 --patch 0014 --patch 0017
```

Load the 0016 module with `options qcom_iris capture_sg=1` (or
`modprobe qcom_iris capture_sg=1`); the parameter is read-only and defaults
off. The driver raises its CAPTURE memory budget from 384 MiB to 720 MiB only
while the parameter reads `Y`, so an unpatched or unparameterised module keeps
the contiguous pool and the Copy policy at 8K.

On 7.3.4, `CONFIG_LOCALVERSION_AUTO` appends `-g<sha>` to the release, so a
tree that has the patches applied as uncommitted changes produces a vermagic
the running kernel does not accept. Commit the applied patches in the scratch
clone, or drop `LOCALVERSION_AUTO` from `.config`, before building.

Never resolve a failed `git apply` by hand or with a relaxed context: the
7.3.4 set adds enum members next to `INST_FW_CAP_MAX` in
`enum platform_inst_fw_cap_type`, and a fuzzy hunk that instead lands them in
the following `enum platform_inst_fw_cap_flags` redefines the flag bits. Every
capability table entry below that point then carries a wrong `flags` value and
answers the wrong control, with no build error. After applying patches, check
where they landed:

```sh
grep -n -B12 '^enum platform_inst_fw_cap_flags' \
    drivers/media/platform/qcom/iris/iris_platform_common.h | \
    grep -E 'DISPLAY_DELAY|SLICE_MODE|SLICE_MAX'
```

Every hit must be above the `enum platform_inst_fw_cap_flags` line. The
same rule covers `HFI_PROP_*` definitions in
`iris_hfi_gen2_defines.h`: a fuzzy hunk that parks one on a neighbouring
`#define` gives a control that registers and then reaches the wrong property.

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

The sixteen patch files are kernel fragments targeting GPL-2.0-only sources under
`drivers/media/platform/qcom/iris/`. They are not compiled into the MIT driver
package, so they have no SPDX header of their own; the patched files keep the
kernel's own identifier. `SHA256SUMS` pins each patch for the downstream
kernel spec.
