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
The bare-V4L2 probe states the same rule from the other side: with 12 access
units and `surfaces=1` the client buffer queued for picture $k$ receives
picture $k$, 12/12, while `surfaces=16` completes the twelve DMA-BUFs in
slots 15,13,12,14,11,10,9,8,7,6,4,5 against a QBUF order of
15,14,13,12,11,10,9,8,7,6,5,4 (strace, 2026-11-19). Every buffer held the
picture its timestamp named, so the pixel content is right and only the
positional association fails; that is why the policy queues one picture at a
time. The VP9 alt-ref clip (`lag720.webm`, 480 frames) reports
`submitted=515 completed=515 errors=0 drops=0 misplaced=0` with framemd5
equal to software. Chrome 156 reported `VaapiVideoDecoder` with
`kIsPlatformVideoDecoder=true` and `submitted=1705 completed=1705 drops=0
retained=1161` in one guarded window and `1588/1588 drops=0` in another;
Firefox 158 reports hardware AV1 on local and live YouTube/Bilibili streams.
A browser decode counts as hardware only when a media surface reports the
selected decoder with frame and drop evidence. Chrome 156 HEVC seeking was
checked on 2026-11-19 on a 720p clip: a backwards seek from ~6.7 s to 5 s
continued decoding (160 -> 278 frames, `dropped=0`, no media error), with and
without `--disable-features=ExtendedVideoBitstreamValidation`, so this driver
needs no seek workaround on that browser.

At 7680x4320, `test/iris-8k-gate.sh` decodes 30-picture clips, one per
qualified codec, with a software framemd5 oracle: 30/30 exact frames each,
granted CAPTURE pool 8 under the 384 MiB memory budget, `errors=0 last=1
drain=1`. Run identity: boot `edd37f6a-cb04-42b0-b9b7-96360172a5ca`, module
`a3732c1c…`, driver `0d816eda…` as a `LIBVA_DRIVERS_PATH` build-tree
override, source `0e5e82c` with the Direct publish fix in the working tree,
decoder node resolved by name. Gate record `gate-8k30b`.

The 30-picture clip found a publish-policy defect the 4-picture run could
not: Direct publish pins one CAPTURE frame per decoded picture the client
has not consumed, and the 8K pool is the firmware minimum plus one (8), so
FFmpeg's hwdownload backlog held every slot and the session stopped at 26 of
30 pictures (`held=8 queued=0`, `sync timeout token=27`). The same clip
completes 30/30 under Import and under Copy, and 4K keeps the full 32-frame
pool and Direct. The automatic policy now offers Direct only when the pool
covers the firmware minimum plus eight pictures and publishes Copy
otherwise; `V4L2_VA_PUBLISH=direct` still forces the old behaviour and still
stalls at 26/30, which is what made the mechanism attributable.

Import works at this geometry: `V4L2_VA_PUBLISH=import` decodes all four
codecs 30/30 framemd5-exact with `pool=8 memory=import` and `errors=0 last=1
drain=1` (record `gate-8k-import-1118b`), and a 120-picture H.264 clip
completes 120/120 exact under Import (`gate-8k120-import`) and under the
automatic policy (`gate-8k120-auto`, which publishes Copy at this pool). Run
identity for the three: boot `edd37f6a-cb04-42b0-b9b7-96360172a5ca`, module
`a3732c1c…`, driver `821bf2a8…` as a `LIBVA_DRIVERS_PATH` build-tree
override, source commit `b934188` with a clean tree, decoder node resolved
by name.

Direct cannot fit at 8K under the stock module. The policy asks for the
firmware minimum plus eight held pictures, 15 for H.264 and HEVC, and an 8K
NV12 frame is 47.5 MiB, so that pool alone is about 713 MiB. The video-codec
node sits behind the SMMU (the live device tree carries `iommus`, and
`/sys/kernel/iommu_groups/9/devices` lists `aa00000.video-codec`), but the
CAPTURE queue uses `vb2_dma_contig_memops`, so the pool is contiguous DMA
memory from the 768 MiB CMA carve-out (`CmaTotal` 786432 kB on this boot).
The memory budget grants the 8-frame, 380 MiB pool instead, and
`V4L2_VA_PUBLISH=direct` still stalls at 26/30 as recorded above. The limit
is the allocator, not the IOMMU.

`packaging/kernel/0016-*` moves that allocator. With `capture_sg=1` the
decoder's CAPTURE queue uses `vb2_dma_sg_memops` and the buffer's device
address is taken from the mapped scatter list at QBUF; the encoder queues and
every internal buffer keep the contiguous allocator. The driver raises its own
budget to 720 MiB only while the module parameter reads `Y`, so Auto at 8K
grants 15 frames, the firmware minimum plus the eight-picture Direct headroom,
and publishes Direct. One boot, module `ca7aa394…`, driver `d15ff198…` as a
`LIBVA_DRIVERS_PATH` build-tree override, source `ec282f5` with the budget
change and 0016 in the working tree, decoder resolved by name:

- `test/iris-8k-gate.sh` under the automatic policy: 30/30 H.264 and 4/4
  HEVC, VP9 and AV1 frames framemd5-exact, `pool=15 memory=mmap`,
  `publish=direct` on every frame, `errors=0 last=1 drain=1` (record
  `gate-8k-sg2`).
- The 30-picture H.264 clip that stalls at 26/30 under `pool=8` completes
  30/30 framemd5-exact under both `V4L2_VA_PUBLISH=direct` and the automatic
  policy.
- `V4L2_VA_PUBLISH=import` at the same geometry: 30/30 exact with `pool=15
  memory=import` and `errors=0`.
- Regression on the same boot: the four-codec 48-frame matrix
  (`gate-matrix-sg`) and `iris-encode-quality.sh` including its 8K encode lane
  (`gate-encode-sg`) both pass.
- The same 8K gate on the installed package (`/usr/lib64/dri`, driver
  `d15ff198…`, no `LIBVA_*` override): all four codecs at `pool=15`, 30/30
  H.264, `publish=direct` (record `gate-8k-installed2`).
- Further coverage under `capture_sg=1` on the installed driver: UBWC Q08C
  read back through Turnip for all four codecs (`gate-ubwc-sg`), VP9
  Profile 2 P010 30/30 under Import (`gate-vp9p2-sg`), and two concurrent
  1080p H.264 clients 60/60 each (`gate-twoplayer-sg`).
- The memory-sensitive paths on the same boot and driver: AV1 film grain
  `grain8` 20/20 and `grain10` 2/2 byte-exact against libdav1d
  (`gates-sg-grain`), H.264 resolution change across 101 contexts and 100
  in-stream transitions with `drops=0 timeouts=0` (`gates-sg-drc`), and the
  VA video processor's eight scale ratios and three P010 cases byte-exact
  against swscale (`gates-sg-vpp`).
- The one-segment guard in `iris_vb2_buf_prepare` refuses a CAPTURE buffer
  whose DMA mapping is not one segment, so a device tree without `iommus`
  cannot make the firmware write past a truncated address. On a later boot
  (module `d65b745e…`, boot `8f8b9c18…`) the automatic policy granted
  `pool=15 memory=mmap` and published Direct on all 30 H.264 frames, and
  `V4L2_VA_PUBLISH=import` decoded 30/30 with `memory=import`. A driver-
  allocated mmap buffer and a client DMA-BUF both map as one segment on this
  node. The refusal itself has no target test: it needs a board whose
  mappings are not one segment.
- The 720 MiB budget held across three boots with `capture_sg=1`
  (`af805219…`, `8f8b9c18…`, `15fa16db…`): all four codecs granted `pool=15`
  and HEVC, whose 221 MiB contiguous bitstream buffer sets the ceiling,
  reached EOS with no internal-buffer allocation failure in `dmesg`.
- GStreamer's VA plugin on the same boot: with `GST_VA_ALL_DRIVERS=1` it
  registered seven elements, `vaav1dec` decoded the 120-frame clip
  frame-identical to the software decode, and `vah264enc` and `vah265enc`
  each returned 60/60 frames (`gates-gstreamer-sg2`).
- Chrome on boot `af805219…`, whose 8K gates recorded `pool=15` with module
  `ca7aa394…`: Chrome 156 selected `VaapiVideoDecoder` with
  `kIsPlatformVideoDecoder=true` from the installed driver `d15ff198…`,
  decoded 357 frames in a 15-second window with 0 dropped frames of an HEVC
  clip, and its GPU process held the resolved decoder node
  (`browser-acceptance/hevc-probe-20261119-013622`). The browser path is
  therefore covered under `capture_sg=1` as well.
- 8K decode throughput, 30 frames per codec on that boot with the installed
  driver (`gates-sg-throughput2`, `gates-throughput-contig`): the automatic
  policy at `capture_sg=1` (Direct) decoded H.264 at 59.6 fps and HEVC at
  52.1, against 32.3 and 30.8 with the contiguous pool (Copy). The native
  v4l2m2m path, which copies every frame out under either allocator,
  measured 37.5 against 36.7 fps on H.264, so the gain is Direct publication,
  not the allocator itself. Native HEVC at 8K fails with ENOMEM during packet
  submission on both allocators.

Sixty-frame 8K clips then measured the frame rate over a longer window, not
just the pool. Run identity: source `9db1fb5`, `source_dirty=0`, build-tree
driver `8c85f87d…0d9`, boot `79f5d3e1-0b32-4f3c-ae95-7435870fb362`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module `1323d58a…66a5` loaded with
`capture_sg=1`, decoder `/dev/video0` resolved by `iris_driver`. The
four-frame gate on the same driver and module passed all four codecs with
`pool=15`, exact framemd5, `errors=0 last=1 drain=1` (record
`8k-gate-sg-1119`). Each clip is
60 frames of `testsrc2` at 24 fps: H.264 `libx264 -preset ultrafast -tune
zerolatency`, HEVC `libx265 -preset ultrafast -x265-params
keyint=60:min-keyint=60:scenecut=0`, VP9 `libvpx-vp9 -b:v 8M -deadline
realtime -cpu-used 8 -g 60`, AV1 `libsvtav1 -preset 12 -g 60`. The throughput
client leaves frames on the VA surfaces (`-f null`), three rounds per codec;
the AV1 number comes from the downloading lane because the no-download client
releases its AV1 surfaces before the show-existing completions are published
(27 of 60 dropped, the same at 1080p), while the download lane publishes 60
direct and 13 invisible completions with no drop and exact pixels.

| Codec | Frames | fps, three rounds | Drops / errors | LAST |
| --- | ---: | --- | --- | --- |
| H.264 | 60 | 65.4 / 65.7 / 66.3 | 0 / 0 | yes |
| HEVC | 60 | 66.5 / 65.9 / 63.8 | 0 / 0 | yes |
| VP9 | 60 | 70.3 / 71.9 / 67.9 | 0 / 0 | yes |
| AV1 | 60 | 39.5 (downloading lane, 1.520 s) | 0 / 0 | yes |

H.264, HEVC and VP9 therefore sustain 8K60 on this boot; AV1 decodes all 60
frames but stays below 60 fps, so it is recorded as a measured value, not an
8K60 claim. The whole-run figure includes client startup, as the throughput
table above does. The guarded fps run peaked at 38.6 °C and the pack went from
73 % to 70 %.

A 60-frame run through the downloading client exposed one drain edge under
the experimental allocator. When every picture has completed before the
client destroys its context (`pending=0`, as the download lane produces),
HEVC and VP9 never receive the terminal LAST while the driver waits: the
firmware's drain-last message is processed only once the next command
(STREAMOFF) reaches the queue, so the session reports `session failed: no
LAST after STOP` after its 2 s bound, and a 6 s bound does not change it. The
pictures are unaffected: the same 60-frame gate run reported
`frames=60 framemd5=exact` and `submitted=60 completed=60 errors=0 drops=0`
for both codecs, and only the LAST check failed. H.264 and AV1 drain
normally, and the same HEVC stream on the contiguous allocator
(`capture_sg=0`, `pool=8`, Copy) reaches LAST with `pending=0`. The parameter
is experimental and off by default, so the qualified path is unaffected.

The parameter defaults off and is not part of the qualified module, so a
stock boot keeps the contiguous pool and Copy publication at 8K.

VP9 12-bit and AV1 12-bit are refused at profile selection; 4:4:4 and
large-scale tile are outside the advertised profiles or cleanly refused by
the translators. The capability table names each rejection.

The 12-bit refusal no longer rests on this driver's profile check alone:
`test/iris-decode-stream-probe` feeds the elementary stream to the node
through the session, which passes compressed data through untouched. A
10-bit Profile 2 control stream decodes 20/20 pictures into P010 (stride
1280, fourcc `P010`) on driver `0d816eda…`, while the 12-bit Profile 2
stream never raises SOURCE_CHANGE, on either the P010 or the NV12 lane, so
the node itself refuses it before a picture exists. The node has no 12-bit
CAPTURE format to offer (NV12, Q08C, P010, Q10C).

MPEG-2 is not available from this firmware. The SM8550 video firmware blob
contains the MPEG-2 decoder (it names `mpeg2Dec_SessionSetBuffers` and the
`MPEG2Decoder` queues), and HFI codec 6 is `HFI_CODEC_DECODE_MPEG2` in the
vendor headers, so `packaging/kernel/0015-*` registers `V4L2_PIX_FMT_MPEG2`
on the decoder, sends codec 6 and carries the MPEG-2 profile and level
tables. On the target the node then enumerates `MPG2` and registers
`mpeg2_profile` and `mpeg2_level` (Main default), but a session that starts
an MPEG-2 stream never produces a CAPTURE frame: the firmware answers
`received system error of type 0x5000003` while the core is opening the
session, the core moves to its error state and the firmware reloads. A
known-good H.264 stream decodes to EOS on the same module afterwards. The
driver's side of the experiment works, so the firmware's codec allow-list is
what excludes MPEG-2; MPEG-2 runs in software. Run identity: boot
`770e5fea-096c-491d-ae8f-ab28d0aaf1f1`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module `9885d2b4…` installed in
`/lib/modules` (patches 0008, 0009, 0013, 0014, 0015), decoder node
`/dev/video17` resolved by the `iris_driver` name, client GStreamer
`v4l2mpeg2dec` on a 30-frame 320x240 MPEG-2 elementary stream.

`test/iris-two-player-gate.sh` runs two independent 1920x1080 H.264 clients
with their own VA display and trace, a and b 90/90 exact, `errors=0 last=1`.
These gates are short exact-pixel checks, not throughput or thermal results.

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
| 7680x4320, 30 frames | 30/30, SSIM 0.997926, IRQ +68 | 30/30, SSIM 0.997392, IRQ +67 |
| 4096x2160, 4 frames | 4/4, SSIM 0.992080, IRQ +17 | 4/4, SSIM 0.991085, IRQ +15 |
| 5120x2880, 4 frames | 4/4, SSIM 0.992741, IRQ +16 | 4/4, SSIM 0.991188, IRQ +17 |

The 8K lane was extended from 8 to 30 pictures on the same boot and module
as the decode run above, with driver `0d816eda…` as a build-tree override
(source `0e5e82c` plus the working-tree fix); the platform decode of each
30-picture 8K stream is byte-identical to the software decode, and the lane
now exercises 30 frames end to end instead of the first few. Run record
`gate-enc8k30`.

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

Multi-slice encode, 11-18: `test/iris-encode-slice-probe` encoded three
640x480 IDR pictures through one CBR context, first with one slice buffer per
picture and then with four. H.264 reported `max_slices=128 structure=0x2`,
wrote 3 slices in the single-slice stream (57167 bytes) and 4 per picture in
the split stream (58349 bytes); HEVC reported the same attributes, 3 slices
(144792 bytes) and 4 per picture (144542 bytes). Both split streams pass the
probe's `slices_per_picture >= 4` check, so the client's slice count reaches
the firmware through `V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE`.
Run identity: boot `e6cbc26b-eb9b-457d-b930-79a59deb0985`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module
`5d295a78998b07e919b5c4f616f10e1cee820359ea8ced77b5283682121606b5` (0008,
0009, 0013), driver `e64c5e10…` as a `LIBVA_DRIVERS_PATH` build-tree override
(the installed package was not rebuilt), source commit `d817308` with the
in-progress Main 10 work in the working tree, encoder node `/dev/video18`
resolved by name. Gate records: `gate-slice-h264c` and `gate-slice-hevc`
under the run directory. An earlier run against the installed package
reported `max_slices=1` and refused the split stream, which is the module's
control probe doing its job: that package predates the kernel patch.

B pictures, 11-18: `test/iris-encode-b-gate.sh` encoded 300 frames of
1280x720 through FFmpeg's `h264_vaapi` with `-bf 1 -async_depth 3 -g 60`.
With `V4L2_VA_B_FRAMES` set, the stream carries 145 B, 145 P and 10 I
pictures, all 300 decode, SSIM against the source is 0.996038 against the
0.90 floor (a blank stream scores 0.000033), and a second run is
byte-identical. Without the opt-in the same client request codes no B
pictures and still decodes 300. The HEVC arm codes no B pictures under the
opt-in, because the config advertises the future direction and the l1
reference for H.264 only. An experimental build that advertised both to HEVC
made FFmpeg request them and the firmware answered with an error completion
after nine pictures, so `configure()` refuses HEVC B pictures as a backstop.
Run identity: boot `edd37f6a-cb04-42b0-b9b7-96360172a5ca`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module `a3732c1c…` (0008, 0009, 0013,
0014), driver `246e1c21…` as the installed package, source commit `249a650`
with the attribute work in the working tree, encoder node `/dev/video18`
resolved by name. Gate record: `gate-bframes-1118c`.
Before this change FFmpeg never requested B pictures: the driver answered
`VAConfigAttribPredictionDirection` with the previous direction alone, which
FFmpeg reads as a P-only encoder however many l1 references are advertised,
so the option fell back to intra and P pictures.

HEVC B-frames and VP9/AV1 encode are not available: the encoder node has no
VP9/AV1 CAPTURE format, and the experimental HEVC B-frame build returned nine
pictures at ~23 dB before an error completion. Packed headers are available
on the HEVC lane only (below).

HEVC Main 10 encode, 11-18: `test/iris-encode-main10-gate.sh` encoded 60
frames of 640x480 from a 10-bit source through FFmpeg's `hevc_vaapi` with
`-profile:v main10`. The encode moved the Iris IRQ counter by 123, `ffprobe`
reports `profile=Main 10` and `pix_fmt=yuv420p10le`, all 60 frames decode, and
the platform decode matches the software decode byte for byte. SSIM against
the 10-bit source is 0.999480 mean and 0.9973 worst against the 0.90 floor; a
blank stream scores 0.000032. The decoded luma carries 731 distinct values
with all four residues modulo 4 present, so the low bits are coded rather than
padded. The same run's 8-bit HEVC lane still encodes and decodes.
Run identity: boot `edd37f6a-cb04-42b0-b9b7-96360172a5ca`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module
`a3732c1c5b6c7ff4403a13734f367a9230680b44bc0e45002e18a4db03110608` (0008,
0009, 0013, 0014), driver `200773f2…` as a `LIBVA_DRIVERS_PATH` build-tree
override, source commit `d817308` with the Main 10 work in the working tree,
encoder node `/dev/video18` resolved by name. Gate record: `gate-main10b`
under the run directory. The first attempt failed at `createConfig` because
FFmpeg opens a probe config with no attributes and the driver defaulted a
Main 10 profile to 8 bits; the default now follows the profile.

Temporal layers, 11-18: `test/iris-encode-temporal-probe` encoded one GOP of
16 HEVC pictures through one CBR context, the first an IDR and the rest P,
with a `VAEncMiscParameterTemporalLayerStructure` of 3 layers rendered once
with the first picture. The driver answers `VAConfigAttribEncRateControlExt`
with six layers and no per-layer rate control, and the stream's VCL NALs
carry temporal ids 0, 1 and 2 (4, 4 and 8 pictures), so the structure reaches
the firmware and it codes a hierarchy. Run identity: boot
`edd37f6a-cb04-42b0-b9b7-96360172a5ca`, module `a3732c1c…`, driver `4016e6de…`
as a build-tree override, source commit `2a10c5d` with the temporal layer
work in the working tree, encoder node `/dev/video18` resolved by name. Gate
record: `gate-temporal3`. The first attempt was refused by the driver itself
(`unsupported encode misc parameter`); accepting the structure and bounding
its layer count and ids is what the unit tests now cover.

Packed headers, 11-19: `VAConfigAttribEncPackedHeaders` answers
`VA_ENC_PACKED_HEADER_MISC` for HEVC encode and no type for H.264. With MISC
advertised on both, FFmpeg 8.1.3's `h264_vaapi` failed every CBR/VBR encode
at `SPS id 0 not available`: it writes a buffering-period SEI as a raw packed
header that names the SPS by id, and the client only writes the SPS itself
when the sequence packed-header type is advertised. HEVC advertises MISC for
the HDR10 SEI (`test/iris-encode-hdr-sei-gate.sh` below); H.264 keeps the
firmware's own headers. After the correction, 60 frames of 1080p at CBR
2 Mbit/s encode for both codecs (H.264 490729 bytes, HEVC 541745 bytes)
through the build-tree driver (artifact `2b897cdf…`, source commit `6c216ba`
plus the working-tree changes).

ROI metadata, 11-19: `test/iris-encode-roi-probe.cc` and FFmpeg `addroi`
drive `VAConfigAttribEncROI` with the 0017 module. Run identity: boot
`79f5d3e1-0b32-4f3c-ae95-7435870fb362`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module
`1323d58a4c69177ccb7b887a4ecca384a2fb7d64e466bb2c8ded3eff4e6c66a5` (0008,
0009, 0013, 0014, 0017) loaded by `modprobe` after the instrumented
experiment module was replaced, encoder `/dev/video18` resolved by name,
driver `2b897cdf…` as a `LIBVA_DRIVERS_PATH` build-tree override from source
commit `6c216ba` plus the working-tree changes (the installed package was not
rebuilt).

- The encoder node registers `0x00990b92` (`V4L2_CID_CODEC_BASE + 658`) as a
  dynamic s32 array. A 1920x1080 H.264 session writes 8160 cells; the
  instrumented module received them unchanged (`a0=-31 … an=-31` for the
  FFmpeg delta) and packed them as `0xa10`.
- `HFI_CMD_DELIVERY_MODE` returns success before the session starts, the
  metadata buffer is queued with its input (`queue meta … index=0 input=0`)
  and completed by the firmware (`done`), with no error path.
- The coded picture does not change. 60 frames of 1080p browser content with
  `addroi=x=0:y=0:w=640:h=480:qoffset=-1` on every picture, H.264 and HEVC,
  CBR: byte-identical to the run without regions (`e6106777…` H.264,
  `3a167212…` HEVC). CQP and a full-frame delta −31 showed the same, and
  neither the pre-start `HFI_PROP_ROI_INFO` property nor the payload size
  with or without the extra 256 bytes changed the result.
- With the opt-in off the client is told: `ROI side data on input frames
  ignored due to lack of driver support.` and no map is written. With
  `V4L2_VA_ROI=1` the trace shows `encode roi regions=1 cells=8160` on all 60
  pictures and the stream is still byte-identical.

The firmware accepts the metadata and ignores the map, so ROI stays opt-in
and `VAConfigAttribEncROI` is not advertised by default. The module is kept
as a packaging option: on a firmware that acts on the map, the driver path is
complete.

HEVC B isolation, 11-19: a scratch build of the same driver relaxed the two
HEVC gates (the future-direction/l1 advertisement and `configure()`'s
H.264-only check) and ran FFmpeg `hevc_vaapi -bf 1 -async_depth 3` at
1080p under VBR. Run identity: same boot, kernel and 0017 module as the ROI
run above; the scratch driver was built from a copy of the deployed source
in `~/Lab/Bridge/tmp/hevc-b/exp-src` and loaded through `LIBVA_DRIVERS_PATH`.

- The session submitted 14 pictures, then waited the full 2000 ms sync bound,
  marked the encoder drained and failed the next `vaEndPicture`. The stream
  holds 11 pictures and does not decode: `Could not find ref with POC 11` /
  `Error constructing the frame RPS`.
- Adding the explicit HEVC hierarchical B layer type and layer count, the
  shape the kernel's `iris_set_layer_type()` accepts under VBR-CFR, shortened
  the run to two pictures in the stream before the same 2 s stall.
- `V4L2_BUF_FLAG_{P,B,KEY}FRAME` is set by the kernel only on completions;
  nothing reads the flags from QBUF, so a client cannot direct the picture
  schedule through the buffer API. The schedule is the firmware's own.

HEVC B stays refused in `configure()` and unadvertised; the capability table
records the boundary.

Long-term references, 11-19: `test/iris-encode-ltr-probe` drives the encoder
node directly, because VA 2.23 has no encode-side LTR field and this driver
exposes none. Run identity: same boot, kernel and 0017 module as the ROI run
above; the probe was built from source commit `421c716` plus the working-tree
changes and resolves the encoder node itself by `iris_driver` QUERYCAP name.

- The node registers `V4L2_CID_MPEG_VIDEO_LTR_COUNT`,
  `..._FRAME_LTR_INDEX` and `..._USE_LTR_FRAMES`, and accepts `LTR_COUNT` only
  with a non-CBR rate mode; the probe runs VBR at 2 Mbit/s, 640x480, GOP 30.
- Three otherwise identical 60-picture sessions: one writes neither control,
  one marks picture 10 as the LTR frame, one also tells picture 12 to predict
  from it. The mark alone changes the coded pictures (`d54d5825…` against
  `ce22f5a8…`), and the reference request changes them again
  (`5f7a9b89…`), with the first differing picture at 10 and picture 12 among
  the 49 that differ.
- All three streams decode to 60 frames with no error.

The kernel and firmware support LTR end to end. There is still no VA interface
and no client, so the driver advertises none; the capability table says so.

Encode latency, 11-19: `test/iris-encode-latency.sh` runs 300 frames of
1920x1080 at CBR 8 Mbit/s with `-async_depth 1`, so the client keeps one
picture in flight and the number is the firmware's, not a queue it built.
Run identity: same boot and 0017 module as the ROI run; build-tree driver
`8c85f87d…` from source commit `4070031` plus the working-tree changes;
render node `/dev/dri/renderD128`.

| Codec | First picture | p50 | p95 |
| --- | --- | --- | --- |
| H.264 | 4335 us | 4211 us | 5497 us |
| HEVC | 4625 us | 3529 us | 4974 us |

Both runs moved the IRQ counter by 608 and both outputs decode 300 frames.
The encoder has no low-latency firmware switch: the session submits each
picture immediately and hands completions back as they arrive, so these are
the steady-state costs of one picture through the firmware.

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

The same scenario scales to more clients at 1080p. `IRIS_SOAK_SESSIONS` was
set to 4 and then 8 with `IRIS_SOAK_SIZE=1920x1080`, first for 120 s and then
for 300 s, all four runs directly from the deployed build tree and all
`profile=SHORTENED`. Run identity: source `aa51a0d`, `source_dirty=0`,
build-tree driver `8c85f87d…0d9`, boot
`79f5d3e1-0b32-4f3c-ae95-7435870fb362`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module `1323d58a…66a5` (the 0017 set),
decoder `/dev/video0` resolved by `iris_driver`.

| Window | Sessions | Clients | Frames each | Preflight | Teardown | Peak `video-thermal` |
| ---: | ---: | --- | ---: | --- | --- | ---: |
| 120 s | 4 | 2 H.264 + 2 HEVC | 2880 | exact framemd5, both codecs | `timeouts=0`, terminal LAST, all four | 63.8 °C |
| 120 s | 8 | 4 H.264 + 4 HEVC | 2880 | exact framemd5, both codecs | `timeouts=0`, terminal LAST, all eight | 67.4 °C |
| 300 s | 4 | 2 H.264 + 2 HEVC | 7200 | exact framemd5, both codecs | `timeouts=0`, terminal LAST, all four | 67.0 °C |
| 300 s | 8 | 4 H.264 + 4 HEVC | 7200 | exact framemd5, both codecs | `timeouts=0`, terminal LAST, all eight | 70.9 °C |

Every client held the 24 fps schedule for the full window. No trace carried
`publish=drop`, `publish=error`, `publish=misplaced`, `capture error index=`,
`sync timeout token=` or `session failed`; the boot ID did not change, the
thermal guard never fired, and the pack went 73 % -> 72 % across the two
120 s runs and read 68 % after the 4-session 300 s run and 67 % after the
8-session one. The 300 s clients
finished with `submitted=7209 completed=7209 errors=0 drops=0 timeouts=0
drains=1`. Eight simultaneous 1080p sessions therefore stay under the
firmware's session ceiling; the runs do not probe how many sessions exist
above eight.

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

## Experimental kernel patches

Patches 0010-0012 (`packaging/kernel/README.md`) were built into the 7.3.4
module from kernel source commit `87ac0f2347eb`, installed as
`9bad78eab5b18fc23781e74d5da140a3cbf0424797eeefd1fc539487b83b607c`, and run
on boot `5aa19762-7d68-49b8-9af8-12fd8aba4b53`. The VA driver stayed the
installed `35cdab55…d591`. They are not part of the qualified combination.

| Gate | Result |
| --- | --- |
| `test/iris-real-clip-gate.sh` `set=plain` | `120/120` Import, `invisible=15`, byte-exact against the software decode, `guard=complete` |
| `test/iris-matrix.sh` | H.264, HEVC, VP9, AV1 `48/48` each, `drops=0 timeouts=0`, AV1 `noshow=23` |
| `test/iris-av1-grain-gate.sh` `set=all` | grain8 `20/20`, grain10 `2/2`, byte-exact |
| DRC clip, qualified module | sequence-change LAST `flags=0x104040`, `invisible=63`, `84/84`, byte-exact |
| DRC clip, experimental set | sequence-change LAST `flags=0x104000`, `invisible=62`, `84/84`, byte-exact |
| H.264 encode, 30 frames | `encode drain result=complete`, no sync timeout |
| `modprobe -r qcom_iris`, qualified module | hung in `D` at `disable_irq -> __synchronize_irq` from `iris_core_deinit` |
| `modprobe -r qcom_iris` + reload, experimental set | both completed in under a second, both nodes re-probed |

0012 is the demonstrated fix. 0011 removes the fabricated error from the
sequence-change completion and, as a side effect, one boundary NOSHOW
completion is no longer published through this driver's invisible path;
visible output is unchanged. 0010 was not isolated from 0011 in this run.
The qualified module was restored (`8f771ada…dc75`) and the target rebooted
after the run.

## UBWC surfaces

Opt-in (`V4L2_VA_UBWC=1`); gate `test/iris-ubwc-gate.sh`. Run identity: source
`8561d10`, `source_dirty=0`, `guard=complete exit=0`, boot
`edd37f6a-cb04-42b0-b9b7-96360172a5ca`, kernel
`7.3.4-sm8550-00104-g87ac0f2347eb`, module `a3732c1c…10608` (the series adds
no kernel patch and the patch set is unchanged), decoder `/dev/video17`
resolved by `iris_driver`. The first pass loaded the build tree
(`246e1c21…770c`); `scripts/install-system.sh` then put the same build at
`/usr/lib64/dri/v4l2_drv_video.so` (the previous file is under
`~/iris-vaapi-lab/rollback/`) and the gate was rerun there with the same
results.

| Gate | Result |
| --- | --- |
| `iris-decode-stream-probe` into Q08C, 640x480, H.264/HEVC/VP9/AV1 | CAPTURE `fourcc=0x3231564e modifier=0x500000000000001 planes=4 size=479232`; Y meta `offset=0 pitch=64 rows=64`, Y `4096/640/480`, UV meta `311296/64/32`, UV `315392/640/256` |
| `vk-ubwc-read`, frames 0 and 11 of each codec | Q08C `luma=0/307200 chroma=0/153600` differing samples against the linear decode |
| linear control, same frames | `luma=0/307200 chroma=0/153600`; the control goes through the identical copy first, so a Q08C mismatch would be the modifier's |
| `iris-va-ubwc-probe`, `V4L2_VA_UBWC=1` | 21/21: the grant, 12 frames decoded into four UBWC surfaces, four R8 layers exported at the Q08C offsets with the modifier, and `vaDeriveImage`/`vaGetImage`/`vaPutImage` answered with `VA_STATUS_ERROR_UNIMPLEMENTED` |
| `iris-va-ubwc-probe`, switch unset | a compressed-only list is refused (`VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT`); a list with a `LINEAR` fallback creates a surface and exports `DRM_FORMAT_MOD_LINEAR` |

The module applies its UBWC stride/scanline property only to AV1 decodes; the
other three codecs read back exactly without it, so no kernel patch is needed
for this.

The linear path is unchanged by these commits, on the same build-tree driver:
`iris-matrix.sh` 48/48 per codec (`drops=0 timeouts=0`),
`iris-real-clip-gate.sh` plain 120/120 (`import=120 invisible=15`),
`iris-av1-grain-gate.sh` grain8 20/20 and grain10 2/2, and
`iris-dynamic-resolution.sh` 101 contexts over 100 transitions (`drops=0`).
Each is `guard=complete exit=0`; the tree moved only through test and
documentation commits (`ffb3105` to `7645dc6`), and the driver hash is the
same at both.

Installed as the package file, the rest of the target gates pass at source
`4b0d63e`, `source_dirty=0`, `guard=complete exit=0`:

| Gate | Result |
| --- | --- |
| `iris-vp9-p2-gate.sh` | 30/30, 12 P010 surfaces |
| `iris-zero-copy-suite.sh` | 24 Direct |
| `iris-two-player-gate.sh` | 450 frames per lane, exact framemd5, terminal LAST (two copies of one 1080p clip, baselines generated for the run) |
| `iris-gstreamer-gate.sh` | 48/48 decode, 60/60 through `vah264enc` and `vah265enc` |
| `iris-encode-main10-gate.sh` | Main 10, SSIM 0.999480 |
| `iris-throughput.sh` | H.264 1080p 705.9 fps against 522.6 native; HEVC 589.4 against 463.0 |
| `iris-encode-baseline.sh` | H.264 458.6 against libx264 120.6 fps; HEVC 540.0 against libx265 36.4 |
| `iris-encode-quality.sh`, `IRIS_ENCODE_8K=0` | SSIM 0.998596 and 0.996808, as above |
| `iris-power-sampler.sh` | 1080p decode 1.24 W against 6.05 W software; H.264 encode 2.58 W against 4.86 W |
| `iris-8k-gate.sh` | 30 pictures per codec, `pool=8`, firmware minima 7 (H.264, HEVC) and 4 (VP9, AV1) |

The browser gates were not rerun; they need an interactive session.

A compressed surface publishes Direct or Import only. Copy, the image entry
points, film grain, video processing and the encoder answer
`VA_STATUS_ERROR_UNIMPLEMENTED`, a completion that would degrade to Copy fails
with `publish=ubwc-copy-refused`, and Q10C is refused because nothing on this
stack advertises `QCOM_COMPRESSED` for P010.

## Boundaries

EGL reads UBWC back as the wrong pixels, so the CPU and Copy paths and the
image API refuse a compressed surface instead of returning them; `Q08C` exists
for consumers that take the modifier and the four-plane descriptor, evidenced
above. `QCOM_COMPRESSED` on P010 is not advertised, so `Q10C` has no reader and
is not offered. No VA client on this target sets
`VASurfaceAttribDRMFormatModifiers` yet, which is why the surface is opt-in.
Imported DMA-BUFs must be linear unless they carry the modifier with the
kernel's four-plane offsets. The encode side has no compressor.

H.264 High 10 decode is absent from the kernel's decode profile mask; VP9
`show_existing_frame` has no `frame_to_show` field in the VA picture
parameter buffer; AV1 `VAConfigAttribDecAV1Features` answers 0 and is read by
no client on this target. Each is named in the capability table.
