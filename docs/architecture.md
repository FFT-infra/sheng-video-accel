# Architecture

This document is the contract for the rebuild started on 2026-09-20 (tag
`pre-rebuild-20260920`, branch `main`). Every later phase, every
review and every task handed to a worker agent is scoped against it. When
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
NV12 input, Annex-B output, one picture in flight per context.

Non-goals: being a generic backend for stateless request-API decoders (that
is upstream `mxsrc/libva-v4l2`'s job and stays on `master`), VP9, AV1 and
10-bit encode, B-frames, ROI, multi-slice and tiles, `VAEntrypointEncSliceLP`,
video post-processing, and shipping a custom Chromium.

## 2. Target and kernel contract

The qualified target is a Snapdragon SM8550 tablet (`sheng`) on Fedora 44
with a self-built kernel from `ianchb/sm8550-mainline` (Iris is the
`qcom-iris` module, HFI gen2). Because the kernel is self-built, kernel-side
fixes are in scope.

Five Iris kernel changes are adopted as part of the target. The first two
were first applied to a `sheng-7.2.2` tree; after the target moved to
`kernel-sheng-7.2.6-1` the rebased patches and the module-only build recipe
live in `packaging/kernel/`:

| Patch | What it changes | Why the driver needs it |
| --- | --- | --- |
| decode-order output (`V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE`, delay 0) | Firmware returns pictures in decode order instead of display order | A VA client may `vaSyncSurface` a picture before submitting the ones it is reordered behind. Display-order firmware withholds it, and the only userspace escape is a timeout plus STOP/START drain. In decode order every submitted picture completes independently. |
| CAPTURE pool up to 64 buffers | Raises the vb2 `max_num_buffers` for the gen2 decoder CAPTURE queue | Threaded VA clients keep more than 32 surfaces; a driver-owned pool needs headroom above the client's pool. |
| encoder loop-filter controls | Exposes `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA` and the HEVC equivalents, sent as `HFI_PROP_DEBLOCKING_MODE` | VA carries deblocking per slice (mode, alpha, beta); the stock kernel has no control to carry it to the firmware, so a non-default request is refused instead of being silently ignored. |
| encoder `REQUEST_SYNC_FRAME` | Exposes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, sent as `HFI_PROP_REQUEST_SYNC_FRAME` with the sequence-header mode from `PREPEND_SPSPPS_TO_IDR` | Without it a requested IDR is produced by moving the intra period, which costs two control writes per request and leaves the parameter sets for the driver to prefix. |
| timestamp on error completions | `iris_vb2_buffer_done()` copies `buf->timestamp` onto a `V4L2_BUF_FLAG_ERROR` CAPTURE completion instead of zeroing it | The CAPTURE timestamp is the token that names the picture. Zeroed, a corrupt picture cannot be attributed: the target stays pending until a drain or the sync bound, and the client sees `TIMEDOUT` rather than `DECODING_ERROR`. |

The target runs a module built from all four patches
(`TEST-RESULTS.md`, `REQUEST_SYNC_FRAME` on the target). The driver still
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
hardware result (`test/remote/provenance.py` already records `uname`; the
module hash is added in Phase 1).

## 3. The one ownership model

The previous tree carried five CAPTURE ownership modes selected by
environment variables. The rebuild has one model with two publish policies
chosen per surface from observed client behaviour.

### 3.1 Objects

```
Session (one per VA context, one fd)
  owns  OUTPUT pool   : 4 MMAP slots, one access unit each
  owns  CAPTURE pool  : N slots, N = clamp(surfaces + 4, 32, kernel max).
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
  after this point hands out the slot's own DMA-BUF.
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
  and failures: TEST-RESULTS.md, 2026-09-22.

`persistent_export` is set by `vaExportSurfaceHandle`, `vaAcquireBufferHandle`
or a `DRM_PRIME_2` import when the surface has no token yet. It is never set
by an environment variable. `V4L2_VA_PUBLISH=copy|direct|import` exists only
to force a policy in diagnostics and is not read on the hot path.

Whether Import is even offered comes from one place,
`data/iris-codec-capabilities.json` (`publish_import` per codec), enforced
against `src/va/driver.cc` by `test/iris-codec-matrix-check.sh`; the session
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

A surface created over client memory (`vaCreateSurfaces2` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` or the older `_DRM_PRIME`) does not
allocate. `src/util/prime.cc` checks the descriptor against this layout and
the resulting `StableBuffer` is a duplicate of the client's fd, marked
`persistent_export`, so Import and Copy act on it unchanged and it works as
an encode input too. A buffer in any other geometry, modifier or object count
is `VA_STATUS_ERROR_INVALID_PARAMETER` at creation: the firmware would write
its own layout into it and neither side would notice until the picture was
wrong.

### 3.5 What is gone

Pre-binding surface *i* to CAPTURE ordinal *i*; the DMA-BUF import path
that queued client buffers into CAPTURE; the scheduled-slot mode; the
no-copy mode; batch sizes above one AU; reset-on-IDR; STREAMOFF-pair reset;
the EOS watchdog thread; `retired_contexts` reaping (a surface holds a
`shared_ptr<Frame>`, so nothing dangles when its context dies);
per-thread current-surface maps; `reference_wrapper` into device vectors.

Client-owned CAPTURE buffers (true zero-copy for Chrome) left the research
track on 2026-09-22 and became the Import publish policy above. The §9
acceptance program found that the firmware does *not* fill CAPTURE buffers
in QBUF order (`test/iris-import-probe.cc`, results in TEST-RESULTS.md);
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
  Mmap and Import memories; `TEST-RESULTS.md` records 2 of 6 framemd5 runs
  passing before the change and 44 of 44 after. In a passing run the boundary
  access unit comes back a second time as `0x4009` (`KEYFRAME`, real pixels).
  The kernel sub-state transitions (`0x16` -> `0x42` -> `0`) are the same in
  both cases, which is why the defect could not be seen there.
- VP9 in-context resolution change and VP9 context recreation rebooted the
  device on display-order firmware; VP9 bring-up (Phase 5) runs under
  `scripts/iris-experiment-guard.sh` only.

### 4.1 Encode Session Architecture

The encoder operates as a secondary V4L2 M2M node with a dedicated execution model. Picture encoding is separated across two stages:
1. `vaEndPicture` translates sequence and picture parameters into V4L2 controls, commits the raw input surface, and returns a tracking ticket.
2. The coded access unit is claimed downstream by `vaSyncBuffer` on the coded buffer, `vaSyncSurface` on the input surface, or `vaMapBuffer` on the coded buffer.

This decoupling enables asynchronous submission in clients such as FFmpeg.

#### State Transitions

```
Closed -> Configured -> Streaming -> Finishing -> Closed
```

- **Closed to Configured**: Triggered on first `vaEndPicture`. Negotiates CAPTURE format (coded bitstream) and OUTPUT format (raw NV12), applies initial controls, requests 4 buffers per queue, and initiates `STREAMON`.
- **Configured / Streaming to Streaming**: Submits raw buffer via `QBUF` to OUTPUT queue and issues an execution ticket.
- **Claiming**: Captures completed buffers from CAPTURE queue via `claim_coded()`. Once claimed, subsequent sync/map calls for the same ticket return cached buffer data without redundant dequeuing.
- **Finishing to Closed**: Context destruction issues `V4L2_ENC_CMD_STOP` and settles all outstanding tickets.

#### Driver Invariants

- **Attribution**: Completions are attributed in strict FIFO submission order matching hardware processing.
- **Queue Sizing**: CAPTURE queue negotiation occurs strictly after rate-control and bitrate configuration to guarantee proper buffer allocation sizing by firmware.
- **Crop Alignment**: Non-mplane OUTPUT selection applies `V4L2_SEL_TGT_CROP`, aligning dimensions to codec macroblock boundaries (16 for H.264, 32 for HEVC) to avoid geometry distortion.
- **IDR Mechanisms**: Direct keyframe generation utilizes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` when supported by the kernel module. On stock modules, keyframes are generated by temporarily setting `GOP_SIZE` to zero before submission and restoring the period immediately afterward. Parameter sets (SPS/PPS) are prefixed when firmware omits them on forced frames.

## 5. Module layout

```
src/
  va/                 VA entry points; thin, no ioctl, no policy
    driver.cc         vtable, object tables, init/terminate, one recursive mutex for tables only
    config.cc         profiles/entrypoints from the capability table
    surface.cc        create/destroy/sync/query/export/derive/getImage
    picture.cc        begin/render/end -> codec translator -> Session::submit,
                      or for an EncSlice context -> EncoderSession::encode
    image.cc          images and subpictures (minimal); derive aliases a surface
  iris/               the only code that touches /dev/videoN
    device.{h,cc}     Device interface (ioctl/poll/mmap virtuals) + real implementation;
                      discovery by `iris_driver` QUERYCAP name, decoder and
                      encoder nodes, roles told apart by which queue carries
                      the compressed format
    session.{h,cc}    Session: the only ioctl sequencer (state machine of §4),
                      publish dispatch, Import backlog
    encode.{h,cc}     EncoderSession: configure, submit/wait split, STOP/drain
                      on finish (§4.1)
    slots.{h,cc}      Frame + SlotPool: allocate, export, requeue, and the
                      Import queue (queue_capture_import, one window slot)
    publish.{h,cc}    CopyEngine interface + CPU engine
    gpu_copy.{h,cc}   EGL/GLES blit on one worker thread, view cache by Frame id
    alloc.{h,cc}      StableBuffer allocation (MSM GEM -> dma-heap), or a
                      duplicate of a client's DMA-BUF for an imported surface
  codec/              VA parameters -> bytestream the firmware accepts; pure functions
    bitwriter.h
    h264.cc           SPS/PPS synthesis, AU assembly
    hevc.cc           VPS/SPS/PPS/RPS synthesis (fix PTL from profile, LTR, non-uniform tiles)
    vp9.cc            raw frame passthrough, superframe for invisible frames
    av1.cc av1_obu.cc VA tiles -> OBU temporal unit
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
test/
  unit/               host-only: slot lifetime, publish policy with FakeDevice, session ordering
                      with scripted DQBUF/event sequences, codec golden bitstreams,
                      encode parameter translation
  iris-*.sh           tablet gates: matrix (framemd5), structure matrix, DRC, soak,
                      browser acceptance, concurrency soak; test/iris-import-probe.cc
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

`va/` may include `iris/` and `codec/`; `iris/` and `codec/` never include
`va/` or each other. `context.cc` and the stateless request-API classes do
not survive; MPEG2/VP8 translators leave with them (no Iris node advertises
those formats).

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
| `V4L2_VA_DUMP` | off | Directory for submitted access units. |

Seven switches, down from eighteen. `test/iris-env-doc-check.sh` keeps README
and `options.cc` in sync by name, not by sentence.

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
`test/trace-test.cc` covers concurrent writers and a record over 1024 bytes.

```
session open node=<path> mode=decode-order|display-order codec=<fourcc> <w>x<h> au=<n> output=<n>
submit token=<n> bytes=<n> output=<i>
capture configure <w>x<h> stride=<n> size=<n> visible=<w>x<h> pool=<n> memory=mmap|import firmware-min=<n>
capture token=<n> index=<i> flags=<hex> publish=direct|copy-gpu|copy-cpu|import
capture noshow token=<n> index=<i> flags=<hex> publish=direct|copy-gpu|copy-cpu
                                                                     (AV1 hidden picture: the node
                                                                      returns it with no pixels, no
                                                                      picture-type bit and no payload;
                                                                      published, attributed by its
                                                                      timestamp when the kernel keeps
                                                                      one, else by decode order)
capture token=<n> index=<i> flags=<hex> publish=drop                 (released target; not published)
capture token=<n> index=<i> flags=<hex> publish=error                (firmware error; not published)
capture token=<n> index=<i> flags=<hex> publish=invisible            (VP9 alt-ref or AV1 hidden install: first of two completions under one token, ERROR with no picture-type bit and no payload; Target::invisible counts it so the second NOSHOW half still publishes the client picture. A released target leaves this completion orphaned; it is still invisible, not a decode error, and its visible half is the one drop)
capture index=<i> flags=<hex> publish=invisible                     (unnamed NOSHOW LAST marker at a DRC boundary; no picture of its own)
capture token=<n> index=<i> flags=<hex> publish=misplaced slot_token=<n>
                                                                     (Import: buffer was queued for
                                                                      another token; both surfaces fail)
capture error index=<i> flags=<hex>                                  (error marker with no token; a
                                                                      stock kernel zeroes the timestamp)
capture error index=<i> flags=<hex> token=<n> publish=drop            (timestamped error whose token
                                                                      is no longer pending)
capture empty index=<i> flags=<hex> last=0|1
capture last
capture format rejected wanted=<fourcc> granted=<fourcc> planes=<n> <w>x<h>
drain begin reason=eos|timeout|finish pending=<n>
drain complete last=1 output_queued=<n>
drain lost token=<n>
drc drain begin pending=<n>
drc drain complete last=1
restart
sync timeout token=<n> pending=<n> queued=<n> held=<n> action=drain|fail
session reconfigure <w>x<h> ->
session reconfigure deferred held=<n>
session reconfigure -> <w>x<h>
session failed: <reason>
session finish submitted=<n> completed=<n> errors=<n> drops=<n> timeouts=<n> drains=<n> misplaced=<n>
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
                                                                      test/iris-clip-trace-check.sh
                                                                      requires this line before
                                                                      accepting those copies)
import fence token=<n> fd=<n> waited=<n>ms          (only when the wait exceeds 20 ms)
import token=<n> failed: no client buffer
va create_surfaces count=<n> <w>x<h> fourcc=<fourcc> imported=0|1 fd=<n>
va create_context id=<n> profile=<id> <w>x<h> targets=<n> mode=<name>
va create_context id=<n> profile=<id> <w>x<h> entrypoint=encslice codec=<fourcc>
va destroy_context id=<n> pending=<n>
va destroy_context id=<n> entrypoint=encslice frames=<n>
va begin_picture ctx=<n> surface=<n>
va begin_picture ctx=<n> surface=<n> entrypoint=encslice
va end_picture ctx=<n> surface=<n> bytes=<n> seq_start=0|1
va end_picture ctx=<n> surface=<n> entrypoint=encslice bytes=<n>
encode open node=<path> codec=<fourcc> raw=<w>x<h> crop=<w>x<h> mode=cbr|vbr|cqp
            gop=<n> rate=<num>/<den>
encode frame index=<n> bytes=<n> idr=0|1 promoted=0|1    (idr=1 covers both the
                                                       client's idr_pic_flag and
                                                       an intra picture without
                                                       it; promoted=1 marks the
                                                       second case, which is the
                                                       only record that the
                                                       request was rewritten
                                                       rather than honoured)
encode input fence fd=<n> waited=<ms>                 (only when the input's
                                                       write fence was pending
                                                       20 ms or longer)
encode deblocking mode=<0|1|2> alpha=<n> beta=<n> via=default|V4L2
encode idr lever=sync-frame|intra-period               (which requested-IDR
                                                       lever this kernel has)
encode drain result=last|timeout
encode control missing name=<control> id=<hex>        (optional control absent;
                                                       the session continues)
va export_surface id=<n> persistent=0|1 source=frame|stable fd=<n> pitch=<n>
```

## 9. Phases and gates

Each phase is independently mergeable and leaves a shippable driver. Each
ends with a tag. Gates are commands, not opinions.

| Phase | Deliverable | Gate | Tag |
| --- | --- | --- | --- |
| 0 | Branch layout, baseline, acceptance script, this document, tests unpinned from source text | static checks; driver bytes identical; baseline in `TEST-RESULTS.md` | `pre-rebuild-20260920` |
| 1 | Kernel: both Iris patches on `sheng-7.2.2`, `qcom-iris.ko` rebuilt and installed with rollback kept (rebased onto `sheng-7.2.6` later; `packaging/kernel/`) | `display_delay*` controls present; acceptance H.264+HEVC on the unchanged driver; boot ID unchanged | (module only; `ROLLBACK.sh` under the lab dir) |
| — | Kernel track: encoder loop-filter controls (`packaging/kernel/0002-*`), the encoder `REQUEST_SYNC_FRAME` control (`packaging/kernel/0003-*`) and error-completion timestamps (`packaging/kernel/0004-*`), all rebased onto `sheng-7.2.6` | 0002/0003: the six DEBLOCK and one `REQUEST_SYNC_FRAME` capability appear once in `inst_fw_cap_sm8550_enc`, `force_key_frame` is listed by `--list-ctrls` on the loaded module, `encode idr lever=sync-frame`, and the IDR/segment/pipelining runs in `TEST-RESULTS.md` match the GOP record. 0004: a truncated HEVC stream fails its surface at dequeue (`publish=error`) instead of at the drain, a mid-stream corrupt picture does not stop the pictures after it, and the decode matrix (AV1 `noshow=23` included) is unchanged | (not tagged) |
| 2 | `src/iris/` core with the FakeDevice unit suite | `meson test` green on host | (in `phase-3-core-rewired`) |
| 3 | VA glue on the core for H.264 + HEVC, GPU blit engine, old tree deleted | host units; matrix 48/48 both codecs; structure matrix; DRC; 120 s soak; Chrome both codecs; FFmpeg `publish=direct` | `phase-3-core-rewired` |
| 5 | VP9 qualified and advertised; AV1 rebuilt OBU and decode contract (matrix, FFmpeg over alt-ref and no-alt-ref, Chrome) | VP9: matrix, 100 context recreations, 100 DRC switches, Chrome with alt-ref, all guarded. AV1: matrix with `--hidden-pictures`, native-vs-VA frame MD5, Chrome 152 acceptance; qualified 2026-10-22, see §9 and `TEST-RESULTS.md` | `phase-5-vp9-qualified` |
| 6 | Harness/packaging/docs trimmed to the shipped tree | static checks; RPM built and installed; acceptance from the installed package for H.264/HEVC/VP9 | `phase-6-packaged` |
| 7 | 10-bit (HEVC Main10, VP9 Profile 2) qualified; Fluster conformance on the new driver; module identity in provenance | 10-bit framemd5 both codecs; Chrome Main10; Fluster H.264/HEVC driver-path counts recorded against the old tree's | `phase-7-conformance` |
| 8 | `VAEntrypointEncSlice` on the second Iris node: H.264 Constrained Baseline/Main/High and HEVC Main, CBR/VBR/CQP, NV12 in, Annex-B out | `vainfo` lists `VAEntrypointEncSlice`; FFmpeg's 300-frame GOP-60 acceptance for both codecs (CQP, no bitrate) with keyframes at 0/60/120/180/240, software-identical decode and an IRQ delta, and the decode matrix re-run unchanged; `encode-session` for the deblocking refusal a kernel without loop-filter controls must produce; `encode-params` for the translation and the request rejections, `annexb` for what a requested IDR carries; on the target, one-second segments cut at `-force_key_frames` in both codecs and a 24 fps run past every GOP boundary | (in `feat/encode-entrypoints`) |
| 9 | AV1 Profile 0 qualified at both bit depths: P010 advertised and the capture format taken from the stream | native-vs-VA frame MD5 over a 10-bit stream, Chrome 152 on the same stream selecting `VaapiVideoDecoder` with P010 surfaces, and the 8-bit/HEVC Main10/VP9 rows unchanged; see §9 and `TEST-RESULTS.md` | (not tagged) |

Phase 4 (GPU copy engine) was delivered inside Phase 3; the numbering above
is the one the tags carry.

Research track, resolved 2026-09-22: client-owned CAPTURE buffers.
`test/iris-import-probe.cc` is the acceptance program; it proved that the
firmware does not honour QBUF order when it has several buffers to choose
from (246/360 wrong under the doc's original model), and the Import policy
is the deformation that survives that finding (one picture in flight, no
holes). Results and the VP9 open failure: TEST-RESULTS.md.

Client memory entering through `vaCreateSurfaces2` (DRM_PRIME_2 or PRIME_1,
§3.4) is the same policy reached from the API. Its gates are `prime-import`
(host: the layout and refusal rules) and `test/iris-va-import-probe.cc`
(target: the surface aliases the client's bytes and a firmware encode reads
them). The allocation chain behind those surfaces is gated on the target by
`test/iris-alloc-chain.sh` (§3.4); it is not a Meson test because every rung
below the first needs an MSM render node.

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
    ticket is claimed (`vaSyncBuffer`, `vaSyncSurface`, or the `vaMapBuffer`
    fallback). A payload larger than the buffer's capacity is
    `ENCODING_ERROR`, never a truncation. A submitted picture is never
    dropped: the drain resolves whatever is still outstanding.
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
    `idr=1` in the trace is the request.
15. An encode timestamp rises strictly: the session rate is fixed at
    configuration time and each picture advances by one frame duration. VA
    clients send the frame rate only with IDR pictures, and a timestamp
    derived per picture therefore went backwards after every GOP boundary,
    which stopped the firmware returning access units.
16. A surface created over client memory is a view of exactly one linear
    DMA-BUF in the CAPTURE layout; the driver owns a duplicate of the fd and
    the client may close its own. Any other geometry, modifier or object
    count is `VA_STATUS_ERROR_INVALID_PARAMETER` at creation, never a picture
    decoded into the wrong pixels.
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
    the first picture with nothing to retry. `p010_decode_profile()` is the
    one place that answers this, and the capability table, the config
    attributes, the surface attributes and the capture format all read it.
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
