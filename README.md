# Qualcomm Iris VA-API Driver (SM8550)

**English** | [中文](README.zh-CN.md)

A libva driver for the Qualcomm Iris V4L2 video decoder and encoder, hardware-optimized for the Xiaomi Pad 6S Pro (sheng, SM8550) on ARM64 Linux. It provides hardware-accelerated video decode and encode for Google Chrome, Firefox, FFmpeg, Kodi, and Sunshine (Moonlight game streaming), routing decoded frames directly into application buffers with zero copy on qualified pipelines.

Kernel patches, diagnostic scripts, and packaging specs are provided in this repository as supporting assets for hardware validation and deployment.

## Project identity

Five names describe the same component. Each is fixed by a different consumer,
so renaming one does not rename the others:

| Layer | Name | Fixed by |
| --- | --- | --- |
| Repository | `qualcomm-iris-vaapi` | downstream `Source0` URLs and product lockfiles |
| Meson project | `libva-v4l2` | `meson.build`; inherited from the upstream fork |
| Source archive | `libva-v4l2-iris-<version>.tar.gz` | `scripts/make-dist.sh`, `%autosetup -n` |
| Package (RPM, deb, PKGBUILD) | `libva-v4l2-iris` | `packaging/` and `debian/` |
| Runtime VA driver | `v4l2_drv_video.so`, `LIBVA_DRIVER_NAME=v4l2` | libva's `<name>_drv_video.so` convention |

`msm_drv_video.so` and `qcom_drv_video.so` are installed aliases of the same
driver, for clients that resolve by DRM driver name or that set
`LIBVA_DRIVER_NAME=qcom`. The source archive is an upstream build artifact:
`.gitattributes` keeps CI, agent instructions and experiment records out of it,
while the three distribution packaging formats stay in because the shipped
spec runs `test/iris-packaging-check.sh` against them.

## Capabilities

### Decode

| Codec | Profile | Buffer Mode | Verification |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import (zero-copy) | Bit-exact MD5, Fluster 72/135 |
| HEVC | Main, Main 10 | Import (zero-copy) | Bit-exact MD5, Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import (zero-copy) | Bit-exact MD5, 2026-11-12 Chrome alt-ref runs |
| AV1 | Profile 0 (8/10-bit) | Import (zero-copy, decode-order kernel) | Bit-exact MD5, 2026-11-11 installed-package and browser runs; 1080p and 4K 10-bit exact against libdav1d over P010 Import; film grain synthesis exact at 8 and 10 bits |

Hardware decode verification requires a media surface that reports the selected hardware decoder (for Chrome, `VaapiVideoDecoder` with `kIsPlatformVideoDecoder=true`; for Firefox, successful VA-API initialization) together with decoded and dropped frame counts over the test window.

AV1 film grain synthesis is supported. When a client names a separate `current_display_picture`, the driver decodes the access unit with grain off and blends the grain onto that surface in user space, synchronously with the picture's `endPicture`, before the client can recycle the ungrained `current_frame`. Clients that export grain parameters as side data keep blending it themselves. `test/iris-av1-grain-gate.sh` requires every downloaded frame to be byte-identical to libdav1d's own grained output.

### Encode

| Codec | Entrypoint | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | 1080p FFmpeg, GOP 60, IRQ delta |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | 1080p FFmpeg, GOP 60, IRQ delta |

Hardware encode verification requires an active Iris IRQ counter increase, error-free bitstream decoding, and valid sequence parameters matching requested profile and geometry.

### Video processing

| Entrypoint | Supported operations | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc` (`VAProfileNone`, opt-in) | Scale at any ratio (nearest-neighbour, bilinear, or driver-selected: area-average down, bicubic up), crop, rotation, mirroring; NV12/P010 bit-depth conversion; HDR10 to SDR tone mapping; up to 16-layer compositing (Y'CbCr or packed BGRA/RGBA) | NV12, P010, BGRA, RGBA | NV12, P010 | `scale_vaapi` nearest-neighbour and crop byte-identical to swscale; bilinear within 1 code of 4-tap model; tone mapper within 1 code of BT.2390/BT.709 pipeline; composite alpha 0 and 1 endpoints match input frames byte-exact |

Video processing runs via pinned Vulkan compute kernels on surface DMA-BUFs. It is disabled by default and enabled whenever the `V4L2_VA_VPP` environment variable is set to a non-empty value (e.g. `V4L2_VA_VPP=0` also enables it). Dispatches wait for pending decode completions before reading source surfaces. Shaders, execution order, and pipeline constraints are documented in [docs/architecture.md](docs/architecture.md).

## Performance

HEVC Main 8-bit throughput on the SM8550 tablet, FFmpeg 8.1.3, five interleaved runs per cell with CPU policies at max (`performance`, 2016/2803/2956 MHz) and GPU at 680 MHz (`userspace`). Synthetic I/P clips at GOP 60, `-async_depth 4`. Decode excludes download/display; encode includes CPU NV12 upload.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 759 fps | 581 fps | 359 fps |
| Encode | 644 fps | 591 fps | 167 fps |

Decode client CPU time is 0.27/0.42/0.40 s per 900 frames at the three sizes; the deferred copy-engine initialization keeps the GPU copy backend idle on a pipeline that never asks for a copy. Encode client CPU time is 1.01/1.94/4.82 s. Detailed multi-codec sweeps (including AV1 8/10-bit), concurrency, transcode, power, and VPP benchmarks are documented in [TEST-RESULTS.md](TEST-RESULTS.md).

## Requirements

- Target SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. GPU copy engine requires EGL, GLES, and GBM.
- Distribution: Fedora 44 is the qualified baseline. Packages for Fedora, Arch and Debian/Ubuntu arm64 are provided in `packaging/` and `debian/`.
- Supporting kernel patches: Located in `packaging/kernel/` with module rebuild instructions. On stock kernels without these patches, the driver falls back to display-order decoding (GPU copy) and intra-period repositioning for IDR requests.

## Installation

### From Package

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

Build the Debian package in a chroot (`sbuild`, `pbuilder`) rather than on a
device: `libgbm-dev` is pinned to the distribution's Mesa and can downgrade a
vendor-patched graphics stack.

The package installs the driver binary along with the `msm_drv_video.so` and `qcom_drv_video.so` symlinks, allowing libva to resolve the driver automatically for clients such as mpv, VLC, and GStreamer without requiring `LIBVA_DRIVER_NAME`, and for client recipes that set `LIBVA_DRIVER_NAME=qcom`.

### Diagnostic Tools

- `iris-device-check`: Inspects device nodes, firmware version, bound driver, and exposed capabilities.
- `iris-verify`: Validates hardware decode and encode pipelines via Iris interrupt counter verification.

## Memory Models

The driver selects buffer publishing policies dynamically:

- **Direct**: Application exports an allocated surface after decode. Zero-copy.
- **Import**: Application allocates and passes DMA-BUFs before decode. Zero-copy when running on decode-order patched kernels.
- **Copy**: Firmware decodes to driver-owned slots; frames are blitted via GPU into application buffers. Used on stock kernels and for the second half of an in-context AV1 resolution change. AV1 uses the Import policy on decode-order patched kernels (qualified 2026-11-11 on the installed package, see TEST-RESULTS.md).

Imported DMA-BUF surfaces must match native CAPTURE layout (single linear object, 128-byte aligned pitch). Other configurations return `VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known Limits

- **AV1 dynamic resolution change & browser selection**: The internal eight-slot DPB emits each picture at `vaEndPicture` without holding frames across access units. After an in-context resolution change, if a client continues exporting surfaces with the previous CAPTURE geometry, the driver falls back to GPU copy for those pictures to avoid pitch corruption. For local file playback, Chromium 152 chooses its built-in software AV1 decoder; hardware AV1 is verified on live Chrome 156 web pages and in Firefox 158.
- **Stock kernel fallback**: On kernels without display-delay or sync-frame controls, decode degrades to GPU copy and requested IDRs are handled via intra-period repositioning.
- **Encode features**: Single full-picture slice only. Multi-slice, tiles, B-frames, and ROI return `VA_STATUS_ERROR_UNIMPLEMENTED`.
- **Packed headers**: Extradata emission reports `VA_ENC_PACKED_HEADER_NONE`. Parameter sets are carried in-band.

## Environment Variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Override path to the V4L2 decoder node |
| `LIBVA_V4L2_ENCODER_PATH` | probe | Override path to the V4L2 encoder node |
| `V4L2_VA_TRACE` | off | Enable stderr execution traces |
| `V4L2_VA_SYNC_TIMEOUT_MS` | 2000 | Bounded session wait timeout in milliseconds |
| `V4L2_VA_COPY` | gpu | Copy engine selection (`gpu` or `cpu`) |
| `V4L2_VA_PUBLISH` | auto | Force publication mode (`auto`, `direct`, `import`, `copy`) |
| `V4L2_VA_VPP` | off | Enable Vulkan video processing (`VAEntrypointVideoProc`: scale, crop, rotate, mirror, NV12/P010 conversion, tone map, overlay) |
| `V4L2_VA_DUMP` | off | Directory path to dump submitted access units |

> **Tip (GStreamer)**: The GStreamer VA plugin (`libgstva`) filters drivers by a vendor allowlist. To register elements with this driver (`vaav1dec`, etc.), set `GST_VA_ALL_DRIVERS=1`. Libva resolves the driver automatically via `msm_drv_video.so` without needing `LIBVA_*` variables; `test/iris-gstreamer-gate.sh` checks both states on the target.
