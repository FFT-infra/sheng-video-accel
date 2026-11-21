// SPDX-License-Identifier: MIT
//
// The one seam between the decoder logic and /dev/videoN. Session talks to
// this interface only; the real implementation wraps ioctl/poll/mmap and a
// FakeDevice in test/unit scripts responses so the state machine can be
// driven without a tablet.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
}

namespace iris {

enum class Queue { Output, Capture };

// The kernel module defines the encoder's ROI delta-QP control locally as
// V4L2_CID_CODEC_BASE + 658 (packaging/kernel/0017-*). A module without that
// patch registers no such control and every write is refused.
constexpr uint32_t kRoiDeltaQpControl = V4L2_CID_CODEC_BASE + 658;

// The shape a caller has to match before writing a control.
struct ControlInfo {
    uint32_t type = 0; // V4L2_CTRL_TYPE_*
    uint32_t flags = 0; // V4L2_CTRL_FLAG_*
    uint32_t elems = 0; // current element count
    uint32_t elem_size = 0;
    uint32_t dims[V4L2_CTRL_MAX_DIMS] = {}; // maximum extent per dimension
    int64_t minimum = 0;
    int64_t maximum = 0;
    int64_t step = 0;
};

// Iris exposes two M2M nodes with the same driver name: the decoder takes a
// compressed OUTPUT format, the encoder produces a compressed CAPTURE one.
enum class NodeRole { Decoder, Encoder };

struct FormatInfo {
    uint32_t pixelformat = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned num_planes = 0;
    unsigned bytesperline = 0; // plane 0
    unsigned sizeimage = 0; // plane 0
};

struct Selection {
    unsigned left = 0, top = 0, width = 0, height = 0;
};

struct BufferInfo {
    unsigned index = 0;
    unsigned length = 0; // plane 0 length in bytes
    unsigned mem_offset = 0; // plane 0 mmap offset
    uint32_t flags = 0; // QUERYBUF flags
};

struct Dequeued {
    unsigned index = 0;
    uint32_t flags = 0;
    uint64_t timestamp_us = 0;
    unsigned bytesused = 0;
    unsigned data_offset = 0;
    bool last() const { return flags & V4L2_BUF_FLAG_LAST; }
    bool error() const { return flags & V4L2_BUF_FLAG_ERROR; }
};

enum class Event { None, SourceChange, Eos };

class Device {
public:
    virtual ~Device() = default;

    virtual std::string path() const = 0;
    virtual int fd() const = 0;

    // The driver's capture_sg module parameter: CAPTURE comes from system
    // memory, so the system-memory budget applies.
    virtual bool capture_from_system() const { return false; }

    // Format negotiation.
    virtual FormatInfo get_format(Queue queue) = 0;
    virtual FormatInfo set_format(Queue queue, const FormatInfo& request) = 0;
    virtual std::optional<Selection> get_compose(Queue queue) = 0;
    // Encoder only: V4L2_SEL_TGT_CROP on the single-planar OUTPUT queue.
    // This is what makes the coded size the visible size.
    virtual void set_crop(unsigned left, unsigned top, unsigned width, unsigned height) = 0;
    virtual bool format_supported(Queue queue, uint32_t pixelformat) = 0;

    // Controls.
    virtual std::optional<int32_t> get_control(uint32_t id) = 0;
    // Returns false if the control does not exist; throws on other errors.
    virtual bool set_control(uint32_t id, int32_t value) = 0;
    // nullopt when the node has no such control; the caller decides from
    // `flags` and `dims` whether it can be written.
    virtual std::optional<ControlInfo> query_control(uint32_t id) = 0;
    // One write of a dynamic s32 array control. False when the node has no
    // such control; throws otherwise, including an array the dims refuse.
    virtual bool set_control_array(uint32_t id, const int32_t* values, size_t count) = 0;

    // Buffers.
    // memory: V4L2_MEMORY_MMAP (driver-owned) or V4L2_MEMORY_DMABUF (client
    // buffers queued per picture, see queue_buffer_dmabuf).
    virtual unsigned request_buffers(Queue queue, unsigned count, uint32_t memory = V4L2_MEMORY_MMAP) = 0;
    virtual BufferInfo query_buffer(Queue queue, unsigned index) = 0;
    virtual int export_buffer(Queue queue, unsigned index) = 0; // returns fd
    virtual void* map_buffer(Queue queue, const BufferInfo& info) = 0;
    virtual void unmap_buffer(void* mapping, unsigned length) = 0;
    virtual void queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us,
        uint32_t flags) = 0;
    // One DMA-BUF of `size` bytes the decoder can write (system DMA heap);
    // the caller closes it. Used for the scratch buffer an Import pool lends
    // the firmware for LAST.
    virtual int allocate_dmabuf(unsigned size) = 0;
    // QBUF an external DMA-BUF into slot `index` of a DMABUF queue.
    virtual void queue_buffer_dmabuf(Queue queue, unsigned index, int dmabuf_fd, unsigned length,
        unsigned bytesused, uint64_t timestamp_us, uint32_t flags) = 0;
    // Non-blocking. nullopt on EAGAIN; throws iris::Error on EIO/EPIPE.
    virtual std::optional<Dequeued> dequeue_buffer(Queue queue) = 0;

    // Streaming and commands.
    virtual void stream(Queue queue, bool on) = 0;
    // Encoder only. `num`/`den` are the frame rate in frames per second;
    // V4L2 stores the inverse, a per-frame interval. CAPTURE sets the rate
    // the firmware rates against; OUTPUT sets the operating rate, which only
    // affects power and clocking.
    virtual void set_frame_rate(Queue queue, unsigned num, unsigned den) = 0;
    virtual void subscribe(uint32_t event_type) = 0;
    virtual Event dequeue_event() = 0;
    // Returns false on EBUSY (retryable), true on success, throws otherwise.
    virtual bool decoder_command(uint32_t cmd) = 0;
    virtual bool encoder_command(uint32_t cmd) = 0;

    // Wait for POLLIN/POLLPRI/POLLOUT activity; returns revents or 0 on timeout.
    virtual short poll(short events, int timeout_ms) = 0;
};

// Open an Iris codec node. With an explicit path the node is verified to be
// an iris_driver M2M node of the requested role; otherwise /dev/video* is
// scanned. The decoder is the node that enumerates a compressed OUTPUT
// format, the encoder the one that enumerates a compressed CAPTURE format.
// Cameras never match.
std::unique_ptr<Device> open_device(
    const std::string& explicit_path, NodeRole role = NodeRole::Decoder);

} // namespace iris
