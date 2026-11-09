// SPDX-License-Identifier: MIT
//
// A scripted stand-in for the Iris decoder node. It models the parts of the
// stateful contract the Session depends on: SOURCE_CHANGE after the first
// OUTPUT queue, completion of one CAPTURE per OUTPUT in decode or display
// order, LAST on STOP, EBUSY on START while an OUTPUT is queued, error
// markers, and a mid-stream resolution change. Tests use it to assert the
// ioctl sequence and the ownership rules without hardware.

#pragma once

#include "../../src/iris/device.h"

#include <deque>
#include <functional>
#include <map>
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

namespace iris::test {

struct FakeConfig {
    bool decode_order_control = true; // kernel has the display-delay patch
    bool display_order_reorder = false; // hold every Nth completion until STOP
    unsigned reorder_depth = 2;
    unsigned capture_max = 64;
    unsigned firmware_min_capture = 4;
    unsigned coded_width = 1920, coded_height = 1088;
    unsigned visible_width = 1920, visible_height = 1080;
    unsigned stride = 1920;
    unsigned au_capacity = 2u << 20;
};

class FakeDevice final : public Device {
public:
    explicit FakeDevice(FakeConfig config = {})
        : config_(config)
    {
    }

    // ----- scripting knobs -----
    // Fail the next N OUTPUT completions with the ERROR flag.
    void inject_output_errors(unsigned count) { output_errors_ = count; }
    // Emit N firmware error markers (ERROR flag, timestamp 0) on CAPTURE.
    void inject_error_markers(unsigned count) { error_markers_ = count; }
    // Emit N corrupt completions that keep their timestamp, as a kernel with
    // packaging/kernel/0004-* does. An optional picture-type flag lets a test
    // model the AV1 hidden (show_frame=0) shape.
    void inject_timestamped_errors(unsigned count, uint32_t extra_flags = 0)
    {
        timestamped_errors_ = count;
        timestamped_error_flags_ = extra_flags;
    }
    // Model a VP9 alt-ref superframe: the next picture completes twice under
    // one token, first as an ERROR completion with no picture-type bit and no
    // payload, then normally. The firmware does this for the invisible frame
    // the translator packed a show_existing_frame into. It does not return
    // the invisible half before it has a slot for the visible one, so with
    // fewer than two CAPTURE slots queued the picture simply waits — the
    // stall a one-buffer Import window produced on the target.
    void inject_altref_superframe() { altref_superframe_ = true; }
    // Return EBUSY from START this many times.
    void inject_start_busy(unsigned count) { start_busy_ = count; }
    // After the next AU, announce a new resolution.
    void inject_resolution_change(unsigned width, unsigned height)
    {
        pending_drc_ = { width, height };
    }
    // Make the next DQBUF on CAPTURE fail with EIO.
    void inject_capture_eio() { capture_eio_ = true; }
    // Stop completing until released (models a stalled reorder window).
    void stall(bool on) { stalled_ = on; }
    // Complete the next N pictures into the second queued CAPTURE slot
    // instead of the first (a firmware that ignores QBUF order).
    void inject_misorder(unsigned count) { misorder_ = count; }
    // Complete the next picture carrying the timestamp of the AU after it
    // (a firmware whose output does not follow the OUTPUT it consumed).
    void inject_token_skew(unsigned count) { token_skew_ = count; }
    // The client fd last queued into a CAPTURE slot (DMABUF queues).
    int capture_fd(unsigned index) const { return index < capture_fds_.size() ? capture_fds_[index] : -1; }
    uint32_t capture_memory() const { return capture_memory_; }
    // By default every poll() completes one queued AU, modelling firmware
    // progress while the driver waits. Off = only explicit tick().
    void set_auto_tick(bool on) { auto_tick_ = on; }
    // Advance the firmware: complete up to `n` queued AUs.
    void tick(unsigned n = 1);

    const std::vector<std::string>& log() const { return log_; }
    std::string joined_log() const;
    unsigned capture_queued() const { return static_cast<unsigned>(capture_queue_.size()); }
    unsigned capture_allocated() const { return capture_count_; }
    bool output_streaming() const { return output_on_; }
    bool capture_streaming() const { return capture_on_; }
    int32_t control(uint32_t id) const
    {
        auto it = controls_.find(id);
        return it == controls_.end() ? -1 : it->second;
    }

    // ----- Device -----
    std::string path() const override { return "/dev/fake-iris"; }
    int fd() const override { return -1; }
    FormatInfo get_format(Queue queue) override;
    // What the driver would report, without logging the query.
    FormatInfo current_format(Queue queue);
    FormatInfo set_format(Queue queue, const FormatInfo& request) override;
    std::optional<Selection> get_compose(Queue queue) override;
    void set_crop(unsigned left, unsigned top, unsigned width, unsigned height) override
    {
        note("s_crop " + std::to_string(left) + " " + std::to_string(top) + " " + std::to_string(width) + " "
            + std::to_string(height));
        crop_ = Selection { left, top, width, height };
    }
    bool format_supported(Queue, uint32_t) override { return true; }
    std::optional<int32_t> get_control(uint32_t id) override;
    bool set_control(uint32_t id, int32_t value) override;
    unsigned request_buffers(Queue queue, unsigned count, uint32_t memory = V4L2_MEMORY_MMAP) override;
    BufferInfo query_buffer(Queue queue, unsigned index) override;
    int export_buffer(Queue queue, unsigned index) override;
    void* map_buffer(Queue queue, const BufferInfo& info) override;
    void unmap_buffer(void* mapping, unsigned length) override;
    void queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us, uint32_t flags)
        override;
    int allocate_dmabuf(unsigned size) override;
    void queue_buffer_dmabuf(Queue queue, unsigned index, int dmabuf_fd, unsigned length,
        unsigned bytesused, uint64_t timestamp_us, uint32_t flags) override;
    std::optional<Dequeued> dequeue_buffer(Queue queue) override;
    void stream(Queue queue, bool on) override;
    void set_frame_rate(Queue queue, unsigned num, unsigned den) override
    {
        note("s_parm " + std::to_string(num) + "/" + std::to_string(den));
        frame_rate_ = { num, den };
        (void)queue;
    }
    void subscribe(uint32_t) override { }
    Event dequeue_event() override;
    bool decoder_command(uint32_t cmd) override;
    bool encoder_command(uint32_t) override { return true; }
    short poll(short events, int timeout_ms) override;

private:
    struct QueuedOutput {
        unsigned index;
        uint64_t timestamp;
    };
    void note(const std::string& line) { log_.push_back(line); }
    void complete_one();

    FakeConfig config_;
    std::vector<std::string> log_;
    std::map<uint32_t, int32_t> controls_;
    Selection crop_;
    std::pair<unsigned, unsigned> frame_rate_ { 0, 0 };
    bool output_on_ = false, capture_on_ = false;
    bool source_change_pending_ = false;
    bool output_format_set_ = false;
    unsigned output_count_ = 0, capture_count_ = 0;
    std::vector<std::vector<uint8_t>> output_memory_;
    std::deque<QueuedOutput> output_queue_; // AUs the firmware has not consumed
    std::deque<unsigned> output_done_; // OUTPUT indices ready for DQBUF
    std::deque<uint32_t> output_done_flags_;
    std::deque<unsigned> capture_queue_; // free CAPTURE slots the firmware owns
    std::deque<Dequeued> capture_done_; // completions ready for DQBUF
    std::deque<QueuedOutput> reorder_hold_; // display-order: held until STOP
    bool stopped_ = false;
    bool stalled_ = false;
    bool drc_blocked_ = false; // new-resolution AU waits for CAPTURE restart
    unsigned output_errors_ = 0, error_markers_ = 0, start_busy_ = 0;
    unsigned timestamped_errors_ = 0;
    uint32_t timestamped_error_flags_ = 0;
    bool altref_superframe_ = false;
    std::optional<std::pair<unsigned, unsigned>> pending_drc_;
    bool capture_eio_ = false;
    bool auto_tick_ = true;
    unsigned decoded_since_stream_ = 0;
    uint32_t capture_memory_ = V4L2_MEMORY_MMAP;
    std::vector<int> capture_fds_;
    unsigned misorder_ = 0;
    unsigned token_skew_ = 0;
    // STOP finished with no CAPTURE slot for LAST: the next queued slot
    // returns LAST at once (iris_vb2_buf_queue does the same).
    bool last_pending_ = false;
    // The SOURCE_CHANGE boundary LAST is emitted once per change, on the
    // event itself rather than on STOP.
    bool drc_last_sent_ = false;
    // Import pool: no slot was free when the event was dequeued, so the next
    // buffer queued to CAPTURE carries the boundary LAST.
    bool drc_last_pending_ = false;
    // The queued source change is a mid-stream one, so it ends with LAST.
    bool drc_change_ = false;
    void emit_last(unsigned slot);
};

} // namespace iris::test
