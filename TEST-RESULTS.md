# Qualcomm Iris VA-API Test Results

Primary latest documented target: Fedora 44 ARM64 Snapdragon SM8550 tablet,
kernel `7.2.0-sm8550`. Device numbers are historical and must be resolved
through `iris_resolve_device`; target labels below distinguish separate boots
and replacement endpoints without exposing private network addresses.

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
The clips are `/tmp/iris-enc/media/clip-{hevc,h264}.mp4`: 1920x1080, 900
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
`~/Lab/iris-vaapi-lab/artifacts/va-matrix`:

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

`~/Lab/kernel-iris-syncframe/` on the target holds the stock module
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

`~/Lab/iris-vaapi-lab/artifacts/av1-grain-roundtrip/` on the target:
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
- Installed with `~/Lab/kernel-iris-syncframe/install.sh`; the previous
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
built on the target (`/tmp/piano-bench/build`, sha256 `798cfd62a91c…`; the
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

On the target: `~/iris-vaapi-lab/artifacts/browser-acceptance/` (one
directory per row above, each with `summary.json`, `cdp.json`,
`browser.log` and `iris-node-holders.txt`),
`/tmp/iris-bench/lab*/artifacts/browser-acceptance/` for the rows that
were run under a scratch lab root, `/tmp/iris-bench/av110-va.md5` and
`av110-sw.md5` for the frame MD5s, and
`~/iris-vaapi-lab/media/10bit/av1-640x360-10bit.mp4` for the clip.

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

Artifact: `/tmp/iris-bench/vp9-va.md` on the target (per-vector marks), plus
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

Artifacts: `/tmp/iris-bench/lab-import/{h264,sintel-1080p30-hevc-b3,
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

Scratch artifacts: `~/Lab/Bridge/tmp/iris-bench/` on the target
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

Artifacts: `~/Lab/Bridge/tmp/iris-bench/av1-720/` on the target
(`hw-all.md5`, `sw-nv12.md5`, `hw1080.md5`, `sw1080.md5`, `rt-real/`,
`browser-av1/`), and
`~/Lab/Bridge/tmp/av1-hdr/` on the builder (the dumped access units, the
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
`~/Lab/Bridge/tmp/iris-bench/av1-720/` (`out-a.log` success, `out-b.log`
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

Artifacts on the target: `~/Lab/Bridge/tmp/iris-bench/av1-720/`
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
`~/Lab/qualcomm-iris-vaapi/build-def/src`:

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

Artifacts: `~/Lab/Bridge/tmp/iris-bench/av1-720/` — `f-*.log` and `v-*.log`
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
(`~/Lab/Bridge/tmp/fluster/h264-failures.csv`, and the same method for HEVC):

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
working-tree change (copied to the target's `~/Lab/qualcomm-iris-vaapi`,
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
is backed up at `~/Lab/Bridge/tmp/rollback-driver-20261024T090903Z/` with its
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
`V4L2_PIX_FMT_VP9` (`~/Lab/Bridge/tmp/vp9-import-probe` on the target, not
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

Artifacts: `~/Lab/Bridge/tmp/vp9probe/` on the target (`trace720-*.log`,
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
`~/Lab/iris-vaapi-lab/src` built from `feat/encode-entrypoints`. Two boots
were used because the tablet reset itself mid-run; the corpus result is the
same on both (see the reboot note at the end). Node `/dev/video17` on the
first boot, `/dev/video0` on the second; the node is resolved by the
`Iris Decoder` QUERYCAP name in every command, never by number.

### The corpus, three decoders

`fluster run -ts VP9-TEST-VECTORS`, `-j 4 -t 30`, per-vector CSV
(`~/Lab/Bridge/tmp/vp9-fl/run/vp9-{sw,va,va2}.csv`). The software arm is the
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
- **`vp90-2-16-intra-only` is the one family where this driver fails and the
  native decoder does not.** FFmpeg's own parse of the vector yields a
  117 963-byte key frame followed by frames of 25 801, 1, 1, 1, 21 400 and
  10 293 bytes, and the first frame's header bytes are
  `84 c9 30 68 40 20 2b …`. Through the driver the first picture is
  submitted with `bytes=30306 seq_start=0`, the kernel raises no
  SOURCE_CHANGE, and `endPicture` returns `no SOURCE_CHANGE after the first
  access unit`; it then retries the same 5 s wait for each following frame.
  The native decoder returns 7 frames in 207 ms. One family, one vector, not
  yet root-caused; the trace is `~/Lab/Bridge/tmp/dec-perf/p16i.trace` on the
  target.
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

Artifacts on the target: `~/Lab/Bridge/tmp/vp9-fl/run/` (CSV and JSON for
every arm), `~/Lab/Bridge/tmp/dec-perf/` (probes `probe8.sh` through
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
  `~/Lab/Bridge/tmp/sheng-fedora-packages/...` was a second fallback
  candidate. It pins `8dd655e…` with `phxinyang/` URLs and produced four
  false URL-pin failures. Only the explicit `IRIS_DOWNSTREAM_SPEC` override
  and the sibling `../sheng-fedora-packages` checkout remain.
- A spec with zero `Source6-9` lines now fails instead of silently passing.
  `packaging/kernel/README.md` documents the same resolution rules.

Verification of that check, fixtures under `~/Lab/Bridge/tmp/iris-check-fixtures`:

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
set to the project's `~/Lab/Bridge/tmp`:

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
`~/Lab/Bridge/tmp/ff-final-20261110/` on the target. Firefox was launched on
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
`~/Lab/Bridge/tmp/rollback-3-20261110T182904Z/v4l2_drv_video.so` (previous
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
`~/Lab/Bridge/tmp` and `V4L2_VA_PUBLISH=import`. Artifacts:
`~/Lab/Bridge/tmp/gates5-20261110T182908Z/`. All three guards ended
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
| Chrome local 1280x720 AV1 clip | AV1 | 287 / 0 | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true`, GPU process holds `/dev/video17` | driver `installed:e97a861f8b66...`; see `~/Lab/Bridge/tmp/browser-clean-final/chrome-local` |
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

Artifacts: `~/Lab/Bridge/tmp/gates5-20261110T182908Z/`,
`~/Lab/Bridge/tmp/ff-final-20261110/ff-av1-clean-1/`,
`ff-av1-clean-2/`, `ff-av1-clean-3/`, `ff-clean-youtube/`,
`ff-clean-bilibili/`, `ff-clean-youtube-noav1/`,
`~/Lab/Bridge/tmp/site-final-20261110/final-clean-chrome-youtube/`,
`final-clean-chrome-bilibili/`,
`~/Lab/Bridge/tmp/browser-clean-final/`.

## Complete AV1 browser matrix (2026-11-11)

The one-off browser probes above are now a repo-owned, repeatable matrix:
`test/iris-av1-browser-matrix.sh` with `test/iris-firefox-media.py`
(WebDriver BiDi + MOZ_LOG), `test/iris-chrome-media.py` (CDP Media domain)
and a generated corpus (`~/Lab/Bridge/tmp/av1-browser-corpus/`). Each case
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
`/home/xinyang/Lab/Bridge/tmp/rollback-final-20261111/v4l2_drv_video.so`
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
used `IRIS_EXPERIMENT_DIR` under `~/Lab/Bridge/tmp/gates-final2-20261111` with
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
`~/Lab/Bridge/tmp/av1-syntax-corpus/a11-guard`:
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
`~/Lab/kernel-iris-syncframe/Module.symvers`, whose `stock.sha256` check
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

A rollback copy under `~/Lab/Bridge/tmp/wp1-verify/out/` was compared
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
| Rollback | `~/Lab/Bridge/tmp/wp1-verify/out/rollback-qcom-iris.ko.zst`, `sha256sum -c` verified before install; restore script `~/Lab/Bridge/tmp/wp1-rollback.sh` was not run |

Before installing, `~/Lab/Bridge/tmp/wp1-install.sh` re-checked the rollback
copy, copied the built module, ran `depmod -a` and rebooted. The reboot changes
the boot ID by design; both IDs are recorded above.

**Baseline reproduction attempt on 0001-0004** (boot
`7a26df99-f7ac-4148-9c80-135ba3adc661`): six runs of the retained 1280x720
480-frame alt-ref VP9 clip, all `rc=0 frames=480 last=1 epipe=0 errors=0
drops=0` with `session finish submitted=515 completed=515 errors=0 drops=0
timeouts=0 drains=1 misplaced=0`. All six framemd5 files are byte-identical
(md5 `653d13e6913e9dec741f77116509b376`) and are retained under
`~/Lab/Bridge/tmp/wp1-baseline-0001-0004/`. A raw V4L2 slow-CAPTURE probe
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

**Final guarded run**: `~/Lab/Bridge/tmp/wp1-verify/guard-20261111T193559Z`,
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
remote root (`~/Lab/Bridge/tmp/deploy-probe-target`, since removed):

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
`~/Lab/Bridge/tmp/fix1-tree` on the target, kernel
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

Artifacts on the target: `~/Lab/Bridge/tmp/vp9final-{matrix,ffmpeg,chrome}`,
`~/Lab/Bridge/tmp/vp9retain-{b,c,d}`, `~/Lab/Bridge/tmp/vp9copy-{a,b}`.

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
(0001-0004 + 0006 + 0005, built with `packaging/kernel/build-module.sh`),
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

After the rollback copy was written back (`rollback_to_module=b90bd823...`
verified on disk) the tablet did not return on its network, so the control arm
ran on the next boot instead of the same one: boot
`b2f1de2f-b3da-4e24-9fa4-163e489d929f`, module
`b90bd823fdab98957739ad2196eb4016f5a264f2280d22735963e40c8488c715`
(0001-0004 + 0006, exactly the module the 0005 build added to), installed
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

The two arms differ in one variable, the kernel module. Same clips, same
client command, same installed driver SHA-256, same node resolution, and the
control reproduces the qualified counts exactly. The 0005 arm completed one to
three pictures per clip; the control completed every submitted picture with
bit-exact output.

No codec, profile or publish policy is advertised or withdrawn by this run.
The qualified baseline module stays `b90bd823` (0001-0004 + 0006) with the
shipped driver unchanged, and 0005 stays out of every module this project
builds.
