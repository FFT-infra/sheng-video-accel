# Hardware evidence

Current evidence for the qualified combination in `README.md`. The
machine-readable capability boundary is `data/iris-codec-capabilities.json`;
this file records the runs behind it. Superseded baselines, refuted hypotheses
and interrupted experiments are not kept here — they live in git history.

Unless a section names a different combination, every target result below is
from `test/remote/gate.sh` with
`guard=complete exit=0`, on boot `fd0b90f4-d968-4340-94f9-f2fd7bc6c52a`,
kernel `7.3.4-sm8550-00104-g87ac0f2347eb`, module
`8f771ada9bc16711465e2f56dcd1fee82d979edcceb5ce985a30ecc99f79dc75`, decoder
`/dev/video17` and encoder `/dev/video18` resolved by `iris_driver` QUERYCAP
name. The installed driver is
`/usr/lib64/dri/v4l2_drv_video.so`, hash
`35cdab5561db2888a74bcd6dddcdff98bc74f45841061f08330ddb446d63d591`, built from
source `aa375a3` with `source_dirty=0` and installed live with
`scripts/install-system.sh` (not a `DESTDIR` staging run). Runs that name a
build-tree override say so.

## Host verification

`sh test/run-static-checks.sh` PASS, `meson test -C build-local` 34/34,
`meson test -C build-asan` 34/34, `git diff --check` clean. Both CI jobs
(`build-and-check`, `sanitizers`) are green on the pushed head. The VPP
shaders, kernel patch hashes, media pins, install manifest and source-archive
boundary are each pinned by a host check.

`test/remote/gate.sh` refuses a run whose guarded child printed nothing
(exit 5); `test/iris-gate-output-self-test.sh` pins the empty, missing,
opt-out and real-output cases on the host and is red on the old behaviour.

## Decode: pixel oracles

`test/iris-matrix.sh`: H.264, HEVC, VP9 and AV1, 48/48 frames each with exact
framemd5 against a software decode, `submitted=48 capture=48 drops=0
timeouts=0`, AV1 `noshow=23`. This is the authoritative positive VA-API pixel
oracle.

| Gate | Result |
| --- | --- |
| `iris-structure-matrix.sh` | H.264 reference structures (all-I, IP-only, B=2, B=4), bit-exact MD5 |
| `iris-real-clip-gate.sh` plain | AV1 720p, 120/120 exact, `import=120 copy=0 invisible=15`, `errors=0 drops=0` |
| `iris-real-clip-gate.sh` drc | in-context 1280x720 -> 854x480, 80/80 exact, `import=40 copy=40 invisible=1`; the GPU-copy half is the expected fail-safe for a client exporting surfaces at the old geometry |
| `iris-real-clip-gate.sh` 4k | AV1 3840x2160, 30/30 exact, `import=30 copy=0` |
| `iris-vp9-p2-gate.sh` | generated 640x360 `yuv420p10le` Profile 2, 30/30 exact, 12 P010 surfaces, 0 NV12, `import=30`, `VAEntrypointVLD` answers `VA_RT_FORMAT_YUV420_10` only |
| `iris-av1-grain-gate.sh` | 8-bit NV12 and 10-bit P010 clips, every frame byte-identical to libdav1d's grained output under Import, direct and copy |
| `iris-dynamic-resolution.sh` | VP9 Import, 100 alternating 640x360/1280x720 switches in both single- and multi-process lanes, 202 frames over 101 contexts, pixels equal to software, `drops=0 timeouts=0` |
| `iris-gstreamer-gate.sh` | `GST_VA_ALL_DRIVERS=1` registers 7 elements; `vaav1dec` 120/120 hashes equal to software; `vah264enc`/`vah265enc` encode 60 frames that decode back |

Import keeps one client buffer in flight and never skips a submitted picture.
The VP9 alt-ref clip (`lag720.webm`, 480 frames) reports
`submitted=515 completed=515 errors=0 drops=0 misplaced=0` with framemd5
equal to software. Chrome 156 reported `VaapiVideoDecoder` with
`kIsPlatformVideoDecoder=true` and `submitted=1705 completed=1705 drops=0
retained=1161` in one guarded window and `1588/1588 drops=0` in another;
Firefox 158 reports hardware AV1 on local and live YouTube/Bilibili streams.
A browser decode counts as hardware only when a media surface reports the
selected decoder with frame and drop evidence.

At 7680x4320, `test/iris-8k-gate.sh` decodes one finite clip per qualified
codec with a software framemd5 oracle: 4/4 exact frames each, granted CAPTURE
pool 8 under the 384 MiB memory budget, `errors=0 last=1 drain=1`. Import at
8K gives `mem=import pool=8 frames=4 md5=exact err=0` for H.264 and HEVC;
`direct`, `import` and `copy` decode the same four frames to the same hashes.
`test/iris-two-player-gate.sh` runs two independent 1920x1080 H.264 clients
with their own VA display and trace, a and b 90/90 exact, `errors=0 last=1`.
Both gates are short exact-pixel checks, not throughput or thermal results.

VP9 12-bit and AV1 12-bit are refused at profile selection; 4:4:4 and
large-scale tile are outside the advertised profiles or cleanly refused by
the translators. The capability table names each rejection.

## Conformance corpora

Fluster through the FFmpeg VA-API wrapper, on the 7.2.6 combination
(`42f3b40c702a` source, driver `f155e2f5…`), which remains qualified:

| Suite | Passed | Total | Profile split |
| --- | ---: | ---: | --- |
| `JVT-AVC_V1` | 72 | 135 | Constrained Baseline 33/33, Main 39/89, Baseline 0/7, Extended 0/6 |
| `JCT-VC-HEVC_V1` | 141 | 147 | Main 131/135, Main 10 9/11, Main Still Picture 1/1 |

Every failure was classified by running the vector directly and reading the
kernel's own message. H.264, 63 failures: 49 are refused by the kernel
(`unsupported content, bit depth: 80008, pic_struct = N`), all with
`frame_mbs_only_flag = 0` — the firmware has no field/MBAFF decoding; 7
Baseline and 6 Extended vectors never reach the hardware because those
profiles are not advertised; 6 more are Baseline/Extended pixel differences on
an unadvertised profile; `FM2_SVA_C` has no readable elementary stream to
probe. The passing Constrained Baseline and Main vectors are frame-coded
streams.

HEVC, 6 failures: two `PICSIZE_A/B_Bossen_1` vectors are 8440 wide, above the
node's 8192 limit; two `WPP_D_ericsson_MAIN_2`/`_MAIN10_2` are 64 wide, below
the 96 minimum; `VPSSPSPPS_A_MainConcept_1` switches 352x288 -> 1280x720 in one
stream and differs on its reference MD5 in the in-context resolution-change
family; `TSUNEQBD_A_MAIN10_Technicolor_2` is refused by FFmpeg's own parser
before the hardware is reached.

## VP9 corpus

Fluster's FFmpeg VA-API decoder leaves `hw_output_format` empty and
`hw_download` false, so it does not export a driver surface. A wrapper
setting those attributes to the values the Vulkan/CUDA classes use
(`-hwaccel_output_format vaapi` + `hwdownload,format=nv12`) scored 175/305;
the plain run scored 176/305, the difference being one 30 s timeout.

Neither number counts decode defects: the corpus reference MD5s predate this
machine's libvpx, whose own software arm scores 242/305 and fails the same
`vp90-2-14` family. Against this machine's libvpx, all 36 `vp90-2-14`
vectors are byte-identical through the driver with or without explicit
hwdownload (36/36 in each). `vp90-2-02` is below the node's 96x96 floor;
`vp90-2-21`, `-05`, `-18`, `-22` time out; `show-existing-frame`,
`intra-only` and `yuv444` are interface or node limits. The other corpus
counts in the capability table are Fluster results and are not per-build
regressions.

## Encode

`test/iris-encode-quality.sh` requires the encode to move the Iris IRQ
counter, the stream to decode with the requested frame count and geometry,
the platform decode to match a software decode byte for byte, per-frame SSIM
above a floor that a blank stream fails, and a decode-to-encode pass to keep
the source geometry. It also runs a large-geometry lane with the same oracle
in framemd5 form (one 8K frame is 50 MiB); `IRIS_ENCODE_EXTRA_GEOM` adds
further geometries with `IRIS_ENCODE_EXTRA_FRAMES` frames each, empty by
default.

| Geometry | H.264 | HEVC |
| --- | --- | --- |
| 1080p, 120 frames | SSIM 0.998596 (worst 0.9985), decode-to-encode 0.997654 | SSIM 0.996808 (worst 0.9963), decode-to-encode 0.995067 |
| 7680x4320, 8 frames | 8/8, SSIM 0.995885, IRQ +23 | 8/8, SSIM 0.994700, IRQ +23 |
| 4096x2160, 4 frames | 4/4, SSIM 0.992080, IRQ +17 | 4/4, SSIM 0.991085, IRQ +15 |
| 5120x2880, 4 frames | 4/4, SSIM 0.992741, IRQ +16 | 4/4, SSIM 0.991188, IRQ +17 |

A blank control scores 0.000034 against the 0.90 floor. H.264 VBR at a
4 Mbit/s target produced 4007485 bit/s over 300 1080p pictures, HEVC
3985581 bit/s, both within 0.4 % of the target. `test/iris-encode-baseline.sh`
at the same 1080p settings: H.264 507 fps against libx264 116 (4.4x) at 3.1
against 37.3 ms client CPU per frame, HEVC 549 against libx265 36 (15.2x) at
2.6 against 161.5 ms. Whole-device power over idle is 2.8 W / 3.0 W against
4.8 W / 5.7 W for the software encoders.

Rolling-row intra refresh through a `VAEncMiscParameterRIR` is honoured under
CBR only and refused under VBR/CQP, where the firmware produces a
byte-identical stream. `test/iris-encode-ir-probe.cc` drives it through the
public VA API. FFmpeg 8.1.3 and GStreamer do not request it, LTR has no VA
encode-side field, and hierarchical coding has no client: these are driver
capabilities with no client gate, recorded as such in the capability table.
`test/iris-encode-misc-bound-probe.cc` requires `renderPicture` to refuse a
1-byte and a header-only misc element with
`VA_STATUS_ERROR_INVALID_PARAMETER`.

HEVC Main10 encode, HEVC B-frames, VP9/AV1 encode and packed headers are not
available: the encoder node has no VP9/AV1 CAPTURE format, no P010 raw input,
and the experimental HEVC B-frame build returned nine pictures at ~23 dB
before an error completion.

## Throughput

CPU policies in `performance` at their maximum (2016/2803/2956 MHz), GPU in
`userspace` at 680 MHz, x265 ultrafast clips with `bframes=0` and
`keyint=60`, five rounds per cell. Decode is
`-hwaccel vaapi -hwaccel_output_format vaapi -f null -` (no download or
display); encode is `-c:v h264_vaapi -rc_mode VBR -async_depth 4` over
`testsrc2` (includes the CPU NV12 upload).

| Operation | 720p | 1080p | 4K |
| --- | ---: | ---: | ---: |
| Decode | 953 fps | 754 fps | 346 fps |
| Encode | 679 fps | 585 fps | 172 fps |

Each cell's spread is under 1.5 %. Decode client CPU time is 0.274/0.423/0.403
s; encode client CPU time 1.01/1.94/4.82 s.

These cells use one synthetic clip per size. Three 3840x2160 HEVC clips at
the same settings but different content, five interleaved rounds each on a
quiet machine:

| Source | Frames | KiB/access unit | Mean fps | Range |
| --- | ---: | ---: | ---: | ---: |
| `testsrc2`, crf 28 (the throughput clip) | 480 | 88.4 | 340.6 | 336.8-343.3 |
| `smptebars`, crf 28 | 300 | 0.3 | 391.5 | 385.6-400.0 |
| `testsrc2`, crf 12 | 300 | 379.4 | 84.9 | 84.1-85.4 |

Decode speed at 4K tracks access-unit size, not resolution alone, so 346 fps
describes one bitrate and one content shape. A kernel-to-kernel difference far
smaller than this content effect cannot be attributed to the driver without
hash-pinned clips on both sides. The 4K encode cell was
probed with three builds (8 OUTPUT slots, 8 CAPTURE slots, a `POLLOUT` wait);
all stayed within the shipped build's spread. No cross-driver claim is made
from these tables.

## Long-window

`test/iris-concurrency-soak.sh mixed` ran 1800 s, `profile=SHORTENED`, under
`test/remote/gate.sh` on source `1670977` (`source_dirty=0`). Two independent
VA contexts decoded H.264 and HEVC 640x360 at 24 fps.

| Lane | Frames | Preflight | Teardown |
| --- | ---: | --- | --- |
| `mixed-h264` | 43200 | exact framemd5 vs software | `timeouts=0`, terminal LAST drain |
| `mixed-hevc` | 43200 | exact framemd5 vs software | `timeouts=0`, terminal LAST drain |

Both traces show `session open`, zero `publish=drop`, `publish=error`,
`publish=misplaced`, `capture error index=` and `sync timeout token=`. The
boot ID is unchanged, the pack went 30 % -> 25 % and the hottest thermal-zone
sample was 93.9 °C, below the 100 °C guard, with no firmware reset. This
1800 s window is the current long-window record (guard profile `SHORTENED`).

## Video processing

`VAProfileNone` / `VAEntrypointVideoProc` is available by default on a build
that found Vulkan; `V4L2_VA_VPP=0` removes it. Pinned compute shaders import
CAPTURE DMA-BUFs as Vulkan external memory.

| Gate | Oracle |
| --- | --- |
| `nv12_scale_gate.sh` / `p010_scale_gate.sh` | every pixel against swscale's point map; bilinear within 1 code of a double-precision model |
| `transpose_vaapi_gate.sh` | byte-exact against FFmpeg software rotation across all 7 directions, NV12 and P010 |
| `crop_vaapi_gate.sh` | byte-exact against `crop,scale` |
| `tonemap_gate.sh` | within 2 codes of a BT.2390/BT.709 double-precision model |
| `overlay_gate.sh` | within 2 codes of a double-precision mixing model; alpha 0/1 byte-exact |
| `overlay_vaapi_gate.sh` | exact against a BT.709 reference with premultiplied BGRA |
| `prereq.sh` | coherent CPU read-back of imported DMA-BUFs |

## Boundaries

UBWC `Q08C`/`Q10C` CAPTURE formats are enumerated by the node and the
firmware writes real UBWC. Turnip decodes Q08C byte-exactly through
`VK_EXT_image_drm_format_modifier`, but EGL — the Copy publish path and
`vaGetImage`/`vaPutImage` — reads back the wrong pixels, `QCOM_COMPRESSED` on
P010 is not advertised, and no VA client sets
`VASurfaceAttribDRMFormatModifiers`. Surfaces stay `NV12`/`P010` and imported
DMA-BUFs must be `DRM_FORMAT_MOD_LINEAR`. The encode side has no compressor.

H.264 High 10 decode is absent from the kernel's decode profile mask; VP9
`show_existing_frame` has no `frame_to_show` field in the VA picture
parameter buffer; AV1 `VAConfigAttribDecAV1Features` answers 0 and is read by
no client on this target. Each is named in the capability table.
