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

> **Note (Debian Packaging)**: Building the Debian package requires a Debian host or chroot (`sbuild`, `pbuilder`). `dh_shlibdeps` aborts on Fedora hosts due to missing shlibs databases. See `TEST-RESULTS.md` §"Debian packaging" for details.

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
    --out /tmp/iris-run-$$ \
    -- sh test/iris-gstreamer-gate.sh --clip <clip-path>
```

`gate.sh` records `summary.json` (schema `qualcomm-iris-vaapi/target-run-v1`),
module and driver hashes, boot ID, thermal and battery state, and the command's
output. `identity.driver` is the driver the command resolves: a
`LIBVA_DRIVERS_PATH` override hashes that build tree, while
`identity.installed_driver` keeps the system package hash for comparison.

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
- **`test/iris-concurrency-soak.sh`**: Multi-context thermal and stability soak (defaults to 7200 s). Enforces an 85 °C thermal ceiling, monitors battery draw, and requires exact MD5 preflight checks:
  ```sh
  ./test/iris-concurrency-soak.sh dual-h264
  ./test/iris-concurrency-soak.sh mixed
  ```

---

## 4. Video Processing (VPP) Gates

Video post-processing runs via pinned Vulkan compute shaders (`VAEntrypointVideoProc`, opt-in via `V4L2_VA_VPP=1`).

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

- **`test/iris-import-probe.cc`**: Acceptance binary for client-owned CAPTURE DMA-BUFs (validates that picture $k$ lands in the $k$-th queued DMA-BUF).
- **`test/iris-va-import-probe.cc`**: Validates `DRM_PRIME_2` and `PRIME_1` import through `vaCreateSurfaces2`.
- **`test/iris-va-image-gate.sh` / `iris-va-image-probe.cc`**: Upload/download testing for `vaPutImage` and `vaGetImage`.
- **`test/iris-gstreamer-gate.sh`**: Validates GStreamer integration (`vaav1dec`, `vah264enc`, `vah265enc`) and allowlist behavior.
- **`test/iris-alloc-chain.sh`**: Walks the stable buffer fallback hierarchy (MSM GEM coherent -> MSM GEM write-combined -> system DMA heap) using `test/alloc-fault.so`.
- **`test/iris-encode-quality.sh`**: Verifies hardware encode fidelity (SSIM floors and IRQ counter increments).

---

## 6. External Corpora & Reference Standards

For deep conformance testing, the following external test suites are used:

- **[Fluster](https://github.com/fluendo/fluster)**: Codec conformance testing across H.264 (`JVT-AVC_V1`), HEVC (`JCT-VC-HEVC_V1`), VP9, and AV1 test vectors.
- **[Chromium media/test/data](https://chromium.googlesource.com/chromium/src/+/lkgr/media/test/data/)**: Container parsing, non-zero start times, and discard-at-start validation.
- **[GStreamer GstValidate](https://gstreamer.freedesktop.org/documentation/gst-devtools/gst-validate.html)**: Play, seek, flush, and EOS pipeline validation.
- **[Qualcomm v4l-video-test-app](https://github.com/quic/v4l-video-test-app)**: Native Qualcomm V4L2 M2M decoder test client.
- **[v4l2-compliance](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)**: Kernel V4L2 ioctl and buffer queue semantics verification.
