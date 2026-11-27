# Porting to another Qualcomm SoC

This is a portability checklist, not a support claim. The only qualified
target is the Xiaomi Pad 6S Pro (`sheng`, SM8550) on Fedora 44, running
either the `sheng-7.2.6` kernel at `42f3b40c702a` with patches 0001-0004 or
`sheng-7.3.4` at `87ac0f2347eb` with patches 0008-0017.
`data/iris-codec-capabilities.json` names that target; do not broaden its
`target` field to imply support for another SoC. No codec, profile or publish
policy may be advertised on a new SoC until its own hardware gate passes and
`TEST-RESULTS.md` records the run.

## What is SM8550-specific

1. **Kernel patches.** The seventeen files in `packaging/kernel/` target
   `ianchb/sm8550-mainline`: 0001-0007 apply to `sheng-7.2.6`
   (`42f3b40c702a`) and 0008-0017 to `sheng-7.3.4` (`87ac0f2347eb`); 0008
   and 0009 are the qualified pair and 0010-0017 are experimental, each with
   its own row in `packaging/kernel/README.md`. On
   7.2.6, 0001 adds `DISPLAY_DELAY_ENABLE`/`DISPLAY_DELAY` to
   `inst_fw_cap_sm8550_dec` and the `sm8550_vdec_input_config_param_*` arrays;
   0002 and 0003 add controls to `inst_fw_cap_sm8550_enc`; 0004 changes
   `iris_vb2_buffer_done()`. On 7.3.4, 0008 carries the decode-order and
   deblocking changes for both gen2 encoder tables and 0009 the timestamp
   change. 0005, 0006 and 0007 are experimental and must stay out of a port
   baseline. 0006 had one guarded run on SM8550 with a "no regression"
   verdict; 0005 and 0007 were rejected on hardware. None of these
   adds a fix claim or portability evidence. A new SoC has its own platform
   tables and firmware capability enum, so every hunk needs re-review and
   rebase, not a blind `git am`. A patch also has to be carried forward to
   whichever kernel branch the port runs: 7.3.4 needs two patches where 7.2.6
   needed four, and the difference is what the kernel already implements, not
   what the driver needs.
   `test/static/iris-kernel-patch-check.sh` pins the current hashes and paths; refresh
   `SHA256SUMS` in the same commit as any patch change.
2. **Kernel build identity.** The target config has BTI/PAC enabled
   (`CONFIG_ARM64_BTI=y`, `CONFIG_ARM64_PTR_AUTH=y`,
   `CONFIG_ARM64_PTR_AUTH_KERNEL=y`). A module-only build must use the running
   kernel's config, `Module.symvers` and toolchain; a differently configured
   module is not acceptable. The recipe in `packaging/kernel/README.md` takes
   the config from `/proc/config.gz` (the `/boot/config-*` files on `sheng`
   differ from the running kernel), writes the `-g<sha>` suffix into
   `localversion`, and derives `Module.symvers` from the running kernel's
   exports.
3. **Device resolution.** Nodes are found at runtime by the `iris_driver`
   QUERYCAP name and the queue carrying the compressed format. No `/dev/videoN`
   literal is allowed in code, launchers or packaging. A new device with a
   different card name needs no code change if the same rule still resolves a
   decoder and an encoder; verify with `scripts/lib/iris-node.sh` and
   `iris-device-check`.
4. **Platform identity.** The driver advertises only on a platform
   `qualified_platforms` in `data/iris-codec-capabilities.json` lists, which
   is `qcom,sm8550-iris` alone. The identity is the first entry of the
   node's devicetree `compatible` list; on any other value `vainfo` shows no
   decode or encode profile, and libva's info message names the value that
   was read. `V4L2_VA_UNVERIFIED_PLATFORM=1` advertises what the node
   enumerates so the gates below can run. It is a porting switch: nothing it
   exposes has evidence, and it is not a way to ship. A port is finished when
   its gates pass, `TEST-RESULTS.md` records them, and the new value is added
   to the table and to `platform_qualified()` in `src/va/driver.cc` in the
   same commit; `test/static/iris-codec-matrix-check.sh` keeps the two lists
   equal.
5. **Capability table.** `data/iris-codec-capabilities.json` records the VA
   profiles, capture formats, Import qualification and rejected features for
   this firmware. `src/va/driver.cc` and
   `test/static/iris-codec-matrix-check.sh` read it. A new SoC may expose a different
   set of compressed formats or bit depths; re-probe and re-record every entry
   before changing the table. Keep `production: supported` only for codecs
   with exact pixel evidence.
6. **Defaults and guards.** `src/iris/session.cc` uses 2000 ms LAST/fence
   waits, `src/util/options.cc` defaults `V4L2_VA_SYNC_TIMEOUT_MS` to 2000
   with a 30000 ms cold start, and `scripts/iris-experiment-guard.sh` defaults
   to 7200 s and 100000 millidegrees Celsius (10 K below this tablet's
   critical trip; the firmware throttles passively at 90000/95000). These were chosen for this
   tablet's firmware; a slower device needs its own guarded measurement, not a
   global knob change.
7. **Packaging.** `packaging/fedora/libva-v4l2-iris.spec`,
   `packaging/arch/PKGBUILD` and `debian/` assume the Fedora 44 / Arch /
   Debian arm64 layouts and install through `scripts/install-system.sh`. The
   Arch PKGBUILD comment names an `arch-xiaomi-sheng` image. The Debian
   package is built in a clean chroot: `libgbm-dev` is pinned to the distro's
   Mesa and is not safe to install over a vendor-patched stack. See
   `docs/development.md` for the chroot recipe.
8. **Validation records.** Every hardware result in `TEST-RESULTS.md` came
   from this target's installed package, driver hash, boot ID, resolved node,
   decoder name, frame/drop counts and kernel module. Those records do not
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
- Run every gate with `V4L2_VA_UNVERIFIED_PLATFORM=1` until the platform is
  in the table, and record that the switch was set.
- Re-derive the capability table from `vainfo`, `v4l2-ctl` and exact
  software/VA framemd5 runs. Update the table, `src/va/driver.cc`, both
  READMEs and the required checks in one commit.
- Re-run the four-codec matrix, real AV1 clip, in-context DRC clip, 10-bit
  probe, 17-case browser matrix and A11-style independent-oracle corpus under
  `scripts/iris-experiment-guard.sh`, recording the source commit, driver
  hash, boot ID, resolved node and frame/drop counts.
- Re-run host gates: `sh test/run-static-checks.sh`,
  `meson test -C build-local --print-errorlogs`, `git diff --check`, and CI.
- Record the new source commit, driver hash, module hash, boot ID, resolved
  node, decoder name, frame/drop counts and whether the run used the installed
  package or a build-tree override.
- A reset, thermal breach or shortened window is a failed experiment, not
  evidence. A `DESTDIR` staging run is not an installation.
