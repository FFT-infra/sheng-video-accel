// SPDX-License-Identifier: MIT
//
// One Iris stateful decode session per VA context: the only object that
// sequences ioctls (docs/architecture.md §4). It owns the OUTPUT and
// CAPTURE pools, associates completions to callers by token, and publishes
// through the policy the caller chose per target.

#pragma once

#include "alloc.h"
#include "device.h"
#include "publish.h"
#include "slots.h"

#include "../util/options.h"
#include "../util/trace.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace iris {

enum class SessionMode { DecodeOrder, DisplayOrder };

enum class SessionState {
    Configured, // OUTPUT streaming, waiting for the first SOURCE_CHANGE
    Streaming, // both queues live
    Draining, // STOP issued, waiting for LAST
    RestartPending, // LAST seen; START not yet issued
    Reconfiguring, // resolution change: CAPTURE torn down, waiting to re-enter Streaming
    Finished, // final drain done, nothing more accepted
    Failed, // firmware or ioctl error; every pending target failed
};

// What the caller wants done with a completed frame for one target.
//   Direct: the target takes the CAPTURE slot itself (zero copies).
//   Copy:   the completion is copied into the target's StableBuffer.
//   Import: the target's StableBuffer is queued into CAPTURE for this
//           picture and the firmware decodes straight into it (zero copies
//           for a client that pre-exports, like Chrome). Needs decode-order
//           output and a pool in Import memory; the session degrades it to
//           Copy when either is missing.
enum class Publish { Direct, Copy, Import };

// A decode target: one VA surface, as far as the session is concerned.
struct Target {
    uint64_t token = 0;
    bool pending = false;
    // Filled by the session on completion.
    FrameRef frame; // Direct: the held CAPTURE slot
    bool completed = false;
    bool error = false;
    // The picture is decoded but never displayed (AV1 show_frame=0). The
    // node returns it without pixels, and that is not a decode failure.
    bool hidden = false;
    // How many CAPTURE completions this picture produces. A VP9 alt-ref
    // superframe is one VA picture the firmware decodes as two, and it wants
    // a buffer for each half before it returns the first, so an Import pool
    // queues this target's client buffer once per completion.
    unsigned completions = 1;
    Publish publish = Publish::Direct;
    StableBuffer* destination = nullptr; // Copy / Import: where pixels go
    unsigned width = 0, height = 0;
    // VA fourcc of the target surface (NV12 or P010). The first submitted
    // target fixes the session's CAPTURE format: a VA client may create its
    // config without an RT format attribute and declare 10-bit only through
    // its surfaces (FFmpeg does).
    uint32_t fourcc = 0;
};

struct SessionConfig {
    uint32_t codec_pixelformat = 0; // V4L2_PIX_FMT_H264 / HEVC / VP9 / AV1
    uint32_t capture_fourcc = 0; // VA_FOURCC_NV12 or P010
    unsigned width = 0, height = 0;
    unsigned surface_count = 0; // the client's pool size, for CAPTURE sizing
    unsigned output_slots = 4;
    unsigned au_capacity = 0; // bytes per OUTPUT slot; 0 = derive from geometry
    // Whether the Import publish policy is qualified for this codec
    // (data/iris-codec-capabilities.json); the VA layer sets it from the
    // driver's capability probe, the session never guesses from the fourcc.
    bool import_allowed = false;
};

struct SessionStats {
    uint64_t submitted = 0, completed = 0, errors = 0, drops = 0, timeouts = 0, drains = 0, reconfigures = 0;
    // Import: a picture completed in a slot queued for another token.
    uint64_t misplaced = 0;
};

class Session {
public:
    Session(std::unique_ptr<Device> device, const SessionConfig& config, const Options& options, CopyEngine* copier,
        const Trace& trace);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    SessionMode mode() const { return mode_; }
    SessionState state() const { return state_; }
    // Whether an Import target can still be honoured: decode-order output,
    // and the CAPTURE pool (if built) is in Import memory.
    bool supports_import() const;
    // Scripted tests only: pretend the codec is not in the capability table.
    void set_import_allowed_for_test(bool allowed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.import_allowed = allowed;
    }
    CaptureMemory capture_memory() const { return pool_.capture_memory(); }
    const SessionStats& stats() const { return stats_; }
    const Layout& capture_layout() const { return pool_.capture_layout(); }
    unsigned width() const { return config_.width; }
    unsigned height() const { return config_.height; }
    // Visible rectangle reported by the decoder after SOURCE_CHANGE, or the
    // configured geometry before then.
    Selection visible() const { return visible_; }

    // Submit one complete access unit for `target`. Assigns the token,
    // copies the bytes into a free OUTPUT slot and queues it. Blocks only
    // while every OUTPUT slot is busy (servicing completions meanwhile),
    // bounded by the sync timeout. Throws iris::Error.
    void submit(const uint8_t* data, size_t size, Target& target);

    // Service completions without blocking. Safe to call from any VA entry
    // point; a no-op when nothing is ready.
    void service();

    // Block until `target` is completed or failed, within the bound. In
    // display-order mode a timeout triggers one STOP/LAST/START drain before
    // giving up. Throws Error(TIMEDOUT / DECODING_ERROR).
    void wait(Target& target);

    // The caller is about to reuse `target` for a new picture (beginPicture)
    // or destroy it. Releases the held frame and invalidates a still-pending
    // token so a late completion is dropped, never published.
    void release(Target& target);
    void release_locked(Target& target);

    // Finish the stream: flush, STOP, wait LAST. After this no submit is
    // accepted. Called from destroyContext.
    void finish();

    // Number of CAPTURE slots the firmware currently owns / the driver holds.
    unsigned capture_queued() const { return pool_.capture_queued(); }
    unsigned capture_held() const { return pool_.capture_held(); }

private:
    void configure_output();
    void configure_capture();
    void enter_streaming();
    void pump(int timeout_ms);
    void handle_events();
    void handle_output_returns();
    bool handle_capture_returns(); // returns true when LAST was seen
    void publish(Target& target, FrameRef frame, const Dequeued& completion, const char* event = nullptr);
    // Import pools: hand the firmware the next client buffer from the
    // backlog when none is in flight. The firmware picks any queued buffer
    // for the picture it finishes, so it is never given more than one to
    // pick from: lockstep is what makes token and buffer agree.
    void queue_import_next();
    static bool import_compatible(const Layout& client, const Layout& capture);
    // wait_output: block until every pre-STOP OUTPUT is recycled, which a
    // following START requires; a resolution change keeps its new-stream
    // OUTPUT queued by design and skips it.
    void drain(const char* reason, bool wait_output);
    void restart();
    void reconfigure();
    // Wait for the LAST the kernel marks on the implicit drain a SOURCE_CHANGE
    // already performed. Issuing a decoder STOP here would ask the firmware
    // for a second drain it will not complete.
    void await_drc_last();
    // Completes a deferred resolution change once no CAPTURE frame is held.
    void continue_reconfigure();
    void fail_unrecoverable_tokens();
    void fail(const char* what);
    void fail_all_pending();
    bool expired(std::chrono::steady_clock::time_point deadline) const;
    int sync_timeout_ms() const;

    std::unique_ptr<Device> device_;
    SessionConfig config_;
    Options options_;
    CopyEngine* copier_;
    Trace trace_;
    SlotPool pool_;
    SessionMode mode_ = SessionMode::DisplayOrder;
    SessionState state_ = SessionState::Configured;
    SessionStats stats_;
    Selection visible_;
    uint64_t next_token_ = 1;
    // Live tokens -> targets. A token leaves the map when it completes,
    // errors, or its target is released.
    std::map<uint64_t, Target*> pending_;
    bool source_change_ = false;
    bool saw_last_ = false;
    bool cold_ = true; // no completion yet in this session
    // The first pool was built in Mmap memory although an Import target
    // asked for it (display-order kernel, or an incompatible client
    // layout); every Import target is published as Copy from then on.
    bool import_rejected_ = false;
    // Import: pictures whose client buffer has not been queued yet, in
    // submission order. The fd is an owned duplicate: the entry outlives a
    // released target, because its picture was already submitted to the
    // OUTPUT queue and must still be given a buffer — a hole in the buffer
    // sequence shifts every later picture into a neighbour's buffer.
    //
    // `completions` is how many CAPTURE completions the picture produces and
    // `lent` how many of its buffers are already with the firmware. A VP9
    // alt-ref access unit is one VA picture the firmware decodes as two, and
    // it wants a buffer for each half before it returns the first, so the
    // entry is lent twice before the window moves on. The entry leaves the
    // backlog when it has nothing left to lend and its token has been
    // answered, which for a superframe is the second completion — never the
    // first.
    struct ImportEntry {
        uint64_t token = 0;
        int fd = -1;
        unsigned completions = 1;
        unsigned lent = 0;
    };
    std::deque<ImportEntry> import_backlog_;
    void clear_import_backlog();
    // Bumped on every completion and state transition. wait() measures its
    // bound from the last progress, not from the call: a drain or
    // reconfigure that took 500 ms is progress, not a stall.
    uint64_t progress_ = 0;
    std::mutex mutex_;
    void* dump_ = nullptr;
};

} // namespace iris
