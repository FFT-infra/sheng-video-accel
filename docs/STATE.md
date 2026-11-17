# Current state

This is the short answer to "what works right now". It records the current
qualified combination and points at the authoritative detail. It is not an
evidence record: `TEST-RESULTS.md` holds the runs, and
`data/iris-codec-capabilities.json` holds the machine-readable support
boundary. If this page and either of those disagree, those two win.

## Qualified combination

| Part | Current value |
| --- | --- |
| Source | `phxinyang/qualcomm-iris-vaapi`, branch `feat/kernel-7.3.4` (PR #22, unmerged) |
| Distribution baseline | Fedora 44, aarch64 |
| Kernel | `ianchb/sm8550-mainline`, branch `sheng-7.3.4`, commit `87ac0f2347eb`, with patches 0008-0009 |
| VA driver runtime | `v4l2_drv_video.so`, resolved by `iris_driver` QUERYCAP name |
| Package | `libva-v4l2-iris`, `Version: 0.1.0`, `Release: 4%{?dist}` |

The 7.3.4 combination is what `TEST-RESULTS.md` records from 2026-11-16: the
four codecs at 8K, two concurrent 1080p clients, the Import policy and the
encode rates, all on boot `fd0b90f4-d968-4340-94f9-f2fd7bc6c52a`. The 7.2.6
combination (`42f3b40c702a` with 0001-0004) remains qualified and is what the
earlier records in that file used. Stock kernels without the patches still
build and install: the driver falls back to display-order decoding with GPU
copy, and requested IDRs are handled by intra-period repositioning. No
capability claim in this page extends to that fallback.

## Capability boundary

`data/iris-codec-capabilities.json` is the single source of truth for codecs,
VA profiles, encode entrypoints and publish-policy qualification. Decode of
H.264, HEVC Main / Main 10, VP9 Profile 0 / 2 and AV1 Profile 0 is qualified
on the combination above; H.264 and HEVC Main encode are qualified. The table
also lists what is rejected on each codec. VA profiles are exposed only for
entries marked `production: supported`, and a publish policy is advertised
only after its hardware gate passed.

Decode is qualified up to the node's maximum geometry, 7680x4320, for all four
codecs, and for two independent 1920x1080 clients decoding at once. The gates
are `test/iris-8k-gate.sh` and `test/iris-two-player-gate.sh`; a 1800 s mixed
H.264/HEVC window on 7.3.4 is recorded, and the two-hour 640x360 soak remains
the long-window record on 7.2.6. Both gates are short exact-pixel checks, not
throughput or thermal results.

## Kernel patches

`packaging/kernel/README.md` describes all nine patches and their evidence.
0001-0004 are the qualified 7.2.6 module. 0005 was run on the target and
rejected; 0006 is one guarded run with a no-regression verdict; 0007 was
built and run with a no-reason-to-ship verdict. None of 0005-0007 belongs in
a released module. 0008-0009 are the 7.3.4 set, and both are required there:
7.3.4 already carries request-sync-frame and the stability fixes 7.2.6 needs
patches for, so the set is smaller, not larger. `SHA256SUMS` and
`test/iris-kernel-patch-check.sh` pin the files downstream build recipes
consume.

## Distribution packaging

Fedora is the qualified baseline. `packaging/arch/` and `debian/` are
maintained as best-effort formats and are held to the same install path,
dependency and manifest checks, but no hardware evidence qualifies them.
The shipped spec runs `test/run-static-checks.sh` from the unpacked source
archive; `.gitattributes` keeps repository-only files out of that archive.

## Where the detail lives

| Question | Read |
| --- | --- |
| What passed, on which boot, with which counters | `TEST-RESULTS.md` |
| What may be advertised | `data/iris-codec-capabilities.json` |
| Trace record vocabulary | `docs/architecture.md` §8 |
| Environment surface | `README.md` |
| Agent working contract | `AGENTS.md` |
| Open workstreams and their history | `docs/POST-MERGE-BACKLOG.md` |
| Build, deploy and rollback | `docs/build-and-deploy.md` |
