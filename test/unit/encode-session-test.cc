// SPDX-License-Identifier: MIT
//
// The encoder session's control contract on a scripted Iris encoder node.
// Deblocking is the interesting one: the request arrives per slice in VA,
// leaves as three V4L2 controls, and the kernel either has them or does not.
// The session must write them when it does and refuse the request when it
// does not, because the firmware's own default is "filtering on, 0/0
// offsets" and returning that for a different request would be a silent lie.
//
// Everything else here is the machinery those cases need: a node that grants
// the encoder formats, accepts the controls a real SM8550 node carries, and
// completes one picture per queued raw buffer.

#include "../../src/iris/encode.h"

#include "../../src/util/error.h"
#include "../../src/util/options.h"
#include "../../src/util/trace.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// Runs `body` and reports the VA status it raised, or -1 when it did not
// raise. A test that only asserts "something threw" would pass on a typo in
// the message, so the status is what gets checked.
template <typename Function> VAStatus status_of(Function&& body)
{
    try {
        body();
    } catch (const iris::Error& error) {
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return static_cast<VAStatus>(-2);
    }
    return static_cast<VAStatus>(-1);
}

constexpr unsigned kCaptureSlot = 1u << 20;
constexpr uint32_t kPayload = 1024;
// The reconstruction surface the first picture occupies; P pictures name it
// as their reference and move to the next one.
constexpr uint32_t kReconstruction = 0x1000;

class FakeEncoderNode final : public iris::Device {
public:
    // `sync_frame_control` models a kernel that carries REQUEST_SYNC_FRAME
    // and therefore exposes V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME; without it
    // the node is the running 7.2.6 shape, where the intra period is the
    // only way to request an IDR.
    explicit FakeEncoderNode(bool loop_filter_controls, bool sync_frame_control = false)
    {
        if (sync_frame_control)
            known_.insert(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);
        for (uint32_t id : { V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
                 V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
                 V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
                 V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_CID_MPEG_VIDEO_BITRATE,
                 V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, V4L2_CID_MPEG_VIDEO_B_FRAMES,
                 V4L2_CID_MPEG_VIDEO_GOP_SIZE, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR,
                 V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
                 V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
                 V4L2_CID_MPEG_VIDEO_H264_MIN_QP, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                 V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP }) {
            known_.insert(id);
        }
        if (loop_filter_controls) {
            for (uint32_t id : { V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
                     V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA,
                     V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
                     V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2,
                     V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2 }) {
                known_.insert(id);
            }
        }
    }

    const std::map<uint32_t, int32_t>& written() const { return written_; }
    unsigned writes(uint32_t id) const
    {
        const auto it = writes_.find(id);
        return it == writes_.end() ? 0 : it->second;
    }
    bool wrote(uint32_t id) const { return written_.count(id) != 0; }
    int32_t value(uint32_t id) const
    {
        const auto it = written_.find(id);
        return it == written_.end() ? INT32_MIN : it->second;
    }
    unsigned crop_width() const { return crop_width_; }
    unsigned crop_height() const { return crop_height_; }

    std::string path() const override { return "/dev/fake-encoder"; }
    int fd() const override { return -1; }

    iris::FormatInfo get_format(iris::Queue queue) override
    {
        iris::FormatInfo info;
        info.pixelformat = queue == iris::Queue::Output ? V4L2_PIX_FMT_NV12 : codec_;
        info.width = width_;
        info.height = height_;
        info.num_planes = 1;
        info.bytesperline = width_;
        info.sizeimage = queue == iris::Queue::Output ? width_ * height_ * 3 / 2 : kCaptureSlot;
        return info;
    }

    iris::FormatInfo set_format(iris::Queue queue, const iris::FormatInfo& request) override
    {
        if (queue == iris::Queue::Capture)
            codec_ = request.pixelformat;
        else
            raw_ = request.pixelformat;
        width_ = request.width;
        height_ = request.height;
        return get_format(queue);
    }

    std::optional<iris::Selection> get_compose(iris::Queue) override { return std::nullopt; }

    void set_crop(unsigned, unsigned, unsigned width, unsigned height) override
    {
        crop_width_ = width;
        crop_height_ = height;
    }

    bool format_supported(iris::Queue, uint32_t) override { return true; }

    std::optional<int32_t> get_control(uint32_t id) override
    {
        if (!known_.count(id))
            return std::nullopt;
        const auto it = written_.find(id);
        return it == written_.end() ? 0 : it->second;
    }

    bool set_control(uint32_t id, int32_t value) override
    {
        if (!known_.count(id))
            return false;
        written_[id] = value;
        ++writes_[id];
        return true;
    }

    unsigned request_buffers(iris::Queue queue, unsigned count, uint32_t) override
    {
        if (count == 0)
            return 0;
        if (queue == iris::Queue::Capture) {
            capture_.assign(std::min(count, 4u), std::vector<uint8_t>(kCaptureSlot));
            return static_cast<unsigned>(capture_.size());
        }
        output_count_ = std::min(count, 4u);
        return output_count_;
    }

    iris::BufferInfo query_buffer(iris::Queue queue, unsigned index) override
    {
        if (queue == iris::Queue::Capture)
            return iris::BufferInfo { index, kCaptureSlot, 0, 0 };
        return iris::BufferInfo { index, 256u * 1024, 0, 0 };
    }

    int export_buffer(iris::Queue, unsigned) override { return -1; }

    void* map_buffer(iris::Queue queue, const iris::BufferInfo& info) override
    {
        if (queue == iris::Queue::Capture && info.index < capture_.size())
            return capture_[info.index].data();
        return nullptr;
    }

    void unmap_buffer(void*, unsigned) override { }

    void queue_buffer(iris::Queue queue, unsigned index, unsigned, uint64_t, uint32_t flags) override
    {
        if (queue == iris::Queue::Capture && flags == 0)
            return;
        if (queue == iris::Queue::Capture && (flags & V4L2_BUF_FLAG_LAST))
            staged_.push_back(iris::Dequeued { index, V4L2_BUF_FLAG_LAST, 0, 0, 0 });
    }

    int allocate_dmabuf(unsigned) override { return -1; }

    void queue_buffer_dmabuf(iris::Queue queue, unsigned index, int, unsigned, unsigned, uint64_t timestamp_us,
        uint32_t) override
    {
        // One raw frame in, one access unit out, and the input buffer comes
        // back — the transaction the session expects from a real node.
        if (queue == iris::Queue::Output) {
            timestamps_.push_back(timestamp_us);
            gop_at_queue_.push_back(written_.count(V4L2_CID_MPEG_VIDEO_GOP_SIZE)
                    ? written_.at(V4L2_CID_MPEG_VIDEO_GOP_SIZE)
                    : INT32_MIN);
            returned_.push_back(index);
            // Withhold completions to model firmware latency: a real node
            // answers a few pictures late, which is what makes the session's
            // pipelining visible.
            if (hold_ == 0) {
                complete_next();
            } else {
                held_outputs_.push_back(timestamp_us);
                ++withheld_;
            }
        }
    }

    // Withhold every completion: the firmware latency a pipelining client
    // hides behind.
    void hold_completions() { hold_ = 1; }
    // Answer every picture at once again.
    void complete_immediately() { hold_ = 0; }
    // Complete `count` withheld pictures, oldest first.
    void release(unsigned count = 1)
    {
        for (unsigned i = 0; i < count && !held_outputs_.empty(); ++i) {
            held_outputs_.erase(held_outputs_.begin());
            complete_next();
            --withheld_;
        }
    }
    // Pictures whose access unit the firmware has not produced yet.
    unsigned outstanding() const { return withheld_; }

    std::optional<iris::Dequeued> dequeue_buffer(iris::Queue queue) override
    {
        if (queue == iris::Queue::Capture) {
            if (staged_.empty())
                return std::nullopt;
            const iris::Dequeued done = staged_.front();
            staged_.erase(staged_.begin());
            return done;
        }
        if (returned_.empty())
            return std::nullopt;
        const unsigned index = returned_.front();
        returned_.erase(returned_.begin());
        return iris::Dequeued { index, 0, 0, 0, 0 };
    }

    void stream(iris::Queue, bool) override { }

    void set_frame_rate(iris::Queue, unsigned num, unsigned den) override
    {
        frame_rate_num_ = num;
        frame_rate_den_ = den;
    }

    const std::vector<uint64_t>& timestamps() const { return timestamps_; }
    const std::vector<int32_t>& gop_at_queue() const { return gop_at_queue_; }
    unsigned frame_rate_num() const { return frame_rate_num_; }
    unsigned frame_rate_den() const { return frame_rate_den_; }

    void subscribe(uint32_t) override { }
    iris::Event dequeue_event() override { return iris::Event::None; }
    bool decoder_command(uint32_t) override { return true; }

    bool encoder_command(uint32_t cmd) override
    {
        // The drain marker: the firmware hands back a bare LAST buffer and
        // the session stops waiting.
        if (cmd == V4L2_ENC_CMD_STOP)
            staged_.push_back(iris::Dequeued { 0, V4L2_BUF_FLAG_LAST, 0, 0, 0 });
        return true;
    }

    short poll(short events, int) override
    {
        short revents = 0;
        if ((events & (POLLIN | POLLPRI)) && !staged_.empty())
            revents |= POLLIN;
        return revents;
    }

private:
    void complete_next()
    {
        staged_.push_back(iris::Dequeued { 0, 0, 0, kPayload, 0 });
    }
    std::set<uint32_t> known_;
    std::map<uint32_t, int32_t> written_;
    std::map<uint32_t, unsigned> writes_;
    std::vector<std::vector<uint8_t>> capture_;
    std::vector<iris::Dequeued> staged_;
    std::vector<unsigned> returned_;
    std::vector<uint64_t> timestamps_;
    std::vector<int32_t> gop_at_queue_;
    uint32_t codec_ = V4L2_PIX_FMT_H264;
    uint32_t raw_ = 0;
    unsigned width_ = 1920, height_ = 1080;
    unsigned crop_width_ = 0, crop_height_ = 0;
    unsigned output_count_ = 0;
    unsigned frame_rate_num_ = 0, frame_rate_den_ = 0;
    // Pictures whose access unit is withheld until release().
    unsigned hold_ = 0;
    unsigned withheld_ = 0;
    std::vector<uint64_t> held_outputs_;
};

std::unique_ptr<iris::EncoderSession> make_session(bool loop_filter_controls, uint32_t codec)
{
    auto device = std::make_unique<FakeEncoderNode>(loop_filter_controls);
    iris::EncodeConfig config;
    config.codec = codec;
    config.width = 1920;
    config.height = 1080;
    return std::make_unique<iris::EncoderSession>(
        std::move(device), config, iris::Options {}, iris::Trace(false));
}

// A stream of pictures in the shape a real client sends: an intra picture
// first, then P pictures that each predict from the surface the previous one
// reconstructed. The session refuses anything else, so a fixture that skipped
// these would test a stream no client can produce.
iris::EncodeParams params(int deblock_mode, int alpha, int beta)
{
    iris::EncodeParams out;
    out.profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    out.rate_mode = iris::RateMode::Vbr;
    out.bitrate = 4000000;
    out.gop_size = 60;
    out.deblock_mode = deblock_mode;
    out.deblock_alpha = alpha;
    out.deblock_beta = beta;
    out.intra = true;
    out.reconstruction = kReconstruction;
    return out;
}

// A P picture that predicts from the surface the previous one reconstructed.
iris::EncodeParams p_params(uint32_t reference = kReconstruction, uint32_t reconstruction = kReconstruction + 1)
{
    iris::EncodeParams out = params(0, 0, 0);
    out.intra = false;
    out.reference = reference;
    out.reconstruction = reconstruction;
    return out;
}

// The reconstruction surface of picture `n` in a chain that started at
// kReconstruction. The first picture is intra and reconstructs kReconstruction.
uint32_t reconstruction_of(unsigned n) { return kReconstruction + n; }

int dmabuf()
{
    const int fd = memfd_create("encode-session-test", 0);
    if (fd >= 0 && ftruncate(fd, kCaptureSlot) != 0) {
        std::fprintf(stderr, "ftruncate: %s\n", std::strerror(errno));
        return -1;
    }
    return fd;
}

// A kernel without the loop-filter controls is the SM8550 case: the firmware
// always filters with 0/0, so only that request may pass. configure() runs
// once per session, so every case gets its own.
void test_default_without_controls()
{
    auto session = make_session(false, V4L2_PIX_FMT_H264);
    check(status_of([&] { session->configure(params(0, 0, 0), 1920, 1080); }) == -1,
        "the kernel's own deblocking default configures");
    check(session->config().width == 1920 && session->config().height == 1080,
        "the surface geometry reaches the encoder crop");

    check(status_of([&] { make_session(false, V4L2_PIX_FMT_H264)->configure(params(1, 0, 0), 1920, 1080); })
            == VA_STATUS_ERROR_UNIMPLEMENTED,
        "disabled deblocking without kernel controls is refused");
    check(status_of([&] { make_session(false, V4L2_PIX_FMT_H264)->configure(params(0, -3, 0), 1920, 1080); })
            == VA_STATUS_ERROR_UNIMPLEMENTED,
        "a deblocking offset without kernel controls is refused");

    // HEVC's across-slices flag is clear by default, which normalises to
    // mode 2: same stream as mode 0 with one slice per picture, so it passes.
    check(status_of([&] { make_session(false, V4L2_PIX_FMT_HEVC)->configure(params(2, 0, 0), 1920, 1080); })
            == -1,
        "HEVC mode 2 configures without kernel controls");
    check(status_of([&] { make_session(false, V4L2_PIX_FMT_HEVC)->configure(params(1, 0, 0), 1920, 1080); })
            == VA_STATUS_ERROR_UNIMPLEMENTED,
        "disabled HEVC deblocking without kernel controls is refused");
}

// The values as they land in the node, read back through the fake. A
// separate session per case keeps the assertion independent of configure()'s
// ordering.
void test_control_values()
{
    struct Case {
        uint32_t codec;
        int mode;
        int alpha;
        int beta;
        uint32_t mode_id;
        uint32_t alpha_id;
        uint32_t beta_id;
        int want_mode;
        const char* what;
    };
    const Case cases[] = {
        { V4L2_PIX_FMT_H264, 0, -3, 4, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA, 0,
            "H.264 keeps the idc numbering" },
        { V4L2_PIX_FMT_H264, 1, 0, 0, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA, 1,
            "H.264 disabled" },
        { V4L2_PIX_FMT_H264, 2, 0, 0, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA, V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA, 2,
            "H.264 disabled at slice boundaries" },
        { V4L2_PIX_FMT_HEVC, 0, 5, -1, V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2, V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2,
            V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED, "HEVC enabled" },
        { V4L2_PIX_FMT_HEVC, 1, 0, 0, V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2, V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2,
            V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED, "HEVC disabled" },
        { V4L2_PIX_FMT_HEVC, 2, 0, 0, V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE,
            V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2, V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2,
            V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY,
            "HEVC disabled at slice boundaries" },
    };
    for (const Case& test : cases) {
        auto node = std::make_unique<FakeEncoderNode>(true);
        FakeEncoderNode* raw = node.get();
        iris::EncodeConfig config;
        config.codec = test.codec;
        config.width = 1920;
        config.height = 1080;
        iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));
        iris::EncodeParams want = params(test.mode, test.alpha, test.beta);
        want.profile = test.codec == V4L2_PIX_FMT_HEVC
            ? static_cast<int>(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN)
            : static_cast<int>(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
        check(status_of([&] { session.configure(want, 1920, 1080); }) == -1, test.what);
        check(raw->value(test.mode_id) == test.want_mode, test.what);
        check(raw->value(test.alpha_id) == test.alpha, test.what);
        check(raw->value(test.beta_id) == test.beta, test.what);
    }
}

// Deblocking is per-context in V4L2 and per-slice in VA. A live context
// cannot take a new value, and saying so beats a control write the kernel
// rejects as not dynamic.
void test_change_needs_a_new_context()
{
    auto node = std::make_unique<FakeEncoderNode>(true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_H264;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    check(status_of([&] { session.configure(params(0, 0, 0), 1920, 1080); }) == -1, "configure");
    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");
    const iris::EncodeResult first = session.encode(fd, kCaptureSlot, params(0, 0, 0));
    check(first.bytes.size() == kPayload, "one picture returns its access unit");
    check(raw->value(V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE) == 0, "filtering stays on");

    check(status_of([&] { session.encode(fd, kCaptureSlot, params(1, 0, 0)); })
            == VA_STATUS_ERROR_UNIMPLEMENTED,
        "changing deblocking mid-context is refused");
    session.finish();
}

// FFmpeg sends VAEncMiscParameterFrameRate only with IDR pictures: its global
// parameters go out with IDR (vaapi_encode.c). A timestamp derived from each
// picture's own rate therefore jumps forward at an IDR and back on the
// picture after it, and the firmware stops returning access units once a
// timestamp goes backwards. The session keeps the rate it configured with and
// advances one frame duration per picture.
void test_timestamps_never_go_backwards()
{
    auto node = std::make_unique<FakeEncoderNode>(true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_H264;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    iris::EncodeParams with_rate = params(0, 0, 0);
    with_rate.frame_rate_num = 24;
    with_rate.frame_rate_den = 1;
    check(status_of([&] { session.configure(with_rate, 1920, 1080); }) == -1, "24 fps configures");
    check(raw->frame_rate_num() == 24 && raw->frame_rate_den() == 1, "S_PARM carries the session rate");

    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");
    // Pictures 1..9 and 11 carry no FrameRate misc, the way FFmpeg encodes
    // them; picture 0 and the IDR at picture 10 do.
    std::vector<iris::EncodeParams> stream;
    for (unsigned i = 0; i < 12; ++i) {
        if (i == 0 || i == 10) {
            // An intra picture: it reconstructs the next surface and needs
            // no reference.
            iris::EncodeParams picture = params(0, 0, 0);
            picture.reconstruction = reconstruction_of(i);
            stream.push_back(picture);
        } else {
            stream.push_back(p_params(reconstruction_of(i - 1), reconstruction_of(i)));
        }
    }
    stream[0].frame_rate_num = 24;
    stream[0].frame_rate_den = 1;
    stream[10].frame_rate_num = 24;
    stream[10].frame_rate_den = 1;
    for (const iris::EncodeParams& picture : stream) {
        const iris::EncodeResult result = session.encode(fd, kCaptureSlot, picture);
        check(result.bytes.size() == kPayload, "every picture returns its access unit");
    }

    const std::vector<uint64_t>& submitted = raw->timestamps();
    bool rising = submitted.size() == stream.size();
    for (size_t i = 1; rising && i < submitted.size(); ++i)
        rising = submitted[i] > submitted[i - 1];
    check(rising, "timestamps rise across an IDR that carries the frame rate");
    session.finish();
}

// The node has no force-keyframe control, so one picture becomes an IDR by
// asking the firmware for an intra picture and putting the period back before
// the picture is queued. An intra-only stream needs no lever, and the lever
// is pulled for every requested picture so no request is dropped.
void test_on_demand_idr()
{
    auto node = std::make_unique<FakeEncoderNode>(true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_H264;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    check(status_of([&] { session.configure(params(0, 0, 0), 1920, 1080); }) == -1, "configure");
    check(raw->value(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 59, "GOP 60 programs 59 into the raw control");

    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");
    check(session.encode(fd, kCaptureSlot, params(0, 0, 0)).bytes.size() == kPayload, "a P-position picture");
    check(raw->value(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 59, "a regular picture leaves the GOP alone");

    iris::EncodeParams key = params(0, 0, 0);
    key.keyframe = true;
    check(session.encode(fd, kCaptureSlot, key).bytes.size() == kPayload, "the requested picture");
    check(raw->value(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 59, "the GOP is back after the IDR");

    // The period must be back before the picture reaches the firmware: the
    // firmware schedules the next boundary from the value it sees, and a
    // restore after the completion made the following picture an IDR too.
    const std::vector<int32_t>& queued = raw->gop_at_queue();
    check(queued.size() == 2 && queued[0] == 59 && queued[1] == 59,
        "the period is restored before the picture is queued");

    // A request at the firmware's own boundary is not skipped: skipping made
    // the driver's schedule drift and dropped requests.
    const unsigned before = raw->writes(V4L2_CID_MPEG_VIDEO_GOP_SIZE);
    check(session.encode(fd, kCaptureSlot, key).bytes.size() == kPayload, "a second requested picture");
    check(raw->writes(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == before + 2,
        "every requested picture pulls the lever and puts it back");

    iris::EncodeParams intra = params(0, 0, 0);
    intra.gop_size = 0;
    intra.keyframe = true;
    check(session.encode(fd, kCaptureSlot, intra).bytes.size() == kPayload, "an intra-only picture");
    check(raw->value(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 0, "an intra-only stream writes no lever");
    session.finish();
}

// A kernel carrying REQUEST_SYNC_FRAME exposes the direct control, and then
// the intra period is left alone: moving it would change the schedule the
// client asked for.
void test_on_demand_idr_by_sync_frame()
{
    auto node = std::make_unique<FakeEncoderNode>(true, true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_H264;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    check(status_of([&] { session.configure(params(0, 0, 0), 1920, 1080); }) == -1, "configure");
    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");
    const unsigned gop_writes = raw->writes(V4L2_CID_MPEG_VIDEO_GOP_SIZE);
    // configure() probes the control once, so count writes, not presence.
    const unsigned probe = raw->writes(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);

    check(session.encode(fd, kCaptureSlot, params(0, 0, 0)).bytes.size() == kPayload, "a regular picture");
    check(raw->writes(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) == probe,
        "a regular picture does not request a sync frame");

    iris::EncodeParams key = params(0, 0, 0);
    key.keyframe = true;
    check(session.encode(fd, kCaptureSlot, key).bytes.size() == kPayload, "the requested picture");
    check(raw->writes(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) == probe + 1,
        "the requested picture uses the direct control");
    check(raw->writes(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == gop_writes,
        "the intra period is untouched when the direct control exists");

    // The direct request is a one-shot on the kernel side, so the picture
    // after it must not ask again.
    const unsigned sync_writes = raw->writes(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);
    check(session.encode(fd, kCaptureSlot, params(0, 0, 0)).bytes.size() == kPayload, "the next picture");
    check(raw->writes(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) == sync_writes,
        "the request is not repeated for the next picture");
    session.finish();
}

// The point of the split: with the firmware holding completions, several
// pictures are in flight at once and each ticket still gets its own access
// unit. A synchronous session could not get past the first submit.
void test_pipelined_pictures()
{
    auto node = std::make_unique<FakeEncoderNode>(true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_H264;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    check(status_of([&] { session.configure(params(0, 0, 0), 1920, 1080); }) == -1, "configure");
    raw->hold_completions();
    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");

    const iris::EncodeTicket first = session.submit(fd, kCaptureSlot, params(0, 0, 0));
    const iris::EncodeTicket second = session.submit(fd, kCaptureSlot, params(0, 0, 0));
    const iris::EncodeTicket third = session.submit(fd, kCaptureSlot, params(0, 0, 0));
    check(raw->outstanding() == 3, "three pictures are in flight without any completion");
    check(first.timestamp_us != second.timestamp_us && second.timestamp_us != third.timestamp_us,
        "every ticket is distinct");

    // A timeout of zero must not consume anything.
    check(status_of([&] { session.wait_timeout(first, 0); }) == VA_STATUS_ERROR_TIMEDOUT,
        "a zero timeout reports the picture is not ready");
    check(raw->outstanding() == 3, "a timed-out wait leaves the picture queued");

    // Completions land oldest first, and each ticket claims its own.
    raw->release(1);
    check(session.wait(first).bytes.size() == kPayload, "the first ticket gets its access unit");
    raw->release(2);
    check(session.wait(third).bytes.size() == kPayload, "a later ticket waits through the middle one");
    check(session.wait(second).bytes.size() == kPayload, "the middle ticket was kept, not overwritten");
    check(raw->outstanding() == 0, "every completion was consumed");

    // The synchronous shape is the same path with the wait attached.
    raw->complete_immediately();
    check(session.encode(fd, kCaptureSlot, params(0, 0, 0)).bytes.size() == kPayload,
        "encode() still submits and waits");
    session.finish();
}

// A picture still with the firmware when the context goes away is resolved
// by the drain rather than dropped: the client that submitted it and never
// waited still gets its bytes.
void test_drain_resolves_outstanding_pictures()
{
    auto node = std::make_unique<FakeEncoderNode>(true);
    FakeEncoderNode* raw = node.get();
    iris::EncodeConfig config;
    config.codec = V4L2_PIX_FMT_HEVC;
    config.width = 1920;
    config.height = 1080;
    iris::EncoderSession session(std::move(node), config, iris::Options {}, iris::Trace(false));

    iris::EncodeParams hevc = params(0, 0, 0);
    hevc.profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
    check(status_of([&] { session.configure(hevc, 1920, 1080); }) == -1, "configure");
    raw->hold_completions();
    const int fd = dmabuf();
    check(fd >= 0, "memfd for the raw frame");
    const iris::EncodeTicket ticket = session.submit(fd, kCaptureSlot, hevc);
    check(raw->outstanding() == 1, "the picture is with the firmware");

    raw->release(1);
    session.finish();
    check(session.wait(ticket).bytes.size() == kPayload, "the drain kept the picture for its ticket");
}

} // namespace

int main()
{
    test_default_without_controls();
    test_control_values();
    test_change_needs_a_new_context();
    test_timestamps_never_go_backwards();
    test_on_demand_idr();
    test_on_demand_idr_by_sync_frame();
    test_pipelined_pictures();
    test_drain_resolves_outstanding_pictures();
    if (failures) {
        std::cerr << failures << " encode session check(s) failed\n";
        return 1;
    }
    std::cout << "PASS encode session deblocking contract\n";
    return 0;
}
