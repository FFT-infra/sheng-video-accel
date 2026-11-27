// SPDX-License-Identifier: MIT
//
// Every environment variable the driver reads, read once. Only options.cc
// touches the environment; test/static/iris-env-doc-check.sh enforces that and
// keeps README.md and this file in agreement by name.

#pragma once

#include <optional>
#include <string>

namespace iris {

enum class CopyMode { Gpu, Cpu };
enum class PublishOverride { Auto, Copy, Direct, Import };

struct Options {
    // Decoder node override; when empty the node is found by QUERYCAP name.
    std::string video_path;
    // Encoder node override, same rule.
    std::string encoder_path;
    bool trace = false;
    // Bounded vaSyncSurface wait. The first frame of a cold session may take
    // firmware bring-up time and gets cold_start_timeout_ms unless the user
    // pinned the bound explicitly.
    int sync_timeout_ms = 2000;
    bool sync_timeout_overridden = false;
    int cold_start_timeout_ms = 30000;
    CopyMode copy_mode = CopyMode::Gpu;
    PublishOverride publish = PublishOverride::Auto;
    // Opt in to the VA video-processing entrypoint (VAProfileNone /
    // VAEntrypointVideoProc). Off by default so a VPP-capable build does not
    // change the decode/encode surface any client already qualified against.
    bool vpp_enabled = false;
    // On unless V4L2_VA_UBWC=0. A client still gets one only when its
    // modifier list asks for QCOM_COMPRESSED.
    bool ubwc = true;
    // Opt in to B pictures in the encoder (V4L2_CID_MPEG_VIDEO_B_FRAMES).
    // Off by default: the firmware holds the newest queued picture until the
    // picture after it is queued, so the client must keep three pictures in
    // flight (FFmpeg: -async_depth 3) instead of its default two, or the
    // session stalls at the first pair boundary. H.264 only; the firmware's
    // HEVC path fails outright.
    bool b_frames = false;
    // Off by default: the firmware completes the metadata buffer but codes
    // the picture unchanged, so a client would get no effect and no error.
    bool roi = false;
    // AV1 film grain: let the firmware synthesise it from the rebuilt frame
    // header instead of blending it on the CPU. The firmware writes the
    // grained picture, so the completion publishes into
    // current_display_picture and current_frame carries no pixels.
    bool hardware_grain = false;
    // Advertise on an Iris platform nothing here was qualified on. Off by
    // default: a port brings its own evidence first (docs/porting.md).
    bool unverified_platform = false;
    // Directory to write each submitted access unit into; empty disables.
    std::string dump_dir;

    static Options from_environment();
};

} // namespace iris
