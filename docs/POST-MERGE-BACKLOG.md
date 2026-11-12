# Post-merge hardware and upstream backlog (2026-11-10)

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
| Known-unqualified | VP9 Import (`publish_import: fallback`), AV1 film grain, AV1 12-bit, AV1 large-scale tile, Firefox AV1 hardware decode, long-window soak, power measurement |

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
| L2 | Screen-off / fixed brightness / fixed refresh state for power runs | `test/iris-power-sampler.sh` refuses `Charging|Full` because a plugged-in gauge cannot measure battery-side draw; without a controlled display state a power delta is not attributable to decode | record display state, brightness, refresh rate, network and input clip identity |
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
| A3 | Build and load 0001-0005 on `sheng`, reboot, confirm the module hash and active controls | target, can reset firmware | A2 design, L1, guard wrapped | **not applicable**: A2 settled that 0005 is not needed and would lose frames with this userspace | none; 0005 remains unbuilt and unloaded (0006 later had one separate guarded WP1 run) |
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
| B1 | Add `refresh_frame_flags` to `VADecPictureParameterBufferAV1` using a word from `va_reserved[VA_PADDING_MEDIUM]`, keeping struct size and ABI; add NEWS/docs | `intel/libva` | `va/va_dec_av1.h` last changed in 2021; no such field; libva 2.15 added the field for **encode** only | upstream merged and installed on the target; `VA_CHECK_VERSION` guard defined |
| B2 | Fill the field in `libavcodec/vaapi_av1.c::vaapi_av1_start_frame()` from `frame_header->refresh_frame_flags` | FFmpeg | `vaapi_av1.c::end_frame()` already reads the value for film-grain refs; `7432fed62` (2026-07-29) removed deprecated fields; `4221a63ce` (2026-10-17) fixed unreleased-picture buffers; neither adds the field | patched FFmpeg installed; `pic_param` trace shows the exact mask |
| B3 | Fill the field in Chromium `media/gpu/vaapi/av1_vaapi_video_decoder_delegate.cc` (libgav1 already parses it) | Chromium | no such change found | Chrome with the new libva reports hardware AV1 with the same frame/drop evidence |
| B4 | Raw-OBU passthrough instead of reconstructed headers, following FFmpeg's `videotoolbox_av1.c::end_frame()` which appends `s->current_obu.units[i]` | libva + FFmpeg + Chromium | VideoToolbox works, VA-API has no equivalent buffer semantics | API meaning agreed upstream; no driver-side OBU rebuild needed |
| B5 | Explicit VA-API flush / end-of-stream entry point | libva + clients | none; `docs/architecture.md` §9 records this as the missing entry point | client can say "no successor is coming" without a sync timeout |
| B6 | Vulkan Video AV1 (`StdVideoDecodeAV1PictureInfo::refresh_frame_flags`) | whole client stack | separate API, not the Iris V4L2 path | only if the platform gains a Vulkan Video driver; treat as a research note, not a task |
| B7 | Driver-side use of exact flags with fallback to the current successor-based reconstruction | this repo | blocked on B1-B3 | two paths both covered by tests; flags absent never guessed; old clients still correct |
| B8 | Do **not** do: hardcode `refresh_frame_flags = 1` (Mesa's gallium-internal choice), guess the mask, or silently drop frames | — | the earlier dynamic-DPB rewrite decoded 33/120 real frames | any prototype must fail closed and be gated by the full A11 corpus |

## 5. Workstream C — Chrome VP9 Import

Current state: `data/iris-codec-capabilities.json` has VP9
`publish_import: fallback`. FFmpeg is clean (515/515, `misplaced=0`, framemd5
equal to software) with one picture one buffer per completion. Chrome releases a
VP9 surface before its picture completes: 564 of 768 queueings abandoned, 541
completions dropped, against 2 and 4 for the H.264 control.

| ID | Item | Pass condition |
| --- | --- | --- |
| C1 | Re-measure Chrome's early surface release on the merged baseline with CDP frame/drop deltas and `abandoned=` trace counts; separate "surface released" from "picture completed" | a per-surface timeline with no ambiguous attribution |
| C2 | Decide the ownership model: retained/duplicated DMA-BUF fd after `destroySurfaces`, or a copy window; write it down before code | one written model, no half-state |
| C3 | Kernel patch 0006 (`7c28b27`, unmatched timestamp metadata ring) — decide whether it is needed at all on SM8550 | **WP1 settled the local kernel question**: the slow-CAPTURE defect was not reproduced on 0001-0004, and one guarded 0001-0004 + 0006 run passed with "no regression". 0006 stays experimental and out of the qualified baseline; no VP9 Import qualification follows from it. Chrome's early surface release (C1/C2) is still open. |
| C4 | Target run: one-buffer vs two-buffer window; known bad case 153/609 misplaced must stay a regression test | FFmpeg 515/515 clean, Chrome no drops, `misplaced=0`, exact framemd5 |
| C5 | Only after C4: qualify VP9 `publish_import` in the table, both READMEs, §8 trace vocabulary, TEST-RESULTS.md and CI | evidence-before-capability satisfied; otherwise Copy stays |

## 6. Workstream D — AV1 feature completeness

| ID | Item | Recorded boundary | Pass condition |
| --- | --- | --- | --- |
| D1 | 1080p 10-bit AV1 | 1920x1080 60-frame `yuv420p10le` clip passed on 2026-11-11: `EXACT` framemd5, 60 Import, 0 errors/drops/misplaced on the installed package; depth is decided before the first AU | done for exact framemd5; a 10-bit browser pixel comparison against software output is still open |
| D2 | Film grain | a scratch build wrote a spec-correct `film_grain_params()` OBU (libdav1d decoded the rebuilt stream) but the hardware run still ended in `getImage: surface has no decoded pixels`: VA-API needs `current_frame` plus a separate `current_display_picture`, and this driver publishes one target per completion. `-export_side_data film_grain` decodes all 24 frames in hardware | keep the fail-closed rejection with the two-surface reason, the host guard and the capability table `rejected` entry; a kernel patch does not remove the VA surface contract |
| D3 | 128x128 superblock / loop-restoration unit shift | one-bit header bug fixed; GStreamer `vaav1dec` still refuses the codec | retest on merged baseline with a native control; browser evidence required for any claim |
| D4 | Tiles, superres, lossless, still picture, global motion, lookahead, 4K 10-bit | **partly done (A11)**: five 640x360 cases (`baseline`, `tiles 2x2`, `lag-in-frames 12`, `still-picture`, `lossless`) each have an independent libdav1d oracle and 5/5 exact framemd5 on the installed package, all `publish=import`, `errors=0 drops=0 misplaced=0`. `superres` produced no packets from the target `libaom-av1`; `global motion` decoded identically to baseline. 4K 10-bit and independent browser pixel comparison remain open | independent software oracle per case (A11), exact pixel hashes, no new `publish=error/drop/misplaced` |
| D5 | AV1 12-bit and large-scale tile | in the capability table `rejected` | keep rejected until a hardware gate exists; do not infer from format enumeration |
| D6 | `show_existing_frame` / hidden alt-ref ordering | §17/18 invariants | explicit negative tests for a hidden picture landing on a shown surface and vice versa |

## 7. Workstream E — long-window, power and cost evidence

| ID | Item | Why open | Pass condition |
| --- | --- | --- | --- |
| E1 | Two-hour concurrency soak (`dual-h264` and `mixed`, 172800 frames per context) | **done 2026-11-12**: both windows ran the full 7200 s on the installed package, boot ID unchanged, no reset or breach; every context submitted and completed 172809 access units (172800 output frames) with `errors=0 drops=0 timeouts=0 misplaced=0`, `drains=1`, `output_queued=0`, and both preflights matched their software framemd5. Peaks are attributed in `TEST-RESULTS.md`: 41.0 C and 47.7 C while decoding, 56.8 C and 78.9 C over the whole run, the higher figures coming from the fixtures the soak generates with x265. The media is 640x360, so this is a stability window and not a resolution or power result |
| E2 | `test/iris-power-sampler.sh` hardware/software A/B | the sampler refuses `Charging|Full` because a plugged-in gauge cannot measure battery-side draw; it runs on any discharging pack | unplugged, fixed brightness/refresh/network/clip, voltage x current samples, start/end capacity recorded, explicit "not comparable" note for any differing input |
| E3 | 4K continuous decode thermal and power | the guarded 4K probe is 30 frames / 30 s and explicitly "not a thermal, power or long-window result" | longer run with guard, temperature ceiling, recorded pack state, no reset |
| E4 | Per-frame 4K cost | 2026-10-25 finding: growth is DVFS, not accumulation; big-cluster clock rises when unpaced | pin core and frequency before quoting a per-frame number; record the clock state and pack state with the measurement |
| E5 | Make the guard pack-aware | L3 | **done**: `battery.log` records capacity, status, voltage, current and USB online; the self-test covers 19 % discharging, 100 % charging and a missing gauge, all reaching `status=complete`. Boot/thermal guards are unchanged |

## 8. Workstream F — kernel and upstream sync policy

Current conclusion: **do not rebase, do not follow a whole upstream tree**. Keep
`main` as the baseline, subscribe to releases, and pull only a change that maps
to one of the items above.

| ID | Item | Evidence | Decision rule |
| --- | --- | --- | --- |
| F1 | `sheng-7.2.6` pin `42f3b40c702a` | unchanged; our 0001-0004 are rebased onto it | keep until a specific gain is demonstrated |
| F2 | `sheng-7.3.0` | Iris changes there are Milos VPU v2 platform data, Gen2 firmware autodetect/fallback, UBWC config guard, VBR time-delta RC change, request-key-frame, PM resume and OPP fixes; none targets a recorded SM8550 defect | rebase only if a named defect is fixed; a rebase invalidates §0 and requires A12-equivalent re-verification |
| F3 | `radxa-pkg/qcom-va-driver` v1.1.1 (2026-10-28) | 0004 and 0005 blobs identical to ours; 0001-0003 differ only because ours merges 0001+0002, rebases to `sheng-7.2.6`, and adds two domain/firmware guards; added-code comparison shows functional equivalence | do not replace our patches; record the diff result in `TEST-RESULTS.md` (H2) |
| F4 | Upstream activity watch | 14 commits in 2026-09, 37 in 2026-10, last commit 2026-10-28; open PRs/issues #7 packed sequence headers, #5 Venus stateful, #2 rate control, #6 HEVC extradata — none maps to Firefox AV1, Chrome VP9 Import or SM8550 evidence | monthly re-check; only open a branch for a mapped change |
| F5 | `mxsrc/libva-v4l2` `74b336a42` (unset `destination_buffer` in `exportSurfaceHandle`) | our `src/va/surface.cc:429` already rejects a null descriptor and uses `shared_ptr<StableBuffer>`, no `std::optional` | no action |
| F6 | FFmpeg `7432fed62` (stop setting deprecated AV1 VA fields) | our `src/codec/av1_obu.cc:145` writes `chroma_sample_position: UNKNOWN`; we never read `tg_start`/`tg_end` | no action; confirm with a matrix run if the target's FFmpeg is upgraded |
| F7 | `intel/libva`, Firefox `FFmpegVideoDecoder.cpp` | `va_dec_av1.h` last changed 2021; recent Firefox changes in that file are Vulkan Video / Android, the "send one frame, immediately receive" loop is unchanged | re-check before starting B work |
| F8 | `yzddmr6/linux-sm8750-piano` v1.2.0 (2026-11-10) | camera, serial, audio; no new Iris commits; 0006's origin commit unchanged | no action |
| F9 | `GhfunNiHe/sheng-fedora-packages` | last push 2026-10-25; pins patches against `iris-kernel-patches-v1` | re-check packaging pin only if a patch hash changes |
| F10 | 0005/0006 experiment branch | no such branch exists; only `feat/vpu-zerocopy-ubwc` and its fork remote; 0006's WP1 build and guarded target run are recorded on `chore/post-merge-hygiene` | open a separate branch before any 0005 work; never enabled by default |

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

## 10. Workstream H — encode backlog

Lower priority than decode. Ground each item in the capability table's
`rejected` list rather than in an assumption about the firmware.

| ID | Item | Pass condition |
| --- | --- | --- |
| H1 | Investigate the recorded rate-control gap against the piano comparison, with a fresh A/B on the merged baseline | a measured, reproducible statement of what differs; no capability change without a gate. **Verdict: closed, code fixed.** The gap was the translator, in two parts. `VAEncMiscParameterRateControl.bits_per_second` is the controller's *maximum* and `target_percentage` is the target inside it (`va.h`), and both H.264 and HEVC translators programmed the raw maximum — FFmpeg turns a plain `-b:v 4M` into 8 Mbit/s at 50 %, so the encoder ran at twice the request. The second part only showed up after the first was fixed: FFmpeg renders the rate-control misc buffer with the first picture only, and the fallback to the sequence field reprogrammed the controller between 4 and 8 Mbit/s on every picture (ten alternating writes per 600-frame arm, 7934 kbit/s). The fix takes the percentage into the target, writes the maximum above it as `BITRATE_PEAK` for VBR as well as CBR, and keeps the last configured target for a picture that carries no rate control. `vbr4` went 8000 -> 3992 kbit/s with one write per control, and `encode-params-test` covers both halves. `TEST-RESULTS.md` carries the seven-arm before/after with the boot ID, both driver digests and both node roles. The piano comparison's remaining difference is content- and firmware-level, not this mapping |
| H2 | Packed sequence headers / extradata for H.264 and HEVC encode | upstream has an open issue on this; decide whether it is in scope for this driver at all before coding. Settled: do not do it here — `VA_ENC_PACKED_HEADER_NONE` is what opens the GStreamer path, and `picture.cc` silently drops packed header buffers, so advertising support would desynchronise MP4 extradata and reopen the double-claim `size 0` crash |
| H3 | Recheck each `rejected` entry (B-frames, packed headers, multi-slice, tiles/WPP, weighted prediction, scaling lists, chroma QP offsets, multiple reference indexes, 10-bit/non-4:2:0, HEVC Main10 encode) on the current firmware | either a hardware gate with tests, or keep the explicit `VA_STATUS_ERROR_UNIMPLEMENTED` and the table entry. A host-side audit found code-level `require` backing for every entry; the only drift is the conditional deblocking item, which no longer fires on a kernel with 0002 installed. Firmware-level availability still needs a hardware gate |
| H4 | Keep the double-claim and `REQUEST_SYNC_FRAME` regressions covered | existing host/target gates stay green after any encode change. `encode-session-test` now links `src/va/buffer.cc` and covers the coded buffer's claim: nothing outstanding is success with no invented bytes, a buffer that lost its session is refused, the first claim stores the access unit and the segment, a second claim on an already-answered ticket leaves those bytes alone, and a buffer smaller than the access unit is refused rather than truncated. The empty-answer case fails on the pre-fix code (checked by removing the guard), so it is a regression test rather than a restatement. Still open: GStreamer `vah264enc`/`vah265enc` has no scripted gate because this target has no GStreamer VA plugins installed (`gst-inspect-1.0` finds none of them), so that gate needs a target with them, not a code change |

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
| §2 Lab prerequisites | L1-L6 | L4 settled as "pass an explicit `IRIS_EXPERIMENT_DIR` under `~/Lab/Bridge/tmp` for every guarded run" (all recorded runs do); the default is left alone so existing `guard.state` semantics do not drift. L1 charger, L2 screen state, L3 guard precondition remain open; L5/L6 are per-run checks |
| §3 AV1 held-picture removal | A1-A14 | done: internal DPB is the default, installed-package matrix/clip/DRC/EOS gates and Firefox/Chrome live-site browser gates pass on 2026-11-11; `TEST-RESULTS.md` records the run. Long corpus and 4K cost work remains in §6/§7 |
| §4 Explicit refresh flags | B1-B8 | open / upstream-blocked; internal DPB removes the local need for an API change |
| §5 Chrome VP9 Import | C1-C5 | open: session-side window is fixed (FFmpeg 515/515, misplaced=0); Chrome reuses a VP9 surface in `beginPicture`, which releases the still-in-flight picture. In the forced-Import run `submitted=718 completed=177 drops=541`, with 564 of 768 import queueings abandoned; the forced-Copy baseline completed 708 with `drops=1`. The H.264 control had 2 abandoned and 4 drops out of 670; the numbers are in `TEST-RESULTS.md` §"Why VP9 Import still is not qualified". Qualifying this needs a retained-target ownership model, its own host regressions for the misplaced/detached case, and a fresh target gate — not a rename of `publish=drop` |
| §6 AV1 feature completeness | D1-D6 | D1 done for exact framemd5: 1920x1080 60-frame 10-bit clip `EXACT`, 60 Import, 0 errors/drops/misplaced on 2026-11-11. D2 settled as a user-space second-target publish gap, not a kernel limitation: scratch OBU writer was libdav1d-clean and the firmware carries the grain capability flag, but the display surface stayed empty; host guard keeps the refusal fail-closed. D4 partly covered by the A11 corpus (tiles, lookahead, still-picture, lossless exact; superres and global motion excluded with a recorded encoder reason). Still open: 10-bit browser pixel comparison, 12-bit, large-scale tile, D3 (128x128 SB / GStreamer), D5/D6 negative tests |
| §7 Long-window, power, cost | E1-E5 | E1 done (both 7200 s windows, clean ledgers), E3 and E4 done (20-minute continuous 4K inside the guard; 3.07 ms per frame with the clocks pinned and the clock state recorded), E5 and L3 done (the guard records pack state); E2 stays open on the software arm's thermal boundary, which is recorded as a boundary rather than a cancelled run |
| §8 Kernel and upstream sync | F1-F10 | open / decision recorded as "do not rebase"; 0005 is unbuilt and unloaded; 0006 was built, installed with 0001-0004, rebooted and run through one guarded WP1 gate on `sheng` with a "no regression" verdict, still experimental and outside the qualified baseline |
| §9 Docs, consistency, hygiene | G1-G14 | all closed: G1, G2, G3, G7, G8, G12, G13 and G14 done; G4, G5, G6, G9, G10 and G11 closed with explicit reasons (untracked scratch notes, intentional scratch default, kernel SPDX provenance, release-note identity fact, no new trace marker) |
| §10 Encode | H1-H4 | H1 closed with a code fix: both translators now take `target_percentage` into the target, carry the client's maximum as `BITRATE_PEAK` for VBR, and keep the last configured target when a picture omits rate control; `vbr4` measured 8000 -> 3992 kbit/s. H2 is settled as out of scope, H3 has a host-side audit, H4's double-claim half is covered by `encode-session-test`, the GStreamer half stays blocked on a target that ships the VA plugins. No capability entry or profile changed |
| §11 CI, tests, release plumbing | I1-I7 | open: host tests (25/25), guarded installed-package gates and the repo-owned 17-case AV1 browser matrix are in PR #5; packaging/CI automation for those target gates remains open |
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
and the VA driver, capability table and trace vocabulary are unchanged. 0005
was not built or loaded. The verified rollback copy and `wp1-rollback.sh`
restore the qualified module with `depmod -a` and a reboot if needed.

### WP2 measure the cost of 0005 — done, verdict "not worth doing"

Measured from the existing timestamped traces, with no new target run and no
driver change (`TEST-RESULTS.md` §"WP2: cost of one extra NOSHOW completion").
The extra completion costs one Import queue event and one borrowed client
buffer per hidden install picture; trace-level delay is 1-2 ms median, 11 ms
worst case, and the Import backlog stayed at 3-4. The 120-frame and DRC clip
traces were not retained, so those contribute counts (42 and 63 invisible)
only. Change the shipped userspace for 0005 only if a future measurement
shows a real target-level cost; the current evidence does not.

### WP3 0005 dual path — not implemented (WP2 verdict "not worth doing")

The following design is retained for the record only. Do not implement it
without a new measured cost and an explicit decision.

Probe for 0005 at configure time the way `sync_frame_control_` is probed. With
0005: `AccessUnit::completions = 1`, no second borrowed client buffer, no
`Target::invisible` accumulation. Without 0005: keep today's path. In the same
commit update session NOSHOW attribution, `queue_import_next`, the §8 trace
vocabulary and its consumers, invariant 17, the capability table `sync_note`
and both READMEs. Re-run the four-codec matrix, 120-frame AV1 clip, DRC,
10-bit probe and the Chrome/Firefox browser matrix.

### WP4 VPP feasibility spike — done, verdict "do not implement VPP"

Read-only inventory on 2026-11-12 (`TEST-RESULTS.md` §"PR #6 tablet read-only
survey"): `/dev/fastrpc-cdsp` and `libcdsprpc.so.1` exist but there is no
Hexagon compiler; Vulkan 1.4.341, `libvulkan.so` and the freedreno ICD exist
but `vulkaninfo`/`glslangValidator` are absent. Neither backend is
buildable/verifiable as-is, and no CDSP session was opened. The current
verdict is "no proven viable backend", subject to a runtime CDSP probe and
package availability checks.

We have no VPP. `radxa-pkg/qcom-va-driver` has optional CDSP and Vulkan VPP
paths (scale, crop, rotate, bit-depth conversion, RGB overlay, HDR10 P010 to
SDR NV12 tone mapping), but the piano spec disables both: CDSP needs a Hexagon
compiler and the Vulkan path was not validated on the device. Check on `sheng`
and Fedora 44 whether a CDSP FastRPC session is reachable and whether
`vulkan-devel` plus `glslangValidator` are available. If a backend is viable,
do the smallest slice: `VAProfileNone` plus one NV12 scale filter, verified on
the target by per-pixel or PSNR baseline. If neither is viable, write the
feasibility conclusion and stop. Do not advertise VPP without a working
backend.

### WP5 RGB import — done, verdict "no RGB import code"

WP4 did not produce a viable backend, and no VA client in this project's
recorded gates needs BGRA/RGBA. The current NV12/P010 CAPTURE-layout contract
is what the decode and encode gates exercise. Keep the capability table's RGB
rejection entries. `radxa-pkg/qcom-va-driver` is the reference implementation
if a real consumer appears (`TEST-RESULTS.md` §"WP4/WP5/WP6 decisions").

### WP6 Debian packaging — done, verdict "do not add debian/"

`docs/build-and-deploy.md` records that `libgbm-dev` pins an exact `libgbm1`
and can downgrade a vendor-patched graphics stack. The 2026-11-12 survey
confirms `mesa-libgbm` and `mesa-libgbm-devel` are both 26.2.5 on `sheng`.
No Debian/Ubuntu distribution is a qualified target; a package that cannot
reproduce the Fedora/Arch provenance and installed-package gates would add an
unverified distribution surface (`TEST-RESULTS.md` §"WP4/WP5/WP6 decisions").

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
