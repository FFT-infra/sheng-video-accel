# Architecture

This document is the contract for the rebuild (tag `pre-rebuild-20260920`,
branch `main`). Every later phase, every review and every task handed to a
worker agent is scoped against it. When
code and this document disagree, one of them is wrong and the disagreement
is a bug to file, not a detail to paper over.

## 1. What this driver is for

Stock Google Chrome on arm64 Linux is built with `use_vaapi=true` and
`use_v4l2_codec=false`: the only hardware video decode entry point compiled
into the binary is libva. The same is true of every prebuilt Electron
application and of Firefox's FFmpeg hwaccel. Fedora's own Chromium is built
with `use_v4l2_codec=true`, talks to the Iris node directly through
Chromium's `V4L2StatefulVideoDecoder`, and never loads this driver.

So the product is: **a libva backend that turns VA-API decode calls into
Qualcomm Iris V4L2 stateful M2M sessions, good enough that Google Chrome
selects `VaapiVideoDecoder` and plays 1080p H.264/HEVC with zero dropped
frames and no CPU-side copy on the hot path.** Everything else (FFmpeg,
GStreamer, mpv, Firefox, Electron) follows from the same mechanism and is
welcome, but Chrome is the acceptance client.

The same display also answers `VAEntrypointEncSlice` for H.264 and HEVC Main
on the encoder node the kernel exposes next to the decoder. Encode is a
second entrypoint, not a second product: it exists so FFmpeg's `h264_vaapi`
and `hevc_vaapi` (and anything else that asks a VA display for an encode
entrypoint) can use the VPU without a second display or a second driver.
Scope: H.264 Constrained Baseline/Main/High and HEVC Main, CBR, VBR or CQP,
NV12 input, and HEVC Main 10 with P010 input on a module carrying kernel
patch 0014; Annex-B output, one reference picture in flight per context. B
pictures are opt-in (`V4L2_VA_B_FRAMES`) and H.264 only; the session then
queues pictures in display order and the client needs one more picture in
flight than the default (invariant 10). Slices are a macroblock budget under
CBR or CQP, up to 60 fps and 4096x2160 (kernel patch 0013); ROI is opt-in
(`V4L2_VA_ROI`, kernel patch 0017, §4.1).

Non-goals: being a generic backend for stateless request-API decoders (that
is upstream `mxsrc/libva-v4l2`'s job and stays on `master`), VP9 and AV1
encode, HEVC B-frames, tiles, `VAEntrypointEncSliceLP`,
and shipping a custom Chromium. (Video post-processing is supported via
a Vulkan compute backend, on by default and disabled by `V4L2_VA_VPP=0`).

## 2. Target and kernel contract

The qualified target is a Snapdragon SM8550 tablet (`sheng`) on Fedora 44
with a self-built kernel from `ianchb/sm8550-mainline` (Iris is the
`qcom-iris` module, HFI gen2). Because the kernel is self-built, kernel-side
fixes are in scope.

Five Iris kernel changes are adopted as part of the target. The first two
were first applied to a `sheng-7.2.2` tree; after the target moved to
`kernel-sheng-7.2.6-1` the rebased patches and the module-only build recipe
live in `packaging/kernel/`. On `sheng-7.3.4` the same five changes are
carried by two patches instead of four, because the 7.3.4 driver already
implements `REQUEST_SYNC_FRAME` and the error-completion stability fixes
that 7.2.6 needs separate patches for:

| Patch | What it changes | Why the driver needs it |
| --- | --- | --- |
| decode-order output (`V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE`, delay 0) | Firmware returns pictures in decode order instead of display order | A VA client may `vaSyncSurface` a picture before submitting the ones it is reordered behind. Display-order firmware withholds it, and the only userspace escape is a timeout plus STOP/START drain. In decode order every submitted picture completes independently. |
| CAPTURE pool up to 64 buffers | Raises the vb2 `max_num_buffers` for the gen2 decoder CAPTURE queue | Threaded VA clients keep more than 32 surfaces; a driver-owned pool needs headroom above the client's pool. |
| encoder loop-filter controls | Exposes `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA` and the HEVC equivalents, sent as `HFI_PROP_DEBLOCKING_MODE` | VA carries deblocking per slice (mode, alpha, beta); the stock kernel has no control to carry it to the firmware, so a non-default request is refused instead of being silently ignored. |
| encoder `REQUEST_SYNC_FRAME` | Exposes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, sent as `HFI_PROP_REQUEST_SYNC_FRAME` with the sequence-header mode from `PREPEND_SPSPPS_TO_IDR` | Without it a requested IDR is produced by moving the intra period, which costs two control writes per request and leaves the parameter sets for the driver to prefix. |
| timestamp on error completions | `iris_vb2_buffer_done()` copies `buf->timestamp` onto a `V4L2_BUF_FLAG_ERROR` CAPTURE completion instead of zeroing it | The CAPTURE timestamp is the token that names the picture. Zeroed, a corrupt picture cannot be attributed: the target stays pending until a drain or the sync bound, and the client sees `TIMEDOUT` rather than `DECODING_ERROR`. |

The 7.2.6 target runs a module built from 0001-0004; the 7.3.4 target runs
0008-0009, which carry all five changes above. The driver still
detects each control and falls back when it is absent, so a stock module
keeps working: display order with the drain fallback, deblocking refused,
the intra-period lever, and an error completion reported at the drain.
On a kernel without the timestamp patch the error branch is reachable only
through the zero-timestamp path, which is also how an AV1 hidden picture
arrives; the driver therefore decides from the picture-type bits and the
payload rather than from the timestamp alone.

The driver **must not require** any of these. It probes for the display-delay
control at session open and runs in one of two session modes:

- **decode-order** (control present): sync waits are bounded by the normal
  timeout and a timeout is a hard error.
- **display-order** (control absent, stock Fedora kernel): sync waits are
  bounded; on timeout the session issues STOP, waits for LAST, and START,
  which flushes the reorder window. This is the old driver's recovery path
  and the only reason it survives. No idle-time watchdog thread: a timer that
  guesses end-of-stream from silence is how the previous tree got a thread
  waking every 25 ms in every context.

Kernel version, Iris module hash and firmware identity are part of every
hardware result (`test/remote/provenance.py` and `test/remote/gate.sh` record
them).

## 3. The one ownership model

The previous tree carried five CAPTURE ownership modes selected by
environment variables. The rebuild has one model with two publish policies
chosen per surface from observed client behaviour.

### 3.1 Objects

```
Session (one per VA context, one fd)
  owns  OUTPUT pool   : 4 MMAP slots, one access unit each
  owns  CAPTURE pool  : N slots, N = min(max(surfaces + 4, firmware min + 8),
                        kernel max) capped by the memory budget, budget =
                        384 MiB, or 720 MiB when the kernel allocates CAPTURE
                        from system memory (capture_sg). A client that
                        declares no surfaces is sized as 17, because FFmpeg's
                        VA-API 1.x path passes zero and then creates that
                        many: a pool equal to the client's count leaves STOP
                        no buffer for LAST (§4.3).
                        MMAP (default): every slot EXPBUF'd once -> Frame.
                        DMABUF (Import): slot i carries the client buffer
                        queued for one picture; nothing is pre-queued and
                        the backlog decides what goes in (3.3)
  owns  pending map   : token -> Target (token = strictly increasing
                        CLOCK_MONOTONIC-shaped timestamp copied by the kernel
                        from OUTPUT to CAPTURE; the only association key)

Frame  (shared_ptr)   : one CAPTURE slot's DMA-BUF fd, layout, index, queued flag
Surface               : width/height/fourcc
                        memory : shared_ptr<Frame> | StableBuffer | none
                        persistent_export : bool
                        token, pending, status
```

### 3.2 Slot lifetime

A CAPTURE slot is in exactly one state, and the state is the `shared_ptr`
use count, not a flag:

| use count | state | meaning |
| --- | --- | --- |
| 1 (pool only), `queued` | Queued | firmware owns it |
| 1 (pool only), not `queued` | Free | will be QBUF'd on the next service pass |
| > 1 | Held | a Surface (or a GPU-copy view) is using its pixels |

`SlotPool::requeue_capture()` QBUFs every Free slot of an MMAP pool (an
Import pool queues nothing on its own; §3.3). A Surface that adopts a Frame
holds it until the surface is reused (`vaBeginPicture` on it), destroyed,
or the client releases its exported handle. There is no "requeue predicate":
releasing the reference is the requeue.

### 3.3 Publish policies

When a CAPTURE completion arrives with a token that has a pending Surface:

- **Direct**: the surface takes the `shared_ptr<Frame>`. Zero copies. This
  is the policy for clients that export or map a surface only after it has
  been decoded (FFmpeg, GStreamer, mpv, `vaGetImage` users). Every export
  after this point hands out the slot's own DMA-BUF. Direct pins one CAPTURE
  frame per decoded picture the client has not consumed, and the driver
  cannot see that queue; the automatic policy therefore offers it only when
  the pool has headroom over the firmware minimum (eight pictures, the
  FFmpeg filter backlog measured at 8K). At a geometry where the memory
  budget leaves less, the completion uses the Copy policy instead, which
  releases the slot immediately. At 8K that is the whole difference between
  the policies: the 384 MiB budget grants eight frames, which is the
  firmware minimum plus one, so Auto publishes Copy; with the patched
  kernel's `capture_sg=1` the same pool is 720 MiB and fifteen frames, and
  Auto publishes Direct. Direct is the faster policy there: a client that
  consumes every picture decodes 8K at 85-92 fps for H.264 and HEVC, 80-85
  for VP9 and 62 for AV1, against the Copy lane's roughly half. Fifteen
  slots are also few enough for a client that declares more to hold all of
  them, and a drain then has no buffer to carry its terminal LAST: that is
  why `capture_sg` stays experimental (TEST-RESULTS.md records both the
  throughput and the failed drains).
- **Copy**: the surface already owns a `StableBuffer` because the client
  exported it *before* its first decode (Chrome pre-exports its whole
  NativePixmap pool and caches the fd for the surface's lifetime). The
  completion is copied into that buffer and the slot is released
  immediately. The copy engine is **GPU blit** (EGL/GLES on Adreno, both
  sides imported as DMA-BUF, no CPU touch) by default, **CPU memcpy** as
  fallback and for diagnostics. Before writing, wait on the destination
  DMA-BUF's implicit fence (`poll(POLLOUT)`): the compositor may still be
  sampling the previous frame from that buffer.
- **Import**: the same pre-exporting surface, but the firmware decodes
  straight into its `StableBuffer`, which is queued into a CAPTURE pool in
  DMABUF memory for exactly that picture. It requires decode-order output,
  a client buffer whose layout equals the CAPTURE layout, **one client
  picture in flight at a time** and **no holes** (a released picture still
  gets its buffer): the firmware picks which queued buffer a picture lands
  in, so only an empty choice is a determined one, and a missing buffer
  shifts every later picture into a neighbour's. A completion whose token
  does not match its buffer's token fails both surfaces and is never
  published. The window is one *picture*, not one buffer: an access unit the
  firmware decodes as several pictures (a VP9 alt-ref superframe) gets its
  client buffer queued once per completion before the first one returns, so
  the firmware still chooses only between the same picture's memory and
  never stalls waiting for somewhere to put the second half. Measured scope
  and failures: TEST-RESULTS.md.

**UBWC surfaces** put the same three policies on a compressed buffer. The
CAPTURE format is Q08C and the surface carries its own four-plane `Layout`
(§3.4); only Direct (the surface takes the slot) and Import (the client's
compressed buffer is queued) have a reader. Copy would have to decompress, so
an Import request that degrades to Copy fails the picture with
`publish=ubwc-copy-refused` in the trace instead of copying compressed bytes
into a different allocation. Every other path that expects linear pixels —
`vaDeriveImage`, `vaGetImage`, `vaPutImage`, film grain, the video processor,
the encoder, and the stable-buffer allocation behind them — answers
`VA_STATUS_ERROR_UNIMPLEMENTED` before touching memory (§8 `va ubwc refused`).
A context's surfaces must all carry the same modifier, and it cannot change
while the context lives.

`persistent_export` is set by `vaExportSurfaceHandle`, `vaAcquireBufferHandle`
or a `DRM_PRIME_2` import when the surface has no token yet. It is never set
by an environment variable. `V4L2_VA_PUBLISH=copy|direct|import` exists only
to force a policy in diagnostics and is not read on the hot path.

Whether Import is even offered comes from one place,
`data/iris-codec-capabilities.json` (`publish_import` per codec), enforced
against `src/va/driver.cc` by `test/static/iris-codec-matrix-check.sh`; the session
carries the resulting flag in its config and never guesses from the fourcc.

### 3.4 Stable buffers

Persistent-export surfaces need memory that the GPU can sample, Iris can be
copied from, and (for Import) the firmware can write into unchanged.
Allocation order: MSM GEM cached-coherent, MSM GEM write-combined, system
DMA heap. The layout is the kernel's CAPTURE layout for the same geometry
(`iris_yuv_buffer_size_nv12`/`_p010`): pitch aligned to 128 (256 for P010),
luma rows to 32, chroma rows to 16, chroma after luma, total rounded to
4 KiB. That makes an exported buffer bit-compatible with what the firmware
writes and also satisfies freedreno's linear import rules. One object, two
layers, linear modifier; the fd, pitch and offsets never change for the
surface's lifetime.

The second rung is a retry only for an unsupported cache mode:
`msm_alloc.c` masks `EINVAL` and `EOPNOTSUPP` and treats every other errno as
a real failure, so a GEM allocation that fails with `ENOMEM` goes straight to
the DMA heap rather than being retried write-combined. `iris-alloc-chain.sh`
walks all three rungs on the target by running `iris-alloc-probe` under
`alloc-fault.so`, which refuses the MSM allocations per mode; it asserts the
resulting `MemoryOrigin` and the exact sequence of attempts, and refuses to
run an injected mode without the shim.

A surface created with the `QCOM_COMPRESSED` modifier allocates the node's
Q08C buffer instead: `iris_yuv_buffer_size_qc08c()`'s four 4 KiB-rounded
planes, Y meta, Y, UV meta and UV, which `src/util/prime.cc` computes and
`configure_capture` compares against the node's `sizeimage`. A disagreement is
`VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT`, not a pool of the wrong size.
`vaExportSurfaceHandle` describes the four planes as R8 layers at the kernel's
offsets together with the modifier, and refuses the composed-layer form. There
is no compressor in this stack, so nothing fills one as an encode input.

A surface created over client memory (`vaCreateSurfaces2` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` or the older `_DRM_PRIME`) does not
allocate. `src/util/prime.cc` checks the descriptor against this layout and
the resulting `StableBuffer` is a duplicate of the client's fd, marked
`persistent_export`, so Import and Copy act on it unchanged and it works as
an encode input too. A QCOM_COMPRESSED descriptor must name the same four
planes and offsets the driver would export. A buffer in any other geometry,
modifier or object count is `VA_STATUS_ERROR_INVALID_PARAMETER` at creation:
the firmware would write its own layout into it and neither side would notice
until the picture was wrong.

### 3.5 What is gone

Pre-binding surface *i* to CAPTURE ordinal *i*; the DMA-BUF import path
that queued client buffers into CAPTURE; the scheduled-slot mode; the
no-copy mode; batch sizes above one AU; reset-on-IDR; STREAMOFF-pair reset;
the EOS watchdog thread; `retired_contexts` reaping (a surface holds a
`shared_ptr<Frame>`, so nothing dangles when its context dies);
per-thread current-surface maps; `reference_wrapper` into device vectors.

Client-owned CAPTURE buffers (true zero-copy for Chrome) left the research
track and became the Import publish policy above. The §9
acceptance program found that the firmware does *not* fill CAPTURE buffers
in QBUF order (`test/target/iris-import-probe.cc`, results in TEST-RESULTS.md);
the policy exists because the session queues exactly one client picture at a
time and never skips a submitted picture. Creating a surface over client
memory is not the path that went: it allocates nothing (§3.4) and enters the
same policy from the API.

## 4. Session state machine

```
Closed --open()--> Configured(OUTPUT S_FMT, DISPLAY_DELAY probe, OUTPUT REQBUFS 4, OUTPUT STREAMON)
Configured --first submit + SOURCE_CHANGE--> Streaming(CAPTURE G_FMT/S_FMT, G_SELECTION, MIN_BUFFERS, REQBUFS N, EXPBUF, QBUF all, STREAMON)
Streaming --SOURCE_CHANGE (DRC)--> Draining(implicit: the kernel finishes the pre-change pictures and marks the boundary with LAST) --LAST--> Reconfiguring(STREAMOFF CAPTURE, REQBUFS 0, drop pool, re-enter Streaming with new geometry)
Streaming --sync timeout in display-order mode--> Draining(STOP) --LAST--> Streaming(START)
Streaming --destroyContext--> Finishing(wait pending, STOP, LAST) --> Closed
any --EIO/EPIPE on DQBUF, firmware error--> Failed (every pending surface -> DECODING_ERROR; no retries on this fd)
```

Ordering facts learned on this firmware and kept verbatim:

- OUTPUT `S_FMT` first, CAPTURE format is only meaningful after
  `SOURCE_CHANGE`.
- Every CAPTURE slot is queued before CAPTURE `STREAMON` and stays queued
  except while Held.
- One access unit per OUTPUT buffer with a strictly increasing timestamp;
  SPS/PPS repeated on every H.264 AU; HEVC VPS/SPS/PPS synthesised from VA
  parameters.
- `EBUSY` on `DECODER_CMD_START` with an OUTPUT buffer still queued is a
  recoverable ordering condition (retry after OUTPUT drain), not a dead
  instance. `EIO` and `EPIPE` are.
- A CAPTURE buffer with `bytesused <= data_offset` or the ERROR flag and a
  zero timestamp is a firmware error marker: count it, requeue it, never
  publish it.
- A mid-stream `SOURCE_CHANGE` is already a drain: the kernel returns the
  pictures from before the change, puts `LAST` on the CAPTURE buffer at the
  boundary (`HFI_BUF_FW_FLAG_PSC_LAST`) and stops the decoder. Issuing
  `DECODER_CMD_STOP` on top of it asks for a second drain the firmware never
  finishes, so the session only waits for that `LAST`. The bring-up
  `SOURCE_CHANGE` is not a drain and carries none — the kernel distinguishes
  the two as `IRIS_INST_SUB_FIRST_IPSC` and `IRIS_INST_SUB_DRC_LAST`.
- The CAPTURE format after a resolution change belongs to the driver, not to
  the client. `iris_hfi_gen2_handle_src_change` derives the new coded size,
  stride and crop from the bitstream and hands them over on the resume; an
  `S_FMT` between the drain and `STREAMON` replaces them with whatever
  rectangle the client asked for. So `configure_capture()` takes the `G_FMT`
  answer and issues `S_FMT` only when that answer is not the pixel format the
  client needs (a P010 surface against an NV12 default). This is a contract
  requirement on its own; it is not what makes an in-context AV1 resolution
  change work — see the next bullet.
- An in-context AV1 resolution change works only if `CAPTURE` is streaming
  before the rebuilt pool is queued. The kernel keeps a DRC boundary sub-state
  (`DRC | DRC_LAST`) after the firmware's `PSC_LAST` and clears it inside
  `iris_process_streamon_output`; a `CAPTURE` `QBUF` that reaches
  `iris_vb2_buf_queue` while those bits are still set is not handed to the
  firmware — the driver completes it on the spot as an empty
  `V4L2_BUF_FLAG_LAST` buffer, which makes vb2 set `q->last_buffer_dequeued`
  and turns every later `CAPTURE` `DQBUF` into `-EPIPE`. Queueing before
  `STREAMON` therefore loses the whole new segment, and whether the race is
  lost varies from run to run: the boundary access unit also completes once
  with `ERROR|LAST`, and that is what makes the failure intermittent rather
  than deterministic. `enter_streaming()` streams `CAPTURE` first, for both
  Mmap and Import memories; queueing before `STREAMON` failed 2 of 6 framemd5
  runs and this order passes 44 of 44. In a passing run the boundary
  access unit comes back a second time as `0x4009` (`KEYFRAME`, real pixels).
  The kernel sub-state transitions (`0x16` -> `0x42` -> `0`) are the same in
  both cases, which is why the defect could not be seen there.
- VP9 in-context resolution change and VP9 context recreation rebooted the
  device on display-order firmware; VP9 runs under
  `scripts/iris-experiment-guard.sh` only on that firmware.
- `STOP` completes by writing `LAST` into a CAPTURE buffer, and a Direct
  completion pins its slot until the client releases the surface. A pool
  sized to exactly the number of surfaces the client creates therefore has
  no buffer left for `LAST`: the drain waits out its bound and the session
  fails with `no LAST after STOP`. Measured at 1080p with FFmpeg, whose
  VA-API 1.x path declares zero surfaces and then creates fifteen for HEVC
  against a fifteen-slot pool; H.264's twelve survived a fourteen-slot one.
  The pool is sized from the client's declared count, or seventeen when it
  declares none, plus four, so it always exceeds what the client can pin.
  `drain begin` records `queued`, `held` and `pool` for exactly this
  question.

### 4.1 Encode Session Architecture

The encoder operates as a secondary V4L2 M2M node with a dedicated execution model. Picture encoding is separated across two stages:
1. `vaEndPicture` translates sequence and picture parameters into V4L2 controls, commits the raw input surface, and returns a tracking ticket.
2. The firmware releases the input buffer as soon as it has read the picture (`OUTPUT` `DQBUF`), and that is what `vaSyncSurface` on the input surface waits for. The coded access unit comes later and is claimed by `vaSyncBuffer` on the coded buffer or by `vaMapBuffer` on it. Keeping the two apart is what lets a client with `-async_depth` pipeline: with one wait, every input sync paid for a whole picture.

This decoupling enables asynchronous submission in clients such as FFmpeg.

Low latency is not a firmware switch here. The encoder side of Qualcomm's
open driver has no low-latency HFI property (only its CBR path sets one
implicitly), and this session already submits every picture as it arrives,
holds none for batching, and hands each completion to the client as soon as
the CAPTURE queue reports it. What remains is the client's own pipeline depth
(`-async_depth`), so the measurable quantity is submission-to-completion
time: `encode frame … latency_us=` reports it, and
`test/target/iris-encode-latency.sh` records the first picture, p50 and p95.

The firmware has no encoder low-latency property of its own: no gen2 property
header on SM8550 defines one. Qualcomm's own driver maps low latency to
`HFI_PROP_STAGE` (0x0300010a) = `STAGE_1`, which the firmware does carry, and
forces it under CBR or slice-delivery; the firmware turns it off above 4K and
with B pictures. The kernel's `iris_set_stage()` sends `STAGE_2` for the
encoder unconditionally, so the node never asks for it. Enabling it would take
a kernel patch, a node control and a gate, none of which is in this tree.

#### State Transitions

```
Closed -> Configured -> Streaming -> Finishing -> Closed
```

- **Closed to Configured**: Triggered on first `vaEndPicture`. Negotiates CAPTURE format (coded bitstream) and OUTPUT format (raw NV12), applies initial controls, requests 4 buffers per queue, and initiates `STREAMON`.
- **Configured / Streaming to Streaming**: Submits raw buffer via `QBUF` to OUTPUT queue and issues an execution ticket.
- **Claiming**: Captures completed buffers from CAPTURE queue via `claim_coded()`. Once claimed, subsequent sync/map calls for the same ticket return cached buffer data without redundant dequeuing.
- **Finishing to Closed**: Context destruction issues `V4L2_ENC_CMD_STOP` and settles all outstanding tickets.

#### Driver Invariants

- **Attribution**: A completion is matched to its picture by the timestamp the firmware echoes, because B pictures come back ahead of the reference submitted before them. A zero echo falls back to submission order.
- **Queue Sizing**: CAPTURE queue negotiation occurs strictly after rate-control and bitrate configuration to guarantee proper buffer allocation sizing by firmware.
- **Crop Alignment**: Non-mplane OUTPUT selection applies `V4L2_SEL_TGT_CROP`, aligning dimensions to codec macroblock boundaries (16 for H.264, 32 for HEVC) to avoid geometry distortion.
- **Picture Geometry**: The coded picture is the geometry the client declared at `vaCreateContext`, narrowed for H.264 by the sequence's frame crop; FFmpeg creates a macroblock-aligned H.264 context (1920x1088) for a 1920x1080 picture, while its HEVC context already carries the visible size. The input surface may be larger than the picture (an aligned decode allocation) and is queued without a copy, so the narrowing stops where the encoder's own raw layout would move the chroma plane; there the surface geometry stands and the decoded layout stays the one the encoder reads.
- **IDR Mechanisms**: Direct keyframe generation utilizes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` when supported by the kernel module. On stock modules, keyframes are generated by temporarily setting `GOP_SIZE` to zero before submission and restoring the period immediately afterward. Parameter sets (SPS/PPS) are prefixed when firmware omits them on forced frames.
- **Packed headers**: a raw packed header (`VAEncPackedHeaderRawData`) is copied at `vaRenderPicture`, because the client may destroy that VA buffer when `vaEndPicture` returns, and is spliced into the access unit behind the parameter sets and in front of the first coded-picture NAL. Sequence and slice packed headers are refused: the firmware writes both itself. The payload is not interpreted, only bounded and required to start a NAL. `VAConfigAttribEncPackedHeaders` answers `MISC` for HEVC encode only: FFmpeg's `h264_vaapi` writes a buffering-period SEI that names the SPS by id and relies on the client's SPS, which with only `MISC` advertised it never writes, so H.264 answers no type and keeps the firmware's own headers.
- **ROI**: opt-in (`V4L2_VA_ROI`) and requires the 0017 kernel patch. `VAEncMiscParameterTypeROI` rectangles are copied at `vaRenderPicture` (the client pointer dies at `vaEndPicture`), mapped onto the coded picture's LCU grid (16x16 H.264, 32x32 HEVC; on overlap the first region wins, the delta is clamped to -31..30), and written to the module's dynamic s32 array control before the picture is queued. `configure` arms one all-zero map before the queues stream: the module builds one metadata buffer per control write, subscribes the firmware's metadata delivery while the session starts, and attaches the armed buffer to the next queued picture, so the map has to be in place before `STREAMON`. The qualified firmware completes the metadata buffer without error and codes a byte-identical picture, so `VAConfigAttribEncROI` is not advertised without the opt-in (TEST-RESULTS.md).

## 5. Module layout

```
src/
  va/                 VA entry points; thin, no ioctl, no policy
    driver.h          the object tables (DriverData) and every entry point's
                      declaration
    driver.cc         vtable, init/terminate, capability probe; one recursive
                      mutex for tables only
    tables.cc         id -> object lookups (surface, context, buffer, owner)
    config.cc         profiles/entrypoints from the capability table
    surface.cc        create/destroy/sync/query/export/derive/getImage
    picture.cc        begin/render/end -> codec translator -> Session::submit,
                      or for an EncSlice context -> EncoderSession::submit,
                      or for a VideoProc context -> iris::vpp::scale_nv12_or_p010
                      or iris::vpp::tonemap_hdr10_to_sdr
                      or iris::vpp::overlay
    vpp.cc            the three vtable_vpp queries over ABI-free answer
                      functions taking DriverData&; no GPU probe
    image.cc          images and subpictures (minimal); derive aliases a surface
    buffer.cc         VA buffers, and a coded buffer's access unit claimed once
    report.cc         exception -> VAStatus, in its own unit so a test can link
                      one VA source file without the driver
    grain.{h,cc}      AV1 film grain synthesis into the display surface: the
                      one pixel algorithm in va/, here because it reads VA's
                      grain structure and an iris::Layout
  iris/               the only code that touches /dev/videoN
    device.{h,cc}     Device interface (ioctl/poll/mmap virtuals) + real implementation;
                      discovery by `iris_driver` QUERYCAP name, decoder and
                      encoder nodes, roles told apart by which queue carries
                      the compressed format
    session.{h,cc}    Session: the only ioctl sequencer (state machine of §4).
    session_submit.cc access units into OUTPUT, the one-buffer Import window,
                      token bookkeeping and release
    session_publish.cc servicing both queues, completion handling, publish, wait
    session_internal.h constants the three session units share; not interface
    encode.{h,cc}     EncoderSession: configure, submit/wait split, STOP/drain
                      on finish (§4.1)
    slots.{h,cc}      Frame + SlotPool: allocate, export, requeue, and the
                      Import queue (queue_capture_import, one window slot)
    publish.{h,cc}    CopyEngine interface + CPU engine
    gpu_copy.{h,cc}   EGL/GLES blit on one worker thread, view cache by Frame id
    alloc.{h,cc}      StableBuffer allocation (MSM GEM -> dma-heap), or a
                      duplicate of a client's DMA-BUF for an imported surface
    msm_alloc.c       the MSM GEM ioctl, in C because msm_drm.h names a field
                      `or`
    vpp/processor.{h,cc} optional Vulkan compute video processor: imports the
                      surface DMA-BUFs and runs one pinned kernel over them
                      (scale with crop and rotation, tone map, and the two
                      composites)
    vpp/nv12_scale_spv.h vpp/p010_scale_spv.h vpp/tonemap_spv.h
    vpp/overlay_spv.h vpp/overlay_rgb_spv.h
                      the pinned SPIR-V as arrays, generated
    vpp/shaders/      the kernels' GLSL, the SPIR-V compiled from it and
                      pinned in SHA256SUMS, make-spv.sh (host, glslang) and
                      spv-to-header.py, which writes the arrays above;
                      copy.comp is the prerequisite gate's and is not embedded
  codec/              VA parameters -> bytestream the firmware accepts; pure functions
    translator.h      the Translator interface and one factory per codec
    bitwriter.h
    h264.cc           SPS/PPS synthesis, AU assembly
    hevc.cc           VPS/SPS/PPS/RPS synthesis (fix PTL from profile, LTR, non-uniform tiles)
    vp9.cc            raw frame passthrough, superframe for invisible frames
    av1.cc av1_obu.{h,cc} VA tiles -> OBU temporal unit
    av1_dpb.{h,cc}    the default AV1 translator: an internal eight-slot DPB,
                      a hidden install frame when all eight are live
    encode.h          EncodePicture + Encoder interface shared by both codecs
    encode_h264.cc    VA encode buffers -> EncodeParams, H.264 menu values
    encode_hevc.cc    the same for HEVC, Main only
    annexb.{h,cc}     NAL splitting and the SPS/PPS a requested IDR needs
  util/
    options.{h,cc}    every environment variable, read once at init
    prime.{h,cc}      the CAPTURE layout in one place, and the client DMA-BUF
                      import rules (DRM_PRIME_2 descriptor, PRIME_1 table)
    encode-params.h   EncodeParams/RateMode: the contract iris/ and codec/ share
                      without including each other
    trace.h           stable trace vocabulary (§8); every record ends ` t=<ms>`
    error.h
    fourcc.h          fourccs the vendored kernel headers may predate
test/
  unit/               host-only: slot lifetime, publish policy with FakeDevice, session ordering
                      with scripted DQBUF/event sequences, codec golden bitstreams,
                      encode parameter translation
  iris-*.sh           tablet gates: matrix (framemd5), structure matrix, DRC, soak,
                      browser acceptance, concurrency soak; test/target/iris-import-probe.cc
                      is the client-owned-buffer acceptance program (§9), and
                      iris-va-import-probe.cc the same memory entered through
                      vaCreateSurfaces2
  remote/             deploy, provenance, cdp
scripts/
  lib/iris-node.sh    the only place the node-resolution rule lives; sourced by
                      both diagnostics, so neither can drift from the other
  iris-device-check.sh  read-only device report (nodes, firmware, driver hash,
                      coded formats, optional kernel controls, libva probe)
  iris-verify.sh      user-facing decode/encode check against the IRQ counter
  iris-configure-clients.sh  opt-in mpv/Firefox configuration in one user's home
  install-system.sh / uninstall-system.sh  the one install path both packages use
```

Each layer includes only the ones below it: `util/` none, `codec/` only
`util/`, `iris/` `util/` and the single header `codec/annexb.h` (the encoder
session reassembles access units with its splitter), `va/` all three. A
translator never reaches the session and the session never reaches a
translator. `test/static/iris-layering-check.sh` enforces the direction, and that
the list above names every file under `src/`. `context.cc` and the stateless
request-API classes do
not survive; MPEG2/VP8 translators leave with them (no Iris node advertises
those formats).

The video processor is a fourth context kind but not a fourth node:
`VAProfileNone` / `VAEntrypointVideoProc` opens no V4L2 node and owns no
session. `vaEndPicture` resolves the two surface DMA-BUFs and runs the pinned
scale kernel for the surface's sample width at the requested ratio over the
surface region the client names, after the clockwise quarter turn and the
mirroring the client names — VA orders rotation, then mirroring, then the
scale — sampling where swscale samples. The interpolation method comes from
`filter_flags`: the unspecified method and the nearest-neighbour flag are both
the point map, `VA_FILTER_INTERPOLATION_BILINEAR` blends the four samples
around the destination sample's centre, and `VA_FILTER_INTERPOLATION_ADVANCED`
is the driver's own per-axis pick — the area average when an axis shrinks,
which is the only one of them that does not alias, and a four-tap Catmull-Rom
cubic when it grows. The pipeline caps name advanced: the field carries one
method rather than a bit set, and nearest | bilinear is the advanced value, so
the word cannot describe a set and the highest method the driver runs is what
a client that wants the best one asks for. Every method is written as up to
four (index, weight) taps per axis and evaluated as one outer product, so the
two axes are chosen independently and the separable order does not change the
result. On a P010 surface the blend runs on the ten-bit code and not on the
padded word, so a mixed sample keeps the low six bits clear. The kernel is
selected by the destination's sample width, and the source's depth is a push
constant: a source of the other depth is converted before any filtering, which
is `code >> 2` coming down to eight bits and `code << 2` going up to ten — the
conversion swscale applies, exact in both directions for a picture whose low
two bits are clear. Because a point map picks one source sample, converting
before the pick and converting after it are the same sample, so a cross-depth
picture is byte-identical to `scale` then `format`. A picture that names the HDR10 tone mapper instead runs
`iris::vpp::tonemap_hdr10_to_sdr` over the two DMA-BUFs and converts P010
BT.2020/PQ to NV12 BT.709 at the surface's own size; a picture whose later
pipeline buffers name a `VABlendState` first copies its main rectangle into the
output — the scale kernel at 1:1, since a blended picture does not scale its
main pass — and then runs one `iris::vpp::overlay` per layer over three
DMA-BUFs, the output standing as the main term and the destination at once, so
each layer mixes onto the pixels the previous one left. The mix runs on the
coded values of both planes — or, when the overlay surface is a packed `VA_RT_FORMAT_RGB32` surface
(`VA_FOURCC_BGRA` or `VA_FOURCC_RGBA`, the shape `overlay_vaapi` builds),
`iris::vpp::overlay_rgb`, which converts every sampled overlay pixel to the
output frame's Y'CbCr matrix before mixing; every other pipeline operation
(luma keying, more than sixteen blending passes, a blending pass rendered
before the main one, any second filter, any colour conversion outside that one
filter, and a mirror or a rotation in a tone-mapped or blended picture) fails
the picture instead of being dropped. Both blends implement the
global alpha of `VABlendState` and `VA_BLEND_PREMULTIPLIED_ALPHA`, which
FFmpeg sets whenever its overlay input carries an alpha channel; with that flag
a packed overlay's own alpha byte multiplies the global one, so the layer's
coverage is `alpha * a` and the mix is `alpha * overlay + (1 - alpha * a) *
main` — the global factor stays on the overlay term because a stored colour
carries only the pixel's own coverage, and leaving it out would composite an
opaque image at full strength whatever `alpha` the client named. The overlay is
sampled from its own surface region with the same tap model the scale kernel
runs, and the output is encoded in the range `output_color_properties` names,
full or limited. The decode lives on the
V4L2 node and the kernel on Vulkan, so `vaEndPicture` waits for each source
surface's pending decode before it reads that surface — a client that issues
the processor's picture straight behind the decode (FFmpeg's `overlay_vaapi`
does) would otherwise composite a black frame — and a source whose decode never
lands fails the picture. The entrypoint is advertised only when the
build found Vulkan and the client did not set `V4L2_VA_VPP=0` (§6), so an
installed decode/encode client sees the surface it qualified against. The kernel and the
VA surface path are gated separately in §9:
`test/target/vulkan-vpp/nv12_scale_gate.sh` and `test/target/vulkan-vpp/p010_scale_gate.sh`
check every pixel of the primitive, for both interpolation methods — the point
map byte for byte, the bilinear blend within one code of its model plus a
constant picture and a 1:1 scale as the two invariants the weights must satisfy
— and
`test/target/vulkan-vpp/scale_vaapi_gate.sh` checks a `scale_vaapi` + `hwdownload`
frame against swscale; `test/target/vulkan-vpp/crop_vaapi_gate.sh` checks a
`surface_region` picture against `crop,scale=...:flags=neighbor`, and
`test/target/vulkan-vpp/transpose_vaapi_gate.sh` checks all seven `transpose_vaapi`
directions — the three quarter turns and the four mirrored ones — against the
software filters, `test/target/vulkan-vpp/tonemap_gate.sh` checks the tone
mapper against the pipeline `tonemap.comp` documents, and
`test/target/vulkan-vpp/overlay_gate.sh` checks the composite against the mix
`overlay.comp` documents plus both of its endpoint frames byte for byte.

### Bounds and refusals

The video processor implements strict bounds and explicit refusals (returning a
VA error status instead of dropping operations silently):

- **Geometry floor**: The minimum scaling geometry is 2x2 (the Vulkan kernel
  limit), allowing scaling down to thumbnail size independent of the V4L2
  decoder node's minimum dimensions. Odd geometries and out-of-bounds crops are
  refused.
- **Scaling & crop**: `scale_vaapi` with nearest-neighbour is byte-identical to
  libswscale `flags=neighbor`. The bilinear method matches within 1 code of a
  four-tap double-precision model, invariant on constant surfaces and identical
  to 1:1 point sampling. `crop_vaapi` matches `crop,scale` byte for byte.
- **Transpose**: All seven `transpose_vaapi` directions (`clock`, `cclock`,
  `reversal`, `hflip`, `vflip`, `clock_flip`, `cclock_flip`) match software
  filters byte for byte across NV12 and P010.
- **Bit-depth conversion**: NV12 to P010 (`code << 2`) and P010 to NV12
  (`code >> 2`) conversions match swscale shifts without dithering. A picture
  combining scaling and depth conversion matches `scale` then `format` byte for
  byte in both directions.
- **HDR10 tone mapping**: `VAProcFilterHighDynamicRangeToneMapping` maps P010
  BT.2020/PQ to NV12 BT.709 via the BT.2390 EETF, within 1 code of the
  documented pipeline. The mapper converts the surface at native size first;
  subsequent passes in the same picture (crop, scale, transpose, or compositing)
  operate on the converted frame and match within 1 code.
- **Compositing**: Stacks up to sixteen blending layers in submission order.
  Alpha 0 and alpha 1 endpoints match main and overlay surfaces byte-exact.
  Constant overlay uploads via packed BGRA produce exact BT.709 codes.
- **Explicit refusals**:
  - Luma keying (`VA_BLEND_LUMA_KEY`).
  - More than sixteen blending passes, or a first pass that blends. A blend
    state that sets no flag on the first pass is accepted as the main pass:
    GStreamer's compositor attaches one to every layer.
  - Any second filter or multiple filters in one picture.
  - HDR-to-HDR tone mapping, or NV12 input to the HDR tone mapper.
  - Colour conversion outside the HDR10-to-SDR tone mapper.
  - Scaled `vaPutImage`.
  - Blending passes with missing or invalid output regions.
  - Output surface aliasing an input surface through the same DMA-BUF (`st_dev`/`st_ino`).

## 6. Environment surface

Read once in `VA_DRIVER_INIT_FUNC`, stored in `Options`, passed down.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Decoder node override (kept for launcher/packaging compatibility). |
| `LIBVA_V4L2_ENCODER_PATH` | probe | Encoder node override. When set, a path that is not an Iris encoder is an init error instead of an absent entrypoint. |
| `V4L2_VA_TRACE` | off | Trace to stderr. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface` wait; first frame of a cold session gets `30000`. |
| `V4L2_VA_COPY` | `gpu` | Copy engine for persistent exports: `gpu` or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy`, `direct` or `import` for diagnostics. |
| `V4L2_VA_VPP` | on | Set to `0` to remove the `VAProfileNone` / `VAEntrypointVideoProc` entrypoint. On keeps the decode/encode surface unchanged. |
| `V4L2_VA_B_FRAMES` | off | Opt in to B pictures in the H.264 encoder; needs three pictures in flight. |
| `V4L2_VA_ROI` | off | Advertise ROI regions; needs kernel patch 0017, and the firmware ignores the map. |
| `V4L2_VA_UBWC` | on | Set to `0` to stop offering Q08C surfaces to clients that request `QCOM_COMPRESSED`. |
| `V4L2_VA_HW_GRAIN` | off | Let the firmware apply AV1 film grain from the rebuilt header and publish the grained picture into `current_display_picture`. Off keeps the software blend (`src/va/grain.cc`). |
| `V4L2_VA_DUMP` | off | Directory for submitted access units. |

Twelve switches. `test/static/iris-env-doc-check.sh` keeps README and `options.cc` in
sync by name, not by sentence.

## 7. Threading and locking

- `Driver::mutex` guards the object tables and nothing else. It is never
  held across a wait, a drain or an ioctl that can block.
- `Session::mutex` serialises submit/service/sync/finish for one context.
  `sync` releases it while polling the fd.
- The driver spawns no timers and no watchdog threads. `querySurfaceStatus`
  and `syncSurface` drive the service pass (`pump`); a client that never
  calls either never needs completions. The GPU copy engine owns exactly
  one worker thread because all EGL state lives on it (arbitrary VA entry
  points arrive from several client threads); it is joined in the engine's
  destructor.
- VA entry points are reentrant per context from different threads only in
  the way libva already allows; `beginPicture..endPicture` state lives in the
  Context, not in thread-local storage.

## 8. Trace vocabulary

Hardware checkers (`iris-eos-check.sh`, `iris-ending-check.sh`,
`iris-context-retirement-check.sh`, `iris-zero-copy-suite.sh`,
`iris-concurrency-soak.sh`) parse trace lines. These records are an API;
renaming one requires updating its consumer in the same commit. Every
record ends with ` t=<ms>` since the first record of the process. A record
is one atomic write under a process-wide mutex: two separate writes let a
concurrent context splice `publish=invisibleva end_picture` into the log,
which loses a vocabulary entry and fails the clip checker. The body is sized
from `vsnprintf`'s return value, not a fixed buffer: a long
`import rejected: <why>` record must keep its trailing fields intact.
`test/unit/trace-test.cc` covers concurrent writers and a record over 1024 bytes.

```
session open node=<path> mode=decode-order|display-order codec=<fourcc> <w>x<h> au=<n> output=<n>
submit token=<n> bytes=<n> output=<i>
capture configure <w>x<h> stride=<n> size=<n> visible=<w>x<h> pool=<n> memory=mmap|import firmware-min=<n>
capture token=<n> index=<i> flags=<hex> publish=direct|copy-gpu|copy-cpu|import latency_us=<n>
                                                                     (latency_us runs from the
                                                                      access unit reaching the
                                                                      OUTPUT queue to this
                                                                      completion, the same
                                                                      submit-to-completion span
                                                                      the encoder reports)
capture noshow token=<n> index=<i> flags=<hex> publish=direct|copy-gpu|copy-cpu|import latency_us=<n>
                                                                     (AV1 hidden picture: the node
                                                                      returns it with no pixels, no
                                                                      picture-type bit and no payload;
                                                                      published, attributed by its
                                                                      timestamp when the kernel keeps
                                                                      one, else by decode order)
capture token=<n> index=<i> flags=<hex> publish=drop                 (released target; not published)
capture token=<n> index=<i> flags=<hex> publish=ubwc-copy-refused  (UBWC picture whose Import
                                                                     request degraded to Copy — the
                                                                     client's layout differed from the
                                                                     node's. The picture fails instead of
                                                                     copying compressed bytes into a
                                                                     different allocation)
capture token=<n> index=<i> flags=<hex> publish=invisible            (VP9 alt-ref or AV1 hidden install: first of two completions under one token, ERROR with no picture-type bit and no payload; Target::invisible counts it so the second NOSHOW half still publishes the client picture. A released target leaves this completion orphaned; it is still invisible, not a decode error. A visible payload completion reports the drop; if both halves were NOSHOW, neither does, so drops stays 0 for that abandoned picture)
capture index=<i> flags=<hex> publish=invisible                     (unnamed NOSHOW LAST marker at a DRC boundary; no picture of its own)
capture token=<n> index=<i> flags=<hex> publish=misplaced slot_token=<n>
                                                                     (Import: buffer was queued for
                                                                      another token; both surfaces fail)
capture error index=<i> flags=<hex>                                  (error marker with no token; a
                                                                      stock kernel zeroes the timestamp)
capture error index=<i> flags=<hex> token=<n> publish=drop            (timestamped error whose token
                                                                      is no longer pending)
target detached token=<n>
                                                     (Import target released by the client while
                                                      its picture was still with the firmware; the
                                                      session keeps the target until the completion
                                                      so the picture is published, not dropped)
capture error token=<n> index=<i> flags=<hex> publish=misplaced slot_token=<n>
                                                                     (error completion in Import whose
                                                                      buffer belongs to another token;
                                                                      both surfaces fail)
capture token=<n> index=<i> flags=<hex> publish=error                (firmware failure for its own token)
capture empty index=<i> flags=<hex> last=0|1
capture last
capture format rejected wanted=<fourcc> granted=<fourcc> planes=<n> <w>x<h>
drain begin reason=eos|timeout|finish pending=<n> queued=<n> held=<n> pool=<n>
drain complete last=1 output_queued=<n>
drain lost token=<n>
drc drain begin pending=<n>
drc drain complete last=1
restart
sync timeout token=<n> pending=<n> queued=<n> held=<n> action=drain|fail
session reconfigure <w>x<h> ->
session reconfigure -> <w>x<h>
session failed: <reason>
session finish submitted=<n> completed=<n> errors=<n> drops=<n> timeouts=<n> drains=<n> misplaced=<n> retained=<n>
                                                     (retained counts Import targets the client
                                                      destroyed while their picture was still with
                                                      the firmware: the picture completes into the
                                                      buffer the client exported and is not a drop)
event source-change state=<n>
output error token=<n> output=<i>
import token=<n> index=<i> fd=<n> backlog=<n> abandoned=0|1 lent=<n>/<n>
                                                     (lent counts this
                                                      picture's buffers with
                                                      the firmware; a VP9
                                                      alt-ref superframe
                                                      reaches 2/2)
import rejected: <why> (client stride=<n> chroma=<n> size=<n>; capture stride=<n> chroma=<n> size=<n>)
                                                                     (after an in-context DRC: the
                                                                      client kept exporting the old
                                                                      geometry, so the rebuilt pool
                                                                      uses Mmap and the pictures
                                                                      publish with copy-gpu;
                                                                      test/target/iris-clip-trace-check.sh
                                                                      requires this line before
                                                                      accepting those copies)
import fence token=<n> fd=<n> waited=<n>ms          (only when the wait exceeds 20 ms)
import token=<n> failed: no client buffer
va create_surfaces count=<n> <w>x<h> fourcc=<fourcc> imported=0|1 fd=<n>
                                                     (a UBWC surface appends
                                                     modifier=qcom_compressed)
va ubwc refused operation=<name>                     (a linear-pixel path met a
                                                     compressed surface and
                                                     answered
                                                     VA_STATUS_ERROR_UNIMPLEMENTED;
                                                     <name> is the entry point:
                                                     vaDeriveImage, vaGetImage,
                                                     vaPutImage, film grain,
                                                     video processing, the
                                                     encoder, or the stable-buffer
                                                     allocation)
va create_context id=<n> profile=<id> <w>x<h> targets=<n> mode=<name>
va create_context id=<n> profile=<id> <w>x<h> entrypoint=encslice codec=<fourcc>
va create_context id=<n> profile=-1 <w>x<h> entrypoint=videoproc
va destroy_context id=<n> pending=<n>
va destroy_context id=<n> entrypoint=encslice frames=<n>
va destroy_context id=<n> entrypoint=videoproc
va begin_picture ctx=<n> surface=<n>
va begin_picture ctx=<n> surface=<n> entrypoint=encslice
va begin_picture ctx=<n> surface=<n> entrypoint=videoproc
va end_picture ctx=<n> surface=<n> bytes=<n> seq_start=0|1
va end_picture ctx=<n> surface=<n> entrypoint=encslice submitted token=<n>
va end_picture ctx=<n> surface=<n> entrypoint=videoproc src=<n> <w>x<h>-><w>x<h>
                                                      [ tonemap=hdr10]
                                                      [ overlay=<src>
                                                        <w>x<h>+<x>,<y>
                                                        alpha=<0..1>
                                                        [source=bgra|rgba]
                                                        [premultiplied]
                                                        [layers=<n>]]
                                                      (the suffix names the
                                                       one operation beyond
                                                       the scale: the tone
                                                       mapper, or the blended
                                                       overlay surface and
                                                       the rectangle it was
                                                       drawn into. `source=`
                                                       marks a packed overlay
                                                       and names its byte
                                                       order; `premultiplied`
                                                       marks a stored-alpha
                                                       blend; `layers=`
                                                       appears only for a
                                                       stack and gives the
                                                       count.)
encode open node=<path> codec=<fourcc> raw=<fourcc> depth=<8|10> <w>x<h>
            crop=<w>x<h> mode=cbr|vbr|cqp gop=<n> rate=<num>/<den>
                                                       (raw is the format the
                                                       raw port accepted: NV12
                                                       or P010)
encode frame index=<n> bytes=<n> idr=0|1 promoted=0|1 latency_us=<n> first=0|1
                                                       (idr=1 covers both the
                                                       client's idr_pic_flag and
                                                       an intra picture without
                                                       it; promoted=1 marks the
                                                       second case, which is the
                                                       only record that the
                                                       request was rewritten
                                                       rather than honoured;
                                                       latency_us runs from the
                                                       client's vaEndPicture to
                                                       the access unit, so it
                                                       includes the pipeline
                                                       depth the client asked
                                                       for, and first=1 marks
                                                       the session's first
                                                       picture, whose firmware
                                                       bring-up is not
                                                       representative)
encode packed header bytes=<n> count=<n>                (the client's raw
                                                       packed headers, spliced
                                                       into the access unit
                                                       behind the parameter
                                                       sets and in front of
                                                       the first slice; the
                                                       SEI path FFmpeg's HEVC
                                                       encoder uses)
encode input fence fd=<n> waited=<ms>                 (only when the input's
                                                       write fence was pending
                                                       20 ms or longer)
encode deblocking mode=<0|1|2> alpha=<n> beta=<n> via=default|V4L2
encode intra-refresh period=<n>                        (pictures one rolling
                                                       sweep takes; the
                                                       firmware honours it
                                                       under CBR only)
encode idr lever=sync-frame|intra-period               (which requested-IDR
                                                       lever this kernel has)
encode slices=<n> mode=max_mb mb=<n>                   (the client's slice
                                                       count as the firmware's
                                                       macroblock budget; the
                                                       firmware picks the
                                                       boundaries)
encode temporal layers=<n>                            (the client's temporal
                                                       layer count; the
                                                       firmware derives the
                                                       membership)
encode roi regions=<n> cells=<n>                      (a picture carrying ROI
                                                       regions and the
                                                       macroblock cells the
                                                       map covers; a picture
                                                       without regions after
                                                       one with them still
                                                       writes a zero map and
                                                       leaves no record)
encode drain result=complete
encode drain result=timeout outstanding=<n>
encode drain marker
encode drain stop failed: <reason>
encode drain failed: <reason>
encode error completion index=<i> flags=<hex>
encode completion timestamp=<n> matches no pending picture
encode streamoff failed: <reason>
encode release failed: <reason>
encode control missing name=<control> id=<hex>        (optional control absent;
                                                       the session continues;
                                                       prepend-spspps-to-idr has no id)
va export_surface id=<n> persistent=0|1 source=frame|stable fd=<n> pitch=<n>
                                                     (a UBWC surface replaces
                                                     the pitch with
                                                     modifier=qcom_compressed
                                                     planes=<n>)
va grain <w>x<h> source=frame|stable                   (a grained AV1 display
                                                      surface's blend ran;
                                                      `source` names where
                                                      the ungrained picture
                                                      came from)
va grain hw surface=<n> display=<n>                    (V4L2_VA_HW_GRAIN: the
                                                      firmware grains the
                                                      picture, so the
                                                      completion lands on
                                                      `display`, not
                                                      `surface`)
```

Four VA-side diagnostics are emitted but intentionally stay out of the
checker-oriented list above: `va init node=<path>`, `va error where=<name>`,
`va sync failed ...` and `va destroy_context encode finish failed: <reason>`.
They name driver startup or teardown failures, not session completions.
These are the accepted exceptions for `test/static/iris-trace-vocab-check.py`, which
compares every trace format prefix in `src/` against the list above.

## 9. Verification and gates

A codec, profile or publish policy is advertised only after its hardware gate
passes and `TEST-RESULTS.md` records the run; anything without a passing gate
stays out of `data/iris-codec-capabilities.json`. Host checks run in CI with
no device. Target gates run on SM8550 under
`scripts/iris-experiment-guard.sh` through `test/remote/gate.sh`, which records
the source commit, driver and module hashes, boot ID, resolved nodes and
guard state beside the command's output.

Host checks are `sh test/run-static-checks.sh`,
`meson test -C build-local --print-errorlogs`, the sanitizer build and
`git diff --check`. The static runner pins the shader binaries, kernel patch
hashes, node-resolution rule, install manifest, source-archive boundary and
the trace vocabulary; `test/meson.build` registers the same host tests CI
requires.

The Import policy accepts only the ownership model that the acceptance
program `test/target/iris-import-probe.cc` can prove: the firmware does not honour
QBUF order when it has several buffers to choose from, so the session queues
exactly one client picture at a time and never skips a submitted picture (one
picture in flight, no holes). A completion whose token does not match its
buffer's token fails both surfaces and is never published.

The UBWC surface is gated the same way. `test/target/iris-ubwc-gate.sh` decodes its
own stream per codec twice, into Q08C and into NV12, and `test/vk-ubwc-read`
imports the exported compressed buffer through
`VK_EXT_image_drm_format_modifier` and requires every luma and chroma sample to
equal the linear decode's. A linear image built from the same reference runs
through the identical plane copy first, so a mismatch cannot be blamed on the
read-back path; the gate also requires the session's CAPTURE layout to be Q08C
with the kernel's four planes and their pitches. `test/iris-va-ubwc-probe`
covers the layer above: a VP9 decode client asks for the modifier in
`vaCreateSurfaces`, exports the four-plane descriptor and the modifier, and
requires `vaDeriveImage`/`vaGetImage`/`vaPutImage` to refuse; the gate runs it
again with `V4L2_VA_UBWC=0`, where a list naming only the compressed modifier
has to be refused and a list carrying a linear fallback has to get `LINEAR`.

Client memory entering through `vaCreateSurfaces2` (DRM_PRIME_2 or PRIME_1,
§3.4) is the same policy reached from the API. Its gates are `prime-import`
(host: the layout and refusal rules) and `test/target/iris-va-import-probe.cc`
(target: the surface aliases the client's bytes and a firmware encode reads
them). The allocation chain behind those surfaces is gated on the target by
`test/target/iris-alloc-chain.sh` (§3.4); it is not a Meson test because every rung
below the first needs an MSM render node.

The geometry and concurrency edges are gated on the target.
`test/target/iris-8k-gate.sh` decodes one finite 7680x4320 clip per qualified codec,
compares every frame with a software framemd5 baseline, and requires the
granted CAPTURE pool to respect the memory budget: 32 8K NV12 frames are
1.5 GiB, more than the video SMMU's shared IOVA space holds, so `STREAMON`
fails with ENOMEM. The cap is eight frames from the 384 MiB budget,
or fifteen when the module reports `capture_sg=Y` and the 720 MiB
system-memory budget applies; the gate reads the same module parameter the
driver reads. `test/target/iris-two-player-gate.sh` runs two independent ffmpeg
processes at 1920x1080, each with its own VA display and driver trace, and
requires both to finish with exact pixels, no completion errors and a terminal
LAST. Neither is a Meson test: both need an Iris node.

The video processor is gated at two levels. `test/target/vulkan-vpp/nv12_scale_gate.sh`
checks the 8-bit nearest kernel byte for byte against a CPU oracle over both
DMA-BUF origins and a matrix of ratios, crops and rotations;
`test/target/vulkan-vpp/p010_scale_gate.sh` does the same for the 10-bit kernel,
including a crop away from the origin, which is the only geometry that
separates a chroma pair at four bytes from one at two;
`test/target/vulkan-vpp/scale_vaapi_gate.sh` checks `scale_vaapi` + `hwdownload`
frames against swscale over the same ratios, in both directions across bit
depth: a P010 source into an NV12 output and an NV12 source into a P010 output
must match `scale` then `format` byte for byte;
`test/target/vulkan-vpp/transpose_vaapi_gate.sh` checks every direction the pipeline
caps advertise — three quarter turns, a horizontal and a vertical mirror, and a
quarter turn with each — against the software filters, in NV12 and P010. Both
VA-level gates check that `V4L2_VA_VPP=0` still refuses the entrypoint.
`test/target/vulkan-vpp/tonemap_gate.sh` drives the one filter through
`iris-va-vpp-probe tonemap`: no FFmpeg in 8.1 can feed `tonemap_vaapi` a raw
P010 frame, so the probe builds the HDR10 metadata buffer itself and compares
every output byte with its own implementation of the documented pipeline,
within 2 codes, plus three properties of the curve that a shared model mistake
would not satisfy (PQ black on 16/128/128, the mastering peak on 235, a
monotone ramp). It also proves the curve follows the metadata by running two
mastering peaks over one frame, and requires an NV12 source, a filter without
its parameters and a rotation in a tone-mapped picture to be refused.
`test/target/vulkan-vpp/overlay_gate.sh` drives `iris-va-vpp-probe overlay`, which
submits the two pipeline buffers overlay_vaapi submits — and, with `--layers`,
a stack of them up to the sixteen the driver accepts: thirty-two geometries
across both sample widths — including a main surface padded wider than its
picture, as a decoder allocates one, packed BGRA and RGBA overlays, the BT.601
matrix, a packed overlay over a tone-mapped main surface, a layer that reads
part of its own surface, all three sampling methods, and a full-range
destination — against the mix `overlay.comp` and `overlay_rgb.comp` document,
within 2 codes, then the two endpoints against the input frames themselves —
an alpha of 0 over the whole surface must reproduce the main frame byte for
byte and an alpha of 1 the overlay frame, which a kernel that read the wrong
plane or ignored the alpha cannot do. Stored alpha gets two checks that do not
come from the model: a premultiplied frame whose alpha byte is opaque has to
land on the same picture as a straight one at the same global alpha, and a
per-pixel alpha that is not opaque has to produce a different one. The three
sampling methods have to produce three different pictures, at a ratio where the
area average and the four-tap blend genuinely differ — at an exact 2:1 they
coincide by construction. Luma keying, a seventeenth pass, a blend before its
base pass, an odd or out-of-bounds rectangle, a crop of the main surface, an
overlay region that leaves its own surface, and an alpha outside 0..1 are
required to be refused. `test/target/vulkan-vpp/overlay_vaapi_gate.sh` then
drives `overlay_vaapi` itself over a BGRA upload, because two things about the
real client are invisible from the probe: FFmpeg asks for
`VA_BLEND_PREMULTIPLIED_ALPHA` whenever its overlay input carries an alpha
channel, and the overlay surface is a `VA_RT_FORMAT_RGB32` surface FFmpeg
fills through the image path. Its frames are single colours, so the expected
output is one BT.709 triple computed in the gate — exact, not within a code —
plus the main frame byte for byte at alpha 0 and the overlay's own codes at
alpha 1. The alpha 0.25 case is what catches the premultiplied form being
wrong: an overlay that is not scaled by the global alpha passes both endpoints
and fails this one. The SPIR-V the driver embeds is
regenerated from the pinned modules and compared by `test/run-static-checks.sh`.

## 10. Invariants (the ones tests exist for)

1. A CAPTURE completion is published to at most one surface, found only by
   exact token match. Unknown tokens are requeued and counted.
2. A slot is QBUF'd only when its use count is one.
3. A persistent export's fd/pitch/offsets never change after the first
   export; pixels are written only after the buffer's implicit fence
   signals.
4. `vaSyncSurface` returns `SUCCESS` only after the surface's token has been
   published; `DECODING_ERROR` if the firmware flagged it; `TIMEDOUT` after
   the bound (decode-order) or after drain still leaves it pending
   (display-order). Never returns `SUCCESS` for pixels that are not there.
   A timestamped error completion fails its surface as soon as it is
   dequeued, so `DECODING_ERROR` does not wait for the bound.
5. No VA entry point sleeps while holding `Driver::mutex`.
6. The node is found by `iris_driver` name and decoder capability; a fixed
   `/dev/videoN` appears nowhere in code, launcher or packaging.
7. Every environment variable is read exactly once, in one file.
8. A firmware error marker (ERROR flag, zero timestamp, or empty payload)
   is never published, with one exception: the node returns an AV1 hidden
   picture exactly that way, and invariant 18 is what tells them apart.
   With the error-timestamp patch a marker keeps its token and fails that
   surface immediately; the token-less form is the stock kernel's.
9. Import keeps exactly one client *picture* with the firmware at a time —
   one buffer per completion it produces — and every submitted picture gets
   its buffer even after its surface was released; a completion whose token
   does not match its buffer's token fails both surfaces and is counted as
   `misplaced`.
10. An encoded picture lands only in the coded buffer the client named in
    `VAEncPictureParameterBuffer{H264,HEVC}::coded_buf`, and only once its
    ticket is claimed (`vaSyncBuffer` on that buffer, or the `vaMapBuffer`
    fallback). `vaSyncSurface` on the input surface is *not* a claim: it
    returns when the firmware has released the input buffer, which happens
    before the access unit exists, so a client that submits ahead is not
    serialised behind its own output wait. A payload larger than the
    buffer's capacity is `ENCODING_ERROR`, never a truncation. A submitted
    picture is never dropped: the drain resolves whatever is still
    outstanding. With B pictures the session queues raw buffers in display
    order, taken from the codec's picture order count, rather than in
    submission order: VA clients submit in coding order and the firmware
    codes the pictures it receives as a display sequence. A picture can
    therefore be queued after a later-submitted one, and up to one picture
    waits in the session for the display predecessor it has to follow. Its
    ticket does not change, and a wait for a picture the session is still
    holding queues it rather than waiting for a submit that will not come.
11. An encode surface is input only. It never carries a decode target, and
    the session queues exactly one raw buffer per picture, so a picture
    cannot be encoded into a buffer that still holds the next one. The
    buffer's pending write fence is waited for before it is queued: the
    kernel attaches no implicit fence to a dmabuf QBUF, so a client that
    rendered into the surface and only flushed would otherwise be encoded
    from stale pixels. The wait is bounded at 2 s and emits `encode input
    fence` only when it exceeded 20 ms.
12. An encode profile is advertised only when the encoder node's CAPTURE
    queue enumerates the codec and its OUTPUT queue enumerates NV12. Both
    lists come from `data/iris-codec-capabilities.json` and the
    `encode-qualified-*` block in `src/va/driver.cc` must match it exactly.
13. A client request the encoder cannot honour fails with
    `VA_STATUS_ERROR_UNIMPLEMENTED`, never a stream that quietly used
    something else. That covers slice deblocking on a kernel without the
    loop-filter controls (the stock SM8550 kernel; §2 lists the patch that
    adds them), more than one slice per picture, tiles and wavefront coding,
    weighted prediction, scaling lists, chroma QP offsets and several
    reference indexes — the firmware writes the sequence headers, owns the
    reference structure and chooses the quantisation, so none of them can be
    forwarded. Deblocking additionally cannot change inside a live context.
14. An encode picture is a requested IDR when the client asked for one, and
    also when the client asked for an intra picture without the IDR flag (a
    non-IDR I picture, or an HEVC CRA): the firmware's only lever emits an
    IDR, so the request is promoted rather than dropped, which would code
    the picture as a P. The session uses
    `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` when the kernel has it, and
    otherwise pulls the intra period to zero for that picture and puts it
    back before queueing it — restoring after the completion made the
    following picture an IDR as well. The lever is never skipped, because a
    driver-side guess at the firmware's own GOP schedule dropped requests.
    `idr=1` in the trace is the request. With B pictures the intra period is
    first rounded down to a whole number of (P, B) groups (1 + n * (B + 1)):
    the firmware's own IDR otherwise lands on a picture inside a pair, and
    the stream is corrupt from that IDR on.
15. An encode timestamp rises strictly: the session rate is fixed at
    configuration time and each picture advances by one frame duration. VA
    clients send the frame rate only with IDR pictures, and a timestamp
    derived per picture therefore went backwards after every GOP boundary,
    which stopped the firmware returning access units.
16. A surface created over client memory is a view of exactly one DMA-BUF in
    the CAPTURE layout — linear with a pitched luma/chroma pair, or
    QCOM_COMPRESSED with the four Q08C planes at the kernel's offsets; the
    driver owns a duplicate of the fd and the client may close its own. Any
    other geometry, modifier or object count is
    `VA_STATUS_ERROR_INVALID_PARAMETER` at creation, never a picture decoded
    into the wrong pixels.
17. An AV1 picture is submitted at `vaEndPicture`. The internal eight-slot
    DPB assigns refresh slots itself and rewrites `ref_frame_idx[]` to those
    slot numbers, so no successor is needed to name refreshed slots. When all
    eight slots are still referenced, the previous uncommitted picture is
    replayed as a hidden NOSHOW frame in the same temporal unit before the
    shown frame; `AccessUnit::completions = 2` and both completions carry one
    token. The first NOSHOW half is invisible and consumes one Import buffer
    without publishing pixels; `Target::invisible` counts it so the second
    half is always the client picture, including when that picture's own
    `show_frame=0` makes both halves NOSHOW. A NOSHOW LAST marker at a DRC
    boundary names no picture and is consumed as invisible. A client
    `show_frame=0` picture is submitted immediately with no client pixels to
    publish. A new key frame clears internal slot identities and starts a
    fresh sequence;
    EOS has no held tail to guess or release. `vaSyncSurface`, `vaGetImage`,
    `vaDeriveImage` and `vaExportSurfaceHandle` wait for the picture's own
    completion, never for a later access unit.
18. An AV1 hidden picture (`show_frame=0`) is published, not failed. The node
    returns it with the ERROR flag, no KEYFRAME/PFRAME/BFRAME bit and zero
    bytes used. On a stock kernel it also has a zero timestamp and only the
    oldest pending target whose `Target::hidden` is set may take it; on a
    kernel with the error-timestamp patch it carries its own token, which
    must match a pending hidden target. Either way an alt-ref can never land
    on a shown picture's surface, and a corrupt shown picture is never
    published as a hidden one.
19. A decode profile's surface depth is decided before the first access unit
    and by the profile, not by the stream. The firmware reads the bit depth
    from the bitstream itself and then refuses any CAPTURE format but the
    matching one, so a session opened on NV12 for a 10-bit stream fails at
    the first picture with nothing to retry. `p010_decode_profile()` in
    `src/va/driver.cc` is the one place that answers this; the config
    attributes, the surface attributes and the capture format read it. The
    capability table lists the resulting P010 capture formats separately.
20. Both publish paths copy the rectangle the source and the destination can
    both hold. A client surface may still carry the pre-change geometry when
    the first picture of a new coded size arrives, because FFmpeg's download
    path reuses its surface pool across a resolution change. `copy_pixels()`
    intersects the client's image rectangle with the buffer's, and the GPU
    engine builds each view from its own layout and copies `min(from, to)`;
    a path that used the destination's size for both failed the whole
    segment with `invalid buffer layout`.
21. The write-combined GEM allocation is a retry for an unsupported cache
    mode and for nothing else. `msm_alloc.c` masks `EINVAL` and
    `EOPNOTSUPP` and propagates every other errno, so a real failure falls
    straight to the DMA heap. Retrying on a real error would either mask it
    or produce a buffer from a mode the caller asked not to use.
    `iris-alloc-chain.sh` walks the chain on the target with a shim that
    injects each failure, and asserts the exact sequence of attempts.
22. An encode stream starts on an intra picture, and every inter picture
    names the immediately preceding picture's reconstruction surface as its
    only L0 reference. The firmware keeps one reconstructed frame and has no
    reference management of its own, so `last_reconstruction_` in the session
    is both the one legal reference and the check: a reference to anything
    else is `VA_STATUS_ERROR_UNIMPLEMENTED`. A picture with
    `reference_pic_flag` clear is refused for the same reason — the next
    picture could only name it, and it will not be there.
23. An encode slice covers the whole picture and is the only one. The
    translator requires `macroblock_address` / `slice_segment_address` at
    zero, an H.264 `num_macroblocks` equal to the sequence's macroblock
    count, an HEVC `num_ctu_in_slice` that is non-zero, I or P slice type
    only, and an IDR paired with an I slice. The HEVC count is not compared
    with the CTU grid: clients send a nominal value and the firmware decides
    the slice layout, so a count that does not cover the grid is accepted
    and coded as the whole picture. An HEVC `coding_type` that disagrees
    with the slice type is `VA_STATUS_ERROR_INVALID_PARAMETER`.

## 11. How work is divided

The `iris/` core, the session state machine and every gate decision live in
this tree and change only with the verification contract in `AGENTS.md`:
each change carries the command that proves it, and the records in
`TEST-RESULTS.md` are updated in the same commit. Work handed to another
implementer (human or tool) is bounded by a written scope with a file
allowlist, a do/don't list and the exact verification command; every diff
is reviewed against §3 to §10 before it lands, and factual claims about
kernel or client behaviour are re-verified on the target before they are
relied on.
