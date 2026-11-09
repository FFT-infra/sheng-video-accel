# sheng video acceleration: Qualcomm Iris VA-API for the Xiaomi Pad 6S Pro (SM8550)

**English** | [中文](README.zh-CN.md)

This is the video-acceleration bundle for `sheng` (Xiaomi Pad 6S Pro, SM8550)
on ARM64 Linux: a libva / VA-API driver for the stateful V4L2 decoder and
encoder Qualcomm's Iris VPU exposes, the four `qcom-iris` kernel patches it
needs, and Fedora RPM / Arch PKGBUILD packages. On the one qualified target
(Fedora 44 with a self-built `ianchb/sm8550-mainline` kernel) Google Chrome,
Electron and Firefox get hardware decode, and H.264/HEVC frames go from the
firmware into Chrome's own buffers with no copy in between; VP9 stays on Copy.
The driver is shared by every VA-API client on the tablet; it carries recorded
evidence on this device only.

## Why this exists

Stock Google Chrome on arm64 Linux is built with `use_vaapi=true` and
`use_v4l2_codec=false`: the only hardware decode entry point compiled into the
binary is libva. The same is true of every prebuilt Electron application and
of Firefox's FFmpeg hwaccel. Without a VA-API driver those clients decode on
the CPU, and a 1080p HEVC stream on a tablet is a fan-and-battery problem.

Iris is a *stateful* V4L2 decoder: the firmware parses the bitstream itself
and the interface is an OUTPUT queue, a CAPTURE queue and a few controls.
Nobody had written the libva backend that turns VA-API decode calls into
Iris sessions. This repository is that backend. Chrome is the acceptance
client; FFmpeg, GStreamer, mpv and Firefox use the same driver and are
welcome, but they also have native V4L2 paths of their own.

## What works today

Every row is backed by a recorded hardware run in
[TEST-RESULTS.md](TEST-RESULTS.md) (boot ID, kernel, module hash and driver
hash for each). Qualified on a Snapdragon SM8550 tablet, Fedora 44, Google
Chrome 152.

| Codec | Chrome | Path in Chrome | Bit-exact check |
| --- | --- | --- | --- |
| H.264 (CB/Main/High) | hardware decoder | Import, zero-copy | frame MD5 equal to the native V4L2 decoder; Fluster JVT-AVC_V1 72/135; the 63 non-passes are classified in TEST-RESULTS.md, and 49 of them are field-coded streams the kernel refuses |
| HEVC Main | hardware decoder | Import, zero-copy | frame MD5 equal; Fluster JCT-VC-HEVC_V1 141/147 |
| HEVC Main10 (P010) | hardware decoder | Import, zero-copy | frame MD5 equal at 10-bit |
| VP9 Profile 0 / 2 | hardware decoder | GPU blit (see limits) | frame MD5 equal, 8- and 10-bit |
| AV1 Profile 0 | hardware decoder | GPU blit (see limits) | frame MD5 equal, alt-ref, no-alt-ref, 8- and 10-bit |

"Hardware decoder" means Chrome's media surface reports `VaapiVideoDecoder`
with the platform flag set and a decoded-frame count over a 30 s window with
no dropped frames; a mapped node or a running GPU process proves nothing.

The same display also answers `VAEntrypointEncSlice`, on the encoder node the
kernel exposes next to the decoder. Chrome does not use it; FFmpeg,
GStreamer and anything else that asks libva for an encode entrypoint do.

| Codec | Entrypoint | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| H.264 Constrained Baseline / Main / High | `VAEntrypointEncSlice`, CBR, VBR or CQP | NV12 | Annex-B | 1080p `ffmpeg h264_vaapi`, 300-frame GOP-60 acceptance (TEST-RESULTS.md) |
| HEVC Main | `VAEntrypointEncSlice`, CBR, VBR or CQP | NV12 | Annex-B | 1080p `ffmpeg hevc_vaapi`, 300-frame GOP-60 acceptance (TEST-RESULTS.md) |

"Hardware encode" means the run moved the Iris IRQ counter, the bitstream
decodes without errors, and its SPS matches the requested profile, level and
geometry. A VA context that only returns bytes proves nothing.

## Requirements

- An SoC with the `qcom-iris` decoder (HFI gen2; SM8550 is the qualified
  one). aarch64.
- libva ≥ 1.1, libdrm ≥ 2.4.52; EGL/GLES/GBM for the GPU copy engine;
  `v4l-utils` at runtime (the launcher finds the decoder node by driver name
  because the number moves between boots).
- Any Linux distribution that can load a libva module. Fedora 44 is the
  qualified one. Fedora and Arch packaging is provided; there is no Debian
  or Ubuntu package, and those distributions are not qualified.
  - On Debian and Ubuntu, building wants `libgbm-dev`, which is a Mesa
    package pinned to an exact `libgbm1`. Installing it can downgrade a
    vendor-patched graphics stack. The build only needs the headers,
    `gbm.pc` and the `libgbm.so` link-time symlink, and Meson treats GBM as
    optional: without it the driver builds with the CPU copy engine instead
    of the GPU one. Either take those three pieces out of the `.deb` by hand
    or build without GBM and accept the slower copy path.
- Optional: five Iris kernel changes. They are decode-order output, a
  64-buffer CAPTURE pool, the encoder loop-filter controls, the encoder's
  `REQUEST_SYNC_FRAME` control, and a timestamp on error completions. They
  are carried rebased in [`packaging/kernel/`](packaging/kernel/README.md),
  with a module-only build recipe. On a stock kernel the driver detects
  their absence and still works. The results of a stock kernel are:
  - Chrome gets a GPU copy per frame, not the zero-copy path.
  - The driver refuses an encoder deblocking request that is not the
    firmware default.
  - A requested IDR moves the intra period. The driver does not ask the
    firmware for a sync frame.
  - The driver reports a corrupt picture after a drain or a sync timeout.
    It does not report it at once.

## Install and use

From the packages, on Fedora:

```sh
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch uses `packaging/arch/PKGBUILD`. Both packages install these files:

- the driver, into libva's driver directory;
- an `msm_drv_video.so` symlink beside it;
- `iris-device-check`, `iris-verify` and `iris-configure-clients`;
- the node-resolution file those commands share;
- an install manifest with the source commit and the artifact hash.

The symlink is what libva resolves on its own. This SoC reports
`DRIVER=msm_dpu`. Without the alias, a client that does not set
`LIBVA_DRIVER_NAME` (`mpv`, VLC, GStreamer) opens no driver at all.

There is no launcher and no desktop entry. Start Chrome the normal way:

```sh
google-chrome-stable
```

It needs no flags, no environment variable and no separate profile: libva
finds the driver through the alias, and the node is resolved by the driver
itself from the `iris_driver` name. `test/iris-browser-acceptance.sh` runs
Chrome exactly that way, with `LIBVA_DRIVER_NAME` and both node overrides
removed from the environment, and requires the GPU process to map this
driver and hold the node. To check a session by hand, open
`chrome://media-internals` while a video plays. The decoder must read
`VaapiVideoDecoder` with `kIsPlatformVideoDecoder: true`.

Two commands answer the rest before anything else is worth trying:

```sh
iris-device-check          # read-only: nodes, firmware, driver, codecs, which path
iris-verify                # decode and encode, with the Iris IRQ counter
```

`iris-device-check` resolves both nodes through the launcher and reports the
device-tree node, the bound driver, the module hash, the firmware, the coded
formats each queue exposes, which optional kernel controls are present, and
whether libva opens this driver with no environment set. `iris-verify`
generates its own clip, checks that libva opens the driver unaided, that a
decode and an encode each move the Iris interrupt counter, and that the
encoded stream decodes. Neither writes anything outside a scratch directory.

mpv and Firefox configuration is opt-in: `iris-configure-clients` writes
`hwdec=vaapi` into the current user's `~/.config/mpv/mpv.conf` and sets
`media.hardware-video-decoding.force-enabled` in the profile that
`profiles.ini` names, backing up an existing mpv.conf first. It touches no
`/etc` file and does not configure Chrome. The Firefox pref is the one that
replaced `media.ffmpeg.vaapi.enabled` in Firefox 137; it has not been
verified as a Firefox decode run on the qualified target.

From a checkout instead of a package:

```sh
meson setup build && meson compile -C build
sudo ./scripts/install-system.sh build       # libva driverdir + launcher under /usr/local
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2 vainfo   # or just try it
```

`scripts/uninstall-system.sh` reverses the helper install using the recorded
manifest. Details of the lab workflow are in
[`docs/build-and-deploy.md`](docs/build-and-deploy.md).

Other VA clients need no configuration once the package is installed, because
of the `msm_drv_video.so` alias. One exception: GStreamer's VA plugin keeps a
vendor allow-list and refuses an unknown driver, so the `va` elements only
register with `GST_VA_ALL_DRIVERS=1`. That is GStreamer's gate, not the
driver's; with it set, `vah264dec` and `vah265dec` decode through Iris.

## How it decodes

One ownership model with three publish policies, chosen per surface from the
client's behaviour. The driver owns the CAPTURE pool.

- **Direct**: a surface that is exported or mapped only after its decode
  (FFmpeg, GStreamer, mpv) takes the completed CAPTURE slot itself. True
  zero-copy.
- **Import**: a surface the client exported *before* its first decode (Chrome
  pre-exports its whole pool) has its buffer queued into CAPTURE for exactly
  that picture, so the firmware decodes straight into it. Needs decode-order
  output from the kernel patch; selected per codec from the measured
  evidence (H.264, HEVC, Main10).
- **Copy**: the same pre-exported surface is filled by a GPU blit (EGL on
  Adreno) from the driver's slot into a stable buffer whose file descriptor
  never changes. VP9 and stock kernels take this path.

Import holds because of two measured rules: exactly one client picture is
with the firmware at a time, and no submitted picture is ever skipped. A
completion that lands in a neighbour's buffer fails both surfaces instead of
publishing. The contract, the state machine and the trace vocabulary are in
[`docs/architecture.md`](docs/architecture.md); the measurements are in
[TEST-RESULTS.md](TEST-RESULTS.md).

A client can also hand over the memory itself: `vaCreateSurfaces2` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` (or the older `_DRM_PRIME`) creates
a surface over one linear NV12/P010 DMA-BUF instead of allocating one. The
buffer must already carry the CAPTURE layout the firmware writes — 128-byte
pitch (256 for P010), chroma after the 32-aligned luma rows — and the fd is
duplicated, so the client may close its own. The surface then takes whichever
policy above applies (Import on a decode-order kernel, Copy otherwise) and
is usable as an encode input as well.

## Known limits

Each limit below states the condition, the result, and why. Where a limit
depends on the kernel, the condition names the kernel feature.

- **VP9 uses the GPU copy path.** One VP9 alt-ref access unit decodes as two
  pictures. A window of one buffer stalls about 500 ms per alt-ref, because
  the firmware will not return the invisible half before it has a slot for
  the visible one. A window of two *pictures'* buffers lets the firmware
  choose between them, and about a quarter of the completions land in the
  wrong buffer. The session now lends one picture one buffer per completion
  instead, which is a different thing: the firmware still chooses only
  between the same picture's memory. On the target that path is exact for
  FFmpeg (515/515, no misplaced completions, framemd5 equal to software), but
  Chrome releases a VP9 surface before its picture completes: 564 of 768
  queueings were abandoned and 541 completions dropped, against 2 and 4 for
  the H.264 control. Chrome still reports 480 frames and 0 dropped, which is
  silent degradation rather than a pass. VP9 therefore stays on Copy until
  that browser path is clean (TEST-RESULTS.md, 2026-10-24).
- **AV1 holds one picture.** VA carries no `refresh_frame_flags`. For this
  reason the translator keeps each AV1 picture until the next access unit's
  reference map names the slots that the picture refreshed.
  - If the client decodes with a lead of one picture, the client never
    notices. FFmpeg with any filter or download does this, and so does
    Chrome.
  - If the client submits one picture and then syncs its surface before it
    submits the next picture, the client waits for the sync bound. At end of
    stream that bound expires. The held picture is then released as final,
    and a following non-intra picture fails with a clear error. It does not
    decode wrong.
  The firmware also returns a hidden (`show_frame=0`) picture with no
  pixels. The session attributes it by decode order and publishes it,
  because FFmpeg shows it later through `show_existing_frame`. Import
  therefore stays on the copy path. AV1 Profile 0 covers 8- and 10-bit
  4:2:0, and the driver advertises both: the firmware picks P010 or NV12
  from the stream. Film grain synthesis is refused, because the kernel gives
  userspace no way to ask the firmware for it.
- **A stock kernel means Copy for Chrome.** The decode-order control comes
  from the kernel patch. Without it the session runs in display-order mode:
  bounded sync waits, plus one STOP/LAST/START drain on timeout. Import is
  refused in this mode.
- **An imported surface must match the CAPTURE layout.** This layout is:
  one object, a linear modifier, the driver's pitch, and the chroma plane at
  the driver's offset. If a client DMA-BUF has a tiled modifier, more than
  one object, a pitch of the client's choosing, or a chroma plane at a
  different offset, `vaCreateSurfaces2` refuses it with
  `VA_STATUS_ERROR_INVALID_PARAMETER`. The driver does not decode into the
  wrong pixels. PRIME_1 has no modifier field, so there the driver trusts
  `VA_SURFACE_EXTBUF_DESC_ENABLE_TILING` to say that the buffer is linear.
- **Chrome 153 has an HEVC seek regression. This is an upstream defect.**
  Chromium 153 turned on `ExtendedVideoBitstreamValidation`. Its shared
  `H265Decoder` treats the first SPS after every seek as a configuration
  change, so the compositor keeps showing pre-seek frames.
  [CFM880/iris-vaapi](https://github.com/CFM880/iris-vaapi) reported and
  bisected it on SM8150, and filed it as
  [Chromium issue 563075803](https://issues.chromium.org/issues/563075803).
  This project did not reproduce it, because the target runs Chrome 152.
  CFM880 now marks the regression resolved in Chrome 156.0.8078.47
  (`stale_repeats = 0/445`, no workaround flags) and says the workaround can
  be dropped; their report also warns that a launcher wrapper injecting the
  flag makes an A/B look fixed. On Chrome 153, launch with
  `--disable-features=ExtendedVideoBitstreamValidation`, or stay on 152.
  Chrome 156+ needs neither.
- **One device is qualified.** All results above come from one SM8550
  tablet. Other Iris gen2 SoCs should work, but they carry no evidence yet.
  Arch packaging is provided but not validated.
- **An on-demand IDR is a direct control on the target, and a GOP move on a
  stock kernel.** The two cases are:
  - If the node has `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, the driver writes
    that control. The module built from `packaging/kernel/0003-*` has it. The
    firmware then writes the sync frame and its parameter sets itself.
  - If the node does not have that control (a stock 7.2.6 module), the driver
    sets the intra period to zero for that one picture, puts the period back
    before it queues the picture, and prefixes the last SPS/PPS. The
    firmware writes SPS/PPS only on its own sync frames.

  Both cases cut where `-force_key_frames` asks. The next automatic IDR
  arrives one GOP after the requested one, not on the original schedule. On
  the GOP path the lever is one control write per requested picture. For
  this reason a request on every frame costs more than a plain intra-only
  stream. A client that asks for an intra picture without the IDR flag —
  FFmpeg does this — is treated as an IDR request, because the only lever
  the firmware has emits an IDR.
- **No B-frames.** The kernel allows at most one B-frame, and a delayed
  CAPTURE would break the one-in-one-out model. A context whose sequence
  parameters ask for `ip_period > 1` fails with
  `VA_STATUS_ERROR_INVALID_PARAMETER`.
- **No 10-bit, VP9 or AV1 encode.** The encoder OUTPUT queue enumerates NV12
  and QC08C only. The driver therefore cannot feed HEVC Main10 or VP9
  Profile 2.
- **No packed headers, so no extradata at open time.** The firmware writes
  SPS/PPS itself, and the driver refuses to echo a second set.
  `VAConfigAttribEncPackedHeaders` answers `VA_ENC_PACKED_HEADER_NONE`, not
  "unsupported": zero means the attribute is known and no packed header is
  accepted, while `VA_ATTRIB_NOT_SUPPORTED` means the client cannot ask at
  all.
  - FFmpeg copes. It warns, carries the parameter sets in-band, and extracts
    `avcC`/`hvcC` from the first packet. `.mp4` output therefore works
    directly: 43 bytes of extradata for H.264, and 124 for HEVC.
  - GStreamer's VA encoder rejects a driver that answers
    `VA_ATTRIB_NOT_SUPPORTED` there, because it cannot tell that from a
    client error. Zero passes its check, so `vah264enc` and `vah265enc` get
    past it. Both then encode 90 frames on the target and the streams decode
    clean; the second blocker was a double claim on the completion, now
    funnelled through `claim_coded()` (TEST-RESULTS.md, 2026-10-24).
- **Encode is pipelined, and `-async_depth` works.** `vaEndPicture` queues
  the picture and returns. The access unit arrives through `vaSyncBuffer` on
  the coded buffer, or through `vaMapBuffer` for a client that never syncs.
  FFmpeg sees `vaSyncBuffer` and switches to its asynchronous path. For this
  reason `-async_depth` pipelines up to the four encoder input slots: 1080p
  HEVC on the target goes from 190 fps at depth 1 to 372 fps at depth 4,
  with byte-identical output at every depth. `vaSyncSurface` on the input
  surface still works, and it means the same picture's completion. A VA
  context has a fixed geometry, so a resolution change needs a new context.
- **No multi-slice, tiles, ROI or `-max_frame_size`.** FFmpeg fails those
  requests loudly. The driver does not ignore them silently. The driver has
  the same rule: it fails with `VA_STATUS_ERROR_UNIMPLEMENTED` when the
  picture parameters carry more than one slice, tiles, wavefront coding,
  weighted prediction, scaling lists, chroma QP offsets, or several
  reference indexes. The firmware writes the sequence headers, owns the
  reference structure and picks the quantisation. A stream that quietly used
  something else would be worse than a failure.
- **The picture contract is enforced, not assumed.** The firmware has one
  reference frame and a single picture type lever, so the driver checks what
  the client asked for against what the encoder can do:
  - A non-IDR I picture, or an HEVC CRA, is promoted to an IDR request. Iris
    exposes `FORCE_KEY_FRAME`, which emits an IDR; the alternative was coding
    the picture as a P and misordering the stream. FFmpeg submits this shape
    when `-idr_interval` is above zero: `hw_base_encode.c` emits
    `FF_HW_PICTURE_TYPE_I` for the GOP boundaries between IDRs, and
    `vaapi_encode_h264.c` then sends `slice_type = 7` with
    `idr_pic_flag = 0`. The default interval is 0, so a plain `-g` run never
    reaches the path.
  - An inter picture must name the previous picture's reconstruction surface
    in its reference list. Anything else fails with
    `VA_STATUS_ERROR_UNIMPLEMENTED`.
  - A stream must start on an intra picture, and a picture marked
    non-reference is refused.
  - Only one full-picture slice is accepted: `macroblock_address` /
    `slice_segment_address` at zero and the slice covering the whole picture
    (`num_ctu_in_slice` for HEVC). B slices, and an HEVC `coding_type` that
    disagrees with the slice type, are refused.
- **The driver forwards slice deblocking, or refuses it.** The VA slice
  parameters are the client's one chance to ask for filtering to be off or
  throttled. The driver forwards them to
  `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA` and the HEVC
  equivalents when the kernel has those controls.
  - The stock SM8550 kernel does not have them. There the firmware always
    filters with the default 0/0 offsets, so any other request fails with
    `VA_STATUS_ERROR_UNIMPLEMENTED`. It does not come back as a stream that
    ignores the request.
  - [`packaging/kernel/`](packaging/kernel/README.md) carries the kernel
    patch that adds the controls.

  Deblocking is per-context in V4L2 and per-slice in VA. The driver
  therefore reads it from the first slice, and a mid-context change needs a
  new context.

## Development

```sh
meson setup build-local && meson compile -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

Hardware gates run on the target: `test/iris-browser-acceptance.sh --clip …`
(Chrome evidence), `test/iris-matrix.sh` (frame MD5 against the native
decoders), `test/iris-structure-matrix.sh`, `test/iris-dynamic-resolution.sh`,
`test/iris-concurrency-soak.sh`, `test/iris-zero-copy-suite.sh`. Experiments
that can reset the firmware go through `scripts/iris-experiment-guard.sh`.
The full list is in [`test/README.md`](test/README.md); the rules for
contributors and for coding agents are in [CONTRIBUTING.md](CONTRIBUTING.md)
and [AGENTS.md](AGENTS.md).

## License and credits

MIT with LGPL-2.1-covered parts, the same terms as upstream; see `COPYING`,
`COPYING.MIT` and `COPYING.LGPL`.

- This is a fork of [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2),
  itself descended from Bootlin's libva-v4l2-request. The VA and V4L2
  scaffolding comes from there; the stateful Iris implementation in `src/`,
  the browser acceptance gates and the packaging are this fork's work.
- `include/linux/` vendors the kernel UAPI headers under their own SPDX tag
  (`GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause`).
- `packaging/kernel/` carries five Iris kernel changes in four patches:
  four from [radxa-pkg/qcom-va-driver](https://github.com/radxa-pkg/qcom-va-driver)
  (Radxa, Xilin Wu; the repository was `strongtz/libva-v4l2` before
  its 2026-10-26 rename) and the encoder half of an upstream `REQUEST_SYNC_FRAME`
  commit (Wangao Wang, Qualcomm). They touch GPL-2.0 kernel code and carry
  that license, not the driver's. The explicit-RPS HEVC slice rewrite follows
  the same project's approach.

## Environment variables

Every switch the driver reads is listed here and read in exactly one place,
`src/util/options.cc`; `test/iris-env-doc-check.sh` fails the build if this
table and that file disagree in either direction. All of them are diagnostic;
the launcher sets none of them.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Use this decoder node instead of scanning `/dev/video*` for the `iris_driver` decoder. |
| `LIBVA_V4L2_ENCODER_PATH` | probe | Use this encoder node instead of scanning `/dev/video*` for the `iris_driver` encoder. A path that is not an Iris encoder fails display init. |
| `V4L2_VA_TRACE` | off | Per-frame trace on stderr. Any value enables it. The record vocabulary is documented in `docs/architecture.md`. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`. Without an override the first frame of a cold session may wait up to `30000` ms for firmware bring-up. |
| `V4L2_VA_COPY` | `gpu` | Engine used to publish a frame into a surface the client exported before its first decode: `gpu` (EGL blit on Adreno) or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy`, `direct` or `import` publication for every surface. Diagnostics only; `auto` picks per surface from the client's export behaviour (Direct for post-decode exporters, Import for pre-exporters on a decode-order kernel, Copy otherwise). `import` degrades to `copy` where the session cannot honour it. |
| `V4L2_VA_DUMP` | off | Directory to write every submitted access unit into. |
