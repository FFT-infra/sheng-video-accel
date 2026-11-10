# Experiment: AV1 internal DPB slot translation (host spike, 2026-11-10)

Branch: `exp/av1-noshow-dpb`, from `origin/main` `65686a2`.
Status: **host spike phase only**. Nothing deployed to `sheng`, no `src/`
change, no kernel module change, no capability claim.

## Question

Firefox 158 syncs each surface from its decoder thread. Our AV1 translator holds
one picture until the next access unit names the DPB slots it refreshed, so the
next access unit cannot arrive while Firefox waits. Radxa `qcom-va-driver`
`src/av1.cpp` allocates its own eight internal DPB slots, emits one complete
temporal unit per VA picture, and when all eight slots are live replays the
previous uncommitted picture as a hidden NOSHOW frame inside the *same* temporal
unit before the current frame.

This spike asks whether that idea can be layered onto our existing
`av1_build_temporal_unit()` by rewriting the VA picture buffer only, without
changing the bit writer. It does not ask whether the hardware accepts it.

## What was built

`~/Lab/Bridge/tmp/av1-noshow-spike/slot-remap.cc`, compiled on the host against
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
   `1, 2, 3, 4, 5, 6, 7` for frames 2-8 and `-1` for frame 9. Radxa resolves
   this only when the *current* picture's reference map stops naming a slot,
   which frees exactly one slot for the hidden install frame. The spike
   confirms that condition: dropping one reference on frame 9 makes
   `free_slot()` return `1` again. The replay is therefore conditional on the
   next picture's reference map, not on a timer or a guess.

3. **The existing writer cannot produce the required temporal unit.**
   `av1_obu.cc:914-920` emits a temporal delimiter, an optional sequence
   header, and exactly one `OBU_FRAME` per call. Radxa instead emits one TD,
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
   completing them through vb2"). **This is the point where 0005 becomes a real
   prerequisite**, which corrects the earlier claim that only `av1.cc` and
   `av1_obu.cc` would need changing.

5. A real AV1 OBU stream from `aomenc` decodes cleanly with libdav1d on this
   host, so the oracle is available: `ffmpeg -f obu -c:v libdav1d ...` produced
   a frame hash for a 64x64 key frame.

6. **One temporal unit with two `OBU_FRAME`s is legal AV1.** A 20-frame
   lookahead stream (`-lag-in-frames 8 -arnr-max-frames 0`) produced TU 9 with
   OBU types `[2, 6, 6]`: one temporal delimiter followed by two frame OBUs.
   libdav1d decoded all 20 frames from that stream. Radxa's container shape is
   therefore sound; our earlier failure was not the TU layout but the hand-
   edited frame header. Clearing the `show_frame` bit in an existing key-frame
   header is invalid: `show_frame=0` inserts a `showable_frame` bit and changes
   the key-frame error-resilient syntax, so every following header field shifts.
   ffmpeg's `av1_frame_merge` rejected it with `zero_bit out of range: 1`. A
   hidden install frame must be *built* from the parsed parameters with
   `show=0` (Radxa `frame(... bool show ...)`), not patched into a shown frame.
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

`src/codec/av1_dpb.cc` / `.h` add `make_av1_dpb()`, a separate translator that
is **not** selected by `src/va/picture.cc` (`make_translator()` still returns
the qualified held-picture translator). Nothing in the shipped runtime changed.

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

- `remember()` still writes `references_.order_hint[s]` from the shown frame's
  `order_hint`; the hidden install frame and GM state need their own values.
- The test constructs plausible VA parameter structs, not pixels from a real
  encoder. There is no libdav1d pixel oracle for the DPB path yet; the only
  pixel oracle is the separate two-frame fixture in `av1-install-tu-roundtrip.sh`.
- `out.hidden` is taken from the shown frame only. The internal hidden frame's
  completion is not represented as a separate target.
- The prototype does not handle `show_frame=0` client pictures, resolution
  changes, EOS, or a sequence restart through the internal DPB.

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

## Stop / next gate

Before selecting this translator in `src/va/picture.cc`:

- Give the DPB translator a pixel oracle that feeds real encoder-derived VA
  parameters through `make_av1_dpb()` and compares the rebuilt OBUs to a
  software decode.
- Remove `wait_for_release()` and the bounded-sync tail in
  `src/va/surface.cc`; the DPB translator holds no picture.
- Re-derive the Import invariant on the target: one client buffer in flight,
  no skipped picture, misplaced completions fail both surfaces.

Then a guarded target probe answers whether the 0001-0004 module returns the
NOSHOW completion to userspace with the shape the host test models
(`scripts/iris-experiment-guard.sh -- ...`). **No such probe has been run and
kernel 0005 has not been loaded in this experiment.** The target was re-read
before this work: boot `7a26df99-f7ac-4148-9c80-135ba3adc661`, `Charging` at
52% with USB online, but the hottest thermal zone was 90.7 C, above the
guard's 85 C default (`scripts/iris-experiment-guard.sh:64`), so a guarded run
would be refused before starting. A reset or a thermal breach is a failed
experiment per `AGENTS.md`.

Only after those are done does the target probe answer whether the 0001-0004
module returns that NOSHOW completion to userspace
(`scripts/iris-experiment-guard.sh -- ...`). A reset or a thermal breach is a
failed experiment per `AGENTS.md`.

## Environment note

`sheng` was re-read before this spike: boot ID
`7a26df99-f7ac-4148-9c80-135ba3adc661`, installed driver SHA-256
`278abdfe1f2b41e373f2bc013f36141db25fcb71e346b25b31e3ee7101858973`, battery
72% `Discharging` at -2.29 A, USB `online=0`. This differs from the frozen
baseline in `docs/POST-MERGE-BACKLOG.md` §0; no target evidence from that
baseline is reused here.
