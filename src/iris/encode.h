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
#include <set>
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
    // Return once the firmware has released this picture's input buffer. It
    // does that as soon as it has read the picture, before the access unit
    // exists; this is what vaSyncSurface on the input surface waits for. The
    // unit itself is still taken by wait()/wait_timeout() through the coded
    // buffer.
    void wait_input(EncodeTicket ticket);
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
        // The client's raw frame, queued later than it was submitted when a
        // B picture arrives before the future reference it needs: VA clients
        // submit in coding order and the firmware consumes display order, so
        // one picture waits in held_ while its predecessor is queued. The
        // client keeps the DMA-BUF alive until it waits for this ticket, and
        // wait() does not return before the picture is queued and coded.
        int dmabuf_fd = -1;
        unsigned dmabuf_length = 0;
        // The timestamp the picture was queued with. The firmware echoes it
        // into the access unit, and it rises in queue order rather than in
        // ticket order, which is what the firmware checks.
        uint64_t queue_us = 0;
    };

    using Clock = std::chrono::steady_clock;

    void apply_dynamic(const EncodeParams& params);
    // Take an input slot, apply this picture's dynamic parameters, pull the
    // IDR lever and queue the raw frame. Called with mutex_ held, in queue
    // order, which is display order when B pictures are in use.
    void queue_pending(Pending& picture);
    void set_control(uint32_t id, int32_t value, const char* name);
    void set_frame_qp(int qp);
    void configure_deblocking(const EncodeParams& params);
    void take_output_slot(Clock::time_point limit);
    // DQBUF every OUTPUT completion that is ready, freeing its input slot and
    // recording that the firmware has released that picture's buffer.
    void pump_output();
    std::optional<Dequeued> wait_capture(Clock::time_point limit);
    // Move the oldest queued completion into `completed_`. Returns false when
    // the firmware produced nothing.
    bool complete_one();
    // Ask the firmware for the pictures it still holds and collect them into
    // `completed_` until `limit`. The STOP command is per session, so it is
    // sent once; a drained session takes no further pictures. Returns true
    // when at least one picture arrived.
    bool drain_locked(Clock::time_point limit);
    // Queue the picture held in held_ when the client is waiting for the
    // display-order predecessor of it: the firmware cannot code that picture
    // without the successor in its queue, and the client waits per picture,
    // so the pair would otherwise stall until the drain.
    void flush_successor(EncodeTicket ticket);
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
    // V4L2_ENC_CMD_STOP was sent. It ends the stream: the firmware returns
    // what it holds and codes nothing after it.
    bool drain_sent_ = false;
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
    // The one picture that is waiting for its display slot while B pictures
    // are in use. Empty without them, and empty between submit() calls: a
    // picture leaves here no later than the next submit(), the drain, or a
    // wait for the picture that has to be coded before it (flush_successor).
    std::optional<Pending> held_;
    // The queue timestamp of the last queued picture. Separate from the
    // ticket timestamps, which follow submission (coding) order.
    uint64_t queue_us_ = 0;
    // Queue timestamp to ticket, for the OUTPUT release that answers
    // vaSyncSurface on the input surface.
    std::map<uint64_t, uint64_t> queued_tickets_;
    // The client's picture order count restarts at every IDR, so the session
    // rebases it on the keyframe each picture is relative to. `last_display_`
    // is the highest display position submitted so far. A client that sends
    // no order at all leaves every picture equal, which reads as submission
    // order.
    int64_t poc_base_ = 0;
    int64_t last_display_ = -1;
    // Tickets whose input buffer the firmware has returned. Pruned at each
    // claim: everything at or below the newest claimed ticket is dead.
    std::set<uint64_t> input_released_;
    // Pictures the firmware has finished, waiting for their ticket holder.
    std::map<uint64_t, EncodeResult> completed_;
    // The newest ticket issued, and the newest one already claimed. A ticket
    // at or below `claimed_us_` was answered once; asking again (a client
    // that syncs both the surface and the coded buffer) succeeds rather than
    // reporting a ticket that no longer exists.
    uint64_t issued_us_ = 0;
    uint64_t claimed_us_ = 0;
    // B pictures complete before the future reference submitted ahead of
    // them, so a claim can jump past a ticket that is still pending. The
    // watermark cannot describe that, and letting it advance would make an
    // unanswered ticket look answered; these are kept here instead. Bounded
    // like input_released_, because the ticket is only asked about soon
    // after it is claimed.
    std::set<uint64_t> claimed_out_of_order_;
    unsigned frames_ = 0;
    // Session frame rate, fixed in configure(), and the timestamp of the
    // last submitted picture. `frame_us_` is one frame at that rate: tickets
    // advance by it per submission and queue stamps by it per queued picture.
    unsigned rate_num_ = 30;
    unsigned rate_den_ = 1;
    uint64_t frame_us_ = 33333;
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
