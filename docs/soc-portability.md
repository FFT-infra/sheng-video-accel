# Porting to another Qualcomm SoC

This is a portability checklist, not a support claim. The only qualified
target is the Xiaomi Pad 6S Pro (`sheng`, SM8550) on Fedora 44 with the
`sheng-7.2.6` kernel at `42f3b40c702a`. `data/iris-codec-capabilities.json`
names that target; do not broaden its `target` field to imply support for
another SoC. No codec, profile or publish policy may be advertised on a new
SoC until its own hardware gate passes and `TEST-RESULTS.md` records the run.

## What is SM8550-specific

1. **Kernel patches.** The six files in `packaging/kernel/` are rebased onto
   `ianchb/sm8550-mainline` `sheng-7.2.6` (`42f3b40c702a`). 0001 adds
   `DISPLAY_DELAY_ENABLE`/`DISPLAY_DELAY` to `inst_fw_cap_sm8550_dec` and the
   `sm8550_vdec_input_config_param_*` arrays; 0002 and 0003 add controls to
   `inst_fw_cap_sm8550_enc`; 0004 changes `iris_vb2_buffer_done()`; 0005 and
   0006 are experimental and must stay out of a port baseline. A new SoC has
   its own platform tables and firmware capability enum, so every hunk needs
   re-review and rebase, not a blind `git am`. `test/iris-kernel-patch-check.sh`
   pins the current hashes and paths; refresh `SHA256SUMS` in the same commit
   as any patch change.
2. **Kernel build identity.** The target config has BTI/PAC enabled
   (`CONFIG_ARM64_BTI=y`, `CONFIG_ARM64_PTR_AUTH=y`,
   `CONFIG_ARM64_PTR_AUTH_KERNEL=y`). A module-only build must use the running
   kernel's config, `Module.symvers` and toolchain; a differently configured
   module is not acceptable. The recipe in `packaging/kernel/README.md` uses
   `/boot/config-$(uname -r)`, a `localversion` file and a `Module.symvers`
   derived from the running kernel's exports.
3. **Device resolution.** Nodes are found at runtime by the `iris_driver`
   QUERYCAP name and the queue carrying the compressed format. No `/dev/videoN`
   literal is allowed in code, launchers or packaging. A new device with a
   different card name needs no code change if the same rule still resolves a
   decoder and an encoder; verify with `scripts/lib/iris-node.sh` and
   `iris-device-check`.
4. **Capability table.** `data/iris-codec-capabilities.json` records the VA
   profiles, capture formats, Import qualification and rejected features
   measured on this firmware. `src/va/driver.cc` and
   `test/iris-codec-matrix-check.sh` read it. A new SoC may expose a different
   set of compressed formats or bit depths; re-probe and re-record every entry
   before changing the table. Keep `production: supported` only for codecs
   with exact pixel evidence.
5. **Defaults and guards.** `src/iris/session.cc` uses 2000 ms LAST/fence
   waits, `src/util/options.cc` defaults `V4L2_VA_SYNC_TIMEOUT_MS` to 2000
   with a 30000 ms cold start, and `scripts/iris-experiment-guard.sh` defaults
   to 7200 s and 85000 millidegrees Celsius. These were chosen for this
   tablet's firmware; a slower device needs its own guarded measurement, not a
   global knob change.
6. **Packaging.** `packaging/fedora/libva-v4l2-iris.spec` and
   `packaging/arch/PKGBUILD` assume the Fedora 44 / Arch aarch64 layouts and
   install through `scripts/install-system.sh`. The Arch PKGBUILD comment
   names an `arch-xiaomi-sheng` image. There is no Debian package; see
   `docs/build-and-deploy.md` and WP6 in `docs/POST-MERGE-BACKLOG.md` §16.
7. **Validation records.** Every hardware result in `TEST-RESULTS.md` came
   from this target's installed package, driver hash, boot ID, resolved node,
   decoder name, frame/drop counts and 0001-0004 module. Those records do not
   transfer to a new SoC. `docs/architecture.md` §2 and §9 describe the kernel
   contract and gate sequence that must be re-run.

## What a port must redo

- Rebase and review all qualified patches against the new kernel's Iris
  platform data; keep 0005/0006 out unless a separate guarded experiment
  qualifies them.
- Build the module with the new kernel's config, symbol versions and
  toolchain; keep a rollback copy of the previous module.
- Resolve nodes by name and confirm the decoder/encoder queue roles with
  `iris-device-check`.
- Re-derive the capability table from `vainfo`, `v4l2-ctl` and exact
  software/VA framemd5 runs. Update the table, `src/va/driver.cc`, both
  READMEs and the required checks in one commit.
- Re-run the four-codec matrix, real AV1 clip, in-context DRC clip, 10-bit
  probe, 17-case browser matrix and A11-style independent-oracle corpus under
  `scripts/iris-experiment-guard.sh`, recording every §1.8 evidence field.
- Re-run host gates: `sh test/run-static-checks.sh`,
  `meson test -C build-local --print-errorlogs`, `git diff --check`, and CI.
- Record the new source commit, driver hash, module hash, boot ID, resolved
  node, decoder name, frame/drop counts and whether the run used the installed
  package or a build-tree override.
- A reset, thermal breach or shortened window is a failed experiment, not
  evidence. A `DESTDIR` staging run is not an installation.
