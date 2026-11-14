# Post-merge hardware and upstream backlog (2026-11-10)

**Historical ledger.** This file records the state of workstreams as they were
triaged, including entries that were later implemented, superseded or
reversed. It is not the current to-do list: `docs/STATE.md` names the current
qualified combination, and `data/iris-codec-capabilities.json` is the current
capability boundary. Keep the entries below as provenance, not as instructions.

This is the completion ledger for the work consolidated into PR #5 (which
subsumes PR #4). The 2026-11-10 draft (PR #3) froze the baseline and listed
every open workstream; PR #4 implemented the AV1 held-picture removal; PR #5
consolidated the independent-oracle syntax corpus, film-grain boundary analysis,
and final documentation sync. Items below are marked with their current state in
§15. Nothing here is a capability claim: a codec or publish policy is advertised
only after its hardware gate passed and `TEST-RESULTS.md` records the run.
Frame/drop evidence and a media-surface decoder report are required before any
browser hardware claim.

## 0. Frozen baseline

Historical baseline recorded from the remote state on 2026-11-10, before PR #4
changed `src/`. It is not the current installed-package state; re-read before
starting any item. A later merge, a target reinstall, or a kernel module swap
invalidates the whole list.

| Fact | Value |
| --- | --- |
| `main` | `65686a20bd80a7598a45287e0dd81d2596a1203b` (merge of PR #2) |
| PR #2 | MERGED 2026-11-10T07:44:55Z, head `56dbefa`, base `af5c0ab`, 22 files, +1106/-22 |
| `main` CI | `build-and-check` success, run `42777457628` |
| Local gates | `sh test/run-static-checks.sh` rc=0, `meson test -C build-local --print-errorlogs` 22/22, `git diff --check` clean |
| Installed driver on `sheng` | SHA-256 `da952fe4fe35896e215976213a1f5b6836cc5aa9ab9d97a1c08f55a909615da3` |
| Installed build | source commit `079480d8b208c5bb6695d4e91e6c1953c4d9010c`, `source_dirty=0`, installed 2026-11-10T01:15:14Z |
| Runtime | installed package, no `LIBVA_*` override, no publish override, no build-tree override |
| Target | kernel `7.2.6-sm8550-g42f3b40-iris1`, module built from 0001-0004 only |
| Boot ID | `2d5eed8d-57d7-4746-9e10-2cda0f4fe768` (unchanged through the recorded runs) |
| Decoder | resolved at runtime by `iris_driver` QUERYCAP, card `Iris Decoder` |
| Qualified AV1 Import evidence | 48/48 four-codec matrix; real 1280x720 AV1 clip 120/120 exact framemd5, 120 `publish=import`, 0 errors/drops; in-context DRC 1280x720 -> 854x480 80/80 exact framemd5, 84 completions (42 import, 42 copy-gpu); guarded 3840x2160 AV1 probe 30/30 exact framemd5, 30 `publish=import`, hottest 39.8 C, no reset/thermal breach |
| Browser evidence | Chrome live YouTube AV1 250/0, 725 `publish=import`; Chrome live Bilibili HEVC 251/0, 761 `publish=import`; Firefox 158 live Bilibili HEVC 251/0, live YouTube with `media.av1.enabled=false` VP9 250/0; Firefox AV1 fails closed on the held-picture sync contract and falls back to software, reproduced twice |
| Known-unqualified | AV1 12-bit, AV1 large-scale tile, Firefox AV1 hardware decode, long-window soak, power measurement |

`src/` at `main` is byte-identical to the PR head that the installed build was
qualified on. Any change under `src/` requires a new installed-package run before
a capability claim.

## 1. Contract that every item must satisfy

From `AGENTS.md` and `docs/architecture.md`. These are not suggestions; an item
that breaks one is not a candidate for merge.

1. Decoder and encoder nodes are found by `iris_driver` QUERYCAP at runtime and
   told apart by which queue carries the compressed format (OUTPUT decoder,
   CAPTURE encoder). No fixed node literal in code, launcher or packaging.
2. One source of truth per fact, updated in the same commit: capability and
   publish-policy qualification in `data/iris-codec-capabilities.json`;
   environment surface in `README.md`; trace vocabulary in
   `docs/architecture.md` §8. The first two are enforced by checks; §8 is not,
   so every new `trace()` line must be added to §8 explicitly.
3. Evidence before capability. A codec, profile or publish policy is advertised
   only after its hardware gate passed and `TEST-RESULTS.md` records the run.
   Import additionally keeps one client buffer in flight and never skips a
   submitted picture; a completion landing in a neighbour's buffer must fail
   both surfaces and never publish.
4. VA profiles are exposed only for codecs the capability table marks
   `production: supported`.
5. Vendored UAPI headers in `include/linux/` are never edited.
6. A browser decode counts as hardware only when a media surface reports the
   selected decoder with frame and drop evidence. A mapped node, a running GPU
   process or a started pipeline proves nothing.
7. Verification before a completion claim:
   ```sh
   sh test/run-static-checks.sh
   meson test -C build-local --print-errorlogs
   git diff --check
   ```
8. For any claim about the target, record source commit, driver hash, resolved
   node, boot ID, decoder name, frame and drop counts, and whether the runtime
   used the installed package or a build-tree override. A `DESTDIR` staging run
   is not an installation.
9. Experiments that can reset firmware go through
   `scripts/iris-experiment-guard.sh -- ...` per `docs/build-and-deploy.md`.
   A reset or a thermal breach is a failed experiment, not evidence of codec
   support.
10. Scratch files, logs, traces and media stay in `~/Lab/Bridge/tmp` on host and
    target, never in `/tmp`, and never in the repository.
11. Preserve untracked user files. No `git clean`, destructive reset, or push
    without an explicit request in the same turn.
12. No AI attribution in commit messages, PR bodies, issue replies or any
    public-facing text.
13. Only `main` carries qualified evidence. Experimental kernel modules,
    build-tree drivers and unpublished capability flags must not be described as
    "installed", "verified", "supported" or "released".

## 2. Lab prerequisites (blocking for every target item)

| ID | Item | Why it blocks | Pass condition |
| --- | --- | --- | --- |
| L1 | Record the pack state before and after each guarded run | Every 2026-11-10 run was `Discharging`: 26 % at -2.195 A, 51 %, 60 %, 58 -> 54 %. A low or discharging pack is a valid lab condition, not a reason to refuse a run; the report must carry the boundary it was measured under. | `battery.log` written by the guard, with start/end capacity, status, voltage, current and USB online; a shortened window is labelled as such |
| L2 | Screen-off / fixed brightness / fixed refresh state for power runs | `test/iris-power-sampler.sh` refuses `Charging|Full` because a plugged-in gauge cannot measure battery-side draw; without a controlled display state a power delta is not attributable to decode | **done**: the guard writes `display.log` (connector status, `enabled`, `dpms`, preferred mode, brightness/max, fbcon blank), `network.log` (operstate per interface) and `command.txt` (argv, one argument per line) at the start, on every thermal sample and at the end; a missing tree records `unknown` instead of failing the run, covered by the self-test's absent-class case. Sysfs does not expose the active refresh rate to an unprivileged reader, so a power report states the refresh it was measured under. First target run 2026-11-12 (guarded `sleep`, boot `b2f1de2f…`): `connected,disabled,Off,3048x2032,1474,2047,4` and `docker0=down;wlp1s0=up`, `status=complete` |
| L3 | Guard records pack state without gating on it | A run can start on a nearly empty pack; that is a measured condition, not an unsafe one. The guard does not refuse it, so the evidence must carry the boundary | **done**: `battery.log` plus self-tests for low/discharging, charging and missing gauge; boot/thermal pass semantics unchanged |
| L4 | Guard scratch default | `scripts/iris-experiment-guard.sh` defaults to `${TMPDIR:-/tmp}/iris-experiment`; guarded 4K run used an explicit `IRIS_EXPERIMENT_DIR` under the project scratch root | either pass the explicit dir for every run or change the default and add a self-test |
| L5 | Freeze the installed package | A kernel module swap or reinstall changes the driver hash and invalidates §0 | record module timestamp, `sha256sum` of the installed `.so`, `source_commit`, `source_dirty`, before and after |
| L6 | Re-read §0 before each item | A later merge, a new upstream release, or a fork sync changes the baseline | `git fetch origin`, `git rev-parse origin/main`, `gh pr list --state open` |

## 3. Workstream A — remove the AV1 one-picture hold using the upstream idea

**Goal.** Firefox 158 syncs each surface on its decoder thread; the former
translator held a picture until the next access unit named the DPB slots it
refreshed, so the next AU could not arrive while Firefox waited. Radxa's
`qcom-va-driver` `src/av1.cpp` instead emits a complete temporal unit per VA
picture immediately: it picks a free DPB slot, and when all eight are live it
replays the uncommitted prior picture as a hidden reference before decoding the
new one (`AV1 install reference`).

**Settled during PR #4.** The guarded native V4L2 probe on the 0001-0004
module showed that the firmware returns the hidden install completion to
userspace with its OUTPUT token. Our existing `completions = 2` VP9 alt-ref
accounting consumes that invisible half and publishes the visible half.
Kernel 0005 is not needed for completion accounting and must not be loaded
against this userspace: `packaging/kernel/README.md` records that it loses
frames. The default AV1 translator in `src/va/picture.cc` now selects
`make_av1_dpb()`; `make_av1()` remains a one-line rollback path.

| ID | Item | Type | Prerequisite | Pass condition | Rollback |
| --- | --- | --- | --- | --- | --- |
| A1 | Read-through of upstream `src/av1.cpp`, `src/decoder.cpp`, `src/internal.hpp` and `tests/decoder/av1.py`; write a contract diff against our `src/codec/av1.cc`, `av1_obu.cc`, `src/iris/session.*`, `src/va/surface.cc` | host, read-only | none | a written list of every behaviour that differs, including DPB slot selection, hidden replay, sequence-header emission, tile layout and EOS | none needed |
| A2 | Define the new V4L2 completion ledger on the 0001-0004 module | target probe | L1, L5, guard | **done**: `docs/experiments/av1-noshow-dpb-spike.md` records the two completions under one token (`ERROR`/no payload, then pixels), the empty LAST at drain, and the zero-timestamp single hidden picture path | none; no module swap happened |
| A3 | Build and load 0001-0005 on `sheng`, reboot, confirm the module hash and active controls | target, can reset firmware | A2 design, L1, guard wrapped | **measured, verdict "do not load"**: module `6ef2fce8...` (0001-0004 + 0006 + 0005) on boot `1edbadbe...` failed all three qualified AV1 clips under import, copy and direct (`submitted=7 completed=1` and worse, `misplaced=1` under import), while native `v4l2av1dec` reached EOS on the same node; `TEST-RESULTS.md` §"0005 on the target" records the traces and the mechanism | rollback copy `b90bd823...` written back with `depmod -a` and a reboot; the control arm on that module then decoded 120/120, 80/80 and 30/30 bit-exact with `errors=0 misplaced=0`. 0005 stays unbuilt and unloaded |
| A4 | Short AV1 decode against the new module with `V4L2_VA_TRACE=1` only to characterise suppressions | target | A3 | **done on 0001-0004**: guarded real VA-API sessions recorded `publish=invisible`, `publish=direct`/`publish=import`, `errors=0`, `misplaced=0` and bit-exact rebuilt OBUs | none; build-tree override only, installed driver untouched |
| A5 | Rewrite the AV1 translator to emit one complete temporal unit per `vaEndPicture` with an explicit 8-slot DPB and hidden-reference replay | code | A1, A4 | **done**: `src/codec/av1_dpb.cc`, `av1_build_install_temporal_unit()`; host tests cover DPB-full install, pending-surface references, client hidden frames, sequence restart and no held EOS tail | revert `src/va/picture.cc` to `make_av1()` |
| A6 | Rewrite the session / surface / EOS accounting for the completions | code | A4, A5 | **done**: `unsent`, `wait_for_release()`, `release_held_picture()` and the bounded-release tail are removed; orphaned invisible halves are consumed as `publish=invisible`; `test/iris-clip-trace-check.sh` still balances submitted/completed | revert `src/iris/` and `src/va/` changes |
| A7 | Re-derive Import ownership under the new ledger | target | A5, A6 | **done**: 79 Import queue events for 49 pictures, 30 `publish=invisible`, `misplaced=0`, `errors=0` as a build-tree probe; the 2026-11-11 installed-package gates then passed (48/48 matrix, 120/120 Import clip, 60 Import 10-bit frames), `misplaced=0` throughout | keep `publish_import: qualified` only while the installed-package gates pass, otherwise return AV1 to the recorded fallback before any claim |
| A8 | DRC path under the default translator | target | A7 | **done**: the 1280x720 -> 854x480 in-context clip reached 80/80 exact framemd5 on the installed package with `import=42 copy=42 invisible=63`; the copy half is the recorded layout-mismatch fail-safe | restore `make_av1()`; do not advertise a partial DRC result |
| A9 | EOS and tail behaviour | target | A6 | **done**: installed-package file decodes finished without a session sync timeout (`matrix timeouts=0 drain=1`, p10 `timeouts=0 drains=1`, 120/80-frame framemd5 files complete); host EOS/no-tail test also passes | revert to `make_av1()` and keep the former fail-closed rule |
| A10 | Firefox 158 live YouTube AV1 and live Bilibili, plus Chrome and FFmpeg controls, on the installed package | target | A7-A9, L1 | **done**: the 2026-11-11 17-case guarded browser matrix reports `failures=0`, `PASS AV1 browser matrix mode=all`; Firefox 9 cases `hw_log=True errors=0`, Chrome 8 cases `VaapiVideoDecoder platform=True dropped=0`, live YouTube/Bilibili and noav1 control included; FFmpeg framemd5 gates exact | do not claim Firefox AV1 hardware support; keep the current README/capability wording |
| A11 | New regression corpus: tiles, superres, lossless, 10-bit, lookahead, 4K, dav1d/software pixel-exact, borrowed as *case design* from upstream `tests/decoder/av1.py` | host + target | A5 | **done with two documented exclusions**: five 640x360 clips (baseline, `tiles 2x2`, `lag-in-frames 12`, `still-picture`, `lossless`) generated on the target with `libaom-av1`, each with an independent `libdav1d` framemd5 oracle; guarded installed-package hardware decode gave 5/5 EXACT, all `publish=import`, `errors=0 drops=0 misplaced=0`, `A11_RESULT failures=0`. Super-resolution produced no packets from this `libaom-av1`, and `-enable-global-motion 1` decoded identically to baseline, so both were dropped rather than counted as coverage. See TEST-RESULTS.md "A11: independent-oracle AV1 syntax corpus" | drop the cases that cannot get an independent oracle |
| A12 | Re-run all previously recorded evidence on the new baseline | target | A7-A11 | **done**: the 2026-11-11 installed-package gates re-ran 48/48 matrix, 120/120 real clip, DRC, 1080p 10-bit framemd5, 4K/10-bit/altref/tiles browser cases, the complete 17-case browser matrix with live YouTube/Bilibili, and the A11 syntax corpus, each with the §1.8 evidence fields and a separate guard (no thermal breach) | abandon the rewrite and keep `main` |
| A13 | Documentation and capability sync, then a merge decision | docs | A12 | **done**: PR #5 merged as `4125b11` on 2026-11-11; capability table, both READMEs, `docs/architecture.md` §8/§10 and the experiment record describe the default internal-DPB path, and `TEST-RESULTS.md` records the installed-package gates | revert all A items |
| A14 | Explicit stop condition | decision | A2-A4 | **done**: A2 observed the kernel contract; 0005 is not needed; no reset or thermal breach occurred in the guarded probes; the Import invariant was re-derived on the target with `misplaced=0` | keep `main` unchanged if the installed-package gates fail |

**Hazards recorded before A2; the first one did not survive the probe:**

- 0005's patch description says non-display pictures at EOS or a resolution
  change return empty LAST buffers, and that both kinds of LAST are excluded
  from the empty-payload error check (patch lines 16-17, 146-152). This was a
  reason to test 0001-0004 first: the guarded probe found no need for 0005,
  and loading it against this userspace loses frames. The DRC gate below still
  depends on `iris_process_streamon_output` clearing the DRC boundary sub-state
  and on the ordering of an earlier CAPTURE QBUF completing as empty LAST,
  after which vb2 returns `-EPIPE` for later DQBUF.
- Upstream `src/av1.cpp` remaps DPB slots by its own choice; our translator
  reconstructs the stream's actual `refresh_frame_flags` from the successor's
  `ref_frame_map`. Both can produce correct pixels; the upstream route is not
  "more accurate", it simply does not hold a client surface hostage.
- A full DPB costs one extra hidden decode per frame. Latency and bandwidth
  need their own measurement; do not infer from a frame count.
- Upstream evidence is SC8280XP / X1E80100 firmware; their client list is
  Chromium, FFmpeg, GStreamer, mpv, VLC, Kodi, not Firefox. Firefox's
  slow-decode heuristic and RDD sandbox remain independent variables.
- An earlier prototype of an 8-slot dynamic DPB without complete lifecycle
  and modifier coupling produced 33/120 frames on a real clip; partial
  implementations fail closed.

## 4. Workstream B — explicit refresh flags through the API

Parallel, longer-term. None of these is a driver-only change. The point of this
workstream is to remove the need for the successor AU without changing the
kernel completion contract.

| ID | Item | Owner outside this repo | Status as of 2026-11-10 | Pass condition |
| --- | --- | --- | --- | --- |
| B1 | Add `refresh_frame_flags` to `VADecPictureParameterBufferAV1` using a word from `va_reserved[VA_PADDING_MEDIUM]`, keeping struct size and ABI; add NEWS/docs | `intel/libva` | `va/va_dec_av1.h` last changed in 2021; no such field; libva 2.15 added the field for **encode** only. Re-checked 2026-11-12: the header's newest commit is still `6e86b4fb4` (2021-06-27), issue #567 asked for this field in 2022 and was closed 2023-01-07 with a client-side workaround (track each frame's `frame_offset`), no pull request adds it, and the tree is not dormant — its head is `f052e984e` (2026-10-23) and 2.24.1 (2026-07-08) is the latest release. The target carries libva 2.23.0 | upstream merged and installed on the target; `VA_CHECK_VERSION` guard defined |
| B2 | Fill the field in `libavcodec/vaapi_av1.c::vaapi_av1_start_frame()` from `frame_header->refresh_frame_flags` | FFmpeg | `vaapi_av1.c::end_frame()` already reads the value for film-grain refs; `7432fed62` (2026-07-29) removed deprecated fields; `4221a63ce` (2026-10-17) fixed unreleased-picture buffers; neither adds the field. Re-checked 2026-11-12: no later commit touches that file | patched FFmpeg installed; `pic_param` trace shows the exact mask |
| B3 | Fill the field in Chromium `media/gpu/vaapi/av1_vaapi_video_decoder_delegate.cc` (libgav1 already parses it) | Chromium | no such change found. Re-checked 2026-11-12: the delegate's newest commit is `e23cdf4ab` (2026-10-09, spanification), and nothing reads or writes a refresh mask | Chrome with the new libva reports hardware AV1 with the same frame/drop evidence |
| B4 | Raw-OBU passthrough instead of reconstructed headers, following FFmpeg's `videotoolbox_av1.c::end_frame()` which appends `s->current_obu.units[i]` | libva + FFmpeg + Chromium | VideoToolbox works, VA-API has no equivalent buffer semantics | API meaning agreed upstream; no driver-side OBU rebuild needed |
| B5 | Explicit VA-API flush / end-of-stream entry point | libva + clients | none; `docs/architecture.md` §9 records this as the missing entry point. Re-checked 2026-11-12: `va.h` carries no flush or end-of-stream entry point; its newest additions are `VA_PICTURE_H264_NON_EXISTING` (2025-12-23), `VAProfileH264High444Predictive` (2026-09-23) and P210 as a format (2026-10-22) | client can say "no successor is coming" without a sync timeout |
| B6 | Vulkan Video AV1 (`StdVideoDecodeAV1PictureInfo::refresh_frame_flags`) | whole client stack | separate API, not the Iris V4L2 path | only if the platform gains a Vulkan Video driver; treat as a research note, not a task |
| B7 | Driver-side use of exact flags with fallback to the current successor-based reconstruction | this repo | blocked on B1-B3 | two paths both covered by tests; flags absent never guessed; old clients still correct |
| B8 | Do **not** do: hardcode `refresh_frame_flags = 1` (Mesa's gallium-internal choice), guess the mask, or silently drop frames | — | the earlier dynamic-DPB rewrite decoded 33/120 real frames | any prototype must fail closed and be gated by the full A11 corpus |

## 5. Workstream C — Chrome VP9 Import

Current state: **closed 2026-11-12**. The session retains a released Import
target and completes it, so Chrome's early surface release no longer costs the
picture. `data/iris-codec-capabilities.json` has VP9 `publish_import:
qualified`, the driver advertises `V4L2_PIX_FMT_VP9` in `import_formats`, and
both READMEs say Import. Evidence: 48/48 four-codec matrix, FFmpeg 515/515
identical framemd5, two guarded Chrome windows (1705/1705 and 1588/1588,
`drops=0`, `misplaced=0`) against a Copy baseline of 1702/1705 with 3 drops.
TEST-RESULTS.md section "VP9 Import qualified (2026-11-12)".

History: FFmpeg was clean (515/515, `misplaced=0`, framemd5 equal to software)
with one picture one buffer per completion; Chrome released a VP9 surface
before its picture completed (564 of 768 queueings abandoned, 541 completions
dropped, against 2 and 4 for the H.264 control).

| ID | Item | Pass condition |
| --- | --- | --- |
| C1 | Re-measure Chrome's early surface release on the merged baseline with CDP frame/drop deltas and `abandoned=` trace counts; separate "surface released" from "picture completed" | **done 2026-11-12**: 1154 of 1705 pictures carried `target detached` in one 60 s window, every one of them completed; the `retained=` counter on `session finish` is the per-window total |
| C2 | Decide the ownership model: retained/duplicated DMA-BUF fd after `destroySurfaces`, or a copy window; write it down before code | **done**: the backlog's duplicate fd plus a retained `Target` copy; the retained copy carries the client layout and a null destination, so nothing follows the freed surface |
| C3 | Kernel patch 0006 (`7c28b27`, unmatched timestamp metadata ring) — decide whether it is needed at all on SM8550 | **WP1 settled the local kernel question**: the slow-CAPTURE defect was not reproduced on 0001-0004, and one guarded 0001-0004 + 0006 run passed with "no regression". 0006 stays experimental and out of the qualified baseline; no VP9 Import qualification follows from it. Chrome's early surface release (C1/C2) needed no kernel change: it was solved in the session (retained targets, 2026-11-12). |
| C4 | Target run: one-buffer vs two-buffer window; known bad case 153/609 misplaced must stay a regression test | **done 2026-11-12**: FFmpeg 515/515 exact framemd5, Chrome 1705/1705 and 1588/1588 with `drops=0 misplaced=0`, Copy baseline 1702/1705 with 3 drops |
| C5 | Only after C4: qualify VP9 `publish_import` in the table, both READMEs, §8 trace vocabulary, TEST-RESULTS.md and CI | **done 2026-11-12**: table, driver `import_formats`, both READMEs, TEST-RESULTS.md and the backlog updated together; the trace vocabulary already carries `target detached` and `retained=` |

## 6. Workstream D — AV1 feature completeness

| ID | Item | Recorded boundary | Pass condition |
| --- | --- | --- | --- |
| D1 | 1080p 10-bit AV1 | 1920x1080 60-frame `yuv420p10le` clip passed on 2026-11-11: `EXACT` framemd5, 60 Import, 0 errors/drops/misplaced on the installed package; depth is decided before the first AU. 4K 10-bit added 2026-11-12: 30/30 exact against libdav1d | done for exact framemd5. The browser pixel comparison is a recorded decision, not an open task: this project counts a browser decode as hardware only from a media surface reporting the selected decoder with frame and drop counts, and a compositor capture cannot be aligned to the software decode's frames, so pixels stay with the FFmpeg framemd5 gates |
| D2 | Film grain | shipped 2026-11-14: the two-surface path was the boundary, not a kernel limitation. The picture decodes into `current_frame` with grain off and `src/va/grain.cc` blends onto `current_display_picture` inside `endPicture` (2026-11-14 in `TEST-RESULTS.md`), exact against libdav1d at 8 and 10 bits | done: `test/iris-av1-grain-gate.sh` compares every downloaded frame with libdav1d's own grained output; browser evidence stays with the browser gates |
| D3 | 128x128 superblock / loop-restoration unit shift | one-bit header bug fixed; the 128x128 clip then decoded through VA 5326/5326 exact on the installed package. The GStreamer half was a package fact, not a decoder result: `gst-inspect-1.0 va` on `sheng` registers `0 features`. That is libgstva's vendor allowlist refusing `Qualcomm Iris V4L2 stateful (libva-v4l2)` in `_gst_va_display_filter_driver`; `GST_VA_ALL_DRIVERS=1` registers the elements, and `vaav1dec` then decodes the 120-frame clip with 120/120 frame hashes equal to the software decode (`TEST-RESULTS.md` §"GStreamer VA elements register once libgstva's vendor allowlist is bypassed", 2026-11-12). The earlier raw `fdsink` mismatch was a dump artifact: `fdsink` ignores `GstVideoMeta`'s plane offset, `pngenc` and the muxed path do not | done for the client gate: `test/iris-gstreamer-gate.sh` runs the decode comparison and both encoder elements. Browser evidence stays with the browser gates; no capability entry changes |
| D4 | Tiles, superres, lossless, still picture, global motion, lookahead, 4K 10-bit | **done except the browser half**: five 640x360 A11 cases (`baseline`, `tiles 2x2`, `lag-in-frames 12`, `still-picture`, `lossless`) each have an independent libdav1d oracle and 5/5 exact framemd5 on the installed package, all `publish=import`, `errors=0 drops=0 misplaced=0`; `superres` produced no packets from the target `libaom-av1` and `global motion` decoded identically to baseline, both excluded with the encoder reason. 4K 10-bit was added on 2026-11-12: 3840x2160 `yuv420p10le`, P010 Import, 30/30 exact against libdav1d, `errors=0 drops=0 misplaced=0`, guard `complete` (`TEST-RESULTS.md` §"4K 10-bit AV1 through Import"). The independent browser pixel comparison is not a gate, by the D1 decision | independent software oracle per case (A11), exact pixel hashes, no new `publish=error/drop/misplaced` |
| D5 | AV1 12-bit and large-scale tile | 12-bit stays in the capability table `rejected`, now with an observed refusal rather than only a table entry: a `yuv420p12le` profile-`Professional` stream exits at profile selection with `Your platform doesn't support hardware accelerated AV1 decoding` and writes no pixels (`TEST-RESULTS.md` §"12-bit AV1 is refused at the VA layer"). The node itself has no 12-bit path (2026-11-12): its CAPTURE formats are `NV12`/`Q08C`/`P010`/`Q10C`, and the native `v4l2av1dec` pipeline that decodes an 8-bit control reaches EOS while the 12-bit stream fails at caps negotiation with no output caps and no CAPTURE dequeue (`TEST-RESULTS.md` §"12-bit AV1 has no format at the node"). Large-scale tile is measured (2026-11-12): 3840x2160 with 32 tiles decodes 30/30 exact against libdav1d through Import, `errors=0 drops=0 misplaced=0` (`TEST-RESULTS.md` §"4K AV1 with 32 tiles through Import") | 12-bit stays rejected: there is no buffer-level gate to run, because the decoder offers no 12-bit format. Do not turn the format list into a positive capability |
| D6 | `show_existing_frame` / hidden alt-ref ordering | both directions are now pinned by host tests (2026-11-12): `noshow completion never lands on a shown surface` fails the shown picture when a NOSHOW completion names it, and the timestamped corrupt case now also asserts `completed` does not move, so a corrupt shown picture is never published as a hidden one. The first test fails when the `target.hidden` guard in `session.cc` is removed | §17/18 invariants; a new direction gets a shipped negative test |

## 7. Workstream E — long-window, power and cost evidence

| ID | Item | Why open | Pass condition |
| --- | --- | --- | --- |
| E1 | Two-hour concurrency soak (`dual-h264` and `mixed`, 172800 frames per context) | **done 2026-11-12**: both windows ran the full 7200 s on the installed package, boot ID unchanged, no reset or breach; every context submitted and completed 172809 access units (172800 output frames) with `errors=0 drops=0 timeouts=0 misplaced=0`, `drains=1`, `output_queued=0`, and both preflights matched their software framemd5. Peaks are attributed in `TEST-RESULTS.md`: 41.0 C and 47.7 C while decoding, 56.8 C and 78.9 C over the whole run, the higher figures coming from the fixtures the soak generates with x265. The media is 640x360, so this is a stability window and not a resolution or power result |
| E2 | `test/iris-power-sampler.sh` hardware/software A/B | **done 2026-11-12**: the guard ceiling is now a runaway backstop at 100000 (the zones trip passively at 90/95, critically at 110), so the software arm completes. Two runs on the installed package, unplugged, screen off, boot unchanged: VA 1.53 / 1.47 W against software 6.01 / 5.84 W, delta about -4.4 W; highest samples 91.1 and 92.3 C, no reset. Whole-device increments, not package power. TEST-RESULTS.md §"E2 closed" |
| E3 | 4K continuous decode thermal and power | the guarded 4K probe is 30 frames / 30 s and explicitly "not a thermal, power or long-window result" | **done 2026-11-12**: 1200 s of continuous real-time 4K AV1 Import under the guard, `status=complete`, boot ID unchanged, 36000 frames at 30.01 fps, peak 43400 millic over 1840 samples, pack 19 % -> 15 % at ~2.5 W, no reset and no breach. Thermal and continuity only; no power ratio is claimed |
| E4 | Per-frame 4K cost | 2026-10-25 finding: growth is DVFS, not accumulation; big-cluster clock rises when unpaced | **done 2026-11-12**: every CPU policy pinned to its maximum (2016/2803/2956 MHz) and recorded, 600 frames in one process at 3.07 ms per frame (326 fps) with 0.26 s of client CPU, and 3.13 ms with the default `schedutil` governor, so the number is not clock-sensitive at this load. The 20-pass loop measured 34.7 ms per frame because each session costs about a second, which is recorded as a session cost rather than a throughput figure |
| E5 | Make the guard pack-aware | L3 | **done**: `battery.log` records capacity, status, voltage, current and USB online; the self-test covers 19 % discharging, 100 % charging and a missing gauge, all reaching `status=complete`. Boot/thermal guards are unchanged |

## 8. Workstream F — kernel and upstream sync policy

Current conclusion: **do not rebase, do not follow a whole upstream tree**. Keep
`main` as the baseline, subscribe to releases, and pull only a change that maps
to one of the items above.

| ID | Item | Evidence | Decision rule |
| --- | --- | --- | --- |
| F1 | `sheng-7.2.6` pin `42f3b40c702a` | unchanged; our 0001-0004 are rebased onto it | keep until a specific gain is demonstrated |
| F2 | `sheng-7.3.0` | Iris changes there are Milos VPU v2 platform data, Gen2 firmware autodetect/fallback, UBWC config guard, VBR time-delta RC change, request-key-frame, PM resume and OPP fixes; none targets a recorded SM8550 defect. Re-checked 2026-11-12 on `ianchb/sm8550-mainline`: the branch's Iris history ends at the `media/v7.3-1` merge (`f4cdf7ca9`, 2026-08-19), so the recorded list is still complete | rebase only if a named defect is fixed; a rebase invalidates §0 and requires A12-equivalent re-verification |
| F3 | `radxa-pkg/qcom-va-driver` v1.1.1 (2026-10-28) | 0004 and 0005 blobs identical to ours; 0001-0003 differ only because ours merges 0001+0002, rebases to `sheng-7.2.6`, and adds two domain/firmware guards; added-code comparison shows functional equivalence; its own backend, built from this head and run on `sheng` on 2026-11-13, is exact on AV1 and VP9 10-bit only with kernel 0005 and wrong after frame 32 without it | do not replace our patches; record the diff result in `TEST-RESULTS.md` (H2) |
| F4 | Upstream activity watch | 14 commits in 2026-09, 37 in 2026-10, last commit 2026-10-28; open PRs/issues #7 packed sequence headers, #5 Venus stateful, #2 rate control, #6 HEVC extradata — none maps to Firefox AV1, Chrome VP9 Import or SM8550 evidence. Re-checked 2026-11-12: `media: iris: add support for multi_slice in iris encoder` (`5f21cd9a4`, 2026-09-29), its `iris_set_slice_count` 64-bit division fix (`89a3d2a22`, 2026-10-02) and a treewide `devm_kmalloc` conversion (`4dd10fbbe`, 2026-11-06). The multi-slice one maps to a recorded rejection, but none of them is in `sheng-7.2.6` and the loaded encoder node lists no multi-slice control, so H3's interface-level reason stands | monthly re-check; only open a branch for a mapped change |
| F5 | `mxsrc/libva-v4l2` `74b336a42` (unset `destination_buffer` in `exportSurfaceHandle`) | our `src/va/surface.cc:429` already rejects a null descriptor and uses `shared_ptr<StableBuffer>`, no `std::optional` | no action |
| F6 | FFmpeg `7432fed62` (stop setting deprecated AV1 VA fields) | our `src/codec/av1_obu.cc:145` writes `chroma_sample_position: UNKNOWN`; we never read `tg_start`/`tg_end` | no action; confirm with a matrix run if the target's FFmpeg is upgraded |
| F7 | `intel/libva`, Firefox `FFmpegVideoDecoder.cpp` | `va_dec_av1.h` last changed 2021; recent Firefox changes in that file are Vulkan Video / Android, the "send one frame, immediately receive" loop is unchanged. Re-checked 2026-11-12: `FFmpegVideoDecoder.cpp` has not been touched since 2025-07-08 | re-check before starting B work |
| F8 | `yzddmr6/linux-sm8750-piano` v1.2.0 (2026-11-10) | camera, serial, audio; no new Iris commits; 0006's origin commit unchanged | no action |
| F9 | `GhfunNiHe/sheng-fedora-packages` | last push 2026-10-25; pins patches against `iris-kernel-patches-v1` | re-check packaging pin only if a patch hash changes |
| F10 | 0005/0006 experiment branch | no such branch exists; only `feat/vpu-zerocopy-ubwc` and its fork remote; 0006's WP1 build, 0005's WP2 build and target run, and 0007's six-stage run are all recorded on `chore/post-merge-hygiene`, and the userspace tolerance lives on `exp/0005-keep-tolerance` | keep all three patches out of the qualified module and out of any released build |

## 9. Workstream G — documentation, consistency and hygiene debt

All of these are small, host-only and independently mergeable. None changes
runtime behaviour.

| ID | Item | Exact location | Fix |
| --- | --- | --- | --- |
| G1 | Stale claim about patch diffs | `TEST-RESULTS.md` §"Sibling backends" said "patch bodies were not diffed" | **done**: the section now records the F3 result (0004/0005 byte-identical, 0001-0003 rebase-only) and why upstream's required 0005 stays experimental here |
| G2 | Stale claim about Chrome 153 | `TEST-RESULTS.md` said "Both READMEs now say 153 needs the flag or 152, 156+ needs neither"; neither README mentions 153 or `ExtendedVideoBitstreamValidation` | **done**: corrected the sentence to the browser versions the READMEs actually record (152 local software, 156 live, Firefox 158) |
| G3 | Browser window wording | `README.md:20` and `README.zh-CN.md:20` previously specified a rigid 30-second zero-drop window | **done**: aligned with the AGENTS.md rule requiring media-surface decoder identification and frame/drop counts over the test window |
| G4 | Stale source paths | local, gitignored `docs/github-community-assets.md:38-41` names `src/h264.cc`, `src/vp9_stateful.cc`, `src/context.cc`, `src/surface.cc`; none exists | **closed**: the file is not tracked in any ref and is a local scratch note, not a shipped doc |
| G5 | Stale capability boundary | local, gitignored `docs/github-community-assets.md:55` keeps VP9/H.264/HEVC as the production boundary | **closed**: same untracked scratch note; the tracked capability table and both READMEs are the source of truth |
| G6 | Scratch root defaults | 9 tracked scripts default to `${TMPDIR:-/tmp}`; `test/README.md:166-169` uses `/tmp/iris-hardware-suite` | **closed as intentional, no code change**: every guarded run passes an explicit `IRIS_EXPERIMENT_DIR` under `~/Lab/Bridge/tmp` (§15, L4); the default is left alone so existing `guard.state` semantics do not drift |
| G7 | Trace vocabulary drift | `docs/architecture.md` §8 had no checker; AGENTS.md calls this out | **done**: §8 matches the current emitters (`publish=import` on `capture noshow`, `encode drain result=complete|timeout outstanding`, `submitted token=`, error-side `publish=misplaced`/`publish=error`, plus the encode error records). `test/iris-trace-vocab-check.py` compares all 54 emitter prefixes against §8, with four named VA startup/teardown exceptions; it is registered in `run-static-checks.sh`, Meson and CI's required list |
| G8 | License wording | `COPYING` said the project was released under LGPL-2.1 and MIT; the SPDX-tagged driver sources are MIT, LGPL appears only in `COPYING.LGPL` and the package metadata; the spec's `MIT AND LGPL-2.1-or-later` is conservative | **done**: `COPYING` now says the driver source is MIT and `COPYING.LGPL` retains upstream provenance; the spec and PKGBUILD comments say the same. `COPYING.LGPL` is retained, and both packages still install all three files under `/usr/share/licenses` |
| G9 | Kernel patch licence headers | the six files in `packaging/kernel/` carry no SPDX header; they patch GPL-2.0 kernel files and are not compiled into the driver package | **closed, no header added**: the patched kernel files keep their own GPL-2.0-only identifier; adding an SPDX line to a patch would change the `SHA256SUMS` pins the downstream kernel spec verifies. `packaging/kernel/README.md` now records this. The MIT driver's licence fields are unchanged |
| G10 | Merge commit identity | merge commits `4125b11` and `65686a2` are authored `xinyang`, while later commits use `phxinyang`; the e-mail is the same | **closed as a release-note fact**: same account, two display names in history. No code action, no history rewrite; mention it when compiling author statistics or release notes |
| G11 | `FIX-LOG.md` 待办 section | local, gitignored file; not tracked in any ref | **closed**: not a shipped checklist; this backlog is the tracked ledger |
| G12 | 4K lane discoverability | `IRIS_REAL_CLIP_SET=4k` is opt-in and not part of `all`; `test/README.md` records it | **done**: `test/README.md:24` states a guarded 4K probe is not a thermal, power or long-window result |
| G13 | CI required-test list | `.github/workflows/ci.yml` named 19 of the then-25 Meson tests | **done**: all names are now listed, including the new `iris-trace-vocab-check`; `meson test` at `:85` already ran all of them, so this closed an inventory-guard gap, not a skipped-test gap |
| G14 | Released hidden-install accounting | a released `inject_hidden_install_unit()` target emits two `publish=invisible` records and no `publish=drop`, while `session.cc` said one abandoned picture is one drop; `iris-clip-trace-check.sh` counts invisible records | **closed, no new trace record**: a visible payload completion reports the one drop, but two NOSHOW halves have no pixels to abandon, so `drops=0` is correct. No per-picture marker is added; §8, the `session.cc` comment and the host test now state the same rule, and the trace vocabulary stays unchanged |
| G15 | Stale decode rejection | `data/iris-codec-capabilities.json` listed HEVC `long-term references (generated parameter sets cannot describe them)`, but `src/codec/hevc.cc` writes an explicit `st_ref_pic_set(0)` plus the long-term section with `delta_poc_msb_present_flag=1` into every slice header, and that writer landed 2026-09-21, one day after the entry | **done 2026-11-12**: the JCT-VC stream `LTRPSPS_A_Qualcomm_1.bit` (`long_term_ref_pics_present_flag=1`, `num_long_term_ref_pics_sps=8`, `num_long_term_pics=1` in 161 slice headers) decodes 500/500 frames with framemd5 identical to libavcodec through the driver, control 20/20. The entry now names the boundary that does exist, long-term pictures whose POC MSB cycles do not ascend. The other decode rejections were measured the same day and keep their entries (`TEST-RESULTS.md` §"Decode rejections") |
| G16 | GStreamer blamed for a plugin-package gap | `TEST-RESULTS.md` and D3/H4 recorded `gst-inspect-1.0 va` reporting `0 features` as "needs a plugin build that registers its elements", which left the client gate waiting on a package that was already installed | **done 2026-11-12**: the plugin registers nothing because `_gst_va_display_filter_driver` in `libgstva` refuses the vendor string `Qualcomm Iris V4L2 stateful (libva-v4l2)`; `GST_VA_ALL_DRIVERS=1` is its documented override and registers seven elements. `vaav1dec` then hashes 120/120 frames equal to the software decode (`test/iris-gstreamer-gate.sh`). The raw-`fdsink` mismatch that looked like a decode difference is a dump artifact: `fdsink` ignores `GstVideoMeta`'s plane offset, `pngenc` and the muxed path honour it. Upstream still carries the same filter (`vadisplay: only register elements of allowed drivers`, `d7b5b54e`, 2023-11-08; last touched 2026-01-28 for const warnings), so the override stays necessary |

## 10. Workstream H — encode backlog

Lower priority than decode. Ground each item in the capability table's
`rejected` list rather than in an assumption about the firmware.

| ID | Item | Pass condition |
| --- | --- | --- |
| H1 | Investigate the recorded rate-control gap against the piano comparison, with a fresh A/B on the merged baseline | a measured, reproducible statement of what differs; no capability change without a gate. **Verdict: closed, code fixed.** The gap was the translator, in two parts. `VAEncMiscParameterRateControl.bits_per_second` is the controller's *maximum* and `target_percentage` is the target inside it (`va.h`), and both H.264 and HEVC translators programmed the raw maximum — FFmpeg turns a plain `-b:v 4M` into 8 Mbit/s at 50 %, so the encoder ran at twice the request. The second part only showed up after the first was fixed: FFmpeg renders the rate-control misc buffer with the first picture only, and the fallback to the sequence field reprogrammed the controller between 4 and 8 Mbit/s on every picture (ten alternating writes per 600-frame arm, 7934 kbit/s). The fix takes the percentage into the target, writes the maximum above it as `BITRATE_PEAK` for VBR as well as CBR, and keeps the last configured target for a picture that carries no rate control. `vbr4` went 8000 -> 3992 kbit/s with one write per control, and `encode-params-test` covers both halves. `TEST-RESULTS.md` carries the seven-arm before/after with the boot ID, both driver digests and both node roles. The piano comparison's remaining difference is content- and firmware-level, not this mapping |
| H2 | Packed sequence headers / extradata for H.264 and HEVC encode | upstream has an open issue on this; decide whether it is in scope for this driver at all before coding. Settled: do not do it here — `VA_ENC_PACKED_HEADER_NONE` is what opens the GStreamer path, and `picture.cc` silently drops packed header buffers, so advertising support would desynchronise MP4 extradata and reopen the double-claim `size 0` crash |
| H3 | Recheck each `rejected` entry (B-frames, packed headers, multi-slice, tiles/WPP, weighted prediction, scaling lists, chroma QP offsets, multiple reference indexes, 10-bit/non-4:2:0, HEVC Main10 encode) on the current firmware | either a hardware gate with tests, or keep the explicit `VA_STATUS_ERROR_UNIMPLEMENTED` and the table entry. A host-side audit found code-level `require` backing for every entry; the only drift is the conditional deblocking item, which no longer fires on a kernel with 0002 installed. Firmware-level availability was then read off the encoder node (2026-11-12, `TEST-RESULTS.md` §"Encoder rejections"): the raw port accepts only `NV12`/`Q08C`, which backs the 10-bit and non-4:2:0 entries; no control exists for multi-slice, weighted prediction, scaling lists, chroma QP offsets or a reference count, so those are interface-level; `hevc_profile` lists `Main 10` but no `P010` input exists, so the entry stays rejected with that reason; and `video_b_frames` is `0..1` with B-frame min/max QP controls for both codecs, so the firmware takes one B-frame. The driver refuses it in three places, and they have to move together: `encode.cc:176` writes the control back to zero, the H.264 and HEVC translators accept only I and P slices with one active reference, and `submit()` requires an inter picture to reference the picture just coded — which a B picture, predicting from the future P, does not. That entry is a design limit of this driver rather than of the firmware, and lifting it is a reordering change (V4L2 returns packets in encode order, not display order), not a control write | the entries stay; B-frames are the only one with firmware support behind them, and they need those three refusals lifted together with a reordering gate, which no client here has asked for |
| H4 | Keep the double-claim and `REQUEST_SYNC_FRAME` regressions covered | existing host/target gates stay green after any encode change. `encode-session-test` now links `src/va/buffer.cc` and covers the coded buffer's claim: nothing outstanding is success with no invented bytes, a buffer that lost its session is refused, the first claim stores the access unit and the segment, a second claim on an already-answered ticket leaves those bytes alone, and a buffer smaller than the access unit is refused rather than truncated. The empty-answer case fails on the pre-fix code (checked by removing the guard), so it is a regression test rather than a restatement. The GStreamer half is scripted as of 2026-11-12: `gst-inspect-1.0 va` reported `0 features` because libgstva's vendor allowlist refuses this driver, and `GST_VA_ALL_DRIVERS=1` registers the elements; `test/iris-gstreamer-gate.sh` then encodes 60 frames through `vah264enc` and `vah265enc` and decodes both streams back to 60 frames (`TEST-RESULTS.md` §"GStreamer VA elements register once libgstva's vendor allowlist is bypassed") |

## 11. Workstream I — CI, tests and release plumbing

| ID | Item | Pass condition |
| --- | --- | --- |
| I1 | Cross-repo fork PR CI still needs base-repo admin approval for `pull_request` runs; fork `push` runs are green | record the actual approval state per run; never describe a pending PR run as passing. **2026-11-11 update**: PR #5's `pull_request` run `42988596199` completed `success` in 1m13s without an approval hold; `.github/workflows/ci.yml` now installs `ffmpeg` and asserts `libdav1d` so the two AV1 oracle checks run instead of failing or silently skipping |
| I2 | Any new invariant gets a regression test that fails on the old code, plus a name in `.github/workflows/ci.yml` required list (G13) | red-on-old, green-on-new, CI inventory check passes |
| I3 | `test/iris-clip-trace-check.sh` compares ordered `^0,` framemd5 lines because host and target `Lavf` versions differ | keep the header-only-difference self-test; do not reintroduce a whole-file `cmp` |
| I4 | `test/run-static-checks.sh` is the single CI entry point for host-only checks; keep hardware suites out of it | unchanged on merge |
| I5 | Provenance: source commit, tracked-source SHA-256, dirty flag, artifact hash; CI must stay clean (`source_dirty=0`) | CI "Generate and verify provenance" step passes. **2026-11-12 update**: `deploy.sh` now re-hashes every tracked path on the target with `sha256sum -c` before the rebuild, so the digest in the manifest is measured where the build happens instead of injected from the sender; a controlled probe (marker file, same-size corruption, tampered file) confirmed the sync repairs content and the check rejects a tree that does not match. The same edit fixed an exclude pattern that had been silently withholding the two tracked files whose names begin with `build` |
| I6 | `ldd -r` rejects undefined symbols in `v4l2_drv_video.so` | unchanged |
| I7 | Distribution layers are reported separately: source, build, staged `DESTDIR`, installed package, target runtime, registry/release | never say "installed" or "released" from a source or CI check alone |
| I8 | Target gates had no runner and no run record; identity was re-collected by hand for every claim | **done 2026-11-12, runner fixed 2026-11-13**: `test/remote/gate.sh -- command` runs one gate inside the remote checkout under the experiment guard and writes `summary.json` (schema `qualcomm-iris-vaapi/target-run-v1`) with source commit, dirty flag, boot, kernel, module hash, driver hash, decoder and encoder names resolved by driver name, exit status and guard state, plus the command's output in `guard/command.stdout`. It does not deploy, so a run against a stale install shows the hashes it actually used. A guard that stopped the run exits 3. The original `sh -s` heredoc transport was a false pass: the guard's backgrounded child read `/dev/null`, so its "first record" (`run-gst-20261112`, boot `31424f69`, exit 0) captured no command output. The command is now landed in a per-run file; the GStreamer results in `TEST-RESULTS.md` were collected independently and are not derived from that empty record. An echo-marker run and every later VPP prereq run carry real child output; the current VPP record is `run-20261112T205511Z` (boot `3356e00e`, module `b90bd823`, driver `323c07cb`, guard `complete`) |

## 12. Priority, prerequisites and cost

| Priority | Items | Prerequisite | Cost | Risk if wrong |
| --- | --- | --- | --- | --- |
| P0 | L1-L6, G1-G13, I1-I7 | none (host-only / lab setup) | low | low; documentation drift and an unguarded lab run |
| P1 | E1-E5 (soak, power, 4K cost) | L1, L2 | medium; discharging is allowed, a plugged-in gauge is not usable for battery-side power | a power claim that cannot be attributed to decode |
| P1 | A1, A2 (contract diff and kernel observation) | none for A1; L1/L5/guard for A2 | medium | **done**; no 0005 module swap |
| P2 | A11-A12 (independent-oracle syntax corpus, full evidence re-run) | A2, L1, guard | **done** on the installed package: five syntax cases with independent libdav1d oracles plus the full evidence re-run; superres and global-motion cases remain excluded with a recorded reason | advertising an unqualified profile, or claiming coverage for a case the encoder could not express |
| P2 | C1-C5 (Chrome VP9 Import) | L1 | medium-high | misplaced completions, silent drops |
| P2 | D1-D6 (AV1 feature coverage) | independent software oracles | medium | advertising an unqualified profile |
| P3 | B1-B8 (upstream API change) | upstream acceptance | high, mostly outside this repo | indefinite wait; no local fallback |
| P3 | F2 (kernel rebase) | a named defect fixed upstream | high | full re-verification for no measured gain |
| P3 | H1-H4 (encode) | L1 | medium | low, decode is the product goal |

## 13. Explicit non-goals

- Do not rebase the whole `src/` tree onto `radxa-pkg/qcom-va-driver`; the two
  backends have different memory models and different target SoCs.
- Do not load 0005 against the current userspace outside a guarded experiment
  branch. Do not treat 0006's single guarded WP1 run as qualification; the
  module stays experimental and outside the qualified baseline.
- Do not delete `COPYING.LGPL`, the vendored UAPI headers, or the experimental
  kernel patches as "cleanup".
- Do not add a browser launcher, a desktop entry, a Chrome feature override, or
  a fixed video-node literal; `test/run-static-checks.sh` forbids all four.
- Do not promote a short clip gate, a `DESTDIR` staging run, a mapped node, or a
  running GPU process into a capability or browser-hardware claim.
- Do not add AI attribution to any commit, PR body, issue or public reply.
- Do not perform a `git clean`, destructive reset, branch switch, or push
  without an explicit request in the same turn.

## 14. Definition of done for the consolidated work (PR #5)

PR #5 (consolidating PR #4) merged as `4125b11` on 2026-11-11. It met its
definition of done: the default internal-DPB translator passed host gates,
installed-package matrix/clip/DRC/EOS gates recorded every §1.8 field in
`TEST-RESULTS.md`, browser runs reported a media-surface decoder with
frame/drop evidence, and the capability table, both READMEs,
`docs/architecture.md` §8/§10 and the required tests were updated together.
The remaining work is tracked as the explicitly open workstreams in §15 and
the WP0-WP7 sequence in §16. A short clip, a `DESTDIR` staging run, a mapped
node, or a running GPU process is not a capability or browser-hardware claim.

## 15. Completion ledger

| Workstream | Items | Status |
| --- | --- | --- |
| §2 Lab prerequisites | L1-L6 | L1/L2/L3 are closed by the guard's own records: `battery.log` without a start gate, `display.log` plus `network.log` plus `command.txt`, and the unchanged boot/thermal semantics. L4 stays settled as "pass an explicit `IRIS_EXPERIMENT_DIR` under `~/Lab/Bridge/tmp` for every guarded run" (all recorded runs do), with the default left alone so existing `guard.state` semantics do not drift. L5/L6 are per-run checks |
| §3 AV1 held-picture removal | A1-A14 | done: internal DPB is the default, installed-package matrix/clip/DRC/EOS gates and Firefox/Chrome live-site browser gates pass on 2026-11-11; `TEST-RESULTS.md` records the run. Long corpus and 4K cost work remains in §6/§7 |
| §4 Explicit refresh flags | B1-B8 | open / upstream-blocked; internal DPB removes the local need for an API change. Re-checked 2026-11-12: `intel/libva` is active (head 2026-10-23, release 2.24.1) but `va/va_dec_av1.h` still ends at 2021-06-27 with no `refresh_frame_flags`, issue #567 was closed with a client-side workaround, and no pull request adds it; FFmpeg's `vaapi_av1.c` is unchanged since `4221a63ce` |
| §5 Chrome VP9 Import | C1-C5 | **done 2026-11-12**: the retained-target model ships (retained `Target`, backlog-owned duplicate fd, client layout copied at submit), VP9 `publish_import` is `qualified` in the table and the driver. Gates on driver `0ef0f34c…`, boot `5feea1b5…`, `/dev/video17`: 48/48 four-codec matrix under Import, FFmpeg 1280x720 alt-ref clip 515/515 with framemd5 equal to software, Chrome 1705/1705 and 1588/1588 with `drops=0 misplaced=0`, Copy baseline 1702/1705 with 3 drops. The host regressions are the two retention unit cases plus a red check with the detach condition disabled. TEST-RESULTS.md §"VP9 Import qualified (2026-11-12)" |
| §6 AV1 feature completeness | D1-D6 | D1 done for exact framemd5: 1920x1080 60-frame 10-bit clip `EXACT`, 60 Import, 0 errors/drops/misplaced on 2026-11-11. D2 shipped as the user-space two-surface path it was: grain synthesised onto `current_display_picture` inside `endPicture`, exact against libdav1d at 8 and 10 bits (2026-11-14). D4 covered by the A11 corpus (tiles, lookahead, still-picture, lossless exact; superres and global motion excluded with a recorded encoder reason) plus 3840x2160 10-bit Import, 30/30 exact against libdav1d on 2026-11-12. 12-bit is answered negatively at the node (no 12-bit CAPTURE format, refusal proven on the native decoder as well as the VA profile path); D3's GStreamer half is settled on the installed package by `test/iris-gstreamer-gate.sh` (vendor allowlist, 120/120 frame hashes). The browser pixel comparison is settled as not a gate (D1); D5's large-scale tile and D6's two negative directions are covered |
| §7 Long-window, power, cost | E1-E5 | E1 done (both 7200 s windows, clean ledgers), E3 and E4 done (20-minute continuous 4K inside the guard; 3.07 ms per frame with the clocks pinned and the clock state recorded), E5 and L3 done (the guard records pack state); E2 closed on the relaxed ceiling with the A/B measured (about -4.4 W device-side, peaks 91-92 C) |
| §8 Kernel and upstream sync | F1-F10 | open / decision recorded as "do not rebase"; 0005 was built and loaded once on `sheng` (2026-11-12) and failed all three qualified AV1 clips, so it is unloaded again; 0006 was built, installed with 0001-0004, rebooted and run through one guarded WP1 gate with a "no regression" verdict; 0007 (the completion-preserving form of 0005) was built, installed, booted and run through nine AV1/VP9 gates before the qualified module was restored, with a "do not ship" verdict. On 2026-11-13 the sibling backend from `radxa-pkg/qcom-va-driver` v1.1.1 was run against the same two modules on the same node: AV1 exact with 0005, wrong from frame 33 without it, VP9 10-bit exact either way, while this driver is exact only without 0005 (`TEST-RESULTS.md` §"The sibling backend goes exact on AV1 only with a kernel contract this driver cannot use"). All three stay experimental and outside the qualified baseline. The monthly watch was run again on 2026-11-12: `sheng-7.3.0` has no Iris commit after its `media/v7.3-1` merge, `radxa-pkg/qcom-va-driver` still ends at `0980a1dec` (2026-10-28), `linux-sm8750-piano` at `7f8d6bedd` (2026-11-10) and `sheng-fedora-packages` at `756362f81` (2026-10-25). Mainline added multi-slice encoder support after this base, which maps to a recorded rejection but is absent from `sheng-7.2.6` and from the loaded node's controls, so "do not rebase" holds |
| §9 Docs, consistency, hygiene | G1-G16 | all closed: G1, G2, G3, G7, G8, G12, G13, G14, G15 and G16 done; G4, G5, G6, G9, G10 and G11 closed with explicit reasons (untracked scratch notes, intentional scratch default, kernel SPDX provenance, release-note identity fact, no new trace marker). G15 is the one entry that changed a capability line: the HEVC long-term rejection was stale and is now the narrower boundary the code enforces. G16 removes a stale package claim: the VA plugin was always installed, and libgstva's vendor allowlist is what hid it |
| §10 Encode | H1-H4 | H1 closed with a code fix: both translators now take `target_percentage` into the target, carry the client's maximum as `BITRATE_PEAK` for VBR, and keep the last configured target when a picture omits rate control; `vbr4` measured 8000 -> 3992 kbit/s. H2 is settled as out of scope, H3 has a host-side audit plus a 2026-11-12 inventory of the encoder node's formats and controls that gives every rejected entry its reason, H4's double-claim half is covered by `encode-session-test` and its GStreamer half by `test/iris-gstreamer-gate.sh` (`vah264enc` and `vah265enc`, 60 frames each, both decoded back to 60). No capability entry or profile changed |
| §11 CI, tests, release plumbing | I1-I8 | host tests 26/26 and static checks green on every commit of PR #6; the guarded installed-package gates and the repo-owned 17-case AV1 browser matrix are in PR #5; I5 verifies the synced tree on the target; I8 gives the target gates one runner and one run record (`test/remote/gate.sh`, schema `qualcomm-iris-vaapi/target-run-v1`), so the identity behind a gate claim is a file rather than a transcription. Target gates stay out of CI, which has no device; what CI gains is that a reported run names its source commit and the exact driver the target held |
| §12 Priority | ranking | updated for the settled 0005 question |
| §13 Non-goals | list | recorded |
| §14 Done criteria | check | **met**: PR #5 merged as `4125b11`; installed-package and browser gates passed. §14 now records the merged state; the WP sequence in §16 and the open items above are the current work |
| §15 Ledger | ledger | updated for the merged state; the stale PR #4 closure instruction is not applicable (see §16) |

## 16. Work packages after the merge

Baseline: `main` == `origin/main` == `4125b11`, re-verified 2026-11-12; the
worktree is clean and `build-and-check` run `43028113058` is green. The
qualified module is 0001-0004; 0005/0006 are experimental. At the time of
WP0, neither had been loaded on `sheng`; WP1 later loaded 0006 for one guarded
run with a "no regression" verdict. That run does not change the qualified
baseline or any capability claim. Each package gets one branch and one draft
PR, no force push, no rewriting `main`, no deleting branches, and no AI
attribution in commits or public text.

Every package runs `sh test/run-static-checks.sh`,
`meson test -C build-local --print-errorlogs`, `git diff --check`, and CI must
be green. Every target experiment uses the guard in `docs/build-and-deploy.md`,
records source commit, driver hash, boot ID, resolved node, decoder name, frame
and drop counts, and whether it used the installed package or a build-tree
override. A reset or thermal breach is a failed experiment. Only one
experimental kernel patch may enter a validation run at a time. Stop after each
package and report in this format:

```
WP: <number and name>
branch/PR: <branch> / <PR link>
commit: <commit>
gates: <which ran, results>
evidence: <TEST-RESULTS section and key numbers>
decision: <keep / roll back / do not do, and why>
open questions: <items needing a maintainer decision>
```

### WP0 baseline recheck — done

No code change. Recorded in `TEST-RESULTS.md` §"Post-merge baseline
(2026-11-12)": live `origin/main` `4125b11`, identical local trees, clean
worktree, green merge-commit CI, host gates 25/25. PR #6 later added the §8
checker, so its own host gates are 26/26.

### WP1 validate 0006 — done, verdict "no regression"

`packaging/kernel/build-module.sh` reproduces the module-only build on
`sheng`: source `ianchb/sm8550-mainline` `42f3b40c702a`, `.config` from
`/proc/config.gz`, `localversion` from `uname -r`, a 333-entry video-subsystem
`Module.symvers` regenerated from `/proc/kallsyms`, and 0001-0004 + 0006
applied in order (0005 absent). The built module's vermagic is
`7.2.6-sm8550-g42f3b40-iris1`, its 136 undefined symbols are all exported by
the running kernel, and a rollback copy was compared byte-for-byte with the
installed `qcom-iris.ko.zst`.

The slow-CAPTURE defect that 0006 addresses was not reproduced on the
0001-0004 baseline. Six 480-frame alt-ref VP9 runs were clean, and a raw V4L2
slow-CAPTURE probe at `delay_us=2000` and `10000` produced no premature LAST,
EPIPE or timecode mismatch; the earlier search of 838 retained guard artifacts
also found no such record. The 0001-0004 framemd5 files are retained.

After explicit approval for the reboot, 0001-0004 + 0006 was installed and
loaded on `sheng`. The guard started after the reboot, so its "boot ID
unchanged" rule measured the experiment. Three interrupted attempts are kept
as audit records (relative EOS path, stale node/gate deployment, and an EOS
checker call without `--hidden-pictures`). The final run
`guard-20261111T193559Z` has `status=complete`, `WP1_GATES_DONE failures=0`,
unchanged boot ID, maximum temperature 40400 millic, no thermal breach and no
reset. The four-codec matrix is 48/48 exact for each codec, six long VP9 runs
keep `submitted=515 completed=515 drops=0 misplaced=0` with byte-identical
framemd5, EOS passes with `--hidden-pictures`, both AV1 clip gates pass, and
both slow-CAPTURE probes stay clean. Full identity fields, numbers and the
rollback path are in `TEST-RESULTS.md` §"WP1: 0006 loaded on sheng".

**Decision**: keep the module installed. The verdict is "no regression", not
"fixed"; 0006 remains experimental and outside the qualified 0001-0004
baseline. No codec, VA profile or publish policy is advertised from this run,
and the VA driver, capability table and trace vocabulary are unchanged. That
WP1 build did not include 0005; the separate 0005 run is WP2 below. The
verified rollback copy and `wp1-rollback.sh` restore the qualified module with
`depmod -a` and a reboot if needed.

### WP2 measure the cost of 0005 — done, verdict "do not load"

Measured twice. The trace-level cost of the extra NOSHOW completion is one
Import queue event and one borrowed client buffer per hidden install picture
(`TEST-RESULTS.md` §"WP2: cost of one extra NOSHOW completion"): 1-2 ms median,
11 ms worst case, Import backlog 3-4.

The target run then showed the cost is not a slowdown but a stop. With the
0005 module loaded (`6ef2fce8...`, boot `1edbadbe...`) all three qualified AV1
clips failed under import, copy and direct, the copy/direct traces ending in
`sync timeout` and `drain lost token=`, the import traces in
`publish=misplaced` (`TEST-RESULTS.md` §"0005 on the target"). The control arm
on the same-module-family baseline (`b90bd823...`) decoded the same clips with
the same client command to 120/120, 80/80 and 30/30 exact and
`errors=0 misplaced=0`, so the difference is the module and nothing else. The
completion 0005 recycles in the kernel is the one this driver uses to clear a
hidden pending target, so the cost of removing it is a userspace contract
change, not a queue event.

### WP3 0005 dual path — not implemented; the "perfect" form was built and measured, and it buys nothing

The probe this design depends on does not exist. 0005 adds no V4L2 control,
no QUERYCAP flag and no extended control, only the internal
`output_stopping` field in `iris_hfi_gen2.h`, so userspace cannot tell which
contract the kernel implements and the configure-time switch below would have
to guess. A dual path for 0005 as written also needs the full DPB-slot
rewrite, because the current accounting clears a hidden pending target from
the completion it recycles (`TEST-RESULTS.md` §"0005 on the target").

Four places in the session read that completion, and each one loses something
when the kernel keeps the buffer instead of completing it:

| Site | What the completion carries |
| --- | --- |
| `session.cc` hidden attribution (`noshow_shape`) | clears a `Target` submitted with `show_frame=0`; without it the client's `vaSyncSurface` waits until the timeout |
| `Target::invisible` counter | separates the install half of a two-completion unit from the client's picture; without it both halves look like the client's |
| `queue_import_next` | an Import entry leaves the backlog only when the slots lent for its token come back; a recycled slot is never dequeued by userspace, so the entry cannot be retired and the next completion lands in a slot lent for someone else |
| `publish()` | the returned buffer is the target's pixels under Direct, and the slot's token is the client buffer under Import |

The 720p traces from the target show both failures. Under Direct and Copy the
drain ends in `drain lost token=2` through `5`: the hidden targets never
complete. Under Import the backlog reaches 5 and the completion carrying
token 7 arrives in the slot lent for token 2
(`capture token=7 index=0 flags=0x4010 publish=misplaced slot_token=2`), which
fails both pictures (`session finish ... errors=6 misplaced=1`).

So "making 0005 work" is not polishing that patch. The only kernel form this
driver could consume keeps the completion and drops just the false
`V4L2_BUF_FLAG_ERROR` while keeping the timestamp. That form was first
modelled on the host with a `FakeDevice` knob that clears the flag: four
scripted cases — an unflagged hidden picture, an unflagged invisible half, an
unflagged hidden install unit, and the Import window on a two-completion
picture — all fail on today's code, three of them by leaving the client
picture pending, and a 29-line attribution in `handle_capture_returns` makes
the suite green (44 session cases, 0 failures; dropping the 29 lines
reproduces all four failures). Import needs no DPB rewrite in that form: the
completion still comes back, so the window rule is unchanged.

That form is still not better than today's route. The flag it removes is not
observable by any client in this project's gates: the VA path republishes
pixels through libva and never exposes V4L2 buffer flags to FFmpeg or Chrome,
and the one raw-V4L2 client, GStreamer's `v4l2av1dec`, does not use this
driver and already reaches EOS without losing frames on this target. The
upside is therefore zero measured change for a client, against a kernel patch
that has to be carried and 29 lines of tolerance no shipped kernel exercises.

That form was then built and run, not left as a prediction. `0007-*` keeps
the completion, attaches no error, no picture-type flag and no payload, with
`0004`'s timestamp; the userspace half is `exp/0005-keep-tolerance`. On
`e48243b8…` with module `f070ae18…`, the installed driver failed all seven
AV1 gates (2 of 120 frames on the 720p clip, `capture empty` six times, no
invisible halves), and the tolerance build decoded every clip byte-identical
to software with the qualified baseline's counters (`invisible=42`,
`capture noshow=59`). On the qualified module the tolerance build and the
untoleranted build are identical, because a kernel that sets the flag never
reaches the new branch. Six stages over three boots, including the rollback
to `b90bd823…` and a re-run that reproduced stage A exactly
(`TEST-RESULTS.md` §"The completion-preserving 0005 form on the target").
The patch is tracked with a "do not ship" verdict; the tolerance branch is
not merged.

The following design is retained for the record only. Do not implement it
without a kernel-visible capability signal, a new measured cost and an
explicit decision.

Probe for 0005 at configure time the way `sync_frame_control_` is probed. With
0005: `AccessUnit::completions = 1`, no second borrowed client buffer, no
`Target::invisible` accumulation. Without 0005: keep today's path. In the same
commit update session NOSHOW attribution, `queue_import_next`, the §8 trace
vocabulary and its consumers, invariant 17, the capability table `sync_note`
and both READMEs. Re-run the four-codec matrix, 120-frame AV1 clip, DRC,
10-bit probe and the Chrome/Firefox browser matrix.

### WP4 CDSP VPP — not merged; the Vulkan VPP shipped behind `V4L2_VA_VPP`

The 2026-11-12 read-only survey concluded "no proven viable backend" because
`vulkaninfo` and `glslangValidator` are absent on the tablet and no CDSP
session was opened. Those two tools are diagnostics and a host shader
compiler, not the runtime contract.

The earlier record that "`sheng` cannot open a FastRPC session at all" was
wrong, and measuring it replaced three guesses with three facts
(2026-11-13). The session opens: `remote_handle_open` reaches the
kernel, and `DSPRPC_GET_DSP_INFO` reports `DOMAIN_SUPPORT=1`,
`UNSIGNED_PD_SUPPORT=1`, `HVX_SUPPORT_128B=4`, `ARCH_VER=0x1008c73` (v73) and
`VTCM_PAGE=8 MiB` for the CDSP domain, with no shell binary present at all.
The session was previously thought to be blocked because the only probe had
been `remote_handle_open` on a nonexistent module, whose `EBADFD` reads as a
session failure and is not one.

What does block it is the PD create, and the shell binary is not the reason
either. A v73 `fastrpc_shell_unsigned_3` taken from Fedora's
`hexagon-dsp-binaries-qualcomm-hamoa-iot-evk` package, placed at the path the
library resolves (`/usr/share/qcom/sm8550/Xiaomi/sheng/cdsp/`, from
`/usr/share/qcom/conf.d/sheng.yaml`), still fails `FASTRPC_IOCTL_INIT_CREATE`
with `0x80000416`. An empty file of the same name fails identically, so the
failure is upstream of the shell's contents: the kernel hands the create to
the CDSP and the firmware refuses it. The device node the library opens is
`/dev/fastrpc-cdsp-secure` — the DT node is `qcom,non-secure-domain` with no
`qcom,nsessions` and no `memory-region`, and the library selects the secure
node. Nothing in this repository can change that; it needs a device-tree
change and a firmware that accepts the unsigned PD.

The toolchain half of the old claim is also wrong. Fedora's clang 22 compiles
`--target=hexagon -mv73` to a `QUALCOMM DSP6` object with no Qualcomm
toolchain installed, and clang ships `hexagon_types.h`, `hexagon_protos.h` and
`hexagon_circ_brev_intrinsics.h`. What is missing is the linker: `ld.lld`
has no `elf32hexagon` emulation and clang looks for a `hexagon-ld` that is not
installed, so a shared object cannot be produced. The pinned Open Access
archive would supply both, and it answers 403 to an unauthenticated download
from this host.

A DSP backend would therefore need a device-tree change, a firmware that
accepts the PD, and a working Hexagon linker before it could be measured,
against a Vulkan backend that already passes every gate. The CDSP/FastRPC
path is not needed for this backend and was not probed further.

A userspace probe settles the
question on `sheng`: `test/vulkan-vpp/prereq.sh` builds a SPIR-V compute
pipeline, imports two DMA-BUF fds as Vulkan external memory (one read, one
written with coherent CPU read-back), and checks exact output for both
DMA-BUF origins the driver allocates (MSM GEM PRIME fd and system DMA heap).
Both pass on Turnip Adreno 740; the run record
is `run-20261112T205511Z` (guard `complete`, exit 0), recorded in
`TEST-RESULTS.md` §"Vulkan VPP prerequisites on the target".

This reverses the "no viable backend" verdict, for the backend alone: a
Vulkan compute pipeline can import both DMA-BUF origins and write them back
on this device. The VA entrypoint was still missing at that point, and the
capability table is still unchanged.

A second gate closes the gap between "compute can read/write a flat DMA-BUF"
and a real scaled frame: `test/vulkan-vpp/nv12_scale_gate.sh` runs
`nv12_scale.spv` over two imported NV12 DMA-BUFs in the exact Iris CAPTURE
layout (128-byte stride, chroma behind luma, 2x subsampling) and checks every
destination byte against a 2x nearest-neighbour CPU oracle, for both DMA-BUF
origins. Both pass byte-exact (12288/12288 pixels) on run
`run-20261112T220313Z` (guard `complete`, boot `3356e00e`, module `b90bd823`,
driver `323c07cb`), recorded in `TEST-RESULTS.md` §"NV12 scale primitive on the
target". The filter kernel now has a per-pixel target baseline, which is the
load-bearing evidence WP4 asked for before advertising VPP.

The VA plumbing that WP4 listed as remaining is implemented on
`feat/absorb-upstream-advantages` (PR #7): `ctx->vtable_vpp` with its three
query hooks, `VAProfileNone` + `VAEntrypointVideoProc` config and context
behind `V4L2_VA_VPP`, buffer types 41 and 42, and `endPicture` dispatching the
pinned SPIR-V over the source and output surface DMA-BUFs. A VideoProc
context opens no node and owns no session. It samples inside the surface region the client names, so a crop is a
first-class picture rather than a post-step, applies the clockwise quarter turn
the client asks for before scaling, and it runs the same map on
P010 with a second kernel (same-depth only: a mixed-depth picture is
refused, because swscale's own 10-to-8 conversion dithers). One filter is
implemented, `VAProcFilterHighDynamicRangeToneMapping`, which converts P010
BT.2020/PQ to SDR NV12 through `tonemap.spv`. A picture may also carry a second
pipeline buffer that composites a surface into one even rectangle of the output
with a global alpha — the shape `overlay_vaapi` submits — through
`overlay.spv`, importing three DMA-BUFs. Mirroring on either axis, and on
both, runs after the rotation and before the scale, so all seven
`transpose_vaapi` directions a client can name are implemented. Everything else
(per-pixel alpha, luma keying, a third pass, a second filter, any other colour
conversion) fails the picture with a VA status instead of being approximated.
Vulkan stays an optional meson dependency, so CI builds the stub and
advertises nothing.

Interpolation follows the picture's `filter_flags`: the default and
nearest-neighbour are the point map above, bilinear is a four-tap blend around
the destination sample's centre, and the "advanced" method is refused rather
than approximated. The caps name bilinear, because that field holds one method
and `NEAREST | BILINEAR` is the advanced value. On P010 the blend runs on the
ten-bit code, not on the padded word. `TEST-RESULTS.md` §"Bilinear interpolation
in the video processor scale" records the batch and its eight gates.

The backend samples where swscale's `SWS_POINT` map samples - the rounded
16.16 step `xInc` and `(i * xInc + (xInc >> 1)) >> 16` - so it scales at any
ratio and not only 2x, and the VideoProc config advertises its own 16x16
surface floor instead of the decoder node's minimum. `scale_vaapi=w=W:h=H` is
then byte-identical to `scale=WxH:flags=neighbor` at every geometry the gate
runs (`TEST-RESULTS.md` §"Any ratio, and the gate run from the installed
package"), eight of eight from both `build/src` and the installed package, and
the same client is refused without `V4L2_VA_VPP`.

The installed-package run is part of that: `scripts/install-system.sh` put
artifact `f6095e79` in `/usr/lib64/dri` with the previous `323c07cb` build kept
as the rollback copy, and the four-codec matrix, the real AV1 clips, the
encode gate and one Chrome acceptance run were repeated on it. Equal in/out
geometry is not a gap: FFmpeg passes such a frame through without calling the
driver. The one behaviour this work surfaced and did not change is that a
surface written through a derived image (an upload with no filter) still fails
`vaGetImage`, because `has_pixels()` only counts decoded and VideoProc
completions, and `vaPutImage` was unimplemented. Both are fixed and gated:
`vaPutImage` copies whole frames and even regions, a derived-image write and a
`vaPutImage` both count as pixels, and `test/iris-va-image-gate.sh` proves the
API contract plus four `hwupload,hwdownload` round trips
(`TEST-RESULTS.md` §"Client pixels: `vaPutImage`, and a read-back that used to
refuse"). The previous installed build fails the same gate ten checks of
fourteen, which is the negative control.

### WP5 RGB import — done, verdict "packed RGB exists for the processor, none for decode"

The original verdict was "no RGB import code". That held for a codec
surface, and still does: no Iris node advertises an RGB CAPTURE format and the
capability table's RGB rejection entries stand. It stopped holding for the
video processor, which is where a real consumer appeared. `overlay_vaapi`
uploads its overlay input into a `VA_RT_FORMAT_RGB32` surface
(`vf_overlay_vaapi.c`), so the surface layer takes `RGB32` on its own, accepts
`VA_FOURCC_BGRA` and `VA_FOURCC_RGBA` as attributes, maps them to
`DRM_FORMAT_ARGB8888` and `DRM_FORMAT_ABGR8888` as a single 32-bit plane, and
`overlay_rgb.comp` converts each sampled pixel to the output frame's matrix
while it mixes. `overlay_vaapi_gate.sh` drives it through FFmpeg
(`TEST-RESULTS.md` §"A packed overlay, stored alpha, and the image list the
client sizes").

What remains unimplemented is RGB on a decode or encode surface, which is a
different thing and has no client: the CAPTURE layout is still NV12/P010. A
driver importing client RGB buffers would need the modifier negotiation the
capability table already records as rejected.

### WP6 Debian packaging — done, verdict revised: `debian/` exists and the artifact is verified

The original verdict was "do not add debian/" for three reasons: `libgbm-dev`
pins an exact `libgbm1` and can downgrade a vendor-patched graphics stack
(`mesa-libgbm` and `mesa-libgbm-devel` are 26.2.5 on `sheng`), no Debian or
Ubuntu system is a qualified target, and a package that cannot reproduce the
Fedora/Arch provenance and installed-package gates would add an unverified
distribution surface.

The first and second still hold. The third is a cost of building it properly:
the package is built in a clean bookworm chroot (never on the device, which is
what the GBM pin argues for anyway), `debian/rules` installs through
`scripts/install-system.sh` and refreshes the provenance manifest after
`dh_strip`, and the built artifact was run on `sheng` against the decode matrix
and the `overlay_vaapi` gate. The clean build also found two real defects that
the Fedora build hides — a missing `<cstddef>` in three headers and the
undeclared FFmpeg dependency of every format's test stage — which is the kind
of surface this work package existed to keep honest.
`TEST-RESULTS.md` §"Debian packaging: built in a bookworm chroot, run on the
target" carries the dependency list `dh_shlibdeps` produced, the manifest hash,
the two reproducible builds and the two guarded runs. What remains unproven is
a Debian runtime: the package was built on bookworm and exercised on Fedora 44.

### WP7 SoC portability audit — done, docs only

`docs/soc-portability.md` lists the SM8550-specific kernel hunks,
`inst_fw_cap` platform data, BTI/PAC build identity, node-resolution rule,
capability table, defaults, packaging and validation records, then names the
patches, probes and gates a new SoC must redo. It changes no capability entry
and makes no support claim for another SoC.

### Order and decision gates

1. WP0 -> WP1 -> WP2, then decide WP3 from WP2's verdict.
2. WP4 may run alongside WP1/WP2 as read-only feasibility research; WP5 waits
   for WP4; WP6 and WP7 are independent.
3. Stop after each package and wait for confirmation before opening the next.

### Known state that does not match earlier drafts

PR #4 is `MERGED` (not open, not closable) and its head `a7e6d26` is contained
in PR #5's merge commit. No PR comment is needed unless one is explicitly
authorised. `radxa-pkg/qcom-va-driver` v1.1.1 has no 0006; 0006 comes from
`yzddmr6/linux-sm8750-piano` (F8). Upstream marks 0005 "required for AV1"
because its AV1 path replays a whole hidden picture; our internal DPB consumes
the NOSHOW completion instead, so the two designs are not interchangeable.
That is now measured rather than inferred from the patch text: on `sheng` with
the same clips and node, their backend is AV1-exact only under 0005 and wrong
from frame 33 without it, and this driver stops after two pictures under 0005
(`TEST-RESULTS.md`, 2026-11-13).
