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

| Codec | Profile | Buffer mode | Conformance |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import (zero-copy) | Fluster 72/135 |
| HEVC | Main, Main 10 | Import (zero-copy) | Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import (zero-copy) | Fluster 176/305 |
| AV1 | Profile 0 (8/10-bit) | Import (zero-copy, decode-order kernel) | Bit-exact against libdav1d, 8 and 10 bit; film grain synthesis included |

A profile is advertised only for the codecs marked `production: supported` in
`data/iris-codec-capabilities.json`. Every number in the Conformance column is
a frame-by-frame comparison against a software decode; the Fluster counts are
corpus results rather than a per-build regression, and `TEST-RESULTS.md` names
each corpus and classifies every vector that fails. A browser session counts as
hardware only when the media surface reports the selected decoder together with
decoded and dropped frame counts.

Decode is qualified up to 7680x4320 for all four codecs, and for two concurrent
1080p clients.

AV1 film grain synthesis is supported. When a client names a separate `current_display_picture`, the driver decodes the access unit with grain off and blends the grain onto that surface in user space, synchronously with the picture's `endPicture`, before the client can recycle the ungrained `current_frame`. Clients that export grain parameters as side data keep blending it themselves. `test/iris-av1-grain-gate.sh` requires every downloaded frame to be byte-identical to libdav1d's own grained output.

### Encode

| Codec | Entrypoint | Input | Output | Conformance |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9986 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9968 at 1080p; encodes the requested rate under CBR and VBR |

### Video processing

| Entrypoint | Supported operations | Input | Output | Conformance |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc` (`VAProfileNone`, opt-in) | Scale at any ratio (nearest-neighbour, bilinear, or driver-selected: area-average down, bicubic up), crop, rotation, mirroring; NV12/P010 bit-depth conversion; HDR10 to SDR tone mapping; up to 16-layer compositing (Y'CbCr or packed BGRA/RGBA) | NV12, P010, BGRA, RGBA | NV12, P010 | Nearest-neighbour scale and crop byte-identical to swscale; bilinear within 1 code of the 4-tap model; tone mapper within 1 code of BT.2390/BT.709; composite alpha 0 and 1 match the input |

Video processing runs via pinned Vulkan compute kernels on surface DMA-BUFs. It is disabled by default and enabled whenever the `V4L2_VA_VPP` environment variable is set to a non-empty value (e.g. `V4L2_VA_VPP=0` also enables it). Dispatches wait for pending decode completions before reading source surfaces. Shaders, execution order, and pipeline constraints are documented in [docs/architecture.md](docs/architecture.md).

## Performance

HEVC Main 8-bit, FFmpeg 8.1.3, CPU pinned at its maximum frequency and GPU
at 680 MHz. Decode excludes download and display; encode includes the CPU NV12
upload.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 953 fps | 754 fps | 346 fps |
| Encode | 679 fps | 585 fps | 172 fps |

Clocks are pinned because throughput on this SoC follows DVFS more than it
follows the driver; an unpinned run measures the governor. These are the
numbers on the 7.3.4 kernel; the 7.2.6 table was 759 / 581 / 359 and
644 / 591 / 167. Multi-codec sweeps, transcode, concurrency, power and VPP
numbers, and the method behind each cell are in
[TEST-RESULTS.md](TEST-RESULTS.md).

Against software encoders at the same 1080p, 4 Mbit/s, 300 frames and GOP 60,
unpinned (`test/iris-encode-baseline.sh`): H.264 encodes 4.4x faster than
libx264 `veryfast` (507 against 116 fps) at 3.1 against 37.3 ms of client CPU
per frame, and HEVC 15.2x faster than libx265 (549 against 36 fps) at 2.6
against 161.5 ms. Encoding reaches 7680x4320 in both codecs, verified by
frame-hash comparison against a software decode.

## Requirements

- Target SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. GPU copy engine requires EGL, GLES, and GBM.
- Distribution: Fedora 44 is the qualified baseline. Packages for Fedora, Arch and Debian/Ubuntu arm64 are provided in `packaging/` and `debian/`.
- Supporting kernel patches: Located in `packaging/kernel/` with module rebuild instructions. The qualified combination is `sheng-7.3.4` (`87ac0f2347eb`) with patches 0008-0009; `sheng-7.2.6` (`42f3b40c702a`) with 0001-0004 also remains qualified. On stock kernels without these patches, the driver falls back to display-order decoding (GPU copy) and intra-period repositioning for IDR requests.

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

## Client setup

Per-client settings (mpv, VLC, Sunshine, Firefox, GStreamer) and the FFmpeg
decode and encode commands are in
[docs/build-and-deploy.md](docs/build-and-deploy.md#client-setup).

## Memory Models

The driver selects buffer publishing policies dynamically:

- **Direct**: Application exports an allocated surface after decode. Zero-copy.
- **Import**: Application allocates and passes DMA-BUFs before decode. Zero-copy when running on decode-order patched kernels.
- **Copy**: Firmware decodes to driver-owned slots; frames are blitted via GPU into application buffers. Used on stock kernels and for the second half of an in-context AV1 resolution change. AV1 uses the Import policy on decode-order patched kernels.

Imported DMA-BUF surfaces must match native CAPTURE layout (single linear object, 128-byte aligned pitch). Other configurations return `VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known Limits

- **AV1 dynamic resolution change**: The internal eight-slot DPB emits each picture at `vaEndPicture` without holding frames across access units. After an in-context resolution change, if a client continues exporting surfaces with the previous CAPTURE geometry, the driver falls back to GPU copy for those pictures to avoid pitch corruption. Chromium chooses its own AV1 decoder for local files and may pick the software one; the browser acceptance gate in [docs/build-and-deploy.md](docs/build-and-deploy.md#browser-acceptance-gate) decides whether a session is on the hardware.
- **Stock kernel fallback**: On kernels without display-delay or sync-frame controls, decode degrades to GPU copy and requested IDRs are handled via intra-period repositioning.
- **Encode features**: Single full-picture slice only. Multi-slice, tiles and ROI return `VA_STATUS_ERROR_UNIMPLEMENTED`. B pictures are opt-in with `V4L2_VA_B_FRAMES` and are H.264 only; they need a client pipeline of at least three pictures (FFmpeg: `-bf 1 -async_depth 3`).
- **Intra refresh**: A rolling-row request (`VAEncMiscParameterRIR`) reaches the encoder and changes the bitstream, under CBR only; the firmware ignores the period under VBR and CQP, so the driver refuses it there. No client requests it today: FFmpeg's `h264_vaapi`/`hevc_vaapi` and GStreamer's `vah264enc` expose no such option. VA has no encode-side long-term-reference field, so `ltr_count`/`frame_ltr_index`/`use_ltr_frames` are unreachable from any VA client and are not advertised.
- **Packed headers**: Extradata emission reports `VA_ENC_PACKED_HEADER_NONE`. Parameter sets are carried in-band.
- **UBWC surfaces**: The decoder node enumerates the compressed `Q08C`/`Q10C` CAPTURE formats and the firmware writes real UBWC. Turnip decodes it byte-exactly through `VK_EXT_image_drm_format_modifier`, but EGL — the Copy publish path and `vaGetImage`/`vaPutImage` — does not, and `QCOM_COMPRESSED` on P010 is not offered at all. No VA client requests a compressed surface either, so surfaces stay `NV12`/`P010` and imported DMA-BUFs must be `DRM_FORMAT_MOD_LINEAR`. See [TEST-RESULTS.md](TEST-RESULTS.md) for the measurement.

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
| `V4L2_VA_B_FRAMES` | off | Opt in to B pictures in the H.264 encoder; requires a client pipeline of three pictures or more (FFmpeg: `-async_depth 3`) |
| `V4L2_VA_DUMP` | off | Directory path to dump submitted access units |
