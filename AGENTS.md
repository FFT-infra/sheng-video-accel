# Agent contract

This file is the working contract for automated coding agents in this
repository. It is not user documentation: README.md, CONTRIBUTING.md and
docs/architecture.md are. Read docs/architecture.md before changing anything
under `src/`.

## Hard rules

- Find the decoder and encoder nodes by their `iris_driver` QUERYCAP name at
  runtime. They are told apart by which queue carries the compressed format:
  OUTPUT for the decoder, CAPTURE for the encoder. No `/dev/videoN` literal
  anywhere in code, launcher or packaging.
- One source of truth per fact, updated in the same commit as the change:
  codec capability and publish-policy qualification live in
  `data/iris-codec-capabilities.json`; the environment surface lives in
  `README.md`; the trace vocabulary lives in `docs/architecture.md` §8.
  Checks enforce the first two; nothing enforces the third, which is exactly
  why it drifts.
- Evidence before capability. A codec, profile or publish policy is
  advertised only after its hardware gate passed and TEST-RESULTS.md records
  the run (docs/architecture.md §9). The Import policy additionally keeps one
  client buffer in flight and never skips a submitted picture; both rules are
  load-bearing, and a completion that lands in a neighbour's buffer must fail
  both surfaces, never publish.
- VA profiles are exposed only for codecs the capability table marks
  `production: supported`.
- Do not edit the vendored UAPI headers in `include/linux/`.
- A browser decode counts as hardware only when a media surface reports the
  selected decoder with frame and drop evidence. A mapped node, a running GPU
  process or a started pipeline proves nothing.

## Verification before a completion claim

```sh
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

For a claim about the target, record the source commit, driver hash, resolved
node, boot ID, decoder name, frame and drop counts, and whether the runtime
used the installed package or a build-tree override. A `DESTDIR` staging run
is not an installation.

## Boundaries

- Preserve untracked user files. Never `git clean`, reset destructively, or
  push unless the user asked for that action in this turn.
- Keep traces, media and build trees out of the repository; large lab
  artifacts belong on the target, not in git.
- Experiments that can reset firmware follow the guard procedure in
  docs/build-and-deploy.md. A reset or a thermal breach is a failed
  experiment, not evidence of codec support.
