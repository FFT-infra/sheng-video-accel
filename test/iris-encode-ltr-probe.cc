// SPDX-License-Identifier: MIT
//
// Hardware probe: does the encoder node's long-term-reference interface reach
// the firmware, and does the firmware code a different picture when a picture
// is told to predict from an LTR frame?
//
// VA 2.23 has no encode-side long-term-reference field, so no VA client can
// ask for one and this driver exposes none. The kernel controls are there
// (V4L2_CID_MPEG_VIDEO_LTR_COUNT, ..._FRAME_LTR_INDEX, ..._USE_LTR_FRAMES),
// so the probe drives the V4L2 encoder node directly through iris::Device and
// writes them itself.
//
// The oracle is a byte comparison between two otherwise identical sessions:
// the same 60 pictures, the same rate control, the same GOP. The first marks
// picture 10 as the LTR frame and tells picture 12 to predict from it; the
// second writes neither control. If the firmware acts on the request, at
// least one access unit differs; if it ignores it, the two streams are
// byte-identical. Picture 10 and picture 12 carry the same pattern, so a
// working LTR reference is also the cheaper reference for picture 12.
//
// Usage: iris-encode-ltr-probe <encoder-node> [output-directory]
//        The streams are written to ltr-use.h264 and ltr-plain.h264 in the
//        output directory (current directory by default) so a caller can
//        decode-check them.

#include "../src/iris/alloc.h"
#include "../src/iris/device.h"

#include "../src/util/error.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
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
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

constexpr unsigned kWidth = 640;
constexpr unsigned kHeight = 480;
constexpr unsigned kFrames = 60;
constexpr unsigned kGop = 30;
constexpr unsigned kMarkPicture = 10;
constexpr unsigned kUsePicture = 12;
constexpr unsigned kSlots = 4;

// Picture 10 and picture 12 share a pattern; every other picture moves, so
// the LTR frame is also the best reference for picture 12.
void fill_frame(std::vector<uint8_t>& frame, unsigned index)
{
    const unsigned y_size = kWidth * kHeight;
    frame.resize(y_size * 3 / 2);
    const unsigned phase = (index == kMarkPicture || index == kUsePicture) ? 0 : index * 5;
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            frame[y * kWidth + x] = static_cast<uint8_t>((x + y * 2 + phase) & 0xff);
    std::memset(frame.data() + y_size, 128, y_size / 2);
}

struct Run {
    std::vector<std::vector<uint8_t>> pictures;
    bool marked = false;
    bool used = false;
};

class Session {
public:
    Session(const std::string& node, const iris::Layout& layout)
        : device_(iris::open_device(node, iris::NodeRole::Encoder))
        , layout_(layout)
    {
    }

    ~Session()
    {
        for (auto& entry : inputs_)
            if (entry.mapping)
                munmap(entry.mapping, layout_.size);
        for (int fd : input_fds_)
            if (fd >= 0)
                close(fd);
        for (Capture& capture : capture_) {
            if (capture.mapping)
                device_->unmap_buffer(capture.mapping, capture.length);
        }
    }

    void configure()
    {
        const iris::FormatInfo capture = device_->set_format(
            iris::Queue::Capture, iris::FormatInfo { V4L2_PIX_FMT_H264, kWidth, kHeight, 1, 0, 0 });
        iris::require(capture.pixelformat == V4L2_PIX_FMT_H264, "encoder did not accept H.264",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        const iris::FormatInfo output = device_->set_format(
            iris::Queue::Output, iris::FormatInfo { V4L2_PIX_FMT_NV12, kWidth, kHeight, 1, 0, 0 });
        iris::require(output.pixelformat == V4L2_PIX_FMT_NV12, "encoder did not accept NV12",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        device_->set_crop(0, 0, kWidth, kHeight);

        iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                          V4L2_MPEG_VIDEO_H264_PROFILE_HIGH),
            "the node wants the H.264 profile", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1), "frame rate control",
            VA_STATUS_ERROR_OPERATION_FAILED);
        iris::require(device_->set_control(
                          V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR),
            "VBR rate mode", VA_STATUS_ERROR_OPERATION_FAILED);
        iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_BITRATE, 2000000), "bitrate",
            VA_STATUS_ERROR_OPERATION_FAILED);
        iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, kGop), "gop size",
            VA_STATUS_ERROR_OPERATION_FAILED);
        // The kernel refuses LTR_COUNT under CBR and with rate control off,
        // so VBR above is part of the interface, not a preference.
        iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_LTR_COUNT, 1), "LTR count",
            VA_STATUS_ERROR_UNIMPLEMENTED);

        iris::require(device_->request_buffers(iris::Queue::Output, kSlots, V4L2_MEMORY_DMABUF) >= kSlots,
            "encoder input buffers", VA_STATUS_ERROR_OPERATION_FAILED);
        const unsigned captures = device_->request_buffers(iris::Queue::Capture, kSlots);
        iris::require(captures > 0, "encoder output buffers", VA_STATUS_ERROR_OPERATION_FAILED);
        for (unsigned index = 0; index < captures; ++index) {
            const iris::BufferInfo info = device_->query_buffer(iris::Queue::Capture, index);
            capture_.push_back(Capture {
                index, info.length, static_cast<uint8_t*>(device_->map_buffer(iris::Queue::Capture, info)) });
        }

        // One DMA-BUF per input slot; the picture content is rewritten in
        // place before every queue, so four buffers carry sixty pictures.
        for (unsigned slot = 0; slot < kSlots; ++slot) {
            const int fd = device_->allocate_dmabuf(layout_.size);
            iris::require(fd >= 0, "input DMA-BUF", VA_STATUS_ERROR_ALLOCATION_FAILED);
            void* mapping = mmap(nullptr, layout_.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            iris::require(mapping != MAP_FAILED, "input DMA-BUF mapping", VA_STATUS_ERROR_ALLOCATION_FAILED);
            input_fds_.push_back(fd);
            inputs_.push_back(Input { mapping });
            free_.push_back(slot);
        }

        device_->stream(iris::Queue::Output, true);
        device_->stream(iris::Queue::Capture, true);
        for (const Capture& capture : capture_)
            device_->queue_buffer(iris::Queue::Capture, capture.index, capture.length, 0, 0);
    }

    // Write one picture's map into its slot, then queue it. `mark`/`use` are
    // written before the QBUF so they land on this picture.
    unsigned outstanding_count() const { return outstanding_; }

    void submit(unsigned picture, bool mark, bool use)
    {
        const unsigned slot = free_.front();
        free_.erase(free_.begin());
        std::vector<uint8_t> frame;
        fill_frame(frame, picture);
        uint8_t* base = static_cast<uint8_t*>(inputs_[slot].mapping);
        for (unsigned y = 0; y < kHeight; ++y)
            std::memcpy(base + static_cast<size_t>(y) * layout_.stride, frame.data() + y * kWidth, kWidth);
        for (unsigned y = 0; y < kHeight / 2; ++y)
            std::memcpy(base + layout_.chroma_offset() + static_cast<size_t>(y) * layout_.stride,
                frame.data() + static_cast<size_t>(kWidth) * kHeight + y * kWidth, kWidth);

        if (mark)
            iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_FRAME_LTR_INDEX, 0),
                "mark the LTR frame", VA_STATUS_ERROR_OPERATION_FAILED);
        if (use)
            iris::require(device_->set_control(V4L2_CID_MPEG_VIDEO_USE_LTR_FRAMES, 1),
                "use the LTR frame", VA_STATUS_ERROR_OPERATION_FAILED);

        device_->queue_buffer_dmabuf(iris::Queue::Output, slot, input_fds_[slot], layout_.size, layout_.size,
            static_cast<uint64_t>(picture) * 33333, 0);
        ++outstanding_;
    }

    // Wait for one completion. The encoder returns one raw buffer and one
    // coded access unit per picture, so both queues are drained together.
    bool collect(Run& run)
    {
        for (;;) {
            if (device_->poll(POLLIN | POLLOUT, 5000) == 0)
                return false;
            if (auto output = device_->dequeue_buffer(iris::Queue::Output)) {
                free_.push_back(output->index);
                --outstanding_;
            }
            if (const auto coded = device_->dequeue_buffer(iris::Queue::Capture)) {
                if (coded->error()) {
                    std::printf("FAIL coded buffer carries V4L2_BUF_FLAG_ERROR\n");
                    ++failures;
                    return false;
                }
                run.pictures.emplace_back(
                    capture_[coded->index].mapping + coded->data_offset,
                    capture_[coded->index].mapping + coded->data_offset + coded->bytesused);
                device_->queue_buffer(iris::Queue::Capture, coded->index, capture_[coded->index].length, 0, 0);
                return true;
            }
        }
    }

private:
    struct Capture {
        unsigned index;
        unsigned length;
        uint8_t* mapping;
    };
    struct Input {
        void* mapping;
    };

    std::unique_ptr<iris::Device> device_;
    const iris::Layout layout_;
    std::vector<Capture> capture_;
    std::vector<Input> inputs_;
    std::vector<int> input_fds_;
    std::vector<unsigned> free_;
    unsigned outstanding_ = 0;
};

// One 60-picture session. `mark` turns the LTR mark on for picture 10 and
// `use` turns the reference request on for picture 12.
Run encode(const std::string& node, const iris::Layout& layout, bool mark, bool use)
{
    Run run;
    Session session(node, layout);
    session.configure();
    for (unsigned picture = 0; picture < kFrames; ++picture) {
        // Keep at most kSlots-1 inputs in the node so a slot is always free
        // when the next picture is queued.
        while (session.outstanding_count() >= kSlots - 1) {
            if (!session.collect(run))
                return run;
        }
        session.submit(picture, mark && picture == kMarkPicture, use && picture == kUsePicture);
    }
    while (run.pictures.size() < kFrames) {
        if (!session.collect(run))
            break;
    }
    return run;
}

bool write_stream(const std::string& path, const std::vector<std::vector<uint8_t>>& pictures)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    for (const auto& picture : pictures)
        std::fwrite(picture.data(), 1, picture.size(), file);
    std::fclose(file);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <encoder-node> [output-directory]\n", argv[0]);
        return 2;
    }
    const std::string node = argv[1];
    const std::string outdir = argc == 3 ? argv[2] : ".";

    // The interface is what the capability table describes: three controls on
    // the encoder node, and LTR_COUNT only accepted with a non-CBR rate mode.
    {
        auto device = iris::open_device(node, iris::NodeRole::Encoder);
        const std::optional<iris::ControlInfo> count
            = device->query_control(V4L2_CID_MPEG_VIDEO_LTR_COUNT);
        const std::optional<iris::ControlInfo> mark
            = device->query_control(V4L2_CID_MPEG_VIDEO_FRAME_LTR_INDEX);
        const std::optional<iris::ControlInfo> use
            = device->query_control(V4L2_CID_MPEG_VIDEO_USE_LTR_FRAMES);
        check(count.has_value() && mark.has_value() && use.has_value(),
            "the encoder node registers the three LTR controls");
        if (!count || !mark || !use) {
            std::printf("%d failure(s)\n", failures);
            return 1;
        }
    }

    const iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, kWidth, kHeight);
    Run use;
    Run marked;
    Run plain;
    try {
        use = encode(node, layout, true, true);
        marked = encode(node, layout, true, false);
        plain = encode(node, layout, false, false);
    } catch (const iris::Error& error) {
        std::printf("FAIL encode status=%#x: %s\n", error.status, error.what());
        std::printf("%d failure(s)\n", failures + 1);
        return 1;
    }

    check(use.pictures.size() == kFrames, "the LTR session codes every picture");
    check(marked.pictures.size() == kFrames, "the mark-only session codes every picture");
    check(plain.pictures.size() == kFrames, "the control session codes every picture");
    check(write_stream(outdir + "/ltr-use.h264", use.pictures), "the LTR stream is written");
    check(write_stream(outdir + "/ltr-marked.h264", marked.pictures), "the mark-only stream is written");
    check(write_stream(outdir + "/ltr-plain.h264", plain.pictures), "the control stream is written");

    // Each arm isolates one control: marking a picture as the LTR frame is
    // visible without any reference request, and asking picture 12 to predict
    // from that frame is visible on top of the mark alone.
    check(use.pictures != marked.pictures, "using the LTR frame changes the coded picture");
    check(marked.pictures != plain.pictures, "marking the LTR frame changes the coded picture");

    const bool identical = use.pictures == plain.pictures;
    if (!identical && use.pictures.size() == plain.pictures.size()) {
        // Report which pictures moved; the interesting one is kUsePicture.
        std::vector<unsigned> moved;
        for (unsigned i = 0; i < use.pictures.size(); ++i)
            if (use.pictures[i] != plain.pictures[i])
                moved.push_back(i);
        std::printf("INFO %zu picture(s) differ; first=%u use_picture_differs=%s\n", moved.size(),
            moved.empty() ? 0u : moved.front(),
            (use.pictures.size() > kUsePicture && use.pictures[kUsePicture] != plain.pictures[kUsePicture])
                ? "yes"
                : "no");
        check(use.pictures.size() > kUsePicture
                && use.pictures[kUsePicture] != plain.pictures[kUsePicture],
            "the picture told to use the LTR frame changes");
    }

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS encode LTR interface and firmware effect\n");
    return 0;
}
