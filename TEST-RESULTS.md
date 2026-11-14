# Qualcomm Iris VA-API Test Results

**Evidence archive.** This file is a chronological record of runs, including
superseded baselines, refuted hypotheses and interrupted experiments. It is not
a statement of what currently works: read `docs/STATE.md` for the current
qualified combination, and `data/iris-codec-capabilities.json` for the current
capability boundary. Sections here remain the evidence behind any claim.

Primary latest documented target: Fedora 44 ARM64 Snapdragon SM8550 tablet,
kernel `7.2.6-sm8550-g42f3b40-iris1`. Device numbers are historical and must be resolved
through `iris_resolve_device`; target labels below distinguish separate boots
and replacement endpoints without exposing private network addresses. `<scratch>`
and `<lab-root>` stand for the operator's scratch directory and deployment tree;
the records keep their relative layout without publishing the absolute paths.

## Passed

- The driver builds with the tablet's Meson/Ninja toolchain.
- `vainfo` loads the test driver from the project build directory.
- H.264 Main, High and Constrained Baseline profiles enumerate through VA-API.
- Chrome on the real GNOME Wayland session enters `VaapiVideoDecoder` and
  exports the full 25-surface VA pool without `invalid VASurfaceID` errors.
- Official Chrome 151 plays the 30-second H.264 test stream continuously in a
  clean single-browser session with the bounded 100 ms stateful sync wait. A
  25-second sample stayed at `readyState=4`, `paused=false`, and reported zero
  dropped frames while crossing one 30-second loop boundary.
- In a non-looping 30-second run, the hardware page reached
  `currentTime=30`, `ended=true`, `paused=true`, `readyState=4`, with all 720
  video frames accounted for and zero dropped frames.
- A clean-process event probe recorded the final `timeupdate` at 29.789s,
  followed directly by the normal `pause(ended=true)` and `ended` events at
  30s. Reloading an already-ended page can make Chrome seek to a restored
  position (for example 26.464s); that pause/seek/play sequence is browser
  media-session restoration, not a decoder stall.
- The default one-AU batches preserve exact timestamp association and completed
  a 30-second run without queue errors. `V4L2_VA_BATCH_SIZE=2..8` remains
  available for explicit throughput experiments; `1` is the production value.
- The stateful Iris path associates OUTPUT and CAPTURE buffers by timestamp and
  repeats SPS/PPS for every H.264 access unit. CAPTURE timestamps with no live
  OUTPUT entry are deliberately dropped; nearest-timestamp fallback was
  removed because a timed-out frame can otherwise be bound to a future surface
  and cause persistent frame corruption.
- Chrome 151 dual-video playback on the SM8550 tablet uses the stable
  per-surface DMA-BUF path and remained free of the exact `[0,136,0]` green
  frame in repeated 10-30 second canvas samples; both videos stayed on
  `VaapiVideoDecoder`.
- The test page defaults to a single pass. A `?loop=1` stress sample showed
  only the expected `30.0 -> 0.0` browser seek transition, with no anomalous
  green pixels. A software Chrome run selected `FFmpegVideoDecoder`, while
  the hardware run selected `VaapiVideoDecoder` for the same file.
- `chrome://gpu` reports `Video Decode: Hardware accelerated` with the rebuilt
  driver. The two-session context isolation probe passes with distinct V4L2
  session file descriptors.
- After the August 30, 2026 system installation, Google Chrome 151.0.7922.173
  was launched through `/usr/local/bin/iris-vaapi-browser` with no
  `LIBVA_DRIVERS_PATH` override. A 1920x1080 H.264 run reached
  `kVideoDecoderName=VaapiVideoDecoder` and
  `kIsPlatformVideoDecoder=true` in `chrome://media-internals`; the 30-second
  file completed with `readyState=4`, `ended=true`, 0 dropped frames, and 30
  seconds of media duration. The same environment intentionally selected
  `FFmpegVideoDecoder`/`kIsPlatformVideoDecoder=false` for a 320x180 sample,
  confirming Chromium's size/performance threshold rather than an installation
  failure.
- The stateful VP9 path decodes 20 frames from the Iris VP9 sample through
  FFmpeg VA-API with no hardware decode errors.
- The repeatable 320x180/24fps Iris matrix now validates decoded content, not
  only queue completion: H.264 and VP9 each decoded 48/48 frames and their
  per-frame `framemd5` output matched the software reference exactly. The
  capture readback path accounts for Iris' padded stride/height, and context
  teardown is serialized with concurrent VA surface downloads.
- The same matrix's native AV1 baseline (`matroskademux ! av1parse !
  v4l2av1dec`) reached GStreamer EOS successfully on `/dev/video17`.
- The matrix records MPEG-2 and VP8 as `SKIP` on this node because Iris does
  not enumerate their OUTPUT formats; this is a hardware capability result,
  not a driver failure.
- The EOS regression trace passes `test/iris-eos-check.sh`: all 720 H.264
  access units produce 720 OUTPUT DQBUF and 720 CAPTURE DQBUF completions,
  followed by one CAPTURE `LAST` marker after `decoder STOP`; CAPTURE sequence
  numbers are contiguous and no capture errors, timeout, or timestamp-miss
  records remain in the strict Chrome run. The pre-drain trace is rejected by
  the same check. Burst-style FFmpeg runs use the explicit
  `--allow-bounded-sync` mode, which still enforces all EOS/queue invariants
  while reporting bounded-wait diagnostics.
- The strict Qualcomm Iris matrix now passes H.264, VP9 and opt-in HEVC on
  five repeated cold starts: each codec produced 48/48 frames whose
  per-frame MD5 matches the software baseline, with 48 OUTPUT, 49 CAPTURE,
  one terminal LAST, no capture errors, and no timeout/timestamp diagnostics.
- A forced `V4L2_VA_SYNC_TIMEOUT_MS=100` HEVC run exercised the trailing
  reorder recovery (one STOP/START pair) and still passed the same 48-frame
  MD5 and strict EOS oracle.
- The browser tail-frame regression was reproduced with a 60 Hz Canvas probe:
  before the follow-up fix, hardware sampling produced hashes that did not
  belong to any complete `requestVideoFrameCallback` frame and occasionally
  skipped `presentedFrames`; software did not. The queue-service path now stages
  CAPTURE data in a per-surface CPU snapshot and `syncSurface()` publishes it
  only while the target is still `Rendering`. Two rebuilt hardware single-pass
  runs and one loop-boundary run had contiguous frame numbers, no foreign pixel
  hashes, and a stable final frame. `test/iris-source-invariants.sh` rejects
  the old direct-copy path (red on the previous source, green on the fix).

## Pending

- Long-running arbitrary sites such as Douyin still need a user-session check;
  network-selected codecs and MSE seek patterns can exercise different paths.
- A new VA context can fail its initial export while another Iris session is
  holding the firmware queue; Chrome then reports `VaapiVideoDecoder` followed
  by an intentional `FFmpegVideoDecoder` fallback. Keep one hardware session
  per decoder node during diagnostics and treat this resource-contention case
  separately from decoded-pixel correctness.
- The stable DMA-BUF path adds one NV12 memcpy per decoded frame. The opt-in
  zero-copy experiment now removes that copy only for the validated one-AU,
  no-B H.264/VP9 contract; multi-slot ownership, B-frame reorder and broad
  dynamic-resolution coverage remain open.
- HEVC is advertised automatically when the node exposes stateful HEVC. The VA
  long format does not carry the original VPS/SPS/PPS, so the driver generates
  parameter sets from VA metadata; this path passes the strict 48-frame
  content/EOS matrix and repeated cold-start runs on both SM8550 tablets.
- AV1 is advertised automatically when the node exposes stateful AV1. The
  translator rebuilds complete temporal units from the VA tile payloads, so
  the OBU requirement recorded here earlier is met by the driver and not by
  the caller. `src/util/options.cc` no longer reads a switch for either codec;
  the qualified AV1 record is the 2026-10-22 section below.

## Deployment note

The latest target deployment rebuilt commit `400a46aed159f8a951bcb0a3d5dc6dd797a960bb`
and installed its driver, launcher and four desktop entries on August 30, 2026.
The system driver is `/usr/lib64/dri/v4l2_drv_video.so` with SHA-256
`f879e5ea417c770ac7042d520625e9cadcec1ffd00b43ca6658cfb811d5763ef`; the
install manifest is `/usr/local/share/iris-vaapi/install-manifest.txt` and
records the previous-driver backup. A system-path `vainfo` smoke test resolved
`/dev/video17` and enumerated H.264 and HEVC VA profiles. Browser playback and
Electron decoder claims still require their separate runtime gates.

## Secondary target

The second SM8550 tablet exposes the Iris decoder as `/dev/video0` (its
`/dev/video17` is a camera node). The full matrix was repeated three times on
`/dev/video0`, with the context-isolation probe before the first run:

- H.264, VP9 and opt-in HEVC each produced 48/48 frames whose per-frame MD5
  matched the software reference exactly.
- Each VA run passed strict EOS: 48 OUTPUT DQBUF, 49 CAPTURE DQBUF, one
  terminal `LAST`, contiguous capture sequence numbers, and zero timeout or
  timestamp-miss diagnostics.
- Native `v4l2h264dec`, `v4l2vp9dec`, `v4l2h265dec` and `v4l2av1dec` all reached
  GStreamer EOS. AV1 remains VA-skipped because the VA caller supplies tile
  payloads rather than the complete OBU temporal unit required by Iris.
- The two-context isolation probe passed with distinct V4L2 session file
  descriptors.

The browser-ending regression was reproduced with Chrome 151 on Wayland using
the secondary-target decoder. Before the fix, Chrome's context reset destroyed 14
stateful `Rendering` surfaces and each synchronous `destroySurfaces()` call
waited for the bounded timeout, producing 14 `Timed out waiting for surface`
records. The driver now removes the stateful batch mapping immediately when
the caller destroys such a target; late CAPTURE buffers are requeued by the
normal queue service. The old trace fails the regression checker with
`teardown_timeouts=14`; the rebuilt driver passes:

```
PASS teardown_timeouts=0 timestamp_misses=0 stateful_surface_teardown_skip=14
```

A clean 10-second single-pass browser probe then reached `ended=true`,
`readyState=4`, 301 presented frames, zero dropped frames, and identical canvas
pixel samples after `ended`. The final frame remained stable for repeated
samples, with no additional decode submissions after EOS.

## Follow-up structure tests (secondary target)

The browser probe was repeated with full RGBA SHA-256 snapshots at 28.5-30.0s.
The final canvas remained stable. The earlier 32-bit canvas hash result that
looked like a repeated old frame was a hash collision; it is not used as
evidence anymore.

Short, generated H.264 samples were then compared against FFmpeg software
`framemd5` output through the VA-API path:

| Sample | Structure | Result |
| --- | --- | --- |
| `ip-g12-1s.mp4` | 24 frames, no B, GOP 12 | 24/24 MD5 match |
| `ip-g48-2s.mp4` | 48 frames, no B, GOP 48 | 48/48 MD5 match |
| `intra-2s.mp4` | 48 all-I frames | 48/48 MD5 match |
| `b8-g120-2s.mp4` | 48 frames, B-frame stream | 48/48 MD5 match |
| `b4-g48-2s.mp4` | 48 frames, B-frame stream | 48/48 MD5 match after a 90s allowance |

The B=4 sample was previously reported as 12-14 frames only because the
15-25s harness timeout expired while the VA caller was synchronously waiting
for the reorder queue. A longer run completed normally and matched software;
that short-timeout result must not be called a decode corruption.

Two GOP-middle `-c copy` clips are different tests. `copy-mid-nots.mp4` starts
with P pictures before the next IDR and has an incomplete reference chain. It
produced 145/145 frames but one timestamp miss and one pixel mismatch. This is
consistent with the Linux stateful decoder contract: a client must seek to a
random-access point with the required SPS/PPS/reference state, and output from
an arbitrary P-frame cut is not a valid picture oracle. By contrast, Chromium's
official `mid-file-start-time.mp4` has a valid stream with a non-zero start PTS
and passed 250/250 VA frames with an exact software MD5 match; non-zero PTS by
itself is not the trigger.

These results narrow the remaining browser report to EOS/compositor timing or
an input-specific random-access pattern. They do not justify another driver
patch yet. Keep the long-GOP middle-cut sample as a negative/contract test, but
use complete files or keyframe-aligned clips for decoded-pixel pass criteria.

The reusable `test/iris-structure-matrix.sh` was run on the same `/dev/video0`
with 640x360/48-frame inputs. Its all-I, IP-only, B=2 and B=4 cases all passed
48/48 frames with exact software MD5, zero timestamp misses and zero bounded
sync timeouts.

## Latest follow-up (2026-08-30, `test/iris-stateful-followup`)

Target: Fedora ARM64 Snapdragon SM8550 tablet, Iris decoder resolved at runtime;
driver built in an external lab workspace.

- Stateful EOS drain is opt-in with `V4L2_VA_STATEFUL_EOS_DRAIN=1` (the legacy
  `V4L2_VA_EOS_DRAIN=1` alias remains supported). The default idle threshold is
  400ms, based on the measured 325ms maximum normal batch gap and the 458ms
  stale-tail onset. A 30s H.264 browser run reached `ended=true` with 72/72
  tail frames and zero repeated hashes; the trace had one STOP/LAST/START,
  zero timeouts and zero timestamp misses.
- The drain implementation now waits for trailing OUTPUT DQBUF after the
  terminal CAPTURE LAST marker before issuing START, as required by the Linux
  stateful V4L2 contract. The post-change `matrix-general-followup` repeated
  the H.264, VP9 and HEVC checks with no regressions.
- A 115s `loop=true` Chrome probe crossed three 30s loop seeks with 2741 frame
  callbacks, zero gaps over 200ms, zero dropped frames and no trace timeout or
  timestamp miss.
- The standard 48-frame VA matrix passed H.264, VP9 and HEVC with exact software
  framemd5 equality. Each had 48 OUTPUT, 49 CAPTURE, one terminal LAST, and no
  capture errors, timeouts or timestamp misses. Native AV1 reached GStreamer
  EOS; VA AV1 remains skipped because the VA tile payload lacks Iris' required
  complete OBU temporal unit.
- The H.264 structure matrix (all-I, IP/GOP12, B=2, B=4) passed 48/48 with exact
  software MD5. HEVC with `V4L2_VA_SYNC_TIMEOUT_MS=100` also passed 48/48 and
  strict EOS with one recovery STOP/START pair.
- The EOS watchdog remains opt-in because VA-API has no portable end-of-input
  callback. Its startup guard now requires eight submitted AUs and one
  completed frame, so normal HEVC startup gaps are not treated as EOS.
- The generic watchdog now requires eight submitted AUs and one completed
  frame, and both timeout recovery and idle drain refuse to issue START until
  CAPTURE `V4L2_BUF_FLAG_LAST` is observed. Default and watchdog-enabled
  H.264/VP9/HEVC matrices on `/dev/video0` passed 48/48 with exact software
  MD5; the forced 100ms timeout HEVC matrix also passed strict EOS and content
  checks. MPEG-2/VP8 remain correctly reported as unavailable on this node,
  while native AV1 reaches EOS and VA AV1 remains skipped for its tile/OBU
  payload contract.

The state-machine changes follow the Linux stateful decoder contract and the
queue handling used by reference clients:

- Linux V4L2 stateful decoder drain: <https://www.kernel.org/doc/html/v6.4/userspace-api/media/v4l/dev-decoder.html>
- Chromium stateful V4L2 client: <https://chromium.googlesource.com/chromium/src/+/a576d2ae67f9a89b4ac118460c7e4e4763f8c22a/media/gpu/v4l2/v4l2_stateful_video_decoder.cc>
- GStreamer V4L2 decoder: <https://github.com/GStreamer/gst-plugins-good/blob/master/sys/v4l2/gstv4l2videodec.c>

This follow-up is recorded on branch `test/iris-stateful-followup` in commits
`95de0d4` (state-machine safety), `e753088` (watchdog/source guards),
`0288082` and `c2f1211` (generic client documentation), and `cafa6b9`
(generic trace-checker arguments).

## Dedicated hardware suite (2026-08-30, `/dev/video0`)

The complete hardware plan from the earlier investigation was run on
the Fedora 44 ARM64 SM8550 tablet. The repeatable entry
point is `test/iris-hardware-suite.sh`; its logs and generated media are kept
under a local scratch directory and are not repository inputs.

Qualcomm's upstream `v4l-video-test-app` (`b1f1a04`) was built natively with
shared Fedora FFmpeg/JsonCpp libraries (the upstream CMake file requests static
libraries that are not shipped by Fedora). Its JSON decoder client passed all
four advertised codecs:

- H.264: 32/32 decoded frames, 44,236,800-byte NV12 dump;
- VP9, HEVC and AV1: 48/48 frames each, 4,147,200-byte NV12 dumps.

Every dump matched an FFmpeg software `nv12` decode byte-for-byte. A second
Qualcomm run with two concurrent H.264 contexts passed both clients and both
outputs matched the software reference. A mixed four-context run (H.264,
VP9, HEVC, AV1) also returned four `SUCCESS` results and all four dumps matched
their software references.

The V4L2 contract checks reported 47/48 tests succeeded. The sole reported
failure is the `v4l2-compliance` stateful decoder control-class check, while
the same device exposes `min_number_of_capture_buffers` (`0x00980927`) through
`v4l2-ctl`; the verbose report and raw control inventory are retained for
follow-up with v4l-utils. No ioctl, format, queue, buffer or decoder-command
failure was reported.

Native GStreamer pipelines (`v4l2h264dec`, `v4l2vp9dec`, `v4l2h265dec`,
`v4l2av1dec`) all reached EOS. GstValidate reached EOS as well, but on this
GStreamer 1.28 build it reports a `gst_structure_remove_field: IS_MUTABLE`
critical for VP9/HEVC/AV1 and an EOS-seqnum diagnostic for H.264; the plain
`gst-launch-1.0` pipelines are clean, so these are recorded as GStreamer
validator diagnostics rather than Iris pixel failures.

Fluster corpus results were completed with the GStreamer V4L2 decoders. The
JSON reports are retained in the tablet scratch directory
`$IRIS_LAB_ROOT/iris-hw-suite-20260830`:

| Suite / decoder | Total | Passed | Error/Fail/Timeout | Interpretation |
| --- | ---: | ---: | ---: | --- |
| JVT-AVC_V1 / H.264 | 135 | 40 | 95 errors | Main-profile vectors pass; failures are concentrated in field/MBAFF/PAFF, extended and other conformance profiles rejected by this node. |
| JVT-FR-EXT / H.264 | 69 | 0 | 69 errors | Fidelity Range Extension/extended interlaced capability is not exposed by the firmware. |
| JCT-VC-HEVC_V1 / HEVC | 147 | 112 | 34 errors, 1 fail | Main/Main10 and ordinary coding vectors pass where advertised; advanced WPP/tiles/10-bit vectors are rejected. |
| VP9-TEST-VECTORS / VP9 | 305 | 235 | 68 errors, 1 fail, 1 timeout | Failures are 8x8 through 66x66 extreme dimensions, 4:2:2/4:4:4, dynamic resize and SVC cases. The timeout is the 16x16 all-intra stress vector. |
| VP9-TEST-VECTORS-HIGH / VP9 | 6 | 0 | 6 errors | 10/12-bit 4:2:0/4:2:2/4:4:4 is outside the advertised 8-bit 4:2:0 capability. |
| AV1-TEST-VECTORS / AV1 | 242 | 200 | 42 errors | Non-passes are limited to tiny dimensions, monochrome, dynamic resize and SVC-layer vectors. |
| CHROMIUM-8bit-AV1-TEST-VECTORS / AV1 | 13 | 13 | 0 | Chromium's 8-bit AV1 coverage passes in full. |

Two corpus downloads reported checksum mismatches (`brcm_freh5` in
`JVT-FR-EXT` and `SLPPLP_A_VIDYO_2` in `JCT-VC-HEVC_V1`). Fluster keeps those
metadata entries as errors; they are not treated as decoded hardware failures.

The one HEVC checksum failure, `RAP_A_docomo_6`, was rerun three times. The
software reference emits 86 416x240 I420 frames (`8a536a80...`); Iris emits a
stable 85-frame sequence (`c074ee17...`) beginning at the software second
frame. The three hardware byte streams were identical. This is a deterministic
RAP/RASL output capability difference in the native stateful path, not a
non-repeatable pixel corruption; it is kept as a negative capability result
until a client/firmware contract for leading RASL pictures is established.

The VP9 timeout is likewise an isolated 16x16 all-intra stress case. The
remaining 235 vectors complete in 64.5 seconds, and no timeout appears in the
standard VA matrix or Qualcomm mixed-codec client. These external corpus
results therefore do not justify another production-driver change.

This run adds the generic orchestration script and documents the distinction
between codec correctness, V4L2 contract checks, and firmware capability
boundaries. It does not change the production driver based on external-tool
diagnostics.

## Follow-up investigation (2026-08-30, branch `codex/claude-followup`, secondary target)

The follow-up route was continued on a durable secondary lab. Local
`bash test/run-static-checks.sh` and `ninja -C build-local` passed. The remote
deployment performed the content sync, clean rebuild and `ldd -r` gate with no
undefined symbols; the resulting driver SHA-256 was
`f8a8e684f6ddb072bc19b31f9bc3c33e3145b2bcc85ecede45ff560919a64ee3`.

The positive 48-frame VA matrix was then repeated from the deployed checkout:
H.264, VP9 and HEVC each produced 48/48 frames with exact software framemd5,
strict EOS (`48 OUTPUT`, `49 CAPTURE`, one terminal `LAST`) and zero timeout or
timestamp-miss diagnostics. Native H.264/VP9/HEVC/AV1 GStreamer baselines all
reached EOS; AV1 VA remains intentionally disabled by its tile-payload versus
OBU contract.

After commit `6074609` reset the barren cold-start budget on every STOP/START,
the driver was deployed again (`68cc99da41d81f162944fec487d9371f15b1beefa7c05020a70a96af936cdb83`)
and the same H.264/VP9/HEVC 48-frame matrix passed unchanged. With
`LIBVA_V4L2_VIDEO_PATH` unset, `vainfo` also auto-discovered `/dev/video0` by
the `iris_driver` name and enumerated the expected H.264, HEVC and VP9
profiles.

The driver-path Fluster probes used `FFmpeg-*-VAAPI`, unlike the firmware-only
`GStreamer-*-V4L2` table above. Results were deliberately kept as diagnostics:

| Probe | Result |
| --- | --- |
| H.264 `BA1_FT_C`, `BA1_Sony_D` | `1/2` passed; `BA1_Sony_D` remains a reproducible content mismatch. |
| HEVC `DBLK_G_VIXS_2`, `RAP_B_Bossen_2`, `SAO_G_Canon_3` | `0/3` passed; all returned content mismatches. |
| VP9 two basic vectors plus one resize vector | `2/3` passed; resize returned an accelerator error. |
| AV1 Chromium 8-bit, three vectors, opt-in translator | `0/3` passed; the experimental VA path returned decoder errors. |

The stock Fluster FFmpeg wrapper uses `-hwaccel vaapi` followed by a software
format filter and does not request an explicit VAAPI output pool. It is useful
for exposing timeout and surface-lifetime boundaries, but `iris-matrix.sh` is
the authoritative VA pixel oracle. A full 135-vector H.264 driver-path run was
started with a 5-second per-vector bound and safely stopped after repeated
cases still took roughly 30 seconds to terminate; its partial output is not
claimed as a suite result.

Two bounded stress checks explain the remaining H.264 signal. For
`BA1_Sony_D`, one-, two- and three-frame keyframe-aligned streams were
bit-exact, while the complete 17-frame stream produced 17 frames with
non-deterministic duplicate/out-of-order hashes. Increasing FFmpeg's surface
pool and toggling `V4L2_VA_COPY_SURFACES` did not remove it. `AUD_MW_E` returned
100 frames in 9 seconds with 63 bounded sync timeouts and 10 startup
backpressure records, but its frame hashes still differed from software. These
are open surface/firmware interaction diagnostics, not grounds for another
unverified production patch.

## Dynamic-resolution follow-up (2026-08-30, branch `codex/claude-followup`, primary target)

The next investigation item was completed against the current Iris node
`/dev/video4` on the SM8550 tablet. `VIDIOC_ENUM_FRAMESIZES` reports the NV12
capture range `96x96 - 8192x8192`; the VA runtime probe returned the same
`MinWidth=96`, `MaxWidth=8192`, `MinHeight=96`, and `MaxHeight=8192` values.
The old hard-coded `32/2048` surface attributes are gone. The range is scoped
to the profile's eligible decoder devices and uses their common intersection.

The deployed driver passed the 48-frame VA matrix for H.264, VP9, and HEVC:
each codec matched the software framemd5 exactly, with strict EOS accounting
(`48 OUTPUT`, `49 CAPTURE`, one `LAST`, zero timeout or timestamp-miss
diagnostics). The two-context isolation probe also passed.

The Fluster VP9 dynamic vector
`vp90-2-14-resize-fp-tiles-1-8.webm` completed all 18 frames through VA. The
driver and native `v4l2vp9dec` outputs were byte-identical for every 432x240 and
3840x2160 segment. One 3840x2160 segment from both hardware paths differs from
the software decoder at the same byte offset, so that isolated difference is
recorded as deterministic firmware behavior rather than a VA-driver mismatch.
The backend now also drains and rebuilds stateful V4L2 queues when a VA client
reuses one context for a surface with a new geometry.

## Zero-copy experiment (2026-08-30, branch `codex/claude-followup`, primary target)

The same route was extended with an opt-in `V4L2_VA_ZERO_COPY=1`
path. Stateful single-plane NV12 CAPTURE buffers can now import the per-surface
DMA-BUF directly; the V4L2 `Buffer` owns a duplicated fd and no longer maps a
kernel MMAP slot. Surface reuse requeues the imported buffer, while setup or
plane validation failures fall back to the existing MMAP plus stable-copy path.
The default remains unchanged and Chrome is not modified.

The Iris firmware assigns a decoded frame to any queued CAPTURE slot rather
than honoring a VA-surface index. The experiment therefore keeps one imported
slot in flight for H.264/VP9 and uses an extra scratch slot for STOP/LAST drain;
otherwise a frame can land in the wrong exported fd. HEVC/AV1 are rejected from
this experimental contract and automatically use the stable-copy path because
their reorder depth needs multiple simultaneously queued CAPTURE slots.

| Run | Result |
| --- | --- |
| Primary target, zero-copy H.264 matrix, 48 frames | `48/48`, exact software framemd5, `49 CAPTURE`, one `LAST`, zero timeout/timestamp misses. Trace confirms no `copy_surface_frame` for decoded CAPTURE. |
| Primary target, zero-copy VP9 matrix, 48 frames | `48/48`, exact software framemd5, strict EOS and zero timeout/timestamp misses. |
| Primary target, zero-copy HEVC matrix, 48 frames | Stable-copy fallback (`codec_reorder_contract`); `48/48` exact software framemd5 and strict EOS. |
| Primary target, zero-copy H.264 structure, 24 frames | all-I and IP/GOP12 pass; B=2/B=4 fail because the one-slot contract cannot satisfy Iris reorder depth. This is retained as a known opt-in limit, not a default-path regression. |
| Primary target, default MMAP structure, 24 frames | all-I, IP/GOP12, B=2 and B=4 all pass with exact software framemd5. |

The remote clean rebuild and `ldd -r` gate passed for every iteration. The
experiment removes the CAPTURE-to-stable-buffer memcpy only for the validated
no-B H.264/VP9 path; it is not yet a universal replacement for native
multi-slot zero-copy behavior.

After the first dynamic VP9 probe, the tablet powered off before the bounded
command returned and the SSH endpoint changed. A follow-up guard now detects a
dynamic-resolution request while DMA-BUF capture is active, releases the
experimental queue without a long drain, and rebuilds the context on MMAP plus
stable copy. This guard passed local static checks and remote compilation on
the earlier primary/secondary targets; hardware confirmation on a replacement
endpoint is still
pending because that address was not reachable during this run.

## Post-reset diagnosis and stable-path regression (2026-08-30, replacement target)

The replacement target was inspected read-only before any
decode run. The previous boot ended abruptly at `17:24:56` and the next boot
started at `17:25:41`; there is no `reboot.target`, `shutdown`, `OOM`, panic,
Oops, or watchdog record in the previous boot. The new boot also reported that
the old system and user journals were corrupted or uncleanly shut down. The
Qualcomm command line carried `bootinfo.pureason=0x80001` and
`bootinfo.pdreason=0x2`: the public Xiaomi bootinfo layout maps this to the
`HWRST` power-up bit plus the `OTHER` reset-reason bit, not a normal software
reboot, kernel panic, or watchdog code. `/sys/fs/pstore` was empty and no
`/proc/last_kmsg` was available, so the lower-level trigger remains unknown.

Immediately before the reset, `uperf-linux` recorded heavy load and a maximum
temperature of `92.7 C`, then returned to `51 C`; this makes a thermal/power
transient plausible but does not prove it. After reboot the battery reported
88% and `Discharging`, so an empty battery is not supported by the snapshot.

The remote `.so` left by the interrupted run was zero-filled (`invalid ELF
header`), which explains why the first post-reset VA probe stopped before
decoding. A single-thread clean rebuild produced a valid AArch64 shared object
and passed `ldd -r` with no undefined symbols. With `V4L2_VA_ZERO_COPY` and all
optional drain/trace switches explicitly unset, the stable MMAP/copy path then
passed:

| Check | Result |
| --- | --- |
| H.264 VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| VP9 VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| HEVC VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| Native AV1 V4L2 | GStreamer EOS |
| VA context isolation | two contexts created successfully |
| `v4l2-compliance` | 47/48; only the known stateful `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` control classification failed |

The device remained on the same boot throughout this regression. The opt-in
DMA-BUF zero-copy path was not run after the reset diagnosis.

## Zero-copy follow-up (2026-08-30, replacement target)

After the clean rebuild, the opt-in DMA-BUF path was exercised on the same
device with the default EOS and watchdog switches unset. Static no-B H.264 and
VP9 both passed 12-frame and 48-frame runs with exact software framemd5. The
48-frame traces each contain 48 OUTPUT, 49 CAPTURE (including one terminal
`LAST`), zero timeout/timestamp diagnostics, and zero `copy_surface_frame`
records; the trace shows the expected DMA-BUF import and single-slot CAPTURE
queue. HEVC with the switch enabled passed 12/12 as well, but correctly logged
`codec_reorder_contract` and used the stable copy path (`copy_surface_frame`
records present).

Dynamic-resolution probes were kept separate from the positive static oracle:

- `320x180 <-> 160x90` was rejected at the second geometry because `160x90`
  is below the node's declared minimum height of 96; only 10/30 frames were
  valid, so this is a capability-boundary result.
- The `432x240 <-> 848x480` VP9 sample produced the same CAPTURE errors and
  sequence failures with both zero-copy and MMAP VA paths. It is not evidence
  of a zero-copy-only regression and remains outside the positive oracle.
- A smaller `352x288 <-> 282x173` resize sample likewise failed in the MMAP
  VA path after its context handoff, with native V4L2 still reaching EOS.

These FFmpeg resize flows create a new VA context for each geometry, so the
context-local `dynamic_resolution` zero-copy fallback is not reached before
the firmware/input failure. Keep zero-copy disabled for dynamic-resolution
clients until a caller that reuses one VA context across the change is tested.
No Chrome or driver source change was made in this follow-up.

## Zero-copy batch-contract guard (2026-08-30, replacement target)

An opt-in probe with `V4L2_VA_ZERO_COPY=1` and `V4L2_VA_BATCH_SIZE=8` showed
that Iris can hold the aggregated OUTPUT until STOP when only one imported
CAPTURE slot is queued; the first `vaSyncSurface()` then failed with an empty
framemd5 output. This is the expected limit of the current one-slot ownership
model, not a default-path regression. The backend now requires the explicit
one-AU batch contract for DMA-BUF capture and logs
`stateful zero-copy fallback reason=batch_contract`, leaving the stable
MMAP/copy path active for larger throughput batches.

Repeating the same `BATCH_SIZE=8` command after the guard logged the expected
fallback, but the stable-copy batch run still failed before its first frame on
this Iris firmware/input combination. Larger batching therefore remains an
unsupported throughput experiment here; the guard's purpose is to prevent it
from being mistaken for a working zero-copy mode.

After deploying the guard, the explicit one-AU run (`V4L2_VA_ZERO_COPY=1`,
`V4L2_VA_BATCH_SIZE=1`) passed the complete 48-frame matrix on the replacement
target:
H.264, VP9, and HEVC each produced exact software framemd5 with strict
`49 CAPTURE / 48 OUTPUT / 1 LAST` accounting and zero timeout/timestamp-miss
diagnostics. H.264 and VP9 had zero `copy_surface_frame` records; HEVC had 48
stable-copy records plus the expected `codec_reorder_contract` fallback.

## Battery power comparison (2026-08-30, replacement target)

This section measures paced GStreamer/FFmpeg pipelines into fakesink, not a
browser, and ends by requiring an actual Chrome playback run before any
battery-life claim. That run is now recorded below under
[Browser battery comparison](#browser-battery-comparison-2026-09-02-chrome-151).

The comparison used the battery gauge exposed at
`/sys/class/power_supply/qcom-battmgr-bat`: `voltage_now` multiplied by the
absolute value of `current_now`, sampled once per second. Each run used the
same 1280x720 or 1920x1080, 30 fps, 60-second H.264 file with no B frames and
ran a 30-second real-time decode. The native path was GStreamer
`v4l2h264dec` with `capture-io-mode=4` (DMABUF capture). The VA paths used the
deployed driver with either stable copy (`V4L2_VA_ZERO_COPY=0`) or the
one-AU DMA-BUF experiment (`V4L2_VA_ZERO_COPY=1`); software used FFmpeg's
ordinary CPU decoder. Every run collected 10 seconds of pre- and post-run
baseline, 30 seconds of decode samples, temperature, load, and capacity.

The primary value below is the battery-side decode increment: run power minus
the midpoint of the pre/post baselines. It is not decoder-package power; the
screen and the rest of the tablet remain included. Values are mean +/- a
two-sided 95% t interval over independent runs.

Important interpretation: this is a paced `-re`/fakesink baseline, not a
browser playback result. It measures the work needed to sustain 30 fps while
allowing the decoder and compositor to sleep between frames, so it can hide
the cost of a busy software pipeline. Do not use the small real-time delta or
the runtime table below as a claim that software video playback costs only a
few tens of milliwatts.

| Workload | Native DMABUF | VA stable copy | VA zero-copy | Software |
| --- | ---: | ---: | ---: | ---: |
| 1280x720, n=5 | +42 +/- 16 mW | +50 +/- 21 mW | +64 +/- 41 mW | +92 +/- 55 mW |
| 1920x1080, filtered n=4/3/4/3 | +68 +/- 21 mW | +100 +/- 5 mW | +50 +/- 27 mW | +187 +/- 133 mW |

Absolute battery power during the 720p runs was 2.554 W (native), 2.554 W
(VA copy), 2.559 W (VA zero-copy), and 2.615 W (software). The 1080p filtered
means were 2.595 W, 2.601 W, 2.556 W, and 2.699 W respectively. Temperature
stayed between 30.5 and 31.0 C; capacity moved from 83% to 79% over the whole
experiment. All 20 runs at 720p and all 16 runs at 1080p returned zero;
the native and zero-copy traces were separately checked for the expected
capture mode and no VA decode errors.

Two 1080p samples were excluded before aggregation because their pre/post
baseline gap was about 7.4 W, a power-management transition unrelated to the
decoder. The raw CSV files and logs remain in
`$IRIS_LAB_ROOT/artifacts/power-compare-20260830/` on the tablet (and
the copied analysis inputs are under a scratch directory).

This measurement does not demonstrate a statistically significant battery
advantage for the experimental zero-copy path over the native or VA-copy
paths at 720p; the memcpy removed by zero-copy is small compared with gauge
noise and whole-device background power. At 1080p the zero-copy result trends
lower than VA copy, but the filtered sample is still too small for a firm
claim. Software is consistently the highest path, while all hardware paths
remain in the same roughly 2.55-2.60 W whole-device band.

### CPU and unrestricted-load cross-check

FFmpeg's verbose stream mapping for the software command was
`h264 (native) -> wrapped_avframe (native)`, with no hardware input options.
During an additional real-time 720p run, the decoder process consumed about
1.7% of one CPU core for native V4L2, 4.4% for VA stable copy, 3.3% for VA
zero-copy, and 21.1% for the software decoder. This confirms that the small
real-time battery delta is not a software-decoder bypass.

To expose the cost hidden by real-time pacing, each path was also run in a
20-second unrestricted loop at 720p. Absolute battery power during that load
was 2.98 W (native V4L2), 3.04 W (VA zero-copy), and 7.23 W (software). The
loop was deliberately stopped by the harness (the FFmpeg paths returned their
expected signal-stop code), so these figures are a stress direction check,
not a playback estimate. They show that CPU software decoding can add several
watts when the content, frame rate, or browser pipeline keeps the decoder busy.

An independent software-only recheck on the same tablet used the same input
and sampling method. It measured 6.764 W during the 20-second unrestricted
decode, with 3.251 W and 3.639 W pre/post baselines; against their 3.445 W
midpoint, the software decode increment was about +3.32 W. The different
absolute baseline shows why one short run cannot provide a fixed watt number,
but the multi-watt software increment reproduces the direction and order of
magnitude of the original stress result.

### Runtime translation

At the end of the run the tablet reported `charge_full=8884000` uAh,
`state_of_health=94`, and 78% capacity. Using a conservative 3.85 V average
battery voltage gives about 25.0 Wh from the observed 78% down to a 5% reserve,
or 32.5 Wh from 100% down to that reserve. Dividing those energies by the
measured whole-device decode power gives the following fakesink baseline:

| Mode | 78% -> 5% | 100% -> 5% |
| --- | ---: | ---: |
| Native DMABUF, 720p | 9.8 h | 12.7 h |
| VA copy, 720p | 9.8 h | 12.7 h |
| VA zero-copy, 720p | 9.8 h | 12.7 h |
| Software, 720p | 9.5 h | 12.4 h |

These are not browser numbers: they include the measured screen/background
load but not Chrome composition, page activity, or network traffic. They are a
controlled low-load baseline, not a promise that software playback only costs
50-100 mW. The unrestricted-load cross-check demonstrates that a busy software
decoder can consume several additional watts. For a real browser estimate,
the decisive variables are codec, resolution, frame complexity, refresh rate,
panel brightness, and Wi-Fi; an actual Chrome playback run is required before
turning this table into a battery-life guarantee.

## Browser battery comparison (2026-09-02, Chrome 151)

The run the section above asks for. Google Chrome 151.0.7922.173 played one
1920x1080, 30 fps, High profile, level 4.0 H.264 file with no B frames, looped
from `file://` in kiosk mode on the tablet's own panel, with hardware VA-API
decode measured against Chrome's software decoder.

`test/iris-power-compare.sh` produced the collection and
`test/remote/power-analyze.py` aggregated it (both were removed from the tree
in the Phase 6 trim, commit `41ef499`; the numbers below stand as recorded
and cannot be reproduced from the current tree). Five interleaved ABBA blocks, ten
runs; each run is a 300-second measurement window after 20 seconds of warmup,
bracketed by 60-second idle baselines with a 30-second gap between runs. The
arms differ by one switch: the software arm adds
`--disable-accelerated-video-decode`. Everything else, including the launcher,
the LIBVA environment, the profile layout and the page, is identical.

### What the runs show

| Arm | Playback | Baseline | Increment | CPU | Decoder |
| --- | ---: | ---: | ---: | ---: | --- |
| Hardware, n=5 | 2.745 +/- 0.040 W | 2.458 W | +0.287 +/- 0.058 W | 0.205 cores | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true` |
| Software, n=5 | 2.844 +/- 0.013 W | 2.443 W | +0.401 +/- 0.039 W | 0.490 cores | `FFmpegVideoDecoder`, `kIsPlatformVideoDecoder=false` |

The statistic is the per-block paired difference, not two pooled means:

```text
software - hardware, playback power:  +0.0988 +/- 0.0448 W   (n=5 blocks)
software - hardware, increment:       +0.1145 +/- 0.0861 W
```

Both intervals are two-sided 95% t intervals and both exclude zero. Hardware
decode costs about 99 mW less whole-device power, roughly 3.5% of the 2.8 W the
tablet draws while playing, and it halves the browser's CPU.

All ten runs were valid and none was dropped. The two arms' idle baselines
landed 15 mW apart (2.458 W against 2.443 W), which is the control that makes
the 99 mW separation readable at all.

### Provenance per run

Playback alone proves nothing here, because both arms play the video. Each run
required the Media domain to name the decoder and the frame counters to advance
inside the window. Independently of Chrome's self-report, every hardware run
had exactly one process holding the resolved Iris node and it was the GPU
process; every software run had none.

Frame accounting was 8991-8997 decoded per 300-second window against a 9000
frame ideal, with one dropped frame across all ten runs. The backlight was
pinned at 1024/2047 and recorded per sample: every run reported a single level
for its whole duration. Thermals stayed between 36.9 C and 37.7 C mean, 39.4 C
peak. Capacity fell from 63% to 52% across the collection; no external supply
came online in any sample.

Driver `f879e5ea417c770a`, source commit `0ced73ee3f3b`, clean tree, kernel
`7.2.2-sm8550-gad75da3`, decoder node `/dev/video0`. The artifacts are under
`$IRIS_LAB_ROOT/artifacts/power-browser-20260902b/` on the tablet, with
`report.json` holding the full per-run record.

### Runtime translation

`charge_full` reads 8931000 uAh. At a conservative 3.85 V average that is about
34.4 Wh, or 32.7 Wh from full down to a 5% reserve.

| Mode | Playback time from 100% to 5% |
| --- | ---: |
| Hardware VA-API | 11.9 h |
| Software | 11.5 h |

About 25 extra minutes of 1080p30 playback per charge. That is the honest size
of this effect for this clip, at this brightness, with no network traffic and a
page that does nothing but hold a video element.

### What this does not say

The earlier fakesink section measured several watts of difference under an
unrestricted decode loop. Both results are real and they answer different
questions: a browser playing 30 fps is a paced workload that lets the CPU idle
between frames, so the software decoder never runs flat out. Content at a
higher resolution or frame rate, a heavier page, or a device with a dimmer
panel would all widen the gap; this run does not measure any of them.

The comparison is also whole-device. The panel at 1024/2047 accounts for most
of the 2.4 W baseline, which is why a 99 mW decode difference is 3.5% of the
total rather than 3.5% of the decoder.

Only H.264 was measured. HEVC is a qualified VA path but was not part of this
collection, and VA VP9 remains withdrawn on this target.

Only Google Chrome was measured, and that is not an arbitrary choice. Fedora's
Chromium 151.0.7922.169, launched through the same `iris-vaapi-browser` with
the same environment, reports `V4L2VideoDecoder` with
`kIsPlatformVideoDecoder=true` and its GPU process holds the Iris node
directly. It decodes in hardware through Chromium's own native V4L2 stack and
never loads this driver, so a Chromium arm would measure that stack rather than
this project. A Chromium hardware-versus-software comparison is still worth
running; it just answers a different question and needs its own section.

### First collection, discarded

An earlier collection the same night lost all ten runs. GNOME's `idle-dim` was
not among the locked settings, so the panel dimmed partway through the first
block, and `brightness_start` was captured once before the first block rather
than per run, which then reported every later run as broken against a value it
never had. The runs were internally consistent, with hardware increments in
0.349-0.403 W and software in 0.468-0.658 W, but by the harness's own rules
they were not comparable and they are not used here.

`idle-dim` is now locked, and the backlight is a sampled column rather than two
edge readings, so a panel that dims and recovers inside a run cannot pass.
`test/iris-power-report-check.sh` holds the analyser to that rule.

## Zero-copy/package follow-up (2026-09-03)

The package provenance issue found after the previous handoff is fixed. The
Fedora spec now runs the complete ELF post-processing chain before refreshing
the staged install manifest, and the Arch PKGBUILD disables a later implicit
strip, strips explicitly, and refreshes the same fields. A real Fedora
`rpmbuild` from the phase-4 source completed with all 17 repository tests
passing; extracting the resulting RPM showed that the manifest hash and size
matched the final stripped `/usr/lib64/dri/v4l2_drv_video.so`. The installed
tablet package was then replaced from that RPM and `rpm -V` passed.

The bounded zero-copy contract was exercised on the installed package under a
boot/temperature guard. The target was Fedora 44 ARM64 on SM8550, kernel
`7.2.2-sm8550-gad75da3`, boot ID
`ee2f8dbc-95e9-4dbb-81fc-6f288ba64c99`, with `/dev/video0` resolved by the
`iris_driver` name as `Iris Decoder` (the sibling `/dev/video1` is `Iris
The first installed artifact for this follow-up was source commit
`c7b251dcbb452d6dda9450f39cc2b8e94adbb5ee`. A final Fedora RPM was then
rebuilt from source commit `42190a4628f97a2dd10f259de35fe6bdda125b5e` and
installed in its place; its final stripped driver SHA-256 is
`3816b5714ce4053aa4398daf9e93802f17d97c2884a5743e6cbc437310267252` and its
size is `301632` bytes. The RPM payload manifest matches those bytes and
`rpm -V` passes.

The short qualification results were:

| Check | Result |
| --- | --- |
| Default installed VA H.264/HEVC | 12/12 exact framemd5 each; strict `13 CAPTURE / 12 OUTPUT / 1 LAST`; zero timeout/miss |
| Direct zero-copy H.264 all-I and IP/GOP12 | 24/24 exact framemd5 each; zero `copy_surface_frame`; strict EOS |
| H.264 B=2 without ownership contract | 24/24 exact framemd5 through stable-copy fallback |
| HEVC with zero-copy requested | 24/24 exact framemd5 through `codec_reorder_contract` stable-copy fallback |
| Two concurrent zero-copy H.264 contexts | 30-second shortened soak; 720 frames per context, zero timeout/miss, strict EOS, boot unchanged |
| Qualcomm `v4l-video-test-app` | 90-frame H.264 NV12 output byte-identical to its reference |
| `v4l2-compliance` | 47/48; the one known stateful `Min Number of Capture Buffers` classification failure |
| Chrome surface acceptance | 1920x1080 real surface, frames advanced with zero drops in the zero-copy launch; decoder evidence was independently `VaapiVideoDecoder`, platform=true |
| Chromium surface acceptance | `V4L2VideoDecoder`, platform=true, 151 frames, zero drops; native V4L2 path, not this VA driver |

The new power harness supports Chrome stable-copy, Chrome zero-copy, and
Chromium native-V4L2 collections. The phase-4 Chrome zero-copy pilot was
correctly rejected because an external charger came online during the window;
the short Chrome-copy pilot was rejected once for an unstable pre/post baseline.
Those samples are not results. The only statistically qualified browser power
claim remains the earlier five-block Chrome stable-copy comparison above; no
zero-copy-versus-copy or Chromium power claim is made until a complete
discharging ABBA collection is run.

Accordingly, zero-copy remains an explicit `h264-no-b-v1` experiment and is
not enabled by the default launcher or package. The current evidence does not
qualify a multi-slot, B-frame, HEVC, AV1, or dynamic-resolution zero-copy path.

## Zero-copy requalification (2026-09-08, current build)

Target: Fedora 44 ARM64 on SM8550, boot ID
`36da96d7-8a95-4abe-b041-949b7c9f8590` (unchanged through every run below),
Chrome `152.0.7977.82`, decoder resolved by the `iris_driver` name. All
firmware-risky runs went through `scripts/iris-experiment-guard.sh` (or the
soak's own boot/temperature guard); no reset, no thermal breach. The build
under test includes the stream-switch green-flash work (completed-frame
tracking with fail-closed error release; sync timeouts stay patient for
B-frame reorder, proven by the B=2 fallback case below).

| Check | Result |
| --- | --- |
| Zero-copy suite, H.264 all-I and IP/GOP12 direct | 24/24 exact framemd5 each; zero `copy_surface_frame`; strict EOS |
| Zero-copy suite, H.264 B=2 without contract | 24/24 exact framemd5 through stable-copy fallback |
| Zero-copy suite, HEVC with zero-copy requested | 24/24 exact framemd5 through `codec_reorder_contract` fallback |
| Dynamic resolution, default path, 100 switches | native 202/100 exact pixels; VA single-process 202/100 exact md5; 101 fresh contexts exact md5 |
| Chrome DRC stream under zero-copy env, ~1000 switches | `VaapiVideoDecoder`, platform=true, 2134 decoded / 2 dropped in 90 s; zero-copy engaged, zero `copy_surface_frame`, zero errors/timeouts, boot unchanged |
| Concurrent dual-H.264 soak, default path, SHORTENED 300 s | 7200 frames per context, zero timeout/miss, strict EOS |
| Concurrent dual-H.264 soak, zero-copy, SHORTENED 300 s | 7200 frames per context, zero timeout/miss, both traces show `zero-copy enabled` with zero copies |

Same-VA-context dynamic-resolution under zero-copy (the
`dynamic_resolution` fallback arm in `reconfigure_stateful_dimensions`)
remains runtime-unproven: both FFmpeg and Chrome open fresh VA contexts per
geometry, so the fallback never fired (0 occurrences in 90 s of Chrome DRC).
The code path is reviewed and logged, but no client exercised it yet.
Accordingly zero-copy stays an explicit `h264-no-b-v1` opt-in; the default
launcher and package are unchanged.

## Full Fluster driver-path runs (2026-09-08, current build)

Same target and boot ID as the zero-copy requalification above; every run
guarded (boot/temperature), no reset. Driver path is `FFmpeg-*-VAAPI`
through this driver; firmware baseline is `GStreamer-*-V4L2` on the same
firmware and resources, bypassing the driver.

| Suite | Driver path (FFmpeg-VAAPI) | Firmware (GStreamer-V4L2) |
| --- | --- | --- |
| JVT-AVC_V1 H.264 (135) | 7 pass / 8 fail / 120 error | 40 pass (matches 2026-08-30) |
| JCT-VC-HEVC_V1 HEVC (147) | 20 pass / 18 fail / 108 error / 1 timeout | 112 pass (matches 2026-08-30) |

The firmware numbers reproduce 2026-08-30 exactly, including the hard
`gst-launch` rejects on SVC (H.264) and WPP (HEVC) vectors. VA-pass is a
strict subset of firmware-pass: the driver never passes what the firmware
cannot do.

Gap analysis on sampled vectors (native `h264_v4l2m2m` / manual VAAPI
controls): CABA1_Sony_D, CVFC1_Sony_C, CAQP1_Sony_B fail at the firmware
too (pixel mismatch or abort), and CANL1_Sony_E aborts natively. These are
firmware capability boundaries, not driver regressions. Two vectors show the
gap is partly methodology: BA1_Sony_D is VAAPI-exact and stable across 3/3
manual runs (the fluster Fail is the known nondeterministic wrapper
verdict from 2026-08-30), and AMP_A_Samsung_7 (Main 8-bit) is VAAPI-exact
while fluster reports Error. The Fluster FFmpeg wrapper turns software
fallback ("Failed setup for format") into Error, so unadvertised profiles
(Main10 and friends, deliberately outside the production matrix) inflate
the Error column by design.

No evidence was found of the driver aborting decodable content: matrices,
browser runs, soaks and the manual VAAPI controls are all green and stable.
Per the standing classification, `iris-matrix.sh` remains the authoritative
VA pixel oracle; these corpus runs are diagnostics.

## Pre-rebuild baseline (2026-09-20, tag `pre-rebuild-20260920`)

This is the tracked reference every rebuild phase is diffed against. It was
produced by the new `test/iris-browser-acceptance.sh`, which reports the
decoder Chrome itself selected, the platform flag, the frame/drop delta over
the window, the Chrome process type holding the Iris node, and the SHA-256 of
the driver the GPU process mapped. All four runs use the packaged default
environment: no `V4L2_VA_*` switch is set.

Target: Fedora 44 ARM64 on SM8550, kernel `7.2.2-sm8550-gad75da3-pr4+`
(self-built from ianchb/sm8550-mainline, Iris as a module, no decode-order
patch), boot ID `5b9707b3-22ad-4b32-ac5e-da3ff7cb6077` (unchanged through
all runs), Google Chrome `152.0.7977.82`, decoder resolved by the
`iris_driver` name. Source commit `05964d5af21f8a9e60ef6956cd2472fe749a3036`.

Clips are 30 s Sintel trailer re-encodes at 1920x1080, 24 fps, 720 frames,
two B-frames of reordering each:

| Clip | Codec / profile | SHA-256 |
| --- | --- | --- |
| `sintel-1080p30-h264-b3.mp4` | H.264 High | `dc3a29432e05af2462f39ec195de863af7df3f9e232a6be2f840d5f252f5612a` |
| `sintel-1080p30-hevc-b3.mp4` | HEVC Main | `2c5ab2a10b74d41433669d00c4224f7eb3a9e8dc0554bdeb79537d3893dbc7b7` |

| Run | Driver | Decoder | Platform | Frames / dropped (30 s) | Iris node holder |
| --- | --- | --- | --- | --- | --- |
| installed H.264 | package `libva-v4l2-iris-0.1.0-1.fc44`, `a86ecd41b7ad…` | `VaapiVideoDecoder` | true | 670 / 0 | `--type=gpu-process` |
| installed HEVC | same | `VaapiVideoDecoder` | true | 671 / 0 | `--type=gpu-process` |
| HEAD H.264 | clean build of `05964d5`, `dd6e9323eb62…` | `VaapiVideoDecoder` | true | 671 / 0 | `--type=gpu-process` |
| HEAD HEVC | same | `VaapiVideoDecoder` | true | 672 / 0 | `--type=gpu-process` |

The installed package was built from `fc680d6` with a dirty tree; the HEAD
build is the same tree at `05964d5`. Both pass identically, so the
zero-copy refresh in `05964d5` did not change the default path. This is the
first tracked HEVC-in-Chrome frame evidence; earlier HEVC browser results
lived only in an untracked log.

Artifacts: `$IRIS_LAB_ROOT/artifacts/browser-acceptance/baseline-*`
on the target (cdp.json, summary.json, browser.log, iris-node-holders.txt).

## Phase 1: decode-order Iris kernel module (2026-09-20)

The two decoder-side kernel changes from `strongtz/libva-v4l2` (decode-order
output through `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY{,_ENABLE}`, and a
64-buffer CAPTURE `max_num_buffers` for HFI gen2) were applied to the
tablet's kernel source (`ianchb/sm8550-mainline`, branch `sheng-7.2.2-pr4`,
HEAD `ad75da348a02`) with fuzz. One hunk of `iris_platform_common.h`
landed the two new capability IDs in `enum platform_inst_fw_cap_flags`
instead of `enum platform_inst_fw_cap_type`; they were moved by hand to the
end of the type enum. Only the `qcom-iris` module was rebuilt (`make LLVM=1
M=drivers/media/platform/qcom/iris modules`, zero warnings) and installed in
place; the previous module, the full local kernel diff and a `ROLLBACK.sh`
are kept under `$IRIS_LAB_ROOT/kernel-rollback-20260920/`.

| | before | after |
| --- | --- | --- |
| module SHA-256 | `4b3087803c4b…` | `1c1a4ef99a5f…` |
| `display_delay` / `display_delay_enable` controls | absent | present (`max=0`, `default=0`) |
| `min_number_of_capture_buffers` | `max=32` | `max=32` (this control reports the firmware minimum's range; the vb2 pool limit is a separate REQBUFS bound and is verified in Phase 2's unit and hardware tests) |

The module was reloaded without a reboot (boot ID
`5b9707b3-22ad-4b32-ac5e-da3ff7cb6077` unchanged). With the control left at
its default, the unchanged installed driver passes the browser gate exactly as
in the pre-rebuild baseline:

| Run | Decoder | Platform | Frames / dropped (30 s) | Node holder |
| --- | --- | --- | --- | --- |
| patched module, installed driver, H.264 | `VaapiVideoDecoder` | true | 671 / 0 | `--type=gpu-process` |
| patched module, installed driver, HEVC | `VaapiVideoDecoder` | true | 672 / 0 | `--type=gpu-process` |

## Phase 3: rebuilt driver on the new core (2026-09-20)

Source commit range `1f1f2fa..f6642b1` on `main`; the driver is now
`src/va` on `src/iris` with `src/codec` translators (docs/architecture.md).
Target as in Phase 1, patched `qcom-iris` module (`1c1a4ef9…`), decode-order
mode active. The tablet rebooted at 14:12 local by a clean `systemd-reboot`
(previous boot's journal ends in an orderly shutdown; no Iris or firmware
message precedes it); every run below is on boot ID
`4944865b-f3b2-424b-8853-375f29bba7b4` and none changed it.

| Gate | Result |
| --- | --- |
| `meson test` on host | 15/15 including the 19-case FakeDevice session suite |
| `test/iris-matrix.sh` | H.264 48/48 and HEVC 48/48 exact framemd5; `submitted=48 capture=48 drain=1 timeouts=0 drops=0` each |
| `test/iris-structure-matrix.sh` | all-I, IP/GOP12, B2, B4 at 640x360: 48/48 each, zero drops or timeouts |
| `test/iris-zero-copy-suite.sh` (Direct path) | 24/24 frames `publish=direct`, zero copies, framemd5 equals software |
| `test/iris-dynamic-resolution.sh` | native 202/100 switches; VA single-process 202 frames, 101 contexts, pixels equal software |
| `test/iris-concurrency-soak.sh` 120 s | dual H.264 2880+2880 frames; mixed H.264+HEVC 2880+2880 frames; strict EOS on all four; boot ID unchanged |
| Chrome 152 H.264 1080p24 B-frames, `V4L2_VA_TRACE=1` | `VaapiVideoDecoder`, platform=true, 719 frames / 0 dropped; trace: 920 `publish=copy-gpu`, 0 drops, 0 timeouts, 1 drain |
| Chrome 152 HEVC 1080p24 B-frames, traced | `VaapiVideoDecoder`, platform=true, 719 / 0; 919 `publish=copy-gpu`, 0 drops, 0 timeouts, 1 drain |
| `iris-ending-check`, `iris-context-retirement-check` on both Chrome traces | pass |

FFmpeg 1080p H.264, 240 frames, CPU time of the ffmpeg process (bash `time`):

| Path | user | sys |
| --- | --- | --- |
| Direct (no copy) | 1.22 s | 0.43 s |
| Copy, GPU engine | 0.75 s | 0.47 s |
| Copy, CPU engine | 1.17 s | 0.38 s |
| Software decode | 2.80 s | 0.18 s |

The GPU engine's pixels are bit-identical to software for 48 frames. Chrome
pre-exports its pool and therefore runs the GPU-copy path; FFmpeg runs
Direct.

Two things learned on hardware and fixed in this phase: FFmpeg destroys a
resolution segment's VA context before downloading that segment's last
pictures, so a completed Direct frame must survive `vaDestroyContext`
(vb2 orphans exported buffers, the fd keeps the pixels); and the soak's own
libx265 encode leaves the prime core at the 85 C guard, so a scenario now
waits for a 15 C margin before its first sample.

Not yet done in this phase: VP9 and AV1 stay unadvertised (Phase 5);
`data/iris-codec-capabilities.json`, the launcher, packaging and the README
prose still describe the old tree (Phase 6).

## Phase 5: VP9 and AV1 on the new core (2026-09-20)

Same target and boot (`4944865b…`) as Phase 3; every run below is on the
decode-order module. Runs marked *guarded* went through
`scripts/iris-experiment-guard.sh` (boot ID and thermal watch).

### VP9: qualified, now advertised by default

| Gate | Result |
| --- | --- |
| `test/iris-matrix.sh` VP9 (640x360, 48 frames) | 48/48 exact framemd5, `submitted=48 capture=48 drain=1 timeouts=0 drops=0` |
| 100 VA context recreations, guarded | first run 10/100 FFmpeg failures; root cause a race between FFmpeg's filter-thread `vaSyncSurface` and its decoder-thread `vaDestroyContext` (the context left the table before draining). Fixed in `va: drain before removing a context from the table`; rerun 0/100 failures, boot unchanged |
| `test/iris-dynamic-resolution.sh` VP9, guarded | native baseline 202 frames / 100 switches; VA single-process 202 frames, 101 contexts, pixels equal software; VA new-context 100 transitions all 2/2 |
| Chrome 152, 1080p24 VP9 with alt-ref (`lag-in-frames 25`, `auto-alt-ref 1`), 30 s | `VaapiVideoDecoder`, platform=true, 716 frames / 0 dropped; trace 932 `publish=copy-gpu`, 0 drops, 0 timeouts |

The old tree withdrew VP9 because context recreation and source changes
rebooted the tablet. Neither reproduced in 200 recreations plus 100
in-stream resolution switches on the patched module. Whether the cause was
the display-order firmware path or the old driver's queue handling is not
separated here; the combination that ships is the one that was tested.

### AV1: firmware output contract, kept opt-in

The OBU writer now reconstructs global motion parameters (the previous
tree rejected any non-identity `wmtype`). Software oracle
(`test/av1-obu-roundtrip.sh`): 27 rebuilt temporal units, every one
bit-exact against libdav1d's decode of the source. The same rebuilt stream
fed to the firmware natively (`v4l2av1dec`) decodes to 27 frames with no
error; the original stream decodes natively to 48.

The 21-frame difference is the AV1 hidden-frame model: libaom emits
alt-ref frames with `show_frame=0` and later displays them through
`show_existing_frame`, a header-only temporal unit that carries no
payload and never reaches a VA-API hardware accelerator (VA has no call
for it). The firmware returns a CAPTURE buffer only for shown frames, so
through VA a client submits 48 pictures and receives 27 completions;
FFmpeg then times out on the first hidden one. A plain IPPP stream
(`lag-in-frames 0`, `auto-alt-ref 0`) exposes a second gap: the firmware
returned the key frame's CAPTURE only after the next AU was queued
(`va frames=1` at the first download), so even show-every-frame AV1 does
not satisfy VA's synchronous per-picture model on this firmware.

AV1 therefore stays behind `V4L2_VA_EXPERIMENTAL_PROFILES=1`. Closing it
needs either firmware/kernel support for returning hidden pictures (a
`DISPLAY_DELAY`-style control for AV1) or a VA client contract that
tolerates missing completions; neither is a driver change.

### AV1 recheck on the encode branch (2026-10-22)

Re-run against `3b91519` with the same build as the encode section
(`v4l2_drv_video.so` SHA-256
`bb4549260f2c6d7b021889d3222e386cfe19735ea61282d93cf97c9e577daed9`,
FFmpeg 8.1.3, 320x240, 30 frames, `V4L2_VA_SYNC_TIMEOUT_MS=500`). The
failure comes before the hidden-frame gap and is not specific to alt-ref
streams:

| Clip | Result |
| --- | --- |
| libaom default (`auto-alt-ref` on) | `Error processing packet in decoder: Input/output error`, no frames |
| `-lag-in-frames 0 -auto-alt-ref 0` | same error at the first download, no frames |

The trace pins it at picture one: `va end_picture … deferred` is followed
by `va error where=getImage … surface has no decoded pixels`, and the
translator releases that picture only when its successor arrives
(`submit token=1`). The one-picture hold is not removable — the VA AV1
picture buffer carries no `refresh_frame_flags`, and the slot a picture
writes is only visible in the next picture's reference map. Separately,
the one access unit that was submitted produced no CAPTURE before STOP
(`drain complete … output_queued=0`), the firmware deferral recorded
above.

AV1 VA decode therefore fails on the first picture of every stream on
this firmware, alt-ref or not. The conclusion is unchanged: opt-in only.

## Phase 6: trimmed tree, packaged, installed (2026-09-20)

The tree now contains only what the rebuilt driver ships (commit `069a58c`
and the docs pass `25dd21b`). Host gates: `meson test` 13/13, static checks
pass, `ldd -r` clean, no gstreamer/udev linkage.

Fedora RPM built on the target from `git archive` of HEAD through
`packaging/fedora/libva-v4l2-iris.spec` (EGL/GLESv2/GBM build deps, one
desktop entry), installed with `dnf reinstall`, `rpm -V` clean. Installed
driver SHA-256 `fe3acad292835061460f6455d080c368b5319ce10aad6ba0786eb37afe354c50`.
`vainfo` on the installed driver lists H.264 CBP/Main/High, HEVC Main/Main10,
VP9 Profile 0/2 and no AV1.

Acceptance from the installed package, no build-tree override, no
`V4L2_VA_*` switch, boot ID `4944865b…` unchanged:

| Clip (30 s window) | Decoder | Platform | Frames / dropped | Node holder |
| --- | --- | --- | --- | --- |
| H.264 1080p24 B-frames | `VaapiVideoDecoder` | true | 718 / 4 | `--type=gpu-process` |
| HEVC 1080p24 B-frames | `VaapiVideoDecoder` | true | 720 / 0 | `--type=gpu-process` |
| VP9 1080p24 alt-ref | `VaapiVideoDecoder` | true | 718 / 0 | `--type=gpu-process` |

The four dropped H.264 frames were the only non-zero drop count in the
rebuild series; that run shared the tablet with the RPM install's
post-transaction work. A rerun on the idle tablet from the same installed
package: `VaapiVideoDecoder`, platform=true, 720 / 0.

Three stale per-user desktop entries from earlier installs
(`chromium-iris-v4l2`, `chromium-iris-vulkan-webgpu`,
`google-chrome-iris-vulkan-webgpu`) were removed from
`~/.local/share/applications` on the target; the package ships only
`google-chrome-iris-v4l2.desktop`.

## Phase 7: 10-bit, conformance corpus, module provenance (2026-09-21)

Same target and boot ID (`4944865b…`) as Phases 3 to 6; every corpus run
guarded, no reset.

### 10-bit profiles

HEVC Main10 and VP9 Profile 2 decode bit-exact against software (48 frames
each, P010 CAPTURE at 128-pixel stride) once two glue defects were fixed:
the session took its CAPTURE fourcc from the config instead of the first
target surface, and the surface attribute list offered only NV12 to
clients that create a config without an RT-format attribute (FFmpeg). A
1080p24 Main10 B-frame stream plays in Chrome 152 as `VaapiVideoDecoder`,
platform=true, on the GPU copy engine with P010 stable buffers (three
runs: 720/4, 719/3, 719/0 dropped; the 8-bit control in the same session
showed 719/2, so the drops track tablet load, not bit depth).

### Fluster conformance, driver path (`FFmpeg-*-VAAPI`)

Firmware baseline is the 2026-09-08 `GStreamer-*-V4L2` run on the same
resources (the native path, bypassing this driver).

| Suite | Old driver (2026-09-08) | Rebuilt driver, first run | Rebuilt driver, final | Firmware |
| --- | --- | --- | --- | --- |
| JVT-AVC_V1 (135) | 7 pass | 40 pass | 40 pass | 40 pass |
| JCT-VC-HEVC_V1 (147) | 20 pass | 57 pass | 111 pass | 112 pass |

H.264: the VA pass set equals the firmware pass set exactly; the 95 others
fail natively too (SVC, interlaced, FMO/ASO and other tools the firmware
does not implement).

HEVC: the first rebuilt run already tripled the old count. The remaining
gap was closed in three steps found by classifying every failing vector's
trace: (1) the generated SPS carried one repeated RPS with only the
current picture's references, which evicted follow pictures and broke any
stream whose slices index different sets; every slice header is now
rewritten with an explicit RPS built from VA's DPB (used flags, long-term
entries) and VA's RefPicList spelled out as a list modification, the
approach strongtz/libva-v4l2 uses; (2) FFmpeg renders one slice data
buffer per slice while the translator assumed one blob for the picture;
(3) three PPS fields were clamped inside their legal ranges
(`init_qp_minus26` for 10-bit, `diff_cu_qp_delta_depth`,
`log2_parallel_merge_level_minus2`).

Final state: 111/147. The two firmware-passing vectors the driver does
not pass (`NUT_A_ericsson_5`, `RPS_D_ericsson_6`) decode all frames
without a firmware error and match software frame for frame under
`framemd5`; Fluster reports them as failing because FFmpeg's software
reference decoder itself logs "Could not find ref with POC" on both and
emits a different frame set than the conformance MD5. `RAP_A_docomo_6`
passes through the driver (RASL pictures after the opening CRA are refused
by the translator, so the output starts where the reference does) while
the native GStreamer path emitted one frame fewer. `DELTAQP_A_BRCM_4`,
`INITQP_B_Main10_Sony_1` and `PMERGE_E_TI_3` complete every frame with no
firmware error but were recorded here as differing in pixels; the recheck
of 2026-10-22 shows that was a measurement artifact and all three match
byte for byte (see "HEVC conformance recheck" below).

The HEVC parameter-set unit test is registered again (it had been dropped
from `test/meson.build` in Phase 3).

### Provenance

`test/remote/provenance.py` records the installed `qcom-iris` module path
and SHA-256 alongside the kernel release, so a result carries the
decode-order patch identity.

### Final installed package (2026-09-21)

RPM rebuilt from `adba51f` and reinstalled; `rpm -V` clean; installed
driver `579d1bc1fa18af5e…`, manifest source commit matches. Chrome 152
from the installed package, packaged defaults, boot ID unchanged:

| Clip | Decoder | Frames / dropped (30 s) |
| --- | --- | --- |
| H.264 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 708 / 2 |
| HEVC Main 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 714 / 0 |
| VP9 1080p24 alt-ref | `VaapiVideoDecoder`, platform=true | 720 / 0 |
| HEVC Main10 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 720 / 3 |

A full-duration (2 h per scenario) `iris-concurrency-soak.sh` against the
installed driver, started after this table on the same boot (`4944865b…`),
passed: `dual-h264` 172800 + 172800 frames and `mixed` H.264 172800 +
HEVC 172800 frames, `teardown_timeouts=0`, `stateful_drain_complete=1` on
all four contexts, hottest zone 57.6 °C, boot ID unchanged. Log:
`$IRIS_LAB_ROOT/artifacts/final-soak-2h-20260921.log`.

## Kernel 7.2.6 and the re-applied decode-order module (2026-09-22)

On 2026-09-21 the target moved from the self-built `7.2.2-sm8550-gad75da3-pr4+`
kernel to the packaged `kernel-sheng-7.2.6-1`
(`7.2.6-sm8550-g42f3b40c702a`). That package ships the stock Iris module, so
the decode-order patch from Phase 1 was gone: no `display_delay` control,
the driver fell back to display-order mode. Every result above this heading
was produced on the patched 7.2.2 kernel.

The two strongtz patches were rebased onto the 7.2.6 tree
(`packaging/kernel/0001-media-iris-decode-order-output-and-64-capture-buffers.patch`;
one enum hunk had to be placed by hand, the fuzzy apply put
`DISPLAY_DELAY*` into `platform_inst_fw_cap_flags` instead of
`platform_inst_fw_cap_type`) and only the `qcom-iris` module rebuilt. Two
things bit on the way and are recorded so nobody repeats them:

- The kernel is built with `CONFIG_ARM64_BTI_KERNEL`. Kconfig silently
  drops that option under clang 22 (`CLANG_VERSION < 210000` guard), so a
  module built from the same `.config` lacks BTI landing pads and the first
  instruction of `init_module` oopses (`Oops - BTI`). Building with
  `KCFLAGS=-mbranch-protection=pac-ret+bti` fixes it; check with
  `llvm-readelf -n qcom-iris.ko` (`aarch64 feature: BTI, PAC`).
- After the oops the module is stuck in `initstate=coming` and
  `modprobe -r` hangs in `iris_vpu_power_off → disable_irq`; the same hang
  happens on any `modprobe -r` while the VPU is powered. Swap the file and
  reboot instead of reloading. `ROLLBACK.sh` under
  `$IRIS_LAB_ROOT/kernel-rollback-20260921-7.2.6/` does exactly that.

Installed module sha `86e5e49aec0d…` (stock `548170065d83…` kept beside
it). After a clean boot (`0bdad14e…`) `/dev/video7` exposes
`display_delay` / `display_delay_enable`, `min_number_of_capture_buffers`
max 32.

### Installed package on kernel 7.2.6 (2026-09-22)

Unchanged installed driver `579d1bc1fa18…` (RPM `libva-v4l2-iris-0.1.0-1`),
Chrome 152 through the packaged launcher, 30 s windows, boot ID unchanged
throughout:

| Clip | Decoder | Frames / dropped |
| --- | --- | --- |
| H.264 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 719 / 18, then 718 / 0 and 719 / 0 |
| HEVC Main 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 718 / 0 |
| VP9 1080p24 alt-ref | `VaapiVideoDecoder`, platform=true | 720 / 6, then 719 / 0 |
| HEVC Main10 1080p24 B-frames | `VaapiVideoDecoder`, platform=true | 720 / 0 |

The two non-zero drop counts were the first runs after the reboot (load
average above 10 for the first minutes); the immediate reruns were clean.

### Target cleanup (2026-09-22)

The pre-RPM install path had left five desktop entries under
`/usr/local/share/applications` (two Chromium ones whose `TryExec` pointed
at a browser that is not installed, the Vulkan/WebGPU experiments, a
Chromium 154 entry for a build that no longer exists) and a stale driver at
`/usr/local/lib64/dri/v4l2_drv_video.so`. They were moved to
`$IRIS_LAB_ROOT/residue-backup-20260922/`, not deleted. What remains:
the RPM's `google-chrome-iris-v4l2.desktop`, the user-level
`google-chrome.desktop` override that routes the plain Chrome icon through
`iris-vaapi-browser`, and Fedora's own `chromium-browser.desktop`.

## Client-owned CAPTURE buffers (Import): the §9 gate and the verdict (2026-09-22)

Branch `iris/zero-copy`. Target, module and driver: boot `0bdad14e…`,
`qcom-iris` `86e5e49a…` (decode-order), driver build `2796989e…`.

The gate the architecture doc demands is `test/iris-import-probe.cc`: a
standalone V4L2 program that feeds a stream whose every picture's luma is a
flat grey encoding its decode-order index, queues one client DMA-BUF per
access unit before the unit, and compares each completion's token with the
token its buffer was queued for.

What it measured:

| Configuration | Wrong association |
| --- | --- |
| 1:1 queue-at-submit, 8-16 surfaces, no holes, B-frames | 246/360 |
| ... bounded to 2-3 buffers in flight | 246-353/360 (pairwise swaps) |
| 300 s Chrome, H.264 b3, holes allowed (released surfaces) | 96-272 misplaced |

The pixels themselves are always correct: for a given token the grey is
identical across runs with different surface counts (360/360), and
completions arrive strictly in decode order. The firmware chooses which
queued CAPTURE buffer a picture lands in; the completion names the picture
(token/timestamp), not the buffer.

Two rules then make the association exact, both measured:

1. **One client buffer in flight.** The next buffer goes in only after the
   previous completion, so the firmware has no second buffer to choose.
2. **No holes.** A picture whose surface the client released still gets its
   buffer (the backlog holds an owned fd duplicate); skipping it shifted
   every later completion into a neighbour's buffer, permanently.

With both, on Chrome 152 (30 s windows, platform decoder, GPU process on
the Iris node, boot unchanged):

| Stream | Import publishes | Misplaced | Dropped |
| --- | --- | --- | --- |
| H.264 1080p24 B-frames | 901 | 0 | 0 |
| Frame-index 1080p30 B-frames | 1125 | 0 | 0 |
| HEVC Main 1080p24 B-frames | 905 | 0 | 0 |
| HEVC Main10 1080p24 B-frames | 907 | 0 | 0 |
| VP9 1080p24 alt-ref | not enabled (Copy) | — | see below |

FFmpeg through Import decodes bit-exactly (`framemd5` identical to the
software decoder, H.264 and HEVC); the 6-switch dynamic-resolution suite
passes in Import mode.

VP9 under Import was chased to its mechanism on 2026-09-22 and stays
excluded:

- a one-buffer window stalls Chrome's VP9 session ~500 ms on every alt-ref
  access unit (the superframe decodes as two pictures and the second has no
  buffer to land in); measured gaps of 477-500 ms at tokens 14, 28, 55, 77,
  ... and a drain failure at teardown; FFmpeg's VP9 through Import is
  unaffected (204/204, `framemd5` identical, 1.7 s wall for 8 s of video).
- a two-buffer window removes the stall (714/719 frames in 30 s) but
  reintroduces the firmware's own buffer choice: 153/609 and 159/599
  misplaced in two runs, matching the probe's finding that association is
  only determined when the firmware has no choice.
- the fix therefore belongs in the VP9 translator (one access unit per
  picture instead of an alt-ref superframe), not in the session.

Verdict: the policy exists, with the two rules enforced in the session and
the misplaced-completion check (both surfaces involved are failed, never
published) as the fail-closed backstop. `V4L2_VA_PUBLISH=import` forces it;
`auto` selects it for H.264/HEVC/Main10 on a decode-order kernel and keeps
Copy for VP9 and for display-order kernels.

## Merge gate: the trunk with the Import policy (2026-09-22)

Branch `main` at `6dd8bd2` (the zero-copy work merged), driver
`f9f4e423050f…` built clean on the target (`ldd -r` reports no undefined
symbols), boot ID `5a337970…` unchanged through every run.

| Gate | Result |
| --- | --- |
| Codec matrix | 15 PASS, 0 FAIL: H.264 / VP9 / HEVC 48/48 frames each, exact frame MD5 against the native V4L2 decoders, AV1 lane included |
| Structure matrix | pass |
| Dynamic resolution (H.264, 100 switches) | pass, 101 fresh contexts, no timeout |
| Chrome 152, auto policy | H.264 902 import, HEVC 905 import, Main10 898 import, VP9 908 copy-gpu; 0 misplaced, 0 decode errors, all platform decoder with the GPU process holding the Iris node |

Chrome's own dropped-frame counts in those 30 s windows were 2, 2, 1 and 0.

### Merge gate continued: soak, package, installed acceptance (2026-09-22)

- Bounded soak on the merged trunk: `dual-h264`, 120 s per context, 2880
  frames each, `teardown_timeouts=0`, `stateful_drain_complete=1`, hottest
  zone 43 °C, boot ID unchanged.
- RPM rebuilt from the merged tree (`source_commit=18be55a`, tracked-source
  hash recorded in the install manifest), reinstalled; `rpm -V` clean. The
  installed ELF hash differs from the build tree by design: the spec
  post-processes the staged runtime module (see the spec header).
- Installed-package acceptance, no `--driver` override, auto policy:
  H.264 902 import, HEVC 906 import, Main10 898 import, VP9 915 copy-gpu;
  0 misplaced, 0 decode errors, `VaapiVideoDecoder`, GPU process holding
  the node, boot unchanged. The Import path is now the shipped default
  where the evidence allows it.

## Encode entrypoint: H.264 and HEVC (2026-10-22)

Implementation commit `f63afe0` on `feat/encode-entrypoints`. Target:
Fedora 44 ARM64 on SM8550 (`sheng`), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10` unchanged through every run.
Decoder and encoder nodes resolved by the `iris_driver` QUERYCAP name:
`/dev/video0` (`Iris Decoder`) and `/dev/video1` (`Iris Encoder`). The
driver under test was `build-encode/src/v4l2_drv_video.so`, SHA-256
`bb4549260f2c6d7b021889d3222e386cfe19735ea61282d93cf97c9e577daed9`,
selected with `LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=<build>/src`.
No installed package was involved in this section.

`vainfo` lists `VAEntrypointEncSlice` next to `VAEntrypointVLD` for H.264
Constrained Baseline, Main and High and for HEVC Main. VP9 and AV1 get no
encode entrypoint: the encoder node's CAPTURE queue enumerates H264 and
HEVC only (`iris_venc.c`, `iris_venc_formats_cap`) and its raw queue NV12
and QC08C (`iris_venc_formats_out`), so there is no hardware path to
advertise.

| Run | Frames in/out | Bytes | Iris IRQ delta |
| --- | ---: | ---: | ---: |
| `h264_vaapi` 1920x1080, 4 Mbit/s VBR, GOP 60 | 300/300 | 9,918,858 | 7441 → 8049 (+608) |
| `hevc_vaapi` 1920x1080, 4 Mbit/s VBR, GOP 60 | 300/300 | 9,966,042 | 8050 → 8660 (+610) |
| `h264_vaapi` 1920x1080, CBR 4 Mbit/s, High, level 4.1 | 300/300 | 4,988,651 | — |
| `h264_vaapi` 1280x720, 2 Mbit/s VBR, GOP 30 | 120/120 | 2,003,275 | — |
| `hevc_vaapi` 1280x720, 2 Mbit/s VBR, GOP 30 | 120/120 | 2,005,117 | — |
| `h264_vaapi` 640x360, 1 Mbit/s VBR, GOP 15 | 30/30 | 118,372 (last run of the loop below) | — |

Every stream decodes with `ffmpeg -v error -i <out> -f null -` and reports
the requested geometry, profile and level:

- H.264 VBR: High, level 4.0, 1920x1080, 300 frames. Key frames at 0, 61,
  122, 183, 244: the firmware counts the IDR inside `GOP_SIZE`, so
  programming `intra_period` directly put them one frame late. The session
  now programs `intra_period - 1`; the acceptance run below shows
  0, 60, 120, 180, 240.
- H.264 CBR with `-profile:v high -level 4.1`: High, level 4.1,
  1920x1080, 300 frames. The byte count is 3.99 Mbit/s over the 10 s of
  content, which is the requested CBR target.
- HEVC VBR: Main, level 4.0, 1920x1080, 300 frames.
- 720p: both codecs report 1280x720, 120 frames. The raw port was
  aligned to 1280x736 and the crop kept the SPS at 720 lines.
- 640x360: 640x360, 30 frames.

The SPS VUI carries `num_units_in_tick=2097152`, `time_scale=125829120`
(30 fps), which is what `S_PARM` on the CAPTURE queue produced. Raw
H.264/HEVC demuxers report 25 fps because they do not read the VUI; the
remux below shows the intended rate.

Context lifetime: ten consecutive 720p `h264_vaapi` runs (create context,
ten pictures, destroy) all exited 0 with no driver error in `dmesg`, and
the last stream decodes clean.

Remux with no extradata: `ffmpeg -framerate 30 -i enc.h264 -c copy
enc.mp4` and the same for HEVC both produce MP4s whose `ffprobe` shows
30 fps and non-zero extradata (46 and 127 bytes) built from the Annex-B
parameter sets, and which decode without error.

Negative results, as designed:

- `-bf 2` is silently clamped by FFmpeg to no B frames; a client that
  sends `ip_period > 1` gets `VA_STATUS_ERROR_INVALID_PARAMETER`.
- `VAConfigAttribEncPackedHeaders` is `VA_ATTRIB_NOT_SUPPORTED`, so
  FFmpeg warns "Driver does not support any packed headers (wanted
  0xd)" once and continues with the firmware's own parameter sets.
- A 16384x16384 source is rejected by `testsrc2` before the driver sees
  it; the driver's own bound is the node's 8192.

### Decode regression

The encode work changes `vaDeriveImage` from a snapshot copy to a view
over the surface's DMA-BUF, and changes node discovery. Both are
decode-visible, so the decode gates ran again on the same build:

| Gate | Result |
| --- | --- |
| `test/iris-matrix.sh` | H.264, HEVC and VP9 48/48 frames each, exact frame MD5 against the native V4L2 decoders; AV1 lane unchanged |
| Chrome 152.0.7977.82, 20 s window, build-tree driver | H.264 `VaapiVideoDecoder`, platform=true, 599 frames, 4 dropped, GPU process holding the node; HEVC `VaapiVideoDecoder`, platform=true, 601 frames, 2 dropped |
| Firefox 158.0, H.264 clip | VA session through the driver (`session open node=/dev/video0 codec=H264 1920x1088`, `submit`, `capture configure`), playback advanced to 13.7 s |
| Chromium 152.0.7977.75 | `FFmpegVideoDecoder`, platform=false: the launcher's documented software fallback for Fedora Chromium ≥ 152, not a driver result |

Firefox needed `LIBVA_V4L2_VIDEO_PATH` and `MOZ_DISABLE_RDD_SANDBOX=1`:
with the RDD sandbox on, the driver's `/dev/video*` scan is denied in that
process. That is a pre-existing sandbox boundary, not an encode change.

**Correction (2026-10-24).** The sandbox is not what Firefox needs, and
disabling it was never necessary. The driver globs `/dev/video*` only for a
node whose path variable is unset: with `LIBVA_V4L2_VIDEO_PATH` and
`LIBVA_V4L2_ENCODER_PATH` both set, `strace -f -e trace=openat` over
`vainfo` shows exactly two opens, `/dev/video2` and `/dev/video3`, and no
scan of the others. Only the decoder path was set in that old run, so the
encoder probe had to glob, which a sandboxed process may be denied. The
launcher now resolves both nodes and exports both paths
(`scripts/iris-vaapi-browser`), so a client keeps the sandbox and still
reaches the driver. Verified on the target: the launcher exports
`LIBVA_V4L2_VIDEO_PATH=/dev/video2 LIBVA_V4L2_ENCODER_PATH=/dev/video3`,
and under those two variables `vainfo` reports the same 12 entries
(6 profiles x VLD and EncSlice) with two `openat` calls. Headless Firefox
plays the clip without a VA session in either configuration, so this is a
mechanism result from `strace` and `vainfo`, not a Firefox decode run.

### What the traces showed

- The SM8550 encoder firmware sends no frame info, so
  `V4L2_BUF_FLAG_KEYFRAME` is always clear on a CAPTURE completion. The
  `encode frame` record therefore carries no key flag; the IDR NAL in the
  stream is the evidence.
- FFmpeg renders the sequence parameter buffer with the IDR only. The
  context retains that buffer, otherwise the second picture fails with
  "no sequence parameters".
- FFmpeg declares an aligned context (1920x1088) and feeds 1920x1080
  frames. The encoder geometry therefore comes from the input surface,
  with the context as the upper bound; a later surface of a different
  size is rejected because the node has no `SOURCE_CHANGE`.

### Piano acceptance: CQP, GOP cadence, decoded pixels (2026-10-22)

The piano project's `scripts/verify-device-media.py` is the acceptance
bar this driver has to clear: 300 frames of 1080p H.264 and HEVC through
`h264_vaapi`/`hevc_vaapi` with no `-b:v` (FFmpeg selects CQP), MP4
output, key frames at 0/60/120/180/240, and 300 decoded frames whose
per-frame MD5 matches a software decode. The driver failed it twice
before this run: `VA_RC_CQP` was not advertised, so the encoder would
not open at all, and the IDRs landed at 61/122/... with `-g 60`.

With CQP, the `intra_period - 1` GOP mapping, and the CAPTURE format
re-set after the rate-control controls, on the same target:

| Run | Result |
| --- | --- |
| `h264_vaapi`, 1920x1080, no bitrate, `-bf 0 -g 60`, 300 frames, MP4 | 17,138,094 bytes, Iris IRQ +610, key frames 0/60/120/180/240 |
| `hevc_vaapi`, the same | 13,171,327 bytes, Iris IRQ +609, the same key frames |
| `h264_v4l2m2m` decode of the H.264 file | 300 frames, every frame MD5 equal to the software decode, Iris IRQ +614 |
| `h264_v4l2m2m` decode of the HEVC file | 300 frames, MD5 equal, Iris IRQ +840 |
| `ffmpeg -hwaccel vaapi` (this driver) decode of the H.264 file | 300 frames, MD5 equal |

Source `3b91519` plus the CQP/GOP change (uncommitted at the time of the
run), driver `build-encode/src/v4l2_drv_video.so` SHA-256
`5805975ab3338e4ebcefac6c126546bb0be015eb8c213be5fb983f3a53d28b33`,
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10` (unchanged).

CQP honors the client QP: 60 frames of 1080p H.264 are 5,445,355 bytes
at `-q:v 10`, 3,398,734 at the default and 244,736 at `-q:v 45`. FFmpeg
still warns that no packed headers are advertised; MP4 muxing succeeds
because the muxer derives `avcC`/`hvcC` from the Annex-B parameter sets
in the stream, but the encoder offers no extradata at open time.

Not covered: a per-picture IDR request (`-force_key_frames`, seeks). The
running kernel has no `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, so the
periodic IDR is the only keyframe source.

## Decode throughput and the build type (2026-10-22)

A user-space cost measurement taken while investigating decode throughput.
The clips are `<scratch>/iris-enc/media/clip-{hevc,h264}.mp4`: 1920x1080, 900
frames, decoded 300 frames each with `-hwaccel vaapi
-hwaccel_output_format vaapi -f null`, so the frames stay on the VA surface
and nothing is downloaded. Wall clock and CPU come from `-benchmark`; the
native rows are `-c:v {hevc,h264}_v4l2m2m` timed with the shell.

| Decoder | HEVC rtime / utime | H.264 rtime / utime |
| --- | --- | --- |
| This driver, `buildtype=debug` (`-O0`) | 6.569 s / 5.458 s | 1.132 s / 0.945 s |
| This driver, `buildtype=debugoptimized` (`-O2`) | 0.453 s / 0.279 s | 0.515 s / 0.123 s |
| Installed RPM driver (2026-09-02, `-O2`) | 0.607 s / 0.462 s | 0.514 s / 0.131 s |
| `hevc_v4l2m2m` / `h264_v4l2m2m` | 0.624 s / 0.168 s | 0.673 s / 0.146 s |

The same driver source at `-O0` is 14x slower on HEVC and 2.2x on H.264.
On the whole 900-frame clip the optimised driver holds 778 fps on HEVC
(1.156 s) and 637 fps on H.264 (1.414 s).
Temporary timers inside `vaEndPicture` located the cost in
`translator->translate` itself: 10.4 ms per HEVC frame at `-O0` against
0.62 ms at `-O2`, with `submit` at 0.96 ms against 0.39 ms. The
instrumentation was removed after the measurement.

Meson's own default is `-O0`, so `meson setup build` as documented in the
README produced an unoptimised driver. `meson.build` now sets
`default_options: ['buildtype=debugoptimized']`. The installed RPM was
built by rpmbuild's own flags and is optimised, so no browser result in
this file changes; only source-tree builds were affected.

Driver `build-def/src/v4l2_drv_video.so` SHA-256
`9e2ecbafcd49dafcf17e23782b66bbf5d82e08d9ea8b8de5483d495aa4c256b8`,
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10`, decoder node resolved by
`iris_driver` name as before.

### 4K HEVC slice rewrite (2026-10-22)

The 1080p throughput above hid a second problem. Synthetic 4K clips
(testsrc2 3840x2160: H.264 from `libx264 -preset ultrafast -qp 20`,
HEVC from `libx265 -preset ultrafast crf=22 keyint=60`) decoded 60
frames bit-exactly through the VA path, with `hwdownload,format=nv12`
and `format=nv12` on the reference decode so both sides hash the same
layout:

| Clip | VA frames matching software |
| --- | --- |
| 4K H.264, 60 frames | 60/60 |
| 4K HEVC, 60 frames | 60/60 |

Throughput was not. Ten copies of the HEVC clip concatenated (600
frames, about 140 KB per slice) took 10.75 s, 9.06 s of it user CPU:
56 fps against 233 fps for `hevc_v4l2m2m` on the same file.
`HEVC::rewrite_slice` re-emits every slice because the original RPS
cannot be reproduced by a generated SPS, and it did that one bit at a
time through `BitWriter::bit` — 1.1 million calls per frame at that
size.

`BitReader::copy` now takes a whole-byte path when the reader position
and the writer are both byte-aligned, through a new `BitWriter::append`;
the reader also reserves the NAL size up front. The same 600 frames:

| | rtime | utime | fps |
| --- | --- | --- | --- |
| Before | 10.750 s | 9.060 s | 56 |
| After | 3.447 s | 2.581 s | 172 |
| `hevc_v4l2m2m` | 2.572 s | 0.775 s | 233 |

Timers inside `vaEndPicture` put `translate` at 0.15 ms per 4K frame
with the clocks at their initial state and 1.5 ms after twenty seconds
of decoding, against 14 ms before the change. That 0.15 -> 1.5 ms growth
is the clock, not accumulation: the 2026-10-25 section below re-ran it
with the core frequency held fixed and got a flat series. The part of the
process CPU that remains beyond the driver is not FFmpeg's VA-API HEVC
parsing alone; it tracks the bitstream size, which is the slice rewrite
(same section).
`test/hevc-parameter-sets-test.cc` now compares the fast path against a
per-bit copy over 18 spans, aligned and unaligned, before the driver is
allowed to use it.

Driver `build-def/src/v4l2_drv_video.so` SHA-256
`d824cd5624c085ce210fa14f882363afe5dd4c2be3448afbe1e918c71f8d65f3`,
same kernel and boot ID as above.

## Client coverage and encode scale-out (2026-10-22)

Every run here used a staged install of the driver
(`DESTDIR=… IRIS_VA_DRIVER_DIR=/usr/lib64/dri scripts/install-system.sh
build-def`), which is also the only way the `msm_drv_video.so` alias exists.
`LIBVA_DRIVER_NAME` was unset throughout, so libva had to resolve the driver
by DRM name the way a client that knows nothing about this project does.
Driver SHA-256 `d824cd5624c085ce210fa14f882363afe5dd4c2be3448afbe1e918c71f8d65f3`,
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10`.

### Clients that set no environment

| Client | Command shape | Result |
| --- | --- | --- |
| FFmpeg | `-hwaccel vaapi -hwaccel_device /dev/dri/renderD128` | decodes; used for every earlier row |
| Chrome 152 | `test/iris-browser-acceptance.sh --driver …/build-def/src/v4l2_drv_video.so` | H.264 600 frames / 2 dropped, HEVC 600 / 2, `VaapiVideoDecoder`, platform=true, GPU process holding the node |
| mpv v0.41 | `--hwdec=vaapi --vo=gpu --gpu-context=wayland`, desktop session | `Using hardware decoding (vaapi)`, 60 frames, 65 capture tokens; libva opened `msm_drv_video.so` and found our init function |
| VLC 3.0.24 | `cvlc --avcodec-hw=vaapi --vout=dummy`, desktop session | `Using Qualcomm Iris V4L2 stateful (libva-v4l2) for hardware decoding`, 60 frames, 61 capture tokens |
| GStreamer 1.28.8 | `filesrc ! qtdemux ! h264parse ! vah264dec ! fakesink` | H.264 100 frames / 101 capture tokens, HEVC 100 / 102 |

Two client-side gates are worth recording, because neither is visible from
the driver:

- GStreamer's VA plugin rejects any driver whose vendor string is not on its
  allow-list (`_gst_va_display_filter_driver`, `gstvadisplay.c:186`), and the
  `va` plugin then registers zero elements. `GST_VA_ALL_DRIVERS=1` overrides
  it; with that set, `vah264dec`/`vah265dec` appear and decode through Iris.
- GStreamer's VA **encoder** could not start while this driver answered
  `VA_ATTRIB_NOT_SUPPORTED` for `VAConfigAttribEncPackedHeaders`:
  `gst_va_display_get_packed_headers` fails, `_init_packed_headers` returns
  FALSE, and `gst_va_base_enc_reset` reports "Error at reconfiguration
  error" before any frame is submitted. The driver now answers
  `VA_ENC_PACKED_HEADER_NONE` (zero) instead, which is the accurate answer —
  the firmware writes SPS/PPS, so there is no packed header to accept, but
  the attribute is known. GStreamer returns -1 only for
  `VA_ATTRIB_NOT_SUPPORTED` (`gstvad.c:171-201`), so value zero passes and
  the encoder proceeds with no packed headers requested.
  **Correction:** an earlier revision of this file claimed Radxa's backend
  answers `VA_ENC_PACKED_HEADER_NONE` and "fails the same check". That was
  wrong. Zero is not the rejection value, and Radxa's backend passes this
  check too.

### MP4 without a remux

The earlier note that MP4 output needs a remux was wrong. FFmpeg warns
("No global header will be written") and then carries the parameter sets
in-band, extracting them from the first packet: a 120-frame 1280x720 encode
yields `extradata_size=43` for H.264 and `124` for HEVC, and both MP4 files
decode cleanly with no remux. The CQP path (no `-b:v`) behaves the same.

### Encode throughput, 4K, and two contexts at once

300 frames of `testsrc2`, CBR 12 Mbps, `-g 60`, NV12 uploaded from system
memory, `ffmpeg -benchmark`:

| Codec | 1920x1080 | 3840x2160 |
| --- | --- | --- |
| H.264 High | 1.872 s, 160 fps | 3.057 s, 98 fps |
| HEVC Main | 1.374 s, 218 fps | 2.816 s, 107 fps |

Every output decodes with no errors, and the IDRs land at
0/60/120/180/240 at both sizes. The 4K H.264 stream reports level 5.1 and
HEVC level 5.0, which is the encoder's choice for that geometry, not a
client request.

Two encodes in parallel on one node (H.264 8 Mbps at 1080p and HEVC 8 Mbps
at 1080p, 300 frames each) both exit 0, produce 19,933,115 and 19,914,057
bytes, decode cleanly, and move the Iris IRQ counter by 1,154 during the
pair. The session serialises each context internally; nothing in the two
runs reported a timeout or a dropped picture.

### HEVC conformance recheck: all three residuals are exact (2026-10-22)

Phase 7 recorded `DELTAQP_A_BRCM_4`, `INITQP_B_Main10_Sony_1` and
`PMERGE_E_TI_3` as decoding every frame without a firmware error but
differing in pixels, and left them unchased. Rechecked on the same target
with the current driver, they are exact.

The earlier comparison hashed `framemd5` straight out of each decoder. A
framemd5 is taken over the frame's planes at the decoder's linesize, so a
padding or stride difference between the VA surface and libavcodec's
frame changes the hash without changing a pixel: the streams report
different hashes for every frame, while a contiguous NV12 dump of the
whole stream is byte-identical. Normalising the software side with
`-vf format=nv12` and the VA side with `hwdownload,format=nv12` removes
the layout difference, and the hashes agree frame for frame:

| Vector | Frames | Result |
| --- | --- | --- |
| `DELTAQP_A_BRCM_4` (Main, 1920x1088) | 96 | MD5 identical, `nv12` |
| `PMERGE_E_TI_3` (Main, 416x240) | 8 | MD5 identical, `nv12` |
| `INITQP_B_Main10_Sony_1` (Main 10, 416x240) | 90 | MD5 identical, `p010le` |

`INITQP_B_Main10_Sony_1` also has to be downloaded as `p010le`: asking for
`nv12` from a 10-bit surface fails outright, which is what the earlier
"VA decode failed" line in this recheck first hit. No driver change was
needed. Use normalised output when comparing hashes across decoders; a
raw framemd5 compares frame layouts as much as pixels.

### Encoder control surface, rechecked (2026-10-22)

`v4l2-ctl -d /dev/video1 --list-ctrls` on the running module lists the GOP
size, `video_bitrate_mode` (VBR/CBR), the frame-level RC switch, the
sequence-header mode, the LTR controls and the H.264/HEVC profile, level and
QP controls. It lists no `force_key_frame`, no RAP control and no
`V4L2_CID_MPEG_VIDEO_FRAME_RATE`. That absence is a property of this kernel
branch, not of the firmware: 7.3-rc4's `inst_fw_cap_sm8550_enc[]` lists
`REQUEST_SYNC_FRAME` and `iris_ctrls.c` maps `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`
onto it. The conclusion recorded here — that
on-demand IDR therefore stays unimplemented — was wrong about what the
absence rules out: the intra period is a GOP control the node does have,
and the session uses it for one picture. See "On-demand IDR by moving the
intra period" below. `intra_refresh_period` is still a different feature
(periodic intra refresh, not an IDR request) and is not used.

## Client requests the encoder cannot honour (2026-10-22)

Source `956fca0` plus this change (uncommitted at the time of the run),
driver `build/src/v4l2_drv_video.so` SHA-256
`c2a57b2dc298fda39e79aa69c0a9e4daf1d6eb26d400de759fbbf1e6e95d4b50`
(deployed from tracked-source SHA-256
`16fb6107f377ee69e47426b11cd8e06a45ff8d516599899e315de00afa2e0c36`),
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10` (unchanged), build tree only, no
installed package. Decoder and encoder nodes resolved by the `iris_driver`
QUERYCAP name: `/dev/video0` (`Iris Decoder`), `/dev/video1`
(`Iris Encoder`).

Two classes of gap against the reference driver (radxa `libva-v4l2`
1.0.0) closed in one change: a client's slice deblocking request was read
by nobody, and the other VA picture/slice fields that the firmware cannot
be told about were ignored silently.

The kernel this target runs exposes **no** loop-filter control on either
node: `v4l2-ctl -d /dev/video1 --list-ctrls | grep -ciE
'loop_filter|lf_(tc|beta)'` is 0, and the same holds for the decoder node.
The firmware there always filters with the default 0/0 offsets, so the
driver forwards the request when those six controls exist and returns
`VA_STATUS_ERROR_UNIMPLEMENTED` when they do not — with one exception,
mode 2 (filtering off across slice boundaries only), which with one slice
per picture produces the same stream as mode 0 and is therefore accepted.
Mode 2 is not hypothetical: it is exactly what an HEVC client that leaves
`slice_loop_filter_across_slices_enabled_flag` clear normalises to, and
FFmpeg sends zeroed slice parameters, so HEVC encode on this target takes
that branch. The trace records which one ran:

```
encode deblocking mode=0 alpha=0 beta=0 via=default t=25     (h264_vaapi)
encode deblocking mode=2 alpha=0 beta=0 via=default t=21     (hevc_vaapi)
```

Acceptance re-run on this build, 300 frames each, `V4L2_VA_TRACE=1`:

| Run (1920x1080, 30 fps) | Frames | Bytes | Iris IRQ delta | Key frames |
| --- | ---: | ---: | ---: | --- |
| `h264_vaapi`, no bitrate (CQP), `-bf 0 -g 60` | 300 | 17,135,872 | +609 | 0/60/120/180/240 |
| `hevc_vaapi`, the same | 300 | 13,169,058 | +610 | 0/60/120/180/240 |
| `h264_vaapi -b:v 4M` (VBR) | 300 | 9,915,972 | — | — |
| `h264_vaapi -rc_mode CBR -maxrate 4M -profile:v high -level 4.1` | 300 | 4,987,971 | — | — |
| `h264_vaapi` 1280x720 `-b:v 2M -g 30` | 120 | 1,994,401 | — | — |
| `hevc_vaapi` 1280x720 `-b:v 2M -g 30` | 120 | 2,010,359 | — | — |

Every output decodes with `ffmpeg -v error -i <out> -f null -`, and the
two CQP files decode through this driver to 300 frames whose per-frame MD5
matches a software decode exactly (`hwdownload,format=nv12` on the VA
side, `format=nv12` on the software side). The CQP byte counts match the
earlier record to four significant figures (17,138,094 and 13,171,327 on
the pre-change build), which is what a change that only adds validation
should look like.

Profiles and the documented failure modes still behave:

| Run (1280x720, 60 frames) | Result |
| --- | --- |
| `-profile:v constrained_baseline` | exits 0, 525,430 bytes, `ffprobe`: Constrained Baseline, level 3.1 |
| `-profile:v main` | exits 0, 522,808 bytes |
| `-bf 2` | exits 0, 523,190 bytes: FFmpeg clamps to no B frames before the driver sees it |
| `-slices 2` | fails in FFmpeg with `Invalid argument` before the driver sees it; the driver's own rule is `VA_STATUS_ERROR_UNIMPLEMENTED` for more than one slice |
| `-q:v 10` (CQP) | exits 0, 2,682,127 bytes for 60 frames |

`test/encode-params-test.cc` covers the translator's side of the refusals
(weighted prediction, scaling lists, chroma QP offsets, reference indexes,
a CABAC initialisation index, two slices, HEVC tiles, wavefront and
dependent slice segments). `test/unit/encode-session-test.cc` is new: a
scripted encoder node with and without the loop-filter controls, which
pins both halves of the deblocking promise — the three control writes with
their HEVC menu mapping, and the refusal when the controls are absent, plus
"changing deblocking mid-context needs a new context". `meson test` is
16/16 and `sh test/run-static-checks.sh` passes.

The refusal path is on the target as well, since no VA client there asks
for non-default deblocking and FFmpeg cannot be made to.
`test/iris-encode-control-probe.cc` (built, not registered as a meson
test: it needs the node) drives `EncoderSession` against the real encoder
node. Resolved by driver name, `/dev/video1`:

```
control h264_loop_filter_mode    absent
control h264_loop_filter_alpha   absent
control h264_loop_filter_beta    absent
control hevc_loop_filter_mode    absent
control hevc_lf_tc_offset_div2   absent
control hevc_lf_beta_offset_div2 absent
PASS h264: the kernel default deblocking request configures
PASS hevc: mode 2 configures (the request FFmpeg's zeroed slice parameters normalise to)
PASS h264: disabling deblocking is refused
PASS h264: a deblocking offset is refused
PASS hevc: disabling deblocking is refused
PASS node=/dev/video1 loop_filter_controls=absent h264_off=0x14 h264_alpha=0x14 hevc_off=0x14
```

`0x14` is `VA_STATUS_ERROR_UNIMPLEMENTED`. The probe expects that status
when the node reports the controls absent and success when it reports them
present, so it is a gate on either kind of kernel, and it configures and
tears down four real sessions on the way (STOP, drain, STREAMOFF).

## Client-owned surfaces: DRM_PRIME_2 and PRIME_1 import (2026-10-22)

Source `2cb3a7b`, driver `build/src/v4l2_drv_video.so` SHA-256
`2f99c2a1d165aeb35fcdea04f2492d42cbf29659a49c3c21cdf6d8b5fc8e1101`
(deployed from tracked-source SHA-256
`9d7df611a09ee3f5fd464c5e6945bb68fbd777d936a98d84aaa50922c92442d3`,
provenance `source_dirty=false`), kernel `7.2.6-sm8550-g42f3b40-iris1`,
boot ID `32b1c05f-5043-4799-acd2-d4b0ec6f0e10` (unchanged), build tree
only, no installed package. Nodes resolved by the `iris_driver` QUERYCAP
name: `/dev/video0` (`Iris Decoder`), `/dev/video1` (`Iris Encoder`).

`vaCreateSurfaces2` now takes the client's own DMA-BUF (DRM_PRIME_2
descriptor or PRIME_1 buffer table) instead of refusing it. The surface is
a view over that object in the CAPTURE layout, the fd is duplicated, and
`persistent_export` is set, so the decode path is the Import policy already
recorded above (2026-09-22) and the encode path is the ordinary input
surface. Everything below is `LIBVA_DRIVER_NAME=v4l2` with
`LIBVA_DRIVERS_PATH=<build>/src`.

`prime-import` (host, no hardware) is the layout and refusal contract:
the accepted composed-layer and two-layer forms for NV12 and P010, the
accepted PRIME_1 table, and every refusal — foreign fourcc or size, two
objects, a missing fd, a tiled modifier, zero layers, swapped luma/chroma
layers, a pitch that is not the CAPTURE pitch, chroma at the wrong offset,
luma not at offset 0, an object smaller than its planes, an allocation
smaller than the descriptor claims, a tiled PRIME_1 table, two PRIME_1
buffers. It also pins the one definition of the layout: `plane_layout()`
and `stable_layout()` agree on stride and chroma offset at 1280x720,
1920x1080 and 3840x2176, and the stable size is the plane bytes rounded to
4 KiB.

`test/iris-va-import-probe.cc` is the target half. It is a plain libva
client: it allocates the client buffer the way a compositor on this
platform does (MSM GEM, `stable_layout` size) or, with the `heap`
argument, the way a client that sizes its own allocation does (system
heap, exactly the plane bytes, no 4 KiB rounding). Both runs, 1280x720
NV12:

```
PASS DRM_PRIME_2 import in the CAPTURE layout creates a surface
PASS driver-allocated surface creates
PASS the imported surface is a view of the client's bytes
PASS H.264 encode config creates
PASS encoding the imported surface gives the same bitstream
PASS changed client bytes change the bitstream by the same amount
payload normal=681554 import=681554 changed=687376 changed_import=687376
PASS a client pitch the firmware does not write is refused
PASS PRIME_1 import in the CAPTURE layout creates a surface
```

What each check establishes:

- **View, not copy.** Bytes written through the client's fd (bracketed with
  `DMA_BUF_IOCTL_SYNC`, chroma at the CAPTURE chroma offset) are what
  `vaDeriveImage` + `vaMapBuffer` returns, without any upload call.
- **The firmware reads the client's memory.** One intra-only CQP picture
  (QP 26, `intra_period = 0`) is encoded from the imported surface and from
  a driver-allocated one through a fresh context each, so both are the
  first picture of their session and no rate-control or `frame_num` state
  can differ. The two bitstreams are byte-identical (681,554 bytes). After
  changing the pixels on both sides the two bitstreams are again identical
  to each other (687,376 bytes) and different from the first pair.
- **Refusal happens at creation.** A descriptor whose luma pitch is twice
  the CAPTURE pitch returns `VA_STATUS_ERROR_INVALID_PARAMETER`; libva
  prints the driver's message.

The first measurement of this probe compared encodes inside one session and
was wrong for a mundane reason worth recording: the second picture was a P
frame predicting from the first, so a 554 KB IDR was followed by a 1.7 KB
P frame and the comparison "different" measured GOP position, not memory.

Regression on the same build: 120 frames each of
`sintel-1080p30-h264-b3.mp4` and `sintel-1080p30-hevc-b3.mp4` decode
through `-hwaccel vaapi -hwaccel_output_format vaapi` with no error, the
H.264 trace ends `session finish submitted=132 completed=132 errors=0
drops=0 timeouts=0 drains=1 misplaced=0`, a 30-frame `h264_vaapi` encode
produces a 30-frame stream, and `iris-encode-control-probe` on
`/dev/video1` reports the same six absent controls and `0x14` for the
three non-default deblocking requests as the previous record.

`meson test` is 17/17 ( `prime-import` added to the CI inventory) and
`sh test/run-static-checks.sh` passes.

## Encode timestamps across IDR pictures (2026-10-22)

Source `92cf5e1`, driver `build/src/v4l2_drv_video.so` SHA-256
`aefa2342228a4c4c084db8ef0b60afa5d30cfcbb1d45381a9fbe436750f5fc05`
(deployed from tracked-source SHA-256
`a42b2f4477c628e1fafff8e33ac98d332de61656570d515ee6ade4a2d94765d0`,
provenance `source_dirty=false`), kernel
`7.2.6-sm8550-g42f3b40-iris1`, module SHA-256
`4e29b74df8c9475f1b4ba4de5dc6aa34d90cb224a935459d6b5168c4590c822a`,
boot ID `32b1c05f-5043-4799-acd2-d4b0ec6f0e10` (unchanged), build tree
only, no installed package. Nodes resolved by the `iris_driver` QUERYCAP
name: `/dev/video0` (`Iris Decoder`), `/dev/video1` (`Iris Encoder`).

The encoder wedged at the first GOP-boundary IDR on every stream whose
rate was not 30 or 31 fps. `h264_vaapi` at 24 fps with `-g 60` stopped
after picture 61 with `endPicture: encoder produced no access unit`; at
24 fps with `-g 30`, after 31; at 24 fps with `-g 120`, after 121; 15, 25,
29, 48 and 60 fps failed the same way, while 30 and 31 fps ran to 300
frames. Size (640x360 too), codec (HEVC too) and rate control (CQP too)
made no difference, and the failure reproduced on the committed baseline
`2f99c2a1`.

Cause, from the frame trace: FFmpeg sends `VAEncMiscParameterFrameRate`
only with IDR pictures — its global parameters go out with IDR
(`vaapi_encode.c`, `base_pic->type == FF_HW_PICTURE_TYPE_IDR`). The driver
derived each picture's timestamp from that picture's own rate, so the
sequence read `… 333333, 458333 (IDR), 400000 …` at 24 fps: forward on
the IDR, backwards on the picture after it. The firmware stops returning
access units once a timestamp goes backwards. A fixed 30 fps fallback
applied to every non-IDR picture is what made exactly 30/31 fps and the
intra-only (`-g 0`, `intra_period = 0`) case pass. Two levers ruled out
the alternatives on the target: skipping `S_PARM` changed nothing, and any
strictly rising timestamp sequence passed even with a 1 ms step.

The session now fixes its rate at `configure()` (also the only point
`S_PARM` can change) and advances one frame duration per picture.

| Rate | GOP | Frames | IDRs | Encode | Decode |
| --- | --- | --- | --- | --- | --- |
| 24 | 60 | 300 | 5 | ok | ok |
| 24 | 30 | 120 | 4 | ok | ok |
| 25 | 50 | 150 | 3 | ok | ok |
| 30 | 60 | 300 | 5 | ok | ok |
| 60 | 60 | 240 | 4 | ok | ok |

1920x1080 `testsrc2`, `-b:v 4M`, Annex-B, 12,448,657 bytes for the
24 fps/300-frame case; every stream decodes with `ffmpeg -i … -f null -`
and no error. Iris IRQ delta over the five runs: 2266. Earlier on the
same tree: `sintel-1080p30-h264-b3.mp4` at 24 fps (~720 frames) encoded
whole, and 24 fps HEVC and 24 fps CBR also ran to completion.

`test/unit/encode-session-test.cc` submits FFmpeg's pattern — rate on
picture 0 and on the IDR, nothing in between — and requires strictly
rising timestamps; the old formula fails it (`FAIL: timestamps rise across
an IDR that carries the frame rate`). `meson test` is 17/17 and
`sh test/run-static-checks.sh` passes.

## On-demand IDR by moving the intra period (2026-10-22)

Source `9c29972`, driver `build/src/v4l2_drv_video.so` SHA-256
`573fd238dbf72fbc0cea229cd517ec3b0d847cc1c159208375821607175be753`,
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10` (unchanged), build tree only, no
installed package, `source_dirty=false`. 1280x720 at 24 fps, `-g 60`,
`-b:v 4M`, `-force_key_frames` from FFmpeg, decoded with
`ffmpeg -i … -f null -`.

| Request | IDR positions in the stream | Frames | Result |
| --- | --- | --- | --- |
| none | 0, 60, 120 | 130 | ok |
| `eq(n,24)` | 0, 24, 84 | 130 | ok |
| `eq(n,10)+eq(n,40)` | 0, 10, 40, 100 | 130 | ok |
| `gte(t,n_forced*1)` | 0, 24, 48, 72, 96, 120, 144, 168, 192, 216, 240, 264, 288 | 300 | ok |
| `eq(n,60)` (the firmware's own boundary) | 0, 60, 120 | 130 | ok |

Iris IRQ delta over the five runs: 1687. Every stream decodes without
error.

Three variants were measured and two of them are wrong, which is why the
session looks the way it does:

- **Restoring the period after the picture completes** puts the following
  picture at the head of a fresh GOP, so `eq(n,24)` produced `0 24 25`
  instead of `0 24`. The period has to be back before the picture is
  queued.
- **Skipping the lever at positions the firmware would make IDRs itself**
  (first picture, GOP boundary) made the driver's idea of the schedule
  drift from the firmware's: with a one-second request the stream came out
  `0 24 48 72 96 144 168 …`, and the requests at 120 and 240 became
  ordinary pictures — silently dropped keyframes, which is worse than an
  extra control write.
- **Never restoring the period** makes every following picture an IDR
  (`0 24 25 26 … 299`).
- Levering on *every* requested picture, including at the firmware's own
  boundary, never drops one. The boundary case produces `0 60 120`: a
  requested IDR there is already an IDR, so the request is satisfied, and
  the next automatic boundary moves accordingly.

The schedule therefore re-anchors on each requested IDR, the way x264 and
libx265 do. `test/unit/encode-session-test.cc` pins the lever, the
restore-before-queue ordering and the intra-only case; removing either
rule fails it (`FAIL: the period is restored before the picture is
queued`, `FAIL: a request at the firmware's own GOP boundary writes no
lever`). `meson test` is 17/17 and `sh test/run-static-checks.sh` passes.

## Segments cut at a requested IDR (2026-10-22)

Source `90f867b`, driver SHA-256
`d11df8030e61a231a21e0355476d5da3b96a448cdcd48e1d2c4309f6789aa4f6`
(`source_dirty=false`), kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10`, build tree only, 1280x720 at
24 fps, `-g 60 -force_key_frames expr:gte(t,n_forced*1)`, `-f segment
-segment_time 1 -segment_format mpegts`.

H.264 and HEVC both write 240 frames as 10 one-second segments; every
segment starts on a keyframe and decodes with `ffmpeg -i … -f null -` and
no error. Iris IRQ delta over both runs: 1633. The same build decodes 120
frames each of `sintel-1080p30-h264-b3.mp4` and
`sintel-1080p30-hevc-b3.mp4` through the VA path with no error.

The first attempt at this produced six of ten H.264 segments and nine of
ten HEVC segments that failed to decode, for two separate reasons:

- **The levered IDR carried no SPS/PPS.** The firmware prefixes the
  parameter sets to its own sync frames only, so a stream cut at a
  requested IDR began with an IDR slice referencing sets the segment did
  not contain (`non-existing PPS 0 referenced`). The session now keeps the
  sets the last access unit carried and prefixes a requested IDR that has
  none, which is what FFmpeg does with extradata before a keyframe.
- **HEVC sets were classified with H.264 rules.** Types 32-34 were not
  recognised as parameter sets at all, so HEVC segments still began with
  `PPS id out of range`. `test/annexb-test.cc` covers both codecs now; it
  also caught the type constants being declared `bool`, which made every
  non-zero NAL type compare equal to SPS.

`meson test` is 18/18 (`annexb` added to the CI inventory) and
`sh test/run-static-checks.sh` passes.

## Requested-IDR edge cases (2026-10-22)

Source `7205500`, same driver SHA-256 as the segment record
(`d11df803…`, `source_dirty=false`), same boot and kernel, 1280x720 at
24 fps.

| Request | Codec | IDR positions | Frames | Decode |
| --- | --- | --- | --- | --- |
| `eq(n,0)` | H.264 | 0 | 40 | ok |
| `expr:1` (every picture) | H.264 | 0 … 59, all of them | 60 | ok |
| `eq(n,59)` (the last picture) | H.264 | 0, 59 | 60 | ok |
| `eq(n,24)` | HEVC | 0, 24 | 70 | ok |
| `gte(t,n_forced*1)` | HEVC | 0, 24, 48, … 192 | 200 | ok |

The nine one-second HEVC segments from the last row all start on a
keyframe. Requesting every picture is not the failure mode the "never
skip the lever" rule was worried about: it is the intended cost of a
request per picture, and it produces an all-intra stream.

## Regression sweep over the encode changes (2026-10-22)

Source `227dfea`, driver SHA-256
`d11df8030e61a231a21e0355476d5da3b96a448cdcd48e1d2c4309f6789aa4f6`
(`source_dirty=false`), kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`32b1c05f-5043-4799-acd2-d4b0ec6f0e10`, build tree only.

- `test/iris-matrix.sh`, 48 frames: H.264, HEVC and VP9 all `PASS` with
  exact per-frame MD5 against the native V4L2 decoders, `submitted=48
  capture=48 drops=0 timeouts=0 drain=1`. AV1 skips for the documented
  OBU contract; MPEG-2/VP8 are not on this node.
- `test/iris-browser-acceptance.sh --driver build/src/v4l2_drv_video.so`,
  20 s of `sintel-1080p30-h264-b3.mp4`: `decoder=VaapiVideoDecoder
  platform=True frames=480 dropped=0`, driver
  `build-tree:d11df8030e61`, boot unchanged. A run without `--driver` also
  passes, on the installed package `df16127ccecf`; that one is the
  package's own record, not this tree's.

The same request pattern ran ten more times in one session (five H.264,
five HEVC, 120 frames each): every run exited 0, decoded cleanly and
carried the same five IDRs, so the lever's per-picture control write does
not drift the firmware's schedule across sessions.

## AV1 through VA: qualified and advertised (2026-10-22)

Source `52047fd`, driver SHA-256
`763025cc9941e7bdaa3145912bb9c1ee6458ac7b2ee703f412b66fc1d13bf32d`
(`source_dirty=false`, build tree only), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot ID
`6534c7a2-f3f7-469a-8e29-a4f6ef516f89` (unchanged after every run below).

### What changed since the 2026-09-22 verdict

Three behaviors, all in the driver:

- **The one-picture hold is explicit, not a failure.** VA's AV1 picture
  buffer carries no `refresh_frame_flags`; the slots a picture refreshes are
  only visible in the *next* picture's `ref_frame_map`. A shown key frame or
  switch frame refreshes all eight by definition
  (`av1_refreshes_all_slots`), so its bits are known at once; any other
  picture is translated with the next AU and its `vaEndPicture` returns with
  the surface still unsent. `vaSyncSurface`, `vaGetImage`, `vaDeriveImage`
  and `vaExportSurfaceHandle` now wait for that release instead of reading a
  surface with no pixels.
- **Hidden pictures are published, not counted as errors.** The firmware
  returns a `show_frame=0` picture with the ERROR flag, a zero timestamp, no
  KEYFRAME/PFRAME/BFRAME bit and zero bytes used, although it wrote pixels.
  The session attributes such a completion to the oldest pending target
  whose `Target::hidden` is set (decode order) and publishes it; a following
  non-intra picture after an end-of-stream release is refused with
  `VA_STATUS_ERROR_OPERATION_FAILED` rather than decoded against nameless
  slots.
- **End of stream releases the hold.** When the stream ends with a picture
  still held, the sync bound expires, the picture is submitted as final
  (refreshing no slot) and the trace records
  `va sync released the held picture surface=<n>`.

The opt-in switch `V4L2_VA_EXPERIMENTAL_PROFILES` is gone. AV1Profile0 is
advertised whenever the decoder node enumerates AV1 and NV12, the same rule
the capability table now encodes (`production: supported`,
`va_profiles: ["AV1Profile0"]`), and
`test/iris-codec-matrix-check.sh` checks the `profile-qualified-*` block
against the table in both directions.

### Evidence

`vainfo` on the build tree, no switch: `VAProfileAV1Profile0 :
VAEntrypointVLD`.

`test/iris-matrix.sh`, 48 frames, artifacts under
`<lab-root>/artifacts/va-matrix`:

| Codec | Result |
| --- | --- |
| AV1 | `PASS frames=48 submitted=50 capture=50 drain=1 timeouts=0 drops=0 noshow=23`, content `frame-md5` equal to software |
| H.264, HEVC, VP9 | unchanged: `submitted=48 capture=48 drops=0 timeouts=0`, content `frame-md5` equal |

Six clips, three runs each, every run's downloaded frames compared with the
software decode of the same clip (`sweep.sh`, both arms `yuv420p`):

| Clip | Stream shape | Runs |
| --- | --- | --- |
| `hw-av1-nolag.mp4` | IPPP, no alt-ref | 3/3 bit-exact |
| `hw-av1-noaltref.mp4` | no alt-ref | 3/3 |
| `hw-av1-lag.mp4` | alt-ref, 720p | 3/3 |
| `allintra.mp4` | all-intra | 3/3 |
| `n12.webm` | 12-bit source downconverted, alt-ref | 3/3 |
| `lag720.webm` | 720p, alt-ref | 3/3 |

`hw-av1-nolag.mp4`, 60 frames, with the sync bound cut to 100 ms: `submitted=60
completed=60 errors=0 drops=0 timeouts=0`, downloaded frames bit-exact. A
pipelined client never waits on the hold, so the bound is not part of normal
operation.

`test/av1-obu-roundtrip.sh`, no switch: `PASS rebuilt=18 source=32 all rebuilt
frames bit-exact` against libdav1d.

Chrome 152, `av1-browser.webm`, 25 s windows:

| Run | Decoder | Platform | Frames / dropped | Node holder |
| --- | --- | --- | --- | --- |
| 1 | `VaapiVideoDecoder` | true | 748 / 15 | `--type=gpu-process` |
| 2 | `VaapiVideoDecoder` | true | 747 / 4 | `--type=gpu-process` |

The two drop counts differ on the same clip and build; the second run had
the tablet to itself. Chrome pre-exports its surfaces, and AV1 Import is
refused (the held picture's client buffer cannot be queued in lockstep), so
Chrome publishes through the GPU copy engine.

### Client contract

A client that submits picture N, syncs its surface and only then submits
picture N+1 cannot be served in one round trip: it waits for the sync bound
and gets N released as final. FFmpeg and Chrome both keep at least one AU
in flight, so neither hits it. This is invariant 17 in
`docs/architecture.md`; the failure is a clear `OPERATION_FAILED`, never
pixels from the wrong picture.

## Pipelined encode: `vaSyncBuffer` and `-async_depth` (2026-10-22)

The encoder used to complete one picture inside `vaEndPicture`. FFmpeg turns
on its asynchronous path only when `vaSyncBuffer` exists
(`libavcodec/vaapi_encode.c`, the `VA_STATUS_ERROR_UNIMPLEMENTED` probe with
`VA_INVALID_ID`), so until now `-async_depth` did nothing and a second
picture could not be in flight.

`vaEndPicture` now submits and returns a ticket; the access unit is claimed
by `vaSyncBuffer` on the coded buffer, by `vaSyncSurface` on the input
surface, or by `vaMapBuffer` when the client does neither. The four encoder
input slots are the pipeline depth, and a submit that finds them all in use
completes the oldest picture itself instead of stalling with every CAPTURE
slot unrearmed.

### Build

- Source `8241dbd` plus an uncommitted working tree, tracked-source
  SHA-256 `710d465ed502a1cda9bc4a4acfbd5bc821c72c1945632e6f17f8a7d8631da8f5`
  (the provenance manifest records `dirty=true`, which is accurate here).
- Driver `build/src/v4l2_drv_video.so`, SHA-256
  `fac86f3da4c47881f352e6c981a7e24311f638e166912ec58ccd6dc80282fc36`,
  built from the build tree (`LIBVA_DRIVERS_PATH`), not the installed
  package. The throughput table below was measured on the immediately
  preceding build (`99c70568…`, identical sources except
  `VAConfigAttribEncMaxRefFrames`); the final build was re-measured at
  depths 1 and 4 and reproduced it (HEVC 3.211 s / 1.617 s, H.264 3.241 s /
  1.657 s) and reproduced the byte-identical sync/async bitstream.
- Kernel `7.2.6-sm8550-g42f3b40-iris1`, `qcom-iris.ko.zst` SHA-256
  `4e29b74df8c9475f1b4ba4de5dc6aa34d90cb224a935459d6b5168c4590c822a`.
- Boot ID `fd1b2389-f4f1-479e-a0aa-58464fc48a80`. Decoder `/dev/video0`,
  encoder `/dev/video1` (both resolved by `iris_driver`).
- FFmpeg 8.1.3.

### Throughput

`testsrc2` 1920x1080 at 30 fps, 600 frames, CBR 12 Mbit/s, `-g 60`, NV12
uploaded from system memory, `ffmpeg -benchmark`, one run each:

| `-async_depth` | HEVC rtime | H.264 rtime |
| --- | ---: | ---: |
| 1 | 3.159 s (190 fps) | 3.506 s (171 fps) |
| 2 | 1.940 s (309 fps) | 2.101 s (286 fps) |
| 4 | 1.605 s (374 fps) | 1.663 s (361 fps) |
| 6 | 1.645 s (365 fps) | — |
| 8 | 1.631 s (368 fps) | — |
| 16 | 1.667 s (360 fps) | — |
| 32 | 1.673 s (359 fps) | — |

Depth 4 is the knee because that is how many input slots the session
requests; deeper settings pipeline through the same four and gain nothing.
Before this change the depth-4 numbers were what depth 1 measures now.

### Correctness

- `-async_depth 1` and `-async_depth 8` over the same 300 frames at CQP 24,
  `-g 60`, produce byte-identical bitstreams (`051021fa44fc99be…`, `cmp`
  clean). Pipelining changes scheduling, not coding decisions.
- Every depth 4/8/16/32 stream reads back as 1920x1080, 600 frames, and
  decodes with `ffmpeg -f null -` without an error.
- On-demand IDR still cuts where it is asked under pipelining: 24 fps,
  `-g 60 -force_key_frames expr:gte(t,n_forced*1) -async_depth 4 -f segment
  -segment_time 1` yields 24 segments, each starting on a key frame.
- The decode matrix is unchanged on the same build: H.264, HEVC, VP9 and AV1
  all `PASS … content=frame-md5` with `timeouts=0 drops=0`, and AV1 still
  reports `submitted=50 capture=50 noshow=23`.

### Acceptance on the final build

Same build (`fac86f3d…`), 1920x1080 `testsrc2`, `-async_depth 4`:

| Case | Result |
| --- | --- |
| H.264 High, 300 frames, CQP 24, `-g 60` | 300 frames, 13122096 bytes, 5 key frames, decodes clean |
| HEVC Main, 300 frames, CQP 24, `-g 60` | 300 frames, 14301743 bytes, 5 key frames, decodes clean |
| HEVC 4K, 120 frames, CBR 20 Mbit/s | 3840x2160, 120 frames, decodes clean |
| H.264 + HEVC in two processes at once | both complete |

The HEVC acceptance run moved the `iris` IRQ line from 37318 to 37924
(delta 606) on `/proc/interrupts` line 229, so the work is on the hardware
encoder and not in a copy path.

### Host tests

`test/encode-session-test` gained two cases: three pictures in flight with
every completion withheld (each ticket claims its own access unit, a later
ticket waits through the middle one, and a zero timeout consumes nothing),
and a picture still outstanding when the context is destroyed, which the
drain resolves for its ticket rather than dropping.

## Encoder `REQUEST_SYNC_FRAME` patch: applies and builds, not loaded (2026-10-22)

The driver side of a direct IDR request is in the same uncommitted tree as
the pipelined encode above: `configure()` writes
`V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` once and records whether the node took
it (`encode idr lever=sync-frame|intra-period`), a requested picture then
writes that control and leaves the GOP alone, and a kernel without it keeps
the intra-period lever and the parameter-set prefix from "On-demand IDR by
moving the intra period". `test/encode-session-test` covers both shapes
(`test_on_demand_idr_by_sync_frame` for the direct control,
`test_on_demand_idr` for the GOP lever); `meson test` is 18/18 and
`sh test/run-static-checks.sh` passes.

`packaging/kernel/0003-media-iris-encoder-request-sync-frame.patch` is the
kernel half: the encoder-only rebase of upstream
`6f62dcefd249` (Wangao Wang) onto `sheng-7.2.6` (`42f3b40c702a`). It adds
`REQUEST_SYNC_FRAME` to `enum platform_inst_fw_cap_type`, the
`HFI_SYNC_FRAME_REQUEST_*` enum and `HFI_PROP_REQUEST_SYNC_FRAME
(0x03000145)` to `iris_hfi_gen2_defines.h`, both directions of the
control/capability map in `iris_ctrls.c`, `iris_set_req_sync_frame`, and one
entry at the end of `inst_fw_cap_sm8550_enc` (`CAP_FLAG_INPUT_PORT |
CAP_FLAG_DYNAMIC_ALLOWED`). The upstream commit also adds the control to the
gen1 encoder table and patches `iris_hfi_gen1_command.c`; this branch has no
gen1 encoder on the platform, so neither is carried.

### What was checked, on the builder

A pristine `42f3b40c702a` worktree, `git am` of 0001, 0002 and 0003 in order
applies all three with no fuzz and no rejects. With the three applied, a
module-only build (`make ARCH=arm64 LLVM=1 HOSTCC=gcc HOSTLD=ld
KBUILD_MODPOST_WARN=1 KCFLAGS=-mbranch-protection=pac-ret+bti
M=drivers/media/platform/qcom/iris modules`, clang 21.1.8, the host's
`.config`) links `qcom-iris.ko`, `llvm-nm` lists `iris_set_req_sync_frame`,
and `llvm-readelf -n` reports `aarch64 feature: BTI, PAC`. The module is
2 216 bytes larger than the same tree built without 0003 (7 726 968 vs
7 724 752), which is the control's table entry and setter. Counts on the
patched tree: `cap_id = REQUEST_SYNC_FRAME` 1 in `iris_hfi_gen2.c` and 0
elsewhere, `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` 2 in `iris_ctrls.c` (one
per map direction) and 0 in `iris_venc.c`/`iris_vidc.c`, `cap_id =
DEBLOCK_MODE_H264` still 1.

The control is created from the capability table: `iris_ctrls_init()` walks
`fw_caps[]`, calls `iris_get_v4l2_id()` and `v4l2_ctrl_new_std()`, and
`V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` is a button
(`v4l2-ctrls-defs.c`, `V4L2_CTRL_TYPE_BUTTON`, `EXECUTE_ON_WRITE`), so the
core runs `s_ctrl` on every write rather than only on a value change. A
button's value is always 0, so the driver's per-picture `S_CTRL(id, 0)` is
the same write every time.

### What is not evidence

No module was installed and no boot was made: the tablet still runs
`qcom-iris.ko.zst` `4e29b74d…` without this patch, and
`v4l2-ctl -d <encoder> --list-ctrls` there still shows no
`force_key_frame`. So the `sync-frame` path has unit coverage and a compiled
kernel half, not a hardware run. Installing it means replacing the module
and rebooting, which needs the target's owner; until then the driver's
probe finds no control and takes the `intra-period` path, which is the one
the records above measured.

## `REQUEST_SYNC_FRAME` on the target: a direct IDR request (2026-10-22)

The kernel half from the section above was built, installed and booted, and
the driver-side probe now finds the control. This replaces the intra-period
lever on the target: `-force_key_frames` reaches firmware as
`HFI_PROP_REQUEST_SYNC_FRAME` instead of two `GOP_SIZE` writes, and the
driver adds no parameter sets of its own.

### Build and install

- Source `e9a9a2f` (clean tree), tracked-source SHA-256
  `d2efb19f27ce92bbda2c3eedfe57daffc130471670559c5da47ce0ba4c8bf6d0`.
- Driver `build/src/v4l2_drv_video.so`, SHA-256
  `8c2423e7f786a45dc2a10bc8c1c8d1e3bcfa2f610fe0a6cf42dcb87ec4d09823`,
  deployed with `test/remote/deploy.sh` to the build tree
  (`LIBVA_DRIVERS_PATH`), not the installed package.
- Module built from `42f3b40c702a` + 0001 + 0002 + 0003, target
  `/boot/config-7.2.6-sm8550-g42f3b40` (`CONFIG_LOCALVERSION=""`,
  `LOCALVERSION_AUTO` off, so `localversion` carries
  `-sm8550-g42f3b40-iris1`), `Module.symvers` derived from the running
  kernel's `/proc/kallsyms` so `depends:` matches, no `KCFLAGS` override
  because this kernel sets `CONFIG_ARM64_BTI` without
  `CONFIG_ARM64_BTI_KERNEL`. `vermagic`
  `7.2.6-sm8550-g42f3b40-iris1 SMP preempt mod_unload aarch64`, note
  `aarch64 feature: PAC` (the stock module's note is the same), compressed
  `qcom-iris.ko.zst` SHA-256
  `802c40c0269fe007e0f2d3fb54ad4dfab8d3089d0fd465f3996130f4d2b0a853`.
- Boot ID `8a7cc414-751d-44ec-8e20-fe89dd676ab6` (the stock module's boot
  was `fd1b2389-…`). Decoder `/dev/video1`, encoder `/dev/video2` (resolved
  by `iris_driver`; the numbers moved at this boot).
- `v4l2-ctl -d /dev/video2 --list-ctrls` now lists
  `force_key_frame 0x009909e5 (button) : value=0 flags=write-only,
  execute-on-write`; the encoder reports 75 controls. `display_delay` is
  still on the decoder and the six loop-filter controls on the encoder, so
  0001 and 0002 survived the rebuild.

### Lever in force

`V4L2_VA_TRACE=1` on a 20-frame H.264 encode prints
`encode idr lever=sync-frame` before `encode open`, so `configure()`
probed the control and took the direct path.

### Requested IDR positions

1280x720 at 24 fps, `-g 60 -async_depth 4`, 130 frames, CQP 24,
`ffprobe -show_entries frame=key_frame`:

| Request | IDR positions | Frames | Decode |
| --- | --- | --- | --- |
| none | 0, 60, 120 | 130 | ok |
| `expr:eq(n,24)` | 0, 24, 84 | 130 | ok |
| `expr:eq(n,10)+eq(n,40)` | 0, 10, 40, 100 | 130 | ok |
| `expr:eq(n,60)` (a firmware boundary) | 0, 60, 120 | 130 | ok |

The positions and the re-anchoring match the intra-period record above
exactly, which is the point: the lever changed, the schedule did not.

### What the requested IDR carries

The first access unit of `expr:eq(n,24)` is NAL types 7, 8, 5 (SPS, PPS,
IDR slice): the firmware wrote the parameter sets itself, as it does for
its own sync frames. The first unit of the second H.264 segment (cut at a
requested IDR) is 9, 7, 8, 5 (AUD, SPS, PPS, IDR), and the segment's first
frame is a key frame. The driver's prefix step finds the sets already in
the access unit and adds nothing, so no duplicate SPS/PPS appears.

### Segments

24 fps, `-g 60 -force_key_frames expr:gte(t,n_forced*1) -async_depth 4
-f segment -segment_time 1 -segment_format mpegts`, 240 frames: H.264 and
HEVC each write 10 segments, every one decodes with `ffmpeg -f null -`,
and every one starts on a key frame.

### Pipelining and decode

The same build, 1920x1080 HEVC 600 frames CBR 12 Mbit/s `-g 60`, three
runs each: depth 1 3.597 / 3.631 / 3.134 s, depth 4 1.019 / 1.016 /
1.004 s. The depth-1 and depth-4 bitstreams are byte-identical
(`b437ff325d8f300c0d7373022f03be2a999eae5a4f48a52de4a575caf627e32d`),
read back as 1920x1080 / 600 frames, and decode clean.

The decode matrix on the new module is unchanged: H.264, VP9, HEVC and AV1
all `PASS … content=frame-md5` with `timeouts=0 drops=0` (AV1
`submitted=25 capture=25 noshow=8`).

Iris IRQ delta over the IDR and segment runs: 1829.

### Rollback

`<scratch>/kernel-iris-syncframe/` on the target holds the stock module
(`qcom-iris-stock.ko`, `8e8487ef…`, the decompressed file), the installed
stock `.zst` (`4e29b74d…`), the new module, the target `Module.symvers` and
`stock.sha256`. `rollback.sh` restores the stock `.zst` and re-runs
`depmod`; a reboot is required either way.

## AV1 film grain: refused at the VA layer, before hardware (2026-10-22)

Source `97ebadd` plus the working-tree change that adds the timestamped-error
branch, driver build tree only, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`8a7cc414-751d-44ec-8e20-fe89dd676ab6`.

### What was asked

AV1 film grain synthesis is the one decode feature the translator still
refuses. This run establishes where the refusal happens, and whether the
firmware would honour the parameters if it were passed through.

### The stream

`ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 60 -c:v
libaom-av1 -cpu-used 8 -crf 40 -aom-params film-grain-test=8`, 275 694
bytes, 60 frames. A software decode writes 60 frame MD5s
(`av1-grain-roundtrip/grain-sw.md5`).

### The refusal

`ffmpeg -hwaccel vaapi … -vf hwdownload,format=nv12,format=yuv420p` fails
with `endPicture: film grain parameters are not reconstructed`
(`VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE`, status 15). The trace shows the
first picture submitted and `va end_picture ctx=6 surface=7 deferred`, then
the second picture rejected at `endPicture`. No Iris session ever sees a
picture with grain parameters, so this is the translator's check in
`src/codec/av1_obu.cc` (`apply_grain`), not a firmware capability result.
The hardware comparison file is empty (0 of 60 frames).

### Why the check is there

The driver rebuilds the AV1 frame header from VA decode parameters rather
than forwarding the client's bitstream. `VAFilmGrainStructAV1` carries the
synthesis parameters, so the header could be rebuilt with them, but the
kernel would not act on them: `FILM_GRAIN` is a `CAP_FLAG_VOLATILE`
capability whose value the kernel takes from the firmware's
`HFI_PROP_AV1_FILM_GRAIN_PRESENT` reply (`iris_hfi_gen2_response.c`) and
sends back at `streamon`. There is no V4L2 control that sets it from
userspace, so the value is whatever the firmware reported — on this node, 0.
Advertising grain support would promise a picture the firmware will not
produce.

### What is not evidence

This run does not show the firmware failing to apply grain, because no
stream with grain ever reached it. A negative firmware result needs the
kernel-side control described in `docs/architecture.md` §1, and that is a
separate change with its own hardware gate.

### Artifacts

`<lab-root>/artifacts/av1-grain-roundtrip/` on the target:
`source.webm`, `grain-sw.md5` (60 frames), `grain-va.md5` (empty), and
`grain-va.log` with the trace above.

## Corrupt HEVC: the firmware's error buffer names nothing (2026-10-22)

Same build and boot as the film-grain record above. The target runs the
module built from 0001–0003, which does **not** carry the error-timestamp
patch; this run is the evidence for adding it.

### Provoking a corrupt frame

Four attempts on `sintel-1080p30-hevc-b3.mp4` (720 frames, HEVC, 25 184 804
bytes), all decoded with `-hwaccel vaapi -hwaccel_output_format vaapi`:

| Damage | Result |
| --- | --- |
| 4 KiB flipped at 60 % of the file | 699/699 frames, `errors=0` |
| 64 KiB zeroed at 60 % | 699/699, `errors=0` |
| 40 slice payloads zeroed | 652/652, `errors=0` |
| 40 slice headers overwritten | 652/652, `errors=0` |
| stream cut mid-NAL at the 100th picture | 178/179, `errors=2` |

The firmware conceals damage inside slice data and reports nothing, so only
the truncation produced an error completion.

### What the driver saw

```
submit token=179 bytes=381 output=0
va destroy_context id=6 pending=2
drain begin reason=finish pending=2
capture token=178 index=13 flags=0x4021 publish=direct
capture error index=4 flags=0x4049
capture empty index=14 flags=0x104001 last=1
drain lost token=179
session finish submitted=179 completed=178 errors=2 drops=0 timeouts=0
```

`0x4049` is ERROR | KEYFRAME | PFRAME: the firmware flagged the picture, but
the kernel zeroed the timestamp (`iris_buffer.c`, the ERROR branch of
`iris_vb2_buffer_done()`), so the completion carried no token. The driver
counted it as a marker and left the target pending; only the drain at end of
stream failed it, as `drain lost`. A client that syncs that surface before
submitting more input waits out the sync bound and gets `TIMEDOUT`, not
`DECODING_ERROR`.

### Fix and its coverage

`packaging/kernel/0004-media-iris-preserve-timestamps-on-error-buffers.patch`
(strongtz, Xilin Wu) copies `buf->timestamp` onto the error completion, and
`Session::handle_capture_returns` now fails the matching target as soon as
the completion is dequeued. `meson test` covers both shapes: a timestamped
corrupt completion fails its surface without a drain, and a timestamped
hidden AV1 picture is still published. On the old driver the first test
reports `wrong status 38 (surface sync timeout)`.

### What is not evidence

The patch has **not been built or loaded on the target**. This record shows
the failure mode it fixes, not the fixed behavior on hardware; that needs a
module rebuild from all four patches and the truncation clip above.

## Error-completion timestamps on the target (2026-10-22)

The record above ("Corrupt HEVC: the firmware's error buffer names nothing")
is the failure it fixes. This run closes it on hardware.

### Build and install

- Source `8dd655e` plus the test fix that follows it, kernel tree
  `42f3b40c702a` with all four patches applied (`git apply`, no fuzz).
- Built on the host against the target's `/boot/config-7.2.6-sm8550-g42f3b40`,
  `localversion` = `-sm8550-g42f3b40-iris1`, `Module.symvers` derived from
  `/proc/kallsyms`, `HOSTCC=gcc HOSTLD=ld`, no `KCFLAGS`.
- `qcom-iris.ko` 8 896 688 bytes, SHA-256
  `02723c5cd09f948a1f5b7888b3f4f821a06d2d194b49a23419a450f1daea79c3`;
  compressed `qcom-iris.ko.zst` SHA-256
  `9b16a40179a08e414d0f4bb38ea54f5bb12ada2b9ce176f0dc6b3b602e7bc83d`;
  vermagic `7.2.6-sm8550-g42f3b40-iris1 SMP preempt mod_unload aarch64`,
  note `aarch64 feature: PAC` (same as the stock module).
- Installed with `<scratch>/kernel-iris-syncframe/install.sh`; the previous
  module (0001–0003, `802c40c0…`) is kept as `qcom-iris.0003.ko.zst`.
- Boot ID `40916c73-1560-484d-bd2e-82fed9bab285`. Decoder `/dev/video0`,
  encoder `/dev/video1` at this boot.
- All four patches are live: the decoder lists two `display_delay`
  controls, the encoder lists the six loop-filter controls and
  `force_key_frame`, and a 20-frame H.264 encode prints
  `encode idr lever=sync-frame`.

### The clip

`trunc.hevc`: `sintel-1080p30-hevc-b3.mp4` converted to Annex-B and cut
mid-NAL at the 100th picture (7 761 287 bytes). `midcorrupt.hevc`: the same
cut followed by the stream resumed at the 13th CRA (17 748 348 bytes), so
valid pictures follow the corrupt one.

### Before and after, same clip

| Driver / module | `trunc.hevc` | `midcorrupt.hevc` |
| --- | --- | --- |
| 0001–0003 module, driver `97ebadd` | `capture error index=4` (no token), then `drain lost token=179`; the surface waited for the drain | not run |
| four-patch module, driver `8dd655e` | `capture token=179 … flags=0x4049 publish=error`; `errors=1` | `capture token=179 … flags=0x4049 publish=error` at `t=592`, `submit token=180` immediately after, `errors=1`, `completed=495` |

The timestamp the kernel now keeps is what lets the session match the
completion to its target: the failed surface is marked at dequeue instead
of at the end-of-stream drain, and decoding continues with the pictures
after it.

### Regression sweep

- `meson test -C build` on the target: 18/18.
- Decode matrix: H.264, VP9, HEVC and AV1 all `PASS content=frame-md5`,
  `timeouts=0 drops=0`; AV1 `submitted=50 capture=50 noshow=23`, which is
  the hidden-picture path the new branch restructures.
- The mid-stream run above submits and completes 495 of 496 pictures.

### A test bug found on the way

`test/encode-params-test.cc` passed `h264_sequence()` as a temporary into
`h264_input()`, which stores a pointer. The host happened to read zeros
from the dead object; the tablet read `bit_depth_luma_minus8 = 2` left by
the previous case and aborted on "10-bit encode is not supported". The
sequence and picture buffers are now locals in every affected case.

### Rollback

`rollback.sh` restores the stock module; `cp qcom-iris.0003.ko.zst` over
the installed file restores the previous patched module. Both need a reboot.

## Against libva-v4l2 (Radxa), on the same tablet (2026-10-22)

libva-v4l2 (`radxa-pkg/libva-v4l2` 1.0.0, the Radxa sibling backend) publishes
throughput for SC8280XP on a Radxa Dragon Q8B. Those numbers cannot be
read against this tablet's: a different SoC, a different Iris generation
and a different firmware build. The comparison below runs both drivers
on sheng instead, in the same boot, through the same FFmpeg.

### Setup

- Source `972e285` (`feat/encode-entrypoints`), driver
  `build-def/src/v4l2_drv_video.so` SHA-256
  `f3b37e6ad91b76b5ba1b9ec20af055f9494594fec4207a6638313558a3239f24`,
  built on the target with `buildtype=debugoptimized`.
- libva-v4l2 at tag `v1.0.0` (`0e4ce1e`), built on the target with
  `buildtype=release` and its default `-Dfastcv=disabled`; SHA-256
  `798cfd62a91c51be733aa8f28b98b1289a30dd7b0cca4d1c84cda7b6efac7937`.
  Its own release packages disable FastCV the same way, and sheng's CDSP
  cannot open a FastRPC session, so neither side has the VPP.
- FFmpeg 8.1.3, libva 2.23.0, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot
  `40916c73-1560-484d-bd2e-82fed9bab285`. Decoder `/dev/video0`, encoder
  `/dev/video1`, both resolved by the `iris_driver` name.
- Each driver was selected with `LIBVA_DRIVER_NAME=v4l2` and its own
  `LIBVA_DRIVERS_PATH`; neither was installed. Three runs per cell, mean
  reported, in both the decode and the encode tables. Every row moved
  the Iris IRQ counter (583 to 1807 per run), so none of them fell back
  to software.
- Decode clips: `testsrc2` at 720p, 1080p and 4K, 600 or 300 frames,
  H.264 from `libx264 -preset ultrafast -qp 20 -bf 0` and HEVC from
  `libx265 -preset ultrafast -crf 22 keyint=60`, plus VP9
  (`libvpx-vp9`) and AV1 (`libsvtav1`) at 1080p. The real-content rows
  are the tracked Sintel re-encodes already listed above.

### Decode, synthetic clips (frames/s, no download)

| Clip | This driver | libva-v4l2 | libva-v4l2 vs this |
| --- | ---: | ---: | ---: |
| H.264 720p | 907 | 836 | -8% |
| H.264 1080p | 760 | 681 | -10% |
| HEVC 720p | 720 | 686 | -5% |
| HEVC 1080p | 523 | 464 | -11% |
| H.264 4K | 339 | 221 | -35% |
| HEVC 4K | 201 | 120 | -40% |
| VP9 1080p | 571 | 524 | -8% |
| AV1 1080p | 692 | fails | — |

The gap widens with frame size and has this driver ahead on both codecs,
which is the opposite of what the HEVC slice rewrite would predict: both
drivers re-emit HEVC slice headers there, and neither rewrites H.264
slices (both copy them verbatim). The cause was not isolated. What the
table supports is narrower than a mechanism: on this firmware this
driver keeps more of a 4K stream's time off the CPU than libva-v4l2 does, and
the difference shrinks to single digits at 1080p and below. The 4K HEVC
runs are the noisiest cell (libva-v4l2 2.03 to 2.78 s, this driver 1.45 to
1.59 s); even libva-v4l2's fastest run is slower than this driver's slowest.

libva-v4l2 advertises no AV1 profile, so `-hwaccel vaapi` on the AV1 clip
fails at `av1 @ … Failed setup for format vaapi` before a single picture
is submitted (exit 69, IRQ delta 0).

### Decode, real content with B-frames (frames/s)

| Clip | This driver | libva-v4l2 | libva-v4l2 vs this |
| --- | ---: | ---: | ---: |
| H.264 High, 2 B-frames, 1080p24 | 311 | 311 | -0.0% |
| HEVC Main, 2 B-frames, 1080p24 | 492 | 493 | -0.2% |
| HEVC Main10, 1080p | 457 | 455 | +0.5% |
| VP9 alt-ref, 1080p | 150 | 150 | +0.2% |

On reordered, 10-bit and alt-ref content the two drivers are the same
within run-to-run noise: what is left is firmware time, and neither
driver's bookkeeping shows through. The synthetic-clip gap is therefore
a cost on the path this driver takes and libva-v4l2 does not, not a general
decode difference.

Frame exactness was checked for the 1080p H.264 and HEVC clips by
comparing a `framemd5` of `hwdownload,format=nv12` against the software
decode. Both drivers MATCH on both codecs.

### Encode, at settings that produce the same bitstream

CQP removes rate control from the picture, so the two drivers can be
compared on output rather than on a rate target they may pursue
differently. At `-rc_mode CQP -q:v 24` and `-q:v 30`, 1080p, 300
frames, the H.264 streams are **byte for byte identical** between the
two drivers, and so is the `-q:v 24` HEVC stream and the same setting on
the Sintel clip. Both sides therefore reach the firmware through the
same encode contract; whatever differs below is not the coded core.

Throughput at that setting, 300 frames, `-async_depth 1` and `4`, three
runs per cell, mean:

| Clip | This driver d=1 | libva-v4l2 d=1 | This driver d=4 | libva-v4l2 d=4 |
| --- | ---: | ---: | ---: | ---: |
| H.264 1080p | 235 fps | 196 fps | 439 fps | 424 fps |
| HEVC 1080p | 249 fps | 208 fps | 453 fps | 423 fps |
| H.264 4K | 85 fps | 74 fps | 116 fps | 110 fps |
| HEVC 4K | 99 fps | 83 fps | 121 fps | 119 fps |

This driver is ahead in every cell: 13 to 17% at depth 1, and 2 to 7% at
depth 4, where the two converge because the firmware, not the driver, is
the constraint. A single earlier run had libva-v4l2 10% ahead at depth 4 on
1080p; the three-run repeats above do not reproduce it (libva-v4l2 0.65 to
0.80 s against this driver's 0.53 to 0.78 s), so that reading was
noise. Both drivers scale the same way, and this driver's own pipelining
record (depth 1 to 4, 190 to 374 fps, byte-identical output) is above.

### Encode, rate control: an open difference

At a rate target the two drivers do not agree, and the gap is on
libva-v4l2's side of the comparison:

| Request | This driver | libva-v4l2 |
| --- | ---: | ---: |
| CQP `-q:v 24` | 3045 kbit/s | 3045 kbit/s (identical bytes) |
| CBR 4 Mbit/s | 4034 kbit/s | 741 kbit/s |
| CBR 24 Mbit/s | 21274 kbit/s | 880 kbit/s |
| VBR 4 Mbit/s | 9780 kbit/s | 808 kbit/s |

600 frames of `sintel-1080p30-h264-b3.mp4` at 30 fps; the kbit/s figure
is bytes x 8 / 20 s. This driver tracks the request; libva-v4l2 does not move
with it. The same 4 Mbit/s request on `testsrc2` gives 4034 against
1854195 bytes over 20 s, i.e. the same shape.

What was ruled out on libva-v4l2, each by rebuilding it and re-measuring:

- **Its own values.** With `IRIS_VAAPI_DEBUG=1` it reports
  `bitrate=4000000 peak=4000000 mode=2 (CBR)`, so the client's request
  reaches it intact.
- **The GOP control.** It writes `GOP_SIZE = INT_MAX` and requests
  keyframes through VA instead. Replacing that with 59 left the output
  at 741 kbit/s.
- **The frame rate.** It derives the rate from VA's sequence parameters
  (24/1 for this clip) rather than from the stream rate. Re-encoding the
  source at 30 fps and feeding that changed it to 669 kbit/s, not to
  4000.
- **The header mode.** Adding `HEADER_MODE = JOINED_WITH_1ST_FRAME` to
  this driver left this driver's output unchanged at 4034.
- **QP bounds.** Pinning 1..51, and passing `-qmin/-qmax` through
  FFmpeg, left both drivers unchanged.
- **The control list.** A scratch copy of this driver with libva-v4l2's exact
  control list (GOP `INT_MAX`, header mode, min/max QP) still produced
  4039 kbit/s.

So the difference is not in the control writes either driver makes, and
the root cause was not isolated. It is recorded as an open item rather
than attributed. The observation that stands: on this target, at these
settings, this driver's rate control follows the client's request and
libva-v4l2's does not. One shape is worth checking first in any follow-up:
libva-v4l2's output at every target lands near 700 to 900 kbit/s, which is
close to what a fixed QP near its default 20 would give on this content,
so a firmware that ignores its rate-control property while honouring
explicit QP would look exactly like this. That is a hypothesis, not a
finding; it was not tested.

### What this comparison does not show

- It does not reproduce libva-v4l2's published SC8280XP numbers, and does not
  claim to: the SoC, the Iris generation and the firmware differ.
- It says nothing about Chrome, mpv, VLC or GStreamer. Those runs are in
  the client-coverage section above and have no libva-v4l2 counterpart here.
- The `-Dfastcv=disabled` build is libva-v4l2's own default and the only one
  obtainable on this tablet, but it is not the configuration its README
  describes when it lists FastCV VPP as a feature.
- Three runs per cell throughout. The encode cells at depth 4 still
  spread by up to 0.25 s run to run, so the 2 to 7% advantage there is
  the shape of the result, not a precise figure; the rate-control gap
  (5x) and the depth-1 encode gap (13 to 17%) are far outside that
  spread.
- The VP9 and AV1 rows are one clip each at one resolution. The VP9 row
  shows the two drivers agree where both decode; the AV1 row shows only
  that libva-v4l2 has no AV1 entrypoint on this node, not how an AV1
  implementation of its own would compare.

## AV1 Profile 0 at 10-bit, and the browser matrix beside libva-v4l2 (2026-10-22)

Source `72873c8` plus the working-tree change that adds
`p010_decode_profile()`, driver build tree only
(`build-def/src/v4l2_drv_video.so`, sha256
`00e60b69eafa2e80…` after the change, `f3b37e6ad91b…` before it), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot ID
`40916c73-1560-484d-bd2e-82fed9bab285`, decoder `/dev/video0`, encoder
`/dev/video1`. Chrome 152.0.7977.82. Neither driver is installed; each row
selects its bytes with `LIBVA_DRIVERS_PATH`.

### Why AV1 10-bit was missing

The capability table said "10-bit and 12-bit (only AV1Profile0 is
advertised)". That was not a hardware limit. The firmware decodes 10-bit
AV1 on its own: a GStreamer `v4l2av1dec` run of the same clip negotiated
`P010_10LE` and wrote 23 frames of 640x360 P010, with no `unsupported
content` in dmesg. The kernel's 8-bit subscribe-params check
(`iris_hfi_gen2_response.c`, `sub_depth != BIT_DEPTH_8`) does not fire,
because `iris_hfi_gen2_set_bit_depth()` always sends `BIT_DEPTH_8` and the
firmware derives the depth from the stream.

The driver was the blocker, twice over:

- `getConfigAttributes` offered `VA_RT_FORMAT_YUV420_10` only for
  `HEVCMain10` and `VP9Profile2`. A client asks for 10-bit AV1 surfaces
  through that attribute, so no client could create them.
- `querySurfaceAttributes` listed `P010` under the same two profiles only.
  FFmpeg picks its surface format from that list and logged
  `Picked nv12 as best match for yuv420p10le`.

With either one missing the failure is the same and looks like a firmware
fault: `capture format rejected wanted=NV12 granted=P010`, then
`decoder CAPTURE layout unsupported` at `endPicture`. The firmware had
already chosen P010 from the bitstream and refuses NV12 afterwards, so
the session cannot recover — the depth has to be right before the first
access unit.

### The change

One predicate, `p010_decode_profile()`, now answers "does this profile
decode into P010" for the config attributes, the surface attributes and
the capability table. `AV1Profile0` is in it. `test/iris-codec-matrix-check.sh`
asserts that the table's `capture_formats` and that predicate agree in
both directions, so the next profile cannot drift from the document.

### Frame MD5, 10-bit AV1

`av1-640x360-10bit.mp4` (libsvtav1, 30 frames, `profile=Main`,
`pix_fmt=yuv420p10le`), `hwdownload,format=p010le` against libdav1d:

| Path | Frames | MD5 equal |
| --- | --- | --- |
| This driver, VA-API, P010 | 30 | 30/30 |

### Browser acceptance, this driver versus libva-v4l2

Same harness (`test/iris-browser-acceptance.sh`), same clips, 20 s window,
build-tree driver on both sides. libva-v4l2 is the Radxa backend, 1.0.0
built on the target (`<scratch>/piano-bench/build`, sha256 `798cfd62a91c…`; the
scratch directory keeps the name the run used).

| Clip | This driver | libva-v4l2 |
| --- | --- | --- |
| H.264 High 1080p, B-frames | `VaapiVideoDecoder`, platform, 482 frames, 0 dropped | same, 481 frames, 0 dropped |
| HEVC Main 1080p | `VaapiVideoDecoder`, platform, 483 frames, 0 dropped | same, 482 frames, 0 dropped |
| HEVC Main10 1080p | `VaapiVideoDecoder`, platform, 482 frames, 0 dropped | video element error 4, no decoder, node never opened |
| VP9 Profile 0 1080p | `VaapiVideoDecoder`, platform, 483 frames, 0 dropped | same, 482 frames, 0 dropped |
| AV1 Main 1080p | `VaapiVideoDecoder`, platform, 599 frames, 0 dropped | `Dav1dVideoDecoder`, not platform, node never opened |
| AV1 Main 10-bit 640x360 | `VaapiVideoDecoder`, platform, 593 frames, 0 dropped | `Dav1dVideoDecoder`, not platform, node never opened |

Every row that passed also shows a Chrome GPU process holding the resolved
Iris node (`--type=gpu-process` in `iris-node-holders.txt`), and a boot ID
unchanged across the run. The AV1 rows on libva-v4l2 fall back to software
because libva-v4l2's `vainfo` lists no `VAProfileAV1Profile0`; the HEVC Main10
row is a harder failure, the element itself errors and nothing is decoded.
Both were re-run once to confirm.

### What this does not show

The 10-bit AV1 clip is 640x360 and 30 frames. It proves the depth
plumbing, not throughput; a 1080p 10-bit AV1 run is not in this record.
Chrome's P010 surfaces were confirmed from the driver trace
(`fourcc=P010` for every `va create_surfaces` on that row), not from a
pixel comparison against software output, which Chrome does not expose.

### Film grain stays refused, and it is a kernel limit

A 1280x720 60-frame AV1 stream encoded with `-aom-params film-grain-test=8`
is still rejected at `endPicture` (`film grain parameters are not
reconstructed`). The refusal is correct: `FILM_GRAIN` is a
`CAP_FLAG_VOLATILE` capability, its value comes from the firmware's
`HFI_PROP_AV1_FILM_GRAIN_PRESENT` reply, and `v4l2-ctl -l` on the decoder
lists no film-grain control at all — nothing in the V4L2 surface lets
userspace turn it on. Passing the parameters through would advertise a
picture the firmware will not produce. The firmware-path probe agrees in
the weakest possible way: GStreamer's `v4l2av1dec` on the same stream
wrote 44 frames out of 60 and no dmesg line, so the firmware drops
pictures on that stream rather than synthesising grain.

### Artifacts

On the target: `<lab-root>/artifacts/browser-acceptance/` (one
directory per row above, each with `summary.json`, `cdp.json`,
`browser.log` and `iris-node-holders.txt`),
`<scratch>/iris-bench/lab*/artifacts/browser-acceptance/` for the rows that
were run under a scratch lab root, `<scratch>/iris-bench/av110-va.md5` and
`av110-sw.md5` for the frame MD5s, and
`<lab-root>/media/10bit/av1-640x360-10bit.mp4` for the clip.

## VP9 conformance through the driver, first full suite (2026-10-22)

Same build and boot as the section above. `FFmpeg-VP9-VAAPI` over the whole
`VP9-TEST-VECTORS` corpus, one vector at a time, 20 s per vector:

| Total | Passed | Fail | Crash | Timeout |
| ---: | ---: | ---: | ---: | ---: |
| 305 | 175 | 36 | 67 | 27 |

Every non-pass falls into one of five families, and the driver trace
explains three of them:

- **64 × `vp90-2-02` tiny dimensions, below the advertised 96x96.** FFmpeg
  refuses before any VA call: `Hardware does not support image size 8x8
  (constraints: width 96-8192 height 96-8192)`. The bounds come from the
  kernel's CAPTURE enumeration, so these vectors cannot reach this driver on
  this SoC. Checked on 08x08 and 66x66.
- **36 × `vp90-2-14` dynamic resize with frame-parallel tiles, content
  mismatch.** The frames decode (the trace shows two sessions, 432x240 then
  896x480, and every submitted picture completes) but the MD5s differ from
  `libvpx-vp9`. Not classified further here.
- **24 × `vp90-2-21` timeouts** at the 20 s bound, and **3 more** in the
  `05`/`18`/`22` vectors.
- **`vp90-2-10-show-existing-frame`** and **`vp90-2-16-intra-only`** crash;
  the first is the case the translator refuses by design
  (`show_existing_frame` must reuse the VA surface).
- **`vp91-2-04-yuv444`** crashes; 4:4:4 is outside the advertised profiles.

This is a driver-path number, not comparable one-for-one with the
firmware-path `GStreamer-*-V4L2` table above (235/305): the Fluster FFmpeg
wrapper decodes through a different client stack. It is the first full run
of this suite through the driver and replaces the three-vector probe that
was recorded earlier.

Artifact: `<scratch>/iris-bench/vp9-va.md` on the target (per-vector marks), plus
`vp9res-va.md5` / `vp9res-sw.md5` for the resize vector above.

## Chrome's decode path, measured inside the browser (2026-10-23)

The Import policy was qualified with a probe and the browser runs counted
frames; neither said which publish policy a browser decode actually took.
This run answers that from the driver's own trace. Source commit `821c9f2`,
build `build-def`, driver sha256 `33fff189…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `90008d1c-4049…` (the target rebooted
since the previous section), Chrome 152.0.7977.82, 20 s window per clip,
`IRIS_ACCEPTANCE_ENV="V4L2_VA_TRACE=1"`.

| Clip | Per-capture policy | Frames | Dropped |
| --- | --- | ---: | ---: |
| H.264 High 1080p, B=3 | `publish=import` ×678 | 481 | 0 |
| HEVC Main 1080p | `publish=import` ×676 | 481 | 2 |
| HEVC Main10 1080p | `publish=import` ×678 | 480 | 0 |
| VP9 P0 1080p | `publish=copy-gpu` ×671 | 481 | 0 |
| AV1 Main 1080p | `publish=copy-gpu` ×853, `publish=drop` ×13 | 593 | 0 |

What this settles:

- **Chrome does not import its own buffers.** Every surface is created with
  `imported=0 fd=-1` (22 of them), and the driver exports each stable buffer
  once (`persistent=1`) for `vaExportSurfaceHandle`. The import decision is
  therefore made by this driver about its own memory, not by a client
  handing over a DMA-BUF. That is the same arrangement §3.3 describes as
  Chrome pre-exporting its pool, now confirmed from the browser rather than
  from the FFmpeg path.
- **H.264, HEVC and HEVC 10-bit are zero-copy in Chrome.** Every capture is
  attributed to an imported buffer, the CAPTURE queue carries a DMABUF fd
  (`import token=1 index=0 fd=94 …`), and no copy operation appears in the
  trace.
- **VP9 and AV1 are not**, exactly as the capability table says: VP9 by the
  alt-ref 1:2 rule, AV1 by the hidden-picture rule.
- The 13 AV1 drops are the hidden pictures, one per alt-ref interval
  (tokens 61, 123, 185, 247, flags `0x4011`), not errors. The session
  attributes them by decode order and publishes no pixels for them.

One number is not clean: the HEVC 8-bit window dropped 2 frames where the
same clip measured 0 earlier. The build and the boot both changed, so this
is recorded as observed rather than attributed.

Artifacts: `<scratch>/iris-bench/lab-import/{h264,sintel-1080p30-hevc-b3,
sintel-1080p30-hevc10,sintel-1080p30-vp9,hw-av1}/` on the target
(`browser.log`, `cdp.json`, `summary.json`).

## Browser playback on real sites, and an alt-ref regression it caught (2026-10-23)

The install gate ran with the build-tree driver only; the browser matrix after
the install covered local clips. This run adds the missing client: plain
`google-chrome` (no environment, resolving the driver through the
`msm_drv_video.so` alias) playing YouTube and Bilibili, with the driver trace
on.

### Install

`install-system.sh build-def` over the 2026-09-22 package (`1d15de2`,
`df16127c…`). Manifest: source `166eb01`, artifact `33fff189…`, the previous
driver kept at `/usr/local/share/iris-vaapi/backup/`. `vainfo` with no
environment lists `VAProfileAV1Profile0` through the alias. Kernel
`7.2.6-sm8550-g42f3b40-iris1` with all four patches, boot
`90008d1c-4049…`.

| Site | Codec, size | Decoder | Frames / dropped | Per-frame policy |
| --- | --- | --- | --- | --- |
| YouTube `aqz-KE-bpKQ` | AV1 854x480 | `VaapiVideoDecoder`, platform | 600 / 0 | `copy-gpu` ×806 |
| Bilibili `BV1GJ411x7h7` | HEVC Main 640x360 | `VaapiVideoDecoder`, platform | 500 / 0 | `import` ×832 |

### The VP9 regression

The four-video YouTube sweep hit a VP9 clip (`LXb3EKWsInQ`, adaptive, mostly
854x480) where every session logged dozens of
`capture token=… flags=0x4041 publish=error` lines, each followed by a
same-token `publish=drop`: 50 errors / 50 drops per session, reproduced. The
same clip through the previous install (build `df16127c…`, which predates
`8dd655e`) shows none: 1080 `copy-gpu`, one drop.

The mechanism: a VP9 alt-ref access unit is one VA picture the firmware
decodes as two completions under one token — the invisible frame first
(ERROR, no picture-type bit, no payload), the visible frame immediately
after. `8dd655e` fails a timestamped error completion at once; on the 0004
module the invisible completion carries its timestamp, so every alt-ref
failed its target and the real frame behind it was dropped as unnamed. The
session unit test now scripts this two-completion shape
(`test_vp9_altref_superframe_is_one_picture`), and the fix keeps a
completion that has no picture-type bit and no payload pending until the
next one publishes it. A real corrupt picture carries a picture-type bit
(the truncated HEVC here came back `0x4049`, ERROR|KEYFRAME) and still fails
at once; that shape is asserted by the existing corrupt test, now fed with
`V4L2_BUF_FLAG_KEYFRAME`.

After the fix (`66a6a98`, installed): the same VP9 clip logs
`publish=invisible` ×118 and zero errors; the decode matrix passes
unchanged (VP9 `48/48` frame-MD5). Committed as `97dde61`.

### Where the fix was verified

| Run | Driver | VP9 854x480, `LXb3EKWsInQ` |
| --- | --- | --- |
| before | `build 2b860ea8` (= `821c9f2`) | 26 drops in Chrome, 8 token-less errors |
| before | installed `df16127c…` (pre-`8dd655e`) | 1080 `copy-gpu`, 1 drop, Chrome 600/0 |
| after | installed `66a6a98` | 118 `publish=invisible`, 0 errors, Chrome 995/6 |

### The AV1 observation, still open

One `yt-music` run served an AV1 1280x720 rendition whose session logged 136
`publish=error` completions (`0x4051` PFRAME|ERROR ×129, `0x4049` ×7), in
consecutive-token pairs, while Chrome reported 501 frames / 0 dropped. Three
later runs at 854x480 and 1080p show zero errors, and the pre-fix tree at
854x480 shows the same class as 8 token-less `0x4041` errors. The video
plays; nothing is attributed to the fix. It is recorded as an open
rendition-dependent observation, not a conclusion.

Scratch artifacts: `<scratch>/iris-bench/` on the target
(`site/ab-*/browser.log` per run, `vp9alt/` for the local alt-ref probes,
`olddrv/` for the previous driver).

## AV1 loop restoration: the frame header carried the wrong unit (2026-10-23)

The open AV1 observation above had a cause, and it was the driver's OBU
writer, not the firmware or the rendition.

### What the failure looked like

`yt-720.webm` (AV1 1280x720, 5326 frames, 4 tiles, 128x128 superblocks,
loop restoration enabled) through `av1_vaapi` died on its third key frame.
The session logged `capture token=18 flags=0x4049 publish=error` and then a
cascade of `0x4051 publish=error` / `publish=drop`; FFmpeg exited with an
I/O error after 24 of 213 submitted pictures. The same file decoded
correctly with the in-kernel `v4l2av1dec`, so the file and the node were
both fine.

### How it was pinned to one bit

- `V4L2_VA_DUMP` wrote every rebuilt access unit; the failing one (16808
  bytes) failed on its own in both `aomdec` ("Failed to decode tile data")
  and libdav1d, so it did not depend on the frames before it.
- A hybrid stream (rebuilt units up to the key frame, then the source's own
  key frame) decoded; the reverse did not. The defect is inside the
  rebuilt key access unit.
- The four tile payloads were byte-identical to the source
  (`[5224, 4686, 4089, 2766]`), so the tiles and the tile-size prefixes
  were not the problem.
- `ffmpeg -bsf:v trace_headers` on the source and rebuilt key access units
  produced the same fields everywhere except two, both of them expected and
  legal (`use_superres` present because our sequence header enables it,
  `tile_size_bytes_minus_1` = 3 instead of 1). One real difference remained:
  `lr_unit_shift`, source 1, rebuilt 2.
- Clearing that single bit in the dumped access unit made `aomdec` accept
  it.

### The defect

`lr_unit_shift` is coded as `read_increment()`: unary ones from the range
minimum, a zero ends it, and `range_max` ends it without a zero
(`libavcodec/cbs_av1.c`, `cbs_av1_write_increment`; spec 5.9.20). With
64x64 superblocks the range is 0..2; with 128x128 superblocks it is 1..2.
The writer emitted `lr_unit_shift != 0` as one bit in both branches, so
VA's resolved 1 became a coded 2 in 128x128 streams. The decoder then ran
restoration over a 128-pixel unit where the encoder had used 64, and the
tile data it read did not match what it expected.

The 64x64 branch had a second bug of the same kind: it wrote
`lr_unit_shift ? 1 : 0` twice, so 1 and 2 were indistinguishable there too.

### Verification

The first run used the build tree (`build-def`) only; the records below are
from that driver and were then repeated against the installed package (see
"Install" at the end of this section). Source `98167ea`, driver
`build-def/src/v4l2_drv_video.so` SHA-256
`0913ddec2eebe3dd6a0da054e524a225f60dc67ec046454e3ef21abaaaeb0ded`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`90670102-4d45-4b5b-935d-a3dbe97de028`. Node resolved by the `iris_driver`
QUERYCAP name: `/dev/video17` (`Iris Decoder`), encoder `/dev/video18`.
Everything below ran through `-hwaccel vaapi` with `hwdownload,format=nv12`
and was compared frame by frame to a software decode of the same file
(`format=nv12`, so stride cannot account for a difference).

| Clip | Before | After |
| --- | --- | --- |
| `yt-720.webm` AV1 1280x720 | stops at token 18, 24/213 pictures, FFmpeg exit 251 | `submitted=5448 completed=5448 errors=0 drops=0`, 5326/5326 frames MD5-identical |
| `yt-1080.mp4` AV1 1920x1080 | not re-run (same writer path) | `submitted=5471 completed=5471 errors=0 drops=0`, 5326/5326 frames MD5-identical |
| `test/av1-obu-roundtrip.sh` default source | 18/32 bit-exact | 18/32 bit-exact, unchanged |
| `test/av1-obu-roundtrip.sh` on 120 frames of `yt-720.webm` | — | 72 rebuilt / 120 source, all bit-exact |

The last row is the roundtrip test pointed at the real multi-tile stream
(`IRIS_AV1_ROUNDTRIP_SOURCE`), which the 320x180 default source cannot
reach: it uses 64x64 superblocks and has restoration disabled.

`test/av1-obu-test.cc` (new) walks the rebuilt header back in spec order
and fails on three checks if the writer reverts to a boolean coding.

Chrome was re-run on the same clip with the build-tree driver
(`test/iris-browser-acceptance.sh`, `V4L2_VA_TRACE=1`): `VaapiVideoDecoder`,
platform true, 501 frames / 0 dropped, `submitted=716 completed=716
errors=0 drops=0`, all `publish=copy-gpu`, GPU process holding
`/dev/video17`.

### Install and the installed-package recheck

`install-system.sh build-def` over the 2026-09-22 package. Manifest: source
`ab19b8a`, artifact `0913ddec…`, the previous driver kept at
`/usr/local/share/iris-vaapi/backup/`. The installed
`/usr/lib64/dri/v4l2_drv_video.so` is byte-identical to the build-tree one,
and `LIBVA_DRIVER_NAME`/`LIBVA_DRIVERS_PATH` were unset for the recheck, so
libva resolved the driver through `msm_drv_video.so` the way a client that
knows nothing about this project does.

| Clip, installed driver | Session | Frames |
| --- | --- | --- |
| `yt-720.webm` | `submitted=5448 completed=5448 errors=0 drops=0` | 5326/5326 MD5-identical |
| `yt-1080.mp4` | `exit 0` | 5326/5326 MD5-identical |

GStreamer's `vaav1dec` is not evidence for this codec yet: its element syncs
each surface as it goes, which releases the held picture and makes the next
`vaEndPicture` fail with "AV1 stream continued after a picture was released
at download time". The FFmpeg and Chrome paths do not do that. Not a
regression from this change: the same pipeline against the backed-up
previous driver (`66a6a98…`) logs the same 30 refusals, so it is a property
of the held-picture contract and this client, not of the header fix. It
belongs in the record as a client that does not decode AV1 through this
driver.

Artifacts: `<scratch>/iris-bench/av1-720/` on the target
(`hw-all.md5`, `sw-nv12.md5`, `hw1080.md5`, `sw1080.md5`, `rt-real/`,
`browser-av1/`), and
`<scratch>/av1-hdr/` on the builder (the dumped access units, the
`trace_headers` diffs, and the one-bit patch that proved the field).

## AV1 and H.264 resolution change inside one VA context (2026-10-23)

A concatenated stream that switches from 1280x720 to 854x480 mid-file
decoded its first segment and then stalled: every 854x480 access unit was
accepted by the driver, none produced a picture, and the session ended on
`sync timeout token=43 ... action=fail` followed by `session failed: no LAST
after STOP`. The same file decoded fully in the in-kernel `v4l2av1dec`
(GStreamer, 80/80 frames), so the file, the node and the firmware were all
capable of it.

### What the trace showed

`V4L2_VA_TRACE=1` on the failing run:

```
event source-change state=1
session reconfigure 1280x720 ->
drc drain begin pending=2
capture token=43 index=5 flags=0x104041 publish=invisible
drc drain complete last=1
capture configure 896x480 ... visible=854x480
session reconfigure -> 854x480
submit token=44 ...            (six 854x480 access units, all returned clean)
output return index=0 flags=0x4001 token=44
sync timeout token=43 pending=6 queued=32 held=0 action=fail
```

The OUTPUT returns carried no error, so the firmware had consumed the new
segment and chosen not to decode it. Kernel `dynamic_debug` on
`iris_inst_change_state` / `iris_inst_change_sub_state` showed an identical
state machine to GStreamer's on the same file (`0x22` -> `0x66` -> CAPTURE
STREAMOFF -> STREAMON -> `0`), so the DRC steps themselves were right.

### The defect

`configure_capture()` issued `S_FMT(CAPTURE, <GEOMETRY FROM G_FMT>)` on every
bring-up, including the one that follows a mid-stream `SOURCE_CHANGE`.
`iris_vdec_s_fmt` (CAPTURE branch) rewrites `inst->fmt_dst` and `inst->crop`
from that request, replacing the coded size, stride and crop
`iris_hfi_gen2_handle_src_change` had just derived from the bitstream. The
resume then hands the firmware the client's rectangle instead of the
stream's. GStreamer's strace shows the contract: it never calls `S_FMT` on
CAPTURE at all, only `G_FMT` then `REQBUFS`.

The fix reads `G_FMT` and issues `S_FMT` only when the driver's answer is not
the pixel format the client needs (a P010 surface against an NV12 default).

### Why it looked like an AV1 defect

Only the AV1 clip reached it here, and not because AV1 is special: FFmpeg
recreates its VA context at every coded-size change, so a mid-file switch in
most streams never reaches the in-context path at all. H.264 was tried as a
control and turned out not to be one — the H.264 run created two contexts
(`va create_context` ×2, `reconfigures=0`) and therefore tested context
teardown, not this defect. The AV1 run stayed in one context
(`va create_context` ×1), which is why it was the one that stalled. What
separated "AV1 writer" from "session ordering" was the dump: the rebuilt
access units decoded cleanly in software (0 errors), so the bytes were
right and the ordering was not.

### Verification

Source `99dcf30`, driver `build-def/src/v4l2_drv_video.so` SHA-256
`f155e2f51ec28cb70459f3f5460c5cbdd94ed791e60db902de094c916e103566`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`. Node resolved by the `iris_driver`
QUERYCAP name: `/dev/video2` (`Iris Decoder`). Everything below used the
build tree (`LIBVA_DRIVERS_PATH=build-def/src`) through `-hwaccel vaapi`
with `hwdownload,format=nv12`, compared frame by frame to a software decode
of the same file.

| Run | Before `99dcf30` | After `99dcf30` |
| --- | --- | --- |
| `resize-av1.mkv` (40x 720p + 40x 854x480, AV1) | 40/80 frames, `submitted=49 completed=42 timeouts=1` then `session failed`; FFmpeg exit 251 | **not fixed.** 1 of 3 runs: 80/80 frames, `submitted=84 completed=84 errors=0 drops=0 timeouts=0 drains=2`, MD5-identical to software. The other 2: the same 40/80 stall and exit 251 |
| `resize-h264.mkv` (same geometry switch, H.264) | exit 0, 80 frames, two contexts | exit 0, 80 frames; not a control for this defect (see above) |
| `test/iris-matrix.sh` (48 frames per codec) | PASS | PASS: h264, vp9, hevc, av1 all `content=frame-md5` |
| `test/iris-dynamic-resolution.sh` (6 switches) | PASS | PASS: native baseline, VA single-process and fresh-context runs all frame-exact |

The AV1 run leaves the in-context path visible in its own counters:
`session reconfigure 1280x720 ->` at the boundary and `session reconfigure ->
854x480` when the pool is rebuilt, with both segments decoded by the same VA
context (`contexts=1`).

### Correction (same day): the defect is not fixed, it is probabilistic

The first version of this section claimed the run above proved the fix. It
did not: one hardware run was treated as a result. Eight further runs from
the same committed build and the same driver binary
(`f155e2f5…`, unchanged) split as follows.

| Runs | Result |
| --- | --- |
| 3 of 11 | 80/80 frames, exit 0 |
| 8 of 11 | 40/80 frames, `sync timeout token=43`, `session failed: no LAST after STOP`, exit 251 |

What separates them, from `V4L2_VA_TRACE=1` plus an OUTPUT-dequeue probe
(`PROBE out index=… token=…`) and `dynamic_debug` on `iris_state.c`:

- The kernel sub-state machine is identical in both
  (`0x22` -> `0x66` -> CAPTURE `STREAMOFF` -> `STREAMON` -> `0`), so the DRC
  steps this driver issues are right. GStreamer `v4l2av1dec` on the same
  file, same node, same kernel decodes 80/80 in 5 consecutive runs, and an
  ioctl-level strace of its DRC window shows the same order: no `S_FMT` on
  CAPTURE, `G_FMT` only, drain to the boundary `LAST`, `STREAMOFF`,
  `REQBUFS(0)`, `REQBUFS(5)`, `STREAMON`.
- The boundary access unit (token 43, the first 854x480 key frame) completes
  exactly once, as `flags=0x104041` (`ERROR|LAST`, no picture-type bit, no
  payload). In a run that works the *same* token completes twice: that
  marker first, then `0x4009` (KEYFRAME, real pixels) after the rebuilt pool
  is streaming.
- In the failing runs the OUTPUT slot that carried token 43 returns before
  the resume, and a later access unit takes the same slot; in the working
  runs it stays owned by the firmware until after `STREAMON`. Same binary,
  same file, same node, different interleaving.
- The failing runs also see one extra empty `LAST` (`0x104001`, token 0) on
  the rebuilt pool immediately after `STREAMON`, which sets
  `q->last_buffer_dequeued` in vb2 and turns every later CAPTURE `DQBUF`
  into `EPIPE`. The firmware answered the resume by declaring the queue
  finished.

None of that is addressable from this driver: it consumes the boundary
access unit, it decides whether to decode it a second time, and it decides
whether the resumed CAPTURE queue is finished. Only FFmpeg's own context
recreation path (`contexts=2`, which H.264 already takes and which every
FFmpeg build does at a coded-size change) avoids it entirely.

So the `S_FMT` change stays — it is a contract requirement
(`docs/architecture.md` §4) and without it the working runs would decode the
new segment at a geometry the stream never named — but in-context AV1 DRC
stays **unqualified**. `data/iris-codec-capabilities.json` and
`docs/architecture.md` carry that, and the target-side evidence is
`<scratch>/iris-bench/av1-720/` (`out-a.log` success, `out-b.log`
failure, `gst.strace`, `flag-a.log`, `dd-c.dmesg`).

### The installed package, no environment overrides (2026-10-24)

The same commit installed with `scripts/install-system.sh build-def`
(`source_commit=434be71`, `source_dirty=0`,
`artifact_sha256=f155e2f5…`), then run with `LIBVA_DRIVER_NAME`,
`LIBVA_DRIVERS_PATH` and `V4L2_VA_TRACE` all absent from the environment:

```
libva info: Trying to open /usr/lib64/dri/msm_drv_video.so
vainfo: Driver version: Qualcomm Iris V4L2 stateful (libva-v4l2)
      VAProfileH264Main : VAEntrypointVLD
      VAProfileH264Main : VAEntrypointEncSlice
session open node=/dev/video2 mode=decode-order codec=AV01 854x480 au=7077888 output=4
session finish submitted=42 completed=42 errors=0 drops=0 timeouts=0 drains=1 misplaced=0
```

Kernel `7.2.6-sm8550-g42f3b40-iris1`, boot
`ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`, `/dev/video2` `iris_driver` /
`Iris Decoder`, `/dev/video3` `iris_driver` / `Iris Encoder`. This is the
installed package, not a `DESTDIR` staging run and not a build-tree
override.

Artifacts on the target: `<scratch>/iris-bench/av1-720/`
(`resize-av1.mkv`, `resize-h264.mkv`, `hw.md5`, `sw.md5`, `hwmd5.log`).

## Correction (2026-10-24): the in-context DRC defect is in this driver

The section above concluded that the intermittent AV1 resolution-change stall
was "not addressable from this driver" because the firmware decides whether
to decode the boundary access unit a second time. That conclusion was wrong.
The empty `LAST` (`0x104001`) on the rebuilt pool is not the firmware
declaring the queue finished — it is the kernel completing a `CAPTURE` `QBUF`
on the client's behalf, and the driver controls when that `QBUF` happens.

`iris_vb2_buf_queue` (`iris_vb2.c:313-328`) has a branch that fires when a
`CAPTURE` buffer arrives while the instance still carries the DRC boundary
sub-state (`IRIS_INST_SUB_DRC | IRIS_INST_SUB_DRC_LAST`): it does not hand
the buffer to the firmware, it completes it on the spot with
`V4L2_BUF_FLAG_LAST`, payload 0, and calls `v4l2_m2m_mark_stopped`. vb2 then
sets `q->last_buffer_dequeued` (`videobuf2-v4l2.c:853-856`), and every later
`DQBUF` returns `-EPIPE` (`videobuf2-core.c:2011`). The kernel clears that
sub-state in exactly one place — `iris_process_streamon_output`
(`iris_common.c:100-155`) — which is the `CAPTURE` `STREAMON` ioctl.

`enter_streaming()` used to call `pool_.requeue_capture()` first and
`device_->stream(Queue::Capture, true)` second, so the 32 pool `QBUF`s
arrived before the sub-state was cleared. Why only two runs in three failed
rather than all of them: the boundary access unit also completes once as
`ERROR|LAST` (`0x104041`), and that completion takes the error path in
`iris_vb2_buffer_done` (`iris_buffer.c:771-778`) which returns before it
reaches the `V4L2_BUF_FLAG_LAST` test that would set
`inst->last_buffer_dequeued`. The two orders therefore differ only by a few
microseconds of interleaving, which is why `strace` made the run pass (it
slows every ioctl) and why the kernel sub-state log looked identical in both
outcomes (`0x16` -> `0x42` -> `0`).

The fix reverses the two calls: `STREAMON` first, then the pool, for both
Mmap and Import memories. `test/unit/session-test.cc` had asserted the old
order ("the first picture's buffer went in before CAPTURE streamed") and now
pins the new one.

Framemd5 oracle, `resize-av1.mkv` (80 frames, one VA context, one
1280x720 -> 854x480 change), `V4L2_VA_TRACE=1`, build tree
`<lab-root>/build-def/src`:

| Build | Runs | 80/80 frames, exit 0 |
| --- | --- | --- |
| before (`7f5936e`) | 6 | 2 |
| after (`c4e957d`) | 44 | 44 |

The 44 are 6 + 10 + 16 + 12 across four sessions. In a passing run the
boundary token completes as `0x4009` (KEYFRAME, real pixels) after the
rebuilt pool streams, and the extra `0x104001` no longer appears; the single
`0x104001` that remains in a passing log is the terminal EOS drain
(`index=10`).

Regression checks on the same build:

- `test/iris-matrix.sh`: PASS h264, hevc, vp9, av1, all `content=frame-md5`,
  48 frames each, `timeouts=0 drops=0`.
- `test/iris-dynamic-resolution.sh`: PASS h264, `switches=100`,
  `contexts=101`, byte-identical native output and exact MD5 across every
  change.
- `sh test/run-static-checks.sh` PASS; `meson test -C build-def`
  19/19 OK, 0 Fail.

Installed package, no environment overrides (`scripts/install-system.sh
build-def`): `source_commit=ff3269f`, `source_dirty=0`,
`artifact_sha256=ea7030b6…`, installed driver sha256 identical. Eight runs
with `LIBVA_DRIVER_NAME`/`LIBVA_DRIVERS_PATH` unset through `env -u`: 8 of 8
gave 80/80 frames, exit 0, `token=43` completing as `0x4009` after
`capture configure 896x480`. The only `0x104001` in those logs is the
terminal EOS drain at `index=10`.

Both fixes together, installed as a package: `source_commit=3cfaa7e`,
`source_dirty=0`, `artifact_sha256=cb9053e0…`, installed driver sha256 equal
to the build tree. Direct path (no overrides) 8 of 8 runs give 80/80 frames,
each byte-identical to the software reference; Copy path
(`V4L2_VA_PUBLISH=copy`) 6 of 6, likewise byte-identical. `meson test -C
build-def` 19/19 OK.

The A/B sweep that confirmed the ordering fix also surfaced a second defect on
the Copy publish path, which the default Direct path never reaches. Forcing
`V4L2_VA_PUBLISH=copy` made the same file fail at the first post-change
picture with `gpu copy: invalid buffer layout`. `GpuBlitter::copy` built both
the source and destination views from `to.width`/`to.height`, the destination
surface's layout, while `from` is the new-resolution frame; a client surface
still at the old geometry then asks for a source rectangle the source buffer
never had (`gpu_copy.cc:132`). FFmpeg's download path reuses its surface pool
across a resolution change, so this is reachable, and it is reachable in
production: the capability table marks only H.264 and HEVC Import-qualified,
so a pre-exporting client decoding AV1 publishes through Copy.

`copy_pixels()` already intersects the client's rectangle with the buffer's
for the CPU path; `copy()` now does the same — each view is built from its own
layout, and the copy covers `min(from, to)` in each dimension. Same harness,
`V4L2_VA_PUBLISH=copy`, `resize-av1.mkv`:

| Build | Runs | Frames | Against software |
| --- | --- | --- | --- |
| before (`ff3269f`) | 3 | 40 of 80, `invalid buffer layout` | n/a |
| after (`378c113`) | 6 | 80 of 80 | byte-identical, every frame |

A file without a resolution change is unaffected: `h720.mkv` gives 40 of 40
frames on both builds, byte-identical to each other. That output differs from
the software decode of the same file, which is a property of the GPU copy's
colour conversion and not of this change; it is the same on both builds.

Artifacts: `<scratch>/iris-bench/av1-720/` — `f-*.log` and `v-*.log`
passing, `n-*.log`/`q-*.log` the before/after pair with the kernel trace,
`cp-*.log` the Copy path after the fix, `pub-copy-*.log` before it.

## Codec conformance: Fluster on the installed package (2026-10-24)

Fluster 0.8.0 (`pip install --user fluster-conformance`), run against the
installed driver with no `LIBVA_DRIVER_NAME` or `LIBVA_DRIVERS_PATH`. Fluster
reports the backend it found on its own:

```
"VA-API": "Qualcomm Iris V4L2 stateful (libva-v4l2) (runtime 1.23)"
"V4L2 M2M": "Iris Decoder (/dev/video2)"
```

The FFmpeg VA-API wrapper inherits the environment, so the numbers below are
this driver's, not the firmware's through a different client. Source commit
`434be71`, driver SHA-256 `f155e2f5…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `ebe7a2b2-…`.

| Suite | Decoder | Passed | Total | Profile split |
| --- | --- | ---: | ---: | --- |
| `JVT-AVC_V1` | `FFmpeg-H.264-VAAPI` | 72 | 135 | Constrained Baseline 33/33, Main 39/89, Baseline 0/7, Extended 0/6 |
| `JCT-VC-HEVC_V1` | `FFmpeg-H.265-VAAPI` | 141 | 147 | Main 131/135, Main 10 9/11, Main Still Picture 1/1 |

Neither number is a driver defect count. Every failure was classified by
running each vector directly and reading the kernel's own message
(`<scratch>/fluster/h264-failures.csv`, and the same method for HEVC):

H.264, 63 failures:

| Count | Cause |
| ---: | --- |
| 49 | Kernel refuses the stream: `unsupported content, bit depth: 80008, pic_struct = N`, then `state changed from 2 to 5`. All 49 carry `frame_mbs_only_flag = 0` in their SPS (verified with `-bsf:v trace_headers`): the firmware has no field/MBAFF decoding. The driver surfaces the kernel's refusal as an error, which is correct. |
| 7 | Baseline profile, 6 Extended: neither is advertised in `data/iris-codec-capabilities.json`, so FFmpeg never reaches the hardware path. Expected. |
| 6 | Baseline/Extended vectors whose decoded pixels differ from the reference, on a profile this driver does not advertise. |
| 1 | `FM2_SVA_C` could not be probed (no readable elementary stream in the vector directory). |

The 33/33 Constrained Baseline result and the 39 passing Main vectors are
frame-coded streams, which is the shape the firmware supports.

HEVC, 6 failures:

| Count | Vector | Cause |
| ---: | --- | --- |
| 2 | `PICSIZE_A/B_Bossen_1` | 8440 wide, above the node's 8192 limit (`iris_ctrls.c`). |
| 2 | `WPP_D_ericsson_MAIN_2`, `_MAIN10_2` | 64x240, below the 96 minimum. |
| 1 | `VPSSPSPPS_A_MainConcept_1` | The vector switches 352x288 -> 1280x720 in one stream. FFmpeg recreates the VA context (`session open` x2) and both paths decode 2 frames, but the reference MD5 differs; it is the same in-context resolution-change family as the AV1 stall above, not a pixel defect. |
| 1 | `TSUNEQBD_A_MAIN10_Technicolor_2` | `Skipping invalid undecodable NALU` in FFmpeg's own parser before the hardware is reached. |

## Power: saturated decode, software versus this driver (2026-10-24)

Method: battery gauge `voltage_now x |current_now|` at 1 Hz, screen off
(backlight 0, idle 1.4 W), arms interleaved A/B per round so background drift
cancels, first 4 s of each window discarded as the ramp transient, 10 s pre-
and post-baseline. The workload re-decodes the clip back to back so the window
is always full; a paced `-re` run leaves the device mostly idle and buries the
difference in gauge noise (measured: a 30 s paced run gave 218 +/- 62 mW for
the VA arm and 124 +/- 32 mW for software, which does not resolve).

| Workload | VA-API | Software | Difference |
| --- | ---: | ---: | ---: |
| 1280x720, unpaced, n=4 | +1861 +/- 64 mW | +5149 +/- 429 mW | **-3289 mW** |
| 1920x1080, unpaced, n=4 | +2089 +/- 158 mW | +6177 +/- 761 mW | **-4087 mW** |

CPU cost of the same two decodes (`-benchmark`, whole clip, no pacing):
720p VA `utime=0.302s stime=0.183s`, software `utime=2.202s stime=0.146s`;
1080p VA `utime=0.487s stime=0.192s`, software `utime=4.020s stime=0.180s`.

Interpretation: at saturation the decoder offload is worth about 3.3 W at
720p and 4.1 W at 1080p on this tablet. That is the cost of doing the work on
the CPU, not a claim about a browser session: a browser that plays at 30 fps
spends most of the time idle and the saving is smaller and noisier, which is
why the paced numbers above cannot resolve it. The gauge measures the whole
device; there is no per-block telemetry on this SoC.

## Stable-buffer allocation chain (2026-10-24)

The chain in §3.4 — MSM GEM cached-coherent, MSM GEM write-combined, system
DMA heap — had no test. The last rung is only reached when the first two are
refused, and no ordinary run refuses them, so `test/iris-alloc-chain.sh` runs
`test/iris-alloc-probe` under `test/alloc-fault.so`, an `LD_PRELOAD` shim
that returns `EINVAL` (or `ENOMEM`) from the MSM GEM allocations it is told
to refuse and logs every attempt.

Run on the target, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`, source `3cfaa7e` plus the
working-tree change, build tree `build-alloc` (probe
`1c71e250dba9…`, shim `0baefc4243…`), render node `/dev/dri/renderD128`
resolved from the display's DRM render fd. Four modes, each a fresh process:

| Mode | Injected | Resulting origin | Attempts logged |
| --- | --- | --- | --- |
| `no-fault` | nothing | MSM cached-coherent | — |
| `coherent` | coherent → EINVAL | MSM write-combined | `coherent-einval wc-ok` |
| `both` | coherent and WC → EINVAL | system DMA heap | `coherent-einval wc-einval` |
| `coherent-enomem` | coherent → ENOMEM | system DMA heap | `coherent-enomem` |

`coherent-enomem` is the one that pins a correctness rule rather than a
memory preference: `msm_alloc.c` masks only `EINVAL` and `EOPNOTSUPP`, so a
real failure must not be retried write-combined. The log shows no `wc` attempt
at all in that mode.

Every mode also asserted that the buffer covers the CAPTURE layout and that a
pattern written to its chroma plane reads back unchanged across a
`DMA_BUF_IOCTL_SYNC` pair, so a fallback rung is real memory and not just a
successfully returned fd.

Two negative controls, both refused with exit 2 rather than reported as a
pass: an injected mode with no shim loaded, and an injected mode with the shim
loaded but `IRIS_TEST_ALLOC_FAULT` unset. Without them a pass could come from
the fast path happening to work.

This is not a Meson test: every rung but the last needs an MSM render node,
and a host without one cannot run any of it. `meson test` (19/19) and
`run-static-checks.sh` are unchanged by the addition.

## Encode input fence (2026-10-24)

`src/iris/encode.cc` queued a surface's DMA-BUF to OUTPUT with no fence
wait. The kernel attaches nothing of its own to a dmabuf QBUF, so a client
that renders into the surface and only flushes was encoded from whatever the
buffer held before its GPU write landed. Every other path already waits —
`session.cc` before an Import capture buffer is handed to the firmware,
`publish.cc` and `gpu_copy.cc` before a copy — and the encoder was the one
that did not. `submit()` now waits on `POLLOUT` (read *and* write fences,
`dma_resv_usage_rw(true)`) before the session lock, bounded at 2 s, and
traces `encode input fence` when the wait reached 20 ms.

`test/iris-encode-fence-probe` measures the result rather than the call.
Three NV12 stable buffers share a chroma plane and differ in luma: `current`
(the picture to encode), `stale` (the one it replaces), and `staged`, which
starts as a CPU copy of `stale`. On a live session the GPU rewrites `stale`
over `staged` 512 times — same content, so the destination's bytes say
nothing about progress but the queue stays busy — and then blits `current`
into it. `staged`'s memory therefore reads `stale` before the blits land and
`current` after them, which is exactly the question the wait decides.

Run on the target, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`, source `7173756` plus the
working-tree change (copied to the target's `<lab-root>`,
whose checkout is at `3cfaa7e` and was not otherwise updated), build tree
`build-alloc` (probe `1413ac6f9ad4…`), encoder node `/dev/video3`
("Iris Encoder") and render node `/dev/dri/renderD128`, both resolved by
name.

| Run | Fence at submit | Fence after submit | Staged access unit | Verdict |
| --- | --- | --- | --- | --- |
| with the wait, 3 runs | pending | signaled | 169245 bytes, equal to `current` | PASS |
| wait removed, 3 runs | pending | pending | 160574 bytes, equal to `stale` | FAIL |

The negative control is the whole point of the probe: with the wait removed
the firmware encoded the picture as the buffer held it *before* the blit, and
the byte counts name which one it read. The first version of this probe
blitted `current` over `staged` directly and passed both ways — a single
blit drained before the submit was queued, so the comparison could not fail.
A probe that cannot fail is not evidence.

The two reference pictures are encoded from their own configured sessions,
so no picture predicts from another and equal pixels must give equal bytes.
The blit runs inside the same function that then submits, after
`configure()` has returned, so the V4L2 setup cannot absorb the window.

Also on this boot: `ffmpeg` `h264_vaapi` and `hevc_vaapi` at 1920x1080,
120 frames each, both decode clean (`-f null -`) and read back 120 frames at
1920x1080; `iris-va-import-probe` passes all twelve of its checks including
the equal-bitstream comparison from an imported surface; a 60-frame run with
`V4L2_VA_TRACE=1` emitted no `encode input fence` record, i.e. a swscale
upload leaves no pending fence and the wait costs nothing on that path.

Throughput, because a wait on the submit path could plausibly cost some. The
recorded pipelining setting (§ "Pipelined encode", `testsrc2` 1920x1080,
600 frames, CBR 12 Mbit/s, `-g 60`) measured 202 fps at `-async_depth 1` and
370 fps at 4 on this build, against the 190 and 374 recorded for `9bd3dbd`.
An interleaved A/B on one binary (`IRIS_TEST_NO_FENCE_WAIT` removing only the
wait, wait and no-wait alternating, three rounds each) came out inside the
run-to-run spread:

| Round | wait d=1 | no wait d=1 | wait d=4 | no wait d=4 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 3.713 s | 3.923 s | 1.901 s | 1.930 s |
| 2 | 4.174 s | 3.950 s | 1.880 s | 1.894 s |
| 3 | 3.976 s | 3.810 s | 1.892 s | 1.898 s |

The wait is a `poll()` that returns immediately when no fence is pending, and
a swscale upload leaves none, so this is what it should cost. The installed
package (`libva-v4l2-iris-0.1.0-1.fc44`) measured 4.131 s and 1.898 s on the
same two depths, so the build tree is not the reason for any difference.

Not a Meson test: it needs the encoder node and a hardware GPU renderer, and
its verdict is a pair of access units. `meson test` (19/19 on the target and
on the builder) and `run-static-checks.sh` are unchanged.

## Encode fidelity against the source (2026-10-24)

The encode gates proved that a stream decodes and that a decode through this
driver matches a decode through the native decoder. Neither says the pixels
are the source's: an encoder that emitted flat grey would pass both.
`test/iris-encode-quality.sh` measures structural similarity against the
source instead, per codec, and refuses to trust the encoder.

Run on the target, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`, build tree `build-alloc` (this
commit's working tree), encoder node `/dev/video3` resolved by
`iris_driver` name and Card type, render node `/dev/dri/renderD128`,
`testsrc2` 1920x1080 120 frames.

| Check | H.264 | HEVC |
| --- | --- | --- |
| Iris IRQ delta | +240 | +240 (`iris` line in `/proc/interrupts`) |
| frames decoded | 120 / 120 | 120 / 120 |
| platform decode vs software decode | byte for byte | byte for byte |
| SSIM against the source, mean | 0.998596 | 0.996808 |
| SSIM against the source, worst frame | 0.9985 | 0.9963 |
| blank-stream control | 0.000034 | 0.000034 |

The blank-stream control is what makes the floor meaningful: a black frame
sequence scores three orders of magnitude below the 0.90 floor, so a pass
cannot come from the check happening not to look. The worst-frame number is
used, not the mean, because one shattered frame is one broken picture handed
back to the client.

The IRQ counter is matched on the line's name (`iris`, `qcom-iris`, `venus`
or `vcodec`) rather than a column position: the name differs between kernel
branches, and a gate reading the wrong line would report zero and fail a run
that worked.

A first version of the script named the HEVC output `.h264`. FFmpeg picks the
muxer from the extension, the H.264 raw muxer rejects HEVC at header write,
and the process then died with SIGSEGV rather than an error — the gate
reported `the encode failed`, which is true but names the wrong layer. Each
stream now carries its own codec's extension, and the failure is FFmpeg's own
message.

Not a Meson test: it needs the encoder node, a render node and FFmpeg. It is
in the hardware suite, not in `meson test` (19/19, unchanged).

## Descriptor accounting across the surface lifecycle (2026-10-24)

The driver duplicates a client's DMA-BUF on import, allocates a stable buffer
for a surface it must export, and holds both V4L2 nodes. One descriptor left
behind per surface is invisible in a short run and fatal in a long one:
Chrome keeps a pool of surfaces for hours, and at the process limit every new
surface fails.

`test/iris-fd-leak-probe` counts `/proc/self/fd` rather than keeping a ledger
the driver would agree with. Each cycle initializes once, then imports a
client DMA-BUF, derives an image, exports a handle, and tears the surface
down. Run on the same target and boot:

```
open fds per cycle: 10 10 10 10 10 10 10 10
steady state 10..10 over 7 cycles
after vaTerminate: 6 fds
  fd 2 -> pipe:…
  fd 3 -> /dev/dri/renderD128
descriptors that are not the render node or a standard stream: 0
```

The count is flat across eight cycles, and after `vaTerminate` the only
descriptors left are the render node the probe opened itself and the standard
streams — so the driver released everything it opened, including the import
duplicates and the export handles.

The negative control is required for this to mean anything: with
`IRIS_TEST_FD_LEAK=1` the probe leaks one DMA-BUF per cycle on purpose and
must fail. It does — the count climbs 10, 11, 12 … 18 and the run reports
`FAIL fd-leak cycles=8 steady=18`, with the leaked descriptors printed as
`/dmabuf:`. A probe whose accounting was broken would report a flat line in
both configurations.

Not a Meson test: it needs libva's DRM display and the built driver.

## CI: the inventory check failed on the meson version (2026-10-24)

PR #1's CI had never run: every push waited for maintainer approval because
the pull request is cross-repository (head in `FFT-infra`, base in
`phxinyang`). The first approved run failed in 1 minute 13 seconds, before a
single test ran: `missing Meson test: session`.

The runner installs the distro's meson 1.3.2, which prints a bare test name.
A developer's meson 1.11 prints `<project>:<name>`. The check grepped for
`":${required}$"`, which only matches the second form, so every job failed on
the first required name regardless of the code.

Reproduced on the builder with meson 1.3.2 in a venv (`--list` prints
`session`, `annexb`, …) and re-run the check loop against that output: the
old pattern finds nothing, the new pattern `"(^|:)${required}$"` finds all 14
required tests and still rejects `session-nonexistent`. The workflow now also
prints `meson --version`, so a future log names the version that ran.

The fix landed as `ci: accept both meson test listing formats` and the next
push went green on the fork, all twelve steps
(`40080541955`, 41 s): configure, build, the inventory check, static checks,
19/19 Meson tests, no undefined symbols, and provenance for
`0b8ed1551b74…` / `6907a881…`.

What stays red is the check on the pull request itself, and that is a
permissions fact rather than a code one. `phxinyang/qualcomm-iris-vaapi`
answers the fork-run API with `Must have admin rights to Repository` (HTTP
403) for the only credential this workspace holds, which belongs to the fork
owner. GitHub reports `action_required` and starts no job, so the PR shows no
result at all. The same commit is fully exercised by the fork's `push` run;
the two differ in the repository that records the check, not in the steps that
run. The settings that would end it need the base repository's owner: either
approve the run on the PR page, or change Settings → Actions → General → Fork
pull request workflows to require approval only from first-time contributors,
after which a push from this fork runs without a click.

## The launcher exports both nodes, and Chrome still decodes (2026-10-24)

`scripts/iris-vaapi-browser` now resolves the encoder node and exports
`LIBVA_V4L2_ENCODER_PATH` beside `LIBVA_V4L2_VIDEO_PATH`. That changes the
environment of every browser it starts, so it needs a browser run, not a
`sh -n`.

`test/iris-browser-acceptance.sh` with the build-tree driver on the same
target and boot:

```
PASS browser=Chrome/152.0.7977.82 decoder=VaapiVideoDecoder platform=True
     frames=600 dropped=0 holders=--type=gpu-process
     driver=build-tree:de253601b3ed boot_unchanged=True
```

Same decoder, platform flag set, 600 frames over a 20 s window with no
drops, and the GPU process holding the node. The launch path the launcher
controls is the one that changed; the decode path is unchanged.

The suite's new encode gate also ran from `test/iris-hardware-suite.sh`:
`iris-matrix`, `iris-structure-matrix` and `iris-encode-quality` all pass.
Before this the encode gate was reachable only by hand.

## The installed package, encode included, with no environment overrides (2026-10-24)

Every encode result in this file before this section came from a build tree
with `LIBVA_DRIVERS_PATH` set. The installed RPM was 80 commits behind
(`source_commit=1d15de26…`, 2026-09-22) and carried no encode support, so no
run had answered the question a user actually asks: after `dnf install`, with
nothing in the environment, does encode work?

### The artifact

| Fact | Value |
| --- | --- |
| Source commit | `0b8ed1551b74317d154b50e306efb3efbd77ec06` (branch `feat/encode-entrypoints`, `source_dirty=false`) |
| tracked-source SHA-256 | `6907a88135811e395d32ceede638a6447ed624d8611054e9e126c7849d03f007` |
| RPM | `libva-v4l2-iris-0.1.0-1.fc44.aarch64.rpm`, SHA-256 `e8b551d9af063e6d69f2b447e9f1bb156c301fd1120227d1ede89d579324299c` |
| Installed driver | `/usr/lib64/dri/v4l2_drv_video.so`, SHA-256 `b2f8151d3980339ccfb477801395e951d17af04845e0e6abeea86c83b4632efe`, 399920 bytes |
| Kernel | `7.2.6-sm8550-g42f3b40-iris1`, `qcom-iris.ko.zst` `9b16a40179a08e…` |
| Boot ID | `ebe7a2b2-9ae2-4725-ad33-be856e6ec7a1`, unchanged through every run below |
| Nodes | decoder `/dev/video2`, encoder `/dev/video3`, both resolved by `iris_driver` QUERYCAP name plus Card type; render `/dev/dri/renderD128` |
| Chrome | 152.0.7977.82, through the packaged launcher and desktop entry |

`rpm -V libva-v4l2-iris` is clean. The driver the RPM installed is not the
driver that was there before: `artifact_sha256` in
`/usr/share/iris-vaapi/install-manifest.txt` equals the file on disk, that
file is 399920 bytes, and the previous installed driver was 8961024. The old
file was a local `scripts/install-system.sh` build with symbols and the new one
is the RPM's stripped payload; they shared only the name. The previous driver
is backed up at `<scratch>/rollback-driver-20261024T090903Z/` with its
SHA-256 recorded, and `install-system.sh`'s own copy is at
`/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`, same hash.

One thing the RPM does not clean up: the ownership rule in
`scripts/uninstall-system.sh` refuses to remove a file that belongs to an RPM,
so the 2026-10-24 09:22 `install-system.sh` install left
`/usr/local/bin/iris-vaapi-browser` and
`/usr/local/share/applications/google-chrome-iris-v4l2.desktop` in place
beside the RPM's copies. `/usr/local/bin` precedes `/usr/bin` on this user's
`PATH`, and `/usr/local/share` precedes `/usr/share` in `XDG_DATA_DIRS`, so a
desktop launch from the icon used the **old** launcher. Pointing both at a
script that prints the environment shows the difference:

```
/usr/bin/iris-vaapi-browser        LIBVA_V4L2_ENCODER_PATH=/dev/video3
/usr/local/bin/iris-vaapi-browser  LIBVA_V4L2_ENCODER_PATH=<unset>
```

The encode path is unaffected — FFmpeg never goes through the launcher — and
the acceptance run below invokes the packaged launcher explicitly. A browser
started from the icon, though, would have the encoder node unnamed, which is
the one thing the launcher change was for. The residue is left in place
rather than deleted: it is the user's file, and removing it is a separate
authorized action.

### Runtime, no overrides

`LIBVA_DRIVER_NAME`, `LIBVA_DRIVERS_PATH`, `LIBVA_V4L2_VIDEO_PATH`,
`LIBVA_V4L2_ENCODER_PATH` and `V4L2_VA_*` were checked absent from the
environment first (`env | grep -E '^(LIBVA|V4L2_VA|GST_VA)'` matched
nothing). libva then resolved the driver through the packaged
`msm_drv_video.so` alias on its own:

```
libva info: Trying to open /usr/lib64/dri/msm_drv_video.so
libva info: Found init function __vaDriverInit_1_23
libva info: va_openDriver() returns 0
```

`vainfo` reports `VLD` for every decode profile and `VAEntrypointEncSlice`
for `H264ConstrainedBaseline`, `H264Main`, `H264High` and `HEVCMain`. That is
the first time encode entrypoints came from an installed package on this
device.

| Check | H.264 | HEVC |
| --- | --- | --- |
| `ffmpeg -vaapi_device` 300 frames 1080p, `-b:v 4M -g 60` | exit 0, 9915972 bytes | exit 0, 9968504 bytes |
| Iris IRQ delta, both in one window | +1150 for the pair | |
| decodes with no error | yes | yes |
| frames / profile / level / geometry | 300, High, 4.0, 1920x1080 | 300, Main, 4.0, 1920x1080 |
| fidelity gate SSIM against the source | mean 0.998596, worst 0.9985 | mean 0.996808, worst 0.9963 |
| blank-stream control | 0.000034 | 0.000034 |

The two FFmpeg warnings are the documented ones and not failures:
`Driver does not support any packed headers (wanted 0xd)`, and for HEVC
`Driver does not advertise encoder features, using guessed defaults`. The
fidelity gate ran with `LIBVA_DRIVERS_PATH=/usr/lib64/dri` and produced the
same numbers as the build-tree run recorded above — as it must, since it is
the same source — which also confirms the gate now measures the driver it is
told to measure.

### The rest of the gates, from the installed package

All run with `LIBVA_DRIVERS_PATH=/usr/lib64/dri`:

| Gate | Result |
| --- | --- |
| `iris-matrix` | H.264, VP9, HEVC, AV1 all `content=frame-md5`; 48 frames each, 1 drain, 0 timeouts, 0 drops; AV1 `noshow=23` as recorded |
| `iris-structure-matrix` | all-I, IP-g12, B=2, B=4 pass |
| `iris-zero-copy-suite` | `direct=24` |
| `iris-encode-quality` | H.264 and HEVC pass, numbers above |
| `iris-browser-acceptance`, packaged launcher, no `--driver` | `decoder=VaapiVideoDecoder platform=True frames=902 dropped=0 holders=--type=gpu-process driver=installed:b2f8151d3980` |
| `iris-hardware-suite` | `iris-matrix`, `iris-structure-matrix`, `iris-encode-quality`, and native GStreamer h264/vp9/hevc/av1 all PASS |

The matrix trace reports `publish=direct` for all 48 H.264 and all 48 HEVC
completions. FFmpeg exports surfaces only after their decode, so Direct is
the expected policy; the Import path is exercised by Chrome, which the
acceptance run above does with the GPU process holding the node.

`iris-browser-acceptance.sh` launches `../scripts/iris-vaapi-browser` from the
checkout it runs in, so testing the packaged launcher needs that path to be
the packaged file. Run from a tree whose `scripts/iris-vaapi-browser` is a
symlink to `/usr/bin/iris-vaapi-browser`, with no `--driver` and no
environment set, Chrome reports `VaapiVideoDecoder`, platform true, 902
frames in 30 s, zero drops, `driver=installed:b2f8151d3980`, and the GPU
process holds `/dev/video2`. That is the packaging path end to end: the RPM's
launcher found both nodes, and the RPM's driver decoded through libva's
`msm_drv_video.so` alias.

### The one failure, and why it is not this driver

`test/iris-hardware-suite.sh` reported `FAIL v4l2-compliance rc=1`:
`Total for iris_driver device /dev/video2: 48, Succeeded: 41, Failed: 7`. The
seven are the kernel's, not the driver's — `v4l2-compliance` talks to the
node through ioctls and links neither libva nor this `.so` (`ldd` shows no
`libva` or `libdrm`). The classes:

- `missing bus_info prefix ('plat:aa00000.video-codec:dec')`, twice, on
  `VIDIOC_QUERYCAP`. Both Iris nodes report `plat:aa00000.video-codec:dec`
  and `plat:aa00000.video-codec:enc`. The v4l2 core fills `bus_info` with
  `platform:` when the driver leaves it empty, so the driver writes this
  string itself; Venus writes the same `plat:%s:dec` form
  (`drivers/media/platform/qcom/venus/vdec.c`). v4l2-compliance's allow-list
  contains `platform:` and not `plat:`.
- `!colorspace` on `VIDIOC_G_FMT`, `TRY_FMT` and `S_FMT` — the Iris formats
  report no `colorspace`, `ycbcr_enc` or `quantization` for the compressed
  OUTPUT formats.
- `min_number_of_capture_buffers` present in `--list-ctrls` but absent from
  the control class `v4l2-compliance` classifies it under.
- `q2.reqbufs(node2, 1) != EBUSY` on `REQBUFS`.

This file records 47/48 for `v4l2-compliance` in August 2026, with only the
control-class failure. That run used kernel `7.2.0-sm8550`; this one uses
`7.2.6-sm8550-g42f3b40-iris1`, which is the kernel with the project's four
patches applied. The extra failures are the QUERYCAP `bus_info`, the
colorspace trio and the `REQBUFS` one. None of the four patches touches
`bus_info`, colorspace or `REQBUFS` (`grep bus_info packaging/kernel/*.patch`
matches nothing), so the difference belongs to the 7.2.6 base or to the
patches applying over it; the August rows are kept as recorded rather than
overwritten. The point of this section is that the current target fails them
and the userspace gates all pass while it does.

`v4l2-compliance` is not a gate on the driver in any case: the suite fails
the run because a strict external check failed, and a maintainer reading the
summary should treat that line as a kernel-interface report, not as a
regression in `src/`.

### One defect the run found in the test tree

`test/iris-encode-quality.sh` set `LIBVA_DRIVERS_PATH=$root/build-local/src`
in the encode command, ignoring the caller. On this target that directory
does not exist, so the gate could only ever have measured an installed
driver, and `iris-hardware-suite.sh` passing `LIBVA_DRIVERS_PATH` to it
silently did nothing. It now reads `LIBVA_DRIVERS_PATH` (default
`build/src`) and fails with a clear message when that directory holds no
`v4l2_drv_video.so`; the negative control above confirms the failure path.

## Encoder picture contract and packed headers: code and host gates only (2026-10-24)

Two changes in the encode path, neither of them hardware-verified yet. The
tablet was at 2% and discharging with no charger connected, so every run
below is on the builder and none of them touches the codec.

### What changed

The encoder had been accepting picture parameters it could not honour and
producing a stream that quietly used something else. Three of those are now
refused or promoted:

- A non-IDR I picture, and an HEVC CRA, become an IDR request. FFmpeg
  submits this shape when `-idr_interval` is above zero —
  `vaapi_encode_h264.c` sets `slice_type = 7` with `idr_pic_flag = 0`
  (`:436-439`, `:548`) and the HEVC path uses `HEVC_NAL_CRA_NUT`
  (`:485-489`) — so this was not a Sunshine-only edge case. Iris has only
  `FORCE_KEY_FRAME`, which emits an IDR; the alternative was the firmware
  coding the picture as a P and misordering the stream. The interval
  defaults to 0, so a plain `-g` run never reaches the path; the section
  below records the run that does.
- An inter picture must name the previous picture's reconstruction surface
  in `RefPicList0[0]` / `ref_pic_list0[0]`. The session keeps
  `last_reconstruction_` and compares. The firmware has no reference
  management, so a reference to anything else is a picture coded against
  pixels nobody can name.
- A stream must start on an intra picture, a picture with
  `reference_pic_flag` clear is refused, one full-picture slice is the only
  layout accepted, B slices are refused, and an HEVC `coding_type` that
  disagrees with the slice type is `VA_STATUS_ERROR_INVALID_PARAMETER`.

`VAConfigAttribEncPackedHeaders` now answers `VA_ENC_PACKED_HEADER_NONE`
(zero) instead of `VA_ATTRIB_NOT_SUPPORTED`, and
`VAConfigAttribEncMaxSlices` answers 1. The first is what stopped
GStreamer's `vah264enc`/`vah265enc` before the first frame: its
`gst_va_display_get_packed_headers` returns -1 only for
`VA_ATTRIB_NOT_SUPPORTED`, so zero passes. An earlier section of this file
claimed Radxa's backend answered NONE and "fails the same check"; that was
wrong, and it is corrected in place.

### What was verified, and where

On the builder, `eea58f3` plus this working tree:

```
sh test/run-static-checks.sh   PASS static checks
meson test -C build-local      Ok: 19, Fail: 0
git diff --check               clean
```

`test/encode-params-test.cc` gained `test_reference_and_slice_shape()` (15
assertions: P reports its reference, intra reports none, a missing or
long-term reference is refused, a partial slice, an offset slice, a B slice,
an IDR paired with a P slice, a non-reference picture and an HEVC
`coding_type` mismatch are all refused). `test/encode-session-test.cc` now
builds a real 12-picture chain with the intra pictures at 0 and 10, because
a session that accepts a broken chain would pass a test that only ever
submitted one picture.

`test/iris-browser-launcher-check.sh` exercises the new
`--print-encoder` mode against a fake browser, including a display with no
encoder node, which must print nothing and exit zero.

### What is not evidence

- No encode was run on the target with this tree. The promoted-IDR path, the
  reference check and the GStreamer encoder all need a hardware run before
  any of them is claimed. The relevant gate is
  `test/iris-encode-quality.sh`, plus `vah264enc`/`vah265enc` for the packed
  header change.
- The stock-kernel intra-period lever is unchanged and still needs its own
  run for a promoted request, not only for `idr_pic_flag`.

### Scripts added to the install

`iris-device-check`, `iris-verify` and `iris-configure-clients` are installed
next to the launcher by `scripts/install-system.sh`, and both packages'
file lists carry them (`test/iris-packaging-check.sh` enforces the three-way
agreement). `iris-device-check` and `iris-verify` resolve both nodes through
`iris-vaapi-browser --print-node` / `--print-encoder`, so a boot that
renumbers `/dev/video*` changes nothing in them; `test/run-static-checks.sh`
asserts neither carries a node literal and both use the launcher.

A `DESTDIR` staging run of `install-system.sh` on the builder produced the
expected payload (driver, alias, launcher, three diagnostics, desktop entry,
manifest), and both diagnostics were executed from that staged tree. On the
builder they correctly report no Iris codec and exit 1. That is a staging
run, not an installation, and it says nothing about the scripts' behaviour
on hardware.

## GStreamer encode, promoted IDR, and the double claim (2026-10-24)

The section above recorded code and host gates only. This one is the hardware
run that was missing, and it found a defect the host gates could not.

| Fact | Value |
| --- | --- |
| Source | `eea58f3` plus the working tree; `source_dirty=true` (uncommitted) |
| tracked-source SHA-256 | `6528305b2d98a96eceafcae6819734a56d4f1faf0eab7e9cde9244c387622fd3` |
| Driver | build tree `build/src/v4l2_drv_video.so`, SHA-256 `69fb872cf04a89d7ce38ce8f64008e42452a5b70d4a73eba6348db3fcff08197` |
| Kernel | `7.2.6-sm8550-g42f3b40-iris1`, `qcom-iris.ko.zst` `9b16a40179a08e41…` |
| Boot ID | `36ef0142-1f34-418f-9704-56e2e0ceae41` |
| Nodes | decoder `/dev/video17`, encoder `/dev/video18`, resolved by `iris_driver` QUERYCAP name; render `/dev/dri/renderD128` |
| Clients | FFmpeg 8.1.3, GStreamer 1.28.8 |
| Runtime | build tree through `LIBVA_DRIVERS_PATH`, not the installed package |

### The defect this run found: two claims on one completion

`VAConfigAttribEncPackedHeaders` answering `VA_ENC_PACKED_HEADER_NONE` got
`vah264enc` past its open check, and the pipeline then failed at the first
frame:

```
gst_video_encoder_allocate_output_buffer: assertion 'size > 0' failed
vabaseenc gstvabaseenc.c:431: Failed to allocate output buffer, size 0
```

`vah264enc` claims a picture twice: `va_sync_surface` on the input surface
inside `gst_va_base_enc_map_coded_buffer`, then `va_map_buffer` on the coded
buffer. `vaSyncSurface` claimed the ticket and discarded the bytes; the
`vaMapBuffer` that followed found nothing outstanding and handed back an
empty segment, so the access unit GStreamer had already paid for measured
zero bytes. FFmpeg never hit it because it syncs the coded buffer, not the
surface.

The fix makes the claim one operation: `claim_coded()` stores the access unit
on the buffer the client named, and `vaSyncSurface`, `vaSyncBuffer` and
`vaMapBuffer` all funnel through it. Whichever arrives first keeps the bytes;
the others find them. `Context::EncSubmitted` now carries the coded buffer id
so `vaSyncSurface` knows where to put the result. An empty result from
`wait_timeout` means another thread already claimed it, and it no longer
overwrites the payload with nothing.

### GStreamer encode, H.264 and HEVC

```
gst-launch-1.0 -e videotestsrc num-buffers=90 ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 \
    ! vah264enc bitrate=4000 ! h264parse ! filesink location=out-gst.h264
```

| Codec | Result | Stream |
| --- | --- | --- |
| H.264 | `rc=0`, 90 pictures submitted, `encode drain result=complete` | 1487150 bytes, High, level 3.1, 1280x720, 90 frames, decodes clean |
| HEVC | `rc=0`, 90 pictures, drain complete | 1487896 bytes, Main, 1280x720, 90 frames, decodes clean |

`GST_VA_ALL_DRIVERS=1` was set; that is GStreamer's own vendor allow-list
gate, not this driver's, and it is documented in the README.

### The promoted IDR, on hardware

FFmpeg only emits a non-IDR I picture when asked to. `-idr_interval` defaults
to 0, so every GOP boundary is an IDR
(`hw_base_encode.c:718`, `gop_per_idr = idr_interval + 1`), and
`FF_HW_FLAG_NON_IDR_KEY_PICTURES` is what makes a larger interval possible.
The earlier note that FFmpeg "does submit these" was true only of that flag;
without it the promotion path never runs.

`-g 60 -idr_interval 2`, 250 frames, 1280x720 at 24 fps, H.264:

| Frame | `idr=` | `promoted=` | Why |
| --- | --- | --- | --- |
| 0 | 1 | 0 | the client's first picture |
| 60 | 1 | 1 | non-IDR I picture, rewritten to an IDR |
| 120 | 1 | 1 | the same |
| 180 | 1 | 0 | FFmpeg's own IDR: `idr_counter` reached `gop_per_idr` |
| 240 | 1 | 1 | the same as 60 and 120 |

Key frames land at 0, 60, 120, 180, 240 and the stream decodes clean. The
`promoted=` field was added for this record: `idr=1` alone cannot distinguish
a request that was honoured from one that was rewritten, and the two produce
the same bitstream.

The same run with the default `-idr_interval 0` and
`-force_key_frames "expr:eq(n,24)+eq(n,40)"` put IDRs at 0, 24, 40, 100 with
`promoted=0` on every one, which matches the re-anchoring schedule recorded in
"REQUEST_SYNC_FRAME on the target" above.

### Regression: decode did not move

`vaSyncSurface` changed, so every decode gate ran again on the same build:

| Gate | Result |
| --- | --- |
| `test/iris-matrix.sh` | H.264, VP9, HEVC, AV1 each 48/48 frames, exact frame MD5 against the native V4L2 decoders; AV1 `submitted=50 capture=50 noshow=23` |
| `test/iris-structure-matrix.sh` | all-i, ip-g12, b2-g48, b4-g48 all PASS |
| `test/iris-zero-copy-suite.sh` | `direct=24`, every completion Direct with no copy engine |
| `test/iris-encode-quality.sh` | H.264 IRQ +244, SSIM mean 0.998596 / worst 0.9985; HEVC IRQ +248, mean 0.996808 / worst 0.9963; blank-stream control 0.000034 |

### `iris-verify` and `iris-device-check`, on the target

`scripts/iris-verify.sh` passed every check: libva opened the driver with no
environment set, 12 profiles, encode entrypoint advertised, decode moved the
Iris IRQ by 102, H.264 encode by 124, HEVC by 121, both streams decoded, and
mpv reported hardware decode on the native V4L2 path.

`scripts/iris-device-check.sh` reported the facts this record needed, and two
of its own assumptions were wrong on the first run:

- It looked for `vpu35*.mbn`. This target's device tree asks for
  `qcom/sm8550/sheng/vpu30_4v.mbn`, which is present at 2306664 bytes. It now
  reads `firmware-name` from the codec's `of_node` and reports whether that
  exact file exists.
- It probed `hevc_loop_filter_mode`. The control is `hevc_loop_filter`; the
  six loop-filter controls are now listed from `--list-ctrls` rather than
  matched against guessed names.

The check also confirms `iommu_group_type=DMA` for `aa00000.video-codec`,
which is the shared address space the GPU blit path depends on.

Committed as `e15c2fe`. As with the Import section above, the deployed tree
matched that commit under `src/` except for one comment, which the commit
rewrote to carry these numbers.

### Power

The tablet was at 6% and charging when the run started, 10% by the end, at
about 2.0 A. No soak or media-generation pass was attempted; the two-hour
concurrency soak was deferred at the time. The guarded runs after 2026-11-12
record pack state in `battery.log` and treat a discharging or low pack as a
measured boundary rather than a start gate.

## VP9 Import: one picture, one buffer per completion (2026-10-24)

The one-buffer Import window was a unit mistake. A VP9 alt-ref access unit is
one VA picture the translator turns into a superframe, and the firmware
decodes that as two pictures. It does not return the invisible half before it
has a slot for the visible one, so a one-buffer window deadlocks on every
alt-ref (the ~500 ms measured 2026-09-22). Widening the window to two
different pictures removed the stall and gave the firmware a choice:
153/609 and 159/599 misplaced.

The rule the firmware needs is *one picture's memory, in enough copies for
its completions*. That is what the session does now.

### What changed

- `AccessUnit::completions` (`src/codec/translator.h`): the translator says
  how many CAPTURE completions an access unit produces. VP9 sets 2 for an
  invisible frame, because it appends the `show_existing_frame` header that
  makes the firmware decode a second picture; every other translator leaves
  the default 1.
- `Target::completions` carries it into the session (`src/va/picture.cc`).
- `ImportEntry` gained `completions` and `lent`. `queue_import_next()`
  queues the client buffer once per completion in a single pass, and the
  entry stays at the head of the backlog until its token is answered, so no
  other picture's buffer is ever queued behind it. `kImportWindow` is gone;
  the pool counts how many slots carry the current token
  (`SlotPool::capture_queued_for`).
- The DMA-BUF fence is waited for once per picture, not once per queued
  buffer: every half of a superframe lands in the same client buffer.
- Trace: `import token=… lent=<n>/<n>` (`docs/architecture.md` §8).

### Host gates

`test/unit/fake_device.cc` now models the requirement rather than only the
shape: a superframe whose second slot is missing stays in the OUTPUT queue
instead of completing, which is the stall, modelled rather than timed.

| Case | Assertion |
| --- | --- |
| `import alt-ref gets a buffer per completion` | two CAPTURE slots, both carrying the client's buffer, then one completion, no error, `misplaced=0` |
| `import alt-ref does not stall the next picture` | the next picture's buffer goes in as soon as the superframe's slots come back |

Both fail when `completions` is set back to 1 (checked by temporarily editing
the test), so they measure the fix and not the harness.

`test/iris-eos-check.sh` counted only `publish=direct|copy-gpu|copy-cpu` as
a capture, so a fully successful Import run reported `capture=0` and failed
the matrix. `publish=import` is counted now, with a self-test case. That
defect predates this change; it surfaced because the matrix was finally run
with `V4L2_VA_PUBLISH=import`.

### On the target

Source commit `eea58f3`, driver `2ffb82cf…` built from the deploy tree,
kernel `7.2.6-sm8550-g42f3b40-iris1`, boot `36ef0142-…`, decoder
`/dev/video17`. VP9 is not qualified for Import in the capability table, so
`V4L2_VA_PUBLISH=import` cannot reach it. The measurement used a scratch
copy of the deploy tree with one extra line in `driver.cc` qualifying
`V4L2_PIX_FMT_VP9` (`<scratch>/vp9-import-probe` on the target, not
committed); everything else is the tree above.

Clip: 1280x720, 480 frames, `libvpx-vp9` two-pass with `-lag-in-frames 25
-auto-alt-ref 1`, which the header parse confirms carries 35 invisible
frames and 4 keys. A one-pass encode at any `-cpu-used` carries none, which
is why the earlier probe clips showed no alt-refs.

| Run | Result |
| --- | --- |
| FFmpeg, `V4L2_VA_PUBLISH=import`, 4 runs | `submitted=515 completed=515 errors=0 drops=0 timeouts=0 misplaced=0`, `framemd5` identical to the software decode every time |
| Chrome 152, forced Import, 20 s | `submitted=718 completed=177 errors=47 drops=541 misplaced=0`; 3 `publish=invisible`, 50 `lent=2/2`, no fence wait over 20 ms; Chrome reported `VaapiVideoDecoder`, platform=true, 480 frames / 0 dropped |
| Chrome 152, forced Copy, 20 s | `submitted=709 completed=708 errors=0 drops=1 misplaced=0`; 708 `publish=copy-gpu`, 49 `publish=invisible` |
| Chrome 152, pre-fix variant (`completions = 1`), 20 s | `submitted=315 completed=1 errors=138 drops=240 misplaced=74`; acceptance failed, no Iris node holder at the end |

The pre-fix row reproduces the recorded failure and shows the fix is
load-bearing: the stall is gone and `misplaced` is 0 everywhere.

### Why VP9 Import still is not qualified

Superseded 2026-11-12 by the section "VP9 Import qualified" at the end of this
file: the released-surface drops measured here are the defect that change
removes.

Chrome releases the surface of a VP9 picture before that picture completes
far more often than it does for H.264. In the forced-Import run 564 of 768
import queueings carried `abandoned=1` and 541 completions were dropped as
released; the H.264 Import control in the same session shape had 2 abandoned
and 4 drops out of 670. All 47 error completions are the invisible halves of
released alt-refs (`flags=0x4040`, token present, target already released),
not decode errors.

The consequence is visible in the counts: 177 pictures completed where the
Copy run completed 708 for the same clip and window. Chrome reported 480
frames and 0 dropped in both, so its own counters do not distinguish
"displayed the decoded picture" from "re-presented the last good one", which
is exactly the silent degradation this project refuses to accept as a pass.

So the session-side defect the design named is fixed and the capability
table's stated blocker is gone, but the Chrome path is not clean. VP9 stays
`publish_import: fallback`; a target run has to show `misplaced=0`, no
drops beyond the Copy baseline, and `completed` within a few pictures of
`submitted` before that changes.

Artifacts: `<scratch>/vp9probe/` on the target (`trace720-*.log`,
`acc2/`, `acc-copy2/`, `acc-prefix/`, `acc-h264/`).

### Regression on the final tree

After the use-after-pop fix in the backlog drain path (`entry` is a reference
into `import_backlog_`, so the fd is saved before `pop_front()`), the tree
was redeployed and every target gate re-run. Driver `4c87ab1b…`.

| Gate | Result |
| --- | --- |
| `test/iris-matrix.sh`, `V4L2_VA_PUBLISH=import` | H.264, VP9, HEVC 48/48 exact frame MD5; AV1 `submitted=50 capture=50 noshow=23`; `import=48` each for H.264 and HEVC, `misplaced=0`, 0 errors, 0 drops |
| `test/iris-structure-matrix.sh` | all-i, ip-g12, b2-g48, b4-g48 all PASS |
| `test/iris-zero-copy-suite.sh` | `direct=24` |
| `test/iris-hardware-suite.sh` | `iris-matrix`, `iris-structure-matrix`, `iris-encode-quality`, all four GStreamer codecs PASS; `FAIL v4l2-compliance rc=1` is the same 7 kernel-interface failures analysed above (41/48, `bus_info` prefix, colorspace, control class, `REQBUFS`), unchanged |

VP9 Import is still not qualified, for the Chrome reason recorded above;
this row only shows the restructure did not move anything that was passing.

### Second pass: a reboot, one more fix, and the canonical-tree browser run

The target rebooted mid-session (`boot_id b7f2e336-4502-4b46-966c-770f18a47c8d`,
kernel unchanged, `7.2.6-sm8550-g42f3b40-iris1`). Before redeploying, one more
defect in the new code: `ImportEntry::lent` counts slots *in the current
CAPTURE pool*, and a resolution change tears that pool down and rebuilds it.
An entry carried across the change would keep saying "already lent" for
buffers the firmware never received, and its picture would never be queued.
`enter_streaming()` now clears `lent` on every backlog entry, which is the
only place a pool is created.

Driver `bd1b0015…`, same source commit, battery 51% and **discharging** —
so no long run was attempted; the two-hour soak is still outstanding.

| Gate | Result |
| --- | --- |
| `test/iris-dynamic-resolution.sh`, `V4L2_VA_PUBLISH=import` | H.264 100 transitions, 101 contexts, every segment `capture=2 drops=0`, PASS |
| `test/iris-matrix.sh`, default `auto` | H.264, VP9, HEVC 48/48 exact frame MD5; AV1 `50/50 noshow=23` |
| `test/iris-matrix.sh`, `V4L2_VA_PUBLISH=import` | same, 48/48 each |
| Chrome 152, H.264 720p, default `auto` on the canonical tree | `VaapiVideoDecoder`, platform=true, 479 frames / 0 dropped, GPU process held the node, `import=653 drops=4 errors=0`, every queueing `lent=1/1` |
| `iris-import-probe` (§9 association gate) | 100/120 misplaced at `max_in_flight=6` — the recorded "several buffers to choose from" failure, unchanged by this work and the reason the session keeps one picture's buffers only |

The Chrome row is the one that was missing: earlier VP9 browser numbers came
from a scratch tree with VP9 patched into `import_formats`. This run uses the
deployed tree and the shipped policy, and it is the H.264 Import control that
the VP9 comparison above needs.

Committed as `e6af9ef`. The tree the runs used carried this code and nothing
else that differs: every file under `src/` was byte-identical to the commit
apart from one comment in `driver.cc`, which is where the VP9 numbers above
now live.

## VP9 corpus re-run, and what the 4K decode cost is (2026-10-25)

Two questions carried over: whether the DRC ordering fix (`enter_streaming()`
issues CAPTURE STREAMON before the pool QBUF, recorded above) lifted any VP9
family, and whether the `translate` cost growth recorded in the 2026-10-22
4K section is real accumulation.

Driver `bd1b0015…`, kernel `7.2.6-sm8550-g42f3b40-iris1`, source tree
`<lab-root>/src` built from `feat/encode-entrypoints`. Two boots
were used because the tablet reset itself mid-run; the corpus result is the
same on both (see the reboot note at the end). Node `/dev/video17` on the
first boot, `/dev/video0` on the second; the node is resolved by the
`Iris Decoder` QUERYCAP name in every command, never by number.

### The corpus, three decoders

`fluster run -ts VP9-TEST-VECTORS`, `-j 4 -t 30`, per-vector CSV
(`<scratch>/vp9-fl/run/vp9-{sw,va,va2}.csv`). The software arm is the
same FFmpeg build decoding with libvpx, which is what the corpus reference
MD5s come from, so it is the control that tells a driver defect from a
corpus/reference mismatch:

| Arm | Success | Fail | Error | Timeout |
| --- | ---: | ---: | ---: | ---: |
| `FFmpeg-VP9` (libvpx, software) | 242 | 63 | 0 | 0 |
| `FFmpeg-VP9-VAAPI` | 176 | 36 | 66 | 27 |

The FFmpeg VA-API wrapper in this Fluster version does not pass
`-hwaccel_output_format vaapi` and has no `hw_download` mapping for VAAPI
(`decoders/ffmpeg.py`), so `format=pix_fmts=yuv420p` filters back to system
memory and FFmpeg downloads each frame. The arm still decodes through this
driver, but it is not the pixel oracle `test/iris-matrix.sh` is; every claim
below was re-checked directly against libvpx.

Against the 2026-10-22 run (175/305) the pass count moved by one vector, and
one family changed:

- **`vp90-2-10-show-existing-frame` now passes** (2/2). It crashed before.
  `src/codec/vp9.cc` refuses `show_existing_frame` by design
  (`VA_STATUS_ERROR_UNIMPLEMENTED`) because VA's VP9 header has no
  `frame_to_show`; this vector reaches the driver only through FFmpeg's own
  `show_existing_frame` handling, which re-feeds the referenced frame instead
  of a header the translator has to accept.
- **`vp90-2-14` is unchanged at 36 Fail.** The DRC ordering fix did not move
  it, and it is not a driver defect. The vectors decode 18 frames through
  the driver and 18 through libvpx; `vp90-2-14-resize-fp-tiles-1-2`,
  `-10frames-fp-tiles-4-8` and `-fp-tiles-1-16` are **pixel-identical to
  libvpx frame for frame**, and `-fp-tiles-16-1` differs on 3 of 18 frames
  (0.07-0.20 % of each frame, confined to luma columns 3389-3596 of a
  4096-wide picture, chroma exact, maximum delta 255 on frames 0 and 2). The
  software arm fails the same 36 vectors: the corpus reference MD5s predate
  the libvpx version on this machine. Recorded as a corpus/reference
  mismatch, not a decode defect.
- **`vp90-2-21` (24) and `vp90-2-05`/`-18`/`-22` (3) time out, and the
  native V4L2 decoder stalls on the same streams.** The `320x180` and
  `640x480` 2-21 vectors and the `vp90-2-16-intra-only` vector, each bounded
  at 25 s, one decoder at a time:

  | Vector | `FFmpeg-VP9-VAAPI` | `vp9_v4l2m2m` | `libvpx-vp9` |
  | --- | --- | --- | --- |
  | `2-21-resize_inter_320x180_5_1-2` | killed at 28 s | killed at 28 s | 177 ms |
  | `2-21-resize_inter_640x480_7_3-4` | killed at 28 s | killed at 28 s | 189 ms |
  | `2-16-intra-only` | fails in 15.3 s | 207 ms | 178 ms |

  The 2-21 family drives `resize_inter`: the first 10 frames are 1920x1080
  and complete (`submitted=10 completed=10`), then the stream switches to
  960x540 in the same context. Every 2-21 vector is a mid-stream geometry
  change that keeps the context, so it is the in-context resize path, and the
  native decoder hangs on it too. That makes it a firmware/kernel-path limit
  rather than this driver's session logic.
- **`vp90-2-16-intra-only` fails on the native V4L2 client too; corrected
  2026-11-14.** The earlier entry here said the native decoder returned 7
  frames in 207 ms. That is the software decoder: `vp9_v4l2m2m` exits 0 with
  `frame=0`, and a raw V4L2 client that queues all seven demuxed packets with
  CAPTURE streaming sees no SOURCE_CHANGE and dequeues no CAPTURE buffer. The
  first packet is a 117 963-byte superframe of four frames (30 299 / 35 391 /
  40 875 / 11 384 bytes plus a 14-byte index); the first three carry
  `84 c9 …` (inter, `show_frame=0`, intra-only) and the fourth `86 02 …`
  (inter, `show_frame=1`). There is no key frame in the stream, the decoder
  node never signals a format change for it, and both VA drivers retry the
  same 5 s wait per access unit. Details and the probe: the intra-only
  section at the end of this file.
- **64 × `vp90-2-02` and `vp91-2-04-yuv444` are the advertised-capability
  boundary**: every 2-02 vector is 8x8 through 66x66, below the node's
  96x96 minimum, and FFmpeg refuses them before any VA call. The 130x132 and
  1920x1080 vectors of the same family decode (10 and 217 frames). Profile 1
  4:4:4 is outside the advertised Profile 0/2.

Re-running the whole corpus on the second boot produced a per-vector
identical result: 176 / 66 / 36 / 27, zero vectors differing.

### The 4K decode cost is bitstream size, not accumulation

Every straight A/B on this tablet is confounded by the SoC clocking during
the run, and the earlier "0.15 ms cold, 1.5 ms after twenty seconds" reading
was taken without a control for that. Method: `/proc/<pid>/stat`
utime+stime sampled against FFmpeg's own `-progress frame=N` every 0.25-0.5 s
inside one process, no `V4L2_VA_TRACE` (one `fprintf` per record is a large
part of the 4K cost), no `showinfo` (a full frame copy per frame on both
arms). Clips are `testsrc2` 3840x2160 HEVC from `libx265 -preset ultrafast`,
1200 frames.

| Check | VA arm | `hevc_v4l2m2m` |
| --- | --- | --- |
| First 3 windows / last 3, one 1200-frame run, three repeats | 1437→1995, 1570→2354, 1468→2180 µs/frame | flat |
| Same, 12 000 frames (10 copies concatenated) | 4171→4538, 4270→4440, 4175→4481 µs/frame | flat |
| Whole-run CPU, 1080p | 947 / 994 / 901 | 618 / 656 / 612 |
| Whole-run CPU, 4K | 3810 / 3469 / 3918 | 1369 / 1377 / 1471 |
| Whole-run CPU, 4K at 1/30 the bitrate (`-crf 40`) | 927 / 714 / 829 | 505 / 442 / 538 |

The growth inside one run is the clock. Three controls, each of which would
have shown accumulation and none of which did:

- A warm second run starts as cold as the first (1437/1570/1468 µs first
  window against 1995/2354/2180 last window), so nothing carries across
  processes, and a 10x longer run has the same tail as a short one
  (4171-4270 against 1995-2354 is the same population).
- The native arm is flat in the same run (`probe18.sh`: 1456/1402/1429 first
  window, 1404/1478/1405 last, three repeats) while the VA arm rises
  (3034→4469), and interleaved 400- and 1200-frame runs give VA 1.53-1.85 and
  native 0.70-0.96 as the last/first ratio (`probe19.sh`).
- Pacing the client with `-re` removes it: 1.85 and 1.56 unpaced against 1.00
  and 1.08 paced (`probe21.sh`).

A timer inside `vaEndPicture` then measured `translate` alone and reproduced
the rise: about 700-860 µs per call over the first 100-300 pictures, 2100-2400
µs by picture 1200, resetting at the start of every new process. Thread CPU
time grows with it (777 → 1953 µs), so the thread is on the core for longer,
not merely descheduled. It is not cache or DRAM pressure: a pure-CPU loop over
the same `BitReader`/`append_escaped_nal` path is flat over 20 s both warm
(0.566 → 0.564 ms) and against 77.6 MB of cold DRAM (0.583 → 0.584 ms)
(`naldrift.cc`, `naldrift2.cc`), and it is flat with a 4K VA decode running
alongside on other cores (1.186 → 1.157 ms), which rules out memory-bandwidth
contention with the firmware's own writes.

What settled it was pinning and clocking. Pinned to cpu7 the run still grows
(608 → 956 µs) while cpu7 falls from its 2957 MHz boost to 1248 MHz; pinned to
the little core at a flat 2.0 GHz it does not (1341 → 1349 µs). Holding cpu7 at
1708.8 MHz through `userspace` + `scaling_setspeed` makes the series dead flat
across two runs, 1034/1032/1032/1031/1033 µs and 921/905/964/980/994 µs, and
the absolute values cross-check: 1034 µs at 1708.8 MHz predicts 598 µs at
2956.8 MHz, where 615 µs was measured. The work is constant per picture and
only the frequency moves. The mechanism behind the trajectory is ordinary
DVFS: an unpaced 4K decode drives the VPU hard, which pushes the big cluster's
clock down, and the more so the deeper FFmpeg's read-ahead keeps the queue.
That is why `-re` removes the growth and why a pinned little core, already at
its ceiling, shows none. A 10x longer run reads as a higher tail for the same
reason: the clocks have longer to settle low.

RSS is flat at 260 MB from 2 s to the end of a 1200-frame run and the fd count
returns to its starting value, so no container accumulates either.

What does scale is the bitstream. At 4K the delta against the native decoder
is 2.3-2.4 ms/frame on a 140 KB/frame clip and 0.29-0.39 ms/frame on a
4.8 KB/frame clip of the same geometry and frame count: about 0.27 ms of
fixed per-picture cost plus 15 µs per kilobyte. The bitrate scaling is the
point, because fixed bookkeeping would not move with it. How much of that
per-kilobyte cost is the translator was then bounded directly: on one
203 532-byte slice NAL, `BitReader`'s emulation-prevention scan costs
0.190 ms, the whole-byte copy 0.007 ms, and `append_escaped_nal` 0.516 ms,
0.713 ms in all, or about 0.5 ms for a 140 KB frame. At a fixed 1.7 GHz the
measured cost of `translate` on the 140 KB clip is 1.03 ms, so the remaining
0.5 ms is the parameter sets, the reference collection and the call itself.
The rest of the whole-run delta is FFmpeg's VA-API-side parsing, which fills
the `VASliceParameterBufferHEVC` the native wrapper never builds; this run
bounded the driver's share but did not separate the two.

### A kernel defect found on the way, and a tablet reset

The tablet reset itself twice while this was running.

- 01:41:48, `NULL pointer dereference` in `iris_check_session_supported+0x38`,
  called from `iris_vb2_start_streaming+0x70`, `Comm: dec0:0:vp9` — the
  native `vp9_v4l2m2m` client, not this driver. The address is 0 and the
  call is STREAMON, so the session pointer the driver dereferences was not
  there. The tablet came back on the same kernel and boot configuration.
- 02:43, during a Fluster run of `FFmpeg-VP9-V4L2m2m`: `received system error
  of type 0x5000003`, then `session error 0x1000006: unknown` and
  `0x4000004: invalid operation for current state` from several concurrent
  clients, then `WARNING` at `videobuf2-core.c:1821` in `vb2_start_streaming`
  for `dec0:0:vp9_v4l2` processes. The `FFmpeg-VP9-VAAPI` arm of the same
  suite run in parallel had no kernel message.

Both crashes are in the module's own session bookkeeping under concurrent
VP9 clients, on a code path the native decoder drives and this driver does
not. They are recorded here because a reset mid-run invalidates any
measurement in flight, and because `panic_on_oops` is 0 on this boot, so the
first one did not panic — the tablet reset for another reason. Neither is
this driver's code, and neither is fixed here.

Artifacts on the target: `<scratch>/vp9-fl/run/` (CSV and JSON for
every arm), `<scratch>/dec-perf/` (probes `probe8.sh` through
`probe23.sh`, raw window CSVs `p10-*`/`p15-*`/`p17-*`, `nalscan.cc`,
`naldrift.cc`/`naldrift2.cc`, `patch_timer_cpu.py`/`patch_timer_freq.py` and
the scratch tree `timer/src` they patch, the trace `p16i.trace`, and the raw
YUV pair `sw.yuv`/`va.yuv` for the `16-1` vector). The frequency-pinning
probe, `probe23.sh`, restores the `schedutil` governor on exit.

## The browser launcher is gone, and Chrome does not need it (2026-10-25)

`scripts/iris-vaapi-browser` and the `Google Chrome (Iris VA-API)` desktop
entry are removed. The launcher existed to resolve the decoder node and export
`LIBVA_DRIVER_NAME` / `LIBVA_V4L2_VIDEO_PATH`, and its comment justified that
with a claim nobody had measured: that a sandboxed Chrome process may be denied
the driver's `/dev/video*` glob. That claim is false here, and it was the only
reason for the wrapper.

### The measurement that justified the removal

The acceptance script was rewritten to launch the browser directly and to drop
`LIBVA_DRIVER_NAME`, `LIBVA_V4L2_VIDEO_PATH` and
`LIBVA_V4L2_ENCODER_PATH` from the environment, so libva has to resolve the
driver through the `msm_drv_video.so` alias the way an ordinary desktop start
does. Three runs, installed driver
`/usr/lib64/dri/v4l2_drv_video.so` SHA-256 `b2f8151d3980…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `23a37146-fa9a-4038-af0c-b6d33aa625d9`,
decoder node `/dev/video0`:

| Run | Decoder | Platform | Window | Dropped | Node holder | Driver mapped in that process |
| --- | --- | --- | ---: | ---: | --- | --- |
| direct-no-env, 15 s | `VaapiVideoDecoder` | true | 450 | 0 | `--type=gpu-process` | live sample below |
| after-removal-r2, 15 s | `VaapiVideoDecoder` | true | 449 | 0 | `--type=gpu-process` | `/usr/lib64/dri/v4l2_drv_video.so` |
| after-removal-r3, 20 s | `VaapiVideoDecoder` | true | 598 | 0 | `--type=gpu-process` | `/usr/lib64/dri/v4l2_drv_video.so` |

The first row's mapping was taken by a separate live sampler, because the
script at that point still sampled after playback stopped and recorded
`driver_mapped_in_gpu_process: null` on a run that had in fact decoded in
hardware. That was a defect in the gate, not in the driver: a GPU process
closes the node and unmaps the driver when playback ends. The script now
samples every second while the window is open, and the third column above is
what that sampling reports.

A sandboxed `--type=gpu-process` both maps this driver and holds the decoder
node with no `LIBVA_*` variable set. That is the whole claim the launcher was
making, and it holds without the launcher.

### What replaced it

The node rule is now one file, `scripts/lib/iris-node.sh`, installed as
`<prefix>/lib/iris-vaapi/iris-node.sh` and sourced by both `iris-device-check`
and `iris-verify`. It is the same rule the launcher held: an exact
`iris_driver` match, decoder and encoder told apart by card type, and a refusal
to guess a node number. `test/iris-node-check.sh` exercises it through two
seams (`IRIS_NODE_SEARCH_ROOT`, `IRIS_NODE_IS_CHAR`) that exist because an
unprivileged build host cannot `mknod`; 13 cases cover resolution, all three
refusal paths, the two overrides and the "a regular file is not a node" case,
and both diagnostics are asserted to source the rule and to name no node
number. `test/iris-browser-launcher-check.sh` is gone with the launcher; the
cases that mattered moved rather than being dropped.

Fedora Chromium's compatibility branch went with the launcher. It had no
recorded evidence behind it: the only line in this file was that Chromium
152.0.7977.75 selects `FFmpegVideoDecoder`, which is the fallback the branch
caused, not a measurement of the GMU fault it claimed to prevent.

### On the target, installed layout

A `DESTDIR` staging install of the new layout, then the uninstaller:

```
PASS installed VA driver: …/usr/lib64/dri/v4l2_drv_video.so
PASS installed driver alias: …/usr/lib64/dri/msm_drv_video.so
PASS installed node rule: …/usr/local/lib/iris-vaapi/iris-node.sh
PASS installed diagnostics: …/usr/local/bin/iris-device-check …/iris-verify …/iris-configure-clients
```

`iris-device-check` and `iris-verify` run from the installed `bin` directory
with the rule in `lib/iris-vaapi` and find both nodes (`/dev/video0` Iris
Decoder, `/dev/video1` Iris Encoder). `iris-verify` from the staging tree:

```
PASS libva opens the driver: Qualcomm Iris V4L2 stateful (libva-v4l2)
PASS driver advertises 12 profiles
PASS encode entrypoint advertised
PASS decode ran on the hardware (Iris IRQ +114)
PASS h264 encode ran on the hardware (Iris IRQ +128)
PASS h264 stream decodes (60 frames)
PASS hevc encode ran on the hardware (Iris IRQ +124)
PASS hevc stream decodes (60 frames)
PASS native V4L2 M2M decode through mpv (independent of this driver)
```

The uninstaller removed every installed file and left nothing behind. A
`DESTDIR` staging run is not an installation; no live system install was made
this round, and the installed RPM on the target is still the 2026-10-24 build
whose provenance is recorded above.

Host gates on this tree: `sh test/run-static-checks.sh` PASS,
`meson test -C build-local --print-errorlogs` 19/19 OK, `git diff --check`
clean.

### The real installation, 0.1.0-2 and 0.1.0-3

A live install, not a staging run. Two packages were built the same evening
and only the second is fit to keep; the first is recorded because it reached
the target and because what was wrong with it is the kind of defect a hash
cannot show.

**0.1.0-2 was assembled by overlaying an untracked working tree.** Its tarball
was HEAD's `src/` plus this change set copied over it. That produced a working
driver, and `src/` was verified byte-identical to HEAD first, so the
uncommitted encode work was correctly excluded. But the diagnostics
(`iris-device-check.sh`, `iris-verify.sh`, `iris-configure-clients.sh`) and
the node rule were **not tracked by git at all**, while the installer, the
Fedora spec and the PKGBUILD all referenced them. Two consequences: a clean
`git archive` of HEAD carried no installer input and would have failed to
build, and the package recorded `source_dirty=0` against a commit that did not
contain those files. The provenance field was wrong.

0.1.0-3 fixes the cause rather than the symptom: the scripts are tracked, and
the package is built from a clean `git archive` of the commit it names, with
no overlay.

| Fact | Value |
| --- | --- |
| RPM | `libva-v4l2-iris-0.1.0-3.fc44.aarch64.rpm`, Release 3 |
| Source commit recorded | `7ea57da71283085256dbb5c7b535c76635e4e4f8`, `source_dirty=0`, a clean archive that contains every shipped file |
| Installed driver | `/usr/lib64/dri/v4l2_drv_video.so`, SHA-256 `93c98565fc3373…`, 399920 bytes |
| Kernel | `7.2.6-sm8550-g42f3b40-iris1` |
| Boot ID | `23a37146-fa9a-4038-af0c-b6d33aa625d9` |
| `rpm -V libva-v4l2-iris` | clean after the upgrade |
| Installed files | `iris-device-check`, `iris-verify`, `iris-configure-clients` in `/usr/bin`, `iris-node.sh` in `/usr/lib/iris-vaapi`, both driver files, the manifest. No launcher, no desktop entry. |

The driver hash differs from the 2026-10-24 package (`b2f8151d…`) even though
`src/` is unchanged, and that is not a code difference: `meson.build` embeds
the RPM's own identity as `{"type":"rpm","name":…,"version":"0.1.0-2.fc44",…}`
in the binary, so a Release bump necessarily changes the hash. Normalizing
that one metadata string makes the two payloads byte-identical — `cmp` reports
0 differing bytes. Recording this because the hash is the field a reader uses
to tell one build from another, and here it does not mean what it looks like.

Two pieces of `/usr/local` residue from the 2026-10-24 morning install were
removed after the upgrade, neither RPM-owned and neither referenced by the new
package: `/usr/local/bin/iris-vaapi-browser`,
`/usr/local/share/applications/google-chrome-iris-v4l2.desktop`,
`/usr/local/share/iris-vaapi` (old manifest and driver backup), and an older
`/usr/bin/iris-vaapi-browser.bak` from 2026-09-20. The new uninstaller refuses
to touch a driver owned by the package manager, which is why the residue was
removed by hand rather than by running it.

Installed package, no `LIBVA_*` variable set:

```
PASS libva opens the driver: Qualcomm Iris V4L2 stateful (libva-v4l2)
PASS driver advertises 12 profiles
PASS encode entrypoint advertised
PASS decode ran on the hardware (Iris IRQ +124)
PASS h264 encode ran on the hardware (Iris IRQ +122)
PASS h264 stream decodes (60 frames)
PASS hevc encode ran on the hardware (Iris IRQ +119)
PASS hevc stream decodes (60 frames)
PASS native V4L2 M2M decode through mpv (independent of this driver)
```

Browser acceptance against the installed package, 20 s window:

```
PASS browser=Chrome/152.0.7977.82 decoder=VaapiVideoDecoder platform=True
     frames=599 dropped=2 holders=--type=gpu-process
     driver=installed:93c98565fc33 boot_unchanged=True
```

with `driver_mapped_in_gpu_process=/usr/lib64/dri/v4l2_drv_video.so`, sampled
during playback. That closes the loop: the package a user installs decodes in
Chrome with no launcher, no desktop entry, no flags and no environment.

## Distribution check and deferred hardware runs (2026-11-10)

Host-side work on `feat/encode-entrypoints`, no product-code changes. The four
Iris patches, both READMEs' known-limits text and the downstream kernel spec
were brought into agreement; the static check that binds them is new.

| Check | Result |
| --- | --- |
| `rpmspec -P kernel-sm8550/kernel-sm8550.spec` | expands; Source6-9 pin `5a0fcb58461deeb8b6392f07cb42b2804accbe9e`, Release `8.sheng.iris1.fc44` |
| `spectool -g -R` (downstream spec) | all nine sources fetched; the four patch URLs returned 200 |
| `rpmbuild -bp --nodebuginfo --nodeps` | `%prep` verified all four recorded sha256 values, then applied 0001-0004 at `--fuzz=0`; 0004 at the recorded −3-line offset; `Tag 7.2.6-mac commit: 42f3b40`, `.lkv_suffix=-sm8550-g42f3b40-iris1` |
| Applied tree | `HFI_PROP_DECODE_ORDER_OUTPUT` defined; `DISPLAY_DELAY_ENABLE`/`DISPLAY_DELAY`/`REQUEST_SYNC_FRAME` in `enum platform_inst_fw_cap_type` (not the flags enum); `DEBLOCK_MODE_H264` once; `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` handled twice in `iris_ctrls.c`; error-branch `vb2->timestamp = buf->timestamp` present |
| `test/iris-kernel-patch-check.sh` | passes with the downstream spec present, skips the cross-repo half without it, fails on a tampered `IRIS_PATCHn_SHA256` and on a URL pin that no longer names the tag commit |

`rpmbuild -ba` was not run: ten BuildRequires (bindgen, ccache, clippy,
net-tools, opencsd-devel, python3-pyyaml, rust-src, rustfmt,
systemd-boot-unsigned, systemd-ukify) are absent on this host, the workflow
allows 30–90 minutes for the aarch64 kernel build, and the host volume had
2.1 GB free. `%prep` is the stage that checks this repository's payload; the
unbuilt half is the RPM's own compile. The 266 MB source tarball and the
prepped BUILD tree were removed afterwards.

Target probes over ssh `sheng`, read-only, boot `20dcd8af-…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`:

- encoder `/dev/video1` lists `force_key_frame` and all six loop-filter
  controls, so the loaded module still carries 0002+0003.
- `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` is probed once in
  `EncodeSession::configure()`; there is no environment override that selects
  the intra-period lever. The stock-kernel promoted-IDR run therefore needs a
  module without 0003 and a reboot. It was not attempted: that changes the
  installed system and the tablet was discharging.
- The two-hour concurrency soak was not run either. Battery was 26 % and
  `Discharging` at −2.195 A at the start of this session and 23 % by the end.
  The soak guards boot ID and an 85 °C ceiling but does not check the pack.
  A charged supply is the prerequisite; the 2026-09-21 full-length soak
  (`dual-h264` and `mixed`, 172800 frames per context) remains the record for
  the earlier tree.

## Sibling backends: the 2026-09-22 deep read, closed (2026-11-10)

Source-only research against the public GitHub API and raw trees; no build,
no target access. It closes the item left open when four 2026-09-22 subagents
were killed by a billing error. Repo state checked 2026-11-10 UTC:

| Repo | HEAD | Last commit | Verdict |
| --- | --- | --- | --- |
| `CFM880/iris-vaapi` | `9852479` | 2026-10-23 | gen1 Iris (SM8150/nabu), decode-only |
| `radxa-pkg/qcom-va-driver` | `0980a1d` | 2026-10-28 | same HFI gen2 / `iris_driver` name |
| `snowf14k3/venus-vaapi-driver` | `37e1030` | 2026-10-07 | older `qcom-venus` stack |

`radxa-pkg/libva-v4l2` and `strongtz/libva-v4l2` both 301-redirect to
`radxa-pkg/qcom-va-driver` (repo id 1360998480, renamed 2026-10-26, tag
`v1.1.1`). The old URLs in our credits were stale and now name the current
repository with the rename recorded.

Findings that changed this tree:

- **Chrome 153 HEVC seek regression is resolved upstream in Chrome 156.**
  CFM880's `docs/chromium-bug-m153-hevc-seek.md` was updated 2026-10-23 to
  `Status: resolved`: Chrome `156.0.8078.47`, 4K HEVC 8-bit, no workaround
  flags, `stale_repeats = 0/445`. It also warns that a launcher wrapper
  injecting `--disable-features=ExtendedVideoBitstreamValidation` makes an A/B
  look fixed. The Chromium issue page itself was not fetched. The current
  READMEs record only the browser matrix itself: Chromium 152 uses its built-in
  software AV1 decoder for local files; Chrome 156 live pages and Firefox 158
  are verified hardware decoders.
- **Upstream now carries five kernel patches, not three.** `patches/` holds
  0001 decode-order, 0002 larger CAPTURE pools, 0003 deblocking, 0004
  error-buffer timestamps (2026-10-21) and 0005 recycle non-display decoder
  buffers for AV1 (2026-10-26). Our four still map to 0001+0002 / 0003 /
  upstream `6f62dcefd249` / 0004. The patch bodies were compared against the
  v1.1.1 tree on 2026-11-11: our 0004 and 0005 are byte-identical to upstream,
  and 0001-0003 differ only because ours merges decode-order and the larger
  CAPTURE pool into one patch, rebases onto `sheng-7.2.6` and adds two
  domain/firmware guards (`docs/POST-MERGE-BACKLOG.md` §8, F3). Upstream's 0005
  is required by their AV1 path, which replays a whole hidden picture; our
  internal DPB consumes the NOSHOW completion instead, so 0005 stays
  experimental here.

Findings that did not change this tree:

- CFM880's `99-qcom-iris-dmaheap.rules` (`MODE="0666"` on
  `/dev/dma_heap/system`) fixes Chrome black video caused by memfd fallback on
  gen1. Our Import path takes a client-exported DMA-BUF and the Chrome 152
  acceptance on this target passes 599 frames with no drops, so the udev rule
  was not added. Their line "`vainfo` succeeding does not prove the
  presentation path works" is already stronger in our gate, which requires a
  media surface reporting `VaapiVideoDecoder` with frame and drop evidence.
  Their `VPU_DIRECT_CAPTURE=1` queues the whole client pool into CAPTURE with
  permanent slot/surface association, a different ownership model from our
  one-picture-in-flight Import; their in-place VP9 resize uses a gen1 firmware
  quirk. Neither is transferable.
- Radxa's `src/vp9.cpp` answers the invisible alt-ref with a synthetic VP9
  superframe showing the refreshed slot, and refuses `show_existing`. A
  genuinely different answer to the same alt-ref problem; our translator
  appends a `show_existing_frame` header instead and qualifies FFmpeg at
  515/515 with `misplaced=0`. `src/decoder.cpp` has no Import path
  (CAPTURE is MMAP + EXPBUF; imported client surfaces are copied into) and
  refuses in-context DRC at line 389, which this tree qualifies. Nothing to
  adopt.
- `snowf14k3/venus-vaapi-driver` is plain C on the `qcom-venus` stack: no
  kernel patches, no decode-order control, no Import, CPU row-copy of every
  frame into MSM GEM surfaces. Its env-gated experimental HEVC and
  `ENUM_FMT`-derived capabilities are already covered by our evidence-before-
  capability rule. Leaving it unmentioned was correct.

Not verified: no runtime or hardware behaviour, no patch-body diff, no read of
CFM880's companion `nabu-iris` module, and the pre-rename `strongtz` history
could not be separated from the shared renamed history.

## Docs, cross-repo check and fork rename (2026-11-10)

Host-side work on `feat/encode-entrypoints`. No product code, capability,
profile or publish policy changed; `data/iris-codec-capabilities.json`,
`packaging/kernel/000[1-4]-*.patch` and `SHA256SUMS` are byte-identical to
`HEAD` (four `sha256sum -c` OK). The four patch files and their hashes are
hash-pinned by the downstream spec and the `iris-kernel-patches-v1` tag, so
this batch does not touch them and creates no new tag: the tag still resolves
to `5a0fcb58461deeb8b6392f07cb42b2804accbe9e` and the four
`%global IRIS_PATCHn_SHA256` values are unchanged.

Three small fixes to `test/iris-kernel-patch-check.sh`'s cross-repo half
(added earlier in this working tree):

- The final PASS message was an AND-list as the last command of the `if`
  branch. It does not abort under this host's `/bin/sh` (bash), but it is
  fragile on dash-based CI or if the trailing lines move; it is now a plain
  `if ... then printf ... fi`.
- The stale experimental clone
  `<scratch>/sheng-fedora-packages/...` was a second fallback
  candidate. It pins `8dd655e…` with `phxinyang/` URLs and produced four
  false URL-pin failures. Only the explicit `IRIS_DOWNSTREAM_SPEC` override
  and the sibling `../sheng-fedora-packages` checkout remain.
- A spec with zero `Source6-9` lines now fails instead of silently passing.
  `packaging/kernel/README.md` documents the same resolution rules.

Verification of that check, fixtures under `<scratch>/iris-check-fixtures`:

| Fixture | Exit | Result |
| --- | --- | --- |
| canonical spec, `IRIS_DOWNSTREAM_SPEC` set | 0 | both PASS lines |
| `IRIS_PATCH1_SHA256` zeroed | 1 | one hash-mismatch FAIL |
| four URLs pinned to `8dd655e…` | 1 | four URL-pin FAILs |
| `Source6-9` lines deleted | 1 | `downstream spec has no Source6-9 patch URLs` |
| no override, no sibling (CI shape) | 0 | `NOTE ... skipped`, cross-repo half skipped |

`docs/architecture.md` §8 gained the missing `publish=invisible` record,
matching `src/iris/session.cc` and the five `publish=invisible` counts in
this file; no consumer parses it, so no other file moved. `sh -n`,
`sh test/run-static-checks.sh`, `meson test -C build-local` (20/20) and
`git diff --check` are green.

Fork rename: `FFT-infra/qualcomm-iris-vaapi` → `FFT-infra/sheng-video-accel`
(GitHub repo name and description only; the base repo
`phxinyang/qualcomm-iris-vaapi` is not renamed). New description:

> Video acceleration for sheng (Xiaomi Pad 6S Pro / SM8550) on ARM64 Linux:
> libva VA-API backend for the Qualcomm Iris V4L2 decoder and encoder, four
> qcom-iris kernel patches, and Fedora/Arch packages. Hardware decode/encode
> in Chrome/Firefox/Electron, zero-copy H.264/HEVC decode. Qualified on this
> one tablet; shared by all its VA-API clients.

Both READMEs' titles and lead paragraphs now name `sheng` / Xiaomi Pad 6S Pro
(SM8550) and state the single qualified target and the device-only evidence
boundary; they make no cross-SoC claim. Package identities
(`meson.build` `project('libva-v4l2')`, `libva-v4l2-iris`,
`qualcomm-iris-vaapi/system-install-v1`) and the packaging `URL:`/`url=` are
untouched. GitHub redirects old-name raw URLs (verified with the
`strongtz/libva-v4l2` → `radxa-pkg/qcom-va-driver` precedent), and the check
validates only the `/5a0fcb5…/packaging/kernel/` commit segment, so no
re-tag or re-hash was needed.

The downstream spec repo `sheng-fedora-packages` (separate checkout, local
commit `3b202951de801dce2e94a46ebabfb130295b928a`, **not pushed**) changes only
the four `Source6-9` URL owner/repo substrings to `FFT-infra/sheng-video-accel`;
the commit pin, the four hashes, `Release` and all `%changelog` entries stay
verbatim. `rpmspec -P` expands, the positive kernel-patch check still passes
against the edited spec, and `git diff --check` is clean.

PR #1 remains `state=OPEN`, `isDraft=true`, `mergeable=MERGEABLE`,
`mergeStateStatus=CLEAN`, head `FFT-infra:feat/encode-entrypoints`. Its
`pull_request` CI runs still need base-repo admin approval (cross-repo fork
PR `action_required`); the permissions fact is recorded in the section
"CI: the inventory check failed on the meson version (2026-10-24)" and is not
changed here. Fork `push` runs are green.

The short-window, low-power browser acceptance for this date is recorded in
the following commit.

## Short-window browser acceptance — blocked (2026-11-10)

The low-power gate failed at execution time, so the run was skipped. A
read-only ssh probe of `sheng` reported
`STATUS=Discharging CAPACITY=100 CURRENT_NOW=-1798000` from
`/sys/class/power_supply/qcom-battmgr-bat`, boot
`20dcd8af-e763-4142-9ae7-85f7f3ac0ed7`, kernel
`7.2.6-sm8550-g42f3b40-iris1`. Charge level is high, but the pack is on
battery and draining at ~1.8 A; the locked precondition for this batch was
"only short-window, low-power, check battery first", with the earlier
two-hour soak explicitly deferred on the same charged-supply prerequisite.
An earlier read-only probe in this session had shown `Charging 82%`; the
state changed before the run, which is why the gate is re-checked rather
than assumed.

No browser was launched, no `LIBVA_*` override was set, no kernel module was
swapped, and the tablet was not rebooted. This section is a blocked-run
record, not hardware evidence: it makes no decode, frame or drop claim and
does not qualify any codec, profile or publish policy. The installed driver
(`/usr/lib64/dri/v4l2_drv_video.so`, with the `msm_drv_video.so` alias) and
the 1080p H.264 clip on the target are unchanged. The 2026-09-21 full-length
soak (`dual-h264` and `mixed`, 172800 frames per context) remains the
standing concurrency record, and the 2026-10-24 Chrome 152 acceptance runs
remain the standing browser-decode record. No rerun is attempted in this
batch; a charged supply is the prerequisite for a later short window.


## AV1 Import qualification, regression A/B and real-site browser runs (2026-11-10)

Target: `sheng`, boot `2d5eed8d-57d7-4746-9e10-2cda0f4fe768` (unchanged
before and after every run below), kernel `7.2.6-sm8550-g42f3b40-iris1`,
booted with the 0001-0004 `qcom-iris` module only. The decoder node was
resolved at runtime by `iris_driver` QUERYCAP as `/dev/video17`; the encoder
was `/dev/video18`. Chrome was `Chrome/156.0.8078.79`. No `LIBVA_*`,
`V4L2_VA_PUBLISH`, `V4L2_VA_ZERO_COPY` or node override was set for the
installed-package runs.

Installed driver: `/usr/lib64/dri/v4l2_drv_video.so` and its
`msm_drv_video.so` alias, SHA-256
`da952fe4fe35896e215976213a1f5b6836cc5aa9ab9d97a1c08f55a909615da3`, size
9120088. `/usr/local/share/iris-vaapi/install-manifest.txt` records
`source_commit=079480d8b208c5bb6695d4e91e6c1953c4d9010c`,
`source_dirty=0`, `tracked_source_sha256=d42c37b46ca54d3c645940f59d6ef81033199f1480b735442d8901d3ab547460`,
`installed_at_utc=2026-11-10T01:15:14Z` and the same artifact hash. This is a
live system install, not a `DESTDIR` stage. The installed source is commit
`079480d8b208c5bb6695d4e91e6c1953c4d9010c`; the later
`54541f051c23d245518722eca01249a13eb6e766` commit changes TEST-RESULTS.md
only and does not affect the driver. The earlier `source_dirty=1` install
(`565073d3…`) was the same code with a stale comment and was replaced by the
installer. The installer backed up that
previous `565073d3…` driver. The inherited broken build
(`c4a2c267af1af1ef6d3994eb1edb6259d29f3c658dc1bb5ae863cb951c3e90e4`)
was backed up by the first install of this batch. `vainfo` resolves the driver through the alias
without `LIBVA_DRIVER_NAME`.

### Regression found in the inherited branch

The inherited rewrite replaced the AV1 held-picture translator with a dynamic
8-slot DPB, added `detached`/`shared_ptr` lifetime machinery and a UBWC
modifier path, and claimed kernel patch 0005 recycled NOSHOW buffers. The
loaded module was built from 0001-0004 (module timestamp 2026-10-22); 0005
and 0006 were never loaded. `detached` was never set true. The two surfaces
that decide qualification disagree with those claims if 0005 is loaded: it
swallows the NOSHOW completions this userspace publishes by decode order.

A/B on the same target, kernel, clips and 48-frame matrix:

| Build | 48-frame four-codec matrix | 1280x720 AV1 real clip | Result |
| --- | --- | --- | --- |
| inherited rewritten build | 48/48 exact framemd5 for all four codecs | 33/120 frames under `auto`, 39/120 under `import`; each with 2 `publish=error` | fails to decode a real AV1 stream |
| committed HEAD (`3ef2e70`) | 48/48 exact | 120/120 exact framemd5, 0 errors | correct |
| HEAD plus only `V4L2_PIX_FMT_AV1` in `import_formats` | 48/48 exact | 120/120 exact framemd5, 120 `publish=import`, 0 errors/drops | AV1 zero-copy works |

The synthetic matrix did not expose the regression; the real clip did. All
twelve rewritten files were reverted to HEAD. The kept change is the one-line
Import enabler in `src/va/driver.cc` plus the capability/docs updates in the
same commit. At the time of this 2026-11-10 run, kernel patches 0005/0006
were unvalidated experimental assets; 0006 later had one separate guarded WP1
run, recorded below.

### Installed-package decode evidence

`test/iris-matrix.sh` on the installed PR-head driver, with
`LIBVA_DRIVERS_PATH=/usr/lib64/dri` pointing at the installed package:
`auto` and `V4L2_VA_PUBLISH=import` each produced 48/48 exact framemd5 for
H.264, HEVC, VP9 and AV1 (`MATCH` against the software baseline for all eight
pairs). Real 1280x720 AV1 clip: 120/120 exact framemd5, trace
`120 publish=import`, `session finish submitted=120 completed=120
errors=0 drops=0 timeouts=0 drains=1 misplaced=0`. In-context DRC clip
`resize-av1.mkv` (1280x720 -> 854x480): 80/80 exact framemd5, 84 completions,
`42 publish=import`, `1 publish=invisible`, `42 publish=copy-gpu` after the
resolution switch, `errors=0 drops=0 misplaced=0`. Import is qualified for
AV1 on the 0001-0004 decode-order kernel; VP9 stays Copy.

Local-file Chrome acceptance on the installed PR-head driver used the AV1
1280x720 clip: `VaapiVideoDecoder`, platform flag `true`, 282 decoded frames /
0 dropped over a 12-second window, GPU process held `/dev/video17` and mapped
`/usr/lib64/dri/v4l2_drv_video.so`, `hardware_decode=true`, trace 433
`publish=import` and 6 `publish=drop`. An earlier control on the
`565073d3…` install reported 192 frames / 0 drops over 8 seconds. Four-codec
local acceptance runs (H.264, AV1, HEVC, VP9) on the build-tree override
`d6d583b7d9f049ae6abbad5202a225486af3a96a67c380b00281fed3d8ac108e` each
reported 240 frames / 0 drops. An `auto`-policy AV1 Chrome run produced 372
`publish=import`, 6 `publish=drop` and 183 noshow/invisible completions.

### Real YouTube and Bilibili media streams

Chrome's default navigation on this tablet hung at `about:blank` even for
plain `http://127.0.0.1`; `curl`/Python resolve and fetch normally through
Tailscale MagicDNS and the Mihomo TUN. The netlog showed CONNECT, TLS
handshake and certificate verification succeeding while the main-frame
request was never sent. The cause was the cookie store waiting on the system
keychain: `--password-store=basic --use-mock-keychain` makes both real pages
load and play with no proxy at all (control runs: YouTube 4 decoded frames /
7 `publish=import`, Bilibili 389 frames / 399 `publish=import`). The
collected 10-second runs used a local HTTP/CONNECT relay that resolves
through the system resolver; it only forwards bytes and does not modify the
site or the video. All of these runs use the installed driver with no
`LIBVA_*` override.

`yt-dlp` downloaded unmodified real-site video-only streams, then
`test/iris-browser-acceptance.sh` played each through the working Chrome path
on the installed PR-head driver. All five passed with
`hardware_decode=true`, `decoder_name=VaapiVideoDecoder`,
`is_platform_video_decoder=true`, 300-301 decoded frames / 0 dropped over a
12-second window, and the GPU process holding `/dev/video17`:

| Site / format | Resolution | Codec | Window frames / drops |
| --- | --- | --- | --- |
| YouTube `dQw4w9WgXcQ` format 397 | 854x480 | AV1 | 300 / 0 |
| YouTube `dQw4w9WgXcQ` format 247 | 1280x720 | VP9 | 300 / 0 |
| Bilibili `BV1GJ411x7h7` format 100024 | 1280x720 | AV1 | 300 / 0 |
| Bilibili `BV1GJ411x7h7` format 100111 | 1280x720 | HEVC | 300 / 0 |
| Bilibili `BV1GJ411x7h7` format 30064 | 1280x720 | H.264 | 301 / 0 |

The earlier 20-second runs on the `565073d3…` install reported 501 frames / 0
drops for each of the same five streams, with 664/647/643/650 `publish=import`
for the four Import codecs and 708 `publish=copy-gpu` for VP9.

Live pages on the installed PR-head driver, collected through CDP Media and
the driver trace over a 10-second window:

| Site | Decoder / platform | Window frames / drops | Codec | Trace |
| --- | --- | --- | --- | --- |
| `https://www.youtube.com/watch?v=dQw4w9WgXcQ` | `VaapiVideoDecoder`, `true` | 251 / 0 | AV1, 854x480 -> 1920x1080 during the window | 547 `publish=import`, 8 `publish=drop` |
| `https://www.bilibili.com/video/BV1GJ411x7h7` | `VaapiVideoDecoder`, `true` | 250 / 0 | HEVC 640x360 | 742 `publish=import`, 3 `publish=drop` |

`stats_first`/`stats_second` for YouTube were 287 -> 538 decoded frames with
`dropped=0` at both ends; Bilibili was 481 -> 731 with `dropped=0`. Both GPU
processes held `/dev/video17`, the page URLs and titles came from the real
sites, and `va init` resolved `/dev/video17` without an override. YouTube's
adaptive player opened a second AV1 session at 1920x1080 while the page was
playing; the first session's drain reports
`submitted=442 completed=439 errors=1 drops=2 timeouts=0 misplaced=0`. Both
surfaces fail closed on that abandoned old session; the live 10-second window
still reports 0 dropped frames and the second session publishes through
Import. No `publish=error` appears in either browser log. The `publish=drop`
records are fail-closed results for abandoned client surfaces in a live
streaming session. These runs qualify the AV1 Import policy and record
real-site hardware decode; they do not change the VP9 Copy policy.

## Real AV1 clip gate and the held-picture / DRC contract (2026-11-10)

These runs add a hardware gate for real AV1 clips and record two behaviours
that the earlier matrix could not distinguish. They do not change the
installed driver.

Target: `sheng`, boot `2d5eed8d-57d7-4746-9e10-2cda0f4fe768` (unchanged
across the runs), kernel `7.2.6-sm8550-g42f3b40-iris1` with the 0001-0004
module. The installed package was used, with no `LIBVA_DRIVER_NAME`,
`LIBVA_DRIVERS_PATH`, `V4L2_VA_PUBLISH` or node override: SHA-256
`da952fe4fe35896e215976213a1f5b6836cc5aa9ab9d97a1c08f55a909615da3`, manifest
`source_commit=079480d8b208c5bb6695d4e91e6c1953c4d9010c`, `source_dirty=0`,
installed at `2026-11-10T01:15:14Z`. The decoder resolved to `/dev/video17`.
The tablet was discharging at 60% with no USB supply; these are short clip
runs, not a soak or a power measurement.

`test/iris-real-clip-gate.sh` was installed into a scratch tree on the target
and run with `IRIS_REAL_CLIP_DIR` pointing at the existing 120-frame AV1 clip
and the 80-frame in-context DRC clip plus their software framemd5 baselines.
The gate decodes through the installed VA driver and calls
`test/iris-clip-trace-check.sh` on the resulting framemd5 and driver trace.

| Clip | Framemd5 | Session | Publish |
| --- | --- | --- | --- |
| `av1-720.webm`, 1280x720 | 120/120 exact | `submitted=120 completed=120 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | 120 `publish=import` |
| `resize-av1.mkv`, 1280x720 -> 854x480 | 80/80 exact | `submitted=84 completed=84 errors=0 drops=0 timeouts=0 drains=2 misplaced=0` | 42 `publish=import`, 42 `publish=copy-gpu`, 1 `publish=invisible` |

Both traces have zero `sync timeout token=` and zero `va sync released the
held picture` lines. `vaDestroyContext` releases the AV1 held picture before
the session drains (`src/va/picture.cc`), so the file decode reaches EOS
without the bounded-sync release. VA-API has no separate flush entry point; a
client that syncs each picture and keeps its context alive can still take the
bounded-sync path in `src/va/surface.cc`, which is now documented as a client
contract rather than a decoder defect.

The DRC trace records `import rejected: client layout differs from the
CAPTURE layout (client stride=1280 chroma=942080 size=1413120; capture
stride=896 chroma=430080 size=647168)`, then configures the rebuilt CAPTURE
pool with `memory=mmap`. The client kept exporting surfaces at the old
1280x720 geometry after the change, so an Import pool would write 896-pitch
pixels into a buffer the client reads at 1280 pitch. The GPU copy is the
fail-safe publication for those pictures. `test/iris-clip-trace-check.sh`
accepts the copy half only when that layout-mismatch line is present; a copy
without the recorded reason fails the gate.

On the host, `sh test/run-static-checks.sh` passed, `meson test -C
build-local --print-errorlogs` reported 22/22, and `git diff --check` was
clean. The new host-only tests are `av1-translator` (held-picture release at
EOS, including refusing a stream that continues after guessed refresh flags)
and `iris-clip-trace-check-self-test` (truncated framemd5, pixel mismatch,
lost completion, unexpected copy, sync-bound release, declared and
unexplained DRC copies). CI's required-test list names both. The checker was
also run against the earlier PR-head traces: both passed, and mutating one
`publish=import` to `publish=copy-gpu` made it fail.

## Short guarded 4K AV1 Import probe (2026-11-10)

This is a 30-frame capability probe, not a 4K streaming, thermal, power or
long-window result. The tablet stayed on battery throughout: at the time of
the guard run it was discharging at 60% with `qcom-battmgr-usb online=0` and
`ucsi-source-psy-pmic_glink.ucsi.01 online=0`; a later check read 58% with
`current_now=-213000`. `test/iris-power-sampler.sh` and the long-window soak
remain deferred until a charger and a screen-off state are available.

The clip was generated on the host with SVT-AV1 v3.1.2 as 3840x2160, 30
frames, AV1 Main profile, 8-bit `yuv420p` (preset M9, CRF 40), then decoded
with a software decoder to produce `av1-4k-sw.md5` (30 frame hashes,
`#software: Lavf62.12.102`). The gate never generates its own oracle. The
same installed package was used, with no `LIBVA_*`, publish or node override:
SHA-256 `da952fe4fe35896e215976213a1f5b6836cc5aa9ab9d97a1c08f55a909615da3`,
manifest `source_commit=079480d8b208c5bb6695d4e91e6c1953c4d9010c`,
`source_dirty=0`, installed `2026-11-10T01:15:14Z`. Boot
`2d5eed8d-57d7-4746-9e10-2cda0f4fe768`, kernel
`7.2.6-sm8550-g42f3b40-iris1` with the 0001-0004 module; the decoder resolved
by `iris_driver` QUERYCAP to `/dev/video17`, card type `Iris Decoder`.

The decode ran under `scripts/iris-experiment-guard.sh` with
`IRIS_EXPERIMENT_TIMEOUT_SECONDS=180` and
`IRIS_EXPERIMENT_MAX_TEMP_MILLIC=85000`. The guard recorded
`status=complete`, unchanged boot ID, started `2026-11-10T05:58:59Z` and
finished `2026-11-10T05:59:29Z`; no reset or thermal breach, hottest sample
39800 millidegrees C (39.8 C). The guard captures the child's stderr into
`guard/command.stderr`, so that file is the driver trace for this run and the
`out/driver.trace` written by the ffmpeg wrapper is empty.

Driver trace: `capture configure 3840x2176 stride=3840 size=12533760
visible=3840x2160 pool=32 memory=import firmware-min=4`, 30
`publish=import`, and `session finish submitted=30 completed=30 errors=0
drops=0 timeouts=0 drains=1 misplaced=0`. Zero `sync timeout token=` and zero
`va sync released the held picture` lines. The VA framemd5 has 30 frame
hashes with `#software: Lavf62.12.103`; the hash lines match the host software
baseline exactly. `test/iris-clip-trace-check.sh` now compares only `^0,`
frame-hash lines because the libavformat version in the header differs
between the host that wrote the baseline and the target that decoded it; the
host-only self-test covers that header-only difference as a passing case.

`IRIS_REAL_CLIP_SET=4k` runs only this lane in
`test/iris-real-clip-gate.sh`; `all` still runs the 720p and DRC clips. Both
lanes were re-run on the target with the final checker and its scratch root
set to the project's `<scratch>`:

| Lane | Framemd5 | Session | Publish |
| --- | --- | --- | --- |
| `av1-4k.mkv`, 3840x2160 | 30/30 exact | `submitted=30 completed=30 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | 30 `publish=import` |
| `all` (`av1-720.webm`, `resize-av1.mkv`) | 120/120 and 80/80 exact | as recorded in the previous section | 120 import; 42 import + 42 copy-gpu + 1 invisible |

Both gates printed `PASS real AV1 clip gate set=4k publish=import` and `PASS
real AV1 clip gate set=all publish=import`. On the host, `sh
test/run-static-checks.sh` passed, `meson test -C build-local
--print-errorlogs` reported 22/22, and `git diff --check` was clean.

## Firefox 158 browser decode and the AV1 held-picture client contract (2026-11-10)

These runs add Firefox 158.0.1 to the browser evidence and re-run Chrome on
the same live pages. They do not change the installed driver. Target: `sheng`,
boot `2d5eed8d-57d7-4746-9e10-2cda0f4fe768` throughout, kernel
`7.2.6-sm8550-g42f3b40-iris1` with the 0001-0004 module, installed package
SHA-256 `da952fe4fe35896e215976213a1f5b6836cc5aa9ab9d97a1c08f55a909615da3`
(`source_commit=079480d8b208c5bb6695d4e91e6c1953c4d9010c`, `source_dirty=0`),
no `LIBVA_*`, publish or node override. The decoder resolved by
`iris_driver` QUERYCAP to `/dev/video17`, `Iris Decoder`. Chrome was
`156.0.8078.79`. The tablet was discharging (capacity fell from 58% to 54%
during the runs, `usb=0`); these are short 10-second windows, not a soak or a
power measurement.

Firefox 158 has no CDP, so the evidence is a WebDriver BiDi
`getVideoPlaybackQuality()` frame/drop delta over a 10-second window, Firefox's
own `MOZ_LOG` (`PlatformDecoderModule`, `FFmpegVideo`, `MediaFormatReader`),
and the driver's `V4L2_VA_TRACE=1` ledger. The harness lives in
`<scratch>/ff-final-20261110/` on the target. Firefox was launched on
Wayland with `media.hardware-video-decoding.force-enabled=true` and
`MOZ_DISABLE_RDD_SANDBOX=1`; the real-site runs use the same local HTTP/CONNECT
relay as the Chrome runs and do not modify the site or the video. No
`V4L2_VA_SYNC_TIMEOUT_MS` override was left in the harness: a longer timeout
cannot release a picture that the same client thread must submit next.

| Browser / page | Codec | Frame/drop window | Decoder evidence | Driver trace |
| --- | --- | --- | --- | --- |
| Firefox live YouTube | AV1 1920x1080 | 250 / 0 (first run 263 / 56) | `av1`, `hw:"true"` then `hw:"false"`; `NotifyError: NS_ERROR_DOM_MEDIA_DECODE_ERR ... avcodec_send_packet error: Input/output error` | 11 `publish=direct`, `submitted=11 completed=11 errors=0 drops=0`, `libva error: endPicture: AV1 stream continued after a picture was released at download time` |
| Firefox live YouTube, `media.av1.enabled=false` | VP9 1920x1080 | 250 / 0 | `IsHardwareAccelerated=true`, 9 `VA-API FFmpeg init successful` | 518 `publish=direct`, 44 `publish=invisible`, no decode error |
| Firefox live Bilibili | HEVC 640x360 | 251 / 0 | `hevc`, `ffmpeg video decoder (RDD remote)`, `hw:"true"` | 498 `publish=direct`, no decode error |
| Firefox local 1080p H.264 clip | H.264 | 303 / 3 | `h264`, `hw:"true"` | 381 `publish=direct`, no decode error |
| Firefox local VP9 / HEVC clips | VP9 / HEVC | 246 / 7 and 253 / 44 | `IsHardwareAccelerated=true` / `hw:"true"` | 297 and 308 `publish=direct`, no decode error |
| Chrome live YouTube (control) | AV1 1920x1080 | 250 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true` | 725 `publish=import`, 3 `publish=drop`, GPU process holds `/dev/video17` |
| Chrome live Bilibili (control) | HEVC 640x360 | 251 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true` | 761 `publish=import`, GPU process holds `/dev/video17` |

Firefox's AV1 failure is the held-picture client contract, not a firmware
reset: the driver trace shows no `session finish` with errors, no thermal or
boot change, and two independent YouTube runs produce the same sequence.
Firefox syncs each submitted surface from its decoder thread. The AV1
translator holds one picture until the next access unit names the DPB slots it
refreshed; that next access unit cannot arrive while the client waits on the
current surface. After the bounded-sync timeout, `src/va/surface.cc` releases
the held picture with guessed refresh flags, and the next real picture is
refused by `src/codec/av1.cc` (`AV1 stream continued after a picture was
released at download time`). Firefox then falls back to software AV1 and the
window still reports 0 dropped frames; the fail-closed error is the reason the
hardware AV1 claim cannot be made for this client. Disabling AV1 makes YouTube
use VP9 hardware decode with 0 dropped frames, and Bilibili's HEVC page is
hardware-decoded with 0 dropped frames. Chrome's live AV1 page does not take
that sync path and reports 725 Import publications with 0 dropped frames in
the same batch.

`src/va/driver.cc` continues to expose `VAProfileAV1Profile0`; the capability
table's AV1 `production: supported` records FFmpeg and Chrome hardware
evidence, not a per-client guarantee. This section describes the older
held-picture path: the default internal DPB has since removed it, and the
current READMEs and `data/iris-codec-capabilities.json` describe the internal
DPB instead of a Firefox synchronous-sync boundary. The 2026-11-11 installed
package and browser gates below supersede the earlier per-client conclusion.

## Internal-DPB AV1 installed-package and browser gates (2026-11-11)

This run qualifies the default internal eight-slot DPB translator as the
installed package, replacing the held-picture path whose Firefox 158 AV1
failure is recorded in the section above. Target: `sheng`, boot
`7a26df99-f7ac-4148-9c80-135ba3adc661` throughout, kernel
`7.2.6-sm8550-g42f3b40-iris1` with the 0001-0004 module (no kernel change or
reload in this run). The decoder resolved by `iris_driver` QUERYCAP to
`/dev/video17`, `Iris Decoder`. The installed package is
`/usr/lib64/dri/v4l2_drv_video.so` SHA-256
`e97a861f8b66aa3297c22a438fa5ac6620e0c347ed147c6398deb220c29b0478`,
`source_commit=a34d348f2354041433d7476abb621cc435218ae6`,
`source_dirty=0`,
`tracked_source_sha256=bfd9ccf12b796f8a83fe53b49f2d5b5ae1f611bd8974bf129d1bd610cd18da57`.
The build came from a clean tree containing only the 154 tracked files of that
commit; no build-tree `LIBVA_DRIVERS_PATH` override was used for the gates.
Rollback snapshot:
`<scratch>/rollback-3-20261110T182904Z/v4l2_drv_video.so` (previous
installed SHA-256
`84a329258baf6b2fdf2e1da8f1038b35a35b3e159637238daf55982da1daeb43`), plus the
installer's own `/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`.

Host and target build gates: `sh test/run-static-checks.sh` rc=0,
`meson test -C build-local --print-errorlogs` 25/25,
`git diff --check` rc=0; the target's clean-tree build also ran 25/25 and
`ldd -r` reported no undefined symbols. The new `trace` test exercises 1600
concurrent records after the fix for interleaved trace lines.

Installed-package hardware gates, each under
`scripts/iris-experiment-guard.sh` with `IRIS_EXPERIMENT_DIR` under
`<scratch>` and `V4L2_VA_PUBLISH=import`. Artifacts:
`<scratch>/gates5-20261110T182908Z/`. All three guards ended
`status=complete`, no reset or thermal breach (battery 74->73%, peak zone
37000 millic, boot ID unchanged).

| Gate | Result |
| --- | --- |
| Four-codec matrix, 48 frames, Import | H.264/HEVC/VP9/AV1 framemd5 EXACT; AV1 `submitted=48 capture=48 drain=1 timeouts=0 drops=0 noshow=23` |
| Real 1280x720 AV1 clip | 120/120 frames, `submitted=120 completed=120 import=120 copy=0 invisible=42`, framemd5 EXACT |
| 1280x720 -> 854x480 in-context DRC clip | 80/80 frames, `submitted=84 completed=84 import=42 copy=42 invisible=63`, framemd5 EXACT, recorded `import rejected: client layout differs from the CAPTURE layout` |

The DRC run on the clean `e5c8fae` install exposed one trace-writer defect,
not a decode failure: `publish=invisibleva end_picture` spliced two records,
so the checker counted 62 instead of 63 while framemd5 was already EXACT.
`a34d348` makes each record one atomic `fprintf` under a process-wide mutex;
the rerun above counts 63 and passes. Completion accounting for the same
branch is from `e5c8fae`: `Target::invisible` consumes the first NOSHOW half
of a two-completion install unit so the second half publishes the client
picture even when both halves are NOSHOW, and a NOSHOW LAST marker at a DRC
boundary is invisible rather than a decode error.

Browser evidence uses the installed package with no `LIBVA_*`, publish or node
override. The local relay on 127.0.0.1:8899 provides the system resolver path
for live sites; `WAYLAND_DISPLAY=wayland-1`, `XDG_RUNTIME_DIR=/run/user/1000`.
Chrome is `156.0.8078.79`, Firefox `158.0.1`, Chromium `152.0.7977.75`. A
browser decode counts as hardware only where a media surface reports the
selected decoder with a frame/drop window.

| Browser / page | Codec | Frame/drop window | Decoder evidence | Driver trace |
| --- | --- | --- | --- | --- |
| Firefox local 1280x720 AV1 clip | AV1 | 243 / 3 | `VA-API FFmpeg init successful`, `IsHardwareAccelerated=true` | 272 `publish=direct`, 84 `publish=invisible`, `va init node=/dev/video17`, no errors |
| Firefox live YouTube | AV1 / VP9 1920x1080 | 114 decoded at second sample (first sample had no video element) | `av1`, `VA-API FFmpeg init successful` | 129 `publish=direct`, 98 `publish=invisible`, no decode error |
| Firefox live Bilibili | AV1 640x360 | 251 / 0 | `av1`, `VA-API FFmpeg init successful` | 447 `publish=direct`, 201 `publish=invisible`, no decode error |
| Firefox live YouTube, `media.av1.enabled=false` (control) | VP9 1920x1080 | 251 / 0 | `vp9`, `VA-API FFmpeg init successful` | 488 `publish=direct`, 42 `publish=invisible`, no decode error |
| Chrome local 1280x720 AV1 clip | AV1 | 287 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`, GPU process holds `/dev/video17` | driver `installed:e97a861f8b66...`; see `<scratch>/browser-clean-final/chrome-local` |
| Chrome live YouTube | AV1 1920x1080 | 251 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`, GPU process holds `/dev/video17` | 662 `publish=import`, 496 `publish=invisible`, `session finish submitted=442 completed=442 errors=0 drops=0 timeouts=0 drains=1 misplaced=0`; 4 `publish=drop` are released-surface completions after a decoder switch |
| Chrome live Bilibili | AV1 640x360 | 251 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`, GPU process holds `/dev/video17` | 750 `publish=import`, 342 `publish=invisible`, 3 `publish=drop` from released surfaces |
| Chromium local AV1 (control) | AV1 | 288 / 0 | `Dav1dVideoDecoder`, `kIsPlatformVideoDecoder=false` | software decoder; Chromium 152 does not select VA-API for this local file |

The earlier Firefox AV1 failure is resolved: the same client that stalled at
2 decoded frames now decodes 243 frames in a 10-second window with hardware
decoder evidence and no held-picture error. Firefox local AV1 shows 3 dropped
frames in the window; the live-page windows report 0. Firefox's own
`MOZ_LOG` and the driver trace both name the hardware decoder, and a live
`/proc` fd scan during local playback found a Firefox content process holding
`/dev/video17` while `publish=direct` ran. Chromium 152's software AV1 choice
is a browser-side decoder selection, so no per-client AV1 claim is made for
it; the Firefox, Chrome and FFmpeg evidence above is the qualification.

Artifacts: `<scratch>/gates5-20261110T182908Z/`,
`<scratch>/ff-final-20261110/ff-av1-clean-1/`,
`ff-av1-clean-2/`, `ff-av1-clean-3/`, `ff-clean-youtube/`,
`ff-clean-bilibili/`, `ff-clean-youtube-noav1/`,
`<scratch>/site-final-20261110/final-clean-chrome-youtube/`,
`final-clean-chrome-bilibili/`,
`<scratch>/browser-clean-final/`.

## Complete AV1 browser matrix (2026-11-11)

The one-off browser probes above are now a repo-owned, repeatable matrix:
`test/iris-av1-browser-matrix.sh` with `test/iris-firefox-media.py`
(WebDriver BiDi + MOZ_LOG), `test/iris-chrome-media.py` (CDP Media domain)
and a generated corpus (`<scratch>/av1-browser-corpus/`). Each case
launches the installed package directly, with no `LIBVA_*`, publish or node
override, on Wayland; a 30-second window reports decoded/dropped frames, the
selected decoder, the driver's `V4L2_VA_TRACE=1` ledger and a live `/proc` fd
scan for the Iris node. Local clips carry libdav1d software framemd5 baselines.
Live sites use the local 127.0.0.1:8899 relay. Chrome 156.0.8078.79,
Firefox 158.0.1, driver `e97a861f8b66aa3297c22a438fa5ac6620e0c347ed147c6398deb220c29b0478`,
`source_commit=a34d348f2354041433d7476abb621cc435218ae6`, `source_dirty=0`:
the matrix adds test scripts only and does not rebuild the driver. Boot
`7a26df99-f7ac-4148-9c80-135ba3adc661` unchanged, no reset, no thermal breach
(peak zone 46100 millic, battery 72 -> 100% while charging).

| Case | Browser | Decoded / dropped (30 s) | Decoder evidence | Driver evidence |
| --- | --- | --- | --- | --- |
| 720p local | Firefox | 724 / 5 | `IsHardwareAccelerated=true`, VA-API init | 929 `publish=direct`, 0 errors |
| 720p local | Chrome | 718 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| 1080p local | Firefox | 726 / 7 | `IsHardwareAccelerated=true`, VA-API init | 929 `publish=direct`, 0 errors |
| 1080p local | Chrome | 714 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| 4K local | Firefox | 732 / 13 | `IsHardwareAccelerated=true`, VA-API init | 925 `publish=direct`, 0 errors |
| 4K local | Chrome | 702 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| 10-bit 720p local | Firefox | 726 / 7 | `IsHardwareAccelerated=true`, VA-API init | 934 `publish=direct`, P010 surfaces |
| 10-bit 720p local | Chrome | 717 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| 4x2 tiles local | Firefox | 725 / 5 | `IsHardwareAccelerated=true`, VA-API init | 925 `publish=direct`, 0 errors |
| 4x2 tiles local | Chrome | 718 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| dense alt-ref local | Firefox | 724 / 5 | `IsHardwareAccelerated=true`, VA-API init | 991 `publish=direct`, 0 errors |
| dense alt-ref local | Chrome | 717 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| live YouTube | Firefox | 750 / 1 | AV1/VP9, VA-API init | 924 `publish=direct`, 0 errors |
| live YouTube | Chrome | 752 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| live Bilibili | Firefox | 750 / 2 | AV1, VA-API init | 892 `publish=direct`, 0 errors |
| live Bilibili | Chrome | 752 / 0 | `VaapiVideoDecoder`, platform=true | hardware session |
| live YouTube, AV1 off (control) | Firefox | 751 / 1 | VP9, VA-API init | 981 `publish=direct`, 0 errors |

`PASS AV1 browser matrix mode=all`, `failures=0`. Firefox MOZ_LOG child
files are parsed with `errors="replace"` after waiting for the init line.
Firefox local windows show 5-13 dropped frames; Chrome and live-site windows
report 0-2. These are 30-second playback windows, not a power or long-window
soak.

The dropped-frame column is browser-side `getVideoPlaybackQuality().droppedVideoFrames`,
which also counts frames not presented in time, not driver `publish=drop`
events. Every Firefox row reports `publish=direct` with `0 errors` and no
`publish=drop` or `publish=misplaced`. The local corpus clips are shorter than
the window (2.5-7.5 s against 30 s) and `test/playback.html` loops the video,
so loop boundaries fall inside the sampled window; the local drop counts track
the number of boundaries. Live-site windows have no loop and report 0-2. This
is an attribution note, not a claim that the loop causes the drops.

No gate in this repository requires a zero-drop browser window.
`test/iris-browser-acceptance.sh` on `main` decides `hardware_decode` from the
selected decoder, the platform flag, decoded frames, a node holder and an
unchanged boot ID; it records `window_dropped_frames` as evidence only. The
Firefox and Chrome PASS conditions in `test/iris-av1-browser-matrix.sh` do the
same. AGENTS.md requires a media surface that reports the selected decoder
with frame and drop evidence. Both READMEs now state that criterion; the
earlier English sentence about "zero frame drops over a 30-second window" was
stale documentation, not an enforced gate.

The 10-bit and 4K rows are capability probes at 720p/2160p 8-bit and one
720p 10-bit corpus clip; they do not extend the capability table's
`va_profiles` beyond `AV1Profile0`, and the 10-bit row has no Chrome pixel
comparison against software output. Film grain, 12-bit, large-scale tile and
Chromium 152's software AV1 selection remain outside this matrix.

## AV1 film grain and 1080p 10-bit probes (2026-11-11)

Target `sheng`, boot `7a26df99-f7ac-4148-9c80-135ba3adc661`, kernel
`7.2.6-sm8550-g42f3b40-iris1` with the 0001-0004 module, decoder
`/dev/video17` (`iris_driver`, `Iris Decoder`). No kernel module was changed
or reloaded during these runs. Battery 98% and full, peak zone 30700 millic,
no reset and no thermal breach.

### Film grain synthesis is a two-surface VA-API contract

A 320x180 24-frame AV1 clip was generated with
`libaom-av1 -aom-params film-grain-test=1`; libdav1d reported
`grain.present=1 grain.update=1 seed=45231 num_y_points=14 ar_coeff_lag=2`.
On the installed package `e97a861f…`, FFmpeg's VA-API path failed at
`endPicture` with `film grain parameters are not reconstructed`, then every
later frame failed with `AV1 reference slot is empty`.

A scratch build replaced the refusal with a full `film_grain_params()` OBU
writer. `V4L2_VA_DUMP` captured the rebuilt stream and libdav1d decoded it
without errors, reporting the same grain parameters as the source, so the
syntax was correct. The firmware also exposes
`HFI_PROP_AV1_FILM_GRAIN_PRESENT` (a 0/1 capability flag, not a parameter
channel), so it applies grain from the bitstream itself. The hardware run
still ended in `getImage: surface has no decoded pixels`: FFmpeg's
`vaapi_av1.c` points `vaBeginPicture` at a separate ungrained `tmp_frame`
when `apply_grain` is set, and names the client's display surface only as
`current_display_picture` in the VA picture parameters. This driver
publishes one target per CAPTURE completion, the one named by
`vaBeginPicture`, and never maps a completion to `current_display_picture`,
so the display surface receives no pixels. Supporting synthesis therefore
needs a second-target publish path in the VA/session layer, not just OBU
syntax. The refusal stays fail-closed with the reason
`AV1 film grain synthesis needs a separate display surface`.
`test/av1-obu-test.cc` asserts both the refusal and that `apply_grain=0`
still rebuilds a frame.

With `-export_side_data film_grain` the client sets `apply_grain=0` and
handles grain itself; that run downloaded all 24 frames
(2073600 bytes of yuv420p) with `submitted=25 completed=25 errors=0
drops=0 misplaced=0`. So grain-carrying AV1 streams decode in hardware when
the client exports the parameters, and the `rejected` entry in
`data/iris-codec-capabilities.json` concerns synthesis by the driver.

### 1080p 10-bit AV1 exact framemd5

A 1920x1080 60-frame `yuv420p10le` clip was encoded with `libsvtav1`
(`-preset 8 -crf 30`); `ffprobe` reports `codec_name=av1`, `profile=Main`,
`pix_fmt=yuv420p10le`, 1920x1080. The software oracle is
`libdav1d`/FFmpeg framemd5 at `yuv420p10le`.

The installed package (`e97a861f…`, `source_commit=a34d348…`,
`source_dirty=0`) was exercised with `LIBVA_DRIVERS_PATH=/usr/lib64/dri` and
`V4L2_VA_PUBLISH=import`, decoding to P010 VA surfaces and downloading to
`yuv420p10le` framemd5:

| Result | Value |
| --- | --- |
| framemd5 | `EXACT` (60/60 frame hashes equal) |
| session | `submitted=60 completed=60 errors=0 drops=0 timeouts=0 misplaced=0` |
| publish | `import=60 copy=0 direct=0 drop=0 error=0 invisible=13` |

This is a capability probe, not a new VA profile: `va_profiles` stays
`AV1Profile0`, and no 10-bit browser pixel comparison was made here.

### Encode rate control: reproducible A/B protocol (recorded 2026-11-11)

Superseded by "Encode rate control: the target was programmed as the maximum"
below: the observation in this section was a translator defect and is fixed.

The table above is unchanged: this driver follows the CBR request and
libva-v4l2 stays near 700-900 kbit/s. The one unisolated observation is VBR
4 Mbit/s producing 9780 kbit/s (about 2.4x the target) while CBR at the same
target produces 4034 kbit/s; `src/iris/encode.cc:163-170` writes
`BITRATE_PEAK` only for CBR. These are recorded as observations, not a
diagnosis, and no encode code or capability entry was changed.

A host-side / target-side A/B that isolates the layer without changing
`src/`:

```sh
# Fixed 600-frame 1080p30 NV12 source, one clip, one boot ID.
# LIBVA_DRIVERS_PATH points at the build tree under test; strace records the
# exact VIDIOC_S_CTRL values so a control-write difference is visible before
# any bitrate measurement is interpreted.
run() {
    tag=$1; shift
    strace -f -e trace=ioctl -yy -o "$tag.ioctl" \
        ffmpeg -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 \
        -f rawvideo -pix_fmt yuv420p -s 1920x1080 -r 30 -i "$SRC" \
        -vf format=nv12,hwupload -c:v h264_vaapi -g 60 "$@" -f h264 -y "$tag.h264"
}
run cqp      -rc_mode CQP -q:v 24
run cbr4     -rc_mode CBR -b:v 4M -maxrate 4M
run vbr4     -b:v 4M
run vbr4peak -b:v 4M -maxrate 4M -bufsize 8M
run vbr1     -b:v 1M
run vbr12    -b:v 12M
run vbr24    -b:v 24M
```

Pass conditions, each pointing at one layer: all runs write
`BITRATE=4000000` (or the matching target) and `FRAME_RC_ENABLE=1` with the
right `BITRATE_MODE`; CBR stays monotone with the target; `vbr4peak` versus
`vbr4` shows whether the missing VBR peak is the lever; a flat VBR curve at
~2x the target across 1M/4M/12M/24M would support (not prove) the firmware
RC-property hypothesis. `strace` is a probe only, not part of the shipped
test suite. Record source commit, driver hash, resolved encoder node, boot
ID, and whether the run used an installed package or a build-tree override.

## Installed-package verification of the completion branch (2026-11-11)

Installed from source commit `a53963d6ca162a1a01ee18fa49ea44443723019c`
(the branch head at install time; later commits are docs-only and are not
part of the installed binary), clean tree of 157 tracked files,
`tracked_source_sha256=3afc83c329e7a89c66058eabe7a9f8aced405931b16fb8a63918eea8c10e4ab4`,
`source_dirty=0`. A fresh target build with the pinned `/usr/bin` toolchain
produced `/usr/lib64/dri/v4l2_drv_video.so` SHA-256
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`, with no
undefined symbols and 25/25 target meson tests. This is an installed package
(`LIBVA_DRIVERS_PATH=/usr/lib64/dri`), not a build-tree override and not a
`DESTDIR` staging run. Rollback: the previous installed drivers
`8f87efde…` and `e97a861f…` are preserved at
`<scratch>/rollback-final-20261111/v4l2_drv_video.so`
(`e97a861f…`), `/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`
(`8f87efde…`), and the `final-install/src/build` tree. No kernel module was
changed or reloaded; boot ID stayed
`7a26df99-f7ac-4148-9c80-135ba3adc661`, decoder `/dev/video17`
(`iris_driver`, `Iris Decoder`), battery 98% and full.

Hardware gates and the browser matrix ran under separate guards to avoid
heat accumulation from consecutive 4K/browser runs; both guards completed
with peak temperatures below the 85000 millic limit.

Host gates: `sh test/run-static-checks.sh` PASS,
`meson test -C build-local --print-errorlogs` 25/25, `git diff --check` clean.
GitHub Actions `build-and-check` passed (`.github/workflows/ci.yml` installs
`ffmpeg` and asserts `libdav1d` for the two AV1 oracle checks). Target runs
used `IRIS_EXPERIMENT_DIR` under `<scratch>/gates-final2-20261111` with
`V4L2_VA_PUBLISH=import`.

| Gate | Result |
| --- | --- |
| four-codec Import matrix | 48/48 exact, all four codecs `content=frame-md5`, `submitted=48 capture=48 drain=1 timeouts=0 drops=0`, AV1 `noshow=23`; guard `status=complete` |
| 120-frame AV1 clip (plain) | `frames=120 submitted=120 completed=120 import=120 copy=0 invisible=42`, framemd5 EXACT; guard `status=complete` |
| in-context DRC clip | `frames=80 submitted=84 completed=84 import=42 copy=42 invisible=63`, framemd5 EXACT; guard `status=complete` |
| 1080p 10-bit AV1 | 60/60 framemd5 EXACT against the libdav1d software oracle, `submitted=60 completed=60 errors=0 drops=0 timeouts=0 misplaced=0`, `publish=import` on all 60 frames, `invisible=13`; guard `status=complete` |
| complete 17-case AV1 browser matrix | `PASS AV1 browser matrix mode=all`, `failures=0`, 17/17 media surfaces, guard `status=complete`; all 8 Chrome cases reporting `decoder=VaapiVideoDecoder platform=True dropped=0` (`decoded=695..752`), all 9 Firefox cases reporting `hw_log=True errors=0` (`decoded=724..751`, `dropped=0..12`); local 720p/1080p/4K/10-bit/altref/tiles plus live YouTube/Bilibili and noav1 control; peak thermal zone 61100 millic, no reset, boot ID unchanged |

## A11: independent-oracle AV1 syntax corpus (2026-11-11)

Five 640x360 AV1 clips were generated on `sheng` with `libaom-av1` and
decoded by an independent `libdav1d` software oracle (the VA driver did not
generate the oracle). Each clip was then decoded through the installed
package (`LIBVA_DRIVERS_PATH=/usr/lib64/dri`, `V4L2_VA_PUBLISH=import`) and
compared by ordered framemd5 hashes. The run was wrapped in
`scripts/iris-experiment-guard.sh` with `IRIS_EXPERIMENT_DIR` under
`<scratch>/av1-syntax-corpus/a11-guard`:
`status=complete`, boot `7a26df99-f7ac-4148-9c80-135ba3adc661` unchanged,
no reset, thermal zones 37600 millic, battery 85% Discharging, driver
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`.

| Case | Encoder syntax | Oracle / hardware frames | Session | Publish | framemd5 |
| --- | --- | --- | --- | --- | --- |
| baseline | `libaom-av1 -cpu-used 8 -crf 32` | 60 / 60 | `submitted=62 completed=62 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | `import=62 invisible=51` | EXACT |
| tiles | `-tiles 2x2` | 60 / 60 | `submitted=62 completed=62 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | `import=62 invisible=51` | EXACT |
| lookahead | `-lag-in-frames 12` | 60 / 60 | `submitted=60 completed=60 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | `import=60 invisible=51` | EXACT |
| stillpicture | `-still-picture 1` (1 frame) | 1 / 1 | `submitted=1 completed=1 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | `import=1` | EXACT |
| lossless | `-aom-params lossless=1` | 60 / 60 | `submitted=62 completed=62 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | `import=62 invisible=52` | EXACT |

`A11_RESULT failures=0`. Two upstream case designs were excluded rather
than reported as passing: frame super-resolution could not be expressed by
the target's `libaom-av1` (all `superres-*` `aom-params` variants produced no
packets), and `-enable-global-motion 1` produced a bitstream whose decoded
framemd5 was identical to the baseline, so it gives no distinguishable
syntax coverage. No new VA profile or capability entry is claimed from this
corpus; it is evidence that the default translator handles these five syntax
shapes on the installed package.

## Post-merge baseline (2026-11-12)

PR #5 merged into `main` as `4125b1190c66ff7cf46e54c2b6d4076a01d7d1cd` on
2026-11-11T13:19:36Z. PR #4 (`exp/av1-noshow-dpb`, head `a7e6d26`) was also
merged two seconds later; its head is an ancestor of the PR #5 merge commit,
so `main` carries one copy of the internal-DPB work and no duplicate commits.
Both branches remain on the fork.

`origin/main`, local `main` and the PR head `a0b7138` all have tree
`2bae05f1c3dce17fa3c280862300e0019f7b08a7`; the worktree is clean. GitHub
Actions `build-and-check` run `43028113058` completed `success` on the merge
commit (13:19:43Z-13:20:59Z). Host gates re-run on 2026-11-12:
`sh test/run-static-checks.sh` rc=0,
`meson test -C build-local --print-errorlogs` 25/25, `git diff --check` clean.
No target run, driver reinstall or kernel module swap accompanied the merge;
the installed driver on `sheng` remains the one recorded at
"Installed-package verification of the completion branch" above (source
commit `a53963d6`, driver SHA-256 `323c07cb…`, boot
`7a26df99-f7ac-4148-9c80-135ba3adc661`, 0001-0004 module).

## PR #6 host hygiene gates (2026-11-12)

PR #6 (`chore/post-merge-hygiene`, draft) adds the §8 trace-vocabulary checker,
closes the Firefox browser-matrix hollow-green paths, makes the AV1 software
oracles fail when their prerequisites are missing, and records the merged
baseline and WP0-WP7 sequence. It is host-only: no target run, no driver
reinstall, no kernel module swap, no capability or publish-policy change.

Host gates on `d74f0f2`: `sh test/run-static-checks.sh` rc=0,
`meson test -C build-local --print-errorlogs` 26/26 (the 26th is
`iris-trace-vocab-check`), `git diff --check` clean. GitHub Actions
`build-and-check` run `43061871540` completed `success` on `d74f0f2`. The
earlier run `43059187865` on `096792d` failed in `Install build dependencies`
before any project step ran; it is a runner-side bootstrap failure, not a
product failure.

The Firefox PASS condition now requires this browser process to publish at
least the sampled window's decoded frames through the installed driver, with
no `publish=error`, `publish=misplaced`, `publish=drop` or `session failed`.
The nine Firefox rows from the 2026-11-11 17-case matrix were re-checked
against that condition on the target's existing artifacts: all nine pass
(`decoded=724..751`, `published=875..974`, `errors=0`). This is a contract
check against recorded traces, not a new browser playback run.

## PR #6 tablet read-only survey (2026-11-12)

WP4 feasibility and WP1 prerequisites were checked on `sheng` over SSH with
read-only commands. No module was built, loaded or replaced; no reboot; no
guarded decode run. Kernel `7.2.6-sm8550-g42f3b40-iris1`, boot ID
`7a26df99-f7ac-4148-9c80-135ba3adc661`, battery 27% Discharging, thermal
zones 34000..37200 millic. The installed VA driver is still
`323c07cbd42f5a15…`; the installed `qcom-iris.ko.zst` hash is
`9b16a40179a08e414d0f4bb38ea54f5bb12ada2b9ce176f0dc6b3b602e7bc83d`.
Decoder/encoder resolved by `iris_driver` were `/dev/video17` and
`/dev/video18`; `display_delay`, `display_delay_enable` and
`min_number_of_capture_buffers` are present on the active module.

VPP backends: `/dev/fastrpc-cdsp` exists and `libcdsprpc.so.1` is installed,
but no Hexagon compiler (`hexagon-clang`, `q6-clang`) is present. Vulkan
1.4.341 and `libvulkan.so` are installed, `vulkan.pc` reports 1.4.341 and
the freedreno ICD file is present, but `vulkaninfo`, `glslangValidator` and
`glslc` are absent. Neither backend is buildable/verifiable as-is; WP4's
current verdict is "no proven viable backend on this target", with the CDSP
runtime probe still outstanding. This is a read-only inventory, not a VPP
capability claim.

WP1 module build prerequisites: `/boot/config-7.2.6-sm8550-g42f3b40` and
`/proc/config.gz` are available (there is no `-iris1` config file), LLVM,
`make`, `zstd`, `git` and `awk` are present, and `sudo -n` works. BTI/PAC are
enabled in the running config (`CONFIG_ARM64_BTI=y`,
`CONFIG_ARM64_PTR_AUTH=y`, `CONFIG_ARM64_PTR_AUTH_KERNEL=y`), so the module
must be built with the same toolchain and config as the running kernel. No
complete kernel source tree or `Module.symvers` was found under `~/Lab`, and
no rollback copy of `qcom-iris.ko.zst` was found. The module-install step is
therefore blocked until a source tree, symbol versions and a rollback copy
are staged on the target.

This was the state at survey time. The later WP1 build section records that
the source tree, symbol table and verified rollback copy were staged; the
survey itself remains a read-only record and is not retroactively edited.

## WP2: cost of one extra NOSHOW completion per AV1 picture (2026-11-12)

Measurement only, from the existing guarded traces on `sheng`. No driver
change, no new target run, no module swap, no reboot. The installed package is
the one recorded above (`323c07cb…`, 0001-0004 module, boot
`7a26df99-f7ac-4148-9c80-135ba3adc661`). The traces carry ` t=<ms>` since the
first record of each process, so the cost is read from their own timestamps.

| Trace | Pictures published | `publish=invisible` | Max `import backlog` | Import queue events | Median invisible -> next publish |
| --- | --- | --- | --- | --- | --- |
| 1080p 10-bit probe (2026-11-11 final gates) | 60 | 13 | 3 | 73 | 2 ms (max 11 ms) |
| 320x180 internal-DPB VA probe | 46 | 30 | 4 | 79 | 1 ms (max 4 ms) |
| 320x180 `make_av1()` baseline (no hidden install) | 47 | 0 | 3 | 49 | n/a |

The extra completion costs one Import queue event and one borrowed client
buffer per hidden install picture. Its trace-level delay is 1-2 ms median and
11 ms worst case in these two processes; the Import backlog stayed between 3
and 4, with no sign of pressure. The 120-frame clip and DRC clip from the
final gates report 42 and 63 invisible completions respectively, but their
`command.stderr` traces were not retained, so they contribute counts only,
not timing.

**WP2 verdict: not worth doing.** On this evidence, changing the shipped
userspace to a 0005 dual path would trade a measured 1-2 ms trace-level cost
for a kernel-module install, a new capability probe, a second completion
ledger in `session.cc`, a trace-vocabulary change and a full re-run of every
installed-package and browser gate. The current 0001-0004 module already
passes those gates with exact framemd5. WP3 is therefore not implemented;
see `docs/POST-MERGE-BACKLOG.md` §16. This is a cost judgment from existing
traces, not a 0005 hardware measurement.

## WP4/WP5/WP6 decisions from the 2026-11-12 survey (no code change)

WP4: the CDSP device node and runtime library exist, but no Hexagon compiler
or SDK package is installed; the read-only `dnf list available` query could
not complete because one configured repository failed GPG metadata
verification, so package availability is unconfirmed. The Vulkan loader and
freedreno ICD exist, but `vulkan-tools` and `glslang` are not installed.
Neither backend is buildable/verifiable as-is. The decision is **do not
implement VPP** in this repository now, and do not advertise a VPP capability.
The inventory does not prove the backends impossible: it records that no
viable backend was demonstrated.

WP5 depends on WP4. With no VPP, the answer to "who consumes RGB import" is
that no recorded VA client in this project's gates needs BGRA/RGBA; the
current CAPTURE-layout NV12/P010 contract is what the hardware decode and
encode gates exercise. The decision is **no RGB import code**, keep the
capability table's RGB rejection entries, and record `radxa-pkg/qcom-va-driver`
as the reference implementation if a real consumer appears.

WP6: `docs/build-and-deploy.md` already records why there is no Debian/Ubuntu
package: `libgbm-dev` pins an exact `libgbm1` and can downgrade a
vendor-patched graphics stack. The 2026-11-12 survey confirms the target has
`mesa-libgbm` and `mesa-libgbm-devel` both at 26.2.5. The decision is **do
not add `debian/`**: no Debian/Ubuntu distribution is a qualified target, and
adding a package that cannot reproduce the Fedora/Arch provenance and the
installed-package gates would create an unverified distribution surface.

## WP1: 0006 module build staged on sheng (2026-11-12)

Source commit `42f3b40c702abbc37c3a04c95af008cd283aeaf3`
(`ianchb/sm8550-mainline`, branch `sheng-7.2.6`). Kernel release
`7.2.6-sm8550-g42f3b40-iris1`, boot ID
`7a26df99-f7ac-4148-9c80-135ba3adc661`. Installed VA driver
`323c07cbd42f5a152cfca427`; installed `qcom-iris.ko.zst`
`9b16a40179a08e414d0f4bb38ea54f5bb12ada2b9ce176f0dc6b3b602e7bc83d`.
Decoder and encoder resolved by `iris_driver` were `qcom-iris-decoder` and
`qcom-iris-encoder`. The installed package was unchanged throughout; this run
used no build-tree driver override and no module override.

The build used `packaging/kernel/build-module.sh` from this branch on the
target's case-sensitive f2fs filesystem. A host-side clone was rejected: its
filesystem is case-insensitive and collided Linux's `xt_TCPMSS.c` /
`xt_tcpmss.c`. `.config` came from `/proc/config.gz`, because the two
`/boot/config-*` files have different hashes and there is no config named
after the `-iris1` release. `localversion` was derived from `uname -r`, and a
333-entry video-subsystem `Module.symvers` was regenerated from
`/proc/kallsyms`; sorting shows its symbol set matches the earlier lab copy
`<scratch>/kernel-iris-syncframe/Module.symvers`, whose `stock.sha256` check
passes. Patches 0001-0004 and 0006 applied in order by `git apply`; the 0005
marker `iris_hfi_gen2_requeue_output_buffer` is absent from the applied tree.
The running config has `CONFIG_ARM64_BTI=y`, `CONFIG_ARM64_PTR_AUTH=y`,
`CONFIG_ARM64_PTR_AUTH_KERNEL=y`, `CONFIG_CC_IS_CLANG=y`, `CONFIG_LD_IS_LLD=y`
and `CONFIG_LTO_CLANG_THIN=y`, so the module was built with `LLVM=1` against
the running config rather than a fresh defconfig.

Two independent builds on `sheng` agreed on the contract:

| Build | `qcom-iris.ko` sha256 | `qcom-iris.ko.zst` sha256 |
| --- | --- | --- |
| initial staged build | `1e15727da6c9ae91efce9b7319cfe477e03ee8d642db70395f56ab505535aa2a` | `d3987ee62109da8a98843709661ddb14500d8338be090a946077d2dcabb0ce72` |
| `build-module.sh` end-to-end re-run | `875b48f3cd15928d182d5447cc9ece30bb3fc305ef0b2bfb952089c120a31ee9` | `b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715` |

The two ELF hashes differ in LTO-local symbol names, not in the patch set;
both have the same vermagic, `7.2.6-sm8550-g42f3b40-iris1 SMP preempt
mod_unload aarch64`, and the same 136 undefined symbols. Both undefined
symbol sets are byte-identical to the running module's, and all 136 names are
present in the running kernel's `__ksymtab`; `modinfo -F license` reports
GPL on both. `readelf -S` shows no `__versions` or `.BTF` section in either
module, consistent with `CONFIG_MODVERSIONS` and BTF being off. The
`KBUILD_MODPOST_WARN=1` messages about core-kernel symbols are expected from
a partial `Module.symvers`; the `__ksymtab` comparison is the actual load
check.

A rollback copy under `<scratch>/wp1-verify/out/` was compared
byte-for-byte with the installed `qcom-iris.ko.zst` before the second build.
Battery was 96% Discharging, thermal zones 24400..37000 millic. No module was
installed, `depmod` was not run, the device was not rebooted, and the boot ID,
driver hash, module hash and node names above were re-read unchanged after
the build. No decode, matrix, browser, EOS or DRC gate ran, so there are no
frame, drop or `misplaced` counts for this section.

A search of 838 retained guard logs/traces found no `EPIPE`, spurious LAST or
premature LAST record. The slow-CAPTURE VP9 defect that 0006 addresses is
therefore still unreproduced on SM8550. Until it is reproduced on the
0001-0004 baseline, a later clean run cannot be called "fixed"; the correct
verdict would be "no regression". Installation and the guarded hardware gates
were later run as a separate, explicitly approved target action; see the next
section. This section is a build and module-identity result, not codec
evidence and not a capability claim.

## WP1: 0006 loaded on sheng — guarded gates, "no regression" (2026-11-12)

One experimental kernel patch was installed at a time: 0001-0004 + 0006, with
0005 absent. The VA driver, capability table, VA profiles and publish policies
are unchanged. Reboot was explicitly approved for this WP1 run; the guard was
started only after the reboot, so its "boot ID unchanged" rule measures the
experiment rather than the installation.

**Identity and runtime** (installed package, no build-tree driver override;
`LIBVA_DRIVER_NAME=v4l2`, `LIBVA_DRIVERS_PATH=/usr/lib64/dri`):

| Field | Value |
| --- | --- |
| Kernel source | `42f3b40c702abbc37c3a04c95af008cd283aeaf3` (`ianchb/sm8550-mainline`, `sheng-7.2.6`) |
| Kernel release | `7.2.6-sm8550-g42f3b40-iris1` |
| Installed VA driver | `323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d` (unchanged) |
| Module before | `9b16a40179a08e414d0f4bb38ea54f5bb12ada2b9ce176f0dc6b3b602e7bc83d` (0001-0004) |
| Module after | `b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715` (0001-0004 + 0006) |
| Loaded vermagic | `7.2.6-sm8550-g42f3b40-iris1 SMP preempt mod_unload aarch64` |
| Boot ID before reboot | `7a26df99-f7ac-4148-9c80-135ba3adc661` |
| Boot ID after reboot / during guard | `17928711-7fd8-4f87-a024-9bf6b5bc44b6` |
| Decoder | `iris_driver` QUERYCAP name `qcom-iris-decoder`, card `Iris Decoder`, node `/dev/video5` after reboot (`/dev/video17` before). Formats `H264 HEVC VP90 AV01`. |
| Encoder | `iris_driver` QUERYCAP name `qcom-iris-encoder` |
| Rollback | `<scratch>/wp1-verify/out/rollback-qcom-iris.ko.zst`, `sha256sum -c` verified before install; restore script `<scratch>/wp1-rollback.sh` was not run |

Before installing, `<scratch>/wp1-install.sh` re-checked the rollback
copy, copied the built module, ran `depmod -a` and rebooted. The reboot changes
the boot ID by design; both IDs are recorded above.

**Baseline reproduction attempt on 0001-0004** (boot
`7a26df99-f7ac-4148-9c80-135ba3adc661`): six runs of the retained 1280x720
480-frame alt-ref VP9 clip, all `rc=0 frames=480 last=1 epipe=0 errors=0
drops=0` with `session finish submitted=515 completed=515 errors=0 drops=0
timeouts=0 drains=1 misplaced=0`. All six framemd5 files are byte-identical
(md5 `653d13e6913e9dec741f77116509b376`) and are retained under
`<scratch>/wp1-baseline-0001-0004/`. A raw V4L2 slow-CAPTURE probe
(640-frame H.264, two CAPTURE slots, `delay_us=2000` and `10000`, OUTPUT
buffers tagged with TIMECODE metadata) reported `submitted=640 completed=632
last_before_end=0 epipe=0 errors=0 tc_checked=632 tc_mismatch=0
queued_cap=634` on both delays; its console output is recorded in the session
log, while the probe binary and clip remain on the target. No separate log file
was kept for that pre-install probe. The earlier search of 838 retained guard
logs/traces also found no `EPIPE`, spurious LAST or premature LAST record. The
slow-CAPTURE defect that 0006 addresses is therefore **not reproduced** on
SM8550 with the qualified module, so a clean run with 0006 can only be called
"no regression", never "fixed".

**Final guarded run**: `<scratch>/wp1-verify/guard-20261111T193559Z`,
`IRIS_EXPERIMENT_TIMEOUT_SECONDS=1800`,
`IRIS_EXPERIMENT_MAX_TEMP_MILLIC=85000`,
`status=complete`, `finished_at=2026-11-11T19:37:00Z`, boot ID unchanged,
maximum recorded temperature 40400 millic, no `failure` file, no thermal
breach and no firmware reset. Three earlier attempts were `interrupted` and are
retained as audit records: a relative `test/iris-eos-check.sh` path, a stale
node/gate deployment after the reboot changed the decoder node, and an EOS
checker invocation without `--hidden-pictures`. None of them was a decode or
firmware failure. The final run used a fresh guard directory.

| Gate | Result |
| --- | --- |
| Four-codec matrix | H.264, VP9, HEVC, AV1 each `frames=48 submitted=48 capture=48 drain=1 timeouts=0 drops=0` with exact framemd5; AV1 `noshow=23`. `SKIP mpeg2/vp8 device-formats-unavailable`. |
| Long VP9 clip, six runs | `rc=0 frames=480 last=1 epipe=0 errors=0 drops=0`, `submitted=515 completed=515 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` each run. All six framemd5 files have the same md5 as the 0001-0004 baseline, and `run1.md5` is byte-identical between the two modules. |
| EOS contract | `PASS frames=480 contexts=1 submitted=515 capture=515 drain=1 timeouts=0 drops=0 noshow=0` with `--hidden-pictures`: 480 visible pictures and 35 hidden alt-reference completions. |
| Real AV1 clip | `PASS frames=120 submitted=120 completed=120 import=120 copy=0 invisible=42` |
| AV1 DRC clip | `PASS frames=80 submitted=84 completed=84 import=42 copy=42 invisible=63` |
| Slow CAPTURE, `delay_us=2000` | `submitted=640 completed=632 last_before_end=0 epipe=0 errors=0 tc_checked=632 tc_mismatch=0 queued_cap=634 elapsed=1343ms` |
| Slow CAPTURE, `delay_us=10000` | Same counters, `elapsed=6454ms` |

The long VP9 reproduction probe requests `V4L2_VA_PUBLISH=import`, which the
capability table refuses for VP9; its traces show `publish=copy-gpu` for the
515 payload completions and `publish=invisible` for 35 hidden ones. It is a
reproduction attempt, not a VP9 Import gate. No browser matrix, 10-bit probe
or capability qualification ran in WP1.

**Decision**: keep the 0001-0004 + 0006 module installed on `sheng`. The
guarded gates show no regression against the 0001-0004 baseline; the defect
0006 addresses was never reproduced here, so there is no "fixed" claim. 0006
stays experimental, the qualified module remains 0001-0004, and no codec,
profile or publish policy is advertised from this run. The verified rollback
copy and `wp1-rollback.sh` restore the qualified module with `depmod -a` and a
reboot if a later run fails or another experiment needs the baseline. 0005 was
not built or loaded.

## Guard pack state: recording replaces the charger gate (2026-11-12)

The battery prerequisite in `docs/POST-MERGE-BACKLOG.md` §2 (L1) previously
required a charger and a stable `Charging|Full` pack before any long-window
run. That turned a measured condition into a start gate: the 2026-11-10 soak
was deferred at 26 % `Discharging`, but the short capability and browser
windows were still useful with a recorded boundary. The guard now writes
`battery.log` with `capacity_percent`, `status`, `voltage_uv`, `current_ua` and
`usb_online` at the start, on each thermal sample and at the end. It does not
refuse a low, discharging or charging pack. Boot ID, thermal inventory, the
85 C ceiling, the timeout and the interrupted-guard rule are unchanged;
`IRIS_EXPERIMENT_BATTERY_ROOT` overrides the power-supply directory for the
host self-test.

Host verification: `shellcheck -s sh` clean on the guard and its self-test;
`test/iris-experiment-guard-self-test.sh` covers 19 % `Discharging`, 100 %
`Charging` and a missing gauge, each reaching `status=complete` with the pack
state written; `sh test/run-static-checks.sh` PASS and
`meson test -C build-local --print-errorlogs` 26/26.

Target smoke run on `sheng`, boot ID
`17928711-7fd8-4f87-a024-9bf6b5bc44b6`, installed VA driver
`323c07cbd42f5a152cfca427`, 0001-0004 + 0006 module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`:
`guard-battery-live3` reached `status=complete` with four `battery.log` rows,
for example `85,Discharging,4174000,-830000,0`. No decode, matrix or browser
gate ran in this smoke test; it is a guard-instrumentation result, not codec
evidence.

`test/iris-power-sampler.sh` still refuses `Charging|Full`: a plugged-in fuel
gauge cannot measure battery-side draw, so that refusal belongs to the power
measurement itself rather than to experiment admission. E1-E4 now need the
guarded target windows with their start/end capacity reported; a shorter window
is a shortened result, not a cancelled experiment.

## AV1 browser matrix, local clips only (2026-11-12, `sheng`)

`test/iris-av1-browser-matrix.sh local-only` over the six generated AV1 clips
(`av1-720p`, `av1-1080p`, `av1-4k`, `av1-10bit`, `av1-altref`, `av1-tiles`),
Firefox and Chrome, 30 s of playback each. Identity: boot ID
`17928711-7fd8-4f87-a024-9bf6b5bc44b6`, installed package
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`, decoder
resolved by the `iris_driver` QUERYCAP name to `/dev/video5`, no `LIBVA_*`,
publish or node override, and the 0001-0004 + 0006 module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715` loaded.
`failures=0`, `PASS AV1 browser matrix mode=local-only cases=12`.

| Case | Result |
| --- | --- |
| Firefox 720p | `decoded=724 dropped=6 hw_log=True direct=927 import=0 errors=0` |
| Firefox 1080p | `decoded=726 dropped=13 hw_log=True direct=936 import=0 errors=0` |
| Firefox 4k | `decoded=732 dropped=17 hw_log=True direct=924 import=0 errors=0` |
| Firefox 10bit | `decoded=726 dropped=12 hw_log=True direct=937 import=0 errors=0` |
| Firefox altref | `decoded=724 dropped=10 hw_log=True direct=1007 import=0 errors=0` |
| Firefox tiles | `decoded=724 dropped=7 hw_log=True direct=924 import=0 errors=0` |
| Chrome, all six | `decoder=VaapiVideoDecoder platform=True dropped=0`, decoded 699-718 |

Every Firefox case published through the direct path only (`import=0`), which
is what `publish_import` AV1 being qualified does not claim: FFmpeg does not
pre-export surfaces, so it never takes the Import path. Chrome dropped no
frames in any case; Firefox dropped 6-17 frames per 30 s window, which is the
browser's own display pacing on this compositor, not a driver drop count.

Two boundaries. The loaded module is the experimental 0001-0004 + 0006 build,
so this run is a no-regression check for that module, not a re-qualification
of the 0001-0004 baseline; the qualified 17-case browser result remains the
2026-11-11 run. And `local-only` means the five live-site cases (YouTube,
Bilibili, and the `media.av1.enabled=false` control) did not run in this
window: they need the relay on `127.0.0.1:8899`.

The first attempt at this matrix failed 12/12 with `no BiDi report` /
`no CDP report` because the deployed tree on `sheng` was missing
`test/iris-firefox-media.py` and `test/iris-chrome-media.py`; the browsers had
in fact decoded (the Firefox trace shows `begin_picture`/`direct` publishes),
but the report step could not run. After syncing both helpers the rerun above
passed. That failed attempt is a deployment gap, not a driver result.

## Battery-side power A/B: the software arm is stopped by the guard's ceiling (2026-11-12)

`test/iris-power-sampler.sh` with a fixed 1080p30 High-profile H.264 source
(`testsrc2`, 10 s, re-decoded back to back), screen off, unplugged, 20 s per
arm with 10 s baselines either side. Identity on every run: boot ID
`17928711-7fd8-4f87-a024-9bf6b5bc44b6`, installed package
`323c07cbd42f...`, 0001-0004 + 0006 module `b90bd823...`, no build-tree
override, start/end pack state in each `run.txt`.

The hardware arm finishes. The software arm does not: eight threads of
ffmpeg software decode take the CPU zones from 40 C to past 85 C in about
nine seconds, and `scripts/iris-experiment-guard.sh` terminated the run on
its 85 C ceiling each time.

| Attempt | Start | Window | Outcome |
| --- | --- | --- | --- |
| 1 | 74 C hottest zone | 20 s x 4 | `temperature exceeded limit: 94700 > 85000`, `cpu7-middle-thermal`, 61 s in |
| 2 | 40.2 C | 20 s x 4 | `90300 > 85000`, `cpu5-bottom-thermal`, in the first software window |
| 3 | 39.8 C | 8 s x 4 | `87600 > 85000`, first software window |

The hardware arm's device-level increment, measured in the same guarded runs
before the software arm was stopped (steady window minus the mean of its two
baselines, whole-device gauge so this is not decoder-package power):

| Run | VA window | Pre | Post | Increment |
| --- | --- | --- | --- | --- |
| 1 | 4.46 W | 2.86 W | 2.93 W | +1.57 W |
| 2 | 3.94 W | 3.06 W | 2.98 W | +0.92 W |
| 3 | 4.58 W | 3.01 W | 3.01 W | +1.57 W |
| 4 (unguarded trace run, 20 s) | 3.98 W | 2.91 W | 2.90 W | +1.08 W |

The software arm was rising through 9 W total (about +6 W over its 2.8-3.3 W
baseline) when the guard cut it, so its steady state is not measurable here.
No VA-versus-software ratio is claimed. The one complete round that exists
came from run 4, which ran under `sh -x` without the guard to find the
failure cause; its increments were `va=1077799uW sw=6163760uW`, and its
thermal state was not sampled, so it is an observation and not the A/B.

For the record, the kernel's own trip points on these zones are passive at
90 C and 95 C and critical at 110 C. The guard's 85 C is the project's
ceiling, stricter than the firmware's, and it was not raised for this
measurement: a run that needs a higher ceiling to finish is a different
experiment, not a longer one.

E2 therefore stayed open at the time with a narrower question: the software
half needs a workload that stays under 85 C at saturation, or an explicit
decision about the ceiling. That decision was taken on 2026-11-12 — the
guard's ceiling is a runaway backstop below the zones' 110 C critical trip,
not a comfort line — and the A/B ran; see "E2 closed" at the end of this file.

The attempts above are retained as they happened, under the 85000 default
that was in force then. The hardware-arm increments are whole-device
increments over their own baselines.

## Encode rate control: the target was programmed as the maximum (2026-11-12)

The recorded rate-control gap (VBR producing about twice its target) was
reproducible and is a mapping defect in the translators, not a firmware
property. `VAEncMiscParameterRateControl` defines `bits_per_second` as *the
maximum* the controller may generate and `target_percentage` as *the target
inside that maximum* (`/usr/include/va/va.h`). Both translators copied the
raw field into the V4L2 target and ignored the percentage.

FFmpeg's `vaapi_encode_init_rate_control()` makes that the common case: when
the mode supports a maximum and the client gave no `rc_max_rate`, it
programs `2 * bit_rate` with `target_percentage = 50`, so a plain `-b:v 4M`
asks VA for 8 Mbit/s at a 50 % target.

Seven arms on `sheng`, installed package `323c07cbd42f...`, boot
`17928711-...`, encoder node `/dev/video6` as the strace shows it, fixed
1080p30 `testsrc2` source, 600 frames per arm, `strace -e trace=ioctl`
recording every `VIDIOC_S_CTRL`:

| Arm | Request | `BITRATE` written | `BITRATE_PEAK` written | Output |
| --- | --- | --- | --- | --- |
| `cqp` | `-rc_mode CQP -q:v 24` | none (`FRAME_RC_ENABLE=0`) | none | 10534 kbit/s |
| `cbr4` | `-rc_mode CBR -b:v 4M -maxrate 4M` | 4000000 | 4000000 | 3986 kbit/s |
| `vbr4` | `-b:v 4M` | 8000000 | none | 8000 kbit/s |
| `vbr4peak` | `-b:v 4M -maxrate 4M -bufsize 8M` | 4000000 (mode 1) | 4000000 | 3981 kbit/s |
| `vbr1` | `-b:v 1M` | 2000000 | none | 2003 kbit/s |
| `vbr12` | `-b:v 12M` | 24000000 | none | 23975 kbit/s |
| `vbr24` | `-b:v 24M` | 48000000 | none | 30405 kbit/s |

Every VBR request without a maximum came out at exactly twice its target,
and the doubled value is visible in the control write, so the layer is
identified: the translator, not the firmware's rate controller. The 24M arm
is content-limited rather than rate-limited at 30.4 Mbit/s.

Fix: the target is now `bits_per_second * target_percentage / 100` (0 means
100 %, which keeps older clients unchanged), and a maximum above the target
becomes `BITRATE_PEAK` for VBR as well as CBR. `encode-params-test` covers
the split for both codecs. No capability entry changed.

### The fix took two stages (same boot, source `efab9b5` then `0dbc385`)

The percentage fix alone did not move the rate. With it built and loaded, the
`vbr4` arm still produced 7934 kbit/s, and the control trace shows why: ten
writes of 4000000 and ten of 8000000, alternating, one pair per picture.
FFmpeg renders the rate-control misc buffer only with the first picture; every
later picture arrives without it, the translator fell back to the sequence
field, and that field carries the client's *rate* — the maximum — rather than
the target derived from it. The controller was reprogrammed between 4 and
8 Mbit/s on every picture.

The second stage keeps the last configured rate when a picture carries no
rate control, so the target the client asked for survives the sequence field.
Two regression cases in `encode-params-test` cover it: a picture without rate
control keeps the target and the ceiling of the picture before it. Both fail
against the first stage's code.

Seven arms again after both stages, build-tree driver
`6efe4de9a6baa5862ea2397c48b2ad479e3b251a2d7de68316df098063b8bf4e`
(`LIBVA_DRIVERS_PATH` override, `ldd -r` clean, tablet unit tests passing),
installed package `323c07cbd42f...` untouched, boot `17928711-...`, decoder
`/dev/video5` (compressed on OUTPUT), encoder `/dev/video6` (compressed on
CAPTURE), same source, frame count and mount:

| Arm | Request | `BITRATE` written | `BITRATE_PEAK` written | Output | Before |
| --- | --- | --- | --- | --- | --- |
| `cqp` | `-rc_mode CQP -q:v 24` | none | none | 10534 kbit/s | 10534 |
| `cbr4` | `-rc_mode CBR -b:v 4M -maxrate 4M` | 4000000 | 4000000 | 3980 kbit/s | 3986 |
| `vbr4` | `-b:v 4M` | 4000000 | 8000000 | 3992 kbit/s | 8000 |
| `vbr4peak` | `-b:v 4M -maxrate 4M -bufsize 8M` | 4000000 | 4000000 | 3986 kbit/s | 3981 |
| `vbr1` | `-b:v 1M` | 1000000 | 2000000 | 1386 kbit/s | 2003 |
| `vbr12` | `-b:v 12M` | 12000000 | 24000000 | 11975 kbit/s | 23975 |
| `vbr24` | `-b:v 24M` | 24000000 | 48000000 | 23934 kbit/s | 30405 |

`vbr4` lands within 0.2 % of its target and writes each control once per
session, which is the change that mattered. `vbr1` overshoots by 39 %: at
1080p30 `testsrc2` the rate controller cannot reach 1 Mbit/s inside the QP
range the firmware uses, so that arm is content-limited from below and is not
a control defect. The 24M arm moved down with the rest, which shows the
sequence-field fallback was in effect there too.

The A/B runner's own header named `/dev/video5` as `encoder_node` because it
probed the OUTPUT queue; the node table above was resolved by queue role and
the runner now prints both. The `driver_sha` line in each run header is the
installed package and is not what the override run used; both digests are
recorded here for that reason.

## E3/E4: 4K AV1 Import under pinned clocks (2026-11-12)

The 4K lane of `test/iris-real-clip-gate.sh` declared `invisible=0` and failed
with `publish=invisible is 1, expected 0`. The decode itself was correct: the
VA framemd5 matched the software baseline on all 30 frames, `submitted=30
completed=30`, and the 14 NOSHOW halves were unchanged from the recorded run.
The expected count was stale. The 4K record in this file was taken on source
`079480d8` (2026-11-10), which predates `e5c8fae` (2026-11-11, "attribute both
NOSHOW halves of an install unit by token"); that commit is what reports the
install half of a two-completion access unit as one invisible record. The clip
carries exactly one such access unit. Three repeats on the installed driver
gave 1 invisible and 14 NOSHOW halves every time, so the lane now declares 1
and cites the reason.

Run identity for both measurements: installed package
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d` (no
`LIBVA_DRIVERS_PATH` override), boot `17928711-7fd8-4f87-a024-9bf6b5bc44b6`,
decoder `/dev/video5` and encoder `/dev/video6` as the driver's own
`va init node=` line reports them, kernel `7.2.6-sm8550-g42f3b40-iris1` with
the 0001-0004 + 0006 module, clip `av1-4k.mkv` (3840x2160, 30 frames, 30 fps),
battery 74 % discharging, no charger attached.

The guarded run (`guard-20261111T211550Z`, timeout 2400 s, temperature interval
5 s) is `status=complete`, unchanged boot ID, hottest sample 42400 millic,
battery 74 % -> 72 %, with every CPU policy pinned to `performance` at its
maximum (policy0 2016 MHz, policy3 2803 MHz, policy7 2956 MHz):

| Measurement | Frames | Wall | Per frame | Note |
| --- | --- | --- | --- | --- |
| clip gate, then 20 separate `ffmpeg` passes | 600 | 20830 ms | 34.7 ms | session-bound, see below |
| one process, `-stream_loop 19` | 600 | 1841 ms | 3.07 ms | 326 fps, `utime=0.132s stime=0.127s` |
| the same, default `schedutil` governor | 600 | 1878 ms | 3.13 ms | 0.31 s client CPU |

The 20-pass figure is not a throughput number. Twenty short sessions pay
twenty client startups — VA init, 4K surface allocation, decoder open and
drain — and one long process shows the same 600 frames in 1.84 s, so about a
second per session dominates a 30-frame clip. The per-frame cost to quote is
3.07 ms with the clocks pinned, and the client's own CPU time is 0.26 s for
600 frames, which is 14 % of the wall time: the rest is the hardware decoder.

Running the same single process with the default governor produced 1878 ms
against 1841 ms pinned. At this load the measurement is not sensitive to CPU
clock state, so the DVFS caution that applies to the earlier per-frame finding
does not change this number. It is still not a thermal, power or long-window
result, and the 4K lane remains an opt-in Import capability probe.

## Deploy sync: the digest is now measured on the target (2026-11-12)

A run earlier in this session reported `PASS provenance verified` for a commit
whose change was not in the tree on the tablet, and the sync was worked around
by hand. The mechanism was reproduced here instead of argued about, from a
clone carrying a marker comment in `src/codec/encode_h264.cc` and a scratch
remote root (`<scratch>/deploy-probe-target`, since removed):

| Probe | Action | Result |
| --- | --- | --- |
| delivery | deploy at the marker commit | marker present on the target, both copies `f9331aa4…`, artifact `1def30fd…` |
| repair | rewrite the target file at the same size | the next deploy restored `f9331aa4…`; `--checksum` does not trust size |
| bite | append a line to the target file, verify the host listing | `src/codec/encode_h264.cc: FAILED`, exit 1 |

So `rsync` delivered content in every probe, and the provenance gate still
carried a gap: `provenance.py verify` compares the artifact against a manifest
whose source identity this host injected, so a sync that delivered an older
tree would produce a manifest that verifies. `deploy.sh` now re-hashes every
tracked path on the target with `sha256sum -c` against the host's own listing
before the rebuild, and fails the deploy on a mismatch. The digest in the
manifest is then a measured fact about the target.

The probe also found that the exclude pattern `build*` matched two tracked
files by basename — `docs/build-and-deploy.md` and
`packaging/kernel/build-module.sh` — so they had never reached the target
while the digest claimed them as part of the identity. The directory patterns
now end in a slash (`build*/`), and both files arrive.

What is still unproven: the earlier session's mismatch was not reproduced
from the current tree, so it is recorded as unexplained and now gated by
measurement rather than by inspection.

## E1: two-hour concurrency soak, both scenarios (2026-11-12)

Two full 7200 s windows on the installed package
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d` (no
`LIBVA_DRIVERS_PATH` override), boot `17928711-7fd8-4f87-a024-9bf6b5bc44b6`
unchanged across both, decoder `/dev/video5` resolved by the `iris_driver`
QUERYCAP name, kernel `7.2.6-sm8550-g42f3b40-iris1` with 0001-0004 + 0006,
production profile, ceiling 85000 millic. Both ran on the pack, unplugged.

| Scenario | Guard | Contexts | Ledger | Peak SoC | Pack |
| --- | --- | --- | --- | --- | --- |
| `dual-h264` | `212101Z`, `status=complete` | 2 x H.264 640x360 @ 24 fps | 172809/172809 each, `errors=0 drops=0 timeouts=0 misplaced=0` | 41.0 C while decoding (56.8 C for the whole run) | 74 % -> 48 % |
| `mixed` | `232127Z`, `status=complete` | H.264 and HEVC 640x360 @ 24 fps | 172809/172809 and 172800/172800, `errors=0 drops=0 timeouts=0 misplaced=0` | 47.7 C while decoding (78.9 C for the whole run) | 48 % -> 19 % |

The media is the repo's own 48-frame 640x360 clip looped, so this is a
stability and concurrency window, not a resolution or power test: the trace
reports `capture configure 640x384 stride=640 pool=32 memory=mmap`, 172809
`publish=direct` records and no Import. The trace counts access units
(172809 submitted here) while the harness counts output frames (172800); the
ledger's two sides agree in both files, which is the property that matters.

Both sessions ended with `drains=1` and `output_queued=0`, and the preflight
frame-MD5 comparison passed for H.264 and HEVC before each window, so the
decode path was correct going in, not merely quiet. The two peak columns are not the same measurement, and the difference is the
fixture. Each peak is the maximum over every thermal-zone sample in the run's
environment CSV (11139 rows, 30 s interval); the single-snapshot
`temperature-current.csv` is overwritten each interval and is not a maximum —
reading it after the fact reports the last sample (37.8 C and 37.2 C). The
whole-run figures are set by media generation, not by decode: `mixed` writes a
451 MB HEVC fixture with x265 through all cores at 23:30:27Z and the 78.9 C
sample is 23:30:34Z, seven seconds later. Restricted to the decode window, the
two scenarios sit at 41.0 C and 47.7 C. Both stay well under the 85 C ceiling
either way, and the guard's own pre-scenario cooldown check reported 40.6 C
before `dual-h264` and 58.8 C before `mixed`, the second because the windows
ran back to back.

This is the same boundary the power sampler records: fixture generation is
load the soak's ceiling covers but does not attribute, so a temperature number
from a soak with generated media is not a decode temperature.

Nothing in either window was shortened, no boot ID changed, and no reset or
thermal breach occurred. Together with E3/E4 below this covers the decode side
of the E-series; E2 remains open on the software arm's thermal boundary.

## E3: twenty minutes of continuous 4K decode (2026-11-12)

The 4K Import probe above is 30 frames; E3 asks for a window long enough for
the thermal steady state. One guarded run decoded the same clip, real-time
paced, for 1200 s under `scripts/iris-experiment-guard.sh` with a 30 s
temperature interval and the 85000 millic ceiling:

| Field | Value |
| --- | --- |
| Guard | `guard-20261112T013054Z`, `status=complete`, timeout 1500 s |
| Decode | 36000 frames, `fps=30.01`, exit 0, no error or drop record |
| Boot ID | `17928711-7fd8-4f87-a024-9bf6b5bc44b6`, unchanged, no reset |
| Peak SoC | 43400 millic over 1840 zone samples; last sample 34700 |
| Pack | 19 % -> 15 %, discharging, 3.69 V x 0.67-0.72 A (~2.5 W) |
| Driver | installed `323c07cbd42f…`, decoder `/dev/video5`, 0001-0004 + 0006 |

The ceiling was never approached, the pack stayed above the point where the
device protects itself, and the rate held at the clip's own 30 fps for the
whole window, so a 4K AV1 Import stream runs at real time indefinitely on this
target rather than degrading. This is a thermal and continuity result only:
no power ratio is claimed from it, and it is not a peak-throughput figure —
that is the 3.07 ms per frame in the section above.

## VP9 Import qualified (2026-11-12)

The 2026-11-11 record (section "Why VP9 Import still is not qualified" above)
left VP9 at `publish_import: fallback` for one measured reason: Chrome destroys
a VP9 surface as soon as it has composited the frame, so 541 of 718 completions
arrived for a target the client had already released and were counted as
drops. The session now retains such a target and completes it — the client's
buffer is already in the firmware's hands and the backlog holds its own
duplicate of the fd, so the picture is delivered and the surface is never
touched (`session: complete an Import picture whose surface the client
dropped`). A retained target outlives its `StableBuffer`, so the backlog entry
carries a copy of the client layout taken at submit; the resolution-change
comparison reads that copy instead of the freed buffer
(`session: keep a retained target's geometry with its picture`).

### Runs

Source commit `966a7df` (host tree clean), build-tree override, driver
`0ef0f34cc6eb8ccd468f431d71485d64230d0e14fd918e6340e0cd9230caceb8` built from
`<scratch>/fix1-tree` on the target, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `5feea1b5-94ff-4793-94d2-689b3cdb23ca`,
decoder resolved at runtime to `/dev/video17`. Every run below is guarded
(`scripts/iris-experiment-guard.sh`, 85000 millic ceiling); all guards reported
`status=complete` with the boot ID unchanged, no reset and no thermal breach
(hottest zone during the Chrome windows 48.9 C, pack 80 % discharging).

| Gate | Result |
| --- | --- |
| Host | `sh test/run-static-checks.sh` PASS, `meson test -C build-local` 26/26, `git diff --check` clean |
| `test/iris-matrix.sh`, `V4L2_VA_PUBLISH=import` | H.264, VP9, HEVC, AV1 48/48 exact frame MD5; `import=48`, `submitted=48 completed=48 errors=0 drops=0 misplaced=0 retained=0` for each codec |
| FFmpeg, 1280x720 VP9 with alt-refs (`lag720.webm`, 480 frames), Import | `submitted=515 completed=515 errors=0 drops=0 misplaced=0`, 515 `publish=import`, `memory=import`; frame MD5 identical to the software decode |
| Chrome 156, same clip, 60 s, Import, run A (`e1d0585b…`) | `submitted=1705 completed=1705 errors=0 drops=0 misplaced=0 retained=1161`; `VaapiVideoDecoder`, platform=true, 1440 decoded / 0 dropped |
| Chrome 156, 640x360 alt-ref clip, 60 s, Import, run B | `submitted=1588 completed=1588 errors=0 drops=0 misplaced=0 retained=12`; 1439 decoded / 0 dropped |
| Chrome 156, 1280x720 clip, 60 s, Import, run C (driver above) | 1705 `submit`, 1705 `publish=import`, 0 `publish=drop`, 0 `publish=error`, 0 `publish=misplaced`, 1154 `target detached`; 1440 decoded / 0 dropped |
| Chrome 156, same clip and window, Copy baseline (installed `323c07cb…`, `memory=mmap`) | `submitted=1705 completed=1702 drops=3`; 1440 decoded / 0 dropped |
| `test/iris-real-clip-gate.sh`, `IRIS_REAL_CLIP_SET=drc` (session-change regression) | 80/80 exact framemd5, `submitted=84 completed=84 import=42 copy=42 invisible=63` — the AV1 resolution-change record unchanged |
| `test/iris-dynamic-resolution.sh`, `IRIS_DYNAMIC_CODECS=vp9`, Import | native baseline and VA both 202 frames over 100 switches, 101 contexts, `drops=0 timeouts=0`; fresh-context transitions 100/100 at 2/2, pixels equal to software |
| FFmpeg over the same 100-switch VP9 stream, Import | 202/202 exact framemd5, 202 `publish=import`, 101 sessions each `memory=import`, `import rejected=0` |
| FFmpeg, 1280x720 VP9 Profile 2 (10-bit, `yuv420p10le`), Import | `submitted=120 completed=120 errors=0 drops=0 misplaced=0`, 120 `publish=import`, P010 stride 2560, framemd5 identical to the software decode |

The Copy baseline is the number the Import path has to beat: it dropped 3 of
1705 and completed 1702, while Import completed every submitted picture and
dropped none. Chrome's own counters (1440 decoded, 0 dropped) are identical in
both, which is why the session counters, not the page's, are the evidence.

`framemd5` needs matching output formats on both sides: this clip decodes as
NV12 under VA and yuv420p under libvpx, so the first comparison hashed
different byte layouts. With `-pix_fmt yuv420p` on both, all 480 frames match;
the raw planes are byte-identical frame by frame, not merely close. The matrix
and the FFmpeg rows use the normalised form.

The 1161 retained targets in run A are the measurement that motivated this
work: nearly 70 % of the pictures Chrome composited had their surface destroyed
before the completion arrived. Under the previous code those were drops.

Two unit cases pin the behaviour: a released Import target completes instead of
dropping, and a two-completion (alt-ref) picture released before either half
returns retires both halves with no drop, no error and no slot left queued.
Both fail with the detach condition disabled.

### What the capability flip changes

`data/iris-codec-capabilities.json` now has VP9 `publish_import: qualified`,
`src/va/driver.cc` advertises `V4L2_PIX_FMT_VP9` in `import_formats`, and both
README capability tables say Import. A pre-exporting client (Chrome, or
`V4L2_VA_PUBLISH=import`) therefore writes VP9 frames straight into its own
exported buffers. A client that does not pre-export still gets Direct or Copy
exactly as before, and any run where the client layout does not match the
CAPTURE layout degrades to Copy with the recorded `import rejected` trace.

The browser evidence is one 720p clip with alt-refs and one 640x360 clip; the
10-bit path (Profile 2) is a separate FFmpeg row above, exact at 120 frames.
Chrome 156 has no 10-bit window on this build: offered the same clip, it
selected `VpxVideoDecoder` and never opened the Iris node (45 s, 1073 frames,
0 dropped, `is_platform_video_decoder=false`). The refusal is Chrome's, not the
driver's — `vainfo` lists `VAProfileVP9Profile2` and FFmpeg decodes the clip
through VA-API — so the 10-bit browser path has no evidence either way.

Resolution changes are covered across decoder contexts, not inside one:
FFmpeg reopens the VA context at each VP9 geometry change (101 sessions,
`reconfigures=0` over the 100-switch stream), so a VP9 in-context reconfigure
cannot be driven from it. That path is exercised by the AV1 clip gate above,
which runs the same `Session::reconfigure` with an Import pool; the VP9
in-context case is a recorded boundary, not a passing gate. The script's
header comment still claimed VP9 was refused — it was withdrawn on the old
tree and Phase 5 re-enabled it — and now says what the code does.
If a later run shows drops beyond the Copy baseline or a non-zero `misplaced`,
the table returns to `fallback` before any claim.

Artifacts on the target: `<scratch>/vp9final-{matrix,ffmpeg,chrome}`,
`<scratch>/vp9retain-{b,c,d}`, `<scratch>/vp9copy-{a,b}`.

## E2 closed: the battery-side A/B runs under the new ceiling (2026-11-12)

The guard's default ceiling moved from 85000 to 100000 millidegrees
(`guard: make the temperature ceiling a runaway backstop`): these zones trip
passively at 90/95 C and critically at 110 C, so the old default stopped runs
inside the firmware's own throttle band. The peak is still sampled every
interval and still belongs in the record, and a reset, a boot-ID change or a
trip past the ceiling still fails the experiment.

Two full A/B runs on the installed package `323c07cbd42f...`, boot
`5feea1b5-94ff-4793-94d2-689b3cdb23ca`, decoder resolved to `/dev/video17`,
unplugged, backlight 0 for the window and restored afterwards, pack
77 % -> 75 % discharging:

| Run | VA arm | Software arm | Delta | Peak zone |
| --- | --- | --- | --- | --- |
| `e2-temp-c` | 1.53 W (+/- 0.64, n=4) | 6.01 W (+/- 0.69, n=4) | -4.48 W | 91.1 C, `thermal_zone14` |
| `e2-temp-d` | 1.47 W (+/- 0.41, n=4) | 5.84 W (+/- 0.47, n=4) | -4.37 W | 92.3 C, `thermal_zone8` |

The temperature column is the highest 30 s sample, not a continuous maximum.

Both guards are `status=complete`, boot ID unchanged, no thermal breach and no
reset. The software arm reached 91-92 C during its windows without the
kernel's critical trip; that is the condition the old 85 C default refused to
allow, and it is recorded rather than filtered.

Identity and boundaries: clip `testsrc2` 1920x1080 High profile, 300 frames,
the stream reports 60 fps (the aborted attempts above used a 30 fps source, so
the two workloads are not directly comparable; both arms here use the same
clip). The gauge is the whole device, so every number is an increment over the
10 s baselines either side, never decoder-package power. Round 1 of the VA arm
is the low outlier in both runs (0.93 W and 1.10 W against 1.6-1.7 W in rounds
2-4) — the first decode window is still warming the clocks. The means above
include it.

E2 is closed: the hardware-versus-software battery-side increment is about
4.4 W in favour of the VA path on this device, measured twice in the same
shape. It is not a per-package power figure and it is not a thermal
qualification of the software arm.

## 0005 on the target: the kernel recycles completions this driver consumes (2026-11-12)

Patch 0005 was built into the module for the first time and run against the
installed driver. The result is not a refinement of the current AV1 path: with
0005 loaded, AV1 decode stops after a handful of pictures under every publish
policy, and the driver's own accounting names the reason.

Identity: module
`6ef2fce835eb5aca52546482058511874c883e908b024f39f61e2775682d8099`
(0001-0005, built with `packaging/kernel/build-module.sh`; its build log lists
the five applies and no 0006),
installed at `/lib/modules/7.2.6-sm8550-g42f3b40-iris1/.../qcom-iris.ko.zst`,
one reboot, boot `1edbadbe-f61b-4c7a-b229-a94b71e8539e`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, installed package
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d` (no
`LIBVA_DRIVERS_PATH` override), decoder and encoder as the driver's own
`va init node=/dev/video0 encoder=/dev/video1` line reports them. The nine
runs are seconds long and were not wrapped in the experiment guard; none of
them changed the boot ID, and none was a thermal window. One experimental
patch was added at
a time; the module before and after the experiment is
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`
(0001-0004 + 0006), kept as a verified rollback copy.

Client: `ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -pix_fmt
yuv420p -f framemd5`, `V4L2_VA_SYNC_TIMEOUT_MS=1000`, `V4L2_VA_TRACE=1`,
`LIBVA_DRIVER_NAME=msm`, one process per clip per policy. The clips are the
ones the qualified gate decodes exactly (`av1-720.webm` 120 frames,
`resize-av1.mkv` 80 frames, `av1-4k.mkv` 30 frames).

| Clip | `V4L2_VA_PUBLISH` | Session finish | Exit |
| --- | --- | --- | --- |
| `av1-720.webm` | import | `submitted=7 completed=1 errors=6 drops=0 timeouts=0 drains=1 misplaced=1` | 251 |
| `av1-720.webm` | copy | `submitted=7 completed=2 errors=4 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |
| `av1-720.webm` | direct | `submitted=7 completed=2 errors=4 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |
| `resize-av1.mkv` | import | `submitted=6 completed=1 errors=5 drops=0 timeouts=0 drains=1 misplaced=1` | 251 |
| `resize-av1.mkv` | copy | `submitted=7 completed=3 errors=3 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |
| `resize-av1.mkv` | direct | `submitted=7 completed=3 errors=3 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |
| `av1-4k.mkv` | import | `submitted=6 completed=1 errors=5 drops=0 timeouts=0 drains=1 misplaced=1` | 251 |
| `av1-4k.mkv` | copy | `submitted=6 completed=2 errors=3 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |
| `av1-4k.mkv` | direct | `submitted=6 completed=2 errors=3 drops=0 timeouts=1 drains=1 misplaced=0` | 251 |

Every run failed with exit 251 after one to three pictures, so the gate's
frame-for-frame comparison against software is not available for this module:
the client never reached the end of a clip. The native control on the same boot
decoded the identical clip through the identical node: `matroskademux !
av1parse ! v4l2av1dec` reached `Got EOS` with exit 0.

### Why this module cannot serve this driver

0005 removes the ERROR flag from NOSHOW completions
(`iris_hfi_gen2_get_driver_buffer_flags`) and then, for a decoder NOSHOW
picture with no error and no LAST flag, calls
`iris_hfi_gen2_requeue_output_buffer()` instead of completing the buffer to
vb2. The completion the driver receives is recycled inside the kernel, so
userspace never dequeues it.

That completion is load-bearing here. `Session::handle_capture_returns()`
attributing a hidden picture (`capture noshow`) is how the pending target
submitted for `show_frame=0` is cleared, how `Target::invisible` is counted,
and — in Import — how a second borrowed client buffer per install unit is
returned. Without the completion, `pending_` never empties: the copy and
direct runs complete 2 or 3 of the 6-7 submitted pictures and never return
the rest, the sync bound expires (`sync timeout token=6 pending=5 queued=32`
in the `av1-720.webm` copy run), and the drain reports `drain lost token=`,
followed by an early `capture empty ... last=1`.

Import fails earlier and more loudly. The pool maps a CAPTURE index to the
token that was lent for it; a recycled buffer comes back later for a different
picture, and the session's neighbour check refuses to publish it —
`capture token=7 index=0 flags=0x4010 publish=misplaced slot_token=2`, then
`va sync failed surface status=23: target decode failed`. The Import invariant
did what it was written to do: a completion that lands in a neighbour's buffer
failed both surfaces instead of publishing wrong pixels.

### The upstream symptom does not reproduce on this target

0005 exists because a client that treats a NOSHOW ERROR completion as a failed
picture drops the pending frame with it (the patch cites 48 of 60 frames from
GStreamer on X1E80100). GStreamer 1.28.8 here does not: with
`GST_DEBUG=v4l2allocator:6` on `av1-720.webm` through `v4l2av1dec`, the decoder
dequeues all 180 CAPTURE buffers — 120 with payload and 60 empty NOSHOW ones —
and `fakesink` receives every pictured frame.

The same pipeline writing to `filesink` lands 89 of 120 frames in the file, but
the decoder still logged 180 dequeues with the same 120/60 split in that run,
so what is missing was lost downstream of the decoder, in how fast the sink
drains. Reading a frame count off that pipeline says nothing about NOSHOW.

### What a complete 0005 would have to be

Not a patch refinement. It changes the decoder-to-userspace contract from
"one completion per submitted picture" to "displayed pictures only", so it
needs a userspace DPB-slot model that never expects a completion for a hidden
picture, a rewrite of the `AccessUnit::completions` / `Target::invisible`
accounting, the §8 trace vocabulary and its consumers, invariant 17, and the
capability table's `sync_note`. It also needs a way for userspace to learn
which contract the kernel implements: 0005 adds no V4L2 control, no QUERYCAP
flag and no `VIDIOC_QUERY_EXT_CTRL` entry, only the internal
`output_stopping` field, so one `v4l2_drv_video.so` cannot serve both kernels.
What it would buy is the removal of a fake `V4L2_BUF_FLAG_ERROR` on NOSHOW
completions, which `src/iris/session.cc` already recognises by shape
(no picture-type bit, no payload, zero timestamp) and publishes as a hidden
picture without treating it as a failure.

### Control arm on the module 0005 was built from

The rollback copy was written back (`rollback_to_module=b90bd823...` verified
on disk) and the tablet rebooted. The control arm ran on the boot that
followed: boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f`, module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`
(0001-0004 + 0006), installed
package `323c07cb...` again, same kernel and same client command as the 0005
arm, each clip wrapped in the experiment guard.

| Clip | Guard | Framemd5 | Session finish | Publish |
| --- | --- | --- | --- | --- |
| `av1-720.webm` | `complete` | 120/120 exact | `submitted=120 completed=120 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | 120 import, 42 invisible |
| `resize-av1.mkv` | `complete` | 80/80 exact | `submitted=84 completed=84 errors=0 drops=0 timeouts=0 drains=2 misplaced=0` | 42 import, 42 copy-gpu, 63 invisible, 1 layout-mismatch rejection |
| `av1-4k.mkv` | `complete` | 30/30 exact | `submitted=30 completed=30 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` | 30 import, 1 invisible |

`IRIS_REAL_CLIP_DIR=... IRIS_REAL_CLIP_SET=all sh test/iris-real-clip-gate.sh`
on the same boot: `PASS av1-720 frames=120 import=120 invisible=42`,
`PASS resize-av1 frames=80 import=42 copy=42 invisible=63`, `PASS real AV1 clip
gate set=all publish=import`.

Same clips, same client command, same installed driver SHA-256, same node
resolution, and the control reproduces the qualified counts exactly. The arms
are two modules, not one patch apart: the 0005 build was made without 0006, so
the arm that completes nothing is the arm without the timestamp patch. That
direction rules 0006 out as an explanation for the failure — the arm that has
it fails, the arm that lacks it is exact — and 0006 only stops copying flags
and timecode from the timestamp ring onto a dequeued buffer, so it adds and
removes no completion. The 0005 arm completed one to
three pictures per clip; the control completed every submitted picture with
bit-exact output.

No codec, profile or publish policy is advertised or withdrawn by this run.
The qualified baseline module stays `b90bd823` (0001-0004 + 0006) with the
shipped driver unchanged, and 0005 stays out of every module this project
builds.

## 4K 10-bit AV1 through Import, exact against libdav1d (2026-11-12)

The 10-bit record was one 1080p clip and the 4K record was 8-bit; the two had
never met. This run decodes a 3840x2160 10-bit AV1 clip through the Import
policy and compares every frame against an independent software decoder on
the same machine.

Clip: 3840x2160, 30 frames, `yuv420p10le`, AV1 profile `Main`, encoded with
`libsvtav1` (`-preset 8 -crf 30`) on the lab host, SHA-256
`83ea2997a9feb88b6db3ab820499580d64ba9d92ce8e446b52778e1bec3e41f6`. The
source is `testsrc2`, so this is a depth-and-geometry probe, not real-content
quality evidence.

Identity: installed package
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d` with no
`LIBVA_DRIVERS_PATH` override, module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`
(0001-0004 + 0006, the installed experimental module), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f` unchanged across the run, decoder and
encoder as the driver's own `va init node=/dev/video17 encoder=/dev/video18`
line reports them, pack 64 % when the boot came up.

Oracle: `ffmpeg -c:v libdav1d ... -pix_fmt yuv420p10le -f framemd5` on the
target. VA run: `-vaapi_device /dev/dri/renderD128 -hwaccel vaapi
-hwaccel_output_format vaapi -vf hwdownload,format=p010le,format=yuv420p10le
-f framemd5`, `V4L2_VA_PUBLISH=import`, `V4L2_VA_SYNC_TIMEOUT_MS=5000`, trace
on, wrapped in the experiment guard (900 s, ceiling 100000 millic).

| Result | Value |
| --- | --- |
| framemd5 | 30/30 frame hashes equal to the libdav1d oracle |
| session | `submitted=30 completed=30 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` |
| publish | `import=30 copy=0 invisible=1 drop=0 error=0`, `import rejected=0` |
| guard | `status=complete`, boot ID unchanged, 31 s window |

P010 surfaces were allocated at 3840x2160 from the first picture
(`va create_surfaces ... fourcc=P010`), so the depth is decided before the
first access unit here as well. This adds no VA profile and no capability
entry: it fills D4's "4K 10-bit" line with exact pixels. The browser 10-bit
pixel comparison is settled as not a gate in the backlog's D1 row, for the
same reason the project counts a browser decode only from media-surface
evidence.

## GStreamer VA plugin on sheng registers no elements (2026-11-12)

`gst-inspect-1.0 va` on `sheng` reports the plugin but `0 features`:
`libgstva.so`, version 1.28.8, source module `gst-plugins-bad`, linking
`libva.so.2` and `libva-drm.so.2`. `gst-inspect-1.0 vaav1dec` fails, and the
full element list contains no `va*dec` or `va*enc`, only the `v4l2*`
decoders and encoders (`v4l2av1dec`, `v4l2h264dec`, `v4l2h265dec`,
`v4l2vp9dec`).

This is where D3's "GStreamer `vaav1dec` refuses the codec" line actually
stands: the element is not registered at all, so a refusal attributed to the
decoder was never a decoder result on this target. D3's GStreamer half and
H4's encode half both need a plugin build that registers its elements before
they mean anything. No driver behaviour and no capability entry changes here.
The filter that refuses this driver is libgstva's vendor allowlist, and
`GST_VA_ALL_DRIVERS=1` bypasses it; see "GStreamer VA elements register once
libgstva's vendor allowlist is bypassed" below.

## 4K AV1 with 32 tiles through Import (2026-11-12)

The tile coverage in the record was four tiles at 1280x720 (the 2026-10-23
loop-restoration run) and the A11 corpus' `tiles 2x2` at 640x360. This run
decodes a 3840x2160 stream split into 32 tiles.

Clip: 3840x2160, 30 frames, `yuv420p`, AV1 profile `Main`, `libsvtav1`
`-preset 8 -crf 30` with `tile-columns=3:tile-rows=2`, SHA-256
`3bb8e33fe60c5bd813c7a7081f54c8aafedcbf18ca498990b90db945fa0a31c8`; libdav1d
reports `tiles 4x8` for it and `trace_headers` shows `tile_cols_log2 = 3`.

Identity is the same boot as the 4K 10-bit probe: installed package
`323c07cb...`, module `b90bd823...` (0001-0004 + 0006), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f` unchanged, decoder `/dev/video17`.

| Result | Value |
| --- | --- |
| framemd5 | 30/30 frame hashes equal to the libdav1d oracle |
| session | `submitted=30 completed=30 errors=0 drops=0 timeouts=0 drains=1 misplaced=0` |
| publish | `import=30 copy=0 invisible=1 drop=0 error=0`, `import rejected=0` |
| guard | `status=complete`, 31 s window |

The multi-tile frame header is rebuilt from VA's decode parameters, so this
also exercises the writer's tile-count path beyond the four-tile case. 12-bit
stays in the capability table's `rejected` list; no gate for it exists and
none is claimed here.

## 12-bit AV1 is refused at the VA layer, not mis-decoded (2026-11-12)

The capability table lists 12-bit as `rejected` with no hardware gate behind
it. This run checks that the rejection is a refusal rather than a decode that
returns wrong pixels.

Clip: 640x360, 10 frames, `yuv420p12le`, AV1 profile `Professional`,
`libaom-av1 -cpu-used 8 -crf 40`. The software oracle decoded all 10 frames
with `libdav1d`. The same boot and driver as the 4K probes (installed package
`323c07cb...`, module `b90bd823...`, boot `b2f1de2f...`, decoder
`/dev/video17`).

The VA attempt (`-vaapi_device /dev/dri/renderD128 -hwaccel vaapi
-hwaccel_output_format vaapi`, `V4L2_VA_PUBLISH=import`, trace on) exited 69
with `Your platform doesn't support hardware accelerated AV1 decoding`,
`Failed to get pixel format`, and `Nothing was written into output file`. The
trace stops after `va init` — no `va create_context`, no session, no
publication.

A 12-bit stream therefore fails where the profile is selected, before any
surface is allocated, and the table's `rejected` entry stays as it is. This is
negative evidence for the entry, not a hardware 12-bit gate.

## 12-bit AV1 has no format at the node, so no hardware gate can exist (2026-11-12)

The VA-layer refusal above left one question open: does the platform decode
12-bit and only this driver refuse it, or is there no 12-bit path at all? The
native V4L2 decoder answers it without touching the VA driver.

Identity: driver `323c07cbd42f5a15…` (installed package), module
`b90bd823fdab9895…` (0001-0004), kernel `7.2.6-sm8550-g42f3b40-iris1`, boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f`, decoder `/dev/video17` resolved by the
`iris_driver` QUERYCAP name. The `v4l2av1dec` element's `device` property is
read-only and defaults to `/dev/video17`; the probe compares that default with
the resolved node and refuses to run if they differ, so no node literal is
used.

Two guarded runs of the same pipeline
(`filesrc ! matroskademux ! av1parse ! v4l2av1dec ! fakesink`, `-e -v`):

| Run | Clip | Guard | Output caps | EOS | CAPTURE dequeues |
| --- | --- | --- | --- | --- | --- |
| control | 640x360 8-bit `libaom-av1`, 5 frames, sha256 `9dc7cec4…` | `status=complete` | `video/x-raw(memory:DMABuf), format=DMA_DRM, drm-format=NV12, 640x360` | yes | 16 |
| 12-bit | 640x360 `yuv420p12le`, profile `Professional`, sha256 `69bbce35…` | `status=interrupted` (child rc=1) | none | no | 0 |

The control decodes on the same node and pipeline, so the pipeline is sound.
The 12-bit run fails at caps negotiation with `not-negotiated (-4)` before any
buffer exists: `av1parse` supplies `bit-depth-luma=12, profile=professional`,
and the decoder has no CAPTURE format to offer it. `VIDIOC_ENUM_FMT` on the
node lists `NV12`, `Q08C`, `P010` and `Q10C`; none carries 12 bits.

The firmware may still decode 12-bit internally, but there is no format to
deliver it in, so no buffer-level gate exists for this platform and the
capability table's `rejected` entry cannot be upgraded by any stream. The
entry stays as it is; this run adds the reason, not a capability.

## Encoder rejections: which are the format space and which are this driver (2026-11-12)

H3 asks whether every rejected encode feature is a hardware limit or a policy
entry. The encoder node answers most of it without encoding anything.

Identity: driver `323c07cbd42f5a15…`, module `b90bd823fdab9895…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `b2f1de2f-b3da-4e24-9fa4-163e489d929f`.
The encoder node is `/dev/video18`, resolved by `iris_resolve_encoder`
(QUERYCAP `iris_driver`, compressed format on CAPTURE).

Raw input (OUTPUT) formats: `NV12`, `Q08C`. Coded (CAPTURE) formats: `H264`,
`HEVC`. Relevant controls, read with `v4l2-ctl --list-ctrls-menus`:

| Control | Range / menu | What it means here |
| --- | --- | --- |
| `video_b_frames` | 0..1, default 0 | the node accepts one B-frame |
| `hevc_profile` | `Main`, `Main Still Picture`, `Main 10` | Main 10 is listed |
| `h264_profile` | `Baseline`, `Constrained Baseline`, `Main`, `High`, `Constrained High` | the node accepts five; the table exposes three (Baseline and Constrained High are not exposed, which is a profile decision outside H3's list) |
| `enable_h264_hierarchical_coding`, `h264_hierarchical_coding_type`, layer bitrates | 0..1, 0..5 layers | temporal layers exist |
| `ltr_count`, `frame_ltr_index`, `use_ltr_frames` | 0..2, -1..1, bitmask 0x3 | long-term references exist |
| `prepend_sps_and_pps_to_idr` | 0..1 | the node's own header mechanism |

No control exists for multi-slice, weighted prediction, scaling lists, chroma
QP offsets or a reference count. Mapping the table's `rejected` list onto that:

| Rejected entry | Node-level fact | Verdict |
| --- | --- | --- |
| B-frames | `video_b_frames` accepts 1, with B-frame min/max QP controls for both codecs | **this driver pins it to 0** (`src/iris/encode.cc:176`), and the translators refuse a B slice (`src/codec/encode_h264.cc`, `encode_hevc.cc`). A client can ask: `VAConfigAttribEncMaxRefFrames` carries `l1_ref_frame`, `VAEncPictureParameterBufferH264.picture_type` names a B picture and `VAEncSliceParameterBufferH264.RefPicList1` names its forward reference, so the entry is this driver's policy rather than a hardware or interface limit |
| packed headers | no V4L2 equivalent; `prepend_sps_and_pps_to_idr` is the node's own mechanism | interface-level, unchanged |
| multi-slice | no control | not requestable through V4L2 |
| weighted prediction, scaling lists, chroma QP offsets | no controls | not requestable; the firmware default applies |
| more than one reference index | no reference-count control (LTR and temporal layers are different knobs and stay at 0) | not requestable through V4L2 |
| 10-bit or non-4:2:0 | the raw port accepts `NV12` and `Q08C` only | hardware-backed: no input format carries 10 bits or another chroma layout |
| HEVC Main10 | `hevc_profile` lists `Main 10`, but the raw port has no `P010` | the profile is advertised and the input format space is not; the entry stays rejected and this is the reason |

`src/iris/encode.cc:140` is where the second-to-last row is enforced: the
driver sets the raw format to `NV12` and refuses the context if the node does
not accept it. No capability entry changes; what changes is that the reasons
are now recorded instead of implied.

## Decode rejections: two are the client's caps, one hangs at the node, one was not real (2026-11-12)

The capability table also keeps `interlaced`, `non-8-bit or non-4:2:0` and
`12-bit` in the decode `rejected` lists, and HEVC keeps `long-term
references`. Each was measured on the node instead of read off the code.

Identity for every run below: driver `323c07cbd42f5a15…` (installed package),
module `b90bd823fdab9895…` (0001-0004), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f`, decoder `/dev/video17` resolved by the
`iris_driver` QUERYCAP name. The GStreamer decoder elements expose `device` as
read-only, so each probe compares the element default with the resolved node
and refuses to run when they differ.

The node enumerates `H264`, `HEVC`, `VP90` and `AV01` on OUTPUT, and `NV12`,
`Q08C`, `P010`, `Q10C` on CAPTURE. Every CAPTURE format is 4:2:0, none carries
more than 10 bits, and that is the format space all of these streams land in.

| Run | Stream | Guard | Reached the node | Outcome |
| --- | --- | --- | --- | --- |
| H.264 control | High, 4:2:0 progressive, sha256 `f19fd692…` | `complete` | yes | 62 CAPTURE dequeues, src caps, EOS, `drm-format=NV12` |
| H.264 interlaced | High, `interlace-mode=interleaved`, `field-order=bottom-field-first`, `coded-picture-structure=field`, sha256 `e1ea1883…` | `interrupted` | no | the element refuses: `Device '/dev/video17' does not support interleaved interlacing`; zero buffers |
| H.264 High 4:2:2 | 4:2:2, Annex-B, sha256 `f8156193…` | `interrupted` | no | `not-negotiated (-4)` at the parser's source link; zero buffers |
| H.264 High 10 | 10-bit 4:2:0, Annex-B, sha256 `fe1cfbcf…` | `interrupted` | no | `not-negotiated (-4)`; zero buffers |
| HEVC Main 12 | `yuv420p12le` Rext, sha256 `4035f7d4…` | `interrupted` | no | `not-negotiated (-4)`; zero buffers |
| VP9 control | profile 0, 8-bit, sha256 `310a6b92…` | `complete` | yes | 10 buffers, src caps, EOS |
| VP9 profile 2 | 12-bit `yuv420p12le`, sha256 `beb375fd…` | `interrupted` | yes | 10 access units accepted, no source-change, no CAPTURE dequeue, `Waiting for decoder stop` until the guard expired |

The 4:2:2, High 10 and Main 12 refusals happen at the decoder element's sink
caps: `gst_v4l2_video_dec_sink_getcaps` returns H.264 profiles
`{baseline, constrained-baseline, main, high, constrained-high}` with no
`chroma-format` or `bit-depth` fields, so `high-4:2:2` and `high-10` never
negotiate and no buffer is queued. That is the client's template, not a
measurement of the node. The node-level half is the CAPTURE enumeration
above: a 4:2:2 or 12-bit picture has no format here to be delivered in. Both
halves say the same thing, and no stream can upgrade either entry.

VP9 profile 2 is the stream that does reach the node with 12-bit content, and
it does not fail cleanly. The node takes all ten access units (`Passing buffer
with system frame number 0` … `9`), never signals a source change, never
returns a CAPTURE buffer, and the element sits in `Waiting for decoder stop`
until the guard budget expires; the first attempt used the full 180 s. The
8-bit control runs the same pipeline, node and debug domain to EOS, so the
pipeline is sound. The entry stays, and what is new is that a 12-bit VP9
client on this node hangs instead of being told no.

Long-term references were the entry that did not survive. The table said
"generated parameter sets cannot describe them", which the slice writer
contradicts: `src/codec/hevc.cc` writes an explicit `st_ref_pic_set(0)` plus
the long-term section with `delta_poc_msb_present_flag=1` into every slice
header, and that writer landed on 2026-09-21, one day after the table entry.
The JCT-VC conformance stream `LTRPSPS_A_Qualcomm_1.bit` (416x240 Main,
sha256 `8f8e50fa…`, from the `fate-suite` mirror of the conformance set) has
`long_term_ref_pics_present_flag=1` and `num_long_term_ref_pics_sps=8` in the
SPS and `num_long_term_pics=1` in 161 of its slice headers, per
`trace_headers`. Through this driver it decodes 500/500 frames with framemd5
identical to libavcodec, and a locally encoded 20-frame control decodes 20/20
identical. `data/iris-codec-capabilities.json` now records the boundary that
does exist — long-term pictures whose POC MSB cycles do not ascend, which
`write_rps` refuses with `VA_STATUS_ERROR_UNIMPLEMENTED` — instead of the
stale claim.

## The completion-preserving 0005 form on the target (2026-11-12)

0005 as written recycles the NOSHOW CAPTURE buffer inside the kernel. The
form this driver could consume is a different patch — 0007: keep the
completion, attach no `V4L2_BUF_FLAG_ERROR`, no picture-type flag and no
payload, keep the timestamp 0004 preserves. It was built, installed and
measured instead of staying a prediction.

Six stages over three boots, all on `7.2.6-sm8550-g42f3b40-iris1`:

| Stage | Boot | Module | Driver | What it measures |
| --- | --- | --- | --- | --- |
| A | `b2f1de2f…` | `b90bd823…` (0001-0004, qualified) | installed `323c07cb…` | baseline |
| B0 | `b2f1de2f…` | `b90bd823…` | chore build `6c554692…`, no tolerance | driver lineage |
| B | `b2f1de2f…` | `b90bd823…` | tolerance build `7260ff05…` | the tolerance on a kernel that sets the flag |
| C | `e48243b8…` | `f070ae18…` (0001-0004 + 0007) | installed `323c07cb…` | the kernel variant alone |
| D | `e48243b8…` | `f070ae18…` | tolerance build `7260ff05…` | the variant with the userspace half |
| E | `31424f69…` | `b90bd823…` | installed `323c07cb…` | rollback, target restored |

Every stage ran the same nine guarded gates: 1280x720 AV1 (120 frames),
1280x720 AV1 with an in-context resolution change (80), 3840x2160 AV1 (30)
and a 1280x720 VP9 alt-ref clip (240), each under `import` and `copy`, plus
the 720p clip under `direct`. A gate passes when its framemd5 matches the
software decode and `publish=error`, `publish=drop` and `publish=misplaced`
are all zero.

| Stage | AV1 gates | VP9 alt-ref | Counters on the AV1 clips |
| --- | --- | --- | --- |
| A | 7/7 identical | 2/2 | `invisible` 42 / 63 / 1, `capture noshow` 59 / 36 / 14, no errors or drops |
| B0 | 7/7 identical | 2/2 | same as A |
| B | 7/7 identical | 2/2 | same as A |
| C | 0/7: `ffmpeg` exits 251 after 2, 3 and 2 frames of 120, 80 and 30 | 2/2 | `capture empty` 6 / 5 / 5, `publish=invisible` 0 |
| D | 7/7 identical | 2/2 | same as A |
| E | 7/7 identical | 2/2 | same as A |

Stage C is what 0007 was built to find out. With the completion kept and the
error flag gone, the installed driver drops the unflagged empty completions
(`capture empty`), never counts an invisible half, and the client's pictures
stay pending until the sync bound: 2 of 120 frames on the 720p clip, 3 of 80
on the resolution change, 2 of 30 at 4K. Stage D runs the same kernel with
the tolerance build and matches stage A field for field, including
`invisible=42` and `capture noshow=59`.

Two things are settled. The host model predicted this split, and on the
target it is the observed split. The tolerance is not a fix for anything in
the field: stages B and B0 are identical, because a kernel that sets
`V4L2_BUF_FLAG_ERROR` never reaches the new branch. 0007 therefore buys
nothing the qualified module does not already do — it is tracked as an
experiment with that verdict, and the userspace tolerance stays on
`exp/0005-keep-tolerance` instead of being merged.

The VP9 alt-ref clip completes in every stage including C, because its
alt-ref superframe is published through Copy here (`publish=copy` 240) and
never exercises the hidden-completion path the patch removes. Stages B and
B0 publish it through Import instead (240 `publish=import`), which belongs to
the driver lineage between the installed package and the chore branch, not to
the tolerance: on the same kernel the two builds are identical.

## GStreamer VA elements register once libgstva's vendor allowlist is bypassed (2026-11-12)

The 0-features result recorded on this page has a cause: `libgstva.so` filters
VA drivers by vendor string in `_gst_va_display_filter_driver` before it
builds any element. With `GST_DEBUG='va*:7'` the plugin logs

```
vadisplay gstvadisplay.c:176:_gst_va_display_filter_driver: VA-API driver vendor: Qualcomm Iris V4L2 stateful (libva-v4l2)
vadisplay gstvadisplay.c:186:_gst_va_display_filter_driver: Unsupported driver: Qualcomm Iris V4L2 stateful (libva-v4l2)
```

and stops, so `gst-inspect-1.0 va` reports `0 features` and `vaav1dec` is not
an element at all. The filter accepts `Mesa Gallium dri`, `Intel i965 drive`
and `Intel iHD driv`; `GST_VA_ALL_DRIVERS=1` is its supported override and
registers seven elements. No `LIBVA_*` variable is set for any run below:
libva reaches this driver through its `msm_drv_video.so` alias, the same way
the browser gate reaches it.

Identity for every run in this section: installed driver `323c07cb...`,
module `b90bd823...` (0001-0004 + 0006), kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot `31424f69-910a-48ac-8385-8f77428e082c`,
`libgstva` 1.28.8, VA-API 1.23.0.

| Check | Result |
| --- | --- |
| `gst-inspect-1.0 va` without the override | `0 features` |
| with `GST_VA_ALL_DRIVERS=1` | 7 elements, including `vaav1dec`, `vah264enc`, `vah265enc` |
| `vaav1dec` decode, 120-frame 1280x720 AV1 clip | 120/120 frame hashes equal to the software decode |
| `vah264enc`, 60 frames 1280x720 NV12 | 60 frames back after the stream is decoded, 1,283,801 bytes |
| `vah265enc`, 60 frames 1280x720 NV12 | 60 frames back, 3,100,049 bytes |

The decode comparison needed a measurement fix before it meant anything. A
first pass piped `vaav1dec ! videoconvert ! video/x-raw,format=NV12 ! fdsink`
into the hasher and differed on all 120 frames at the right size — mean
absolute difference 93/255 on the luma plane, with an unchanged luma average
(125.18 against 125.22) and no shift or chroma swap explaining it. Rendering
one frame through `pngenc` showed the same picture as the software decode —
that render checks geometry, not hashes, because each tool applies its own
NV12-to-RGB conversion and the two PNGs sit at 24 dB apart for that reason
alone — and the raw dump matched the software decode only after a per-frame
circular shift (best roll per frame, mean absolute difference 0.25, six bytes
above 8 once rolled). `fdsink` writes from the buffer base and ignores
`GstVideoMeta`'s plane offset; `pngenc` maps through `GstVideoFrame` and does
not. Muxing the element's own I420 output and hashing that file after a
software decode is exact: 120/120. A raw dump that disagrees with software
therefore says nothing about the decoder, and the gate uses the muxed path.

`test/iris-gstreamer-gate.sh --clip PATH` runs all of the above and prints
the identity block. It is a client-side gate: the driver is the same one the
FFmpeg and browser gates exercise, and no capability entry changes because of
it.

## The sibling backend goes exact on AV1 only with a kernel contract this driver cannot use (2026-11-13)

The piano recipe builds its backend from `radxa-pkg/qcom-va-driver` v1.1.1
(`0980a1d`), and that tree's README makes its AV1 path depend on kernel patch
0005. Comparing patch text cannot settle what that means on this device, so
both user-space drivers were run against both kernel modules with the clips,
the node and the hash harness held fixed.

| | 0001-0004 + 0006 | 0001-0005 |
| --- | --- | --- |
| this driver (installed package) | AV1 720p 120/120 exact, 4K 30/30 exact | 2 of 120 pictures, then `syncSurface: surface sync timeout` |
| `radxa-pkg/qcom-va-driver` v1.1.1 | AV1 720p 78/120 (42 differ, first at frame 33), 4K 29/30 (frame 25), VP9 10-bit 120/120 | AV1 720p 120/120, 4K 30/30, VP9 10-bit 120/120 |

The sibling backend was built on the target at
`<scratch>/radxa-v111/src` (`meson setup build --buildtype=release
-Dcdsp_vpp=disabled -Dvulkan_vpp=disabled`, `ninja -C build`) and reached
through `LIBVA_DRIVERS_PATH` plus `LIBVA_DRIVER_NAME=qcom`. It was never
installed, so the system driver stayed
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`. Every
cell is `ffmpeg -hwaccel vaapi -hwaccel_device <render node>` with
`-hwaccel_output_format vaapi` and `-vf
hwdownload,format=<nv12|p010>,format=<pix fmt>` into `-f framemd5`, compared
line for line against the software decode of the same clip. Clips:
`av1-720.webm` (120 frames, 1280x720), `av1-4k-tiles.mkv` (30 frames, 32
tiles), and a 10-bit VP9 clip (120 frames, downloaded as P010). Every command
ran under `scripts/iris-experiment-guard.sh` with its own
`IRIS_EXPERIMENT_DIR`; every guard ended `status=complete`, the pack sat
between 22% and 26% and discharging, and no thermal zone came near the
ceiling.

Identity: kernel `7.2.6-sm8550-g42f3b40-iris1` on both arms; module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`
(0001-0004 + 0006) on boot
`3356e00e-5a48-4718-8dc1-868fa5cc559d`; module
`6ef2fce835eb5aca52546482058511874c883e908b024f39f61e2775682d8099`
(0001-0005) on boot `17b22f21-5e3d-4888-a891-a87a53cc5247`; both built by
`packaging/kernel/build-module.sh` from `42f3b40c702a`. Each arm was run
twice with the same result. The 0005 module was built without 0006, so the
arms differ by two patches rather than one. That does not weaken the AV1
result: 0006 is absent from the arm that decodes exactly, so its presence
cannot be what makes the other arm fail, and its own WP1 arm recorded no
frame effect.

The frames that differ are not out of order. None of the 42 AV1 hashes that
differ appears anywhere else in the software sequence, and the same holds for
the single 4K frame, so the sibling backend wrote different pixels at those
positions instead of the right pictures in the wrong order; its first 33
pictures match.

Both drivers are exact on one kernel and not the other, and the two kernels
ask userspace for different things. This driver counts one completion per
submitted picture and consumes the NOSHOW completion, which 0005 removes
before it reaches userspace; that is why its arm stops after two pictures and
why `src/iris/session.cc` reports the sync bound instead of a decode error.
The sibling backend is built for the opposite contract and is exact on the
clip set here once the kernel provides it.

Scope: one sibling tree at one commit, one device, three clips. Nothing in
`data/iris-codec-capabilities.json` changes, the qualified module stays
`b90bd823`, and 0005 stays out of every module this project ships.

## Vulkan VPP prerequisites on the target (2026-11-13)

The WP4 verdict of 2026-11-12 was "no proven viable backend": `vulkaninfo`
and `glslangValidator` are absent on the tablet, and no runtime import test
had been done. Those two tools are diagnostics and a shader compiler, not the
runtime contract, and `glslangValidator` runs on the host while the tablet has
`libvulkan`, freedreno, Vulkan headers and libdrm. A userspace probe now
measures the three facts a zero-copy Vulkan video processor needs: a SPIR-V
compute pipeline runs with exact output, an imported DMA-BUF can be read by
that pipeline (VPP source), and a second imported DMA-BUF can be written by it
with the bytes visible through a coherent CPU mmap (VPP destination). Reading
an imported fd alone was the first version of this gate; writing one is what a
video processor actually does to its output surface.

`test/vulkan-vpp/prereq.sh` runs `test/vulkan-vpp/probe.c` on the target. The
shader (`copy.comp`) is a byte copy-plus-one, so 256 output words are fully
predictable. The checked-in `copy.spv` is pinned by `SHA256SUMS` and verified
at run time; `make-spv.sh` regenerates it on a host. The probe allocates both
its source and destination DMA-BUFs each way `src/iris/alloc.cc:allocate_stable`
does: MSM GEM (cached-coherent with a write-combined fallback, mirroring
`src/iris/msm_alloc.c`) exported as a PRIME fd, and the system DMA heap. Both
fds import through `VK_KHR_external_memory_fd` with dedicated allocation; the
shader reads one and writes the other, and the CPU mmap of the destination is
checked after an implicit-fence wait.

Recorded run `run-20261112T205511Z` (`test/remote/gate.sh -- sh
test/vulkan-vpp/prereq.sh`), guard `complete`, exit 0:

| origin | device | result |
| --- | --- | --- |
| system DMA heap | Turnip Adreno 740, Vulkan 1.4.354 | `PASS read-import=exact write-import=exact cpu-coherent=256/256` |
| MSM GEM PRIME fd | Turnip Adreno 740, Vulkan 1.4.354 | `PASS read-import=exact write-import=exact cpu-coherent=256/256` |

Identity is the installed-package arm: kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`3356e00e-5a48-4718-8dc1-868fa5cc559d`, module `b90bd823`, installed driver
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`, with the
decoder and encoder nodes resolved by their `iris_driver` QUERYCAP names
(`/dev/video17` and `/dev/video18` on this boot). The gate needs no driver
rebuild, deploy or V4L2 node; it is a pure userspace probe. SPIR-V sha256
`fa6c588297f62454db4b2e88775d81341891c4f216fff7e2257fc007dfe4e764`, render node
`/dev/dri/renderD128`. The run record's child output holds both PASS lines.

`test/remote/gate.sh` gained a command-file transport in the same commit. Its
heredoc was fed to `sh -s`, but the guard backgrounds that child, so a
non-interactive shell gave it `/dev/null` for stdin: the earlier VPP run
`run-20261112T193045Z` and the earlier GStreamer record `run-gst-20261112`
both have empty child stdout and an exit 0 that ran nothing. The GStreamer
frame-hash results above were collected independently of `gate.sh` and do not
come from that empty record; the I8 ledger row in `POST-MERGE-BACKLOG.md`
records the correction. A marker test (`echo VPP_GATE_MARKER_FIXED`) now
appears in the child's captured stdout, and `run-static-checks.sh` asserts the
transport stays a command file. This is a prerequisite result only: no
`VAProfileNone` / `VAEntrypointVideoProc` capability is advertised, and no VPP
filter exists yet.

## NV12 scale primitive on the target (2026-11-13)

The prerequisite gate above proves a compute pipeline can read and write a
*flat* imported DMA-BUF. A VPP output is not flat: it is NV12 with a
pitch-aligned stride, a luma plane followed by a chroma plane, and 2x
horizontal/vertical subsampling. A stride or chroma-offset error there
produces a plausible but wrong scaled frame, so the load-bearing filter kernel
is gated separately with a byte-exact CPU oracle before any VA VPP entry point
is wired.

`test/vulkan-vpp/nv12_scale_gate.sh` runs `nv12_scale_probe.c` on the target.
It allocates a 64x32 NV12 source and a 128x64 NV12 destination (2x upscale)
as two DMA-BUFs laid out exactly as `src/iris/alloc.cc:stable_layout` and
`src/util/prime.cc:plane_layout` produce (128-byte stride, 32-row luma,
16-row chroma behind luma, total rounded to 4 KiB), fills every source byte
with a distinct address-dependent pattern, and imports both as Vulkan external
memory. `nv12_scale.comp` takes stride, luma/chroma offsets and visible
geometry as push constants — the same dispatch a VA video processor would use,
not a test with a baked-in size — and each invocation owns disjoint 32-bit
destination words so the result is race-free. After `vkWaitForFences` the CPU
mmap checks **every** destination byte (luma and both chroma channels) against
a 2x nearest-neighbour oracle and verifies the source is unchanged. A wrong
offset, stride or subsample lands on a different pattern byte and fails.
Both DMA-BUF origins are gated, and `spirv-val` validates the pinned module.

Recorded run `run-20261112T220313Z` (`test/remote/gate.sh -- sh
test/vulkan-vpp/nv12_scale_gate.sh`), guard `complete`, exit 0, child stdout
holds both PASS lines:

| origin | device | result |
| --- | --- | --- |
| system DMA heap | Turnip Adreno 740, Vulkan 1.4.354 | `PASS nv12_2x=exact pixels=12288/12288 src=unchanged` |
| MSM GEM PRIME fd | Turnip Adreno 740, Vulkan 1.4.354 | `PASS nv12_2x=exact pixels=12288/12288 src=unchanged` |

Identity (installed package, no build-tree override): source commit
`1f29b4b`, kernel `7.2.6-sm8550-g42f3b40-iris1`, boot
`3356e00e-5a48-4718-8dc1-868fa5cc559d`, module `b90bd823`, installed driver
`323c07cbd42f5a152cfca4278a78d64d726534a85f95f0d0db22379e4317a37d`, decoder
and encoder nodes resolved by `iris_driver` QUERYCAP name (`/dev/video17` /
`/dev/video18`), render node `/dev/dri/renderD128`. Guard battery 92%
discharging at start. SPIR-V sha256
`be8b516eabf8df38dca8f9c7858a7eed319ad3a36493a41efe4225e39374052f`; `copy.spv`
and `nv12_scale.spv` are both pinned by `SHA256SUMS` and regenerated by
`make-spv.sh` on a host. The flat prereq and this scale gate together prove
the compute primitive; neither advertises `VAProfileNone` /
`VAEntrypointVideoProc`, no VPP vtable hooks, context, buffer types 41/42 or
filters are wired in `src/`, and `data/iris-codec-capabilities.json` is
unchanged.

## VA video processor wired end to end (2026-11-13)

The two entries above stop at "neither advertises `VAProfileNone` /
`VAEntrypointVideoProc`". This one wires them: the three `vtable_vpp` queries,
a context that opens no node and owns no session, buffer types 41 and 42, and
an `endPicture` that resolves the source and output surface DMA-BUFs and
dispatches the pinned NV12 2x nearest kernel.

The backend is one operation. Crop, rotation, blending, filters, colour
conversion, a ratio other than 2x and a second pipeline buffer in one picture
all fail the picture with a VA status instead of being dropped. The output
surface reports pixels only after the dispatch fence signals, so `vaGetImage`
and `hwdownload` cannot read a half-written frame. The entrypoint is
advertised only when the build found Vulkan and the client sets
`V4L2_VA_VPP`; without the opt-in `vaCreateConfig` answers
`VA_STATUS_ERROR_UNSUPPORTED_PROFILE` (12) and the client is refused.

`test/vulkan-vpp/scale_vaapi_gate.sh` is the gate. One 128x128 NV12 frame is
uploaded, scaled to 256x256 by FFmpeg's `scale_vaapi`, downloaded with
`hwdownload`, and compared with swscale `flags=neighbor` on the same input;
the same client then runs without the opt-in and must fail. swscale's nearest
2x map is the map the kernel runs, so this is a byte-for-byte oracle, not a
tolerance.

Recorded run `run-20261113T032443Z` (`test/remote/gate.sh -- sh
test/vulkan-vpp/scale_vaapi_gate.sh`), guard `complete`, exit 0:

| stage | sha256 of the raw NV12 |
| --- | --- |
| swscale `flags=neighbor` 128x128 -> 256x256 | `6070d81c88964f4eee42a53ccbfeb9abfdaadf99d4dd992ed8a387921c6d37f5` |
| `hwupload,scale_vaapi,hwdownload` | `6070d81c88964f4eee42a53ccbfeb9abfdaadf99d4dd992ed8a387921c6d37f5` |

The trace for that picture shows the pipeline, not only a success status:

```
va create_context id=8 profile=-1 256x256 entrypoint=videoproc
va begin_picture ctx=8 surface=5 entrypoint=videoproc
va end_picture ctx=8 surface=5 entrypoint=videoproc src=1 128x128->256x256
va destroy_context id=8 entrypoint=videoproc
```

Without `V4L2_VA_VPP` the same run reported `createConfig: VPP not
advertised`, `va error where=createConfig status=12`, and exited 1.

Identity: source commit `40c7daa`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, boot
`3356e00e-5a48-4718-8dc1-868fa5cc559d`, module `b90bd823`, decoder and encoder
nodes resolved by `iris_driver` QUERYCAP name (`/dev/video17` /
`/dev/video18`), render node `/dev/dri/renderD128`. One VPP picture; no decode
frames and no drops (the context has no session). The runtime used a
**build-tree override** (`LIBVA_DRIVERS_PATH=build/src`, driver
`36f761b848bc10fb1b29a4be0f8a27403ee1eee244c9c0adbbbe771b3959dbb0`); the
installed `/usr/lib64/dri/v4l2_drv_video.so` (`323c07cb`) was not loaded and
does not yet carry the entrypoint. Guard battery 78% discharging at start,
peak 41 °C. The embedded shader header is regenerated from the pinned
`nv12_scale.spv` and compared by `test/run-static-checks.sh`, and Vulkan is an
optional meson dependency, so a build without it compiles and does not
advertise the entrypoint.

## Any ratio, and the gate run from the installed package (2026-11-13)

The processor described above ran one ratio, 2x. This changes the kernel's
sample position to the one libswscale uses for `flags=neighbor`, so
`scale_vaapi=w=W:h=H` and `scale=WxH:flags=neighbor` produce the same bytes at
any geometry:

```
xInc          = ((src << 16) + (dst >> 1)) / dst
src_index(i)  = (i * xInc + (xInc >> 1)) >> 16
```

The chroma plane uses the same integers on its own half-size grid. That is
`initFilter`'s `SWS_POINT` branch (filterSize 1) combined with the rounded
`lumXInc`/`chrXInc` of `sws_init_context`, not an approximation of them. The
model was checked against the host `scale` filter on fifteen sizes before any
shader was written, luma and both chroma channels: exact for 1:1, 0.5x, 1.5x,
3.125x, an odd 62x34 source and 1280x720, including the sizes where the fixed
point lands on an integer boundary.

The kernel takes both geometries as push constants and writes one 32-bit
destination word per invocation (four luma samples of one row, or two chroma
pairs), so a per-pixel oracle still means something at a 3:1 ratio. The
VideoProc config also advertises its own 16x16 surface floor instead of the
decoder node's 96x96; before that change FFmpeg refused `scale_vaapi=w=32:h=32`
with `constraints: width 96-8192`.

### Kernel gate, both DMA-BUF origins

`test/vulkan-vpp/nv12_scale_gate.sh`, run `run-20261113T044033Z` (guard
`complete`, exit 0), dispatches the pinned `nv12_scale.spv`
(`6a0169c1d007da20fdbf75e45c9b9e2c826dc325798b7dfa9c26f3360d87a691`) over one
MSM GEM PRIME fd and one system DMA-heap buffer for each ratio. Every case
reports `pixels_exact` with the source unchanged:

| source -> destination | ratio |
| --- | --- |
| 64x32 -> 128x64 | 2x |
| 64x32 -> 96x48 | 1.5x |
| 64x32 -> 32x16 | 0.5x |
| 64x32 -> 42x22 | 1.375x |
| 64x32 -> 200x100 | 3.125x |
| 62x34 -> 62x34 | 1:1 |
| 128x72 -> 86x48 | 0.67x |

### VA gate, build tree and installed package

`test/vulkan-vpp/scale_vaapi_gate.sh` runs eight geometries through
`hwupload,scale_vaapi,hwdownload` and compares each frame with swscale. The
same script run twice, once against `build/src` (`run-20261113T043948Z`) and
once with `IRIS_VPP_INSTALLED=1`, where libva resolves `/usr/lib64/dri` and no
`LIBVA_DRIVERS_PATH` is exported (`run-20261113T044224Z`). Both are guard
`complete`, exit 0, eight of eight:

| geometry | sha256, swscale and hardware |
| --- | --- |
| 64x64 -> 128x128 | `1076510ee62f55127e53ec796263d7a525eb5c3244b310aa0de71e9bb69f9a20` |
| 64x64 -> 96x96 | `980d1df6363ae8058ee4e2a7a71baed9c2e27fe5d7a609c89d2f0b686eabd761` |
| 64x64 -> 32x32 | `fad2794ecfcb3bb8a734ac19dc99b3257010ccebbba5ae27ffb3a836369687b5` |
| 128x128 -> 16x16 | `359d6a9ee42b7d64f7caf51bae4bced7923beeeeb889f40e3a1a9a7f9a72c6a7` |
| 128x128 -> 200x150 | `c4cd41c4fbddbb34aa54d8798fd7d3dbe54c6a5775bee02977f481032e2c26ac` |
| 62x34 -> 46x26 | `6f24f58a9b8184bc57af13d4e17d8124c8c02427a1b8fc2b64427ba24bff1210` |
| 1280x720 -> 854x480 | `7bf14757595b2da36ea95576232e73fd2e749299c7ae9faf725cab6f638647c7` |
| 1280x720 -> 1920x1080 | `b75a0b8ca08de7bb908adb88bc25e7ef77dae7d458fd4d00b0aefa314347b298` |

The installed-package run records `driver_source=installed-package`,
`driver_path=/usr/lib64/dri`, `driver_sha256=f6095e7950b89097040dc77d7f6337499f973fca8eb2387c3dd84de26a232f8b`,
and the same eight hashes as the build-tree run. Both runs also check that the
same client is refused without `V4L2_VA_VPP`.

### The installed package, re-qualified

`scripts/install-system.sh build` installed that artifact on 2026-11-13 and
recorded `source_commit=33fafdb6b8df902b3b21a95a83036f65fa737e1a`,
`previous_driver_backup=/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`
(the former `323c07cb` build). The decode and encode paths were re-run on it,
because `src/` changed:

| gate | run | result |
| --- | --- | --- |
| `iris-matrix.sh` | `run-20261113T044321Z` | 48/48 exact for H.264, HEVC, VP9, AV1; `drops=0`, AV1 `noshow=23` |
| `iris-real-clip-gate.sh` | `run-20261113T044426Z` | AV1 120/120 exact, `import=120 copy=0`; DRC 80 frames, `import=42 copy=42` |
| `iris-encode-quality.sh` | `run-20261113T044513Z` | H.264 and HEVC 1080p 120 frames; IRQ +237 / +249; SSIM mean 0.9986 / 0.9968 |
| `iris-browser-acceptance.sh` | `run-20261113T044626Z` | Chrome 156 `VaapiVideoDecoder`, `platform=True`, 478 frames, 0 dropped, driver `installed` |
| `iris-av1-browser-matrix.sh local-only` | `run-20261113T045207Z` | Firefox 484 decoded / 7 dropped with `hw_log=True`; Chrome 476 decoded / 0 dropped, `VaapiVideoDecoder`, `platform=True` |

Identity for the whole set: kernel `7.2.6-sm8550-g42f3b40-iris1`, module
`b90bd823`, boot `972c13d1-1848-4bb3-8c9b-ce60c74376cb` (the tablet rebooted
before this sequence; every guard recorded the new boot ID), decoder and
encoder nodes resolved by `iris_driver` QUERYCAP name (`/dev/video17` /
`/dev/video18`), render node `/dev/dri/renderD128`. Guard battery 99%
discharging, peak 37.4 °C, no reset and no thermal breach.

### Two findings this gate produced

Equal input and output geometry never reaches the driver: FFmpeg's
`scale_vaapi` sets `vpp_ctx->passthrough` when the size, format and colour
properties are unchanged, so the frame is forwarded untouched. The VA gate
therefore does not carry a 1:1 case; the kernel gate does.

A client that uploads and downloads without any filter fails:
`format=nv12,hwupload,hwdownload` exits at `vaGetImage` with `surface has no
decoded pixels`, because a surface written through a derived image never sets
`has_pixels()`. It is not new: the previous installed driver (`323c07cb`) fails
the same command the same way, and `vaPutImage` is `VA_STATUS_ERROR_UNIMPLEMENTED`.
No client in this project's gates reads back an uploaded surface, so the
behaviour is recorded rather than changed here; relaxing `has_pixels()` for
client-written surfaces is the candidate fix and needs its own gate.

## Client pixels: `vaPutImage`, and a read-back that used to refuse (2026-11-13)

`format=nv12,hwupload,hwdownload` with no filter between them exited at
`vaGetImage` with `surface has no decoded pixels`. The upload worked: FFmpeg
derives the surface, maps that view and writes the frame into it. What was
missing is the other half — `Surface::has_pixels()` counted decode completions
and VideoProc pictures only, so nothing told `vaGetImage` the pixels were
there. `vaPutImage`, the second upload route, was
`VA_STATUS_ERROR_UNIMPLEMENTED`.

`vaPutImage` now copies a client image into the surface's memory: whole frame
or an even region, image and surface formats must match, and scaling is
refused with `VA_STATUS_ERROR_UNIMPLEMENTED`. A derived image handed out
before the surface held a decoded frame — and every `vaPutImage` — marks the
surface as client-written, which is the third term in `has_pixels()`.

### The gate

`test/iris-va-image-probe.cc` drives the public API: a full-frame
`vaPutImage` → `vaGetImage` round trip, a 32x32 region at (16,16) with the rest
of the surface left alone, a derived-image write with no `vaPutImage` at all,
and three refusals (scale, odd 4:2:0 offset, region past the surface).
`test/iris-va-image-gate.sh` runs the probe, then four `hwupload,hwdownload`
round trips through FFmpeg: NV12 64x64, NV12 128x72 (odd chroma row count),
P010 64x64 and NV12 32x32.

| check | result |
| --- | --- |
| probe | 14/14 PASS |
| nv12 64x64 round trip | exit 0, sha256 `e3b611e4b7daa213bd2e1e1a08ba60afb3f2479f3c43793f1914298610f7e400` |
| nv12 128x72 round trip | exit 0, sha256 `cb1b634377d6426ca6f5beb95b4b42c38add416c25cebce49413934e8bb908c2` |
| p010 64x64 round trip | exit 0, sha256 `ee46c1eebcc8fa3076a0a330528c3eaee86ae689f9f12f312d1f024990868f93` |
| nv12 32x32 round trip | exit 0, sha256 `a5ddec719d3c971229a22cb59f5d3f18581118a836b5a0011ec5929fb762c98d` |

Run `run-20261113T051637Z`, guard `complete`, exit 0, source commit `c6acb33`,
driver `f19aeea769312e1aa8a1f19b6fe7eb2b9c876c02996357fb192e3604d9636aae`
from `build/src`. Each round-trip digest is the digest of the input frame:
the gate compares the bytes, and the table names what came back.

### The same gate on the previous driver

The gate is not self-confirming. Run against the `323c07cb` build kept as the
rollback copy, with the same probe binary and its own `LIBVA_DRIVERS_PATH`, it
fails ten of fourteen probe checks and prints the original symptom three times
(`libva error: getImage: surface has no decoded pixels`); the ffmpeg half never
runs. Run `run-20261113T051731Z`, guard `complete`, exit 1.

Identity for the pair: kernel `7.2.6-sm8550-g42f3b40-iris1`, module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`, boot
`972c13d1-1848-4bb3-8c9b-ce60c74376cb`, decoder and encoder nodes resolved by
`iris_driver` QUERYCAP name (`/dev/video17`, `/dev/video18`), battery 93 %
discharging, thermal maximum 43.0 °C, no reset and no thermal breach.

## Crop through the video processor (2026-11-13)

A VA client crops by naming a `surface_region` in the pipeline buffer, and the
driver refused every region but the whole surface. The kernel now samples the
region's rectangle: the 16.16 step, the row index and the chroma index are all
computed from `region.width/height` with `region.x/y` as the origin, and an
origin or size that is not even is refused with
`VA_STATUS_ERROR_INVALID_PARAMETER` instead of being rounded — a 4:2:0 chroma
sample covers a 2x2 luma block.

### Two gates

`test/vulkan-vpp/nv12_scale_gate.sh` gained four crop cases over both DMA-BUF
origins (a 2:1 downscale of the whole surface, a fractional crop, a crop that
scales down to a quarter, and an 86x48 result from a 96x40 region):

```
PASS nv12-scale ratios=7 crops=4 origins=2 pixels_exact
spv=70609a51bf154c3580718df219c09e42b777538abb583d61efe3d0f1292a42c6
```

Run `run-20261113T052342Z`, guard `complete`, exit 0, driver `c30e2bbb`.

At the VA level FFmpeg cannot ask for a region: 8.1.3 has no `crop_vaapi`
filter (`ffmpeg -filters` lists `scale_vaapi`, `overlay_vaapi`,
`transpose_vaapi`, `tonemap_vaapi`, `pad_vaapi`, `drawbox_vaapi`, none of
which submits a `surface_region`). `test/iris-va-vpp-probe.cc` is therefore
the client for `test/vulkan-vpp/crop_vaapi_gate.sh`: it uploads an NV12 frame
through a derived image, submits one VideoProc picture with the region, and
downloads the result with `vaGetImage`. The oracle is
`crop=W:H:X:Y,scale=DWxDH:flags=neighbor` on the same frame.

| source | crop | output | sha256, oracle and hardware |
| --- | --- | --- | --- |
| 128x72 | 96x40+16+16 | 86x48 | `8808bfec693fcaf27ff6176ee73fa820d3f534dcd17e856fa23075262fe2737c` |
| 64x32 | 32x16+16+8 | 128x64 | `d77022a6f8e82282ff10fb1a7cc43909f8c7eaf8e80af6bc5394123e41746497` |
| 128x128 | 64x64+32+32 | 32x32 | `8ab51afd4d6998f70bd3be3269ce454460de7dc463d13f369e01bc88a41da1a5` |

The fourth case asks for a 31x16 region and requires `vaEndPicture` to fail;
it did. Run `run-20261113T052425Z`, guard `complete`, exit 0, driver
`c30e2bbb` from `build/src`.

Identity: source commit `a575c20`, kernel `7.2.6-sm8550-g42f3b40-iris1`, module
`b90bd823…`, boot `972c13d1…`, decoder and encoder nodes resolved by
`iris_driver` name (`/dev/video17`, `/dev/video18`), battery 93 % → 92 %
discharging, thermal maximum 53.2 °C on the kernel run and 48.5 °C on the VA
run, no reset and no thermal breach.

## P010 scaling in the video processor (2026-11-13)

The processor handled NV12 only, so a 10-bit stream could not be scaled on
the GPU at all. `p010_scale.comp` runs the same point map over 16-bit samples:
two samples per 32-bit word in luma, one u,v pair per word in chroma, a pair
being four bytes of the interleaved plane. The driver picks the kernel from
the surface's sample width (`ScaleJob::bytes`), and the VideoProc config and
its surfaces advertise `VA_RT_FORMAT_YUV420_10` once the decoder node reports
10-bit capture formats.

Bit depth is not converted. Both ends must be NV12 or both P010, and a mixed
picture is refused with `VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT`. swscale's own
10-to-8 conversion is neither a shift nor a round: measured on the host with
32 code values, 10-bit 33 comes out as 9 and 10-bit 165 as 42, where
`(c + 2) >> 2` gives 8 and 41 — the residue of a position-dependent dither.
There is therefore no byte-exact oracle for a 10-to-8 conversion, while
same-depth scaling has one, and that is what the gate uses.

### The gate

`test/vulkan-vpp/scale_vaapi_gate.sh` now runs its comparison in 10 bits as
well: `format=p010,hwupload,scale_vaapi,hwdownload` against
`scale=WxH:flags=neighbor` on the same P010 frame.

| case | sha256, swscale and hardware |
| --- | --- |
| 64x64 -> 128x128 | `c2e36f5dc6930906595a02f965d9e6b2b785b5139efd163b7876c25035897a25` |
| 128x128 -> 96x96 | `89107492493054db4df70abfd1d09b59ceac77eda781c92c3a869fc63c70e65f` |
| 1280x720 -> 854x480 | `235187959c7b2df3b361577e17abbe8c48fcdadd61509226e37fd1cb4b6c44f3` |

The eight NV12 cases keep their own hashes from the previous run, and both
negatives hold: the same client is refused without `V4L2_VA_VPP`, and a P010
source asked to produce NV12 output is refused rather than converted.

Run `run-20261113T053245Z`, guard `complete`, exit 0, source commit `1febae7`,
driver `7e5b3713` from `build/src`, `spv` `p010_scale.spv`
`8ab4699c81013e23e1ad50af6f30fb67899d9c3f7550e71ffaa704d520ca9f24`.

Two defects this work produced, both caught by the gate rather than by review:
`beginPicture` refused a P010 target before `run_vpp` saw the picture, and the
chroma branch packed two pairs into a word that holds one, which put the first
mismatched byte at the start of the chroma plane and left luma exact.

Identity: kernel `7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`, boot
`972c13d1…`, decoder and encoder nodes resolved by `iris_driver` name
(`/dev/video17`, `/dev/video18`), battery 90 % discharging, thermal maximum
45.3 °C, no reset and no thermal breach.

## Rotation through the video processor, and the P010 crop origin (2026-11-13)

VA orders a picture as a clockwise rotation, then a scale. Both kernels now
follow that order: the point map runs on the rotated size and each destination
point is walked back through the rotation to the source, so a quarter turn and
a scale are one dispatch. `queryVideoProcPipelineCaps` advertises rotation
0/90/180/270 and still no mirroring; a client that names a mirror is refused,
not given unmirrored pixels.

### The map, checked on the host first

`test/vulkan-vpp/rotation_model_check.py` runs the same coordinates as
`scale_probe.c` against FFmpeg's `crop,transpose,scale=flags=neighbor` chain (a
half-turn is `hflip,vflip`, since ffmpeg 8 dropped the named one from the
software filter) on non-square sources, a crop and a downscale. This is the
check that found the first version of the map reading the rotated width for
the y coordinate and the rotated height for x: identical on a square frame,
out of the crop on 64x32. It is part of `run-static-checks.sh`, so the mistake
now fails in CI instead of on the target.

### The three gates

| gate | cases | result |
| --- | --- | --- |
| `nv12_scale_gate.sh` | 18 geometries x 2 DMA-BUF origins | every destination pixel matches the oracle; `spv` `8aa340cd…` |
| `p010_scale_gate.sh` | 9 geometries x 2 origins | same, in 16-bit samples; `spv` `9c354b3d…` |
| `transpose_vaapi_gate.sh` | `clock`, `cclock`, `reversal` x NV12, P010 | 9/9 byte-identical to the software filter |

The kernel gates now include a quarter turn onto a transposed output, a
quarter turn onto an unrelated size (rotate then scale), and a quarter turn
inside a crop. `transpose_vaapi_gate.sh` also requires two refusals: the
`clock_flip` direction, which needs a vertical mirror the driver does not
advertise, and the same pipeline without `V4L2_VA_VPP`. Every transpose case
reports `videoproc=4` in its trace.

Runs `run-20261113T054800Z`, `run-20261113T054852Z` and `run-20261113T054935Z`
(guard `complete`, exit 0 for each) at source commit `a1d1fae`, driver
`7dc6f386` from `build/src`. Identity: kernel `7.2.6-sm8550-g42f3b40-iris1`,
module `b90bd823…`, boot `972c13d1…`, decoder `/dev/video17`, encoder
`/dev/video18`, battery 88-89 % discharging, thermal maximum 39.8 °C.

### The P010 crop origin this exposed

A P010 chroma pair spans two luma columns and four bytes, so a surface region
starting at x begins its chroma at byte `2 * x` of the row; the kernel used
`x`. Every 10-bit crop case until now started at the origin, where the two
agree. The gate's crop cases now start at `+16+16`, and both origins pass.

### The negative control, the scale gate and the installed package

The pre-fix `p010_scale.spv` (`8ab4699c…`) run against the crop case that
starts at `+16+16` fails on every chroma pair (1920 samples: `96/2 * 40/2 * 2`)
and on no luma sample, and the fixed kernel passes the same case in the same
run (`run-20261113T082219Z`, guard `complete`, exit 0). That is what makes the
crop case evidence rather than a coincidence.

`scale_vaapi_gate.sh` was re-run after the rotation change: 8 NV12 ratios and
3 P010 ratios, all byte-identical to swscale (`run-20261113T082306Z`). The
rotation work did not move the scale path.

The same two VA-level gates then ran from the installed package: artifacts
`7dc6f386` in `/usr/lib64/dri` (previous build kept as the rollback copy),
`IRIS_VPP_INSTALLED=1`, no `LIBVA_DRIVERS_PATH`. Transpose 9/9 and scale
8+3/11 byte-identical (`run-20261113T082519Z`, `run-20261113T082604Z`), both
reporting `driver_source=installed-package` and the same `driver_sha256`.
Battery 59 % discharging at the last run.

## HDR10 to SDR tone mapping through the video processor (2026-11-13)

`VAProcFilterHighDynamicRangeToneMapping` with HDR10 metadata now runs
`tonemap.spv` over a P010 surface and writes SDR NV12: PQ EOTF, the BT.2390
EETF toward 100 cd/m² as one gain on the largest channel, the linear BT.2020 to
BT.709 matrix, the BT.709 OETF. The picture is one whole surface at its own
size. A crop, a scale, a rotation, an NV12 source, HDR to HDR and a second
filter are refused with a VA status; the client's colour standards may differ
only as BT.2020 in and BT.709 out, and may be left unnamed.

### The gate

`tonemap_gate.sh` runs `iris-va-vpp-probe tonemap`, which builds the
`VAProcFilterParameterBufferType` buffer itself. FFmpeg cannot be the client:
`tonemap_vaapi` refuses a raw P010 frame because it demands mastering-display
side data a rawvideo input cannot carry. There is no byte-exact reference for
a tone curve, so the probe compares every output byte with its own
implementation of the pipeline `tonemap.comp` documents, within 2 codes, and
checks three properties separately so a model and a kernel that share a
mistake still fail: PQ black on 16/128/128, the mastering peak on 235, and a
monotone ramp. One frame runs at 1000 and at 4000 cd/m² and the two outputs
must differ.

| case | geometry | peak | result |
| --- | --- | --- | --- |
| `sdrwhite` | 128x72 | 1000 | `max_diff=1` |
| `mastering4000` | 128x72 | 4000 | `max_diff=1` |
| `ragged` | 318x178 (width not a multiple of four) | 1000 | `max_diff=1` |

Refused in the same run: a tone map with an NV12 source, one with the filter
named and no parameter buffer, and one with `rotation_state = 90`. Without
`V4L2_VA_VPP` the entrypoint does not exist and the client fails at
`vaCreateConfig`; the gate requires that.

Runs `run-20261113T085605Z` (build tree) and `run-20261113T085313Z` (installed
package), guard `complete`, exit 0, `source_dirty=0` at commit `ccc94b1`,
driver `dbd0630e` both ways. Identity: kernel `7.2.6-sm8550-g42f3b40-iris1`,
module `b90bd823…`, boot `4b73926c…`, decoder `Iris Decoder` on `/dev/video0`,
encoder `Iris Encoder` on `/dev/video1`, battery 57 % discharging, thermal
maximum 37.8 °C for the build-tree run and 42.2 °C for the installed one.
The installed artifact keeps `7dc6f386` as the rollback copy at
`/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`.

The three existing VA-level gates ran again against the same driver, both
from `build/src` and from `/usr/lib64/dri`, because the defect below was in
the shared Vulkan backend: `transpose_vaapi_gate.sh` 9/9 and
`scale_vaapi_gate.sh` 8+3/11 byte-identical (`run-20261113T085032Z`,
`run-20261113T085152Z`, and `run-20261113T085358Z`,
`run-20261113T085514Z` installed), `crop_vaapi_gate.sh` 3/3 exact
(`run-20261113T085114Z`, `run-20261113T085437Z`).

### The first run failed, and it was the driver

The gate's first run stopped every case at
`VK_ERROR_INVALID_EXTERNAL_HANDLE`. The driver turned out to ask
`vkEnumeratePhysicalDevices` how many devices there are and then hand it the
address of a single `VkPhysicalDevice`; the target reports two, so the second
handle landed on the next word of the caller's frame, which in this path is
the job's own buffer descriptor. The fds arrived at `vkAllocateMemory` as 0
and as a pointer, which is what the error says.

The scale path had the same overflow and never showed it: the word after its
physical-device slot was rewritten before it was read, so the corruption only
became visible when the tone mapper put its own locals there. Fixed in
`a119930`, which sizes the array by the count the way `compute_family` already
did; the selected device is the same one. The evidence is the debugging cycle
itself: the same job printed `dst_fd=5` at entry and `dst_fd=0` after the
enumeration in the same frame.

### The second failure was the oracle

With the kernel running, the gate failed on `max_diff=11` at one chroma column
of each row. The oracle took the top-left pixel of each 2x2 block and called it
the pair; the kernel averages the four. On a smooth picture those agree to a
code, which is why the luma plane passed. Fixed in `ccc94b1`; every byte of
all three cases is then within one code, which is the float32 kernel against a
double-precision model. The oracle now writes its frame beside the kernel's
(`<out>.expected`), so a disagreement names a plane and a column instead of a
count.

## Global-alpha compositing through the video processor (2026-11-13)

`overlay_vaapi` submits one picture with two pipeline buffers: the main surface
over the whole output, then an overlay surface into a rectangle of it with a
`VABlendState` naming `VA_BLEND_GLOBAL_ALPHA`. The processor now runs
`overlay.spv` over three DMA-BUFs for that shape, and
`queryVideoProcPipelineCaps` answers `blend_flags = VA_BLEND_GLOBAL_ALPHA` and
nothing else — premultiplied alpha and luma keying need an RGB or alpha-plane
kernel this backend does not have, so they are refused rather than
approximated.

The mix runs on the coded Y'CbCr values. For a *global* alpha that is the same
picture: a limited-range code is affine in the normalised value
(`16 + 219 v`, `128 + 224 c`), so `alpha*o + (1-alpha)*m` on codes equals the
mix on the normalised values. The rectangle must be even on both axes so no
chroma cell is half covered.

The main pass is an offset copy, not a copy of the whole surface. A decoder
allocates its surfaces wider than the picture — the client below decodes 3048
columns into a 3056-column surface with a 3072-byte stride — and FFmpeg names
that whole surface as `surface_region` regardless, so the main pass copies the
client's rectangle into an output of the same size and the overlay is scaled
into a rectangle of that output.

### The gate

`overlay_gate.sh` runs `iris-va-vpp-probe overlay`, which submits the two
pipeline buffers in FFmpeg's shape (`params[0]` with the whole surface region
and a full-output `output_region`, `params[1]` copied from it with the overlay
surface, no surface region, its own `output_region` and the blend state). There
is an exact reference for a global-alpha mix, so every output byte is compared
with the probe's double-precision model of what `overlay.comp` documents.

| case | main | overlay | rect | alpha | result |
| --- | --- | --- | --- | --- | --- |
| `half` | 128x72 | 128x72 | 128x72+0,0 | 0.50 | `max_diff=0` |
| `opaque` | 160x96 | 64x48 | 64x48+16,16 | 1.00 | `max_diff=0` |
| `scale` | 128x72 | 64x48 | 32x24+34,12 | 0.60 | `max_diff=0` |
| `p010` | 128x72 P010 | 32x24 P010 | 32x24+64,48 | 0.50 | `max_diff=0` |
| `ragged` | 318x178 | 64x48 | 64x48+100,50 | 0.25 | `max_diff=0` |
| `padded` | 128x72 in a 144-column surface | 64x48 | 64x48+16,16 | 0.35 | `max_diff=0` |

Three checks run beside that comparison. At alpha 0 over the whole surface the
output must be the main frame byte for byte and at alpha 1 the overlay frame,
which a kernel that read the wrong plane or dropped the alpha cannot produce;
the half-alpha case must equal neither input. Eight shapes must be refused:
premultiplied alpha, luma keying, a third pass, a blend before the pass it
blends onto, a rectangle that leaves the output, an odd rectangle, a crop of
the main surface, and an alpha of 1.5. Without `V4L2_VA_VPP` the entrypoint
does not exist and the same client fails at `vaCreateConfig`.

Runs `run-20261113T092702Z` (build tree) and `run-20261113T093308Z`
(installed package), guard `complete`, exit 0, `source_dirty=0` at commit
`b971f26`, six cases `max_diff=0` both ways. The artifact installed by
`scripts/install-system.sh build` is the same bytes the build tree ran
(`fe3a1822`), with `c6dea809` kept as the rollback copy at
`<lab-root>/rollback/v4l2_drv_video.so.c6dea809`. Identity: kernel
`7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`, boot `4b73926c…`, decoder
`Iris Decoder` on `/dev/video0`, encoder `Iris Encoder` on `/dev/video1`.

The other six VPP gates ran again at the same driver, because `b971f26`
touches the shared `run_vpp` path: `tonemap_gate.sh`,
`scale_vaapi_gate.sh`, `crop_vaapi_gate.sh`, `transpose_vaapi_gate.sh`,
`p010_scale_gate.sh` and `nv12_scale_gate.sh` all exit 0 with guard
`complete` (`run-20261113T092740Z`, `run-20261113T092817Z`,
`run-20261113T092854Z`, `run-20261113T092931Z`, `run-20261113T093009Z`,
`run-20261113T093046Z`).

The artifact above is what `/usr/lib64/dri` now serves, so the decode, encode and
browser paths were re-run on it with `LIBVA_DRIVERS_PATH` unset:

| gate | run | result |
| --- | --- | --- |
| `iris-matrix.sh` | `run-20261113T093608Z` | H.264, HEVC, VP9, AV1: 48/48 frames exact, `drops=0` |
| `iris-real-clip-gate.sh` | `run-20261113T094012Z` | AV1 120/120 `import=120 copy=0`; DRC 80 frames `import=42 copy=42` |
| `iris-encode-quality.sh` | `run-20261113T093721Z` | 120 frames each, IRQ +238 H.264 / +239 HEVC, SSIM 0.9986 / 0.9968 |
| `iris-browser-acceptance.sh` | `run-20261113T094050Z` | Chrome 156 `VaapiVideoDecoder`, `platform=True`, 477 frames, 0 dropped |
| `iris-av1-browser-matrix.sh local-only` | `run-20261113T094128Z` | Firefox 485 decoded / 9 dropped `hw_log=True`; Chrome 477 / 0 |

All five at commit `2817112`, `source_dirty=0`, guard `complete`, exit 0; battery
54 % discharging through the set, thermal peak 46.1 °C, boot `4b73926c…`
unchanged from the first run to the last. Three earlier attempts in this set
(`run-20261113T093644Z`, `run-20261113T093758Z`, `run-20261113T093835Z`) are
recorded `interrupted` with empty child output because the gates were invoked
without `IRIS_REAL_CLIP_DIR` and the browser clip arguments and exited 2, 1 and
2 before doing any work; they are a record of the mis-invocation, not of the
driver.

### The real client

The probe mirrors what the code says a client sends; FFmpeg says what it
actually sends. A 3048x2032 H.264 screen recording decoded through `vaapi` and
composited with `testsrc2` at half alpha:

```sh
ffmpeg -init_hw_device vaapi=va:/dev/dri/renderD128 -filter_hw_device va \
  -hwaccel vaapi -hwaccel_device va -hwaccel_output_format vaapi \
  -i clip.mp4 -f lavfi -i testsrc2=size=320x200:rate=30 -filter_complex \
  '[1:v]format=nv12,hwupload[ov];[0:v][ov]overlay_vaapi=x=100:y=100:w=160:h=100:alpha=0.5,hwdownload,format=nv12[out]' \
  -map '[out]' -f rawvideo out.yuv
```

The trace reports `overlay=44 160x100+100,100 alpha=0.50` on both frames
(`src=2 3056x2032->3048x2032` then `src=25 3072x2048->3048x2032`) and the
session closes at `submitted=14 completed=14 errors=0 drops=0`.

Against the same decode without the filter: 0 of 6,177,536 luma pixels outside
the rectangle changed, 15,968 of 16,000 inside it did, and all 4,000 chroma
cells inside it changed. With `alpha=0.0` the output is byte-identical to the
plain decode (`cmp`).

### Three defects only the real client found

The probe passed its first runs while every FFmpeg attempt failed, and each
failure was a client contract the probe had modelled from the code:

1. `ff_vaapi_vpp_init_params` always fills `surface_region`, even when it is
   the whole frame, and the driver refused any region on a blended main pass,
   so every `overlay_vaapi` picture was refused. Fixed in `66b65c6`.
2. The main surface is wider than its picture, so a whole-surface copy wrote
   3056 columns where the output had 3048 and FFmpeg's `surface_region` could
   not be honoured at all. The main pass became an offset copy of the client's
   rectangle (`2a3ff37`), and the probe grew `--pad` to allocate the same kind
   of surface.
3. FFmpeg issues the filter picture straight after the decode and relies on one
   command stream to order them. Here the decode is a V4L2 job and the kernel a
   Vulkan one, so the processor read a surface that had not been decoded yet
   and composited black. The trace shows the picture at `t=1248` and the
   source's `capture token=1` at `t=1265`. `vaEndPicture` now waits for each
   source's pending decode outside the driver mutex (`661dbb5`), and a source
   with neither a frame nor a stable buffer passes the pixel check as pending
   (`b971f26`).

## The tone mapper runs before the scale, the mirror and the blend (2026-11-13)

Until this build a tone-mapped picture was the tone mapper alone: a crop, a
scale, a quarter turn, a mirror or a second buffer was refused with
`VA_STATUS_ERROR_UNSUPPORTED_FILTER`. The conversion does not move pixels, so
those are separate passes rather than a conflict. The picture now runs the
tone mapper into a scratch NV12 surface at the source's own size and hands
that surface to the scale pass or to the blend pass, in the order the VA
pipeline buffer documents (rotation, mirroring, scaling).

Three smaller corrections ride along in the same processor: the pipeline caps
answer a query that names the tone mapper with P010 in and NV12 out
(BT.2020/BT.709) instead of the unfiltered format list; the geometry floor
drops from 16x16 to 2x2, because the kernel writes one 32-bit word per
invocation inside a 128-byte-aligned row and never needed the decoder's
minimum; and a picture whose output aliases its input through two surfaces
over one DMA-BUF is refused by `st_dev`/`st_ino` rather than by object
identity.

The gate table below ran against the **build tree** (`LIBVA_DRIVERS_PATH`
pointing at `build/src`): source commit `681b21d`, tracked source
`02aec978…`, driver `cc0d022b…`, `source_dirty=0`. The same artifact was then
installed and the whole client surface re-run; that is the second table.
Boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`, decoder node resolved by
`iris_driver` QUERYCAP at `/dev/video0`, encoder at `/dev/video1`.

| Gate | Run | Cases | Result |
| --- | --- | ---: | --- |
| `tonemap_gate.sh` | `run-20261113T101907Z` | 11 + 3 refusals | every frame within 1 code of the documented pipeline, the 2x2 thumbnail exact |
| `overlay_gate.sh` | `run-20261113T101943Z` | 7 + 2 endpoints + 8 refusals | within 1 code, the tone-mapped composite included |
| `nv12_scale_gate.sh` | `run-20261113T102018Z` | 27 x 2 origins | byte-exact, `spv=5425102c…` |
| `p010_scale_gate.sh` | `run-20261113T102054Z` | 12 x 2 origins | byte-exact, `spv=957a0e4f…` |
| `crop_vaapi_gate.sh` | `run-20261113T102129Z` | 3 | hardware and reference hashes identical |
| `scale_vaapi_gate.sh` | `run-20261113T102205Z` | 8 ratios, 3 P010 | exact |
| `transpose_vaapi_gate.sh` | `run-20261113T102240Z` | 19, 7 directions, 2 formats | exact |

The tone-map cases are `sdrwhite`, `mastering4000`, `ragged` (318x178),
`rotate90`, `rotate270`, `halfturn`, `hflip`, `vflip`, `bothflips`,
`crop-scale` (crop 64x48 of 128x72 into 32x24) and `thumbnail` (64x64 into
2x2). `halfturn` and `bothflips` produce the same hash, which is the
geometry: a half turn is a flip on both axes. The refusals are an NV12
source, a filter without its parameters, and an odd crop.

### Mirroring, from the same artifact

`transpose_vaapi` asks for the mirroring bits through
`pipeline_caps.mirror_flags` and refuses any direction the driver does not
advertise, so its 19 cases are the mirroring gate as well: `hflip`, `vflip`,
`clock_flip` and `cclock_flip` match FFmpeg's software `hflip`/`vflip`
chains byte for byte, on NV12 and P010. The kernel gates carry the same
mapping over imported DMA-BUFs (`nv12` cases with `mirror=1..3` on both
origins). The first build-tree run of the transpose gate was
`run-20261113T095809Z` at `d7398bf`; this one supersedes it.

### HEVC encode attributes

FFmpeg's `vaapi_encode_h265.c` asks for `VAConfigAttribEncHEVCFeatures` and
`VAConfigAttribEncHEVCBlockSizes`; the driver answered both with
`VA_ATTRIB_NOT_SUPPORTED` and FFmpeg logged "Driver does not advertise
encoder features, using guessed defaults" on every HEVC encode. The firmware
writes the parameter sets, so the honest answer comes from its own bitstream:
`ffmpeg -bsf:v trace_headers` over a 1080p `hevc_vaapi` stream reports
`log2_min_luma_coding_block_size_minus3=0`, `log2_diff_max_min_luma_coding_
block_size=2`, `log2_min_luma_transform_block_size_minus2=0`,
`log2_diff_max_min_luma_transform_block_size=3`, both transform hierarchy
depths 0, `amp_enabled_flag=0`, `sample_adaptive_offset_enabled_flag=1`,
`sps_temporal_mvp_enabled_flag=1`, `cu_qp_delta_enabled_flag=1` and
`deblocking_filter_override_enabled_flag=1`. The driver now returns exactly
those as REQUIRED/SUPPORTED with a 32x32 CTB, and the same encode runs
without the two warnings.

### The installed package, re-qualified

`scripts/install-system.sh build` installed `cc0d022b…` as
`/usr/lib64/dri/v4l2_drv_video.so` at source commit `94c646e`, tracked source
`6bf8c526…`, `IRIS_SOURCE_DIRTY=0`. The manifest records the rollback as
`/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`, the former
`fe3a1822…`. Every run below has `source_dirty=0` and resolved the driver
through libva with no `LIBVA_DRIVERS_PATH`.

| gate | run | result |
| --- | --- | --- |
| `tonemap_gate.sh` (`IRIS_VPP_INSTALLED=1`) | `run-20261113T102614Z` | 11 cases, 3 refusals |
| `overlay_gate.sh` (`IRIS_VPP_INSTALLED=1`) | `run-20261113T102649Z` | 7 cases, 2 endpoints, 8 refusals |
| `crop_vaapi_gate.sh` (`IRIS_VPP_INSTALLED=1`) | `run-20261113T102725Z` | 3 exact |
| `scale_vaapi_gate.sh` (`IRIS_VPP_INSTALLED=1`) | `run-20261113T102800Z` | 8 ratios exact |
| `transpose_vaapi_gate.sh` (`IRIS_VPP_INSTALLED=1`) | `run-20261113T102836Z` | 7 directions, 2 formats |
| `iris-matrix.sh` | `run-20261113T102931Z` | H.264, HEVC, VP9, AV1 48/48 exact, `drops=0` |
| `iris-real-clip-gate.sh` | `run-20261113T103007Z` | AV1 120/120 `import=120 copy=0`; DRC 80 frames `import=42 copy=42` |
| `iris-encode-quality.sh` | `run-20261113T103047Z` | 120 frames each, SSIM 0.9986 / 0.9968, HEVC IRQ +241 |
| `iris-browser-acceptance.sh` | `run-20261113T103134Z` | Chrome 156 `VaapiVideoDecoder`, `platform=True`, 478 frames, 0 dropped, `driver=installed:cc0d022b` |
| `iris-av1-browser-matrix.sh local-only` | `run-20261113T103213Z` | Firefox 485 decoded / 8 dropped `hw_log=True`; Chrome 477 / 0 |

## A packed overlay, stored alpha, and the image list the client sizes (`569fd19`)

`overlay_vaapi` had never run against this driver. FFmpeg uploads its overlay
input into a `VA_RT_FORMAT_RGB32` surface, and for any input carrying an alpha
channel it sets `VA_BLEND_PREMULTIPLIED_ALPHA` (`vf_overlay_vaapi.c`:
`have_alpha_planar`). This backend had neither a packed surface nor the second
blend flag, so the graph failed before the processor was reached.

The packed surface is one plane of 32-bit words end to end: `BGRA` maps to
`DRM_FORMAT_ARGB8888` and `RGBA` to `DRM_FORMAT_ABGR8888`, the image layer
fills, views and copies a single plane at four bytes a sample, and the surface
layer takes `RGB32` on its own and picks the byte order from the pixel-format
attribute. `overlay_rgb.comp` converts each sampled pixel with the picture's
own matrix (BT.601, BT.709, BT.2020) to normalised Y'/Cb'/Cr', scales the
stored main codes by `1 - alpha`, adds, and encodes once. Mixing in codes is
equivalent for a straight source because limited-range Y'CbCr is affine in RGB;
it is not equivalent for a stored one, where the neutral offset is part of the
pixel. `overlay.comp` gained the same stored-alpha form around each plane's
neutral code for its Y'CbCr overlay.

Found on the way: `queryImageFormats` wrote one entry per format it knew with no
regard for the caller's capacity. FFmpeg sizes its array from
`vaMaxNumImageFormats()`, and the two new formats pushed the write past it — a
`malloc(): invalid next size` in every FFmpeg run, including a plain NV12
upload and download, before any trace line beyond `va init`. Valgrind on the
target named `va::queryImageFormats` at `image.cc:172`, "Invalid write of size 8
… 0 bytes after a block of size 96". The driver now answers the full count,
writes at most `*count` entries, and `kMaxImageFormats` is the single value
both sides read.

### The gates

`overlay_gate.sh` grew from seven cases to fifteen: both byte orders, both bit
depths, the stored-alpha form, BT.601, and a packed overlay over a tone-mapped
main surface. It also grew the first check whose expectation does not come
from the probe's own model of the kernel: the same overlay at straight and at
stored alpha, compared byte for byte after the straight frame is premultiplied
in the gate. Seven shapes are refused (luma keying, a third pass, a blend
before its pass, a rectangle outside the output, an odd rectangle, a crop of
the main surface, an alpha of 1.5).

`overlay_vaapi_gate.sh` drives the client itself. Both frames are one colour,
so the expected result is one BT.709 triple computed in awk, or one
stored-alpha mix, and all three cases are exact: alpha 0 with a black overlay
is the main frame byte for byte; alpha 1 is the overlay colour's own codes
(62, 102, 240 for the `rgb(254,0,0)` lavfi produces, after asserting the band
is uniform); alpha 0.25 is `overlay + 0.75·main` in normalised codes,
`max_diff=0`. The trace must say `source=bgra premultiplied`, and the same
graph must be refused without `V4L2_VA_VPP`.

Build tree first, `LIBVA_DRIVERS_PATH=$PWD/build/src`, source commit `569fd19`,
tracked source `15d42f03…`, `source_dirty=0`, driver `acec075a…`, run
`run-20261113T105641Z`:

| gate | cases | result |
| --- | ---: | --- |
| `nv12_scale_gate.sh` | 27 x 2 origins | byte-exact, `spv=5425102c…` |
| `p010_scale_gate.sh` | 12 x 2 origins | byte-exact, `spv=957a0e4f…` |
| `tonemap_gate.sh` | 11 + 3 refusals | within 1 code |
| `overlay_gate.sh` | 15 + 2 endpoints + 7 refusals | within 1 code, the stored-alpha cross-check exact |
| `overlay_vaapi_gate.sh` | 3 | exact, `client=ffmpeg` |
| `scale_vaapi_gate.sh` | 8 ratios, 3 P010 | exact |
| `crop_vaapi_gate.sh` | 3 | exact |
| `transpose_vaapi_gate.sh` | 19, 7 directions, 2 formats | exact |

### The installed package, re-qualified

`scripts/install-system.sh build` installed `acec075a…` as
`/usr/lib64/dri/v4l2_drv_video.so` at source commit `569fd19`, tracked source
`15d42f03…`, `IRIS_SOURCE_DIRTY=0`. The manifest records the rollback as the
former `cc0d022b…` at
`/usr/local/share/iris-vaapi/backup/v4l2_drv_video.so`. Every run below is
`source_dirty=0`, guard `complete`, exit 0, and resolved the driver through
libva with no `LIBVA_DRIVERS_PATH`. The image-format list and the config's RT
format changed, so the decode, encode and browser paths were re-run, not just
the processor.

| gate | run | result |
| --- | --- | --- |
| eight VPP gates, `IRIS_VPP_INSTALLED=1` | `run-20261113T105817Z` | all cases above, `driver_source=installed-package` |
| `iris-matrix.sh` | `run-20261113T110108Z` | H.264, VP9, HEVC, AV1 48/48 exact, `drops=0`, AV1 `noshow=23` |
| `iris-real-clip-gate.sh` | `run-20261113T105948Z` | AV1 120/120 `import=120 copy=0`; DRC 80 frames `import=42 copy=42` |
| `iris-encode-quality.sh` | `run-20261113T105948Z` | 120 frames each, SSIM 0.998596 / 0.996808, HEVC IRQ +243 |
| `iris-browser-acceptance.sh` | `run-20261113T110108Z` | Chrome 156.0.8078.79 `VaapiVideoDecoder`, `platform=True`, 478 frames, 0 dropped, `driver=installed:acec075a` |
| `iris-av1-browser-matrix.sh local-only` | `run-20261113T110108Z` | Firefox 484 decoded / 11 dropped `hw_log=True`; Chrome 477 / 0 |

Identity for all four runs: kernel `7.2.6-sm8550-g42f3b40-iris1`, module
`b90bd823…`, boot `4b73926c…` before and after, decoder
`Iris Decoder` at `/dev/video0` and encoder
`Iris Encoder` at `/dev/video1`, both resolved by
`iris_driver` QUERYCAP. Battery 40 % discharging at ~1.0 A, thermal peak
50.5 °C over the VPP suite and 53.2 °C over the matrix and browsers.

## Debian packaging: built in a bookworm chroot, run on the target (`9dccdeb`)

WP6 said not to add `debian/`: `libgbm-dev` pins an exact `libgbm1` and can
downgrade a vendor-patched graphics stack, no Debian or Ubuntu system is a
qualified target, and a package that cannot reproduce the Fedora/Arch
provenance and installed-package gates would be an unverified distribution
surface. The first two hold. The third is a cost, not a prohibition, and it is
paid here: the package is built where Debian builds happen (a clean chroot)
and the artifact it produces runs the gates on `sheng`.

`debian/rules` installs through `scripts/install-system.sh` like the other two
formats, pins `/usr/lib/$(DEB_HOST_MULTIARCH)/dri`, strips before refreshing
the provenance manifest, and takes the source identity from the checkout or
from `IRIS_SOURCE_COMMIT`, `IRIS_TRACKED_SOURCE_SHA256` and `IRIS_SOURCE_DIRTY`
when a tarball has no `.git`. All three formats now also build-depend on
Vulkan, because the video processor is compiled only when the loader and
headers are found, and on FFmpeg, because the test stage runs the host checks
and three of them compare against it.

### Two defects the clean build found

Both were invisible on the qualified target, which has GCC 16 and FFmpeg:

1. `src/codec/annexb.h` names `size_t` without `<cstddef>`. bookworm's
   libstdc++ 12 stops at `'size_t' has not been declared`; GCC 16 pulls the
   type in through `<vector>`, which is a property of that library, not of the
   header. `bitwriter.h` and `session.h` name it the same way. Fixed in
   `0f3b9a0`.
2. The test stage of all three formats runs the host checks, and the rotation
   model plus the two AV1 oracles need FFmpeg. None of the three declared it,
   so a clean build root fails at `%check` / `dh_auto_test` while a developer
   machine with ffmpeg installed hides it. Fixed in `4296c1c`.

### The build

Debian bookworm arm64 rootfs, extracted from the image and entered with
`chroot` on the lab host (`crun` refuses `RLIMIT_NPROC` in this VM, so a
container was not available); `apt-get install build-essential meson
ninja-build pkg-config python3 libva-dev libdrm-dev libegl-dev libgles-dev
libgbm-dev libvulkan-dev dpkg-dev debhelper ffmpeg`, then
`dpkg-buildpackage -b -us -uc` with the source identity passed for the tarball.
`meson test` inside the chroot: 26/26.

The Fedora target cannot build this package: `dh_shlibdeps` needs Debian's
shlibs database and aborts with "no dependency information found for
/lib64/libdrm.so.2". That is the reason the build lives in the chroot, not a
workaround for it.

`libva-v4l2-iris_0.1.0_arm64.deb`, sha256 `e1de06d7…`, 160,992 bytes,
Installed-Size 513:

```text
Depends: libc6 (>= 2.34), libdrm2 (>= 2.4.38), libegl1, libgbm1 (>= 7.11~1),
         libgcc-s1 (>= 3.0), libgles2 (>= 1.0.0), libstdc++6 (>= 12),
         libvulkan1 (>= 1.2.131.2), v4l-utils
```

Those are `dh_shlibdeps`' own answers, not a hand-written list; `v4l-utils` is
the node rule's requirement and is the one line `debian/control` states. The
package carries the driver, the `msm_drv_video.so` symlink, `iris-node.sh`, the
three diagnostic commands, both READMEs, `copyright` and the install manifest.
The manifest's `artifact_sha256` equals the `.so` the package contains
(`c0598c7e…`), which is what proves the refresh after `dh_strip` ran on the
payload and not on the build-tree file.

Two independent chroot builds — `4296c1c` and `9dccdeb` — produced a
byte-identical driver (`c0598c7e…`), so the artifact the gates below ran
against is the artifact in the final package.

### The package on the target

Extracted with `dpkg-deb -x` and pointed at with `LIBVA_DRIVERS_PATH`; nothing
was copied into `/usr/lib64/dri`, so this is the packaged payload and not the
installed package.

| check | run | result |
| --- | --- | --- |
| `vainfo` through libva | — | `va_openDriver() returns 0`, H.264/HEVC/VP9 profiles listed |
| `overlay_vaapi_gate.sh` | `run-20261113T112919Z` | 3 cases `max_diff=0`, `driver=c0598c7e…` |
| `iris-matrix.sh` | `run-20261113T113021Z` | H.264, VP9, HEVC, AV1 frame-md5 exact, `drops=0` |

Both runs are guard `complete`, exit 0, `source_dirty=0`, boot `4b73926c…`,
module `b90bd823…`, decoder `Iris Decoder` at `/dev/video0`. The gate
record says `driver_source=build-tree` because that field names how the driver
was found, and `LIBVA_DRIVERS_PATH` is the build-tree mechanism; the hash is
the packaged bytes.

### What this does not prove

No Debian or Ubuntu system is a qualified target. The package was built on
bookworm and run on the Fedora 44 target, which proves the artifact, its
dependency metadata and its provenance — not a Debian runtime. The `msm`
alias, the node rule and the diagnostics are carried by the package but were
exercised from the installed-tree copies on this boot, not from the extracted
tree.

## Bilinear interpolation in the video processor scale (`234a7f7`)

The scale kernel sampled at swscale's point positions, which is what made
`scale_vaapi` byte-identical to `scale=flags=neighbor`. It had no second
method, so a picture naming `VA_FILTER_INTERPOLATION_BILINEAR` was refused —
one interpolation method where VA names three, and the one FFmpeg's
`scale_vaapi` reaches for when a client asks for anything better than nearest.

Both kernels (`nv12_scale.comp`, `p010_scale.comp`) gained a second path
behind one push-constant switch. The point map is untouched. The bilinear path
maps the destination sample's centre back into the source, clamps it to the
source's extent, and blends the four samples around it. VA mirrors after
rotating, so the reflection applies to that continuous position rather than to
a rounded index, and a 1:1 scale lands exactly on the sample the point map
picks. On P010 the blend runs on the ten-bit code and not on the padded word:
mixing words would leave bits in the low six, which is not a legal P010
sample.

Two defects turned up while wiring the VA layer and the gates:

- `caps->filter_flags` was set to `NEAREST | BILINEAR`. That is 0x1000 | 0x2000
  = 0x3000, exactly `VA_FILTER_INTERPOLATION_ADVANCED`: the field carries one
  method, not a bit set. The first run of `crop_vaapi_gate.sh` reported the
  driver advertising the one method it refuses.
- `--expect-refusal` was matched in `argv[12]` only, so adding a second option
  to `iris-va-vpp-probe` made the refusal case run the picture instead, and the
  gate reported a correct refusal as a driver that ran it.

The probe's own bilinear oracle had the same kind of fault: it mirrored the
continuous position and then mirrored the rounded index again, so every
mirrored case failed on the target while the kernel was right. The oracle now
mirrors once, in the caller, exactly where the kernel does.

### The gates

Source `cef4349` for the two primitive gates — their 1:1 bilinear cases are
compared byte for byte from that commit on — and `234a7f7` for the VA-level
ones. Driver `40ab046a…` (build-tree override, `LIBVA_DRIVERS_PATH`), boot
`4b73926c…`, kernel `7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`,
decoder resolved by driver name to `/dev/video0`, encoder to `/dev/video1`.
Every run below is a guarded target run with `source_dirty=0`.

| gate | run | cases | verdict |
| --- | --- | --- | --- |
| `nv12_scale_gate.sh` | `121526Z` | 27 nearest + 12 bilinear, both DMA-BUF origins | `PASS` |
| `p010_scale_gate.sh` | `121607Z` | 12 nearest + 9 bilinear, both origins | `PASS` |
| `crop_vaapi_gate.sh` | `120602Z` | 3 crops, 1 bilinear picture, 2 refusals | `PASS` |
| `scale_vaapi_gate.sh` | `120643Z` | 8 NV12 + 3 P010 ratios | `PASS` |
| `transpose_vaapi_gate.sh` | `120721Z` | 19: all seven directions, NV12 and P010 | `PASS` |
| `tonemap_gate.sh` | `120803Z` | 11 cases, 3 refusals | `PASS` |
| `overlay_gate.sh` | `120842Z` | 15 cases, 2 endpoint frames, 7 refusals | `PASS` |
| `overlay_vaapi_gate.sh` | `120922Z` | 3 through FFmpeg's `overlay_vaapi` | `PASS` |

The bilinear cases are compared within one code of a double-precision model of
the blend, and two invariants bracket them: a constant picture must come out
that constant at any ratio, because the weights sum to one, and a 1:1 scale
must reproduce the point map byte for byte — the probe drops its one-code
tolerance for exactly that geometry, because at a sample centre there is
nothing to blend. The 8-bit gate's constant case is `77` over
`64x32 -> 200x100`, a ratio whose weights are all fractional; the 10-bit gate's
is `1023`, the largest code, which also proves the clamp is the ten-bit one
rather than the eight-bit saturation carried over.

The host half runs in CI: `rotation_model_check.py` compares the bilinear map
with swscale's `flags=bilinear` on six upscales, where swscale uses the same
four taps (maximum difference 1 code, four of the six exact). Downscales are
excluded there and only there, because swscale widens its support with the
ratio while the kernel always takes two neighbours per axis; the primitive
gates measure those ratios against the kernel's own model instead.

### What this does not prove

The bilinear path has no byte-exact software reference at any ratio: swscale's
`flags=bilinear` is a wider tent when it downsamples, so the comparison the
point map gets is not available. The evidence is a model of the documented
blend plus the two invariants, not an equivalence with a reference filter. No
client was measured using it end to end — FFmpeg 8.1's `scale_vaapi` submits
`filter_flags = VA_FRAME_PICTURE` (zero) on every call, so the method is
reached today only by a client that sets it, such as Chromium's VA-API wrapper
on the paths where it asks for EU-bilinear. The `crop_vaapi_gate.sh` bilinear
case proves the VA layer accepts and runs the method; it does not compare
those pixels with anything.

## The composite samples its own region, all three methods, and both ranges (`31987d8`)

The composite read the overlay surface whole, sampled it only at the point map,
and encoded its result in the limited range. A client that named an overlay
region, an interpolation method or a full-range output got the whole surface at
nearest and a limited-range frame, silently.

The two overlay kernels now run the same tap model the scale kernel runs, over
the layer's own region instead of the whole source: the point map, the
four-tap blend, the area average and the cubic, with `advanced` resolving per
axis. The region is `surface_region` of the blending pass, which FFmpeg leaves
null for the whole surface. The destination's range comes from
`output_color_properties`, which moves the luma floor from 16 to 0 and the
chroma span to the full one.

Three defects turned up on the target, two in this change and one older:

- **The premultiplied form was wrong, and had been since it shipped.** It
  computed `out = overlay + (1 - alpha) * main`, which leaves the overlay at
  full strength whatever global alpha the client names. FFmpeg sets
  `VA_BLEND_PREMULTIPLIED_ALPHA` for *any* overlay input with an alpha channel
  (`vf_overlay_vaapi.c`, `have_alpha_planar`), so a plain opaque BGRA overlay
  composited at `alpha=0.25` came out at `alpha=1`. The correct form is
  `out = alpha * overlay + (1 - alpha * a) * main`: the stored colour carries
  the pixel's own coverage, not the global factor. The sibling repository's
  shader (`src/vulkan/overlay.comp:55`) has the same defect. Both the
  primitive gate's model and `overlay_vaapi_gate.sh`'s awk oracle encoded the
  old form, so both passed; the FFmpeg gate's alpha-0.25 case is what caught
  it once the oracle was corrected, and `alpha0` and `alpha1` passed
  throughout because both endpoints are insensitive to it.
- **The probe's oracle did not clamp.** A mix of two codes can leave the
  representable range — the cubic has negative lobes, so an overshoot is
  ordinary — and the kernel clamps in `code_bits` while the model wrote an
  unclamped value that wrapped. The cubic case reported a 255-code difference
  against a kernel that was right.
- **The gate's sampling-method cases used a 2:1 shrink.** At exactly 2:1 the
  area average over two samples *is* the bilinear midpoint, so the two methods
  coincide by construction and the case could not tell them apart. The cases
  moved to 3.2:1, where the area footprint is three or four samples.

### The gates

Source `31987d8`, tracked source `60ecc051…`, driver `980390ef…` (build-tree
override, `LIBVA_DRIVERS_PATH=build/src`), boot `4b73926c…`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`, decoder resolved by
`iris_driver` QUERYCAP to `/dev/video0`, encoder to `/dev/video1`. Every run
below is a guarded target run with `source_dirty=0`.

| gate | run | cases | verdict |
| --- | --- | --- | --- |
| `overlay_gate.sh` | `133243Z` | 26 cases, 2 endpoints, 9 refusals | `PASS`, every case within 1 code |
| `overlay_vaapi_gate.sh` | `133621Z` | 3 through FFmpeg's `overlay_vaapi` | `PASS`, exact |

The 26 cases are the previous 15 plus `crop`/`rgbcrop` (a layer reading
`48x36+24+18` of a 96x72 surface), `bilinear`/`rgblinear` and
`advanced`/`rgbadvanced` over a 3.2:1 shrink, `cubic` over a 1.56:1 growth,
`full`/`rgbfull`/`premult-full`, and `rgbopaque` (a premultiplied packed
overlay whose alpha byte is 255, which has to land byte-exact on the same
picture as a straight-alpha run at the same global alpha). The refusals gained
an overlay region that leaves its own surface and an odd one.

Four checks in the gate do not come from the model: the two endpoint frames
byte for byte, the premultiplied-equals-straight equality above, a per-pixel
alpha that is not opaque having to differ from an opaque one, and the three
sampling methods having to produce three different pictures at a ratio where
they genuinely differ.

`overlay_vaapi_gate.sh`'s three cases are the ones that matter for the
premultiplied defect: `alpha 0` is the main frame byte for byte, `alpha 1` is
the overlay colour's BT.709 codes exactly, and `alpha 0.25` is the global-alpha
mix — the only one of the three that a premultiplied form missing the global
factor can fail.

### What this does not prove

The overlay region and the full-range output are proven by the probe and by
the primitive gate's model, not by a real client: FFmpeg 8.1's `overlay_vaapi`
sets neither, so no FFmpeg run exercises them. The cubic has no software
reference at any ratio — like the bilinear path, its evidence is the tap model
plus the endpoints, not an equivalence with a reference filter. Full-range
output is checked against the same model that defines it; no client was
measured consuming a full-range frame from this driver.

## Bit-depth conversion in the VPP scale (2026-11-13, branch `feat/absorb-upstream-advantages`)

The processor refused a picture whose two ends disagreed on bit depth, on the
reasoning that swscale's 10-to-8 path dithers by a count that depends on the
sample position. That reasoning does not hold for the default path: FFmpeg
picks the dither from `sws_flags`, and with the point map and no dither flag
the conversion is a plain `code >> 2`. Measured on the host before any code
changed (`<scratch>/bitdepth2/`): a P010 frame converted to NV12 differs
from `(word >> 6) >> 2` on 0 of 4096 luma and 0 of 2048 chroma samples, and
`code << 2` in the other direction is exact for a code whose low two bits are
clear.

Both kernels now take a `src_wide` push constant and convert before any
filtering, so the destination's depth is the module's own — `nv12_scale.spv`
packs four samples per word, `p010_scale.spv` two — and the source's is free.
A point map picks one source sample, so converting before the pick and after
it are the same sample; that is why a cross-depth picture is byte-identical to
`scale` then `format` rather than merely close.

### The gates

Source `8686e62` with this change in the working tree (`source_dirty=1`),
driver `11ce0c15…` (build-tree override, `LIBVA_DRIVERS_PATH=build/src`),
boot `4b73926c…`, kernel `7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`,
decoder resolved by `iris_driver` QUERYCAP to `/dev/video0`, encoder to
`/dev/video1`.

| gate | run | cases | verdict |
| --- | --- | --- | --- |
| `nv12_scale_gate.sh` | `135141Z` | 27 + 12 bilinear + 6 area + 5 cubic + 5 advanced + 7 down, 2 origins | `PASS`, exact |
| `p010_scale_gate.sh` | `135224Z` | 12 + 9 bilinear + 6 area + 5 cubic + 5 advanced + 7 up, 2 origins | `PASS`, exact |
| `scale_vaapi_gate.sh` | `135601Z` | 8 NV12 + 3 P010 + 4 down + 4 up | `PASS`, byte-identical to swscale |

The `down` list runs a P010 source into the eight-bit kernel and the `up` list
an NV12 source into the ten-bit one, each under the point map and the bilinear
blend, over ratios, crops, rotations and mirrors, against a model that applies
the same shift. The constant cases bracket them: 1020 is the widest 10-bit
code that survives `code >> 2` without rounding and must come out as 255, and
255 is the top of the eight-bit range and must come out as 1020.

The four VA-level down cases and the four up cases are the ones a client can
reach: `scale_vaapi=w=W:h=H:format=` asks FFmpeg for the other depth on the
output surface, and every output frame matched `scale` then `format` byte for
byte, including a 1280x720 frame each way. An unsupported output format
(`yuv420p10le`, which no surface path in this driver exposes) is still refused,
so the format gate is still a gate.

### What this does not prove

The conversion is proven for the exact shift and nothing else. A client that
sets `SWS_DITHER_AUTO` or a dithering flag on the software side gets a
different picture than this driver produces, and no FFmpeg run here exercised
that. FFmpeg's `scale_vaapi` cannot name a dither, so the mismatch is not
reachable through it; it is reachable for a client comparing against a
software chain that dithered. The chroma planes go through the same shift
without the chroma-specific rounding swscale applies when the two depths have
different chroma sample positions, which NV12 and P010 do not.

## Stacked compositing (2026-11-13, branch `feat/absorb-upstream-advantages`)

A picture could carry one main pass and one blending pass. VA puts no limit on
the number of pipeline buffers, and the sibling driver accepts sixteen; a
client that stacks a subtitle over a logo over a video had no way to submit
that here.

The context now keeps a list of blending passes in submission order, and
`run_vpp` copies the main rectangle into the output once — the scale kernel at
1:1, since a blended picture does not scale its main pass — and then runs one
overlay kernel per layer with the output standing as both the main term and the
destination. Reading the output back between dispatches is what lets one kernel
serve any number of layers: layer i covers layers 0..i-1 exactly as it would
cover the main surface, because the kernel only ever sees one "main" buffer.
The main rectangle is the output's own size, which is the kernel's rule and
also what makes the main term an identity read.

### The gates

Source `c257d92` with this change in the working tree (`source_dirty=1`),
driver `f20b0588…` (build-tree override, `LIBVA_DRIVERS_PATH=build/src`), boot
`4b73926c…`, kernel `7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823…`, decoder
resolved by `iris_driver` QUERYCAP to `/dev/video0`, encoder to `/dev/video1`.

| gate | run | cases | verdict |
| --- | --- | --- | --- |
| `overlay_gate.sh` | `141003Z` | 30 cases, 2 endpoints, 9 refusals | `PASS`, every case within 1 code |
| `overlay_vaapi_gate.sh` | `141049Z` | 3 through FFmpeg's `overlay_vaapi` | `PASS`, exact |
| `overlay_gate.sh` (bound) | `165001Z` | 32 cases, 2 endpoints, 9 refusals | `PASS`, sixteen layers at `max_diff=0` |

The four new cases are `layers2`, `layers3`, `layers2rgb` and
`layers2premult`. Each layer blends a rectangle two pixels down and right of
the one before, so the stack is a picture no single layer produces, and the
probe's model composites them in the same order onto the pixels the previous
one left — a model that reset to the main frame each time would pass every
single-layer case and be wrong for every stack. All four came out with
`max_diff=0`.

The last row is the bound reached rather than named. `layers16` and
`layers16rgb` stack sixteen layers — the figure the driver refuses a
seventeenth of, and the one the README advertises — on a 32x24 rectangle whose
sixteenth position still ends at 78,70 inside a 128x72 output, so the layer
count is the only thing under test. Run `165001Z` on committed source
`e70ff2a` (`source_dirty=0`) reports `cases=32 endpoints=2 refused=9`,
`max_diff=0` on both, guard `complete`, driver `f20b0588…` from `build/src`,
boot `4b73926c…`, module `b90bd823…`. The first attempt at this row put its
comments inside the case heredoc and the loop read them as cases; the two real
cases passed in that run as well, and the clean record is the one above.

Two refusals changed. `--third` used to be a refusal case and is now a feature,
so it is gone. `too-many-layers` replaces it at seventeen layers, one past the
bound, on geometry that fits — the seventeenth rectangle ends at 72,68, so the
driver's layer bound is what fails the picture rather than its geometry or the
probe's argument check. `blend-first` still passes, which is the ordering rule
the list depends on — a picture whose first pipeline buffer carries a blend
state is refused rather than composited onto an undefined main surface.

### What this does not prove

The cost of a stack is one dispatch per layer with a full output read-back
between them, so a sixteen-layer picture is sixteen passes over the output
rather than one — correct, but not free, and nothing here measured what it
costs. The sibling driver's figure was copied as a bound on a single
`vaEndPicture`, not as a performance target.

## The CDSP FastRPC path, measured instead of assumed (2026-11-13)

Three claims about the DSP backend were recorded without a measurement and
all three were wrong. They are corrected here from probes run on `sheng`,
boot `4b73926c…`, kernel `7.2.6-sm8550-g42f3b40-iris1`. The probes are
throwaway C programs under `<scratch>/fastrpc-probe/`; none of them is
in the repository, because none of them gates a capability.

| claim | measurement | verdict |
| --- | --- | --- |
| `sheng` cannot open a FastRPC session | `DSPRPC_GET_DSP_INFO` answers for the CDSP domain: `DOMAIN_SUPPORT=1`, `UNSIGNED_PD_SUPPORT=1`, `HVX_SUPPORT_128B=4`, `ARCH_VER=0x1008c73` (v73), `VTCM_PAGE=8 MiB`, `VTCM_COUNT=1` | wrong: the session opens and the domain is fully described |
| no Hexagon compiler exists | Fedora clang 22 compiles `--target=hexagon -mv73` to an object `file` reports as `ELF 32-bit LSB relocatable, QUALCOMM DSP6`; `hexagon_types.h`, `hexagon_protos.h` and `hexagon_circ_brev_intrinsics.h` ship with clang | wrong: the compiler exists; the **linker** does not (`ld.lld` has no `elf32hexagon`, no `hexagon-ld`) |
| the shell binary is what blocks the PD | a v73 `fastrpc_shell_unsigned_3` from `hexagon-dsp-binaries-qualcomm-hamoa-iot-evk`, placed at `/usr/share/qcom/sm8550/Xiaomi/sheng/cdsp/`, still fails `FASTRPC_IOCTL_INIT_CREATE` with `0x80000416`; an empty file of that name fails identically | wrong: the failure is upstream of the shell's contents |

The first claim survived because the only probe used was `remote_handle_open`
on a module that cannot exist. With no shell installed, that path returns
`AEE_EBADFD` (`0x8000044f`) — a "bad file descriptor", which reads as a
session failure and is not one. `DSPRPC_GET_DSP_INFO` goes through
`remote_handle_control` with a null handle and answers regardless.

The device node matters. The library opens `/dev/fastrpc-cdsp-secure`, not
`/dev/fastrpc-cdsp`, and the device-tree node is `qcom,non-secure-domain`
with no `qcom,nsessions` and no `memory-region` property. With an empty file
failing exactly as a real shell does, the create is refused by the firmware
after the kernel forwards it, so no userspace change reaches it.

### What this does not prove

Nothing here measures a DSP computation, because no skeleton can be loaded and
none can be built: the compiler emits objects but no Hexagon linker exists to
produce a `.so`, and the pinned Open Access archive that would supply one
answers 403 to an unauthenticated download from this host. The v73 shell came
from a different board's package (`hamoa-iot-evk`), so "the shell is not the
problem" is proven for that shell on this firmware and not for a vendor-matched
one. A DSP VPP backend remains blocked on a device-tree change, a firmware
that accepts the unsigned PD, and a Hexagon linker — three things outside this
repository, against a Vulkan backend that already passes every gate.

## AV1 film grain synthesis: exact against libdav1d at 8 and 10 bits (2026-11-14)

This supersedes the "film grain synthesis is a two-surface VA-API contract"
record above. The two-surface path now exists: the picture decodes into
`current_frame` with grain off, and the blend onto `current_display_picture`
runs in `src/va/grain.cc`.

Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1` (`qcom_iris` module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`,
`qcom-iris.ko.zst`, unchanged and not reloaded). The decoder node is resolved
by QUERYCAP driver name `iris_driver` (`/dev/video0` this boot) and the
encoder is the second `iris_driver` node (`/dev/video1`). Source commit
`6eb15ab` plus the film-grain worktree (tracked-content digest
`32bc0027e67760e1fbbc5143baa8511132d052b076bfea175c422545504beed1`).
The run used the build tree
(`LIBVA_DRIVERS_PATH=$HOME/iris-vaapi-lab/src/build/src`, driver sha256
`2f655483ebc3a88d183eaca289f4312350a788cb676275b88f6e2cb2750699dc`), not an
installed package. Battery 99% discharging, peak thermal zone 37000 millic,
no firmware reset.

### Gate

`test/iris-av1-grain-gate.sh` decodes each clip through FFmpeg's VA-API
decoder and compares every downloaded frame with a framemd5 generated by
libdav1d, which applies grain itself. The gate does not generate its own
baseline: a broken blend would bless its own output.

```
IRIS_GRAIN_CLIP_DIR=$HOME/iris-vaapi-lab/clips/grain \
LIBVA_DRIVERS_PATH=$HOME/iris-vaapi-lab/src/build/src \
sh test/iris-av1-grain-gate.sh
PASS grain8  frames=20 blends=20 import=21 copy=0 invisible=11
PASS grain10 frames=2  blends=2  import=2  copy=0 invisible=0
PASS AV1 film grain gate set=all publish=import driver=build tree
```

| clip | geometry | store | frames | blends | publish | result |
| --- | --- | --- | --- | --- | --- | --- |
| grain8 | 320x240 | NV12 | 20 | 20 | import=21, invisible=11 | every frame hash equal to libdav1d's grained output |
| grain10 | 352x290 | P010 | 2 | 2 | import=2 | every frame hash equal to libdav1d's grained output |

`grain8` submits 21 pictures for 20 displayed frames: AV1 also decodes
`show_frame=0` pictures, and the 11 `publish=invisible` records are install
halves of two-completion units. The session ledger for the clip is
`submitted=21 completed=21 errors=0 drops=0 timeouts=0 misplaced=0
retained=0`.

The same two clips were decoded under the other publish policies by hand
(not by the gate): every frame hash matched under `V4L2_VA_PUBLISH=direct`
(21 direct, 11 invisible) and `V4L2_VA_PUBLISH=copy` (21 copy-gpu, 11
invisible), and `grain10` matched under both with 2 pictures each.

### Two defects the gate caught

The luma plane was byte-identical to libdav1d from the first run while the
chroma planes differed by up to 12 codes. The scaling index of a chroma
sample mixes its own code with the luma it covers through
`cb_mult`/`cb_luma_mult`/`cb_offset`, which AV1 carries as biased unsigned
syntax elements (multiplier + 128, offset + 256); the driver passed the raw
codes through, and a constant index shift still produces plausible noise.
`grain_params()` now reduces them to the signed values and `blend_chroma()`
scales the offset by the bit-depth shift, as the specification does.

The first version ran the blend from a completion callback on the decode
target. FFmpeg decodes into a scratch `current_frame` that it recycles at the
next access unit, so the callback raced the surface's reuse; the trace ended
in `film grain: the picture's source surface was reused before it decoded`.
The blend now runs inside `endPicture`, after the session settles the
picture and before the client can reuse the surface, which is also the order
Mesa's drivers use.

### What this does not prove

No browser was run on a film-grain stream, so there is no Chrome or Firefox
frame/drop window for this feature. Synthesis is a CPU pass over the decoded
picture and its cost was measured on the host (2.52 ms at 720p, 3.69 ms at
1080p, 7.43 ms at 2160p), not on the target. The gate covers two clips, one
from the AOM film-grain test pattern and one 10-bit clip, not the conformance
corpus. The Direct and Copy results are manual runs; the gate exercises
Import only.

## Startup and encode throughput: two serialised costs removed (2026-11-14)

Two costs sat on the client's critical path and both are gone. `vaCreateContext`
initialised GPU copy, which starts EGL, for every context — 105 ms on this
tablet, paid by encode contexts that never copy. The copy engine now builds its
backend on the first copy. Separately, `vaSyncSurface` on an encode context's
input surface claimed the access unit, so it waited for the whole picture;
FFmpeg syncs the input surface per picture, which doubled the per-frame
latency and made `-async_depth` useless. The firmware returns an input buffer
as soon as it has read the picture, and the session now answers the surface
sync from that, before the access unit exists.

Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1` (`qcom_iris` module unchanged and not reloaded),
encoder node `/dev/video1` by QUERYCAP name `iris_driver`, render node
`/dev/dri/renderD128`. Source commit `be2c6aa`, then `bd8d268`; the deployed
tree was this worktree before the second commit (`source_commit=be2c6aa`,
`source_dirty=1`, tracked digest
`36b13402878ed401acb3777c7cab114263fc78a2e5bc629685e2a82c9ef14f7f`). Release
build tree (`LIBVA_DRIVERS_PATH=$HOME/iris-vaapi-lab/src/build-rel/src`, driver
sha256 `f38e2fe8272e684e2346623f1369ead3962a7d2c6cd199f6ca989ee5aeafca70`), not
an installed package. Battery 93% discharging, no firmware reset.

### One frame and the copy policy

The sibling is `radxa-pkg/qcom-va-driver`, built in the same tree layout, run
interleaved on the same node with the same ffmpeg command.

| case | before | after | sibling |
| --- | --- | --- | --- |
| 720p HEVC encode, 1 frame | 0.106-0.137 s | 0.014-0.036 s | 0.013-0.019 s |
| 720p HEVC decode, 1 frame | 0.164 s | 0.051-0.070 s | 0.048 s |

`iris-matrix.sh` passes on H.264, HEVC, VP9 and AV1 on this driver: 48/48
frames exact, `drops=0`, under the default policy and under
`V4L2_VA_PUBLISH=copy V4L2_VA_COPY=gpu` (12 PASS records each; the trace names
`publish=copy-gpu`, so the deferred engine did start on the first copy).

### Encode after the input-release split

300 frames of `testsrc2` at 1280x720, five interleaved runs per driver, same
command:

| `-async_depth` | this driver | sibling |
| --- | --- | --- |
| 1 | 1.258 s | 1.84-2.01 s |
| 2 | 0.623 s | 0.69-0.81 s |
| 4 | 0.471-0.478 s | 0.49-0.51 s |
| 8 | 0.471 s | 0.52-0.53 s |

The `depth 4` rows of the size sweep, same interleaving:

| size | this driver | sibling |
| --- | --- | --- |
| 720p encode | 0.474-0.563 s | 0.486-0.497 s |
| 1080p encode | 0.530-0.538 s | 0.527-0.557 s |
| 720p decode | 0.421-0.435 s | 0.431-0.449 s |
| 1080p decode | 0.554-0.588 s | 0.553-0.584 s |

4K encode stays inside the run-to-run spread of both drivers (1.80-2.16 s)
with the core and frequency unpinned; the earlier 1.70-1.73 s figures for this
driver were taken before a long run. The DVFS section of the 4K decode record
above explains why an unpaced 4K workload moves with the clock, and the same
caution applies here.

### Correctness gates

- `iris-encode-quality.sh` on the build tree: H.264 SSIM 0.9986, HEVC 0.9968,
  both 120 frames, the platform decode byte-identical to the software decode,
  and the HEVC run moved the Iris IRQ counter by 249 (the hardware really
  coded it).
- `gst-launch-1.0 ... vah265enc` produced 7,396,834 bytes from 120 NV12
  frames and the stream decodes back to 120 frames with no errors. This is the
  client that syncs the surface and then maps the coded buffer, which is the
  path the split changes.
- Unit tests on the target: `encode-session` (including the new split case)
  and `deferred-copy`, plus the full local suite (27 tests).

### What this does not prove

No browser and no GStreamer `vah265dec`-based acceptance window was run for
this change; Chrome does not encode, and the Chrome decode pass is unchanged
by both commits. The matrix and quality gates ran from the build tree, not
from an installed package. 4K numbers are not comparable across runs on this
tablet without pinning CPU and GPU frequency.

<a id="cross-driver-benchmarks"></a>
## Comparative benchmarks and overhead summary (SM8550 vs radxa-pkg) (2026-11-14)

A consolidated index of head-to-head measurements conducted on the same tablet
(`sheng`, SM8550, kernel `7.2.6-sm8550-g42f3b40-iris1`) comparing this driver
against `radxa-pkg/qcom-va-driver`. Each row reflects the specific test
protocol, clip length, and boot session detailed in the referenced subsections
below.

| Benchmark | Frames / Duration | This driver | Sibling (`radxa-pkg`) | Relative difference / Notes | Source section |
| --- | --- | ---: | ---: | --- | --- |
| HEVC Main 8-bit Decode 720p | 900 frames | 759.1 fps | 758.5 fps | Parity (+0.1%), client utime -17% (0.274 vs 0.331 s) | §"Pinned-clock HEVC throughput..." |
| HEVC Main 8-bit Decode 1080p | 900 frames | 581.1 fps | 578.8 fps | Parity (+0.4%), client utime -16% (0.423 vs 0.501 s) | §"Pinned-clock HEVC throughput..." |
| HEVC Main 8-bit Decode 4K | 480 frames | 358.6 fps | 358.0 fps | Parity (+0.2%), client utime -19% (0.403 vs 0.495 s) | §"Pinned-clock HEVC throughput..." |
| HEVC Main 8-bit Encode 720p | 900 frames | 644.4 fps | 631.0 fps | +2.1% throughput, equal client utime (1.01 vs 1.08 s) | §"Pinned-clock HEVC throughput..." |
| HEVC Main 8-bit Encode 1080p | 900 frames | 591.0 fps | 575.0 fps | +2.8% throughput, equal client utime (1.94 vs 1.91 s) | §"Pinned-clock HEVC throughput..." |
| HEVC Main 8-bit Encode 4K | 480 frames | 167.2 fps | 163.2 fps | +2.5% throughput, equal client utime (4.82 vs 4.80 s) | §"Pinned-clock HEVC throughput..." |
| 1080p Decode: H.264 | 300 frames | 738.2 fps | 729.6 fps | +1.2% throughput, utime 0.073 vs 0.089 s | §"Decode, six codecs at 1080p" |
| 1080p Decode: VP9 Profile 0 | 300 frames | 477.1 fps | 471.8 fps | +1.1% throughput, utime 0.053 vs 0.052 s | §"Decode, six codecs at 1080p" |
| 1080p Decode: AV1 Main 8-bit | 300 frames | 468.9 fps | 427.2 fps | +9.8% throughput, client utime -35% (0.028 vs 0.044 s) | §"Decode, six codecs at 1080p" |
| 1080p Decode: AV1 Main 10-bit | 300 frames | 411.0 fps | 368.5 fps | +11.5% throughput, client utime -77% (0.029 vs 0.125 s) | §"Decode, six codecs at 1080p" |
| 1080p Decode: HEVC Main10 | 300 frames | 490.4 fps | 502.2 fps | -2.4% throughput (scheduling), client utime -16% (0.118 vs 0.140 s) | §"Decode, six codecs at 1080p" |
| Dual concurrent 1080p HEVC decodes | 600 frames/run | 835.8 ms | 829.2 ms | Wall-time parity (tie), aggregate utime -17% (0.254 vs 0.306 s) | §"Two concurrent decode sessions" |
| 1080p HEVC decode -> H.264 encode | 300 frames (5 rds) | 436.9 fps | 435.0 fps | Throughput tie, client utime -23% (0.121 vs 0.157 s) | §"Transcode" |
| Kodi 4K HEVC playback | 16 s clip | 18.92 s wall | 18.82 s wall | Wall-time parity (includes ~2.5 s startup), decoder CPU -6% (2.668 vs 2.848 s) | §"Kodi playback" |
| VPP 1080p -> 720p Vulkan scale | 900 frames | 540.0 fps | 535.6 fps | Throughput tie (spread > delta), client utime -14% (0.541 vs 0.619 s); swscale is 287 fps / 8.4 s | §"VPP against the sibling..." |
| VPP feature support | 30 frames | Mirroring, YUV & stacked overlays | Refused (-22) | This driver supports all 7 transpose directions and YUV overlays | §"VPP against the sibling..." |

*Note*: Test workloads were captured under pinned clocks (`performance` governor at 2016/2803/2956 MHz, GPU at 680 MHz) using identical FFmpeg 8.1.3 builds unless noted otherwise. Kodi wall time encompasses application launch, rendering, and exit; video decoding alone remains at wall-time parity. VPP throughput difference (540.0 vs 535.6 fps) falls within the run-to-run variation margin across 5 interleaved rounds.

## Pinned-clock HEVC throughput against the sibling on one node (2026-11-14)

Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, decoder node `/dev/video0` and encoder node
`/dev/video1` resolved by QUERYCAP name `iris_driver`, render node
`/dev/dri/renderD128`. This driver `f74ea6ad0defee68`, sibling
`f7dd97109f3a9965`, both built in the same tree layout and run from a
release build tree in interleaved rounds on the same clips. FFmpeg 8.1.3,
battery 83% discharging, thermal zones 31-47 °C, no firmware reset.

Throughput on this tablet tracks the clock, so CPU policies 0, 3 and 7 were
held in the `performance` governor at 2016000, 2803200 and 2956800 Hz and
`3d00000.gpu` in `userspace` at 680000000 Hz for the whole run. Each cell is
the mean of five rounds, alternating drivers. Clips are `x265` ultrafast with
`bframes=0` and `keyint=60`, 900 frames at 720p and 1080p and 480 frames at
4K, so process start-up is amortised rather than measured. Encode is
`-c:v h264_vaapi -b:v 5M/10M/20M -g 60 -async_depth 4` over an `-f lavfi`
`testsrc2` source, which includes the CPU NV12 upload. Decode is
`-hwaccel vaapi -hwaccel_output_format vaapi -f null -`, so it excludes the
download and the display.

| operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| decode, this driver | 759.1 fps | 581.1 fps | 358.6 fps |
| decode, sibling | 758.5 fps | 578.8 fps | 358.0 fps |
| encode, this driver | 644.4 fps | 591.0 fps | 167.2 fps |
| encode, sibling | 631.0 fps | 575.0 fps | 163.2 fps |
| decode client utime, this driver | 0.274 s | 0.423 s | 0.403 s |
| decode client utime, sibling | 0.331 s | 0.501 s | 0.495 s |

Decode sits at parity, within 0.4 % in every size. Encode is 2.1 %, 2.8 % and
2.5 % ahead. The client CPU column is the part that a browser or a player
feels: decode costs 17 %, 16 % and 19 % less user time per 900 frames, which
is the deferred copy engine — a pipeline that never asks for a copy never
starts the GPU copy backend. Encode utime is equal between the two drivers
(1.01 against 1.08 s at 720p, 1.94 against 1.91 s at 1080p, 4.82 against
4.80 s at 4K), so the fps difference there is scheduling overhead, not work.

The first attempt at this table was thrown away: a leftover profiler ffmpeg
had been spinning in `R` for 50 minutes and held a core, and its temperatures
were 52-64 °C. The numbers above are from a quiet machine.

What this does not prove: one synthetic clip per size, so scene complexity
and bitrate effects are not sampled; the sibling's published table is
SC8280XP and cannot be compared with these SM8550 numbers; 4K at 167 fps is
close enough to the encoder's own ceiling that a different content mix would
move it more than the driver difference.

## The video processor's per-picture Vulkan cost, and what it buys (2026-11-14)

The first working scale pipeline created a `VkInstance`, a physical device, a
logical device and a compute pipeline per picture. It passed every
correctness gate and ran 1080p to 720p at 20.8 fps on 34.6 s of client CPU
per 900 frames: the cost was device creation, not the scale.

`src/iris/vpp/scale.cc` now keeps one device and one compute queue for the
process, compiles each kernel once, and serialises submit-plus-wait on that
queue under a mutex. A job owns only its buffers, memories, descriptor and
command pools and fence; the device, descriptor set layout, pipeline layout
and pipeline are borrowed and outlive it. The shared state is deliberately
leaked so the device is not torn down while a client still holds surfaces.

Same command, same pinned clocks, same clip, five interleaved rounds:

| 1080p to 720p | fps | client utime per 900 frames |
| --- | ---: | ---: |
| VPP scale, cached device | 535.7 | 0.542 s |
| `hwdownload,format=nv12,scale=1280:720` | 287.0 | 8.370 s |

The scale is 1.87x the throughput of downloading and scaling on the CPU, and
it costs 6.4 % of the client CPU time, because the picture never leaves the
device: `hwdownload` pulls 900 frames out of the GPU and swscale runs on the
SoC cores. The old per-picture code would have been 4.8 % of the cached
figure, which is why this had to be fixed before the VPP number meant
anything.

Correctness did not move with the ownership change. On driver `f74ea6ad` the
seven VPP gates pass: `scale_vaapi` 8/8 byte-identical to swscale, a crop
matches `crop,scale`, all seven `transpose_vaapi` directions exact, the tone
mapper within one code of its model, `overlay_vaapi` at the documented codes,
the 32-case compositing gate, and the bilinear method against its four-tap
model.

What this does not prove: the queue is one per process, so two clients that
both stream through VPP serialise their picture submissions; the measured
client is FFmpeg's `scale_vaapi` on one stream, not a compositor mixing
several; the CDSP backend is still blocked on this tablet by the FastRPC PD
create (see the FastRPC record above), so all of this runs on the GPU.

## Kodi plays H.264 and HEVC from the installed driver (2026-11-14)

Kodi 22.0-RC1 (21.90.901, Fedora 44 package) on Wayland, driven with
`kodi --test <clip>` so it plays the file and exits. Driver installed with
`scripts/install-system.sh build-rel` from source `5ec922c` with a dirty tree
(tracked digest `227f62fa5fab0e61`), sha256 `f74ea6ad0defee68` under
`/usr/lib64/dri/`, both `msm_drv_video.so` and `qcom_drv_video.so` symlinks
present. No `LIBVA_*` override. Battery 83% discharging, no firmware reset.

| clip | session | result |
| --- | --- | --- |
| H.264 1920x1080, 300 frames | `mode=decode-order codec=H264`, 300 capture tokens, all `publish=direct` | submitted=300 completed=300 errors=0 drops=0 timeouts=0 drains=1 misplaced=0, 11.59 s |
| HEVC 1920x1080, 300 frames | `mode=decode-order codec=HEVC`, 300 capture tokens, all `publish=direct` | submitted=300 completed=300 errors=0 drops=0 timeouts=0 drains=1 misplaced=0, 11.49 s |

Both ran in real time (300 frames at 30 fps is 10 s of content, and Kodi's
own log closes `VAAPI::Close` 10.1 s after "Opening stream"). A 300-frame
decode through `LIBVA_DRIVER_NAME=qcom` and through `msm` produced the same
300/300 direct session, so the compatibility alias reaches this driver and
not libva's software fallback.

Kodi probes the video processor on startup. With VPP off it logs
`VAAPI/vpp vaCreateConfig error: the requested VAProfile is not supported
(12)` and plays the file anyway. With `V4L2_VA_VPP=1` the profile-none config
is created and the remaining message is `vaQueryVideoProcFilterCaps error:
the requested filter is not supported (32)`, which is Kodi asking for
deinterlacing filters this driver does not implement; the same clip played
300/300 direct. Neither message changes the playback result.

What this does not prove: no navigation through Kodi's UI, no audio, no
subtitles, no 4K and no interlaced source; VPP stayed on the probe path
because progressive 1080p needs no filter; this is the installed driver, but
the client was driven from the command line rather than by a person.

## Sunshine streams 1080p60 with both halves on the hardware (2026-11-14)

Sunshine 2026.914.233613 captured the DSI-1 KMS output and encoded
it; Moonlight 6.2.0 on the same tablet decoded the stream through the same
driver. The encoder probe accepted `h264_vaapi` (H.264 High, level 4.2) and
`hevc_vaapi` (HEVC Main) and refused `av1_vaapi` and HEVC Main10 with "No
usable encoding entrypoint found", which is the encoder surface this driver
advertises, not a client fault.

A 30 s session at 1920x1080, 60 fps, H.264 10 Mbps encoded 1847 pictures
between t=31.0 s and t=61.9 s of the driver trace, 8 of them IDRs, and every
`encode drain result=complete`; a second session brought the log to 2813
pictures with no encode error. The encoder ran CBR at the client's requested
7.3 Mbps, level 4.2.

Moonlight's session summary at exit:

| metric | value |
| --- | --- |
| incoming frame rate | 60.31 fps |
| decoding frame rate | 60.31 fps |
| rendering frame rate | 59.53 fps |
| host processing latency min/max/avg | 4.7 / 20.3 / 7.8 ms |
| network latency | 1 ms |
| rendering time, including vsync | 4.47 ms |

Moonlight's FFmpeg decoder selected VAAPI for the incoming stream and logged
"Direct mapping possible" on the stream surfaces, so the decode half is this
driver too.

Two operational facts for anyone repeating this. The f2fs rootfs does not
support the `security.capability` xattr: `setcap` fails with "Operation not
supported", so the packaged capability setup cannot be used and Sunshine has
to run as root; its DRM worker holds CAP_SYS_ADMIN and CAP_SYS_NICE and the
main thread logs `drop_elevated_privileges succeeded`. Pairing failed with
"Incorrect PIN" through both the stdin path and the web API until
`/root/.config/sunshine` and the Moonlight profile were removed and the
credentials re-created: stale pairing state, not the PIN submission.

What this does not prove: no gamepad or input-to-photon latency, no HEVC
session (HEVC was probed but not streamed), no AV1 encode, one display output
(DSI-1), and loopback only, so the 1 ms network latency says nothing about a
real Wi-Fi link.

## Codec sweep, Kodi, transcoding and the VPP head-to-head (2026-11-14)

Same node and boot as the pinned-clock record above: `sheng`, boot
`4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, decoder `/dev/video0` and encoder
`/dev/video1` by QUERYCAP name `iris_driver`. This driver
`f74ea6ad0defee68`; the sibling is `f7dd97109f3a9965` for decode, encode,
transcode and Kodi, and `d420cb64b2dab420` for the VPP comparison — the same
sibling sources configured with `-Dvulkan_vpp=enabled`, because the artifact
used elsewhere here had Vulkan VPP compiled out. glslang 16.2.0 was installed
on the tablet only to build that comparison artifact. FFmpeg 8.1.3, Kodi
22.0-RC1, Moonlight 6.2.0. CPU policies 0/3/7 in `performance` at
2016000/2803200/2956800 Hz, GPU `3d00000.gpu` in `userspace` at 680000000 Hz,
five interleaved rounds per cell, thermal zones 27-52 °C, battery 82-83 %
discharging, no firmware reset.

### Decode, six codecs at 1080p

300-frame `testsrc2` clips, GOP 60, about 10 Mbps: H.264 and HEVC from x264 /
x265 ultrafast with `bframes=0`, HEVC Main10 as `yuv420p10le`, VP9 realtime
with `lag-in-frames=0`, AV1 from SVT-AV1 (preset 12 maps to 10) at 8 and 10
bits. Decode is `-hwaccel vaapi -hwaccel_output_format vaapi -f null -`, so
there is no download and no rendering. utime is the client process's user
time for the 300 frames.

| codec | this driver | sibling | change | this driver utime | sibling utime |
| --- | ---: | ---: | ---: | ---: | ---: |
| H.264 | 738.2 fps | 729.6 fps | +1.2 % | 0.073 s | 0.089 s |
| HEVC Main | 554.3 fps | 547.0 fps | +1.3 % | 0.115 s | 0.150 s |
| HEVC Main10 | 490.4 fps | 502.2 fps | -2.4 % | 0.118 s | 0.140 s |
| VP9 Profile 0 | 477.1 fps | 471.8 fps | +1.1 % | 0.053 s | 0.052 s |
| AV1 Main 8-bit | 468.9 fps | 427.2 fps | +9.8 % | 0.028 s | 0.044 s |
| AV1 Main 10-bit | 411.0 fps | 368.5 fps | +11.5 % | 0.029 s | 0.125 s |

HEVC Main10 is the one cell behind, and ten further interleaved rounds
confirmed it: this driver 0.596-0.618 s against 0.584-0.602 s over 300
frames, slower in 10 of 10 rounds. `strace -c -f` over 60 frames shows the
successful work is identical (140 dequeue calls here, 141 there) but this
driver issues 219 failing `VIDIOC_DQBUF` calls against 143, and 111
`VIDIOC_DQEVENT` against 74; the extra attempts cost 0.046 s of kernel time
against 0.041 s. Its client CPU is still 16 % lower on that cell. The sync
path carries the Import and decode-order contracts, so it was left alone
rather than reshaped for 2.4 % in one profile. The obvious reshape was tried:
skipping the pre-submit `pump()` when no picture is outstanding and an output
slot is free. That build ran 0.598-0.626 s over 300 frames (mean 0.612)
against 0.596-0.618 s (mean 0.605) for the unchanged one, so the pump is
load-bearing for pipeline depth and was reverted.

Re-measured on the release build after the transcode work, 26 interleaved
pinned-clock rounds on the same clip: 0.6104 s against 0.5998 s, a paired
difference of +10.6 ms ± 6.4 ms, slower in 19 of 26 rounds. The client CPU is
still 21 % lower (0.1166 s against 0.1478 s). The gap does not grow with
`-async_depth` (1, 2, 4 and 8 all sit inside 0.59-0.63 s on both drivers), so
it is not pipeline depth. It scales weakly with stream length: three passes
over the clip (900 frames) give 1.712 s against 1.694 s. A request-level
`strace` comparison shows the same successful work in both drivers — 4 OUTPUT
and 32 CAPTURE buffers, 264 against 262 successful `DQBUF`, 132 against 133
failing `DQEVENT` — so the difference is in how the kernel and firmware
schedule the pictures, not in what either driver asks for. In the browser,
where a user meets this codec, the two are at parity: 450 frames and 0
dropped against 448 and 0.

Probed again on 2026-11-14 over the whole clip, both drivers, with the
release build tree: `DQBUF` 1204 against 1203 (EAGAIN 602 against 603),
`DQEVENT` 303 against 303, `QBUF` 621 against 621, and the same
`QUERYBUF`/`EXPBUF` shape at session setup. `poll` is entered four times in
the entire 300-frame run on both, so the earlier guess that a missed 10 ms
poll wake explained the gap is disproven — this workload does not poll per
frame. Client utime stays about 21 % lower on this driver. No userspace
difference remains to remove; the residual is firmware scheduling, and the
sync path was left alone rather than reshaped for 2 % of one profile.

### Two concurrent decode sessions

Two FFmpeg processes decoding the HEVC 1080p clip at once, five interleaved
rounds, 600 frames per run: 835.8 ms wall here (826-843) against 829.2 ms for
the sibling (819-841), a 0.8 % difference inside the round-to-round spread,
so wall time is a tie. Aggregate client utime is 0.254 s against 0.306 s,
17 % lower, and the per-session cost matches the single-session cells above.
A second resident decoder neither changes the throughput nor gives up the CPU
advantage.

AV1 is the opposite case: the 8-bit stream decodes 9.8 % faster and the
10-bit stream 11.5 % faster, and the 10-bit client CPU is 4.4x lower than the
sibling's. On this client our AV1 path publishes `direct` (13 direct, 14
drop, 6 invisible in a 30-frame trace) and its CPU per frame stays at the
8-bit cost, while the sibling's doubles at 10 bits.

### Transcode

1080p HEVC Main decoded and encoded to H.264 5 Mbps, GOP 60,
`-async_depth 4`, null muxer, five interleaved rounds: this driver 436.9 fps
on 0.121 s utime, sibling 435.0 fps on 0.157 s. Throughput is a tie; the
client CPU is 23 % lower.

### Kodi playback

`kodi --test` on each clip, three alternating rounds, child rusage for CPU.
Wall time is launch to exit, so it includes Kodi's own startup (about 2.5 s
here) rather than video duration alone.

| clip | this driver wall | sibling wall | this driver CPU | sibling CPU |
| --- | ---: | ---: | ---: | ---: |
| H.264 1080p, 10 s | 12.52 s | 12.88 s | 2.019 s | 1.982 s |
| HEVC 1080p, 10 s | 12.51 s | 12.53 s | 1.963 s | 1.977 s |
| HEVC 4K, 16 s | 18.92 s | 18.82 s | 2.668 s | 2.848 s |

Playback is at parity in wall time. CPU is within 2 % at 1080p, where Kodi's
rendering and startup dominate, and 6 % lower at 4K, where the decoder's
share is visible. A single 13.6 s sibling launch appears in the mean; that is
the spread of Kodi's own startup, not decode time.

### Streaming latency

Sunshine H.264 at 1920x1080 and 60 fps, two 22 s sessions per
driver: incoming 60.20-60.34 fps here and 60.15-60.41 fps there, rendered
59.68-59.78 against 59.63-59.71, host processing latency 6.9 and 8.1 ms
against 8.0 and 8.1 ms, no dropped frames in any session. This driver's
sessions ran with its trace enabled while the sibling's did not, so the
6.9 ms is the conservative side of that comparison.

### VPP against the sibling's Vulkan backend

Both drivers scale 1080p to 720p through their own Vulkan kernels on the same
900-frame clip with the same command, five interleaved rounds: this driver
540.0 fps (538.3-542.5) on 0.541 s utime, the sibling 535.6 fps (521.1-545.1)
on 0.619 s. Throughput is a tie inside the run-to-run spread; the client CPU
is lower in all five rounds (median 0.526 against 0.612 s).

The same two builds differ on operations the sibling's README marks
unsupported, and each was checked directly on the target:

| operation | this driver | sibling `d420cb64` |
| --- | --- | --- |
| `transpose_vaapi=dir=hflip`, 30 frames | renders 30/30 | refused, -22 |
| YUV overlay (`overlay_vaapi`, NV12 upload), 30 frames | renders 30/30 | refused, -22 |
| two chained YUV overlays, 30 frames | renders 30/30 | refused, -22 |

The sibling's support text names "no mirroring" and "YUV overlays ... are
unsupported", so those refusals are its design, not a build accident. Its
VPP does advertise crop, scale, rotation, bit-depth conversion, RGB overlays
and HDR tone mapping, which this driver also implements, with three
interpolation methods and stacked RGB overlays in addition.

What this does not prove: one synthetic clip per codec, so bitrate and scene
complexity are sampled once; the AV1 sweep is 1080p only; the sibling VPP
artifact is a local build with its documented option, not its distribution
package; Kodi CPU includes startup and rendering; streaming is loopback with
no power or long-window claim; and no client was driven through its GUI.

## Decode-to-encode without a round trip: two defects the extended gate found (2026-11-14)

`iris-encode-quality.sh` now also decodes its stream on the platform and hands
each decoded surface to the encoder in the same FFmpeg process, the shape
`ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in -c:v h264_vaapi out`
uses. That pass found two defects in the default Direct publish path. The
first: FFmpeg issues the encoder's `vaBeginPicture` before the decode
completes and never calls `vaSyncSurface` between the two, and `vaBeginPicture`
released the surface's pending decode when the surface moved to the encode
context. The picture was dropped, the encoder ran on an unpainted allocation,
and the gate scored the result 0.000 (black). The surface now carries the
decode session that still holds its pixels, and `vaEndPicture` waits on that
session before submitting.

The second defect was geometry. The encoder took the surface's size, and
FFmpeg allocates its decode surfaces at the coded size: a 1920x1080 stream
produced a 1920x1088 H.264 or HEVC stream. The pixels were correct — the
top 1080 rows scored 0.9977 against the source — but a client that reads the
geometry back gets a picture it never decoded. The encoder now codes the
picture the client declared at `vaCreateContext`, narrowed for H.264 by the
sequence's frame crop (FFmpeg's H.264 context is 1920x1088 with a bottom crop
of four chroma samples; its HEVC context is already 1920x1080). The surface
may be larger than the picture; it is still queued without a copy, and the
narrowing only stands while the encoder's raw layout matches the decoded
buffer's, so the chroma plane never moves under it.

Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1`, encoder node `/dev/video1` resolved by
QUERYCAP name `iris_driver`, render node `/dev/dri/renderD128`. Source commit
`fa048e2`; release build tree
(`LIBVA_DRIVERS_PATH=$HOME/iris-vaapi-lab/src/build-rel/src`, driver sha256
`a1b3ef16ba3a82c4...`), not an installed package. The sibling is
`radxa-pkg/qcom-va-driver` built in the same tree layout, driver
`f7dd97109f3a9965`. The gates below also ran on the `debugoptimized` tree
(`ad990dfc800e64a6...`) with the same results; the throughput table is from
the release build.

### Gates

- `iris-encode-quality.sh`, 120 frames, build tree: H.264 encode SSIM 0.9986,
  decode-to-encode SSIM 0.9977, HEVC encode 0.9968, decode-to-encode 0.9951,
  both transcoded streams 1920x1080 and 120 frames, platform decode
  byte-identical to the software decode in every case, and the Iris IRQ
  counter moved by 227 (H.264) and 224 (HEVC).
- CQP H.264 transcode of the 24 Mbps 1080p HEVC clip, `-qp 26 -g 60`: this
  driver and the sibling both 1920x1080 and byte-identical,
  md5 `ab446a44aa5ebe2ef76ff75d49bfd1f2`.
- Decode is unchanged: the same clip through this build reproduces the
  software framemd5 for 300/300 frames.
- All three publish policies agree. The same 60-frame decode-to-encode pass
  under `V4L2_VA_PUBLISH=direct`, `copy` and `import` produces 1920x1080 in
  both codecs and byte-identical output within each codec
  (`25cb0b4d7a9a2021...` H.264, `1855cf9fe078d895...` HEVC), so the fix reads
  the right buffer whichever policy published the surface.

### Pinned-clock throughput

1080p HEVC in, H.264 out, 600 frames, `-async_depth 4`, four interleaved
rounds with CPU policies at `performance` and the GPU at its maximum
userspace frequency:

| rate control | this driver | sibling |
| --- | ---: | ---: |
| `-qp 26` | 1.328 s (1.320-1.343) | 1.338 s (1.328-1.350) |
| `-b:v 5M` | 1.111 s (1.107-1.118) | 1.048 s (1.029-1.068) |

4K HEVC in, H.264 out, 300 frames, same pinning and interleaving, six rounds:
3.119 s here (3.070-3.152) against 3.161 s there (3.128-3.188), and the CQP
streams are byte-identical at 3840x2160 (36,439,584 bytes,
md5 `d74eb1767929f9473e1128f94c48622a`).

At `-b:v 5M` the two drivers do not deliver the same stream: this one emits
5,007,842 bps and the sibling 1,636,836 bps from the same request, so the
sibling encodes roughly a third of the bits and the wall times are not a
like-for-like measure. Under CQP, where both produce the same bytes, the two
are a tie: 0.7 % apart, inside the round-to-round spread.

The rate gap is a rate-control difference, not a speed one. Asked for
1,600 kbps the sibling delivers 1,631,217 bps and this driver 1,673,128 bps,
and both then score the same fidelity: SSIM 0.866634 and 0.868831 against
the source. At 5,000 kbps this driver delivers 5,007,842 bps at SSIM
0.936295, while the sibling stays at 1,636,836 bps and 0.867035 — it does not
follow the request upward. The same delivered bitrate buys the same quality
on both, so the sibling's lower `-b:v 5M` wall time is spent on fewer bits.

### What this does not prove

The gate and the measurements ran from the build tree, not from an installed
package. One clip per size. No GUI client was driven through a transcode
window. The rate-control comparison uses one clip and one target, so it shows
the sibling does not follow a raised target rather than mapping its
rate-control curve.

## Browser playback, five codecs, both drivers, Chrome 156 (2026-11-14)

Chrome 156.0.8078.79 on the target, one `iris-browser-acceptance.sh` run per
codec and driver, 15 s measured windows after a 5 s warmup. Each run launches
Chrome directly with no LIBVA variable; the driver reaches it through
`LIBVA_DRIVERS_PATH` and the `msm_drv_video.so` alias, which is how libva
resolves it from the render node. Target `sheng`, boot
`4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel `7.2.6-sm8550-g42f3b40-iris1`.
This driver `a1b3ef16ba3a82c4` (release build tree, source `fa048e2`), sibling
`f7dd97109f3a9965`. Battery 99 %, on external power, no firmware reset.

| clip | this driver | sibling |
| --- | --- | --- |
| H.264 1080p30 | `VaapiVideoDecoder`, platform, 450 frames, 0 dropped | same, 449, 0 |
| HEVC Main 1080p30 | same, 450, 0 | same, 449, 0 |
| HEVC Main10 1080p30 | same, 450, 0 | same, 448, 0 |
| VP9 Profile 0 1080p30 | same, 448, 0 | same, 448, 0 |
| AV1 Main 1080p30 | same, 448, 0 | same, 447, 0 |

Every run had a `--type=gpu-process` holding the resolved Iris decoder node.
One H.264 run on this driver recorded 2 dropped frames out of 452; three
further runs of the same cell recorded 451, 450 and 449 with 0 dropped, so
that was a first-run artifact rather than a repeatable loss.

Chrome 156's VA-API path on this build reports no `VADisplayStateSingleton`
render node when started with `--headless=new`, which is why the gate runs a
real Wayland session; that is a property of the browser's headless GPU
initialisation, not of either driver.

The acceptance gate's build-tree branch also gained the alias it needs: it
pointed `LIBVA_DRIVERS_PATH` at the build directory, which ships only
`v4l2_drv_video.so`, so every build-tree run failed at `vaInitialize` before
a picture. The gate now links `msm_drv_video.so` into a per-run directory
when the tree lacks it. Installed-package runs are unaffected.

What this does not prove: one clip per codec, 15 s windows, and the sibling
rows are a local build rather than its distribution package.

### The 4K cell

The matrix above is 1080p. The same gate on two 3840x2160 HEVC clips, one
synthetic (`hevc-4k.mp4`, 16 s, 480 frames) and one real (`jelly-4k-hevc.mp4`,
10 s, 300 frames), interleaved runs on both drivers:

| clip | this driver | sibling |
| --- | --- | --- |
| `hevc-4k.mp4`, 15 s window, 10 rounds | 446-450 frames, 0 dropped in 4 rounds, 2 or 4 in the other 6 (12 total) | 445-448 frames, 0 dropped in 9 rounds, 2 in one |
| `jelly-4k-hevc.mp4`, 15 s window, 4 rounds | 445-447 frames, 0 dropped in 3 rounds, 2 in one | 440-441 frames, 0 dropped in all 4 |
| `hevc-4k.mp4`, 8 s window, 8 rounds | 240 frames, 0 dropped in all 8 | 239-241, 0 dropped in all 8 |
| `hevc-4k.mp4` looped to 40 s, 15 s window, 8 rounds | 450-451 frames, 0 dropped in 6 rounds, 2 in two | 450-451, 0 dropped in 7 rounds, 2 in one |

The small drop counts are the renderer's, not the decoder's. A traced 15 s
run that recorded 2 dropped frames has the session ledger
`submitted=626 completed=626 errors=0 drops=0 timeouts=0 drains=1
misplaced=0 retained=3` and 626 `publish=import` records — every picture the
client asked for arrived with no error and no drop. A run that recorded 4
has 621 publishes and `va destroy_context id=2 pending=6` at teardown, so the
browser stopped consuming and closed the session with six pictures in flight.
The 8 s window, which fits inside the 16 s clip and never crosses its loop
point, records 0 on both drivers in all 8 rounds; the drops concentrate where
the window crosses the loop or the clip ends.

Publish policy does not explain the drops. Twelve further 15 s runs on the
same clip, three per policy: `auto` dropped 0, 2, 0; `import` 2, 2, 0;
`direct` 0, 2, 2; `copy` 2, 0, 0. Every run passed with 448-450 frames and a
platform decoder. A traced run that dropped nothing shows 626 captures,
626 completions, one drain at finish and no reconfiguration anywhere in the
window, so the loop crossing does not produce a driver-level event at all.

The same traced cell names one real difference. The sibling copies every
picture through the GPU — 621 `surface copy mode=gpu` records for the run,
mean 4387 us, 2.72 s of copy time — because Chrome exports its surfaces and
the sibling has no import path. This driver published 626 `publish=import`
records with no copy. The copy paces delivery one picture at a time; it is a
plausible reason the sibling records fewer boundary drops at 4K, but that is
a hypothesis, not a measurement, and the copy is also 2.72 s of GPU work
the import path does not do.

What this does not prove: two clips, 15 s and 8 s windows, and a drop count
is a renderer statistic — this record says nothing about which layer would
drop under a compositor load it did not see.

### CPU cost of the same playback

Frame counts and drop counts do not say what a decode costs the machine.
Three interleaved rounds of HEVC, AV1 and VP9 1080p playback, this driver and
the sibling alternating within each round, 15 s windows, same gate and same
`LIBVA_DRIVERS_PATH` mechanism as above. The figure is CPU time summed over
every process in the run's Chrome profile, divided by the window's wall time.
Samples are per-pid high-water marks; a first-minus-last sum goes negative
once Chrome's helper processes exit and their ticks leave the total.

| clip | this driver | sibling |
| --- | ---: | ---: |
| H.264 1080p30 | 34.1 % | 35.0 % |
| AV1 1080p30 | 35.4 % | 34.4 % |
| VP9 1080p30 | 33.3 % | 33.6 % |

Means of three rounds, one core as 100 %. Every cell is within a percentage
point of its counterpart, and the sign of the difference flips between
codecs, so this resolves no gap either way. It also does not contradict the
decoder-layer measurement: this figure is the whole browser — renderer,
compositor, GPU process and decoder — over video that the compositor works on
regardless of which driver decoded it, so a decode-layer difference of a few
percent is diluted below the noise here.

What this does not prove: one 15 s clip per codec, three rounds, CPU time
only. Power draw was not measured; the tablet was on external power at 99 %
during these runs, and a power comparison needs it unplugged.

## AV1 correctness: the frames the sibling replaces with neutral gray (2026-11-14)

This driver decodes AV1 byte-exactly against libdav1d on every clip measured.
The sibling does not: on all six AV1 clips here it substitutes flat neutral
gray for a fixed set of frames and reports success, so a client sees a correct
frame count, no dropped frames and a picture that is wrong.

Same node, boot and module as the records above: `sheng`, boot
`4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1` (`qcom_iris` `b90bd823fdab9895`), decoder
resolved by QUERYCAP name `iris_driver`. This driver
`a1b3ef16ba3a82c4` (release build tree), sibling `f7dd97109f3a9965` (source
commit `0980a1d`). FFmpeg 8.1.3, battery 98 % charging, thermal zone
37100 millic, no firmware reset.

### The measurement

Every run decodes through FFmpeg's VA-API decoder, downloads the surfaces as
NV12, converts to `yuv420p` (or `yuv420p10le` from P010) and writes
`framemd5`. The baseline is libdav1d over the same file with no hardware in
the path; a driver that mangles output cannot bless its own result.

| clip | frames | this driver | sibling |
| --- | ---: | --- | --- |
| `av1-8-1080p.mkv` (SVT-AV1, 8-bit) | 300 | 0 mismatched | 65 mismatched |
| `av1-10-1080p.mkv` (SVT-AV1, 10-bit) | 300 | 0 mismatched | 65 mismatched |
| `av1-720.webm` | 120 | 0 mismatched | 42 mismatched |
| `resize-av1.mkv` (dynamic resolution) | 80 | 0 mismatched | 62 mismatched |
| `test-av1.webm` (matrix clip) | 48 | 0 mismatched | 40 mismatched |
| `av1-4k.mkv` (3840x2160) | 30 | 0 mismatched | 1 mismatched |

The 1080p result repeats exactly: six interleaved rounds of the 8-bit clip
gave this driver 0 mismatched and 0 replacement records in every round, the
sibling 65 and 65 in every round. It does not depend on the publish policy —
`direct`, `copy` and `import` each gave this driver 300/300 exact and the
sibling 65 mismatched. It is not an FFmpeg artifact: `vaav1dec` through
GStreamer, remuxed so `GstVideoMeta` plane offsets are honoured, gave this
driver 300 frames with 0 mismatched and no replacement record, and the
sibling 300 frames with 65 mismatched and 65 records. The 10-bit clip was
re-run on its own with the replacement count read from the log rather than
inferred from the hashes: 65 records, 65 mismatched frames.

Whole-clip PSNR against libdav1d, computed over a pipe with no intermediate
file: this driver `y:inf u:inf v:inf`, the sibling `y:19.85 u:16.85
v:16.41 average:18.49 min:11.78`.

### What the replacement is

Sampled frames confirm the content, not just the hashes. Frame 33, software
decode: luma `min 19 / avg 126.242 / max 226`. Same frame, this driver:
identical statistics and a byte-identical frame. Same frame, sibling:
`min 128 / avg 128 / max 128` — every luma sample at the midpoint, which is
what `fill_neutral_surface()` writes (`memset` 128 for NV12, `512 << 6` for
P010). Frames 36, 59, 93, 153, 273 and 299 read the same way. None of the
five keyframes (0, 60, 120, 180, 240) is in the mismatched set; frames 60 and
120 were compared directly and are byte-identical to libdav1d on both drivers.

The affected frame indices are `33 34 36 37 41 42 45 49 50 52 53 57 59`, then
`93 94 96 97 101 102 105 109 110 112 113 117 119`, and the same 13 offsets in
each of the last three GOPs: 65 frames at five identical GOP-relative
positions. A defect that tracks the GOP structure exactly is not damage from
the bitstream or the firmware; it is a userspace decision applied to a
picture class.

### Why it happens

Our trace of the same clip, same node, names the mechanism. An AV1 access
unit that installs a reference produces two CAPTURE completions under one
token: an empty one with `flags=0x4041` (ERROR, timestamp-copy, mapped; no
picture-type bit, `bytesused=0`) and the real picture later with `flags=0x4011`
(PFRAME, payload). This driver consumes the empty completion as the install
half — `capture token=36 index=2 flags=0x4041 publish=invisible` — and
publishes the payload completion that follows. The run's ledger is
`submitted=300 completed=300 errors=0 drops=0 timeouts=0 misplaced=0`, with
65 `publish=invisible` records and 300 `publish=direct` records.

The sibling treats the same empty completion as a damaged picture. Its trace
shows `CAPTURE index=2 used=0 offset=0 ts=0:36 flags=4041`, then
`replaced damaged picture token=36 with neutral gray`, then
`CAPTURE index=10 used=3133440 offset=0 ts=0:36 flags=4011` — a full decoded
picture, 3,133,440 bytes — and finally `discarding completed token=36`. It
allocates a fresh surface, fills it gray, completes the client's surface with
that, and throws the real picture away. Across the clip: 300 submissions, 365
CAPTURE returns (300 with payload, 65 empty), 65 replacements and 65
discards.

The code is `recover_picture()` in the sibling's `src/decoder.cpp:258`, called
from both the OUTPUT and CAPTURE paths (`:327`, `:348`) on any buffer with
`V4L2_BUF_FLAG_ERROR`; `fill_neutral_surface()` is at `src/surface-copy.cpp:91`.
Of the 65 replaced pictures, 40 had `original-show=1` and 25
`original-show=0` — it replaces pictures the client was going to display, not
only internal ones.

This is also why `packaging/kernel/0007-*` was measured and rejected: removing
the false ERROR from the completion fixes the sibling's symptom, but this
driver already decodes these pictures exactly with the error present, so the
patch buys nothing the qualified module does not do.

The browser does not detect this. Chrome reported 448 frames and 0 dropped
for the sibling on the same clip, as it did for this driver, because the
acceptance gate counts media-pipeline frames and drops, not pixels. A user
watching AV1 sees 13 gray frames in every 60-frame GOP and the browser
reports a clean playback.

### What this does not prove

Six clips, 8- and 10-bit, 720p to 4K, all AV1. The 4K clip shows one
replacement in 30 frames, so the rate per frame is not the same as at 1080p
and this record does not characterise it. The sibling is a local build
of commit `0980a1d`; a later revision may have changed `recover_picture()`, so
this is a statement about the measured artifact, not about the project's
present state. No AV1 clip with a real bitstream error was in the set, so
nothing here says how either driver behaves on genuinely corrupt input.

## Whole-device power per delivered frame, both clock states (2026-11-14)

The earlier power records here compare this driver with software decode
(E2). This one compares the two hardware drivers, unplugged, screen off.
Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1` (`qcom_iris` `b90bd823fdab9895`, patches
0001-0004), decoder resolved by QUERYCAP name `iris_driver`. This driver
`a1b3ef16ba3a82c4` (release build tree, source `fa048e2`), sibling
`f7dd97109f3a9965` (source `0980a1d`). FFmpeg 8.1.3, clip `hevc-4k.mp4`
(3840x2160, 480 frames) decoded in an endless loop with
`-hwaccel vaapi -hwaccel_output_format vaapi -f null -`; the process is
killed after the window. Battery 89 % to 87 %, discharging, backlight 0 for
every window and restored afterwards, peak thermal 42.2 C, no reset.

The gauge is the whole device: `|current_now| x voltage_now`, 200-ish
samples per window. Frame counts come from ffmpeg's `-progress` file, so
energy is reported per delivered frame. That matters here: the two drivers
do not decode at the same rate under the default governor, and a fixed
window of saturated decode is therefore not the same amount of work.

Runs are ABBA (this driver, sibling, sibling, this driver), two independent
runs per clock state, 45 s of steady state after a 5 s ramp. Idle baselines
were measured separately in the same clock states, screen off, 45 s each:
3.289 W pinned, 2.994 W with the default governor.

| 4K HEVC decode | this driver | sibling |
| --- | ---: | ---: |
| `performance` governor + GPU at max, throughput | 385.2 / 385.6 fps | 382.8 / 386.8 fps |
| `performance` governor + GPU at max, whole device | 5.161 / 5.206 W | 5.289 / 5.378 W |
| `performance` governor + GPU at max, decrement per frame | 4.86 / 4.97 mJ | 5.22 / 5.40 mJ |
| default `schedutil`, throughput | 354.0 / 353.8 fps | 269.7 / 267.2 fps |
| default `schedutil`, whole device | 4.243 / 4.281 W | 4.022 / 4.003 W |
| default `schedutil`, decrement per frame | 3.53 / 3.64 mJ | 3.81 / 3.78 mJ |
| default `schedutil`, whole-device energy per frame | 11.99 / 12.10 mJ | 14.91 / 14.98 mJ |
| default `schedutil`, client CPU per frame | 3001 / 3015 us | 3695 / 3695 us |

With the clocks pinned the two decode at the same rate and this driver
spends 7.5 % less per frame on its own share of the power. Under the
governor a user actually gets, this driver delivers 32 % more frames per
second; the fixed-window wattage reads 6 % higher for it because it is
doing 32 % more work, while energy per delivered frame is 20 % lower
(11.99 mJ against 14.91 mJ) and the baseline-subtracted decrement is 5.6 %
lower. The client CPU column agrees with the CPU records above.

A saturation comparison by wattage alone points the wrong way when the
work rate differs, and an earlier draft of this page read the fixed-window
numbers as a deficit. It is not one: per delivered frame this driver is
ahead in both clock states, and the gap is larger in the state users run.

### In the browser

Chrome 156 playing the same clip at display rate, screen off, battery 85 %,
ABBA, two rounds per driver, 12 s of sampling inside each run's 15 s measured
window after warm-up. Both arms decoded 447-450 frames with 0 dropped, so
the work rate is identical.

| 4K HEVC in Chrome | this driver | sibling |
| --- | ---: | ---: |
| whole device | 4.471 / 4.452 W | 4.553 / 4.614 W |
| decrement over the 3.098 W idle baseline | 1.373 / 1.354 W | 1.455 / 1.516 W |

This driver's decode-plus-compositor increment is 8.2 % lower. The traced
cell above gives the likely reason and it is visible in the work: the
sibling copies every 4K picture through the GPU (mean 4.4 ms) because Chrome
exports its surfaces and it has no import path, while this driver publishes
the client's own buffer. This is a per-delivered-frame comparison, not a
saturation one.

What this does not prove: one 4K clip, decode only in the saturation part and
browser playback in the table above — no encode, no standby, no Firefox; the
gauge measures the whole device, so these are increments, not
decoder-package power; the idle baselines were taken after the decode runs
rather than inside each round; two rounds per state, and the 1080p clip was
not run in this shape. The browser sample covers 12 s of a 15 s window, so
start-up and teardown are outside it.

## The branch head as an installed package (2026-11-14)

Every record above ran from a build tree through `LIBVA_DRIVERS_PATH`. This
one installs the branch head and runs the same gates with no `LIBVA_*`
variable in the environment, so libva resolves the driver from the render
node and the `msm_drv_video.so` alias.

- source commit `b9d6a4b430f18d198e69276f3f5eca1f8a24c48c`, `source_dirty=0`,
  tracked digest `0356ad123936b264c74760ee83d2c6ae12eb5f5eb20e41031ae4bb098994b128`
  (the code is `fa048e2`; every commit since is documentation)
- artifact `a1b3ef16ba3a82c44c02920981d25bbb551144074f360adf55cd8d509457b61f`,
  747,544 bytes, installed at `/usr/lib64/dri/v4l2_drv_video.so` with
  `msm_drv_video.so` and `qcom_drv_video.so` pointing at it; manifest at
  `/usr/local/share/iris-vaapi/install-manifest.txt`; previous driver
  `f74ea6ad0defee68` kept in the manifest's backup path
- target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
  `7.2.6-sm8550-g42f3b40-iris1`, module `b90bd823fdab9895` (patches
  0001-0004), no firmware reset

| gate | result |
| --- | --- |
| `iris-matrix.sh` | PASS H.264, HEVC, VP9 and AV1 content against the native baselines; AV1 ledger `submitted=48 capture=48 drops=0 timeouts=0` |
| `iris-encode-quality.sh` | H.264 and HEVC, 120 frames; HEVC SSIM mean 0.996808, worst 0.9963; decode-to-encode 0.995067; blank stream 0.000034 below the 0.90 floor |
| `iris-gstreamer-gate.sh` | `driver=a1b3ef16ba3a82c4 source=installed`, `decode frames sw=300 gst=300 identical=yes`, `vah264enc` 300/300 at 6,462,414 bytes, `vah265enc` 300/300 at 20,941,657 bytes |
| browser, 4K HEVC | `VaapiVideoDecoder`, platform, 449 frames, 0 dropped, `driver=installed:a1b3ef16ba3a` |
| browser, AV1 1080p | same decoder and platform, 448 frames, 1 dropped (one renderer drop in that run; the five-codec matrix recorded 0) |

The installed bytes are the same artifact the build-tree records used, so
those records now describe what the system actually serves; the only
difference is the source commit's documentation content. This closes the
"installed package at the branch head" gap.

What this does not prove: one run per gate, and the power campaign was not
re-run on the installed copy — it is the same sha256, so it would measure the
same bytes. `iris-matrix.sh` skips MPEG-2 and VP8 because the device does not
advertise those formats. The AV1 browser run recorded one drop; a drop count
is a renderer statistic, as the 4K cell above explains.

## The VP9 intra-only vector is a decoder-node boundary (2026-11-14)

Target `sheng`, boot `4b73926c-a774-4ae4-8968-d0a26b20a9c3`, kernel
`7.2.6-sm8550-g42f3b40-iris1` (`qcom_iris` `b90bd823fdab9895`, patches
0001-0004), decoder node `/dev/video0` by QUERYCAP name `iris_driver`.
Vector `vp90-2-16-intra-only.webm` from the fluster VP9 corpus, FFmpeg 8.1.3.
Four clients, same file:

| client | result |
| --- | --- |
| `libvpx-vp9` (software) | 7 frames, 1 064 448 bytes of NV12 |
| `vp9_v4l2m2m` (kernel via FFmpeg) | `frame=0`, exit 0 |
| this driver `a1b3ef16` | `no SOURCE_CHANGE after the first access unit`, 5 s per access unit |
| sibling `f7dd9710` rebuilt from `radxa-v111` | same `endPicture` failure, VA status 38 |
| raw V4L2 probe, no VA driver | all 7 packets queued, CAPTURE streaming, 8 CAPTURE buffers queued: no event, 0 captures in 2 s |

The probe is a 120-line scratch program that opens the node, sets the
bitstream and capture formats, streams both queues, queues the demuxed
packets, subscribes to `V4L2_EVENT_SOURCE_CHANGE`, and polls. The same probe
on a one-second keyframed VP9 clip raises the event (`changes=1`) and the
three clients decode 30/30, so the negative result is not the probe failing
to subscribe. The control also rules out a missing capture queue: queueing
capture buffers does not change the result.

Why the stream is outside the node's contract: its first packet is a
117 963-byte superframe of four frames (30 299 / 35 391 / 40 875 / 11 384
bytes plus a 14-byte index). The first three sub-frames have header
`84 c9 …` — inter, `show_frame=0`, intra-only — and the fourth `86 02 …` —
inter, `show_frame=1`. There is no key frame in the stream. The firmware
therefore never sends `HFI_CMD_SETTINGS_CHANGE` on the bitstream port, and
`iris_hfi_gen2_handle_src_change` has nothing to forward. Submitting the
sub-frames raw, without this driver's invisible-frame wrapper, changes
nothing; so does configuring CAPTURE without waiting for the event.

The earlier entry in the VP9 corpus section measured this same silent
failure, not a decode: `vp9_v4l2m2m` exits in about 0.2 s with `frame=0`.
No userspace change is proposed. Rebuilding a key-frame header around an
intra-only frame would be compensating complexity around a firmware parser,
for a stream shape that carries no key frame at all.

What this does not prove: one vector, one kernel build, one sibling build
(`radxa-v111` with `cdsp_vpp` and `vulkan_vpp` disabled; the decode path is
unaffected). A different firmware or a kernel change could move the
boundary. The probe hardcodes `/dev/video0` and lives in scratch space, not
in the repository.

## 4K HEVC encode at pinned clocks (2026-11-14)

The earlier 4K encode cell (1.80-2.16 s) was unpinned and mixed both drivers.
This one pins the cores in `performance` at max frequency and the GPU in
`userspace` at 680 MHz, renders 30 NV12 frames of `testsrc2` at 3840x2160
once to a raw file, loops that file to 300 frames, uploads with `hwupload`,
and encodes HEVC with `-async_depth 4` to the null muxer. Five ABBA rounds,
wall clock around ffmpeg.

| round | this driver (ms) | sibling (ms) |
| --- | ---: | ---: |
| 1 | 1636 | 1662 |
| 2 | 1634 | 1684 |
| 3 | 1635 | 1684 |
| 4 | 1644 | 1630 |
| 5 | 1658 | 1635 |

This driver 1634-1658 ms (mean 1641), sibling 1630-1684 ms (mean 1659);
paired mean difference −18 ms, this driver faster in three of five rounds.
The verdict is parity: the spread is larger than the difference, and the
source is already NV12 so no conversion sits on either side.

What this does not prove: one source, HEVC only, wall time includes the
null muxer and the raw-file loop; no bitrate or quality cell here.

## One-frame encode, ten interleaved rounds (2026-11-14)

The earlier first-frame cell was two samples per driver (0.014-0.036 s
against 0.013-0.019 s). This one is ten ABBA rounds of a one-frame 720p HEVC
encode, `-benchmark` rtime from ffmpeg (process and filter setup included):

| | this driver | sibling |
| --- | ---: | ---: |
| rtime samples (ms) | 26, 25, 21, 16, 16, 24, 23, 21, 24, 21 | 21, 19, 24, 15, 17, 15, 23, 22, 21, 20 |
| mean | 21.7 | 19.7 |
| range | 16-26 | 15-24 |

Paired mean +2.0 ms for this driver; it is slower in six rounds, faster in
three and tied in one. The two-sample range overstated the tail: with ten
rounds the distributions overlap and the residual is about 2 ms of a
measurement that includes roughly 15 ms of process and filter setup that is
not driver work.

What this does not prove: no per-phase counters were added, so this does not
locate the 2 ms; `rtime` also includes the software `format=nv12`
conversion feeding `hwupload`.
