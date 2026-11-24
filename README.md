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
| AV1 | Profile 0 (8/10-bit) | Import (zero-copy, decode-order kernel) | Bit-exact against libdav1d; film grain included |

A profile is advertised only for codecs `data/iris-codec-capabilities.json`
marks `production: supported`. The conformance numbers are frame-by-frame
comparisons against a software decode; Fluster counts are corpus results, not
per-build regressions, and every failing vector is classified. Decode is
qualified up to 7680x4320 for all four codecs, and for two concurrent 1080p
clients.

UBWC surfaces are offered unless `V4L2_VA_UBWC=0`: a client that lists
`DRM_FORMAT_MOD_QCOM_COMPRESSED` gets a Q08C surface exported as one R8 layer
per plane, publishing Direct or Import only, because nothing here decompresses
UBWC. AV1 film grain is blended in user space onto `current_display_picture`
before the client can recycle `current_frame`. Both gates require byte-exact
read-back: `test/target/iris-ubwc-gate.sh` through Turnip, `test/target/iris-av1-grain-gate.sh`
against libdav1d.

### Encode

| Codec | Entrypoint | Input | Output | Conformance |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9986 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9968 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC Main 10 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | P010 | Annex-B | SSIM 0.9995 mean at 640x480; needs the 0014 kernel patch |

### Video processing

| Operation | Notes |
| --- | --- |
| Scale | any ratio; nearest, bilinear, or driver-selected (area average down, bicubic up) |
| Crop, rotate, mirror | seven VA directions, NV12 and P010 |
| Bit depth | NV12 <-> P010 |
| Tone mapping | HDR10 to SDR |
| Compositing | up to 16 layers, Y'CbCr or packed BGRA/RGBA |

Input is NV12, P010, BGRA or RGBA; output is NV12 or P010. The backend is
pinned Vulkan compute on surface DMA-BUFs; `V4L2_VA_VPP=0` removes the
entrypoint.

## Requirements

- SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. The GPU copy engine
  needs EGL, GLES and GBM.
- Distribution: Fedora 44 is the qualified baseline. `packaging/` and `debian/`
  carry Fedora, Arch and Debian/Ubuntu arm64 packages.
- Kernel: `sheng-7.3.4` (`87ac0f2347eb`) with patches 0008-0009, or
  `sheng-7.2.6` (`42f3b40c702a`) with 0001-0004; both are qualified. Each
  capability is advertised only when the loaded module provides it. Sources and
  rebuild instructions are in `packaging/kernel/`.

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
`qcom_drv_video.so` aliases, so libva resolves it without `LIBVA_DRIVER_NAME`.
Per-client settings are in `docs/install.md#client-setup`.

### Diagnostic tools

`iris-device-check` is read-only: nodes, firmware version, bound driver and
queue formats. `iris-verify` decodes and encodes a sample clip and requires the
Iris interrupt counters to move. The full four-codec pixel oracle is
`test/target/iris-matrix.sh`. A browser session counts as hardware only when the
media surface names the selected decoder and reports decoded and dropped
frames.

## Performance

HEVC Main 8-bit, FFmpeg 8.1.3, CPU pinned at its maximum frequency and GPU at
680 MHz. Decode excludes download and display; encode includes the CPU NV12
upload.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 953 fps | 754 fps | 346 fps |
| Encode | 679 fps | 585 fps | 172 fps |

Throughput follows DVFS more than the driver, so an unpinned run measures the
governor. At 1080p and 4 Mbit/s, H.264 encodes 4.4x faster than libx264
`veryfast`, HEVC 15.2x faster than libx265, and both reach 7680x4320.

## Memory model

- **Direct**: the client exports the surface the driver allocated, after decode.
- **Import**: the client allocates DMA-BUFs and passes them before decode.
  Zero copy on decode-order kernels.
- **Copy**: firmware decodes into driver-owned slots and the GPU blits into
  client buffers. Used on stock kernels and for the second half of an in-context
  AV1 resolution change.

Imported DMA-BUFs must match the native CAPTURE layout: one linear object with a
128-byte aligned pitch, or four R8 layers at the kernel's Q08C offsets under the
`QCOM_COMPRESSED` modifier. Anything else returns
`VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known limits

- **AV1 resolution change**: after an in-context change, a client that keeps
  exporting surfaces at the old geometry gets GPU copy for those pictures.
- **Stock kernel**: decode degrades to GPU copy, and IDR requests move the intra
  period instead of forcing a frame.
- **Above 8K**: the node advertises up to 8192x8192. H.264 and AV1 decode
  8192x4352; HEVC and VP9 stop at 8192x4320 and fail the first dequeue above
  it.
- **8K Direct**: Direct publication at 8K needs the experimental
  `capture_sg=1`, which is off by default and not part of the qualified module;
  without it Auto publishes Copy. A client that holds every slot leaves the
  drain no buffer for LAST, so HEVC and VP9 report a failed drain.
- **Encode**: slices become a macroblock budget under CBR or CQP, up to 60 fps
  and 4096x2160, and need 0013. Tiles, long-term references and per-layer
  bitrates are not offered. B pictures, ROI, temporal layers and rolling-row
  intra refresh are opt-in and qualified only on the kernel patches they
  require.
- **Packed headers**: HEVC encode answers `VA_ENC_PACKED_HEADER_MISC` and splices
  the client's raw header behind the parameter sets, which is how FFmpeg's
  `hevc_vaapi` carries HDR10 and caption SEI. H.264 answers none, because
  `h264_vaapi`'s buffering-period SEI names the SPS by id and FFmpeg writes no
  parameter sets for MISC alone. The firmware writes sequence and slice headers.

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
| `V4L2_VA_ROI` | off | Opt in to advertising ROI regions; needs the 0017 kernel patch, and the qualified firmware ignores the map |
| `V4L2_VA_UBWC` | on (`0` disables) | Offer UBWC (Q08C) decode surfaces to clients that request the `QCOM_COMPRESSED` modifier |
| `V4L2_VA_HW_GRAIN` | off | Let the firmware apply AV1 film grain from the rebuilt header; the grained picture lands on `current_display_picture` |
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
