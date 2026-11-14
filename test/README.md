# Testing

## Host Unit Tests
Run the host-side unit tests without hardware:
```sh
meson test -C build-local
```

`test/iris-packaging-check.sh` is the one host check that reads the packaging
instead of the code. It runs inside `run-static-checks.sh` and fails when an
installed path is missing from the spec's `%files`, when any of the three
manifests (`packaging/fedora`, `packaging/arch`, `debian/`) stops delegating
to `scripts/install-system.sh` or stops refreshing the provenance manifest
after its own strip step, when `v4l-utils` is dropped from the runtime
dependencies, or when a format forgets the Vulkan or FFmpeg build dependency
the driver and its test stage need.

Building the Debian package needs a Debian host or chroot: `dh_shlibdeps`
aborts on a Fedora machine because there is no shlibs database for its
libraries. `TEST-RESULTS.md` §"Debian packaging" records the chroot recipe and
what the artifact was run against.

## On an installed system

`scripts/iris-verify.sh` is the user-facing check, not a gate: it generates
its own clip, requires libva to open the driver with no environment set,
requires a decode and an encode to each move the Iris interrupt counter, and
requires the encoded stream to decode. `scripts/iris-device-check.sh` is the
read-only report in front of it: which nodes exist, what each queue exposes,
which optional kernel controls are present. Neither is a substitute for the
gates below; both are what to run first when a report says "hardware decode
does not work".

## Hardware Gates
On the tablet, run the following verification gates:
- `test/iris-browser-acceptance.sh --clip ...`: verifies Chrome `VaapiVideoDecoder` selection, platform decoder status, and zero dropped frames.
- `test/remote/gate.sh -- command`: runs any target gate remotely under the experiment guard and writes `summary.json` (schema `qualcomm-iris-vaapi/target-run-v1`) with the boot, kernel, module hash, driver hash, resolved decoder and encoder names, exit status and guard state, plus the command's captured output in `guard/command.stdout`. Set `IRIS_REMOTE_HOST`; it does not deploy, so the recorded hashes are what the target actually holds. The command is landed in a per-run script and read by path: a heredoc fed to the guard's backgrounded `sh -s` child gets `/dev/null` for stdin and would run nothing while still reporting exit 0.
- `test/vulkan-vpp/prereq.sh`: VPP prerequisite gate. On the target it builds a SPIR-V compute pipeline, imports two DMA-BUF fds as Vulkan external memory (one read, one written with coherent CPU read-back) and checks exact shader output for both DMA-BUF origins the driver uses (MSM GEM PRIME fd and system DMA heap). Run it through `test/remote/gate.sh`. Needs a Vulkan ICD, `libvulkan`, libdrm + `msm_drm.h`, Vulkan headers, a system DMA heap, an MSM render node, `cc` and `pkg-config`; CI has no GPU, so `run-static-checks.sh` only verifies the `SHA256SUMS` hash pin and script syntax. `make-spv.sh` regenerates both SPIR-V binaries on a host with `glslangValidator`.
- `test/vulkan-vpp/nv12_scale_gate.sh`: NV12 scale primitive gate. The prereq above proves a flat DMA-BUF can be a compute source and destination; this one proves the NV12 plane math a real VPP output needs. It dispatches `nv12_scale.spv` over two imported DMA-BUFs laid out exactly like the Iris CAPTURE NV12 buffers (128-byte stride, 32-row luma, 16-row chroma behind luma) and checks **every** destination pixel against a CPU oracle that uses swscale's point positions and VA's rotation order, over a matrix of ratios, crops and clockwise quarter turns, for both DMA-BUF origins, with a distinct sample per source address so a wrong stride, offset, subsample, rotation or sample position cannot pass. A second pass runs the same kernel with the four-tap bilinear method (`--interp 1`), compared within one code of a double-precision model of the blend, plus the two invariants that hold at any ratio: a constant picture must come out that constant (the weights sum to one) and a 1:1 scale must reproduce the point map byte for byte. Run it through `test/remote/gate.sh`. Needs the same target prerequisites plus `spirv-val`; CI has no GPU, so it stays out of `meson test` and `run-static-checks.sh` only pins `nv12_scale.spv`, checks script syntax, and asserts the per-pixel oracle is intact. This is evidence for the VPP kernel only: it advertises no `VAProfileNone` / `VAEntrypointVideoProc` and changes no capability.
- `test/vulkan-vpp/p010_scale_gate.sh`: the same gate for `p010_scale.spv`, built with `scale_probe.c --p010`. The oracle decodes the same 16-bit words the kernel copies, so the whole sample is compared, not only its top eight bits. Its crop cases start away from the origin: a chroma pair spans two luma columns and four bytes in P010, so a crop at 0 cannot separate the real origin from a pair at two bytes. The bilinear pass runs there too, on the ten-bit code rather than the padded word: a kernel that blended the words would leave bits in the low six and a clamp carried over from the 8-bit kernel would clip every highlight above 255.
- `test/vulkan-vpp/rotation_model_check.py`: the host half of the rotation work, and the only part that runs in CI. It applies the same coordinate map as `scale_probe.c` to an NV12 frame and compares it with `crop,transpose,scale` (a half-turn is `hflip,vflip`, since ffmpeg 8 dropped the named one from the software filter), over non-square geometries and a crop. A second list does the same for the bilinear map against `flags=bilinear`, on upscales only: swscale widens its support when it downsamples while the kernel always takes two neighbours per axis, so a downscale is a different filter by design rather than a map to check. It checks the formula, not the driver.
- `test/vulkan-vpp/transpose_vaapi_gate.sh`: rotation and mirroring through the whole VA path. Uploads a frame, runs `transpose_vaapi` in all seven directions (`clock`, `cclock`, `reversal`, `hflip`, `vflip`, `clock_flip`, `cclock_flip`), downloads it and compares byte for byte with FFmpeg's software filters, in NV12 and P010. Because `transpose_vaapi` refuses any direction missing from `pipeline_caps.mirror_flags`, the mirrored cases are also the caps check. It also requires the entrypoint to stay refused without `V4L2_VA_VPP`; `IRIS_VPP_INSTALLED=1` runs it against the installed package.
- `test/vulkan-vpp/tonemap_gate.sh`: HDR10 to SDR tone mapping through the VA path. `iris-va-vpp-probe tonemap` builds the `VAProcFilterParameterBufferType` buffer (HDR10 metadata naming the mastering peak) and the pipeline buffer naming `VAProcFilterHighDynamicRangeToneMapping`, converts a synthetic P010 surface to NV12, and compares every output byte with its own implementation of the pipeline `tonemap.comp` documents, within 2 codes since the kernel is float32 and the model is double. Three properties are checked separately so a model and a kernel that share a mistake still fail: PQ black on 16/128/128, the mastering peak on 235, and a monotone ramp. Two peaks run one frame and their outputs must differ, which is what proves the metadata is read; an NV12 source, a filter without parameters and a rotation in the same picture must be refused, and so is the entrypoint without `V4L2_VA_VPP`. `IRIS_VPP_INSTALLED=1` runs it against the installed package. FFmpeg cannot be the client here: `tonemap_vaapi` refuses a raw P010 frame because it demands mastering-display side data a rawvideo input cannot carry.
- `test/vulkan-vpp/overlay_gate.sh`: global-alpha compositing through the VA path. `iris-va-vpp-probe overlay` submits what `overlay_vaapi` submits — the main surface over the whole output, then the overlay surface into one rectangle with a `VABlendState` naming `VA_BLEND_GLOBAL_ALPHA` — and compares every output byte with its own double-precision model of the mix `overlay.comp` and `overlay_rgb.comp` document over fifteen cases on both sample widths, within 2 codes. The cases cover the Y'CbCr overlay, the stored-alpha form of the same mix on both bit depths, packed BGRA and RGBA overlays (two byte orders, both read the way VA names them), the BT.601 matrix, a packed overlay onto a tone-mapped main surface, and a main surface padded wider than its picture. Two endpoint runs are compared with the input frames themselves: alpha 0 over the whole surface must reproduce the main frame byte for byte and alpha 1 the overlay frame. Stored alpha also gets a check the model cannot give: the same picture with a premultiplied frame and with the straight one has to land within 1 code. Luma keying, a third pass, a blend before its base pass, an odd or out-of-bounds rectangle, a crop of the main surface and an alpha outside 0..1 must be refused, and so is the entrypoint without `V4L2_VA_VPP`. `IRIS_VPP_INSTALLED=1` runs it against the installed package.
- `test/vulkan-vpp/overlay_vaapi_gate.sh`: the same composite through the real client. `overlay_vaapi` uploads a BGRA frame into a `VA_RT_FORMAT_RGB32` surface and always sets `VA_BLEND_PREMULTIPLIED_ALPHA` for an overlay input with an alpha channel, so this is the shape the probe cannot reproduce on its own. Both frames are a single colour, so the gate computes the one expected BT.709 triple itself and requires an exact match, plus the main frame byte for byte at alpha 0 with a black overlay. The entrypoint must stay refused without `V4L2_VA_VPP`.
- `test/vulkan-vpp/crop_vaapi_gate.sh`: a VideoProc picture with a `surface_region`. `iris-va-vpp-probe` uploads an NV12 frame, runs one cropped scale and downloads the result; the gate compares it byte for byte with `crop=W:H:X:Y,scale=DWxDH:flags=neighbor`. No FFmpeg filter submits a region (there is no `crop_vaapi` in 8.1), which is why this one needs its own client. The last case asks for an odd region and requires a refusal.
- `test/iris-va-image-gate.sh`: the upload half of the image API. Runs `iris-va-image-probe` (full-frame and regional `vaPutImage`, a derived-image write, and the three refusals) and then four `hwupload,hwdownload` round trips through FFmpeg — NV12 at 64x64 and 128x72, P010 at 64x64, NV12 at 32x32 — each compared byte for byte with the input. `IRIS_GATE_INSTALLED=1` runs the same cases from `/usr/lib64/dri` with no `LIBVA_DRIVERS_PATH`; `IRIS_IMAGE_PROBE` names the probe binary when the build tree is not beside the driver.
- `test/iris-gstreamer-gate.sh --clip ...`: the client-side GStreamer half. Checks that libgstva's vendor allowlist is what keeps `gst-inspect-1.0 va` at 0 features and that `GST_VA_ALL_DRIVERS=1` registers the elements, then compares `vaav1dec` frame hashes with the software decode and decodes back what `vah264enc` and `vah265enc` produced. The decode comparison muxes the element's I420; a raw `fdsink` dump is not comparable because it ignores `GstVideoMeta`'s plane offset.
- `test/iris-matrix.sh`: framemd5 output equivalence against software.
- `test/iris-hevc-scaling-check.sh`: HEVC `scaling_list_enabled_flag` regression. It encodes a synthetic clip with x265's default scaling lists, decodes it through the driver and requires every frame to match the software decode. The SPS uses the standard default lists, so the driver must still supply a VA IQ buffer; opt-in target gate with the same shape as the others.
- `test/iris-real-clip-gate.sh`: real 120-frame AV1 clip and the in-context DRC clip against software framemd5, with the session's completion and publish counts. Needs `IRIS_REAL_CLIP_DIR` holding the clips and software baselines; opt-in from `test/iris-hardware-suite.sh`. `IRIS_REAL_CLIP_SET=4k` runs only the short 3840x2160 AV1 Import probe; `all` does not include it. A guarded 4K probe is not a thermal, power or long-window result.
- `test/iris-throughput.sh`: decode fps through this driver against the native V4L2 decoders, plus per-frame MD5 at every size it runs.
- `test/iris-power-sampler.sh`: battery-side power of a saturated decode, this driver against software, arms interleaved. Needs the screen off; a paced playback run does not resolve the difference on this gauge.
- `test/iris-structure-matrix.sh`: decoding structural variants (all-I, B-frames).
- `test/iris-dynamic-resolution.sh`: handles in-stream resolution changes.
- `test/iris-concurrency-soak.sh`: multi-context thermal/stability soaking.
- `test/av1-obu-roundtrip.sh`: software oracle for AV1 OBU repacking.

`annexb-test` covers the NAL splitting a requested IDR needs (both NAL
layouts, the parameter sets a cut stream must carry);
`encode-session-test` covers the session rules, including the on-demand
IDR lever and the strictly rising timestamps, and the coded buffer's claim:
whichever of `vaSyncSurface`, `vaSyncBuffer` and `vaMapBuffer` arrives first
takes the access unit, and the others succeed with those bytes rather than
replacing them with an empty answer.

These probes build with the tree and run on the target only:

- `test/iris-import-probe.cc <decoder-node> <stream.264>`: the Import
  acceptance program — proves the firmware writes picture k into the k-th
  queued client DMA-BUF (docs/architecture.md §9). The stream must be an
  H.264 Annex-B file with one AUD per picture and flat luma encoding the
  decode-order index (`16 + 2*(k%100)`), and it must be **High or Main
  profile**: x264's `-qp 0` produces High 4:4:4 Predictive, which the
  firmware rejects with `session error 0x4000008 unsupported stream` and no
  SOURCE_CHANGE, so the probe exits at bring-up. `-qp 1 -profile:v high`
  is lossless enough for the grey values and decodes.
- `test/iris-va-import-probe.cc <render-node>`: the same client memory
  entered through the public API. It needs `LIBVA_DRIVER_NAME=v4l2` and
  `LIBVA_DRIVERS_PATH` pointing at the build tree; it checks DRM_PRIME_2 and
  PRIME_1 import, that the surface aliases the client's bytes, that an
  encode from it matches the same picture from a driver-allocated surface,
  and that a wrong pitch is refused.
- `test/iris-va-image-probe.cc <render-node>`: the upload half of the image
  API with no decode involved — a full-frame `vaPutImage` and a regional one,
  a derived-image write, `vaGetImage` after each, and the refusals
  (`vaPutImage` does not scale; 4:2:0 regions are even and inside both
  surfaces). `test/iris-va-image-gate.sh` runs it and then the same two
  routes through FFmpeg's `hwupload,hwdownload`.
- `test/iris-encode-control-probe.cc <encoder-node>`: which loop-filter
  controls the node exposes, and that a deblocking request it cannot honour
  is refused rather than ignored.
- `test/iris-encode-fence-probe.cc <encoder-node> <render-node> [w h]`:
  whether an input surface's pending GPU write is waited for before the
  picture is queued. It submits a buffer whose last GPU blit is still in
  flight and compares the access unit against the picture before and after
  that write; with the wait removed it encodes the stale picture, which is
  what makes the comparison evidence rather than a coincidence.
- `test/gpu-fence-check.cc <render-node> [w h]`: whether a GPU job leaves a
  pollable pending fence on its destination after `glFlush()`. That is the
  window the probe above depends on; if it is not observable on a GPU, no
  probe can claim the wait mattered.
- `test/iris-fd-leak-probe.cc <render-node> [cycles]`: whether the descriptor
  count returns to its starting value across the surface lifecycle. Counts
  `/proc/self/fd`, and with `IRIS_TEST_FD_LEAK=1` leaks on purpose to prove
  the accounting can fail.
- `test/iris-encode-quality.sh [encoder-node] [render-node]`: encode fidelity
  against the source. Asserts the encode moved the Iris IRQ counter, the
  stream decodes, every decoded frame matches a software decode, and both the
  mean and the worst-frame SSIM against the source clear a floor that a blank
  stream cannot. It measures `$LIBVA_DRIVERS_PATH` (default `build/src`), so
  naming `/usr/lib64/dri` verifies an installed package.

## Qualcomm Iris codec matrix

The repeatable matrix (media generation, VA-API decode through FFmpeg, EOS
oracle and per-frame MD5 against software) is `test/iris-matrix.sh`. Every
script resolves the Iris node by driver name through `test/lib/iris-env.sh`
and keeps artifacts under `$IRIS_LAB_ROOT/artifacts/` by default:

```
LIBVA_DRIVERS_PATH=$PWD/build/src ./test/iris-matrix.sh
```

H.264, HEVC, VP9 and AV1 run through VA and must match software exactly;
AV1's lane passes `--hidden-pictures` because the stream's alt-ref pictures
are decoded but shown later by the client. `IRIS_MATRIX_FRAMES` shortens a
development run.

`test/iris-structure-matrix.sh` isolates H.264 reference structure: all-I,
IP-only, B=2 and B=4 streams with exact MD5 equality. Keep GOP-middle
`-c copy` clips out of this positive matrix; a cut that starts on a P/B
picture without its references is a negative random-access test.

`test/iris-zero-copy-suite.sh` is the Direct-path oracle: FFmpeg does not
pre-export surfaces, so every completion must be `publish=direct` with zero
copy-engine involvement and software-identical pixels.

`test/iris-alloc-chain.sh` walks the stable-buffer allocation chain
(`docs/architecture.md` §3.4): MSM GEM cached-coherent, MSM GEM
write-combined, system DMA heap. The last rung is only reached when the
first two are refused, which no ordinary run does, so the script runs
`test/iris-alloc-probe` under `test/alloc-fault.so` — an `LD_PRELOAD` shim
that returns `EINVAL` (or, for the `coherent-enomem` mode, `ENOMEM`) from the
MSM GEM allocations the probe asks it to refuse. Each mode asserts the
resulting `MemoryOrigin`, that the allocation log shows exactly the rungs the
chain may try, and that a written pattern survives a CPU sync:

```
./test/iris-alloc-chain.sh /dev/dri/renderD128 build
```

The `ENOMEM` mode is the one that matters for correctness: `msm_alloc.c`
masks only `EINVAL` and `EOPNOTSUPP`, so a real failure must fall straight to
the DMA heap without a write-combined retry. The probe refuses to run an
injected mode without the shim, or with the shim loaded but
`IRIS_TEST_ALLOC_FAULT` unset, so a pass cannot come from the fast path
happening to work.

`test/iris-dynamic-resolution.sh` alternates 640x360 and 1280x720 one
hundred times for H.264 (default) or VP9 (`IRIS_DYNAMIC_CODECS=vp9`) and
requires byte-identical native output, exact MD5 from one VA process across
every change, and exact output from 101 fresh VA processes.
`IRIS_DYNAMIC_SWITCHES` shortens a development run.

For dual-H.264 and mixed H.264+HEVC soaks:

```
./test/iris-concurrency-soak.sh dual-h264
./test/iris-concurrency-soak.sh mixed
```

Both default to two hours, retain progress, traces, temperature samples and
boot IDs, refuse to start after an interrupted predecessor (guard file),
enforce an 85 C ceiling, wait for the SoC to cool after generating media, and
run an exact MD5 preflight first. `IRIS_SOAK_SECONDS` shortens a development
run; shortened output is labelled and is not release evidence.

Trace checkers for a client run with `V4L2_VA_TRACE=1` (the record
vocabulary is in `docs/architecture.md` §8):

```
test/iris-eos-check.sh /path/to/trace.log 720            # every AU completed, one drain
test/iris-eos-check.sh /path/to/trace.log 720 --allow-bounded-sync
test/iris-ending-check.sh /path/to/trace.log             # no sync timeouts, terminal drain present
test/iris-context-retirement-check.sh /path/to/trace.log # every destroyed context finished
```

Firmware experiments (anything that can reset the decoder) go through
`scripts/iris-experiment-guard.sh -- command`, which records the boot ID,
temperature and pack state (`battery.log`), applies a timeout, and leaves an
`interrupted` marker that must be reviewed before another run. The guard
records a low or discharging pack instead of refusing it; report the start and
end capacity with the result. `IRIS_EXPERIMENT_BATTERY_ROOT` overrides the
power-supply directory for a host-side self-test.

## Full target hardware suite

`iris-hardware-suite.sh` combines the repository matrices with a V4L2
capability inventory, `v4l2-compliance`, native GStreamer EOS checks, the
Qualcomm `v4l-video-test-app`, and optional Fluster conformance runs. It is
device-agnostic, probes for `iris_driver`, and fails rather than guessing a
node; set `V4L2_DEVICE` (or `LIBVA_V4L2_VIDEO_PATH`) for an inspected override.
All logs and generated media stay
under a scratch directory (`$IRIS_TEST_SCRATCH_ROOT`, default `${TMPDIR:-/tmp}`):

```
IRIS_HARDWARE_SUITE_DIR=/tmp/iris-hardware-suite \
./test/iris-hardware-suite.sh
```

The repository matrices are enabled by default. Set `IRIS_RUN_REPO_MATRICES=0`
when only external tools are desired. To add the Qualcomm JSON client, point
`IRIS_V4L_TEST_BINARY` at `iris_v4l2_test` and provide a whitespace-separated
`IRIS_V4L_TEST_CONFIGS` list. Set `IRIS_V4L_TEST_CWD` to the Qualcomm
checkout root when a JSON file uses relative `InputPath`/`Outputpath` values.
To add Fluster, set `FLUSTER_DIR`,
`IRIS_FLUSTER_SUITES` and `IRIS_FLUSTER_DECODERS`, for example:

```
FLUSTER_DIR=$HOME/iris-vaapi-lab/tools/fluster \
IRIS_FLUSTER_SUITES='JVT-AVC_V1 JCT-VC-HEVC_V1 VP9-TEST-VECTORS AV1-TEST-VECTORS' \
IRIS_FLUSTER_DECODERS='GStreamer-H.264-V4L2' \
./test/iris-hardware-suite.sh
```

`GStreamer-*-V4L2` talks directly to the Iris kernel node and therefore
characterises firmware capabilities without loading this VA-API driver. To
exercise this repository's code, select the matching `FFmpeg-*-VAAPI` decoder:

```
FLUSTER_DIR=$HOME/iris-vaapi-lab/tools/fluster \
IRIS_FLUSTER_SUITES='JVT-AVC_V1' \
IRIS_FLUSTER_DECODERS='FFmpeg-H.264-VAAPI' \
V4L2_VA_SYNC_TIMEOUT_MS=100 \
./test/iris-hardware-suite.sh
```

Fluster's stock FFmpeg VA-API wrapper uses a software pixel-format filter and
does not request a VAAPI output pool explicitly. Its results are useful for
finding surface-lifetime and timeout boundaries, but the repository's
`iris-matrix.sh` is the authoritative positive VA-API pixel oracle. Record the
decoder name with every Fluster result so firmware-only numbers are not
mistaken for driver coverage. Fluster's non-zero result is retained as a
capability warning because its corpus intentionally includes profiles, bit
depths, dimensions and dynamic-resolution modes that a particular Iris
firmware may not implement. The raw JSON and log are the review artifacts.
`v4l2-compliance` is strict except for the known stateful mem2mem control
classification; set
`IRIS_ALLOW_KNOWN_COMPLIANCE_LIMIT=0` to make that result fail the suite.

## Open-source test corpora

There is no single corpus that covers both codec conformance and browser
surface-lifetime behavior. Use the following projects together:

- [Fluster](https://github.com/fluendo/fluster) is the primary codec
  conformance corpus. It includes H.264/AVC, HEVC, VP9 and AV1 suites with
  reference checksums, and can run an external decoder command. The current
  suites include 204 H.264 vectors (`JVT-AVC_V1` + `JVT-FR-EXT`), 147 HEVC
  vectors, 311 VP9 vectors and 242 AV1 vectors. A focused download is:

  ```
  git clone https://github.com/fluendo/fluster.git
  cd fluster
  ./fluster.py download JVT-AVC_V1 JVT-FR-EXT JCT-VC-HEVC_V1 VP9-TEST-VECTORS AV1-TEST-VECTORS
  ./fluster.py list
  ```

  Start with one vector and the software decoder, then add the VA-API decoder
  wrapper. Compare decoded frame count and pixels, not only process exit status.
- [Chromium media/test/data](https://chromium.googlesource.com/chromium/src/+/lkgr/media/test/data/)
  is useful for container and playback regressions. In particular,
  `front-discard.mp4`, `mid-file-start-time.mp4`, `bear-1280x720.mp4`,
  `avc-bitstream-format-0.h264` and `avc-bitstream-format-1.h264` cover short
  files, non-zero start timestamps, discard-at-start and H.264 framing. The
  directory also contains VP9/HEVC/AV1 samples and documents how each file
  was generated.
- [GStreamer GstValidate](https://gstreamer.freedesktop.org/documentation/gst-devtools/gst-validate.html)
  is a scenario runner rather than a corpus. It is useful for replaying a
  downloaded clip through play, seek, flush and EOS scenarios, for example:

  ```
  gst-validate-1.0 playbin uri=file:///path/to/sample.mp4
  ```

- [Qualcomm v4l-video-test-app](https://github.com/quic/v4l-video-test-app)
  provides a V4L2 decoder test client and JSON-configured codec cases. It is a
  good native baseline for Iris, while [v4l2-compliance](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)
  checks ioctl/queue semantics rather than decoded picture correctness.

For the current Iris investigation, the smallest useful local set is the
generated 1-3 second matrix (all-I, IP-only, B-frame, long-GOP and a GOP-middle
`-c copy` clip) plus Chromium's `mid-file-start-time.mp4`. Record codec/profile,
GOP/B-frame structure, start PTS, decoded frame count, per-frame MD5 or pixel
error, CAPTURE/OUTPUT/`LAST` counts, timestamp misses and bounded sync timeouts.
The positive 640x360 matrix currently passes for IP/all-I/B=2/B=4. The
long-GOP middle-cut sample remains a negative random-access/contract test.

References:

- [Linux `VIDIOC_DECODER_CMD` documentation](https://docs.kernel.org/userspace-api/media/v4l/vidioc-decoder-cmd.html)
- [Linux stateful decoder interface](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
- [Chromium video decoder test usage](https://chromium.googlesource.com/chromium/src/+/b1a274d92dbc807081d60a4dd110294205bdbd87/docs/media/gpu/video_decoder_test_usage.md)
- [Chromium EOS flush tests](https://chromium.googlesource.com/chromium/src/+/c6a258bb5d64045152f89681e84a982d7ae68f46/media/gpu/video_decode_accelerator_tests.cc)
- [ChromiumOS stateful decoder reference client](https://chromium.googlesource.com/chromiumos/platform/drm-tests/+/8db6cd9d8e4a7927f43e499066352354b8297b86/v4l2_stateful_decoder.c)
- [v4l-utils codec ioctl checks](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)
