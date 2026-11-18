# Qualcomm Iris VA-API Driver (SM8550)

**English** | [中文](README.zh-CN.md)

A libva driver for the Qualcomm Iris V4L2 video decoder and encoder on SM8550
(sheng, Xiaomi Pad 6S Pro) running ARM64 Linux. Decoded frames reach client
buffers without a copy on qualified pipelines. Chrome, Firefox, FFmpeg, Kodi,
mpv, GStreamer and Sunshine use it through VA-API.

## Capabilities

### Decode

| Codec | Profile | Buffer mode | Conformance |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import (zero-copy) | Fluster 72/135 |
| HEVC | Main, Main 10 | Import (zero-copy) | Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import (zero-copy) | Fluster 176/305 |
| AV1 | Profile 0 (8/10-bit) | Import (zero-copy, decode-order kernel) | Bit-exact against libdav1d; film grain synthesis included |

A profile is advertised only for codecs marked `production: supported` in
`data/iris-codec-capabilities.json`. Conformance numbers are frame-by-frame
comparisons against a software decode. Fluster counts are corpus results, not
per-build regressions; `TEST-RESULTS.md` classifies every failing vector. The
VP9 reference MD5s predate this machine's libvpx; 176/305 is not a decode-defect
count.

UBWC surfaces are opt-in (`V4L2_VA_UBWC=1`). A client that lists
`DRM_FORMAT_MOD_QCOM_COMPRESSED` in `VASurfaceAttribDRMFormatModifiers` gets a
Q08C surface, the node's four-plane compressed allocation, and
`vaExportSurfaceHandle` reports the modifier with one R8 layer per plane. Such
a surface publishes Direct or Import only: Copy, the image entry points, film
grain, video processing and the encoder return `VA_STATUS_ERROR_UNIMPLEMENTED`,
because nothing here decompresses UBWC. Q10C is not offered.
`test/iris-ubwc-gate.sh` decodes all four codecs into Q08C and requires a GPU
read-back (`test/vk-ubwc-read`, Turnip) to reproduce the linear decode's luma
and chroma byte for byte.

Decode is qualified up to 7680x4320 for all four codecs, and for two concurrent
1080p clients.

AV1 film grain synthesis runs in user space. When a client names a separate
`current_display_picture`, the driver blends grain onto that surface before the
client can recycle `current_frame`. Clients that pass grain as side data blend
it themselves. `test/iris-av1-grain-gate.sh` requires every frame to match
libdav1d's grained output byte for byte.

### Encode

| Codec | Entrypoint | Input | Output | Conformance |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9986 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9968 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC Main 10 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | P010 | Annex-B | SSIM 0.9995 mean at 640x480; needs the 0014 kernel patch, and the profile is advertised only when the module takes P010 |

### Video processing

| Operation | Notes |
| --- | --- |
| Scale | any ratio; nearest, bilinear, or driver-selected (area average down, bicubic up) |
| Crop, rotate, mirror | seven VA directions, NV12 and P010 |
| Bit depth | NV12 <-> P010 |
| Tone mapping | HDR10 to SDR |
| Compositing | up to 16 layers, Y'CbCr or packed BGRA/RGBA |

Input is NV12, P010, BGRA or RGBA; output is NV12 or P010.

Video processing uses pinned Vulkan compute kernels on surface DMA-BUFs. A build
that found Vulkan exposes `VAProfileNone` / `VAEntrypointVideoProc`;
`V4L2_VA_VPP=0` removes it. Details are in `docs/architecture.md`.

## Requirements

- SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. The GPU copy engine
  needs EGL, GLES and GBM.
- Distribution: Fedora 44 is the qualified baseline. `packaging/` and `debian/`
  carry Fedora, Arch and Debian/Ubuntu arm64 packages.
- Kernel: `sheng-7.3.4` (`87ac0f2347eb`) with patches 0008-0009, or
  `sheng-7.2.6` (`42f3b40c702a`) with 0001-0004. Both are qualified. On 7.3.4,
  0013 adds the encoder multi-slice controls and 0014 adds P010 input for HEVC
  Main 10; the driver advertises each capability only when the loaded module
  provides it. On a stock kernel the driver still builds and installs; the
  fallback is under Known limits. Sources and rebuild instructions are in
  `packaging/kernel/`.

## Install

Fedora:
```sh
sh scripts/make-dist.sh --out ~/rpmbuild/SOURCES
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch:
```sh
sh scripts/make-dist.sh
cd packaging/arch && makepkg -si \
    IRIS_SOURCE_COMMIT=$(git -C ../.. rev-parse HEAD) \
    IRIS_TRACKED_SOURCE_SHA256=$(git -C ../.. ls-files -z \
        | xargs -0 sha256sum | sha256sum | awk '{print $1}')
```

Debian / Ubuntu (arm64):
```sh
dpkg-buildpackage -b -us -uc
sudo apt install ../libva-v4l2-iris_0.1.0_arm64.deb
```

Build the Debian package in a chroot (`sbuild`, `pbuilder`): `libgbm-dev` is
pinned to the distribution's Mesa and can downgrade a vendor-patched graphics
stack. The package installs the driver plus the `msm_drv_video.so` and
`qcom_drv_video.so` aliases. libva resolves the driver without
`LIBVA_DRIVER_NAME`.

## Verify

```sh
iris-device-check
iris-verify
```

`iris-device-check` is read-only: nodes, firmware version, bound driver and
queue formats. `iris-verify` decodes and encodes a sample clip and requires the
Iris interrupt counters to move. The full four-codec pixel oracle is
`test/iris-matrix.sh`. A browser session counts as hardware only when the media
surface names the selected decoder and reports decoded and dropped frames; the
procedure is in `docs/install.md`.

## Performance

HEVC Main 8-bit, FFmpeg 8.1.3, CPU pinned at its maximum frequency and GPU at
680 MHz. Decode excludes download and display; encode includes the CPU NV12
upload.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 953 fps | 754 fps | 346 fps |
| Encode | 679 fps | 585 fps | 172 fps |

These are the numbers on the qualified 7.3.4 combination. Throughput follows
DVFS more than the driver, so an unpinned run measures the governor. Sweeps,
power and method are in `TEST-RESULTS.md`.

At 1080p and 4 Mbit/s, H.264 encodes 4.4x faster than libx264 `veryfast`, HEVC
15.2x faster than libx265, and both reach 7680x4320
(`test/iris-encode-baseline.sh`).

## Memory model

- **Direct**: the client exports the surface the driver allocated, after decode.
- **Import**: the client allocates DMA-BUFs and passes them before decode.
  Zero copy on decode-order kernels.
- **Copy**: firmware decodes into driver-owned slots and the GPU blits into
  client buffers. Used on stock kernels and for the second half of an in-context
  AV1 resolution change.

Imported DMA-BUFs must match the native CAPTURE layout: one linear object with a
128-byte aligned pitch. A UBWC surface imports four R8 layers at the kernel's
Q08C offsets under the `QCOM_COMPRESSED` modifier. Anything else returns
`VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known limits

- **AV1 resolution change**: after an in-context change, a client that keeps
  exporting surfaces at the old geometry gets GPU copy for those pictures.
  Chromium may choose its software AV1 decoder for local files; the browser gate
  decides what ran.
- **Stock kernel**: decode degrades to GPU copy, and IDR requests move the intra
  period instead of forcing a frame.
- **Encode**: a client's slice list becomes a macroblock budget
  (`ceil(macroblocks / slices)`) under CBR or CQP, up to 60 fps and 4096x2160,
  and needs the 0013 kernel patch; without it every frame is one full-picture
  slice and `VAConfigAttribEncMaxSlices` answers 1. Tiles and ROI return
  `VA_STATUS_ERROR_UNIMPLEMENTED`. B pictures are opt-in with
  `V4L2_VA_B_FRAMES`, H.264 only, and need at least three pictures in flight
  (FFmpeg: `-bf 1 -async_depth 3`).
- **Temporal layers**: a `VAEncMiscParameterTemporalLayerStructure` names 1-6
  layers and the firmware derives the membership from the count, so the
  client's ids are bounds-checked rather than followed. Per-layer bitrates are
  not honoured. Advertised through `VAConfigAttribEncRateControlExt` when the
  node registers the hierarchical controls.
- **Intra refresh**: rolling-row requests (`VAEncMiscParameterRIR`) are honoured
  under CBR and refused under VBR/CQP. `ltr_count`, `frame_ltr_index` and
  `use_ltr_frames` are not advertised.
- **Packed headers**: extradata reports `VA_ENC_PACKED_HEADER_NONE`; parameter
  sets travel in-band.
- **UBWC**: opt-in (`V4L2_VA_UBWC=1`), 8-bit Q08C only, and only through Direct
  or Import publication. The Copy path, `vaDeriveImage`/`vaGetImage`/
  `vaPutImage`, film grain, video processing and the encoder have no
  decompressor and refuse a compressed surface. Q10C has no reader and is not
  offered; imports must carry the modifier and the kernel's four-plane offsets.

## Environment variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Override path to the V4L2 decoder node |
| `LIBVA_V4L2_ENCODER_PATH` | probe | Override path to the V4L2 encoder node |
| `V4L2_VA_TRACE` | off | Enable stderr execution traces |
| `V4L2_VA_SYNC_TIMEOUT_MS` | 2000 | Bounded session wait timeout in milliseconds |
| `V4L2_VA_COPY` | gpu | Copy engine selection (`gpu` or `cpu`) |
| `V4L2_VA_PUBLISH` | auto | Force publication mode (`auto`, `direct`, `import`, `copy`) |
| `V4L2_VA_VPP` | on | Set to `0` to disable Vulkan video processing |
| `V4L2_VA_B_FRAMES` | off | Opt in to B pictures in the H.264 encoder; needs three pictures in flight (FFmpeg: `-async_depth 3`) |
| `V4L2_VA_UBWC` | off | Opt in to UBWC (Q08C) decode surfaces for clients that request the `QCOM_COMPRESSED` modifier |
| `V4L2_VA_DUMP` | off | Directory path to dump submitted access units |

## Naming

Five names describe the same component. Each is fixed by a different consumer,
so renaming one does not rename the others:

| Layer | Name | Fixed by |
| --- | --- | --- |
| Repository | `qualcomm-iris-vaapi` | downstream `Source0` URLs and product lockfiles |
| Meson project | `libva-v4l2` | `meson.build`; inherited from the upstream fork |
| Source archive | `libva-v4l2-iris-<version>.tar.gz` | `scripts/make-dist.sh`, `%autosetup -n` |
| Package (RPM, deb, PKGBUILD) | `libva-v4l2-iris` | `packaging/` and `debian/` |
| Runtime VA driver | `v4l2_drv_video.so`, `LIBVA_DRIVER_NAME=v4l2` | libva's `<name>_drv_video.so` convention |
