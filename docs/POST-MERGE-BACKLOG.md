# Post-merge hardware and upstream backlog (2026-11-10)

This is the completion ledger for the work consolidated into PR #4. The
2026-11-10 draft (PR #3) froze the baseline and listed every open workstream;
PR #4 then carried out the AV1 held-picture removal. Items below are marked
with their current state in §15. Nothing here is a capability claim: a codec or
publish policy is advertised only after its hardware gate passed and
`TEST-RESULTS.md` records the run. Frame/drop evidence and a media-surface
decoder report are required before any browser hardware claim.

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
| L1 | Connect a charger and confirm `qcom-battmgr-usb online=1` | Every 2026-11-10 run was `Discharging`: 26 % at -2.195 A, 51 %, 60 %, 58 -> 54 %. The 4K clip and browser windows are short capability probes, not power or soak evidence. | `status=Charging` or `Full`, stable capacity, recorded before and after |
| L2 | Screen-off / fixed brightness / fixed refresh state for power runs | `test/iris-power-sampler.sh` deliberately refuses `Charging|Full` and needs an unplugged, charged pack; without a controlled display state a power delta is not attributable to decode | record display state, brightness, refresh rate, network and input clip identity |
| L3 | `scripts/iris-experiment-guard.sh` currently has no battery or charger check | It records boot ID, thermal inventory and the 85 C ceiling, but a run can start while the pack is nearly empty | decide whether to add a pack precondition to the guard, with a self-test; do not silently change the guard's pass semantics |
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
| A3 | Build and load 0001-0005 on `sheng`, reboot, confirm the module hash and active controls | target, can reset firmware | A2 design, L1, guard wrapped | **not applicable**: A2 settled that 0005 is not needed and would lose frames with this userspace | none; 0005/0006 remain unbuilt and unloaded |
| A4 | Short AV1 decode against the new module with `V4L2_VA_TRACE=1` only to characterise suppressions | target | A3 | **done on 0001-0004**: guarded real VA-API sessions recorded `publish=invisible`, `publish=direct`/`publish=import`, `errors=0`, `misplaced=0` and bit-exact rebuilt OBUs | none; build-tree override only, installed driver untouched |
| A5 | Rewrite the AV1 translator to emit one complete temporal unit per `vaEndPicture` with an explicit 8-slot DPB and hidden-reference replay | code | A1, A4 | **done**: `src/codec/av1_dpb.cc`, `av1_build_install_temporal_unit()`; host tests cover DPB-full install, pending-surface references, client hidden frames, sequence restart and no held EOS tail | revert `src/va/picture.cc` to `make_av1()` |
| A6 | Rewrite the session / surface / EOS accounting for the completions | code | A4, A5 | **done**: `unsent`, `wait_for_release()`, `release_held_picture()` and the bounded-release tail are removed; orphaned invisible halves are consumed as `publish=invisible`; `test/iris-clip-trace-check.sh` still balances submitted/completed | revert `src/iris/` and `src/va/` changes |
| A7 | Re-derive Import ownership under the new ledger | target | A5, A6 | **done**: 79 Import queue events for 49 pictures, 30 `publish=invisible`, `misplaced=0`, `errors=0` as a build-tree probe; the 2026-11-11 installed-package gates then passed (48/48 matrix, 120/120 Import clip, 60 Import 10-bit frames), `misplaced=0` throughout | keep `publish_import: qualified` only while the installed-package gates pass, otherwise return AV1 to the recorded fallback before any claim |
| A8 | DRC path under the default translator | target | A7 | **done**: the 1280x720 -> 854x480 in-context clip reached 80/80 exact framemd5 on the installed package with `import=42 copy=42 invisible=63`; the copy half is the recorded layout-mismatch fail-safe | restore `make_av1()`; do not advertise a partial DRC result |
| A9 | EOS and tail behaviour | target | A6 | **done**: installed-package file decodes finished without a session sync timeout (`matrix timeouts=0 drain=1`, p10 `timeouts=0 drains=1`, 120/80-frame framemd5 files complete); host EOS/no-tail test also passes | revert to `make_av1()` and keep the former fail-closed rule |
| A10 | Firefox 158 live YouTube AV1 and live Bilibili, plus Chrome and FFmpeg controls, on the installed package | target | A7-A9, L1 | **done**: the 2026-11-11 17-case guarded browser matrix reports `failures=0`, `PASS AV1 browser matrix mode=all`; Firefox 9 cases `hw_log=True errors=0`, Chrome 8 cases `VaapiVideoDecoder platform=True dropped=0`, live YouTube/Bilibili and noav1 control included; FFmpeg framemd5 gates exact | do not claim Firefox AV1 hardware support; keep the current README/capability wording |
| A11 | New regression corpus: tiles, superres, lossless, 10-bit, lookahead, 4K, dav1d/software pixel-exact, borrowed as *case design* from upstream `tests/decoder/av1.py` | host + target | A5 | **open**: every case needs an independent software baseline that the VA driver did not generate; framemd5/raw-hash ordered comparison; no self-generated oracle. A `generate.sh` draft exists under `~/Lab/Bridge/tmp/av1-syntax-corpus` but was not executed: the target has no `aomenc` (only `libaom-av1`/`libsvtav1`), the host `aomenc` aborts with a glibc buffer overflow, and the run would add another thermal load | drop the cases that cannot get an independent oracle |
| A12 | Re-run all previously recorded evidence on the new baseline | target | A7-A11 | **blocked by A11**: the 2026-11-11 installed-package gates re-ran 48/48 matrix, 120/120 real clip, DRC, 4K/10-bit/altref/tiles browser cases, live YouTube/Bilibili and the Firefox matrix with the §1.8 evidence fields; the independent-oracle syntax corpus (A11) has not run, so A12 is not complete | abandon the rewrite and keep `main` |
| A13 | Documentation and capability sync, then a merge decision | docs | A12 | **in progress**: capability table, both READMEs, `docs/architecture.md` §8/§10 and the experiment record describe the default internal-DPB path; installed-package gate evidence is recorded in `TEST-RESULTS.md`; the merge decision waits on A12 | revert all A items |
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
- The earlier low-model attempt at a dynamic 8-slot DPB produced 33/120 frames
  on a real clip. It was a half-coupled build (0005 not loaded, `detached`
  never true, UBWC modifier and scanout claims mixed in), so it does not
  disprove this route, but it does prove that partial implementation fails.

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
| C3 | Kernel patch 0006 (`7c28b27`, unmatched timestamp metadata ring) — decide whether it is needed at all on SM8550 | build/load only if C1 shows a spurious LAST/EPIPE; otherwise record "not needed" and leave 0006 experimental |
| C4 | Target run: one-buffer vs two-buffer window; known bad case 153/609 misplaced must stay a regression test | FFmpeg 515/515 clean, Chrome no drops, `misplaced=0`, exact framemd5 |
| C5 | Only after C4: qualify VP9 `publish_import` in the table, both READMEs, §8 trace vocabulary, TEST-RESULTS.md and CI | evidence-before-capability satisfied; otherwise Copy stays |

## 6. Workstream D — AV1 feature completeness

| ID | Item | Recorded boundary | Pass condition |
| --- | --- | --- | --- |
| D1 | 1080p 10-bit AV1 | 1920x1080 60-frame `yuv420p10le` clip passed on 2026-11-11: `EXACT` framemd5, 60 Import, 0 errors/drops/misplaced on the installed package; depth is decided before the first AU | done for exact framemd5; a 10-bit browser pixel comparison against software output is still open |
| D2 | Film grain | a scratch build wrote a spec-correct `film_grain_params()` OBU (libdav1d decoded the rebuilt stream) but the hardware run still ended in `getImage: surface has no decoded pixels`: VA-API needs `current_frame` plus a separate `current_display_picture`, and this driver publishes one target per completion. `-export_side_data film_grain` decodes all 24 frames in hardware | keep the fail-closed rejection with the two-surface reason, the host guard and the capability table `rejected` entry; a kernel patch does not remove the VA surface contract |
| D3 | 128x128 superblock / loop-restoration unit shift | one-bit header bug fixed; GStreamer `vaav1dec` still refuses the codec | retest on merged baseline with a native control; browser evidence required for any claim |
| D4 | Tiles, superres, lossless, still picture, global motion, lookahead, 4K 10-bit | upstream `tests/decoder/av1.py` covers these on other SoCs; our gate covers 720p, DRC and 30-frame 4K 8-bit | independent software oracle per case (A11), exact pixel hashes, no new `publish=error/drop/misplaced` |
| D5 | AV1 12-bit and large-scale tile | in the capability table `rejected` | keep rejected until a hardware gate exists; do not infer from format enumeration |
| D6 | `show_existing_frame` / hidden alt-ref ordering | §17/18 invariants | explicit negative tests for a hidden picture landing on a shown surface and vice versa |

## 7. Workstream E — long-window, power and cost evidence

| ID | Item | Why open | Pass condition |
| --- | --- | --- | --- |
| E1 | Two-hour concurrency soak (`dual-h264` and `mixed`, 172800 frames per context) | explicitly deferred on 2026-11-10: `Discharging`, 26 % at -2.195 A at session start, 23 % at end; the short-window browser acceptance section is marked blocked | L1 satisfied, guard wrapped, boot ID unchanged, no reset, no thermal breach, per-context ledger balances |
| E2 | `test/iris-power-sampler.sh` hardware/software A/B | script refuses `Charging|Full`; all recent runs were discharging short windows | charged pack then unplugged, fixed brightness/refresh/network/clip, voltage x current samples, explicit "not comparable" note for any differing input |
| E3 | 4K continuous decode thermal and power | the guarded 4K probe is 30 frames / 30 s and explicitly "not a thermal, power or long-window result" | longer run with guard, temperature ceiling, battery state, no reset |
| E4 | Per-frame 4K cost | 2026-10-25 finding: growth is DVFS, not accumulation; big-cluster clock rises when unpaced | pin core and frequency before quoting a per-frame number; record the clock state with the measurement |
| E5 | Make the guard pack-aware | L3 | a self-test showing a low/near-empty pack is refused or explicitly recorded, without weakening boot/thermal guards |

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
| F10 | 0005/0006 experiment branch | no such branch exists; only `feat/vpu-zerocopy-ubwc` and its fork remote | created from `main` when A3 is authorised; never enabled by default |

## 9. Workstream G — documentation, consistency and hygiene debt

All of these are small, host-only and independently mergeable. None changes
runtime behaviour.

| ID | Item | Exact location | Fix |
| --- | --- | --- | --- |
| G1 | Stale claim about patch diffs | `TEST-RESULTS.md` §"Sibling backends" says "patch bodies were not diffed" | replace with the F3 result: 0004/0005 blobs identical, 0001-0003 rebase-only differences, added-code comparison functionally equivalent |
| G2 | Stale claim about Chrome 153 | `TEST-RESULTS.md` says "Both READMEs now say 153 needs the flag or 152, 156+ needs neither"; neither `README.md` nor `README.zh-CN.md` mentions 153, `ExtendedVideoBitstreamValidation` or 156 | either restore the compatibility note in both READMEs or correct the TEST-RESULTS sentence; one source of truth per fact |
| G3 | Browser window wording | `README.md:20` and `README.zh-CN.md:20` say a 30-second zero-drop window; the recorded live-page windows are 10 s, local acceptance 12 s, and §1.6 requires media-surface decoder plus frame/drop evidence | align the wording with the recorded windows and the AGENTS rule, or run a genuine 30 s window and record it |
| G4 | Stale source paths | `docs/github-community-assets.md:38-41` names `src/h264.cc`, `src/vp9_stateful.cc`, `src/context.cc`, `src/surface.cc`; none exists | map to the current `src/codec/`, `src/va/`, `src/iris/` layout, or mark the section historical |
| G5 | Stale capability boundary | `docs/github-community-assets.md:55` says "AV1 先走 native V4L2, unless a VA caller keeps OBUs" and keeps VP9/H.264/HEVC as the production boundary | update to the merged table: AV1 Profile 0 Import qualified, VP9 Copy, H.264/HEVC Import, encode H.264/HEVC |
| G6 | Scratch root defaults | 9 tracked scripts still default to `${TMPDIR:-/tmp}`: `scripts/iris-experiment-guard.sh`, `test/iris-alloc-chain.sh`, `test/iris-deploy-path-check.sh`, `test/iris-eos-check-self-test.sh`, `test/iris-experiment-guard-self-test.sh`, `test/iris-install-manifest-check.sh`, `test/iris-node-check.sh`, `test/iris-setup-path-check.sh`, `test/provenance-self-test.sh`; `test/README.md:166-169` documents and uses `/tmp/iris-hardware-suite` | switch defaults to `$HOME/Lab/Bridge/tmp`, update the doc example, keep the env overrides, add a self-test that the default path is not `/tmp` |
| G7 | Trace vocabulary drift | `docs/architecture.md` §8 has no checker; AGENTS.md calls this out | add a host check that every `trace(` format string in `src/` appears in §8, or explicitly list the accepted exceptions |
| G8 | License wording | `COPYING` says the project is released under LGPL-2.1 and MIT; all 54 SPDX-tagged source files are MIT, LGPL appears only in `COPYING.LGPL`, the Fedora spec and the Arch PKGBUILD; the spec's `MIT AND LGPL-2.1-or-later` is conservative | clarify `COPYING` and the spec comment as "derived work MIT; upstream LGPL text retained"; do not delete `COPYING.LGPL`; confirm the `/usr/share/licenses` install list still covers all three files |
| G9 | Kernel patch licence headers | the six files in `packaging/kernel/` carry no SPDX header; they patch GPL-2.0 kernel files and are not compiled into the driver package | decide whether to add a header; confirm packaging licence fields need no change |
| G10 | Merge commit identity | PR #2 merge commit author is `xinyang`, the local git user is `phxinyang` | note for release notes / author statistics; no code action |
| G11 | `FIX-LOG.md` 待办 section | the checklist is all `[x]` and predates the merged work; no open items | either close the section as historical or point it at this backlog |
| G12 | 4K lane discoverability | `IRIS_REAL_CLIP_SET=4k` is opt-in and not part of `all`; `test/README.md` records it | keep as-is, but state in the hardware-suite doc that a 4K lane is never a soak/power result |
| G13 | CI required-test list | `.github/workflows/ci.yml:42-59` names 18 required tests; new tests must be added there | add every new test name in the same commit that adds the test |

## 10. Workstream H — encode backlog

Lower priority than decode. Ground each item in the capability table's
`rejected` list rather than in an assumption about the firmware.

| ID | Item | Pass condition |
| --- | --- | --- |
| H1 | Investigate the recorded rate-control gap against the piano comparison, with a fresh A/B on the merged baseline | a measured, reproducible statement of what differs; no capability change without a gate. `TEST-RESULTS.md` now carries a `strace`-backed A/B protocol; the unisolated observation is VBR 4M -> 9780 kbit/s (~2.4x) while `encode.cc` writes `BITRATE_PEAK` only for CBR. CBR already follows the request, so no code change is warranted before the A/B produces a verdict |
| H2 | Packed sequence headers / extradata for H.264 and HEVC encode | upstream has an open issue on this; decide whether it is in scope for this driver at all before coding. Settled: do not do it here — `VA_ENC_PACKED_HEADER_NONE` is what opens the GStreamer path, and `picture.cc` silently drops packed header buffers, so advertising support would desynchronise MP4 extradata and reopen the double-claim `size 0` crash |
| H3 | Recheck each `rejected` entry (B-frames, packed headers, multi-slice, tiles/WPP, weighted prediction, scaling lists, chroma QP offsets, multiple reference indexes, 10-bit/non-4:2:0, HEVC Main10 encode) on the current firmware | either a hardware gate with tests, or keep the explicit `VA_STATUS_ERROR_UNIMPLEMENTED` and the table entry. A host-side audit found code-level `require` backing for every entry; the only drift is the conditional deblocking item, which no longer fires on a kernel with 0002 installed. Firmware-level availability still needs a hardware gate |
| H4 | Keep the double-claim and `REQUEST_SYNC_FRAME` regressions covered | existing host/target gates stay green after any encode change. Gap recorded: `claim_coded()` has no host unit test, and GStreamer `vah264enc`/`vah265enc` has no scripted gate; the only evidence is one manual target run in `TEST-RESULTS.md`. Adding it needs its own target run, not this decode delivery |

## 11. Workstream I — CI, tests and release plumbing

| ID | Item | Pass condition |
| --- | --- | --- |
| I1 | Cross-repo fork PR CI still needs base-repo admin approval for `pull_request` runs; fork `push` runs are green | record the actual approval state per run; never describe a pending PR run as passing |
| I2 | Any new invariant gets a regression test that fails on the old code, plus a name in `.github/workflows/ci.yml` required list (G13) | red-on-old, green-on-new, CI inventory check passes |
| I3 | `test/iris-clip-trace-check.sh` compares ordered `^0,` framemd5 lines because host and target `Lavf` versions differ | keep the header-only-difference self-test; do not reintroduce a whole-file `cmp` |
| I4 | `test/run-static-checks.sh` is the single CI entry point for host-only checks; keep hardware suites out of it | unchanged on merge |
| I5 | Provenance: source commit, tracked-source SHA-256, dirty flag, artifact hash; CI must stay clean (`source_dirty=0`) | CI "Generate and verify provenance" step passes |
| I6 | `ldd -r` rejects undefined symbols in `v4l2_drv_video.so` | unchanged |
| I7 | Distribution layers are reported separately: source, build, staged `DESTDIR`, installed package, target runtime, registry/release | never say "installed" or "released" from a source or CI check alone |

## 12. Priority, prerequisites and cost

| Priority | Items | Prerequisite | Cost | Risk if wrong |
| --- | --- | --- | --- | --- |
| P0 | L1-L6, G1-G13, I1-I7 | none (host-only / lab setup) | low | low; documentation drift and an unguarded lab run |
| P1 | E1-E5 (soak, power, 4K cost) | L1, L2 | medium, needs a charged pack | a power claim that cannot be attributed to decode |
| P1 | A1, A2 (contract diff and kernel observation) | none for A1; L1/L5/guard for A2 | medium | **done**; no 0005 module swap |
| P2 | A11-A12 (independent-oracle syntax corpus, full evidence re-run) | A2, L1, guard | medium: corpus generation needs an encoder that can express superres/lossless/still-picture/global-motion/tiles with an independent software oracle; A5-A10 are done on the installed package | advertising an unqualified profile, or calling A12 done while A11 has not run |
| P2 | C1-C5 (Chrome VP9 Import) | L1 | medium-high | misplaced completions, silent drops |
| P2 | D1-D6 (AV1 feature coverage) | independent software oracles | medium | advertising an unqualified profile |
| P3 | B1-B8 (upstream API change) | upstream acceptance | high, mostly outside this repo | indefinite wait; no local fallback |
| P3 | F2 (kernel rebase) | a named defect fixed upstream | high | full re-verification for no measured gain |
| P3 | H1-H4 (encode) | L1 | medium | low, decode is the product goal |

## 13. Explicit non-goals

- Do not merge this backlog document into `main` while it is a draft; it is a
  tracking PR.
- Do not rebase the whole `src/` tree onto `radxa-pkg/qcom-va-driver`; the two
  backends have different memory models and different target SoCs.
- Do not load 0005 or 0006 against the current userspace outside a guarded
  experiment branch.
- Do not delete `COPYING.LGPL`, the vendored UAPI headers, or the experimental
  kernel patches as "cleanup".
- Do not add a browser launcher, a desktop entry, a Chrome feature override, or
  a fixed video-node literal; `test/run-static-checks.sh` forbids all four.
- Do not promote a short clip gate, a `DESTDIR` staging run, a mapped node, or a
  running GPU process into a capability or browser-hardware claim.
- Do not add AI attribution to any commit, PR body, issue or public reply.
- Do not perform a `git clean`, destructive reset, branch switch, or push
  without an explicit request in the same turn.

## 14. Definition of done for this consolidated PR

The work converges in PR #4. It is done when the default internal-DPB
translator passes the host gates, the installed-package matrix/clip/DRC/EOS
gates record every §1.8 field in `TEST-RESULTS.md`, the browser runs report a
media-surface decoder with frame/drop evidence, and the capability table, both
READMEs, `docs/architecture.md` §8/§10 and the required-test list are updated
in the same commit. Unfinished items stay open in §15; a short clip, a
`DESTDIR` staging run, a mapped node or a running GPU process is not a
capability or browser-hardware claim.

## 15. Completion ledger

| Workstream | Items | Status |
| --- | --- | --- |
| §2 Lab prerequisites | L1-L6 | L4 settled as "pass an explicit `IRIS_EXPERIMENT_DIR` under `~/Lab/Bridge/tmp` for every guarded run" (all recorded runs do); the default is left alone so existing `guard.state` semantics do not drift. L1 charger, L2 screen state, L3 guard precondition remain open; L5/L6 are per-run checks |
| §3 AV1 held-picture removal | A1-A14 | done: internal DPB is the default, installed-package matrix/clip/DRC/EOS gates and Firefox/Chrome live-site browser gates pass on 2026-11-11; `TEST-RESULTS.md` records the run. Long corpus and 4K cost work remains in §6/§7 |
| §4 Explicit refresh flags | B1-B8 | open / upstream-blocked; internal DPB removes the local need for an API change |
| §5 Chrome VP9 Import | C1-C5 | open: session-side window is fixed (FFmpeg 515/515, misplaced=0); Chrome reuses a VP9 surface in `beginPicture`, which releases the still-in-flight picture, so 541 of 718 completions are dropped. The Copy baseline shows the same reuse (527 drops), and the project's acceptance rule refuses to count a re-presented frame as a pass. Qualifying this needs a retained-target ownership model, its own host regressions for the misplaced/detached case, and a fresh target gate — not a rename of `publish=drop` |
| §6 AV1 feature completeness | D1-D6 | D1 done for exact framemd5: 1920x1080 60-frame 10-bit clip `EXACT`, 60 Import, 0 errors/drops/misplaced on 2026-11-11. D2 settled as a user-space second-target publish gap, not a kernel limitation: scratch OBU writer was libdav1d-clean and the firmware carries the grain capability flag, but the display surface stayed empty; host guard keeps the refusal fail-closed. Still open: 10-bit browser pixel comparison, 12-bit, large-scale tile, D3-D6 corpus work |
| §7 Long-window, power, cost | E1-E5 | open / blocked on charger |
| §8 Kernel and upstream sync | F1-F10 | open / decision recorded as "do not rebase"; 0005/0006 remain unbuilt and unloaded |
| §9 Docs, consistency, hygiene | G1-G13 | open: only the AV1/default-path drift tied to PR #4 plus this branch's film-grain/10-bit evidence is fixed here |
| §10 Encode | H1-H4 | H1/H2/H3/H4 re-examined and recorded with evidence and explicit non-goals; no encode code, capability entry or profile changed in this branch. H1 has a reproducible A/B protocol, H2 is settled as out of scope, H3 has a host-side audit, H4 has a named test-coverage gap |
| §11 CI, tests, release plumbing | I1-I7 | open: host tests (25/25), guarded installed-package gates and the repo-owned 17-case AV1 browser matrix are in PR #4; packaging/CI automation for those target gates remains open |
| §12 Priority | ranking | updated for the settled 0005 question |
| §13 Non-goals | list | recorded |
| §14 Done criteria | check | PR #4 is the single delivery PR; installed-package and browser gates pass. Remaining items are the explicitly open workstreams above, not blockers for this PR |
