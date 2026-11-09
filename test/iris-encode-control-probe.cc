// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): what the encoder node's control surface
// lets the session honour. It drives iris::EncoderSession against the real
// node, so the answers come from the kernel's control list and not from a
// scripted device.
//
// Usage: iris-encode-control-probe <encoder-node>
//
// Reports, for both codecs:
//   - which of the six loop-filter controls the node exposes;
//   - a configure() with the kernel's own deblocking default (the request
//     FFmpeg makes) must succeed;
//   - a configure() that asks for filtering off, or a non-zero offset, must
//     fail with VA_STATUS_ERROR_UNIMPLEMENTED when the controls are absent;
//   - HEVC's mode 2 (across-slice filtering off, the request a client that
//     leaves the flag clear normalises to) must be accepted either way.
//
// The probe is the hardware half of the rule in docs/architecture.md §10
// invariant 13; test/unit/encode-session-test.cc is the portable half.

#include "../src/iris/device.h"
#include "../src/iris/encode.h"

#include "../src/util/error.h"
#include "../src/util/options.h"
#include "../src/util/trace.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

extern "C" {
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

iris::EncodeParams base_params(uint32_t codec, int deblock_mode, int alpha, int beta)
{
    iris::EncodeParams params;
    params.profile = codec == V4L2_PIX_FMT_HEVC ? static_cast<int>(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN)
                                                : static_cast<int>(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    params.rate_mode = iris::RateMode::Vbr;
    params.bitrate = 4000000;
    params.gop_size = 60;
    params.deblock_mode = deblock_mode;
    params.deblock_alpha = alpha;
    params.deblock_beta = beta;
    return params;
}

// Returns the VA status of one fresh session's configure(), or -1 when it
// succeeded.
VAStatus configure_status(const std::string& node, uint32_t codec, const iris::EncodeParams& params)
{
    auto device = iris::open_device(node, iris::NodeRole::Encoder);
    iris::EncodeConfig config;
    config.codec = codec;
    config.width = 1920;
    config.height = 1088;
    iris::EncoderSession session(std::move(device), config, iris::Options {}, iris::Trace(false));
    try {
        session.configure(params, 1920, 1080);
        session.finish();
    } catch (const iris::Error& error) {
        return error.status;
    }
    return static_cast<VAStatus>(-1);
}

// Prints the six controls and returns whether the node has all of them: the
// expected answer below follows the node's own report, so the probe is a gate
// on a kernel with the controls as well as on one without.
bool report_controls(const std::string& node)
{
    auto device = iris::open_device(node, iris::NodeRole::Encoder);
    const struct {
        const char* name;
        uint32_t id;
    } controls[] = {
        { "h264_loop_filter_mode", V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE },
        { "h264_loop_filter_alpha", V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA },
        { "h264_loop_filter_beta", V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA },
        { "hevc_loop_filter_mode", V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE },
        { "hevc_lf_tc_offset_div2", V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2 },
        { "hevc_lf_beta_offset_div2", V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2 },
    };
    bool present = true;
    for (const auto& control : controls) {
        const bool has = device->get_control(control.id).has_value();
        std::printf("control %-24s %s\n", control.name, has ? "present" : "absent");
        present = present && has;
    }
    return present;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <encoder-node>\n", argv[0]);
        return 2;
    }
    const std::string node = argv[1];

    // The node's own answer decides which requests can be honoured, so print
    // it before judging anything.
    const bool controls = report_controls(node);
    const VAStatus honoured = static_cast<VAStatus>(-1);

    check(configure_status(node, V4L2_PIX_FMT_H264, base_params(V4L2_PIX_FMT_H264, 0, 0, 0)) == -1,
        "h264: the kernel default deblocking request configures");
    check(configure_status(node, V4L2_PIX_FMT_HEVC, base_params(V4L2_PIX_FMT_HEVC, 2, 0, 0)) == -1,
        "hevc: mode 2 configures (the request FFmpeg's zeroed slice parameters normalise to)");

    const VAStatus h264_off = configure_status(node, V4L2_PIX_FMT_H264, base_params(V4L2_PIX_FMT_H264, 1, 0, 0));
    const VAStatus h264_alpha
        = configure_status(node, V4L2_PIX_FMT_H264, base_params(V4L2_PIX_FMT_H264, 0, -3, 0));
    const VAStatus hevc_off = configure_status(node, V4L2_PIX_FMT_HEVC, base_params(V4L2_PIX_FMT_HEVC, 1, 0, 0));

    // A node with the controls takes the request; one without them refuses it
    // rather than encoding with different filtering.
    const VAStatus want_other = controls ? honoured : static_cast<VAStatus>(VA_STATUS_ERROR_UNIMPLEMENTED);
    check(h264_off == want_other, controls ? "h264: disabling deblocking is honoured"
                                           : "h264: disabling deblocking is refused");
    check(h264_alpha == want_other, controls ? "h264: a deblocking offset is honoured"
                                             : "h264: a deblocking offset is refused");
    check(hevc_off == want_other, controls ? "hevc: disabling deblocking is honoured"
                                           : "hevc: disabling deblocking is refused");

    std::printf("%s node=%s loop_filter_controls=%s h264_off=%#x h264_alpha=%#x hevc_off=%#x\n",
        failures ? "FAIL" : "PASS", node.c_str(), controls ? "present" : "absent", h264_off, h264_alpha,
        hevc_off);
    return failures ? 1 : 0;
}
