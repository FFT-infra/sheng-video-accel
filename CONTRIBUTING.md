# Contributing

A change keeps the hardware-decoder contract: real media playback, evidence
that the selected decoder ran, frame and drop counters, and a safe fallback
when the hardware path is unavailable. A video that plays is not the bar.

---

## Development Loop

### 1. Build and Host Verification

Configure and build with Meson, then run the host-side gates used by CI:

```sh
meson setup build-local
ninja -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

All five commands must pass cleanly before submitting changes.

### 2. Code Style

The codebase follows the style defined in `.clang-format`:

```sh
clang-format -i src/**/*.cc src/**/*.h
```

### 3. Hardware Gates

Hardware verification requires an ARM64 Linux target running on Qualcomm Snapdragon SM8550 with the Iris VPU.

Set `IRIS_REMOTE_HOST` explicitly before running remote deployment or gates:

```sh
IRIS_REMOTE_HOST=your-tablet-host ./test/remote/deploy.sh
```

Remote scripts intentionally have no private-host default. Keep downloaded media corpora, traces, screenshots, and build artifacts outside the git repository.

---

## Core Development Rules

Before modifying code, review the automated agent contract in [AGENTS.md](AGENTS.md):

- **Dynamic Node Resolution**: Never hardcode boot-specific `/dev/videoN` nodes in code, scripts, launchers, or packaging. Always resolve nodes dynamically at runtime via the `iris_driver` QUERYCAP name.
- **Single Source of Truth**: Update documentation, capabilities, and code in the same commit:
  - Codec capability and publish policies: `data/iris-codec-capabilities.json`
  - Environment switches: `README.md` and `README.zh-CN.md`
  - Trace vocabulary: `docs/architecture.md` §8
  - Kernel patch hashes: `packaging/kernel/SHA256SUMS`
- **Evidence Before Capability**: Codecs, profiles, and publish policies are advertised only after the hardware gate passes and `TEST-RESULTS.md` records the run.
- **Vendored Headers**: Do not edit vendored UAPI headers in `include/linux/`.

---

## Commits and Pull Requests

- **Commit Messages**: Use short, imperative subjects (e.g., `feat: ...`, `fix: ...`, `docs: ...`, `test: ...`).
- **Validation Record**: Pull request descriptions should state:
  - Target SoC and kernel version.
  - Resolved decoder/encoder names.
  - Driver hash and boot ID.
  - Decoded and dropped frame counts.
  - Any hardware limitations or caveats.
- **Privacy & Cleanliness**: Do not commit private operator paths, device IP addresses, credentials, or generated lab binaries.
