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
   a frame hash for a 64x64 key frame. The spike has not yet produced a valid
   multi-frame TU for that oracle to judge.

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

## Stop / next gate

Do not deploy to `sheng` until a host prototype builds a legal two-frame
temporal unit that libdav1d decodes to the same pixels as a software-encoded
reference, and the internal-completion accounting is decided. Target probe
(`scripts/iris-experiment-guard.sh -- ...`) then answers only whether the
0001-0004 module returns that NOSHOW completion to userspace; a reset or a
thermal breach is a failed experiment per `AGENTS.md`.

## Environment note

`sheng` was re-read before this spike: boot ID
`7a26df99-f7ac-4148-9c80-135ba3adc661`, installed driver SHA-256
`278abdfe1f2b41e373f2bc013f36141db25fcb71e346b25b31e3ee7101858973`, battery
72% `Discharging` at -2.29 A, USB `online=0`. This differs from the frozen
baseline in `docs/POST-MERGE-BACKLOG.md` §0; no target evidence from that
baseline is reused here.
