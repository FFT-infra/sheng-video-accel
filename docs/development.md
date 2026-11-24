# Development

## Build

The target-independent build is reproducible on the development host:

```sh
CXX=/usr/bin/g++ CC=/usr/bin/gcc PKG_CONFIG=/usr/bin/pkg-config \
PKG_CONFIG_PATH=/usr/lib64/pkgconfig meson setup build-local
ninja -C build-local
ldd -r build-local/src/v4l2_drv_video.so
```

The last command must print no `undefined symbol` lines. A successful link is
not enough for a VA-API driver: an unresolved relocation otherwise appears to
clients as a silent software-decoder fallback.

## Deploy to a target

For a tablet or other V4L2 target, set `IRIS_REMOTE_HOST` to an SSH config
host or address and run:

```sh
IRIS_REMOTE_HOST=your-tablet-host ./test/remote/deploy.sh
```

The remote scripts require this variable and do not contain a project-local
host default. This keeps a public checkout from accidentally targeting a
maintainer's private lab machine.

The deploy script synchronizes by content, excludes generated build and test
artifacts, refreshes source timestamps, performs a clean Ninja rebuild, and
gates the resulting shared object with `ldd -r`. It rejects absolute,
dot-relative, parent-relative, whitespace-containing and shell-active
`IRIS_REMOTE_ROOT` values before the `rsync --delete` command can contact the
target. The remote lab directory is durable and defaults to
`$HOME/iris-vaapi-lab` (`IRIS_LAB_ROOT` overrides it); downloaded Fluster
resources and Qualcomm test tools stay there rather than under a disposable
scratch directory.

After the sync and before the rebuild, every tracked path is re-hashed on the
target with `sha256sum -c` against the host's own listing. The digest the
manifest records is therefore measured on the target rather than assumed from
the sender: a sync that delivered an older tree fails here instead of
producing a manifest that verifies against a library built from the wrong
source. A checkout without `.git` has no file list to check and says so.

Every successful deployment also writes
`build/iris-vaapi-provenance.json`. Because `.git` is deliberately not copied
to the target, `deploy.sh` resolves the full source commit and a SHA-256 over
the current contents of all tracked paths locally, then explicitly injects
both values into the clean remote build. The manifest additionally records
whether the source checkout was dirty, Meson's compiler inventory, the libva
and Meson versions, the target kernel, and the exact driver path, size and
SHA-256. Deployment immediately verifies the manifest against the generated
shared object; a copied, rebuilt or modified `.so` cannot retain a passing
record accidentally.

What the manifest verification alone does not prove is that the tree on the
target is the tree the identity names — it compares the artifact and the
identity against each other, both of which came from this host. The tracked
path check above is what closes that gap.

To re-check a deployed artifact later:

```sh
cd "$HOME/iris-vaapi-lab/src"
python3 test/remote/provenance.py verify \
  --manifest build/iris-vaapi-provenance.json \
  --artifact build/src/v4l2_drv_video.so
```

For a source export that has no local `.git`, deployment requires the source
identity to be explicit:

```sh
IRIS_SOURCE_COMMIT=<full-object-id> \
IRIS_TRACKED_SOURCE_SHA256=<sha256-of-tracked-paths> \
./test/remote/deploy.sh
```

The decoder node is resolved by the `iris_driver` name in
`test/remote/setup.sh`. Do not hardcode `/dev/video17`: V4L2 numbering changes
across boots and that node may belong to the camera subsystem. Override the
resolved node for a run with `LIBVA_V4L2_VIDEO_PATH`.

After deployment, run the short positive oracle before external corpora:

```sh
ssh "$IRIS_REMOTE_HOST" 'cd "$HOME/iris-vaapi-lab/src" && \
  . ./test/lib/iris-env.sh && device=$(iris_resolve_device) && \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_DRIVERS_PATH=$PWD/build/src \
  LIBVA_V4L2_VIDEO_PATH="$device" \
  ./test/target/iris-matrix.sh'
```

The matrix requires exact software/VA frame MD5 equality and strict stateful
EOS accounting. Fluster and native V4L2 suites are additional capability and
firmware diagnostics; always record which decoder path was used.

## Record a target run

A gate result means nothing without the boot, module and driver it ran
against, and those were being copied out of the target by hand for every
claim. `test/remote/gate.sh` runs one command inside the remote checkout
under `scripts/iris-experiment-guard.sh` and writes the identity, the exit
status, the guard state and the source commit next to the command's output:

```sh
IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh \
    --out "$HOME/iris-vaapi-lab/artifacts/gstreamer" \
    -- sh test/target/iris-gstreamer-gate.sh --clip CLIP
```

The record is `summary.json` with schema `qualcomm-iris-vaapi/target-run-v1`,
plus `identity.txt`, `command.stdout`, `command.stderr` and the guard
directory copied back from the target. The runner does not deploy: `identity`
names both the installed package (`installed_driver`) and the driver the
command actually resolves (`driver`/`driver_path`). When the command sets
`LIBVA_DRIVERS_PATH`, `driver` is that build tree, so a run that used a build
tree is visible in the record instead of being remembered as an installed
package. A guard that stopped the run exits 3, and its state says why.

## Kernel upgrade and boot-slot rollback

A kernel change on `sheng` is two independent steps: the boot image decides
which kernel boots, and the module tree decides which `qcom-iris.ko` that
kernel loads. They fail separately, so record and revert them separately.

The tablet has two boot slots. `qbootctl -a` prints the active one and
`/dev/disk/by-partlabel/boot_a` / `boot_b` name the partitions. Flashing the
slot that is *not* active leaves the running system intact until the next
boot, and the bootloader falls back to the other slot if the new one does not
come up.

Back up both slots before touching either. A slot is 96 MiB, so keep the
copies on the target rather than in the repository:

```sh
mkdir -p ~/iris-vaapi-lab/rollback
sudo dd if=/dev/disk/by-partlabel/boot_a of=~/iris-vaapi-lab/rollback/boot_a.img bs=1M
sudo dd if=/dev/disk/by-partlabel/boot_b of=~/iris-vaapi-lab/rollback/boot_b.img bs=1M
sha256sum ~/iris-vaapi-lab/rollback/boot_*.img
```

The slot carries an `ANDROID!` header: kernel size and load address, a zero
ramdisk size, tags at `0x1e00000`, page size 4096. Rebuild the image by
changing only the payload and recomputing the header fields; the cmdline is
part of the header and on this device is `root=PARTLABEL=linux rootwait rw`
plus the Android boot properties the bootloader supplies. An image whose
cmdline differs from the one that boots today is a different experiment.

Flashing is a readback-verified write, not a blind `dd`:

```sh
sudo dd if=~/iris-vaapi-lab/rollback/boot_734_sheng.img \
    of=/dev/disk/by-partlabel/boot_b bs=1M conv=fsync
sudo dd if=/dev/disk/by-partlabel/boot_b bs=1M count=<image-size-in-MiB> \
    | cmp - ~/iris-vaapi-lab/rollback/boot_734_sheng.img
```

`cmp` must print nothing. Then set the slot active, reboot, and confirm
`uname -r`. Do not write the module tree until the new kernel is running:
`build-module.sh` takes `uname -r` and `/proc/config.gz` from the running
kernel, so a module built before the reboot targets the old release and is
rejected by vermagic.

```sh
sudo qbootctl -s b
sudo systemctl reboot
uname -r
```

Installing the module tree is a separate step; the recipe and the
`CONFIG_LOCALVERSION_AUTO` trap are in `packaging/kernel/README.md`. Keep the
previous `qcom-iris.ko.zst` beside the new one so a reload can be undone
without a reflash.

Rolling back is the same two steps in reverse: reflash the backup image onto
the slot, or switch the active slot back with `qbootctl -s a`, restore the
previous module, `depmod -a`, reboot. A boot that hangs after a kernel change
is a failed experiment: revert first, then re-read the change. Never treat a
boot that needed a manual restart as evidence about the kernel under test.

## Packaging

`scripts/install-system.sh` is enough for a lab target, but it needs a build
tree, a checkout and root. For an image that should work on first boot, build
a package instead. All three manifests live under `packaging/` (and `debian/`)
and all three install through that same script, so the packaged layout cannot
drift from the one the lab uses:

| Target | Manifest | libva driver directory |
| --- | --- | --- |
| Fedora aarch64 | `packaging/fedora/libva-v4l2-iris.spec` | `%{_libdir}/dri` |
| Arch aarch64 | `packaging/arch/PKGBUILD` | `/usr/lib/dri` |
| Debian/Ubuntu arm64 | `debian/rules` | `/usr/lib/$(DEB_HOST_MULTIARCH)/dri` |

```sh
# Fedora
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"

# Debian/Ubuntu, from a checkout
dpkg-buildpackage -b -us -uc

# Debian/Ubuntu, from a tarball that has no .git
IRIS_SOURCE_COMMIT=$(git rev-parse HEAD) \
IRIS_TRACKED_SOURCE_SHA256=$(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}') \
IRIS_SOURCE_DIRTY=0 \
    dpkg-buildpackage -b -us -uc
```

The Debian build reads the source identity from the checkout, or from
`IRIS_SOURCE_COMMIT`, `IRIS_TRACKED_SOURCE_SHA256` and `IRIS_SOURCE_DIRTY` in
the environment when it builds from a tarball with no `.git` — the same
contract `rpmbuild --define` and the PKGBUILD's variables follow. With neither,
the install is recorded dirty rather than claiming an identity nothing
verified.

All three build-depend on FFmpeg, because their test stage runs the host checks
and three of those compare against it — the rotation model against software
transpose, and two AV1 oracles against `libdav1d`. A machine that happens to
have FFmpeg installed hides the missing build dependency until a clean build
root runs the test stage and fails there.

All three declare `v4l-utils` as a hard runtime dependency. The diagnostic
commands resolve the decoder node by driver name because the number moves
between boots on this device, and without `v4l2-ctl` they exit instead of
falling back to a fixed `/dev/videoN` that may be a camera.

No browser is a dependency, and no browser is configured. The driver is useful
to any VA-API client, and libva resolves it through the alias without help.

Which browser benefits differs by distribution. On Fedora 44, Chromium
151.0.7922.169 already decodes in hardware through Chromium's own native V4L2
stack and reports `V4L2VideoDecoder` without loading this driver at all.
Google Chrome has no such path and is the client this driver exists for; it is
the one that moves from `FFmpegVideoDecoder` to `VaapiVideoDecoder`.

`test/static/iris-packaging-check.sh` runs in CI and fails when an installed path is
missing from the spec's `%files`, when any of the three manifests stops
delegating to `install-system.sh`, when a manifest stops refreshing the
provenance file after its own strip step, when `v4l-utils` is dropped, when a
format forgets the Vulkan build dependency that the video processor needs, or
when one of the three diagnostics stops being installed beside the driver.

Debian and Ubuntu packaging exists for one aarch64 target and is built in a
clean chroot, not on the device. The reason it took until now is the build
dependency, not the package: `libgbm-dev` is a Mesa package pinned to an exact
`libgbm1`, and installing it into a vendor-Mesa system can downgrade the
graphics stack the vendor shipped. A build chroot (`sbuild`, `pbuilder`) never
touches the running system, which is what the Debian toolchain assumes anyway;
installing build dependencies directly on the tablet is still the wrong move.
Because Meson treats GBM as optional (`meson.build`), a machine without the
development package still builds — the CPU copy engine stays and the GPU blit
path in `iris/gpu_copy.cc` is not compiled in. That is a slower Copy policy,
not a broken build, and it is not the configuration any record in
`TEST-RESULTS.md` measured. The Debian package therefore build-depends on
`libgbm-dev` and lets `${shlibs:Depends}` carry the runtime library, so a
package built in a chroot installs on a device whose own Mesa already
provides it.

For the Arch image projects, drop `packaging/arch/PKGBUILD` into the image
repository's `pkgs/libva-v4l2-iris/` directory and add it to the package list.
The generated rootfs then ships the driver in libva's standard directory and
no user has to set `LIBVA_DRIVERS_PATH`. That path is not validated here: the
qualified target in this repository is Fedora, and an Arch image needs its own
`vainfo` and `media-internals` acceptance before its result may be claimed.

## Target qualification

The production/fallback/rejected codec boundary is machine-readable in
`data/iris-codec-capabilities.json`; keep it in sync with source capability
advertisement and test expectations. It is intentionally conservative: a
codec with an empty `va_profiles` list is not a VA production capability even
when the native V4L2 node can decode it.

For experiments that may reset firmware, wrap the bounded command with
`scripts/iris-experiment-guard.sh -- ...`. It records the initial boot ID,
thermal samples, pack state (`battery.log`: capacity, charging status, voltage,
current and USB online), the display and network boundary
(`display.log`: connector state, `enabled`, `dpms`, preferred mode string,
brightness and fbcon blank level; `network.log`: `up`/`down` per interface;
`command.txt`: the run's own argv, one argument per line) and command output,
enforces a timeout, and refuses to reuse a running guard file until the
previous run has been inspected. A power run is only attributable to decode
when those boundaries are recorded: the kernel does not expose the active
refresh rate to an unprivileged reader, so the report states the refresh it
measured under rather than implying it from `display.log`. Battery
level is a recorded boundary, not a start gate: a discharging or low pack is
valid lab state, and the report must say what the pack did. Classify a long
run by the recorded start/end capacity; a shortened window is a shortened
result, not a cancelled experiment. Temperature works the same way at the top
end: the ceiling (`IRIS_EXPERIMENT_MAX_TEMP_MILLIC`, default 100000) is a
runaway backstop 10 K under the zones' 110000 critical trip, not a comfort
line, and the firmware's own passive throttle band (90000/95000) is inside it.
a record whose peak lands above 95000 must say so. A reset, a boot-ID change
or a trip past the ceiling is a failed experiment, not evidence of codec
support.

### Dynamic resolution

Run the dedicated H.264 reconfiguration gate on the target:

```sh
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_DRIVERS_PATH=$PWD/build/src \
LIBVA_V4L2_VIDEO_PATH="$device" \
./test/target/iris-dynamic-resolution.sh
```

The production default assembles deterministic 640x360 and 1280x720 keyframe
segments into streams with exactly 100 geometry changes. H.264 passes only if
the native GStreamer V4L2 decoder produces byte-identical I420 output, one
FFmpeg VA process survives every change with exact software frame MD5 and EOS
accounting, and 101 fresh VA processes decode the alternating geometries with
the same checks. FFmpeg is allowed to recreate VA contexts on a coded-size
change; the single-process lane therefore qualifies context retirement and
session cleanup rather than asserting a client behavior FFmpeg does not have.

VP9 runs the same lanes with `IRIS_DYNAMIC_CODECS=vp9`; on the decode-order
kernel module it passed all 100 switches in both lanes (TEST-RESULTS.md). Run
it through `scripts/iris-experiment-guard.sh` on any other kernel, because the
display-order module can reset the device on VP9 source changes.
A missing device format, required element or VA capability is a failure,
not a skip.
Development runs may shorten the matrix explicitly with
`IRIS_DYNAMIC_SWITCHES` and `IRIS_DYNAMIC_FRAMES_PER_SEGMENT`; qualification
results must retain the defaults.

### Concurrent soak

The concurrent runner has separate dual-H.264 and mixed H.264+HEVC scenarios:

```sh
./test/target/iris-concurrency-soak.sh dual-h264
./test/target/iris-concurrency-soak.sh mixed
# Run both sequentially:
./test/target/iris-concurrency-soak.sh all
```

Each selected scenario defaults to 7200 seconds of wall-clock playback, using
real-time throttling rather than decoding two hours of timestamps as quickly
as possible. Before the soak starts, every selected codec must pass an exact
software/VA frame-MD5 oracle. During the run the script records progress,
driver traces, boot ID, uptime and every readable thermal zone. It terminates
workers and fails on a boot-ID change, an unreadable thermal inventory, a
temperature above 100000 millidegrees Celsius, a driver timeout/timestamp
error, incomplete stateful teardown, too few decoded frames, or any worker
failure.

`IRIS_SOAK_SECONDS` may shorten a development run, but the report is labelled
`SHORTENED` unless it is exactly 7200 seconds. `IRIS_SOAK_SESSIONS` (1-16,
default 2) sets how many clients the `mixed` scenario runs, alternating H.264
and HEVC, and `IRIS_SOAK_SIZE` (default 640x360) sets the generated geometry.
`IRIS_SOAK_MAX_TEMP_MILLIC` and `IRIS_SOAK_TEMPERATURE_INTERVAL_SECONDS` tune
the thermal guard. A durable
guard file records a running test; a later invocation refuses to hide an
interrupted or rebooted soak until the operator inspects it and deliberately
sets `IRIS_SOAK_ALLOW_STALE_GUARD=1`.

### 8K geometry and two-client gates

The soak above uses 640x360 lanes. Two more target-only gates cover the
geometry and client-count edges. Run both through `test/remote/gate.sh`: a
failed allocation at 8K can reset the firmware, and only the guard records
that a run survived.

Generate the 8K clips and their software baselines once, on the target, in a
scratch directory (never in the repository). The gate reads `h264.mkv` and
`hevc.mkv` but `vp9.webm` and `av1.webm`, and every baseline is named
`<name>-sw.md5`:

```sh
cd /path/to/8k-clips
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=7680x4320:rate=24 -frames:v 4 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 4 \
    h264.mkv
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=7680x4320:rate=24 -frames:v 4 \
    -c:v libx265 -preset ultrafast -x265-params keyint=4:min-keyint=4:scenecut=0 \
    -pix_fmt yuv420p hevc.mkv
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=7680x4320:rate=24 -frames:v 4 \
    -c:v libvpx-vp9 -b:v 8M -deadline realtime -cpu-used 8 -pix_fmt yuv420p \
    -g 4 vp9.webm
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=7680x4320:rate=24 -frames:v 4 \
    -c:v libsvtav1 -preset 12 -pix_fmt yuv420p -g 4 av1.webm
for clip in h264.mkv hevc.mkv vp9.webm av1.webm; do
    ffmpeg -y -hide_banner -loglevel error -i "$clip" -pix_fmt yuv420p \
        -f framemd5 "${clip%.*}-sw.md5"
done
```

The software baselines must come from a software decoder, never from this
driver: a broken driver must not bless its own output. Every clip must be
7680x4320; the gate reads the geometry from `ffprobe` and fails otherwise.

`iris-8k-gate.sh` checks the granted pool against the memory budget, exact
pixels, terminal LAST and zero completion errors for all four codecs. The
two-player gate needs two finite 1920x1080 H.264 clips and a `<clip>.sw.md5`
baseline beside each; both processes decode under real-time pacing with their
own VA display and driver trace:

```sh
IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh \
    --out ~/iris-vaapi-lab/artifacts/run-8k \
    -- env IRIS_8K_DIR=/path/to/8k-clips sh test/target/iris-8k-gate.sh

IRIS_REMOTE_HOST=your-tablet-host test/remote/gate.sh \
    --out ~/iris-vaapi-lab/artifacts/run-two-player \
    -- env IRIS_TWO_PLAYER_CLIP_A=/path/to/a.mp4 \
        IRIS_TWO_PLAYER_CLIP_B=/path/to/b.mp4 sh test/target/iris-two-player-gate.sh
```

Leave `LIBVA_DRIVERS_PATH` unset to gate the installed package; set it to an
absolute build-tree path to gate a build instead. Neither gate is a Meson
test: both need the Iris decoder node. A run whose boot ID changes, or that
trips the thermal ceiling, is a failed experiment.

#### Real-clip media and the pin

Two gates decide from a recorded publish count rather than from a whole-file
comparison: `iris-real-clip-gate.sh` and `iris-av1-grain-gate.sh`. A count is
a property of one exact file, so both refuse a media directory that does not
pin its clips. The directory must hold a `SHA256SUMS` beside the media:

```sh
cd /path/to/realclip
sha256sum *.webm *.mkv *.md5 > SHA256SUMS
```

Generate the clips and their software baselines once, on the target, in a
scratch directory (never in the repository). Every baseline is named
`<name>-sw.md5` and must come from a software decoder:

```sh
# 1280x720 AV1, 120 frames, every picture Import.
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=1280x720:rate=30 -frames:v 120 \
    -c:v libsvtav1 -preset 8 -pix_fmt yuv420p -g 30 av1-720.webm

# 80 frames that switch 1280x720 -> 854x480 mid-stream (DRC). Encode the two
# halves at the sizes the driver was gated with, then concatenate; the second
# half must open on a key frame or it cannot be decoded at the new geometry.
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=1280x720:rate=30 -frames:v 40 \
    -c:v libsvtav1 -preset 8 -pix_fmt yuv420p -g 20 hi.mkv
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=854x480:rate=30 -frames:v 40 \
    -c:v libsvtav1 -preset 8 -pix_fmt yuv420p -g 20 lo.mkv
ffmpeg -y -hide_banner -loglevel error -i hi.mkv -i lo.mkv \
    -filter_complex '[0:v][1:v]concat=n=2:v=1[out]' -map '[out]' \
    -c:v libsvtav1 -preset 8 resize-av1.mkv
rm -f hi.mkv lo.mkv

# 3840x2160 AV1, 30 frames.
ffmpeg -y -hide_banner -loglevel error -f lavfi \
    -i testsrc2=size=3840x2160:rate=30 -frames:v 30 \
    -c:v libsvtav1 -preset 10 -pix_fmt yuv420p -g 15 av1-4k.mkv

for clip in av1-720.webm resize-av1.mkv av1-4k.mkv; do
    ffmpeg -y -hide_banner -loglevel error -c:v libdav1d -i "$clip" \
        -pix_fmt yuv420p -f framemd5 "${clip%.*}-sw.md5"
done
sha256sum *.webm *.mkv *.md5 > SHA256SUMS
```

The expected publish counts live in each gate's `expected_counts()`, keyed by
clip hash. A clip the table does not know is refused rather than measured
against another file's numbers. To add one, run the decode once by hand with
the trace on, read the counts off it, and record them with a comment saying
why they are what they are:

```sh
V4L2_VA_TRACE=1 V4L2_VA_PUBLISH=import \
ffmpeg -y -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 \
    -hwaccel vaapi -hwaccel_output_format vaapi -i new-clip.mkv \
    -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 /dev/null \
    2>new-clip.trace
grep -c 'publish=import' new-clip.trace
grep -c 'publish=copy-' new-clip.trace
grep -c 'publish=invisible' new-clip.trace
tail -1 new-clip.trace     # session finish: submitted, completed, drains
```

Then add the hash to `SHA256SUMS` and a row to `expected_counts()`. The counts
describe the qualified Import policy; a clip whose first run reports
`publish=copy-*` or `publish=error` is not qualified and must not be added
with those numbers.

## Host-side release gate

The CI and local release sequence is:

```sh
meson setup build
ninja -C build
sh test/run-static-checks.sh
meson test -C build --list
meson test -C build --no-rebuild --print-errorlogs
ldd -r build/src/v4l2_drv_video.so
```

CI asserts that the Meson inventory is non-empty and names every required
hardware-independent test, including `stateful-session` and the provenance
tamper test, before executing it. This prevents a renamed, unregistered or
unbuilt behavioural test from yielding an empty green result.
