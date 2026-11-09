// SPDX-License-Identifier: MIT
//
// One Iris stateful encode session per VA context. A picture is submitted and
// completed as two steps: submit() queues the raw frame, wait() takes the
// access unit. The client sees the same split: vaEndPicture submits and
// vaSyncBuffer waits. A client that never asks to wait (vaMapBuffer on the
// coded buffer is the one FFmpeg calls) gets the wait inside the map.
// The firmware owns GOP placement and the sequence headers.

#pragma once

#include "alloc.h"
#include "device.h"

#include "../codec/annexb.h"
#include "../util/encode-params.h"
#include "../util/options.h"
#include "../util/trace.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace iris {

struct EncodeConfig {
    uint32_t codec = 0; // V4L2_PIX_FMT_H264 or V4L2_PIX_FMT_HEVC
    // The context's declared size, used only as an upper bound. FFmpeg
    // aligns a context up (1920x1088) while its frames stay 1920x1080, so
    // the real raw geometry comes from the first surface (configure()).
    unsigned width = 0, height = 0;
};

struct EncodeResult {
    std::vector<uint8_t> bytes;
};

// One submitted picture: the handle a completion is claimed with. The
// timestamp is the token the firmware echoes, and it is unique per session
// because the session advances it by one frame duration per picture.
struct EncodeTicket {
    uint64_t timestamp_us = 0;
};

class EncoderSession {
public:
    EncoderSession(std::unique_ptr<Device> device, const EncodeConfig& config, const Options& options,
        const Trace& trace);
    ~EncoderSession();
    EncoderSession(const EncoderSession&) = delete;
    EncoderSession& operator=(const EncoderSession&) = delete;

    bool configured() const { return configured_; }
    const EncodeConfig& config() const { return config_; }
    // Codec hook for a requested IDR: the firmware prefixes SPS/PPS to its
    // own IDRs only, so a levered one would not be decodable from a cut.
    using PrepareAccessUnit
        = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&, std::vector<codec::NalUnit>&)>;
    // First picture only: formats, crop, controls, buffer pools, STREAMON.
    // `width`/`height` are the input surface's, which may be smaller than
    // the context the client declared.
    void configure(const EncodeParams& params, unsigned width, unsigned height, PrepareAccessUnit prepare = {});
    // Queue `dmabuf_fd` as one raw frame and return its ticket without
    // waiting. The caller keeps the DMA-BUF alive until the matching wait():
    // the firmware reads it until it completes the picture. The buffer's
    // pending write fence is waited for first, bounded at 2 s, so a client
    // that rendered into it and only flushed is encoded as it drew it. The
    // V4L2 timestamp advances by one frame duration of the session rate on
    // every picture: the firmware echoes it, and it stops producing access
    // units if the sequence ever goes backwards. A submitted picture is never
    // dropped; submit() blocks only when every input slot is still in use.
    EncodeTicket submit(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params);
    // Take the access unit for `ticket`. Completions arrive in submission
    // order, so an earlier ticket is resolved on the way. Safe to call from
    // any thread, and safe for two threads to wait on two different tickets.
    EncodeResult wait(EncodeTicket ticket);
    // The same, bounded by the client's vaSyncBuffer timeout. A timeout of
    // zero returns at once when the picture is not ready; the ticket stays
    // valid, so the client can ask again.
    EncodeResult wait_timeout(EncodeTicket ticket, uint64_t timeout_ns);
    // submit() then wait(): the synchronous shape, kept for the unit tests
    // and for a client that wants one picture at a time.
    EncodeResult encode(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params);
    // STOP, drain the tail, STREAMOFF, REQBUFS(0). Idempotent.
    void finish();
    // False once a completion failed. The failure is reported to every
    // outstanding ticket, so a client waiting on a later picture sees it.
    bool healthy() const { return healthy_; }

private:
    struct CaptureSlot {
        unsigned index = 0;
        unsigned length = 0;
        uint8_t* mapping = nullptr;
    };
    struct Pending {
        EncodeTicket ticket;
        EncodeParams params;
        bool force_idr = false;
    };

    using Clock = std::chrono::steady_clock;

    void apply_dynamic(const EncodeParams& params);
    void set_control(uint32_t id, int32_t value, const char* name);
    void set_frame_qp(int qp);
    void configure_deblocking(const EncodeParams& params);
    void take_output_slot(Clock::time_point limit);
    std::optional<Dequeued> wait_capture(Clock::time_point limit);
    // Move the oldest queued completion into `completed_`. Returns false when
    // the firmware produced nothing.
    bool complete_one();
    bool ticket_known(EncodeTicket ticket) const;
    void claim(EncodeTicket ticket);
    bool claimed(EncodeTicket ticket) const;
    int sync_timeout_ms() const;
    Clock::time_point deadline() const;
    Clock::time_point wait_deadline(uint64_t timeout_ns) const;

    std::unique_ptr<Device> device_;
    EncodeConfig config_;
    Options options_;
    Trace trace_;

    bool configured_ = false;
    bool finished_ = false;
    bool cold_ = true;
    bool healthy_ = true;
    // The node takes V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME (a kernel with the
    // REQUEST_SYNC_FRAME capability). False means the intra period is the
    // only lever a requested IDR has.
    bool sync_frame_control_ = false;
    // The firmware answered the drain marker (V4L2_ENC_CMD_STOP).
    bool drained_ = false;
    EncodeParams applied_;
    // The reconstruction surface of the last submitted picture. The encoder
    // has no reference management of its own: it codes against the picture it
    // just produced, so this is the only legal reference for the next inter
    // picture.
    uint32_t last_reconstruction_ = 0;

    std::vector<CaptureSlot> capture_;
    std::deque<unsigned> free_output_;
    // Submitted pictures whose access unit has not arrived, oldest first.
    std::deque<Pending> pending_;
    // Pictures the firmware has finished, waiting for their ticket holder.
    std::map<uint64_t, EncodeResult> completed_;
    // The newest ticket issued, and the newest one already claimed. A ticket
    // at or below `claimed_us_` was answered once; asking again (a client
    // that syncs both the surface and the coded buffer) succeeds rather than
    // reporting a ticket that no longer exists.
    uint64_t issued_us_ = 0;
    uint64_t claimed_us_ = 0;
    unsigned frames_ = 0;
    // Session frame rate, fixed in configure(), and the timestamp of the
    // last submitted picture.
    unsigned rate_num_ = 30;
    unsigned rate_den_ = 1;
    uint64_t timestamp_us_ = 0;
    // Parameter sets carried by the last access unit, for a requested IDR the
    // firmware wrote without them.
    std::vector<codec::NalUnit> parameter_sets_;
    PrepareAccessUnit prepare_;
    // Guards the queues, the slot lists and finish(). It is never held while
    // the firmware is waited on: wait() releases it across poll(), so a
    // second thread can submit while the first is blocked.
    std::mutex mutex_;
    // Wakes every waiter when a completion lands or the session finishes.
    std::condition_variable ready_;
};

} // namespace iris
