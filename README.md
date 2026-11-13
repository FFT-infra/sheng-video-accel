# Qualcomm Iris VA-API Driver (SM8550)

**English** | [中文](README.zh-CN.md)

A libva driver for the Qualcomm Iris V4L2 video decoder and encoder, hardware-optimized for the Xiaomi Pad 6S Pro (sheng, SM8550) on ARM64 Linux. It provides hardware-accelerated video decode and encode for Google Chrome, Firefox, FFmpeg, Kodi, and Sunshine (Moonlight game streaming), routing decoded frames directly into application buffers with zero copy on qualified pipelines.

Kernel patches, diagnostic scripts, and packaging specs are provided in this repository as supporting assets for hardware validation and deployment.

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

| Entrypoint | Operation | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| `VAEntrypointVideoProc` (`VAProfileNone`, opt-in) | scale at any ratio with nearest-neighbour or bilinear interpolation, optional crop, rotation and mirroring; bit-depth conversion between NV12 and P010; HDR10 to SDR tone mapping; global-alpha compositing of up to sixteen layers over the main surface, each Y'CbCr or packed BGRA/RGBA | NV12, P010, BGRA, RGBA | NV12, P010 | `scale_vaapi` byte-identical to swscale `flags=neighbor`; the bilinear method within one code of a four-tap model, constant-invariant and identical to the point map at 1:1; a crop matches `crop,scale`; a cross-depth picture matches `scale` then `format` byte for byte in both directions; all seven `transpose_vaapi` directions match the software filters; the tone mapper within 1 code of its documented pipeline, alone and followed by any of the seven transpose directions, a crop-scale or a composite; the composite's 0 and 1 alpha endpoints match their input frames byte for byte; `overlay_vaapi` over a BGRA upload produces the exact BT.709 codes of a constant overlay |

The processor is off unless `V4L2_VA_VPP` is set. It runs the pinned Vulkan compute kernel on the surface DMA-BUFs and offers one operation, a scale at the ratio the client asks for, sampling where swscale samples, starting from the surface region the client names and applied after the clockwise quarter turn the client asks for. The interpolation method comes from the picture's `filter_flags`: the default and `VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR` are the point map, `VA_FILTER_INTERPOLATION_BILINEAR` blends the four samples around the destination sample's centre, and `VA_FILTER_INTERPOLATION_ADVANCED` is the driver's own pick per axis — the area average when an axis shrinks, where a two-tap filter aliases, and a four-tap cubic when it grows. The pipeline caps name advanced, because the field holds one method rather than a bit set and that is the best of the three. NV12 and P010 surfaces are scaled by the same taps, one kernel per destination sample width, and either source depth feeds either output: the kernel converts `code >> 2` on the way down to eight bits and `code << 2` on the way up to ten before it samples, which is the conversion swscale applies, so a picture that only changes bit depth is byte-identical to `format` and a scale in the same picture matches `scale` then `format`. Colour conversion and the filters outside the one below are refused. Mirroring on either axis, or on both, runs after the rotation and before the scale, so every `transpose_vaapi` direction a client can name works. Its geometry floor is the kernel's own 2x2 rather than the decoder node's minimum, so a client can scale down to a thumbnail. One filter is implemented: `VAProcFilterHighDynamicRangeToneMapping` with HDR10 metadata takes a P010 BT.2020/PQ surface to SDR NV12 through the BT.2390 EETF, the BT.2020 to BT.709 matrix and the BT.709 OETF. It converts the whole surface at its own size and runs first, so a crop, a scale, a rotation, a mirror or a second buffer in the same picture is a later pass over the converted frame rather than a refusal. An NV12 source, HDR-to-HDR and a second filter are still refused. The processor also composites: a picture may carry one main pipeline buffer and up to sixteen blending passes on top of it, each mixing a surface into one even rectangle of the output with a `VABlendState` — the shape `overlay_vaapi` submits, generalised to a stack. The passes run in submission order, so a later one covers an earlier one; the driver copies the main rectangle into the output and then reads the output back for each layer, which is why one kernel serves any number of them. The overlay is either the output's own Y'CbCr format, mixed on the coded values of both planes, or a packed `VA_RT_FORMAT_RGB32` surface (`VA_FOURCC_BGRA` or `VA_FOURCC_RGBA`), converted to the output frame's BT.601, BT.709 or BT.2020 matrix while it is mixed. The overlay is sampled from its own surface region, so a client composites part of a surface rather than all of it, with the same interpolation method the scale kernel runs. Both forms implement `VA_BLEND_GLOBAL_ALPHA` and `VA_BLEND_PREMULTIPLIED_ALPHA`, the flag FFmpeg sets for an overlay input with an alpha channel: with it, a packed overlay's own alpha byte multiplies the global one, so a surface with a real alpha channel produces the picture that channel describes. The output's range comes from `output_color_properties`: a full-range destination is encoded on 0..255 rather than 16..235. Luma keying is refused, and an alpha of 0 or 1 reproduces the main or the overlay frame byte for byte.

## Performance

HEVC Main 8-bit throughput on the SM8550 tablet, FFmpeg 8.1.3, five
interleaved runs per cell with every CPU policy at its maximum
(2016/2803/2956 MHz) and the GPU at 680 MHz. Clips are synthetic I/P at GOP 60
and VBR 5/10/20 Mbps; encoding includes the CPU NV12 upload and runs at
`-async_depth 4`; decoding excludes download and rendering. 900 frames per run
at 720p and 1080p, 480 at 4K, so process start-up is amortised. The comparison
is the sibling `radxa-pkg/qcom-va-driver` built and run on the same node and
clips.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode, this driver | 759 fps | 581 fps | 359 fps |
| Decode, sibling | 759 fps | 579 fps | 358 fps |
| Encode, this driver | 644 fps | 591 fps | 167 fps |
| Encode, sibling | 631 fps | 575 fps | 163 fps |

Decode is at parity; encode is 2.1-2.8 % ahead at all three sizes. Decode also
costs 16-19 % less client CPU per process (0.27/0.42/0.40 s against
0.33/0.50/0.50 s), consistent with this driver deferring the copy engine until
a copy is actually requested. The sibling's published table is SC8280XP and is
not comparable with these SM8550 numbers.

The VA video processor scales 1080p to 720p at 536 fps on 0.54 s of client CPU
per 900 frames, against 287 fps and 8.4 s for
`hwdownload,format=nv12,scale=1280:720` behind the same hardware decode: the
scale runs on the GPU instead of a copy out to the CPU. It is opt-in
(`V4L2_VA_VPP`).

## Requirements

- Target SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. GPU copy engine requires EGL, GLES, and GBM.
- Distribution: Fedora 44 is the qualified baseline. Packages for Fedora, Arch and Debian/Ubuntu arm64 are provided in `packaging/` and `debian/`.
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

Debian / Ubuntu (arm64):
```sh
dpkg-buildpackage -b -us -uc
sudo apt install ../libva-v4l2-iris_0.1.0_arm64.deb
```

Build the Debian package in a chroot (`sbuild`, `pbuilder`) rather than on a
device: `libgbm-dev` is pinned to the distribution's Mesa and can downgrade a
vendor-patched graphics stack.

The package installs the driver binary along with the `msm_drv_video.so` and `qcom_drv_video.so` symlinks, allowing libva to resolve the driver automatically for clients such as mpv, VLC, and GStreamer without requiring `LIBVA_DRIVER_NAME`, and for client recipes that set `LIBVA_DRIVER_NAME=qcom`, the name the sibling distribution uses.

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
| `V4L2_VA_VPP` | off | Opt in to the `VAProfileNone` / `VAEntrypointVideoProc` entrypoint (NV12/P010 scale at any ratio with any of the three interpolation methods, bit-depth conversion, rotation, mirroring, HDR10 to SDR tone mapping, and global-alpha compositing of up to sixteen layers, including packed BGRA/RGBA overlays) |
| `V4L2_VA_DUMP` | off | Directory path to dump submitted access units |

GStreamer's VA plugin (`libgstva`) filters VA drivers by vendor string before
it registers any element, and this driver's vendor string is not on its list:
`gst-inspect-1.0 va` reports `0 features` and `vaav1dec` does not exist.
`GST_VA_ALL_DRIVERS=1` is the plugin's own override and registers the
elements; libva still resolves this driver through its `msm_drv_video.so`
alias, so no `LIBVA_*` variable is involved. `test/iris-gstreamer-gate.sh`
checks both states on the target.
