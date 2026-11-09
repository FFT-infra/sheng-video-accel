// SPDX-License-Identifier: MIT

#include "device.h"

#include "alloc.h"

#include "../util/error.h"
#include "../util/fourcc.h"

#include <cerrno>
#include <cstring>
#include <glob.h>
#include <system_error>

extern "C" {
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace iris {

namespace {

constexpr unsigned OUTPUT = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr unsigned CAPTURE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

unsigned buf_type(Queue queue) { return queue == Queue::Output ? OUTPUT : CAPTURE; }

int call(int fd, unsigned long request, void* arg)
{
    int result;
    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

void checked(int fd, unsigned long request, void* arg, const char* name)
{
    if (call(fd, request, arg) < 0)
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string(name) + ": " + std::strerror(errno));
}

class RealDevice final : public Device {
public:
    RealDevice(std::string path, int fd)
        : path_(std::move(path))
        , fd_(fd)
    {
    }
    ~RealDevice() override
    {
        if (fd_ >= 0)
            close(fd_);
    }

    std::string path() const override { return path_; }
    int fd() const override { return fd_; }

    FormatInfo get_format(Queue queue) override
    {
        v4l2_format format = {};
        format.type = buf_type(queue);
        checked(fd_, VIDIOC_G_FMT, &format, "G_FMT");
        return from(format);
    }

    FormatInfo set_format(Queue queue, const FormatInfo& request) override
    {
        v4l2_format format = {};
        format.type = buf_type(queue);
        checked(fd_, VIDIOC_G_FMT, &format, "G_FMT");
        format.fmt.pix_mp.pixelformat = request.pixelformat;
        format.fmt.pix_mp.width = request.width;
        format.fmt.pix_mp.height = request.height;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (queue == Queue::Output) {
            // Compressed OUTPUT sizeimage is the AU capacity; the driver
            // cannot derive it from the geometry.
            format.fmt.pix_mp.num_planes = 1;
            format.fmt.pix_mp.plane_fmt[0].sizeimage = request.sizeimage;
            format.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
        }
        checked(fd_, VIDIOC_S_FMT, &format, "S_FMT");
        return from(format);
    }

    std::optional<Selection> get_compose(Queue queue) override
    {
        v4l2_selection selection = {};
        selection.type = buf_type(queue);
        selection.target = V4L2_SEL_TGT_COMPOSE;
        if (call(fd_, VIDIOC_G_SELECTION, &selection) < 0)
            return std::nullopt;
        return Selection { static_cast<unsigned>(selection.r.left), static_cast<unsigned>(selection.r.top),
            selection.r.width, selection.r.height };
    }

    void set_crop(unsigned left, unsigned top, unsigned width, unsigned height) override
    {
        v4l2_selection selection = {};
        // The encoder's crop target is only reachable on the single-planar
        // OUTPUT type; _MPLANE returns EINVAL.
        selection.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        selection.target = V4L2_SEL_TGT_CROP;
        selection.r.left = static_cast<int32_t>(left);
        selection.r.top = static_cast<int32_t>(top);
        selection.r.width = width;
        selection.r.height = height;
        checked(fd_, VIDIOC_S_SELECTION, &selection, "S_SELECTION");
    }

    bool format_supported(Queue queue, uint32_t pixelformat) override
    {
        v4l2_fmtdesc desc = {};
        desc.type = buf_type(queue);
        while (call(fd_, VIDIOC_ENUM_FMT, &desc) == 0) {
            if (desc.pixelformat == pixelformat)
                return true;
            desc.index++;
        }
        return false;
    }

    std::optional<int32_t> get_control(uint32_t id) override
    {
        v4l2_control control = { .id = id, .value = 0 };
        if (call(fd_, VIDIOC_G_CTRL, &control) < 0)
            return std::nullopt;
        return control.value;
    }

    bool set_control(uint32_t id, int32_t value) override
    {
        v4l2_control control = { .id = id, .value = value };
        if (call(fd_, VIDIOC_S_CTRL, &control) == 0)
            return true;
        if (errno == EINVAL || errno == ENOTTY || errno == EACCES)
            return false;
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string("S_CTRL: ") + std::strerror(errno));
    }

    unsigned request_buffers(Queue queue, unsigned count, uint32_t memory = V4L2_MEMORY_MMAP) override
    {
        v4l2_requestbuffers request = {};
        request.type = buf_type(queue);
        request.memory = memory;
        request.count = count;
        checked(fd_, VIDIOC_REQBUFS, &request, "REQBUFS");
        memory_[queue == Queue::Output ? 0 : 1] = memory;
        return request.count;
    }

    BufferInfo query_buffer(Queue queue, unsigned index) override
    {
        v4l2_plane plane = {};
        v4l2_buffer buffer = {};
        buffer.type = buf_type(queue);
        buffer.index = index;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        checked(fd_, VIDIOC_QUERYBUF, &buffer, "QUERYBUF");
        return BufferInfo { index, plane.length, plane.m.mem_offset, buffer.flags };
    }

    int export_buffer(Queue queue, unsigned index) override
    {
        v4l2_exportbuffer request = {};
        request.type = buf_type(queue);
        request.index = index;
        request.plane = 0;
        request.flags = O_CLOEXEC | O_RDWR;
        checked(fd_, VIDIOC_EXPBUF, &request, "EXPBUF");
        return request.fd;
    }

    void* map_buffer(Queue, const BufferInfo& info) override
    {
        void* mapping = mmap(nullptr, info.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, info.mem_offset);
        require(mapping != MAP_FAILED, "mmap V4L2 buffer");
        return mapping;
    }

    void unmap_buffer(void* mapping, unsigned length) override
    {
        if (mapping)
            munmap(mapping, length);
    }

    void queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us, uint32_t flags)
        override
    {
        v4l2_plane plane = {};
        v4l2_buffer buffer = {};
        buffer.type = buf_type(queue);
        buffer.index = index;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        // Native Iris clients submit both queues with FIELD_NONE; FIELD_ANY
        // is accepted for the first CAPTURE QBUF but fails firmware
        // validation on a later one.
        buffer.field = V4L2_FIELD_NONE;
        buffer.flags = flags;
        plane.bytesused = bytesused;
        buffer.timestamp.tv_sec = static_cast<time_t>(timestamp_us / 1000000);
        buffer.timestamp.tv_usec = static_cast<suseconds_t>(timestamp_us % 1000000);
        checked(fd_, VIDIOC_QBUF, &buffer, "QBUF");
    }

    int allocate_dmabuf(unsigned size) override { return allocate_dma_heap(size); }

    void queue_buffer_dmabuf(Queue queue, unsigned index, int dmabuf_fd, unsigned length,
        unsigned bytesused, uint64_t timestamp_us, uint32_t flags) override
    {
        v4l2_plane plane = {};
        v4l2_buffer buffer = {};
        buffer.type = buf_type(queue);
        buffer.index = index;
        buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.length = 1;
        buffer.m.planes = &plane;
        buffer.field = V4L2_FIELD_NONE;
        buffer.flags = flags;
        plane.m.fd = dmabuf_fd;
        plane.length = length;
        plane.bytesused = bytesused;
        plane.data_offset = 0;
        buffer.timestamp.tv_sec = static_cast<time_t>(timestamp_us / 1000000);
        buffer.timestamp.tv_usec = static_cast<suseconds_t>(timestamp_us % 1000000);
        checked(fd_, VIDIOC_QBUF, &buffer, "QBUF DMABUF");
    }

    std::optional<Dequeued> dequeue_buffer(Queue queue) override
    {
        v4l2_plane plane = {};
        v4l2_buffer buffer = {};
        buffer.type = buf_type(queue);
        buffer.memory = memory_[queue == Queue::Output ? 0 : 1];
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (call(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return std::nullopt;
            // EPIPE on CAPTURE means the queue is drained after LAST; that is
            // the same "nothing to dequeue" answer as EAGAIN for our loop.
            if (errno == EPIPE && queue == Queue::Capture)
                return std::nullopt;
            throw Error(VA_STATUS_ERROR_DECODING_ERROR, std::string("DQBUF: ") + std::strerror(errno));
        }
        Dequeued result;
        result.index = buffer.index;
        result.flags = buffer.flags;
        result.timestamp_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1000000
            + static_cast<uint64_t>(buffer.timestamp.tv_usec);
        result.bytesused = plane.bytesused;
        result.data_offset = plane.data_offset;
        return result;
    }

    void stream(Queue queue, bool on) override
    {
        unsigned type = buf_type(queue);
        checked(fd_, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type, on ? "STREAMON" : "STREAMOFF");
    }

    void set_frame_rate(Queue queue, unsigned num, unsigned den) override
    {
        v4l2_streamparm parm = {};
        parm.type = buf_type(queue);
        v4l2_fract& interval = queue == Queue::Output ? parm.parm.output.timeperframe
                                                      : parm.parm.capture.timeperframe;
        // The ioctl takes the frame interval, not the rate. The kernel
        // rejects a rate below 1 fps with EINVAL, so an inverted pair (30/1
        // as 30 s per frame) is not a harmless mistake.
        interval.numerator = den;
        interval.denominator = num;
        checked(fd_, VIDIOC_S_PARM, &parm, "S_PARM");
    }

    void subscribe(uint32_t event_type) override
    {
        v4l2_event_subscription subscription = {};
        subscription.type = event_type;
        checked(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    }

    Event dequeue_event() override
    {
        v4l2_event event = {};
        if (call(fd_, VIDIOC_DQEVENT, &event) < 0)
            return Event::None;
        if (event.type == V4L2_EVENT_SOURCE_CHANGE)
            return Event::SourceChange;
        if (event.type == V4L2_EVENT_EOS)
            return Event::Eos;
        return Event::None;
    }

    bool decoder_command(uint32_t cmd) override
    {
        v4l2_decoder_cmd command = {};
        command.cmd = cmd;
        if (call(fd_, VIDIOC_DECODER_CMD, &command) == 0)
            return true;
        if (errno == EBUSY)
            return false;
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string("DECODER_CMD: ") + std::strerror(errno));
    }

    bool encoder_command(uint32_t cmd) override
    {
        v4l2_encoder_cmd command = {};
        command.cmd = cmd;
        if (call(fd_, VIDIOC_ENCODER_CMD, &command) == 0)
            return true;
        if (errno == EBUSY)
            return false;
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string("ENCODER_CMD: ") + std::strerror(errno));
    }

    short poll(short events, int timeout_ms) override
    {
        pollfd pfd = { fd_, events, 0 };
        int result;
        do {
            result = ::poll(&pfd, 1, timeout_ms);
        } while (result < 0 && errno == EINTR);
        require(result >= 0 && !(pfd.revents & POLLNVAL), "poll decoder fd");
        return result > 0 ? pfd.revents : 0;
    }

private:
    static FormatInfo from(const v4l2_format& format)
    {
        const auto& mp = format.fmt.pix_mp;
        return FormatInfo { mp.pixelformat, mp.width, mp.height, mp.num_planes, mp.plane_fmt[0].bytesperline,
            mp.plane_fmt[0].sizeimage };
    }

    std::string path_;
    int fd_;
    uint32_t memory_[2] = { V4L2_MEMORY_MMAP, V4L2_MEMORY_MMAP };
};

// A coded bitstream, as opposed to the compressed *raw* formats (QC08C,
// QC10C) the decoder's CAPTURE queue also enumerates. Both carry
// V4L2_FMT_FLAG_COMPRESSED, so the flag alone does not decide the node's
// role: only these fourccs are access units.
bool is_bitstream(uint32_t format)
{
    return format == V4L2_PIX_FMT_H264 || format == V4L2_PIX_FMT_HEVC || format == V4L2_PIX_FMT_VP9
        || format == V4L2_PIX_FMT_AV1;
}

bool is_iris_node(int fd, NodeRole role)
{
    v4l2_capability capability = {};
    if (call(fd, VIDIOC_QUERYCAP, &capability) < 0)
        return false;
    if (std::strcmp(reinterpret_cast<const char*>(capability.driver), "iris_driver") != 0)
        return false;
    const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps
                                                                            : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE))
        return false;
    // Both nodes carry the same driver name. The decoder consumes a coded
    // OUTPUT format; the encoder produces a coded CAPTURE one, so the role
    // is decided by which queue enumerates a bitstream.
    v4l2_fmtdesc desc = {};
    desc.type = role == NodeRole::Decoder ? OUTPUT : CAPTURE;
    while (call(fd, VIDIOC_ENUM_FMT, &desc) == 0) {
        if ((desc.flags & V4L2_FMT_FLAG_COMPRESSED) && is_bitstream(desc.pixelformat))
            return true;
        desc.index++;
    }
    return false;
}

} // namespace

std::unique_ptr<Device> open_device(const std::string& explicit_path, NodeRole role)
{
    const bool encoder = role == NodeRole::Encoder;
    const char* variable = encoder ? "LIBVA_V4L2_ENCODER_PATH" : "LIBVA_V4L2_VIDEO_PATH";
    std::vector<std::string> candidates;
    if (!explicit_path.empty()) {
        candidates.push_back(explicit_path);
    } else {
        glob_t matches = {};
        if (glob("/dev/video*", 0, nullptr, &matches) == 0)
            for (size_t i = 0; i < matches.gl_pathc; ++i)
                candidates.emplace_back(matches.gl_pathv[i]);
        globfree(&matches);
    }
    for (const auto& candidate : candidates) {
        const int fd = open(candidate.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (is_iris_node(fd, role))
            return std::make_unique<RealDevice>(candidate, fd);
        close(fd);
    }
    throw Error(VA_STATUS_ERROR_OPERATION_FAILED,
        explicit_path.empty()
            ? std::string("no /dev/video* node reports iris_driver with a compressed ")
                + (encoder ? "CAPTURE" : "OUTPUT") + " format"
            : std::string(variable) + " is not an iris_driver "
                + (encoder ? "encoder: " : "decoder: ") + explicit_path);
}

} // namespace iris
