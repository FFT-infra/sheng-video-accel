# Experiment: AV1 internal DPB slot translation (host spike, 2026-11-10)

`<scratch>` stands for the operator's scratch directory and `<lab-root>` for
the deployment tree; evidence keeps its relative layout without publishing
absolute paths.

Branch: `exp/av1-noshow-dpb`, from `origin/main` `65686a2`.
Status: **shipped as the default in PR #4 (consolidated into PR #5)**. The host prototype, guarded
native V4L2 completion probe and guarded real VA-API sessions below
established bit-exact rebuilt OBUs, the real two-completion install unit and
the false-accounting fix. The held-picture wait chain is removed from
`src/va/`; `Target::invisible` attributes both NOSHOW halves of an install
unit, and a NOSHOW LAST marker at a DRC boundary is invisible rather than a
decode error. On 2026-11-11 the installed package passed the guarded
four-codec Import matrix, the 120-frame AV1 clip and the in-context DRC clip
with exact framemd5; Firefox 158 local AV1 and live YouTube/Bilibili AV1,
plus Chrome 156 live pages, report the selected hardware decoder with
frame/drop windows. `TEST-RESULTS.md` records the source commit, driver hash,
node, boot ID and counts. Chromium 152 still selects a software AV1 decoder
for a local file, so no per-client claim is made for it. No kernel module was
changed or loaded during the spike; 0005 is not needed for completion
accounting.

## Question

Firefox 158 syncs each surface from its decoder thread. Our AV1 translator holds
one picture until the next access unit names the DPB slots it refreshed, so the
next access unit cannot arrive while Firefox waits. Another Iris backend
`src/av1.cpp` allocates its own eight internal DPB slots, emits one complete
temporal unit per VA picture, and when all eight slots are live replays the
previous uncommitted picture as a hidden NOSHOW frame inside the *same* temporal
unit before the current frame.

This spike asks whether that idea can be layered onto our existing
`av1_build_temporal_unit()` by rewriting the VA picture buffer only, without
changing the bit writer. It does not ask whether the hardware accepts it.

## What was built

`<scratch>/av1-noshow-spike/slot-remap.cc`, compiled on the host against
`src/codec/av1_obu.cc` and the system `va/va_dec_av1.h`:

```
/usr/bin/g++ -std=c++20 -Wall -Wextra -O0 -g -I src/codec -I src -I include \
  slot-remap.cc ../src/codec/av1_obu.cc -o slot-remap
```

It maintains an eight-slot DPB with our own slot numbers, rewrites
`ref_frame_idx[]` and `ref_frame_map[]` to those slot numbers before calling
`av1_build_temporal_unit()`, keeps `AV1ReferenceState` indexed by the same
numbers, and drives nine pictures (one key frame, eight inter frames).

## Findings

1. The rewrite is mechanically possible. `av1_obu.cc:731` codes
   `picture.ref_frame_idx[i]` verbatim, and `av1_obu.cc:563`, `:830`, `:856`
   index `AV1ReferenceState` with the same values. Rewriting the VA buffer and
   the reference state together makes the existing bit writer emit our internal
   slot numbers without a change to its code. GM state and order hints must be
   updated with the *remapped* picture, not the original.

2. A full DPB is real and needs the hidden replay. `free_slot()` returns
   `1, 2, 3, 4, 5, 6, 7` for frames 2-8 and `-1` for frame 9. That backend resolves
   this only when the *current* picture's reference map stops naming a slot,
   which frees exactly one slot for the hidden install frame. The spike
   confirms that condition: dropping one reference on frame 9 makes
   `free_slot()` return `1` again. The replay is therefore conditional on the
   next picture's reference map, not on a timer or a guess.

3. **The existing writer cannot produce the required temporal unit.**
   `av1_obu.cc:914-920` emits a temporal delimiter, an optional sequence
   header, and exactly one `OBU_FRAME` per call. That backend instead emits one TD,
   an optional sequence header, and *two* `OBU_FRAME`s (hidden install +
   current picture) inside one OUTPUT buffer. Concatenating two calls produces
   two temporal units, not one; the spike's stream was rejected by libdav1d
   (`Error submitting packet to decoder: Invalid data found when processing
   input`, `Decode error rate 1 exceeds maximum`), and its fake tile payloads
   were not valid AV1 tile data either. `src/codec/av1_obu.cc` needs a new
   entry point that builds a multi-frame temporal unit, or the caller must be
   able to append a second frame OBU to an existing unit.

4. The hidden install frame has no client surface. `src/va/picture.cc:71-92`
   submits one AU for one `Surface`, and `src/iris/session.cc:403-453`
   registers exactly one `Target` per submit with one token and one Import
   buffer. A second frame in the same OUTPUT buffer produces a second CAPTURE
   completion with no matching `Target`. `session.cc:573-605` already consumes
   NOSHOW completions by decode order for hidden *client* pictures, but an
   internally generated frame has no target at all. Either the session learns
   to expect an internal completion, or kernel patch 0005 recycles that
   completion inside the kernel (`packaging/kernel/0005-*.patch` description:
   "Recycle successful NOSHOW CAPTURE buffers within the driver instead of
   completing them through vb2"). At this point 0005 looked like a real
   prerequisite; the later session tests and the guarded target probe below
   corrected that: the userspace `completions = 2` accounting already consumes
   this completion, and the 0001-0004 firmware does return it. This paragraph
   records the state of the spike before that evidence existed.

5. A real AV1 OBU stream from `aomenc` decodes cleanly with libdav1d on this
   host, so the oracle is available: `ffmpeg -f obu -c:v libdav1d ...` produced
   a frame hash for a 64x64 key frame.

6. **One temporal unit with two `OBU_FRAME`s is legal AV1.** A 20-frame
   lookahead stream (`-lag-in-frames 8 -arnr-max-frames 0`) produced TU 9 with
   OBU types `[2, 6, 6]`: one temporal delimiter followed by two frame OBUs.
   libdav1d decoded all 20 frames from that stream. That backend's container shape is
   therefore sound; our earlier failure was not the TU layout but the hand-
   edited frame header. Clearing the `show_frame` bit in an existing key-frame
   header is invalid: `show_frame=0` inserts a `showable_frame` bit and changes
   the key-frame error-resilient syntax, so every following header field shifts.
   ffmpeg's `av1_frame_merge` rejected it with `zero_bit out of range: 1`. A
   hidden install frame must be *built* from the parsed parameters with
   `show=0` (another backend's `frame(... bool show ...)`), not patched into a shown frame.
   The earlier `slot-remap.cc` stream was rejected by libdav1d for the same
   reason: concatenated per-call temporal units plus fake tile payloads, not a
   single valid multi-frame TU.

## Consequence for the implementation

The host-only path is not a "rewrite the VA parameters" patch. It needs at
least:

- `src/codec/av1_obu.cc`: a temporal-unit builder that can append a hidden
  frame OBU before the shown frame OBU, sharing one TD/sequence header.
- `src/codec/av1.cc`: the eight-slot DPB, `free_slot()`, `remember()`, and the
  pending uncommitted reference, replacing the one-picture hold.
- `src/iris/session.cc` and `src/va/picture.cc`: a way to account for a
  completion that has no client surface, or a kernel-side recycle via 0005.
- `src/va/surface.cc`: removal of `wait_for_release()` and the bounded-sync
  tail path once no picture is held.
- Import invariant re-derivation: one client buffer in flight, no skipped
  picture, misplaced completions fail both surfaces.

## Host implementation landed (2026-11-10, commit `d23b450`)

`src/codec/av1_obu.cc` now exposes `av1_build_frame_obu()` and
`av1_build_install_temporal_unit()`. The latter builds one temporal delimiter,
one sequence header, and two `OBU_FRAME`s: a hidden frame built from its own
VA parameters with `show_frame=0`, followed by the shown frame. The original
`av1_build_temporal_unit()` is a thin wrapper over the same frame helper, so
the shipped single-frame path is byte-identical; the new C++ test asserts
byte parity between the two builders.

Host gates:

- `test/av1-install-tu-test.cc` (meson `av1-install-tu`): OBU structure, the
  two `show_frame` bits, and single-frame byte parity.
- `test/av1-install-tu-roundtrip.sh` (invoked by `run-static-checks.sh`):
  decodes `test/fixtures/av1-two-frame-tu.obu` with libdav1d and requires two
  distinct frame hashes. The fixture is a 3030-byte `testsrc2` clip encoded
  with libaom-av1 (TD + SEQ + two real key-frame FRAME OBUs); it is checked in
  so the test never generates its own oracle.
- `sh test/run-static-checks.sh`, `meson test -C build-local --print-errorlogs`
  23/23, `git diff --check`: all green. The new meson test is in CI's required
  list.

An earlier prototype tried to patch `show_frame` on an existing key-frame
header and failed libdav1d with `zero_bit out of range: 1`. Passing the real
frame OBU payload as tile bytes also decoded only one frame, because the tile
group then contained a nested frame header. Both are the reason the new entry
point *builds* the hidden frame from VA parameters instead of editing a shown
one.

## Experimental DPB translator (2026-11-10, commit `45a36b9`)

At this commit, `src/codec/av1_dpb.cc` / `.h` added `make_av1_dpb()` as a
separate translator that was **not** selected by `src/va/picture.cc`
(`make_translator()` still returned the qualified held-picture translator).
The branch later selects the DPB translator; see the status at the top.

What it does:

- Every `vaEndPicture` emits a complete temporal unit immediately; no picture
  is held, `flush()` has no tail, and `held_surface()` is always invalid.
- An internal eight-slot DPB maps VA surface IDs to slot numbers.
  `ref_frame_idx[]` is rewritten to those slot numbers before calling the OBU
  writer, and `AV1ReferenceState` is kept in the same numbering.
- `free_slot()` is derived from the *current* picture's `ref_frame_map`. When
  no slot is free, the picture refreshes nothing and is kept as the pending
  uncommitted reference. When a later picture still references that surface
  and has released a slot, `av1_build_install_temporal_unit()` replays the
  pending picture as a hidden frame into that slot, followed by the shown
  frame, in one temporal unit. The AU sets `completions = 2`, reusing the
  session's existing VP9 alt-ref accounting (`src/iris/session.cc:302-387`)
  so the same client buffer is lent twice. **This is the userspace path that
  may remove the need for kernel 0005**; it has not been tested on hardware.

`test/av1-dpb-test.cc` (meson `av1-dpb`) drives a key frame, seven inter
frames that fill the DPB, a full-DPB frame that becomes pending, and a tenth
frame that releases one slot. It asserts no `deferred`, two `completions`, and
two FRAME OBUs in the install unit. Host gates: `sh test/run-static-checks.sh`
rc=0, `meson test -C build-local --print-errorlogs` 24/24,
`git diff --check` rc=0; the new test is in CI's required list.

Three real bugs were found and fixed while making that test pass; they are
worth keeping in mind before this prototype is promoted:

- Intra frames code no reference indices, so resolving `ref_frame_idx[]` on a
  key frame rejected valid input.
- Retiring an old surface and assigning the refreshed slot in one pass cleared
  the slot that had just been written, leaving the DPB oscillating between two
  slots. `remember()` now retires first, then assigns.
- `need_install` originally required `free_slot < 0`, which is backwards: the
  released slot is exactly where the pending picture gets installed.

Known gaps in the prototype, not hidden by the passing test:

- The test constructs plausible VA parameter structs, not pixels from a real
  encoder. There is no libdav1d pixel oracle for the DPB path yet; the only
  pixel oracle is the separate two-frame fixture in `av1-install-tu-roundtrip.sh`.
- `out.hidden` is taken from the shown frame only. The internal hidden frame's
  completion is not represented as a separate target.
- The prototype does not handle `show_frame=0` client pictures, resolution
  changes or EOS through the internal DPB.

The `remember()` order-hint issue and the install-order bugs listed here are
corrected by the "DPB semantics audit" section below; this paragraph records
the state of the prototype at commit `45a36b9`.

## NOSHOW completion accounting (2026-11-10, commit `756269a`)

Two host cases were added to `test/unit/session-test.cc` using the existing
`FakeDevice::inject_altref_superframe()` shape — one token, first completion
`V4L2_BUF_FLAG_ERROR` with no picture-type bit and no payload, second with
pixels. That is the same shape an internal hidden install frame would produce:

- `install hidden frame needs no extra target`: with `completions = 2`, the
  shown frame completes, and `errors`, `drops` and `misplaced` all stay zero.
  The first NOSHOW completion is consumed by the invisible-completion path
  (`src/iris/session.cc:626-634`) without a second `Target`.
- `import install hidden frame lends one buffer twice`: the Import window
  queues exactly two CAPTURE slots, both carrying the same client fd, and both
  return. No slot from the next picture is queued alongside them.

`sh test/run-static-checks.sh` rc=0, `meson test -C build-local
--print-errorlogs` 24/24, `git diff --check` rc=0.

**Consequence:** kernel 0005 is not required *by this accounting*. The session
already handles a two-completion access unit whose first completion has no
client surface. What remains unverified is whether the Iris firmware on the
0001-0004 module actually returns that NOSHOW completion with this exact shape
(flags, timestamp, payload) — that is a target probe, not a host test. 0005
would still be needed if the firmware recycles the buffer internally and
returns only one completion, or if its NOSHOW completion carries no usable
timestamp and would be counted as an error.

## Real-clip oracle (2026-11-10, commit `defa6a8`)

`test/av1-dpb-oracle.sh` (invoked by `run-static-checks.sh`) generates a
20-frame `testsrc2` clip with libaom-av1 lookahead, locates the temporal unit
that contains two `OBU_FRAME`s, and asserts the first frame has `show_frame=0`
and the second `show_frame=1`. libdav1d decodes the whole source into 20
distinct frame hashes. This is an independent oracle: the VA driver never
generated the clip.

A single temporal unit from the middle of an inter-frame stream cannot be
decoded standalone (its frame header depends on earlier reference state), so
the oracle decodes the whole stream rather than the isolated unit. That also
means the DPB translator itself still lacks a pixel-level oracle; the test
proves the container shape is real AV1, not that this translator emits decodable
pixels for arbitrary VA parameters.

## DPB semantics audit against another backend (2026-11-10)

Re-reading that backend's AV1 translator (`free_slot()`, `remember()`,
`reference_slots()`, `av1_bitstream()`) against the prototype exposed a class of
bugs the original `av1-dpb` test could not see: it set every `ref_frame_idx[i]`
to 0, so no reference ever named a surface outside slot 0, and it never
inspected the generated frame headers. A new test drives `ref_frame_idx[i] = 7`
against the pending surface and failed with
`AV1 reference is outside the internal DPB` before any fix.

What was wrong, and what changed in `src/codec/av1_dpb.cc`:

- **Install order.** The hidden install frame has to be registered in the DPB
  before the shown frame's `ref_frame_idx[]` is resolved, otherwise a successor
  that names the pending surface has no slot to point at. That backend does
  `frame(pending, ...)` → `remember(pending)` → `frame(current, ...)`. The
  prototype resolved all references first and called `remember()` only after
  the whole temporal unit was built.
- **One slot refreshed twice.** Hidden and shown frames both used
  `1 << install_slot`; the second `remember()` retired the first, so the
  install disappeared. The shown frame now recomputes `free_slot()` after the
  install and refreshes a different slot (or becomes the new pending picture).
  A bit-level test now decodes both frame headers and asserts the two
  `refresh_frame_flags` differ.
- **Hidden key frames.** Forcing `frame_type = INTER_FRAME` on a pending key
  frame made the writer emit inter-frame syntax for intra tiles. A hidden key
  is now written as `INTRA_ONLY_FRAME`, matching that backend's
  `frame(... show=false, reset=false)`.
- **Reference state.** `remember()` was fed the *current* picture's
  `order_hint`, and `AV1ReferenceState` / `gm_params` were updated from the
  remapped copy rather than the frame that actually refreshed the slot. The
  hidden install frame now uses a snapshot of the pre-install reference state,
  and each `remember()` / `av1_update_reference_gm()` call gets its own picture.
- **Dead write.** `remapped.ref_frame_map[s] = slots_[s].surface` wrote surface
  IDs into a field `av1_obu.cc` never reads; the bitstream codes the eight DPB
  slot numbers directly. It is gone. `ref_frame_idx[]` is now the only field
  remapped, resolved against `slots_[]` the same way that backend's
  `reference_slots()` does.
- **Sequence restart.** A new sequence now resets `references_`, `slots_[]`
  and the pending picture instead of translating the first frame against a DPB
  from the previous sequence.
- `out.hidden` is still taken from the shown frame only. For an install unit
  the shown frame is `show_frame=1`, so the session's `completions = 2`
  invisible path is selected; the internal hidden frame has no `Target`.

Host gates after the fix: `sh test/run-static-checks.sh` rc=0,
`meson test -C build-local --print-errorlogs` 24/24, `git diff --check` rc=0.
At this commit this was still a host-only correctness fix: it did not select
the translator, did not touch `src/va/`, and added no hardware evidence.

## Target completion probe (2026-11-10, unshipped scratch tool)

The read-only guarded probe described in the previous section was run on
`sheng` with the installed 0001-0004 module. The tool is a standalone native
V4L2 program in `<scratch>/av1-noshow-spike/`; it is **not** in the
repository, loads no VA driver, changes no kernel module and uses no build-tree
override. The decoder was resolved at runtime by `iris_driver` QUERYCAP
(`scripts/lib/iris-node.sh`), with no fixed node number in the launcher.

Input: a 320x180, 20-temporal-unit `testsrc2`/libaom-av1 lookahead OBU stream
(SHA-256 `f2535e34…`). TU 9 contains two `OBU_FRAME`s, first `show_frame=0`
and second `show_frame=1`; libdav1d decodes the whole stream to 20 frames on
the host. Each TU was queued as one OUTPUT buffer with `timestamp = TU+1`, so
token 0 remains reserved for "the firmware gave this completion no timestamp".

Evidence (all artifacts under `<scratch>/av1-noshow-probe/`):

- Source commit `2e04d77585d2faf1ba3230dd8d261f21ed1735e0`, boot ID
  `7a26df99-f7ac-4148-9c80-135ba3adc661` (unchanged after the run), kernel
  `7.2.6-sm8550-g42f3b40-iris1`, resolved node `/dev/video17`, decoder
  `Iris Decoder` / `iris_driver`, installed driver SHA-256 `278abdfe…`
  (recorded but **not loaded**), module `qcom_iris`, patch set 0001-0004,
  0005/0006 not built or loaded.
- Guard: `scripts/iris-experiment-guard.sh -- …`, started at 65.5 C hottest
  zone, limit 85000 mC, timeout 120 s, `status=complete`, finished in 6 s,
  no thermal breach and no boot-ID change.
- 22 CAPTURE completions for 20 TUs: 20 shown frames, one hidden-frame
  completion, one empty `LAST` at drain. Every shown frame carries its own
  token and 110592 bytes.
- **TU 9 (token=10) produced exactly two completions with the same token:**
  `flags=0x4041 ts=10 bytes=0` (`ERROR`, no picture-type bit, no payload),
  then `flags=0x4011 ts=10 bytes=110592` (normal picture). The drain `LAST`
  is `flags=0x104001 ts=0 bytes=0`.

This is the answer the branch was missing. The 0001-0004 firmware does return
the hidden frame's NOSHOW completion to userspace, and it carries the OUTPUT
token in its timestamp rather than zero. That is the VP9 alt-ref shape the
`completions = 2` session path (`src/iris/session.cc:626-634`) consumes: the
first completion is `noshow_shape && !target.hidden`, so the target stays
pending and the second completion publishes it. `make_av1_dpb()` sets
`out.hidden` from the *shown* frame, which is `show_frame=1` for an install
unit, so it takes that path correctly. **Kernel 0005 is not needed for this
completion accounting**; loading it would instead recycle the buffer inside
the kernel and, as `packaging/kernel/README.md` records, lose the frame with
the current userspace.

The zero-timestamp NOSHOW completion in `TEST-RESULTS.md` (2026-10-22) is a
different case: a single `show_frame=0` client picture, whose completion is
attributed by decode order to the oldest `Target::hidden`
(`src/iris/session.cc:588-605`). Both paths exist on this module; this probe
exercised the two-FRAME one.

Boundaries of this evidence: it is a native MMAP CAPTURE probe, not a VA
session, so there are no VA frame/drop counts and **the Import invariant has
not been re-derived on the target** at the time of this probe. It proves the
completion shape, not the installed-package or browser gate. A reset or a thermal breach would
have made this a failed experiment; neither happened.

## Real VA session on the target (2026-11-10, build-tree override only)

The DPB translator was finally run through a real VA-API client (FFmpeg
`vaapi` hwaccel) on `sheng`, using a **clean full-source sync** of commit
`6191d13` into `<scratch>/av1-dpb-target/`. The old
`<lab-root>/src` tree was not used: its `src/codec/av1.cc` turned
out to be a different, older internal-DPB implementation that expects kernel
0005, so a mixed tree would have invalidated every result. At the time of this
run a local-only patch to `src/va/picture.cc` selected `make_av1_dpb()` and
added the `av1_dpb.h` include; the branch source now selects it directly.
`LIBVA_DRIVERS_PATH` points at a freshly built
`build-dpb/src/v4l2_drv_video.so`; `meson test -C build-dpb` was 24/24 before
the run. Nothing was installed, no module was changed or loaded, no reboot.

For a control, the same clean tree was built a second time with
`make_av1()` selected (`build-baseline/src/v4l2_drv_video.so`, SHA-256
`2a7a6e8a…`, no `av1_dpb.cc` symbols). Both runs used the **same fixed clip**
`<scratch>/av1-dpb-fixed.webm` (SHA-256 `9ecd7d59…`, 48-frame
320x180 `testsrc2`/libaom-av1 with `-lag-in-frames 12`), resolved the decoder
by `iris_driver` QUERYCAP, and ran under
`scripts/iris-experiment-guard.sh`. Both guards returned `status=complete`,
boot ID `7a26df99-f7ac-4148-9c80-135ba3adc661` unchanged, hottest zone
56.6 C at start and 59.9 C after, no reset and no thermal breach. Source
commit, build/installed driver hashes, node, decoder name, module and patch
set are in each run's `provenance.txt`.

| | DPB (`make_av1_dpb`) | baseline (`make_av1`) |
| --- | --- | --- |
| VA submitted | 49 | 49 |
| completed | 47 | 47 |
| errors | **2** | 0 |
| drops | 2 | 2 |
| timeouts | 0 | 0 |
| misplaced | 0 | 0 |
| rebuilt OBU frames decoded by libdav1d | 45 | 45 |
| source frames decoded by libdav1d | 48 | 48 |
| framemd5 diff | 3 source-only frames | 3 source-only frames |

**Positive result.** The rebuilt OBU stream is bit-exact against the software
decode in both runs, with the identical three-frame difference. Those three
are `show_existing_frame` pictures, which VA never routes to the decoder;
`test/av1-obu-roundtrip.sh` documents that this count differs by design. The
DPB translator therefore reconstructs decodable AV1 from real FFmpeg-supplied
VA parameters. The trace also confirms that a real install temporal unit
produces two completions under one token and that the session's
`completions = 2` path consumes the first as `publish=invisible` and publishes
the second (`publish=direct`), on hardware, with no kernel 0005.

**Corrected diagnosis of the two extra errors.** DPB reported two `capture
error index=… flags=0x4041 token=N publish=drop` events (tokens 11 and 25),
each followed by a normal completion for the same token also reported as
`publish=drop`. Re-reading both traces shows the same surface reuse in both
runs: `vaBeginPicture` reuses surface 18 before token 11 completes (t=142 DPB,
t=181 baseline) and surface 34 before token 25 completes (t=157 DPB, t=192
baseline). `c->session->release(s->target)` erases that token in both runs;
baseline reports one drop per token, while DPB's token is an install temporal
unit whose first completion is an invisible NOSHOW ERROR half. The old session
code counted that orphaned invisible half as a decode error even though the
target had merely been released, then dropped its visible half. This is not
a no-hold-specific surface-lifetime defect: the same client recycles surfaces
in the qualified held-picture run. `session.cc` now consumes an orphaned
NOSHOW completion as `publish=invisible` without raising `errors`; the visible
half remains the one drop for the abandoned picture. A host regression
(`test/unit/session-test.cc`, `released install completions are not errors`)
models both halves and asserts `errors=0`, `drops=1`.

**Guarded re-run with the accounting fix (2026-11-10).** Both build-tree
drivers were rebuilt from the same clean source with the fixed `session.cc`
(`build-dpb` SHA-256 `ecd1a5e9…`, `build-baseline` SHA-256 `9907d530…`);
`meson test` was 24/24 in both trees before the runs. Two Direct pairs and one
Import pair used the same fixed clip, same kernel `7.2.6-sm8550-g42f3b40-iris1`,
same module patch set 0001-0004, decoder resolved by `iris_driver` QUERYCAP,
boot ID `7a26df99-f7ac-4148-9c80-135ba3adc661` unchanged. Every guard returned
`status=complete`; hottest zone stayed between 37.1 C and 42.2 C, below the
85000 mC limit, with no reset or thermal breach. The installed driver
`278abdfe…` was untouched; `LIBVA_DRIVERS_PATH` selected a build-tree override.

| | DPB Direct | baseline Direct | DPB Import | baseline Import |
| --- | --- | --- | --- | --- |
| submitted | 49 | 49 | 49 | 49 |
| completed | 47 | 39–47 | 46 | 47 |
| errors | **0** | 0 | **0** | 0 |
| drops | **2** | 2–10 | 3 | 2 |
| misplaced | 0 | 0 | **0** | 0 |
| timeouts | 0 | 0 | 0 | 0 |
| rebuilt OBU frames | 45/48 | 45/48 | not measured | not measured |

DPB Direct was stable at `errors=0 drops=2` across both reruns. Baseline Direct
swung between `drops=2`, `drops=3` and `drops=10`: FFmpeg's null-output path
recycles surfaces at varying times, so the exact drop count is a client timing
artifact, not a stable equality target. Every drop in both paths is a visible
completion whose token `release()` had already erased; no decode error and no
misplaced Import completion remains. The Direct framemd5 oracle again returned
45 rebuilt frames against 48 source frames, with the same three source-only
`show_existing_frame` hashes and no rebuilt-only hashes.

The Import pair re-derived the one-buffer-in-flight rule on hardware: 79 Import
queue events for 49 submitted DPB pictures (two buffers for each of the 30
two-completion units), 30 `publish=invisible` completions, `misplaced=0`,
`errors=0`. A completion that lands in a neighbour's buffer still fails both
surfaces by the existing host tests; no skipped picture was observed. Both runs
published `publish=import` on the visible completions.

These are build-tree override experiments. They justified selecting
`make_av1_dpb()` in the branch source and adding host coverage for the client
hidden-picture, sequence-restart and EOS paths; they do not by themselves
qualify the installed package or a browser client. A `DESTDIR` staging run
would not be an installation either. The remaining gates are listed below.

Artifacts: `<scratch>/av1-dpb-va/` and
`<scratch>/av1-baseline-va/` (guard state, command stdout/stderr
trace, `dump/iris-session-*.bin`, `provenance.txt`, framemd5 files). The Direct
and Import launches reuse these directories, so the files hold the latest pair
of each policy.

## Remaining gate

The host prototype gaps that blocked selection are closed: client
`show_frame=0`, sequence restart and EOS now have host tests, and the
held-picture wait chain is removed. `src/va/picture.cc` selects
`make_av1_dpb()`. The pixel-oracle gap was closed by the real VA session: the
rebuilt OBU stream decodes bit-exact in both DPB and baseline.

Before the installed-package capability claim:

- Re-run the four-codec matrix, 120-frame real AV1 clip, DRC clip and EOS
  checks with the installed driver, recording source commit, driver hash,
  resolved node, boot ID, decoder name, frame/drop counts and guard state.
- Run a browser client that syncs each surface: the FFmpeg null-output path
  recycles surfaces early, so it proves completion accounting and rebuilt
  pixels but not the Firefox 158 send-one-receive-one loop this spike started
  from. A media surface must report the selected decoder with frame and drop
  evidence before any Firefox AV1 hardware claim.

0005 was not built or loaded for this spike. 0006 later had its own WP1
module-only build and one guarded target run on `sheng` (`TEST-RESULTS.md`
§"WP1: 0006 loaded on sheng"); the verdict was "no regression", 0006 stayed
experimental and the qualified module stayed 0001-0004. Those later runs are
not evidence for this spike's AV1 DPB design. A reset or a thermal breach is a
failed experiment per `AGENTS.md`.

## Environment note

`sheng` was re-read before this spike: boot ID
`7a26df99-f7ac-4148-9c80-135ba3adc661`, installed driver SHA-256
`278abdfe1f2b41e373f2bc013f36141db25fcb71e346b25b31e3ee7101858973`, battery
72% `Discharging` at -2.29 A, USB `online=0`. This differs from the frozen
baseline in `docs/POST-MERGE-BACKLOG.md` §0; no target evidence from that
baseline is reused here.
