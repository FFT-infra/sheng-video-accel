# Install

`README.md` covers the three distribution packages. This page installs a build
tree and configures clients; `docs/development.md` builds and deploys that tree.

## Durable system and browser installation

A deployment from `docs/development.md` runs from the build tree. Once that
artifact has passed the codec matrix and soak gates, install it into libva's
configured system driver directory together with the diagnostic commands:

```sh
cd "$HOME/iris-vaapi-lab/src"
sudo ./scripts/install-system.sh build
```

`install-system.sh` obtains `driverdir` from `pkg-config libva` (falling back
to libva's `libdir/dri` only when the package omits `driverdir`) and runs
`ldd -r` before copying the shared object. It installs the diagnostic commands
under `/usr/local/bin` and the node rule they share under
`/usr/local/lib/iris-vaapi` by default. `IRIS_INSTALL_PREFIX` changes only that
prefix; it does not redirect the VA driver away from libva's canonical driver
directory. `DESTDIR` provides a non-root packaging/staging mode.

The helper records an install manifest at
`/usr/local/share/iris-vaapi/install-manifest.txt` (or the selected prefix).
It includes the source commit, tracked-source digest, artifact hash, resolved
driver path, and any pre-existing driver backup. To remove only that recorded
installation and restore the previous driver, run
`sudo ./scripts/uninstall-system.sh`; removal refuses to proceed if the
installed artifact was modified after installation. A `DESTDIR` staging run
is packaging evidence only and must not be described as a live system install.

Package builders must refresh the manifest after any ELF post-processing. The
Fedora spec runs the distribution brp strip hooks before
`scripts/refresh-install-manifest.sh`; the Arch PKGBUILD disables makepkg's
later implicit strip, strips explicitly, and runs the same refresh. This keeps
the recorded artifact hash and size equal to the installed package payload.

The node rule lives in one file, `scripts/lib/iris-node.sh`, installed as
`<prefix>/lib/iris-vaapi/iris-node.sh`. It resolves `/dev/video*` on every
call, requires an exact `iris_driver` match, and tells the decoder from the
encoder by the card type, so the companion Iris encoder cannot be selected
merely because its number happens to sort first. An explicit
`LIBVA_V4L2_VIDEO_PATH` remains available for a checked diagnostic override.
`v4l2-ctl` from v4l-utils is therefore a runtime dependency of the
diagnostics.

Three commands are installed, and none of them carries a second copy of the
node rule: `iris-device-check` and `iris-verify` both source that file, so a
boot that renumbers the nodes changes nothing in them. `iris-device-check` is
read-only and reports the device-tree node, the bound driver, the module hash,
the firmware, each queue's coded formats, which optional kernel controls are
present, and whether libva opens the driver with no environment set.
`iris-verify` generates its own clip and checks that a decode and an encode
each move the Iris interrupt counter, that the encoded stream decodes, and that
mpv reaches the native V4L2 path. `iris-configure-clients` writes
`hwdec=vaapi` and the Firefox VA pref into the invoking user's home only.

Check the installed driver with nothing set at all. This is the property the
package ships: libva resolves the driver through the `msm_drv_video.so` alias
(`qcom_drv_video.so` is installed beside it for recipes naming the qcom driver),
and the driver resolves its own node.

```sh
env -u LIBVA_DRIVERS_PATH -u LIBVA_DRIVER_NAME -u LIBVA_V4L2_VIDEO_PATH vainfo
```

Unset the variable; an empty `LIBVA_DRIVERS_PATH=` is not the same thing. libva
takes the empty string as "search no directory" and `vaInitialize` fails with
`unknown libva error` even though the installed driver is fine, which is how a
probe lost its first run.

There is no launcher and no desktop entry. An earlier revision shipped both,
and the launcher exported `LIBVA_DRIVER_NAME`, both node paths, and an isolated
`--user-data-dir`. A desktop start then behaved differently from a terminal
start, and the isolated profile meant a user who clicked the entry got a
browser with no bookmarks and no login state. Chrome needs none of it: measured
on the qualified target with `LIBVA_DRIVER_NAME` and both node overrides
removed, `--type=gpu-process` maps `/usr/lib64/dri/v4l2_drv_video.so` and holds
the decoder node while a clip plays. `test/target/iris-browser-acceptance.sh` launches
the browser directly and samples the holders during playback, so the same
claim is re-checked on every run.

### Browser acceptance gate

Successful playback is not proof of hardware decoding. Use a high-resolution
H.264 or HEVC sample, open `chrome://media-internals`, select the player by its
exact URL, and require both:

```text
kVideoDecoderName = VaapiVideoDecoder
kIsPlatformVideoDecoder = true
```

The qualified run reached this gate with the project driver mapped in the GPU
process and a dynamically resolved Iris node, `VaapiVideoDecoder` and
`kIsPlatformVideoDecoder=true`; its VA trace showed repeated
`vaBeginPicture`/`vaEndPicture` and `stateful capture complete` events. A Fedora
Chromium run is a separate native-V4L2 baseline and reports `V4L2VideoDecoder`;
that result does not prove this driver.

Also record decoded/dropped frame counts, confirm the Iris V4L2 context and
OUTPUT/CAPTURE activity in a trace-enabled diagnostic run, and compare the
device boot ID before and after. Small videos may fall below Chromium's
hardware-decoder performance threshold, so a low-resolution
`FFmpegVideoDecoder` result is not a release verdict. H.264, HEVC, VP9 and
AV1 are the qualified VA profiles (each with its own caveat in the README).
Validate Google Chrome; Fedora's Chromium decodes through its own native V4L2
stack and its result says nothing about this driver.

### Client setup

The package installs the driver and the `msm_drv_video.so` alias, so a client
needs no environment variable to find it. What each client still needs is its
own setting. `iris-configure-clients` writes the mpv and Firefox ones into the
invoking user's home; the rest are set in the client.

| Client | Setting |
| --- | --- |
| mpv | `hwdec=vaapi` in `~/.config/mpv/mpv.conf`, or `--hwdec=vaapi` per run |
| VLC | Preferences -> Input / Codecs -> Hardware-accelerated decoding: **VA-API video decoder via DRM** |
| Sunshine | Encoder `h264_vaapi` or `hevc_vaapi` |
| Firefox | `media.hardware-video-decoding.force-enabled` = `true` in `about:config` (or in `user.js`, which `iris-configure-clients` writes) |
| Chrome | Nothing; confirm with the browser gate above |
| GStreamer | `GST_VA_ALL_DRIVERS=1`, because the plugin filters drivers by vendor allowlist |

FFmpeg decodes through the VA driver with its own flags; the input needs the
raw-video geometry when it is not a container:

```sh
# decode, frames stay on the GPU
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i input.mp4 -f null -

# encode from raw NV12, 1280x720 at 30 fps
ffmpeg -vaapi_device /dev/dri/renderD128 -f rawvideo -pix_fmt yuv420p \
    -s 1280x720 -r 30 -i input.yuv -vf format=nv12,hwupload \
    -c:v h264_vaapi -b:v 4M out.h264
ffmpeg -vaapi_device /dev/dri/renderD128 -f rawvideo -pix_fmt yuv420p \
    -s 1280x720 -r 30 -i input.yuv -vf format=nv12,hwupload \
    -c:v hevc_vaapi -b:v 4M out.hevc
```

`h264_vaapi` and `hevc_vaapi` are the qualified encoders. B pictures in the
H.264 encoder are off unless `V4L2_VA_B_FRAMES` is set, and then the client
needs three pictures in flight: `-bf 1 -async_depth 3`. At FFmpeg's default
`-async_depth 2` a B picture session stops after the first pair, which is the
firmware holding the newest picture rather than a driver defect. HEVC has no
B-picture option: this firmware stalls a reordered stream (TEST-RESULTS.md).
