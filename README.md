# Qualcomm Iris VA-API Driver (SM8550)

**English** | [中文](README.zh-CN.md)

A libva driver for the Qualcomm Iris V4L2 video decoder and encoder on SM8550
(sheng, Xiaomi Pad 6S Pro) running ARM64 Linux. Decoded frames reach client
buffers without a copy on qualified pipelines. Chrome, Firefox, FFmpeg, Kodi,
mpv, GStreamer and Sunshine use it through the standard VA-API interface.

## Capabilities

### Decode

| Codec | Profile | Buffer mode | Conformance |
| --- | --- | --- | --- |
| H.264 | Constrained Baseline, Main, High | Import (zero-copy) | Fluster 72/135 |
| HEVC | Main, Main 10 | Import (zero-copy) | Fluster 141/147 |
| VP9 | Profile 0, Profile 2 | Import (zero-copy) | Fluster 176/305 |
| AV1 | Profile 0 (8/10-bit) | Import (zero-copy, decode-order kernel) | Bit-exact against libdav1d; film grain synthesis included |

A profile is advertised only for codecs marked `production: supported` in
`data/iris-codec-capabilities.json`. Every conformance number is a frame-by-frame
comparison with a software decode; the Fluster counts are corpus results rather
than per-build regressions, and `TEST-RESULTS.md` names each corpus and
classifies every vector that fails. The VP9 reference MD5s predate this
machine's libvpx, so 176/305 is not a decode-defect count.

Decode is qualified up to 7680x4320 for all four codecs, and for two concurrent
1080p clients.

AV1 film grain synthesis runs in user space. When a client names a separate
`current_display_picture`, the driver decodes with grain off and blends it onto
that surface before the client can recycle the ungrained `current_frame`.
Clients that pass grain parameters as side data keep blending it themselves.
`test/iris-av1-grain-gate.sh` requires every frame to match libdav1d's grained
output byte for byte.

### Encode

| Codec | Entrypoint | Input | Output | Conformance |
| --- | --- | --- | --- | --- |
| H.264 | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9986 at 1080p; encodes the requested rate under CBR and VBR |
| HEVC | `VAEntrypointEncSlice` (CBR, VBR, CQP) | NV12 | Annex-B | SSIM 0.9968 at 1080p; encodes the requested rate under CBR and VBR |

### Video processing

| Operation | Notes |
| --- | --- |
| Scale | any ratio; nearest, bilinear, or driver-selected (area average down, bicubic up) |
| Crop, rotate, mirror | seven VA directions, NV12 and P010 |
| Bit depth | NV12 <-> P010 |
| Tone mapping | HDR10 to SDR |
| Compositing | up to 16 layers, Y'CbCr or packed BGRA/RGBA |

Input is NV12, P010, BGRA or RGBA; output is NV12 or P010. Nearest-neighbour
scale and crop are byte-identical to swscale, bilinear is within 1 code of the
4-tap model, tone mapping within 1 code of BT.2390/BT.709, and composite alpha 0
and 1 match the input.

Video processing uses pinned Vulkan compute kernels on surface DMA-BUFs. A build
that found Vulkan exposes `VAProfileNone` / `VAEntrypointVideoProc`; set
`V4L2_VA_VPP=0` to remove it. Dispatches wait for pending decode completions
before reading source surfaces. Shaders and pipeline constraints are in
[docs/architecture.md](docs/architecture.md).

## Requirements

- SoC: Qualcomm SM8550 (HFI gen2 Iris VPU), aarch64.
- Core dependencies: `libva` >= 1.1, `libdrm` >= 2.4.52. The GPU copy engine
  needs EGL, GLES and GBM.
- Distribution: Fedora 44 is the qualified baseline. `packaging/` and `debian/`
  carry Fedora, Arch and Debian/Ubuntu arm64 packages.
- Kernel: `sheng-7.3.4` (`87ac0f2347eb`) with patches 0008-0009, or
  `sheng-7.2.6` (`42f3b40c702a`) with 0001-0004. Both are qualified. The driver
  still builds and installs on a stock kernel; see Known limits for the
  fallback. Patch sources and rebuild instructions are in `packaging/kernel/`.

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

## Verify

```sh
iris-device-check
iris-verify
```

`iris-device-check` is read-only: nodes, firmware version, bound driver and
queue formats. `iris-verify` decodes and encodes a sample clip and requires the
Iris interrupt counters to move. The full four-codec pixel oracle is
`test/iris-matrix.sh`. A browser session counts as hardware only when the media
surface names the selected decoder and reports decoded and dropped frames; see
[docs/build-and-deploy.md](docs/build-and-deploy.md#browser-acceptance-gate).

## Performance

HEVC Main 8-bit, FFmpeg 8.1.3, CPU pinned at its maximum frequency and GPU at
680 MHz. Decode excludes download and display; encode includes the CPU NV12
upload.

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 953 fps | 754 fps | 346 fps |
| Encode | 679 fps | 585 fps | 172 fps |

Clocks are pinned because throughput on this SoC follows DVFS more than the
driver; an unpinned run measures the governor. These are the numbers on the
qualified 7.3.4 combination. Multi-codec sweeps, transcode, concurrency, power,
VPP numbers and the method behind each cell are in
[TEST-RESULTS.md](TEST-RESULTS.md).

At 1080p, 4 Mbit/s, 300 frames, GOP 60, unpinned
(`test/iris-encode-baseline.sh`): H.264 is 4.4x faster than libx264 `veryfast`
(507 against 116 fps) and HEVC 15.2x faster than libx265 (549 against 36 fps),
at 3.1 against 37.3 ms and 2.6 against 161.5 ms of client CPU per frame. Both
codecs reach 7680x4320, verified by frame hash against a software decode. With
the screen off on battery, encode adds 2.8 and 3.0 W over idle against 4.8 and
5.7 W for the software encoders (`test/iris-power-sampler.sh --workload
encode`).

## Memory model

- **Direct**: the client exports the surface the driver allocated, after decode.
- **Import**: the client allocates DMA-BUFs and passes them before decode.
  Zero copy on decode-order kernels.
- **Copy**: firmware decodes into driver-owned slots and the GPU blits into
  client buffers. Used on stock kernels and for the second half of an in-context
  AV1 resolution change.

Imported DMA-BUFs must match the native CAPTURE layout: one linear object with a
128-byte aligned pitch. Anything else returns
`VA_STATUS_ERROR_INVALID_PARAMETER`.

## Known limits

- **AV1 resolution change**: after an in-context change, a client that keeps
  exporting surfaces at the old geometry gets GPU copy for those pictures
  instead of a pitch mismatch. Chromium may choose its software AV1 decoder for
  local files; the browser gate decides what ran.
- **Stock kernel**: decode degrades to GPU copy, and IDR requests move the intra
  period instead of forcing a frame.
- **Encode**: one full-picture slice. Multi-slice, tiles and ROI return
  `VA_STATUS_ERROR_UNIMPLEMENTED`. B pictures are opt-in with
  `V4L2_VA_B_FRAMES`, H.264 only, and need at least three pictures in flight
  (FFmpeg: `-bf 1 -async_depth 3`).
- **Intra refresh**: rolling-row requests (`VAEncMiscParameterRIR`) are honoured
  under CBR and refused under VBR/CQP. No current client requests them. VA has
  no encode-side long-term-reference field, so `ltr_count`, `frame_ltr_index`
  and `use_ltr_frames` are unreachable and not advertised.
- **Packed headers**: extradata reports `VA_ENC_PACKED_HEADER_NONE`; parameter
  sets travel in-band.
- **UBWC**: the node enumerates `Q08C`/`Q10C` and the firmware writes real UBWC.
  Turnip decodes Q08C byte-exactly, but EGL (the Copy path and
  `vaGetImage`/`vaPutImage`) does not, and P010 has no compressed format. No VA
  client asks for one, so surfaces stay `NV12`/`P010` and imports must be
  `DRM_FORMAT_MOD_LINEAR` planes.

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

## Documentation

- Client settings and FFmpeg commands: [docs/build-and-deploy.md](docs/build-and-deploy.md#client-setup)
- Tests, gates and diagnostics: [test/README.md](test/README.md)
- Internal design and trace vocabulary: [docs/architecture.md](docs/architecture.md)
- Current qualified combination: [docs/STATE.md](docs/STATE.md)
- Hardware evidence: [TEST-RESULTS.md](TEST-RESULTS.md)
