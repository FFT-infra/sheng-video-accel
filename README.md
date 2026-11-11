# Qualcomm Iris VA-API Driver (SM8550)

**English** | [中文](README.zh-CN.md)

A libva driver for the Qualcomm Iris V4L2 video decoder and encoder, hardware-optimized for the Xiaomi Pad 6S Pro (sheng, SM8550) on ARM64 Linux. It provides hardware-accelerated video decode and encode for Google Chrome, Firefox, and FFmpeg, routing decoded frames directly into application buffers with zero copy.

Kernel patches, diagnostic scripts, and packaging specs are provided in this repository as supporting assets for hardware validation and deployment.

## Capabilities

### Decode

| Codec | Profile | Buffer Mode | Verification |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import (zero-copy) | Bit-exact MD5, Fluster 72/135 |
| HEVC | Main, Main 10 | Import (zero-copy) | Bit-exact MD5, Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Copy (GPU blit) | Bit-exact MD5 |
| AV1 | Profile 0 | Import (zero-copy, decode-order kernel) | Bit-exact MD5, 2026-11-11 installed-package and browser runs |

Hardware decode verification requires a media surface that reports the selected hardware decoder (for Chrome, `VaapiVideoDecoder` with `kIsPlatformVideoDecoder=true`; for Firefox, successful VA-API initialization) together with decoded and dropped frame counts over the test window.

### Encode

| Codec | Entrypoint | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | 1080p FFmpeg, GOP 60, IRQ delta |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | 1080p FFmpeg, GOP 60, IRQ delta |

Hardware encode verification requires an active Iris IRQ counter increase, error-free bitstream decoding, and valid sequence parameters matching requested profile and geometry.

## Requirements

- Target SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. GPU copy engine requires EGL, GLES, and GBM.
- Distribution: Fedora 44 is the qualified baseline. Packages for Fedora and Arch are provided in `packaging/`.
- Supporting kernel patches: Located in `packaging/kernel/` with module rebuild instructions. On stock kernels without these patches, the driver falls back to display-order decoding (GPU copy) and intra-period repositioning for IDR requests.

## Installation

### From Package

Fedora:
```sh
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch:
```sh
cd packaging/arch && makepkg -si
```

The package installs the driver binary along with the `msm_drv_video.so` symlink, allowing libva to resolve the driver automatically for clients such as mpv, VLC, and GStreamer without requiring `LIBVA_DRIVER_NAME`.

### Diagnostic Tools

- `iris-device-check`: Inspects device nodes, firmware version, bound driver, and exposed capabilities.
- `iris-verify`: Validates hardware decode and encode pipelines via Iris interrupt counter verification.

## Memory Models

The driver selects buffer publishing policies dynamically:

- **Direct**: Application exports an allocated surface after decode. Zero-copy.
- **Import**: Application allocates and passes DMA-BUFs before decode. Zero-copy when running on decode-order patched kernels.
- **Copy**: Firmware decodes to driver-owned slots; frames are blitted via GPU into application buffers. Used on stock kernels, for VP9, and for the second half of an in-context AV1 resolution change. AV1 uses the Import policy on decode-order patched kernels (qualified 2026-11-11 on the installed package, see TEST-RESULTS.md).

Imported DMA-BUF surfaces must match native CAPTURE layout (single linear object, 128-byte aligned pitch). Other configurations return `VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known Limits

- **VP9 zero-copy**: Requires the GPU copy path due to client-side surface lifecycle constraints during multi-picture access unit completions.
- **AV1 dynamic resolution change & browser selection**: The internal eight-slot DPB emits each picture at `vaEndPicture` without holding frames across access units. After an in-context resolution change, if a client continues exporting surfaces with the previous CAPTURE geometry, the driver falls back to GPU copy for those pictures to avoid pitch corruption. For local file playback, Chromium 152 chooses its built-in software AV1 decoder; hardware AV1 is verified on live Chrome 156 web pages and in Firefox 158.
- **AV1 film grain synthesis**: Refused at `endPicture`. VA-API picture parameters distinguish `current_frame` (ungrained reference) from `current_display_picture` (grained display surface). Because the driver maps each CAPTURE completion to a single target surface, synthesis requires a separate dual-target publishing path in user space. Streams with film grain decode in hardware when the client exports grain parameters as side data (`apply_grain=0`).
- **AV1 10-bit profile**: Verified on hardware via a 1080p 60-frame 10-bit capability probe (exact framemd5 with zero-copy Import), but no 10-bit VA profile (such as Main 10) is currently advertised in the driver.
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
| `V4L2_VA_DUMP` | off | Directory path to dump submitted access units |
