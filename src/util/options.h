// SPDX-License-Identifier: MIT
//
// Every environment variable the driver reads, read once. Only options.cc
// touches the environment; test/iris-env-doc-check.sh enforces that and
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
    // Opt in to UBWC (Q08C) decode surfaces. Off by default: the CAPTURE
    // format is compressed and only a Direct or Import publish can hand the
    // buffer to a client that understands the modifier.
    bool ubwc = false;
    // Opt in to B pictures in the encoder (V4L2_CID_MPEG_VIDEO_B_FRAMES).
    // Off by default: the firmware holds the newest queued picture until the
    // picture after it is queued, so the client must keep three pictures in
    // flight (FFmpeg: -async_depth 3) instead of its default two, or the
    // session stalls at the first pair boundary. H.264 only; the firmware's
    // HEVC path fails outright.
    bool b_frames = false;
    // Opt in to advertising ROI regions in the encoder. Off by default: the
    // kernel module carries the metadata buffer and the firmware completes
    // it, but the coded picture is unchanged on the qualified image, so a
    // client that sets regions would get no effect and no error. The kernel
    // control must exist either way; see TEST-RESULTS.md.
    bool roi = false;
    // Directory to write each submitted access unit into; empty disables.
    std::string dump_dir;

    static Options from_environment();
};

} // namespace iris
