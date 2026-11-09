// SPDX-License-Identifier: MIT
//
// Scripted sequences against the FakeDevice. Every case asserts an
// invariant from docs/architecture.md §10 or an ordering fact from §4.

#include "fake_device.h"

#include "../../src/iris/session.h"
#include "../../src/util/error.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <sys/stat.h>
}
#include <functional>
#include <string>
#include <vector>

using namespace iris;
using iris::test::FakeConfig;
using iris::test::FakeDevice;

namespace {

int failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                      \
            ++failures;                                                                                        \
        }                                                                                                      \
    } while (0)

#define CHECK_THROWS(expression, expected_status)                                                             \
    do {                                                                                                       \
        bool threw = false;                                                                                    \
        try {                                                                                                  \
            expression;                                                                                        \
        } catch (const Error& error) {                                                                         \
            threw = true;                                                                                      \
            if (error.status != (expected_status)) {                                                           \
                std::fprintf(stderr, "  FAIL %s:%d: wrong status %d (%s)\n", __FILE__, __LINE__,             \
                    error.status, error.what());                                                               \
                ++failures;                                                                                    \
            }                                                                                                  \
        }                                                                                                      \
        if (!threw) {                                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: expected throw\n", __FILE__, __LINE__);                      \
            ++failures;                                                                                        \
        }                                                                                                      \
    } while (0)

Options fast_options()
{
    Options options;
    options.sync_timeout_ms = 60;
    options.sync_timeout_overridden = true; // no 30 s cold start in tests
    options.trace = std::getenv("V4L2_VA_TRACE") != nullptr;
    return options;
}

SessionConfig config(unsigned surfaces = 16)
{
    SessionConfig c;
    c.codec_pixelformat = V4L2_PIX_FMT_H264;
    c.capture_fourcc = VA_FOURCC_NV12;
    c.width = 1920;
    c.height = 1080;
    c.surface_count = surfaces;
    // The VA layer sets this from the capability table; the scripted cases
    // exercise the session's own rules.
    c.import_allowed = true;
    return c;
}

struct Harness {
    FakeDevice* fake;
    std::unique_ptr<Session> session;
    std::unique_ptr<CopyEngine> copier;
    std::vector<uint8_t> au = std::vector<uint8_t>(4096, 0x42);

    explicit Harness(FakeConfig fc = {}, Options options = fast_options(), SessionConfig sc = config())
    {
        auto device = std::make_unique<FakeDevice>(fc);
        fake = device.get();
        copier = make_cpu_copy();
        session = std::make_unique<Session>(std::move(device), sc, options, copier.get(), Trace(options.trace));
    }
    void submit(Target& target) { session->submit(au.data(), au.size(), target); }
};

bool log_has(const FakeDevice& fake, const char* needle)
{
    return fake.joined_log().find(needle) != std::string::npos;
}

size_t log_index(const FakeDevice& fake, const char* needle)
{
    const auto& log = fake.log();
    for (size_t i = 0; i < log.size(); ++i)
        if (log[i].find(needle) != std::string::npos)
            return i;
    return static_cast<size_t>(-1);
}

// --- bring-up ------------------------------------------------------------

void test_open_sequence()
{
    Harness h;
    const auto& fake = *h.fake;
    // §4: OUTPUT S_FMT, display-delay controls, REQBUFS, STREAMON before any
    // CAPTURE call; CAPTURE only after SOURCE_CHANGE.
    CHECK(log_index(fake, "S_FMT OUTPUT") < log_index(fake, "S_CTRL DISPLAY_DELAY"));
    CHECK(log_index(fake, "S_CTRL DISPLAY_DELAY_ENABLE") < log_index(fake, "STREAMON OUTPUT"));
    CHECK(!log_has(fake, "S_FMT CAPTURE"));
    CHECK(h.session->mode() == SessionMode::DecodeOrder);
    CHECK(h.session->state() == SessionState::Configured);

    Target t;
    h.submit(t);
    CHECK(log_index(fake, "QBUF OUTPUT 0") < log_index(fake, "EVENT SOURCE_CHANGE"));
    CHECK(log_index(fake, "EVENT SOURCE_CHANGE") < log_index(fake, "G_FMT CAPTURE"));
    CHECK(log_index(fake, "G_FMT CAPTURE") < log_index(fake, "REQBUFS CAPTURE"));
    CHECK(log_index(fake, "REQBUFS CAPTURE") < log_index(fake, "STREAMON CAPTURE"));
    // The driver's own format is the one the firmware will decode at; the
    // session must not second-guess it with an S_FMT when it already matches
    // (docs/architecture.md §4: the DRC resume publishes the geometry the
    // driver derived from the bitstream).
    CHECK(!log_has(fake, "S_FMT CAPTURE"));
    CHECK(h.session->state() == SessionState::Streaming);
    // Pool: 16 surfaces + 4 headroom -> clamp to 32 minimum.
    CHECK(fake.capture_allocated() == 32);
    CHECK(h.session->visible().width == 1920 && h.session->visible().height == 1080);
}

void test_display_order_fallback()
{
    FakeConfig fc;
    fc.decode_order_control = false;
    Harness h(fc);
    CHECK(h.session->mode() == SessionMode::DisplayOrder);
    CHECK(!log_has(*h.fake, "S_CTRL"));
}

void test_pool_sizing_respects_kernel_max()
{
    FakeConfig fc;
    fc.capture_max = 32;
    Harness h(fc, fast_options(), config(40)); // wants 44
    Target t;
    h.submit(t);
    CHECK(h.fake->capture_allocated() == 32);
    CHECK(h.session->state() == SessionState::Streaming);
}

// --- ownership -----------------------------------------------------------

void test_direct_publish_holds_slot_until_release()
{
    Harness h;
    Target t;
    h.submit(t);
    h.session->wait(t);
    CHECK(t.completed && !t.error && !t.pending);
    CHECK(t.frame != nullptr);
    // Invariant 2: a held slot is never queued. 32 slots, one held.
    CHECK(h.session->capture_held() == 1);
    CHECK(h.session->capture_queued() == 31);
    h.session->release(t);
    h.session->service();
    CHECK(h.session->capture_held() == 0);
    CHECK(h.session->capture_queued() == 32);
}

void test_copy_publish_releases_slot_immediately()
{
    Harness h;
    Layout layout = stable_layout(VA_FOURCC_NV12, 1920, 1080);
    // The CPU engine needs a real DMA-BUF-like fd; a memfd stands in.
    const int fd = memfd_create("stable", 0);
    CHECK(fd >= 0 && ftruncate(fd, layout.size) == 0);
    StableBuffer stable(fd, layout.size, MemoryOrigin::DmaHeap, layout);
    Target t;
    t.publish = Publish::Copy;
    t.destination = &stable;
    h.submit(t);
    h.session->wait(t);
    CHECK(t.completed && !t.frame);
    CHECK(h.session->capture_held() == 0);
    h.session->service();
    CHECK(h.session->capture_queued() == 32);
}

void test_late_completion_after_release_is_dropped()
{
    // Invariant 1: a released target's token must never be published.
    Harness h;
    h.fake->set_auto_tick(false);
    Target a, b;
    h.submit(a);
    // Bring-up consumed the first AU; b is queued and not yet completed.
    h.fake->tick(1); // completes a
    h.session->service();
    h.submit(b);
    h.session->release(b); // client moved on before completion
    h.fake->tick(1); // firmware still completes b's token
    h.session->service();
    CHECK(!b.completed && !b.pending && b.frame == nullptr);
    CHECK(h.session->stats().drops == 1);
    CHECK(h.session->capture_held() == 1); // only a
}

void test_reuse_pending_target_invalidates_old_token()
{
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    Target t;
    h.submit(t); // token 2, pending
    h.submit(t); // reused before completion: token 3
    CHECK(t.pending && t.token == 3);
    h.fake->tick(2);
    h.session->service();
    CHECK(t.completed && t.frame != nullptr);
    CHECK(h.session->stats().drops == 1); // token 2 dropped
}

// --- error paths ---------------------------------------------------------

void test_firmware_error_marker_is_never_published()
{
    // Invariant 8.
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    h.fake->inject_error_markers(1);
    Target b;
    h.submit(b);
    h.fake->tick(1);
    h.session->service();
    CHECK(b.completed && !b.error);
    CHECK(h.session->stats().errors == 1);
    CHECK(h.session->stats().completed == 2);
    h.session->service();
    // The marker's slot went back to the firmware.
    CHECK(h.session->capture_queued() + h.session->capture_held() == 32);
}

void test_output_error_fails_target()
{
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    h.fake->inject_output_errors(1);
    Target b;
    h.submit(b);
    h.fake->tick(1);
    h.session->service();
    CHECK(!b.pending && b.error);
    CHECK_THROWS(h.session->wait(b), VA_STATUS_ERROR_DECODING_ERROR);
}

void test_timestamped_corrupt_completion_fails_target_at_once()
{
    // With packaging/kernel/0004-* an error completion keeps its timestamp,
    // so the failed picture is named and its surface fails on the spot. The
    // stock behavior — no token, wait for the drain — is covered by
    // test_firmware_error_marker_is_never_published. A rejected picture
    // carries a picture-type bit (the truncated HEVC in TEST-RESULTS came
    // back as 0x4049, ERROR|KEYFRAME); that bit is what separates it from
    // the invisible half of a VP9 alt-ref superframe.
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    CHECK(a.completed && !a.error);
    h.fake->inject_timestamped_errors(1, V4L2_BUF_FLAG_KEYFRAME);
    Target b;
    h.submit(b);
    h.fake->tick(1);
    h.session->service();
    CHECK(!b.pending && b.error);
    CHECK(h.session->stats().errors == 1);
    // The target is failed before any drain: a wait reports the decode error,
    // not a timeout, and no STOP was needed to get there.
    CHECK_THROWS(h.session->wait(b), VA_STATUS_ERROR_DECODING_ERROR);
    CHECK(!log_has(*h.fake, "DECODER_CMD STOP"));
}

void test_timestamped_hidden_picture_is_published_not_failed()
{
    // An AV1 alt-ref is a decoded picture the stream never shows. It carries
    // the ERROR flag, no picture-type bit and no payload. On a kernel with
    // 0004 it also carries its timestamp, and it must still be published
    // rather than failed.
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    h.fake->inject_timestamped_errors(1);
    Target b;
    b.hidden = true;
    h.submit(b);
    h.fake->tick(1);
    h.session->service();
    CHECK(b.completed && !b.error);
    CHECK(h.session->stats().errors == 0);
}

void test_vp9_altref_superframe_is_one_picture()
{
    // A VP9 alt-ref access unit is one VA picture that the firmware decodes
    // as two: an invisible frame that completes with the ERROR flag, no
    // picture-type bit and no payload, then the visible frame under the same
    // token. On a kernel with 0004 the invisible completion carries its
    // timestamp, so it looks exactly like a named corrupt picture unless the
    // shape says otherwise. It must not fail the target: the pixels arrive
    // on the second completion, and dropping the target turns the real frame
    // into a misplaced drop.
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    CHECK(a.completed && !a.error);
    h.fake->inject_altref_superframe();
    Target b;
    b.completions = 2;
    h.submit(b);
    h.fake->tick(1);
    h.session->service();
    CHECK(b.completed && !b.error);
    CHECK(h.session->stats().errors == 0);
    CHECK(h.session->stats().drops == 0);
}

void test_eio_fails_session_and_all_pending()
{
    Harness h;
    h.fake->set_auto_tick(false);
    Target a;
    h.submit(a);
    h.fake->tick(1);
    h.session->service();
    Target b, c;
    h.submit(b);
    h.submit(c);
    h.fake->inject_capture_eio();
    CHECK_THROWS(h.session->wait(b), VA_STATUS_ERROR_DECODING_ERROR);
    CHECK(h.session->state() == SessionState::Failed);
    CHECK(b.error && c.error && !b.pending && !c.pending);
    Target d;
    CHECK_THROWS(h.submit(d), VA_STATUS_ERROR_DECODING_ERROR);
}

// --- timeouts and drains ---------------------------------------------------

void test_decode_order_timeout_is_terminal()
{
    Harness h;
    Target a;
    h.submit(a);
    h.session->wait(a);
    h.fake->stall(true);
    Target b;
    h.submit(b);
    CHECK_THROWS(h.session->wait(b), VA_STATUS_ERROR_TIMEDOUT);
    CHECK(!log_has(*h.fake, "DECODER_CMD STOP")); // no drain in decode order
    CHECK(b.error && !b.pending);
    // The token was invalidated: a late completion is dropped.
    h.fake->stall(false);
    h.fake->tick(1);
    h.session->service();
    CHECK(h.session->stats().drops == 1);
    CHECK(h.session->state() == SessionState::Streaming);
}

void test_display_order_timeout_drains_then_recovers()
{
    FakeConfig fc;
    fc.decode_order_control = false;
    fc.display_order_reorder = true;
    fc.reorder_depth = 2;
    Harness h(fc);
    Target a;
    h.submit(a);
    // With a reorder depth of 2 and no further input, a is withheld; the
    // sync timeout must STOP, see LAST, START, and a completes.
    h.session->wait(a);
    CHECK(a.completed && !a.error);
    CHECK(h.session->stats().drains == 1);
    CHECK(log_index(*h.fake, "DECODER_CMD STOP") < log_index(*h.fake, "DECODER_CMD START"));
    CHECK(h.session->state() == SessionState::Streaming);
    // Decoding continues after the restart.
    Target b, c, d;
    h.submit(b);
    h.submit(c);
    h.submit(d);
    h.session->wait(b);
    CHECK(b.completed);
}

void test_start_ebusy_is_retried()
{
    FakeConfig fc;
    fc.decode_order_control = false;
    fc.display_order_reorder = true;
    Harness h(fc);
    h.fake->inject_start_busy(3);
    Target a;
    h.submit(a);
    h.session->wait(a);
    CHECK(a.completed);
    CHECK(h.session->state() == SessionState::Streaming);
    int busy = 0;
    for (const auto& line : h.fake->log())
        if (line.find("START (EBUSY)") != std::string::npos)
            ++busy;
    CHECK(busy == 3);
}

void test_finish_drains_and_refuses_submit()
{
    Harness h;
    Target a, b;
    h.submit(a);
    h.submit(b);
    h.session->finish();
    CHECK(h.session->state() == SessionState::Finished);
    CHECK(log_has(*h.fake, "DECODER_CMD STOP"));
    CHECK(!b.pending);
    Target c;
    CHECK_THROWS(h.submit(c), VA_STATUS_ERROR_OPERATION_FAILED);
}

// --- resolution change -----------------------------------------------------

void test_resolution_change_rebuilds_capture()
{
    Harness h;
    Target a;
    h.submit(a);
    h.session->wait(a);
    h.session->release(a); // nothing held across the change
    h.fake->inject_resolution_change(1280, 720);
    Target b;
    h.submit(b);
    h.session->wait(b);
    CHECK(b.completed);
    CHECK(h.session->stats().reconfigures == 1);
    CHECK(h.session->width() == 1280 && h.session->height() == 720);
    const auto& fake = *h.fake;
    // SOURCE_CHANGE is the implicit drain, so the boundary LAST arrives with
    // the event and no decoder STOP follows: STREAMOFF CAPTURE -> REQBUFS 0 ->
    // S_FMT -> REQBUFS -> STREAMON. A STOP here would ask the firmware for a
    // second drain it never finishes, which is what stranded a real AV1
    // stream at its resolution change.
    CHECK(!log_has(fake, "DECODER_CMD STOP"));
    const size_t change = log_index(fake, "EVENT SOURCE_CHANGE");
    const size_t off = log_index(fake, "STREAMOFF CAPTURE");
    CHECK(change < off);
    CHECK(log_has(fake, "REQBUFS CAPTURE 0"));
    // OUTPUT was never stopped.
    CHECK(!log_has(fake, "STREAMOFF OUTPUT"));
    CHECK(h.session->state() == SessionState::Streaming);
}

void test_resolution_change_keeps_held_frames()
{
    // A Direct client still displaying an old-geometry frame must not lose
    // the session and must not have to release it first: the change is
    // usually triggered by that same client submitting the new-resolution
    // picture, so waiting for a release would wait on the caller. vb2 orphans
    // the old buffer, so the pool is rebuilt under the holder.
    Harness h;
    Target a;
    h.submit(a);
    h.session->wait(a); // a holds a slot
    h.fake->inject_resolution_change(1280, 720);
    Target b;
    h.submit(b);
    h.fake->tick(1); // firmware parses the new-resolution AU
    h.session->service(); // and the session sees SOURCE_CHANGE
    CHECK(h.session->state() == SessionState::Streaming);
    CHECK(b.pending); // its OUTPUT is still queued: not lost
    CHECK(a.frame != nullptr); // the held frame survived untouched
    h.session->wait(b);
    CHECK(b.completed && !b.error);
    CHECK(h.session->width() == 1280);
    h.session->release(a);
    CHECK(h.session->state() == SessionState::Streaming);
}

// --- misc ------------------------------------------------------------------

void test_au_larger_than_slot_is_rejected()
{
    Harness h;
    std::vector<uint8_t> huge(3u << 20, 0);
    Target t;
    CHECK_THROWS(h.session->submit(huge.data(), huge.size(), t), VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
}

void test_visible_rectangle_prefers_kernel_compose_when_sane()
{
    // The kernel reports the firmware crop through COMPOSE. A 1080-line
    // stream decodes into 1088 coded rows; the visible rectangle must be
    // 1080, and a nonsense compose (larger than coded) must not be trusted.
    FakeConfig fc;
    fc.coded_height = 1088;
    fc.visible_height = 1080;
    Harness h(fc);
    Target t;
    h.submit(t);
    CHECK(h.session->visible().height == 1080);
    CHECK(h.session->capture_layout().height == 1088);
    CHECK(h.session->height() == 1080);

    FakeConfig bad;
    bad.coded_height = 1088;
    bad.visible_height = 4000; // corrupt compose: exceeds coded size
    Harness g(bad, fast_options(), config());
    Target u;
    g.submit(u);
    CHECK(g.session->visible().height == 1080); // falls back to the client's geometry
}

void test_stable_layout()
{
    // iris_buffer.c: pitch to 128, luma rows to 32, chroma rows to 16,
    // total to 4 KiB. 720x405 -> 768 x 416 luma, 768 x 208 chroma.
    const Layout l = stable_layout(VA_FOURCC_NV12, 720, 405);
    CHECK(l.stride == 768);
    CHECK(l.storage_height == 416);
    CHECK(l.chroma_offset() == 768 * 416);
    CHECK(l.size == ((768 * 416 + 768 * 208 + 4095) & ~4095u));
    // 1080p: what the kernel reports as sizeimage for NV12.
    const Layout f = stable_layout(VA_FOURCC_NV12, 1920, 1080);
    CHECK(f.stride == 1920 && f.storage_height == 1088);
    CHECK(f.size == 1920 * 1088 + 1920 * 544);
    const Layout p = stable_layout(VA_FOURCC_P010, 1920, 1080);
    CHECK(p.stride == 3840);
    CHECK(p.storage_height == 1088);
}

// ---- Import publish (client-owned CAPTURE buffers) ----

// The session duplicates the client fd before queueing it, so identity is
// compared by the underlying object, not the descriptor number.
bool same_buffer(int a, int b)
{
    struct stat sa = {}, sb = {};
    return a >= 0 && b >= 0 && fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_ino == sb.st_ino;
}

struct ClientBuffer {
    int fd = -1;
    std::unique_ptr<StableBuffer> stable;
    explicit ClientBuffer(const Layout& layout)
    {
        fd = memfd_create("client", 0);
        CHECK(fd >= 0 && ftruncate(fd, layout.size) == 0);
        stable = std::make_unique<StableBuffer>(fd, layout.size, MemoryOrigin::DmaHeap, layout);
    }
};

Layout capture_layout_of(const FakeConfig& fc)
{
    Layout l = stable_layout(VA_FOURCC_NV12, fc.coded_width, fc.coded_height);
    return l;
}

void test_import_queues_client_buffer_per_picture()
{
    Harness h;
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout), b(layout);
    Target ta, tb;
    ta.publish = tb.publish = Publish::Import;
    ta.destination = a.stable.get();
    tb.destination = b.stable.get();
    h.submit(ta);
    CHECK(h.session->capture_memory() == CaptureMemory::Import);
    CHECK(log_has(*h.fake, "REQBUFS CAPTURE 32 DMABUF"));
    CHECK(h.fake->capture_memory() == V4L2_MEMORY_DMABUF);
    // CAPTURE streams before the first picture's buffer goes in: the kernel
    // only clears the DRC boundary sub-state in iris_process_streamon_output,
    // and a CAPTURE QBUF that arrives first is completed as an empty LAST
    // instead of being handed to the firmware.
    CHECK(h.fake->joined_log().find("STREAMON CAPTURE") < h.fake->joined_log().find("QBUF CAPTURE DMABUF 0 fd="));
    h.session->wait(ta);
    CHECK(ta.completed && !ta.error && ta.frame == nullptr);
    CHECK(same_buffer(h.fake->capture_fd(0), a.fd));
    h.submit(tb);
    h.session->wait(tb);
    CHECK(tb.completed && !tb.error);
    CHECK(h.session->capture_held() == 0);
    // Only the picture in flight ever occupies a slot: nothing is
    // pre-queued and the firmware never has two buffers to choose from.
    CHECK(h.session->capture_queued() == 0);
    CHECK(h.session->stats().misplaced == 0);
    CHECK(h.session->supports_import());
    // Reusing a buffer picks the slot that already carries it.
    h.submit(ta);
    h.session->wait(ta);
    CHECK(same_buffer(h.fake->capture_fd(0), a.fd));
    CHECK(h.session->stats().misplaced == 0);
}

void test_import_misplaced_completion_is_an_error()
{
    Harness h;
    h.fake->set_auto_tick(false);
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout), b(layout);
    Target ta, tb;
    ta.publish = tb.publish = Publish::Import;
    ta.destination = a.stable.get();
    tb.destination = b.stable.get();
    h.submit(ta);
    h.submit(tb);
    // Lockstep: only the first picture's buffer is with the firmware.
    CHECK(h.session->capture_queued() == 1);
    CHECK(same_buffer(h.fake->capture_fd(0), a.fd));
    // The firmware finishes that buffer carrying the second AU's token: the
    // client's surface for b holds a's pixels. Never published.
    h.fake->inject_token_skew(1);
    h.fake->tick(1);
    CHECK_THROWS(h.session->wait(tb), VA_STATUS_ERROR_DECODING_ERROR);
    CHECK(tb.error && !tb.pending);
    // The buffer's owner is compromised too: it holds foreign pixels.
    CHECK_THROWS(h.session->wait(ta), VA_STATUS_ERROR_DECODING_ERROR);
    CHECK(ta.error && !ta.pending);
    CHECK(h.session->stats().misplaced == 1);
    CHECK(h.session->stats().errors == 2);
    CHECK(h.session->stats().completed == 0);
    CHECK(h.session->state() == SessionState::Streaming); // the session itself goes on
}

void test_import_rejected_on_display_order_kernel()
{
    FakeConfig fc;
    fc.decode_order_control = false;
    Harness h(fc);
    const Layout layout = capture_layout_of(fc);
    ClientBuffer a(layout);
    Target ta;
    ta.publish = Publish::Import;
    ta.destination = a.stable.get();
    CHECK(!h.session->supports_import());
    h.submit(ta);
    CHECK(h.session->capture_memory() == CaptureMemory::Mmap);
    CHECK(log_has(*h.fake, "REQBUFS CAPTURE 32") && !log_has(*h.fake, "DMABUF"));
    h.session->wait(ta);
    // Degraded to Copy: the pixels reached the client buffer anyway.
    CHECK(ta.completed && !ta.error && ta.publish == Publish::Copy && ta.frame == nullptr);
}

void test_import_rejected_for_an_unqualified_codec()
{
    Harness h;
    h.session->set_import_allowed_for_test(false);
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout);
    Target ta;
    ta.publish = Publish::Import;
    ta.destination = a.stable.get();
    CHECK(!h.session->supports_import());
    h.submit(ta);
    CHECK(h.session->capture_memory() == CaptureMemory::Mmap);
    h.session->wait(ta);
    CHECK(ta.completed && ta.publish == Publish::Copy);
}

void test_import_rejected_on_layout_mismatch()
{
    Harness h;
    Layout wrong = capture_layout_of({});
    wrong.stride += 128; // a client whose pitch the firmware cannot write
    wrong.size = wrong.stride * (wrong.storage_height + wrong.storage_height / 2);
    ClientBuffer a(wrong);
    Target ta;
    ta.publish = Publish::Import;
    ta.destination = a.stable.get();
    h.submit(ta);
    CHECK(h.session->capture_memory() == CaptureMemory::Mmap);
    CHECK(!h.session->supports_import());
    h.session->wait(ta);
    CHECK(ta.completed && ta.publish == Publish::Copy);
}

void test_import_drain_lends_scratch_for_last()
{
    Harness h;
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout);
    Target ta;
    ta.publish = Publish::Import;
    ta.destination = a.stable.get();
    h.submit(ta);
    h.session->wait(ta);
    h.session->finish();
    CHECK(h.session->state() == SessionState::Finished);
    CHECK(log_has(*h.fake, "DECODER_CMD STOP"));
    // LAST needed a buffer; the session lent one rather than a client's.
    const int last_fd = h.fake->capture_fd(0);
    CHECK(last_fd >= 0 && last_fd != a.fd);
}

void test_import_resolution_change_rebuilds_import_pool()
{
    Harness h;
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout);
    Target ta;
    ta.publish = Publish::Import;
    ta.destination = a.stable.get();
    h.submit(ta);
    h.session->wait(ta);
    h.fake->inject_resolution_change(1280, 720);
    FakeConfig small;
    small.coded_width = 1280;
    small.coded_height = 720;
    ClientBuffer b(capture_layout_of(small));
    Target tb;
    tb.publish = Publish::Import;
    tb.destination = b.stable.get();
    h.submit(tb);
    h.session->wait(tb);
    CHECK(tb.completed && !tb.error);
    CHECK(h.session->stats().reconfigures == 1);
    CHECK(h.session->capture_memory() == CaptureMemory::Import);
    CHECK(log_has(*h.fake, "REQBUFS CAPTURE 0 DMABUF"));
}

void test_import_altref_gets_a_buffer_per_completion()
{
    // The firmware decodes a VP9 alt-ref superframe as two pictures and does
    // not return the invisible half before it has a slot for the visible
    // one. An Import pool that lent the picture one buffer therefore stalled
    // on every alt-ref (~500 ms measured on the target, TEST-RESULTS.md
    // 2026-09-22). Both halves land in the same client buffer, so lending it
    // twice keeps the firmware's choice empty while giving it the room it
    // needs.
    Harness h;
    h.fake->set_auto_tick(false);
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout), b(layout);
    Target ta, tb;
    ta.publish = tb.publish = Publish::Import;
    ta.destination = a.stable.get();
    tb.destination = b.stable.get();
    h.submit(ta);
    h.fake->tick(1);
    h.session->wait(ta);
    h.fake->inject_altref_superframe();
    tb.completions = 2;
    h.submit(tb);
    // Two slots, both carrying the same client buffer, and nothing of the
    // next picture.
    CHECK(h.session->capture_queued() == 2);
    CHECK(same_buffer(h.fake->capture_fd(0), b.fd));
    CHECK(same_buffer(h.fake->capture_fd(1), b.fd));
    h.fake->tick(1);
    h.session->service();
    CHECK(tb.completed && !tb.error);
    CHECK(h.session->stats().errors == 0);
    CHECK(h.session->stats().misplaced == 0);
    CHECK(h.session->capture_queued() == 0);
}

void test_import_altref_does_not_stall_the_next_picture()
{
    // With one buffer lent per picture, an alt-ref held the window: the next
    // picture's buffer could not be queued until the first completion came
    // back, and that completion needed the second half's slot. Nothing
    // completes, so the session times out. Two buffers for the superframe
    // and one picture at a time is what removes the deadlock.
    Harness h;
    h.fake->set_auto_tick(false);
    const Layout layout = capture_layout_of({});
    ClientBuffer a(layout), b(layout), c(layout);
    Target ta, tb, tc;
    ta.publish = tb.publish = tc.publish = Publish::Import;
    ta.destination = a.stable.get();
    tb.destination = b.stable.get();
    tc.destination = c.stable.get();
    h.submit(tc);
    h.fake->tick(1);
    h.session->wait(tc);
    h.fake->inject_altref_superframe();
    ta.completions = 2;
    h.submit(ta);
    h.submit(tb);
    // Still only the alt-ref's two buffers: the next picture waits its turn.
    CHECK(h.session->capture_queued() == 2);
    CHECK(same_buffer(h.fake->capture_fd(0), a.fd));
    CHECK(same_buffer(h.fake->capture_fd(1), a.fd));
    h.fake->tick(1);
    h.session->service();
    CHECK(ta.completed && !ta.error);
    // Its buffers came back, so the next picture's may go in.
    CHECK(h.session->capture_queued() == 1);
    CHECK(same_buffer(h.fake->capture_fd(0), b.fd) || same_buffer(h.fake->capture_fd(1), b.fd));
    h.fake->tick(1);
    h.session->service();
    CHECK(tb.completed && !tb.error);
    CHECK(h.session->stats().misplaced == 0);
}


} // namespace

int main()
{
    struct Case {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        { "open sequence", test_open_sequence },
        { "display-order fallback", test_display_order_fallback },
        { "pool sizing respects kernel max", test_pool_sizing_respects_kernel_max },
        { "direct publish holds slot until release", test_direct_publish_holds_slot_until_release },
        { "copy publish releases slot immediately", test_copy_publish_releases_slot_immediately },
        { "late completion after release is dropped", test_late_completion_after_release_is_dropped },
        { "reuse of pending target invalidates old token", test_reuse_pending_target_invalidates_old_token },
        { "firmware error marker is never published", test_firmware_error_marker_is_never_published },
        { "output error fails target", test_output_error_fails_target },
        { "timestamped corrupt completion fails target at once",
            test_timestamped_corrupt_completion_fails_target_at_once },
        { "timestamped hidden picture is published", test_timestamped_hidden_picture_is_published_not_failed },
        { "VP9 alt-ref superframe is one picture", test_vp9_altref_superframe_is_one_picture },
        { "EIO fails session and all pending", test_eio_fails_session_and_all_pending },
        { "decode-order timeout is terminal", test_decode_order_timeout_is_terminal },
        { "display-order timeout drains then recovers", test_display_order_timeout_drains_then_recovers },
        { "START EBUSY is retried", test_start_ebusy_is_retried },
        { "finish drains and refuses submit", test_finish_drains_and_refuses_submit },
        { "resolution change rebuilds capture", test_resolution_change_rebuilds_capture },
        { "resolution change keeps held frames", test_resolution_change_keeps_held_frames },
        { "AU larger than slot is rejected", test_au_larger_than_slot_is_rejected },
        { "visible rectangle prefers sane kernel compose", test_visible_rectangle_prefers_kernel_compose_when_sane },
        { "stable layout", test_stable_layout },
        { "import queues the client buffer per picture", test_import_queues_client_buffer_per_picture },
        { "import misplaced completion is an error", test_import_misplaced_completion_is_an_error },
        { "import rejected on a display-order kernel", test_import_rejected_on_display_order_kernel },
        { "import rejected for an unqualified codec", test_import_rejected_for_an_unqualified_codec },
        { "import rejected on a layout mismatch", test_import_rejected_on_layout_mismatch },
        { "import drain lends a scratch buffer for LAST", test_import_drain_lends_scratch_for_last },
        { "import resolution change rebuilds the import pool", test_import_resolution_change_rebuilds_import_pool },
        { "import alt-ref gets a buffer per completion", test_import_altref_gets_a_buffer_per_completion },
        { "import alt-ref does not stall the next picture", test_import_altref_does_not_stall_the_next_picture },
    };
    int ran = 0;
    for (const auto& c : cases) {
        const int before = failures;
        try {
            c.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "  FAIL %s: uncaught %s\n", c.name, error.what());
            ++failures;
        }
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", c.name);
        ++ran;
    }
    std::printf("%d cases, %d failures\n", ran, failures);
    return failures == 0 ? 0 : 1;
}
