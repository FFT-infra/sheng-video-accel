# Testing Qualcomm Iris VA-API

Test suite and verification gates for the Qualcomm Iris VA-API driver: host-side
unit tests (runnable in CI without V4L2 hardware), diagnostics for installed
systems, target hardware gates, and external conformance corpora.

---

## 1. Host-Side Static Checks & Unit Tests

Host-side checks validate packaging, node resolution, trace vocabulary, and non-hardware invariants before code is deployed to physical targets.

### Running Host Tests

```sh
# Run static checks (syntax, invariants, packaging, trace vocabulary)
sh test/run-static-checks.sh

# Run compiled unit tests
meson test -C build-local --print-errorlogs
```

### Packaging & Distribution Checks

`test/iris-packaging-check.sh` validates downstream packaging without building packages:
- Verifies that all installed paths exist in the RPM spec's `%files`.
- Confirms that RPM, PKGBUILD, and Debian manifests delegate to `scripts/install-system.sh`.
- Asserts that `v4l-utils`, FFmpeg, and Vulkan build/runtime dependencies are declared.
- Verifies provenance refresh after stripping.

> **Note (Debian Packaging)**: Building the Debian package requires a Debian host or chroot (`sbuild`, `pbuilder`). `dh_shlibdeps` aborts on Fedora hosts due to missing shlibs databases. See `docs/development.md` for the chroot recipe.

---

## 2. Diagnostics on Installed Systems

User-facing diagnostic scripts verify hardware acceleration on an active system without requiring external test harnesses:

| Command | Purpose |
| --- | --- |
| `iris-device-check` | Read-only report: inspects device nodes, firmware version, bound driver, queue formats, and optional kernel controls. |
| `iris-verify` | Functional verification: generates a sample clip, runs decode and encode, and verifies that hardware Iris interrupt counters advance. |
| `iris-configure-clients` | Helper script to configure user-level hardware acceleration in mpv (`hwdec=vaapi`) and Firefox. |

Neither replaces the hardware gates below; both are first-line diagnostics
when a decode does not reach the hardware.

---

## 3. Target Hardware Verification Gates

Hardware gates execute on the physical SM8550 tablet and verify bit-exact pixel oracles, session lifetimes, and browser integration.

### Remote Execution via Experiment Guard

Target gates should be executed remotely under `scripts/iris-experiment-guard.sh`:

```sh
IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh \
    -- sh test/iris-gstreamer-gate.sh --clip <clip-path>
```

`gate.sh` records `summary.json` (schema `qualcomm-iris-vaapi/target-run-v1`),
module and driver hashes, boot ID, thermal and battery state, and the command's
output. `identity.driver` is the driver the command resolves: a
`LIBVA_DRIVERS_PATH` override hashes that build tree, while
`identity.installed_driver` keeps the system package hash for comparison.
A run whose guarded child prints nothing is refused as a false green, even
when the guard itself reached `complete` with exit 0; set
`IRIS_GATE_ALLOW_EMPTY_OUTPUT=1` for a deliberately silent command.
`test/iris-gate-output-self-test.sh` pins that contract on the host.

### Browser Acceptance Gate

Verifies that Google Chrome uses `VaapiVideoDecoder` with platform acceleration.
Fedora's Chromium decodes through its own native V4L2 stack, so a Chromium run
says nothing about this driver:

```sh
test/iris-browser-acceptance.sh --clip <sample-path>
```

Acceptance requires inspecting `chrome://media-internals` during playback:
- `kVideoDecoderName = VaapiVideoDecoder`
- `kIsPlatformVideoDecoder = true`
- Zero dropped frames over the test window.

### Codec Matrix & Pixel Oracles

- **`test/iris-matrix.sh`**: The authoritative positive VA-API pixel oracle. Generates clips, decodes via FFmpeg VA-API, and requires exact per-frame MD5 equality against software decoders across H.264, HEVC, VP9, and AV1.
- **`test/iris-structure-matrix.sh`**: Isolates H.264 reference structures (all-I, IP-only, B=2, B=4) with bit-exact MD5 checks.
- **`test/iris-zero-copy-suite.sh`**: Validates the Direct zero-copy path (`publish=direct`) with zero GPU/CPU copy overhead.
- **`test/iris-dynamic-resolution.sh`**: Alternates 640x360 and 1280x720 geometries 100 times to verify in-stream dynamic resolution changes across single-process and multi-process contexts.
- **`test/iris-concurrency-soak.sh`**: Multi-context thermal and stability soak (defaults to 7200 s). Enforces a 100 °C thermal guard, monitors battery draw, and requires exact MD5 preflight checks:
  ```sh
  ./test/iris-concurrency-soak.sh dual-h264
  ./test/iris-concurrency-soak.sh mixed
  ```
- **`test/iris-8k-gate.sh`**: 7680x4320 decode for all four qualified codecs. Asserts the granted CAPTURE pool is the memory-budgeted one (not the old fixed 32), exact framemd5 against a software baseline, terminal LAST and zero completion errors. See [docs/development.md](../docs/development.md#8k-geometry-and-two-client-gates) for the clip generation.
- **`test/iris-two-player-gate.sh`**: Two independent 1920x1080 ffmpeg processes with their own VA display and driver trace, both required to finish with exact pixels and no completion errors. Leaves `LIBVA_DRIVERS_PATH` unset to test the installed package.
- **`test/iris-vp9-p2-gate.sh`**: VP9 Profile 2 (10-bit) decode. Generates its own `yuv420p10le` clip, requires `VAEntrypointVLD` to answer `VA_RT_FORMAT_YUV420_10` only, and checks that a client opening the config with no RT-format attribute still gets P010 surfaces byte-identical to a software decode. Pins the `createConfig` default and the `getConfigAttributes` answer tested by `test/unit/va-config-attributes-test.cc`.
- **`test/iris-ubwc-gate.sh`**: UBWC (Q08C) CAPTURE for all four codecs. Generates an access-unit-delimited stream per codec, decodes it twice through the session — once into Q08C and once into NV12 — and requires `iris-decode-stream-probe` to report the kernel's four-plane Q08C layout. `test/vk-ubwc-read` then imports the exported compressed buffer through `VK_EXT_image_drm_format_modifier`, copies both planes on the GPU, and requires luma and chroma to equal the linear decode sample for sample; a linear image built from the same reference runs through the identical copy first, so the read-back path is controlled. Needs Turnip and a Vulkan-enabled build. `test/iris-va-ubwc-probe` adds the VA-API half: a VP9 decode client asks for `QCOM_COMPRESSED` and checks the exported four-plane descriptor and the image-entry-point refusals, then runs again without `V4L2_VA_UBWC`, where a compressed-only list must be refused and a list with a linear fallback must get `LINEAR`.

---

## 4. Video Processing (VPP) Gates

Video post-processing runs via pinned Vulkan compute shaders (`VAEntrypointVideoProc`, available by default on a build that found Vulkan; disable with `V4L2_VA_VPP=0`).

| Gate Script | Description | Oracle / Tolerance |
| --- | --- | --- |
| `test/vulkan-vpp/prereq.sh` | Builds compute pipeline, imports DMA-BUFs as Vulkan external memory, verifies coherent CPU read-back. | Exact shader output. |
| `test/vulkan-vpp/nv12_scale_gate.sh` | NV12 scale primitive over imported CAPTURE DMA-BUFs across scaling ratios, crops, and clockwise quarter turns. | Every pixel checked against swscale point map; bilinear within 1 code of double-precision model. |
| `test/vulkan-vpp/p010_scale_gate.sh` | 10-bit P010 scaling primitive gate, validating full 16-bit word handling and chroma pair alignments. | Full 16-bit sample check. |
| `test/vulkan-vpp/rotation_model_check.py` | Host-side check comparing coordinate maps against FFmpeg software rotation filters. | Runs in CI without GPU. |
| `test/vulkan-vpp/transpose_vaapi_gate.sh` | End-to-end VA-API rotation and mirroring across all 7 directions in NV12 and P010. | Byte-exact match with FFmpeg software filters. |
| `test/vulkan-vpp/tonemap_gate.sh` | HDR10 to SDR tone mapping (`VAProcFilterHighDynamicRangeToneMapping`). | Within 2 codes of BT.2390/BT.709 double-precision model. |
| `test/vulkan-vpp/overlay_gate.sh` | Global-alpha multi-layer compositing (`VA_BLEND_GLOBAL_ALPHA`) across 15 cases. | Within 2 codes of double-precision mixing model; alpha 0/1 match input frames byte-exact. |
| `test/vulkan-vpp/overlay_vaapi_gate.sh` | Real-client compositing via `overlay_vaapi` with BGRA surfaces and premultiplied alpha. | Exact match against BT.709 reference. |
| `test/vulkan-vpp/crop_vaapi_gate.sh` | VideoProc cropping (`surface_region`) with scale. | Byte-exact match with `crop,scale` filter. |

---

## 5. Subsystem Probes & Utilities

Target-compiled diagnostic probe binaries:

- **`test/iris-import-probe.cc`**: Acceptance binary for client-owned CAPTURE DMA-BUFs. With one client buffer in flight (`surfaces=1`) it requires picture $k$ to land in the DMA-BUF queued for it; with several (`surfaces=8`) it measures the firmware's own completion order, which is not QBUF order and is why the Import policy queues one picture at a time.
- **`test/iris-va-import-probe.cc`**: Validates `DRM_PRIME_2` and `PRIME_1` import through `vaCreateSurfaces2`.
- **`test/iris-va-image-gate.sh` / `iris-va-image-probe.cc`**: Upload/download testing for `vaPutImage` and `vaGetImage`.
- **`test/iris-gstreamer-gate.sh`**: Validates GStreamer integration (`vaav1dec`, `vah264enc`, `vah265enc`) and allowlist behavior.
- **`test/iris-alloc-chain.sh`**: Walks the stable buffer fallback hierarchy (MSM GEM coherent -> MSM GEM write-combined -> system DMA heap) using `test/alloc-fault.so`.
- **`test/iris-encode-quality.sh`**: Verifies hardware encode fidelity (SSIM floors and IRQ counter increments), including a 7680x4320 lane per codec (`IRIS_ENCODE_8K=0` skips it). `IRIS_ENCODE_EXTRA_GEOM` adds further geometries, each with the same oracle and `IRIS_ENCODE_EXTRA_FRAMES` frames.
- **`test/iris-encode-baseline.sh`**: Same source and bitrate through `h264_vaapi`/`hevc_vaapi` and libx264/libx265. Reports fps, client CPU seconds per frame and the round-to-round spread; `--power` also runs the encode workload through `iris-power-sampler.sh`.
- **`test/iris-encode-ir-probe.cc`**: Encodes one multi-picture CBR stream through the public VA API twice, with and without a rolling intra refresh, and requires the bitstream to differ. The unit tests cover the controls and the mapping; only the firmware can say whether the request changes what it codes. `test/iris-encode-control-probe.cc` is the same idea for the deblocking controls.
- **`test/iris-encode-slice-probe.cc`**: Encodes IDR pictures through the public VA API with one slice and with N slices, and counts the coded slice NAL units. The unit tests cover the translator and the control writes; only the firmware can say whether it splits the picture the client asked for.
- **`test/iris-encode-temporal-probe.cc`**: Encodes one GOP of HEVC pictures through the public VA API with a `VAEncMiscParameterTemporalLayerStructure`, and requires the coded stream to carry a picture in every temporal id the client named. The unit tests cover the translation and the control writes; only the firmware can say whether it codes a hierarchy.
- **`test/iris-encode-main10-gate.sh`**: Verifies HEVC Main 10 encode end to end: FFmpeg uploads a 10-bit source as P010, the stream must report profile Main 10 and a 10-bit pixel format, the platform decode must match the software decode, SSIM against the source must clear the floor with a blank control below it, and an 8-bit lane in the same run must still pass.
- **`test/iris-encode-hdr-sei-gate.sh`**: Verifies the raw packed header path end to end. An HDR10 source with mastering-display and content-light-level side data is re-encoded through `hevc_vaapi`, and the output's own frames must still report both side-data blocks while the 8-bit lane in the same run still encodes.
- **`test/iris-encode-b-gate.sh`**: Verifies H.264 B pictures end to end. With `V4L2_VA_B_FRAMES` set, FFmpeg's `-bf 1` arm must code B pictures, decode to the requested picture count, clear the SSIM floor (with a blank control below it) and repeat byte-identically; without the opt-in the same request must fall back to intra and P pictures, and HEVC must stay P-only under the opt-in.
- **`test/iris-encode-roi-probe.cc`**: Encodes the same stream twice through the public VA API, the second with `VAEncMiscParameterTypeROI` regions on every other picture, and requires both to encode every picture and the payloads to differ. Run with `V4L2_VA_ROI=1` on a kernel carrying the 0017 patch; on the qualified firmware the two streams are identical because the firmware completes the metadata and ignores the map (TEST-RESULTS.md), which is why the probe reports the difference rather than presuming it.
- **`test/iris-encode-misc-bound-probe.cc`**: Built-only target probe for an encode misc parameter buffer whose element is too small for its header. Requires `renderPicture` to refuse a 1-byte element and a header-only QualityLevel element with `VA_STATUS_ERROR_INVALID_PARAMETER`, while a full element still opens a picture. The host unit tests cannot open an encoder context, so this runs against the target's nodes.
- **`test/iris-power-sampler.sh`**: Battery-side power of a decode (`--clip`) or an encode (`--workload encode --codec h264|hevc`) against software, as an increment over a pre/post baseline.
- **`test/iris-throughput.sh`**: Decode throughput through this driver against the native V4L2 decoders on the target. A regression guard for two builds on the same boot, not a cross-machine benchmark.
- **`test/iris-hevc-conformance.sh`**: Compares this driver with libavcodec on the HEVC vectors Fluster records as pixel-different, with both sides normalised to one pixel format before hashing.
- **`test/iris-hevc-scaling-check.sh`**: Opt-in target regression for `scaling_list_enabled_flag`: a VA IQ buffer is required even when the stream uses the default scaling lists.
- **`test/av1-obu-roundtrip.sh`**: Decodes the driver's rebuilt AV1 OBU stream (`V4L2_VA_DUMP`) with libdav1d and compares it with a software decode of the source.
- **`test/iris-av1-browser-matrix.sh`**: One AV1 clip through Firefox (WebDriver BiDi) and Chrome (CDP) plus the driver trace, against the installed package with no overrides.
- **`test/vulkan-vpp/make-spv.sh`**: Host-only regeneration of the checked-in SPIR-V modules and `SHA256SUMS`; the target has no glslang.

---

## 6. External Corpora & Reference Standards

For deep conformance testing, the following external test suites are used:

- **[Fluster](https://github.com/fluendo/fluster)**: Codec conformance testing across H.264 (`JVT-AVC_V1`), HEVC (`JCT-VC-HEVC_V1`), VP9, and AV1 test vectors.
- **[Chromium media/test/data](https://chromium.googlesource.com/chromium/src/+/lkgr/media/test/data/)**: Container parsing, non-zero start times, and discard-at-start validation.
- **[GStreamer GstValidate](https://gstreamer.freedesktop.org/documentation/gst-devtools/gst-validate.html)**: Play, seek, flush, and EOS pipeline validation.
- **[Qualcomm v4l-video-test-app](https://github.com/quic/v4l-video-test-app)**: Native Qualcomm V4L2 M2M decoder test client.
- **[v4l2-compliance](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)**: Kernel V4L2 ioctl and buffer queue semantics verification.
