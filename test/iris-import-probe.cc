// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): does the Iris CAPTURE queue deliver
// picture k into the k-th queued DMA-BUF? This is the acceptance test
// docs/architecture.md §9 requires before the Import policy may exist.
//
// The input is an H.264 Annex-B stream in which every picture's luma is a
// flat grey encoding its position in decode order (value = 16 + 2*(k%100));
// the probe splits the stream on AUD NALs, queues one client DMA-BUF per
// access unit *before* the unit (the order the driver uses), rotates over
// `surfaces` buffers like a browser pool, and reads the grey back out of
// every completed buffer. A completion whose timestamp does not match the
// grey in its buffer is a misplacement. `skip=` punches holes: AUs that
// get no client buffer, modelling a client that released the surface.
//
// The association check is only meaningful with one client buffer in flight
// (`surfaces=1`). With several buffers queued, the firmware completes them in
// its own order, not QBUF order: on sheng with 12 access units and 16 slots,
// the driver dequeued slots 15,13,12,14,11,10,9,8,7,6,4,5 against a QBUF
// order of 15,14,13,12,11,10,9,8,7,6,5,4 (strace round, 2026-11-19), while
// every buffer still held the picture its timestamp named. That is the
// property the Import policy avoids by queueing exactly one picture at a
// time; `surfaces>1` measures the firmware's order rather than asserting it.
//
// Usage: iris-import-probe <node> <stream.264> [surfaces=N] [skip=K1,K2]
//        [expect=FILE]   one expected grey per access unit, in decode order
//                        (compute with ffprobe packet order); without it the
//                        stream must be B-free so that decode order equals
//                        display order.

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace {

int xioctl(int fd, unsigned long request, void* arg, const char* name)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    if (r == -1 && errno != EAGAIN && errno != ENOENT && errno != ENODATA)
        std::fprintf(stderr, "ioctl %s: %s\n", name, std::strerror(errno));
    return r;
}

int allocate_heap(uint32_t size)
{
    const int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0)
        return -1;
    dma_heap_allocation_data data = {};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    const int r = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap);
    return r < 0 ? -1 : static_cast<int>(data.fd);
}

std::vector<std::pair<size_t, size_t>> split_access_units(const std::vector<uint8_t>& bytes)
{
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < bytes.size(); ++i)
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 && (bytes[i + 3] & 0x1f) == 9)
            starts.push_back(i);
    std::vector<std::pair<size_t, size_t>> units;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t end = i + 1 < starts.size() ? starts[i + 1] : bytes.size();
        units.emplace_back(starts[i], end - starts[i]);
    }
    return units;
}

int qbuf(int vfd, uint32_t type, unsigned index, uint32_t memory, int fd, unsigned length,
    unsigned bytesused, uint64_t timestamp_us)
{
    v4l2_plane plane = {};
    v4l2_buffer buffer = {};
    buffer.type = type;
    buffer.index = index;
    buffer.memory = memory;
    buffer.field = V4L2_FIELD_NONE;
    buffer.length = 1;
    buffer.m.planes = &plane;
    plane.bytesused = bytesused;
    if (memory == V4L2_MEMORY_DMABUF) {
        plane.m.fd = fd;
        plane.length = length;
    }
    buffer.timestamp.tv_sec = static_cast<time_t>(timestamp_us / 1000000);
    buffer.timestamp.tv_usec = static_cast<suseconds_t>(timestamp_us % 1000000);
    return xioctl(vfd, VIDIOC_QBUF, &buffer, "QBUF");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <node> <stream.264> [surfaces=N] [skip=K1,K2]\n", argv[0]);
        return 2;
    }
    unsigned surfaces = 8;
    std::vector<unsigned> holes;
    std::vector<int> expected_grey;
    std::string dump_path;
    for (int i = 3; i < argc; ++i) {
        if (std::sscanf(argv[i], "surfaces=%u", &surfaces) == 1)
            continue;
        if (std::strncmp(argv[i], "skip=", 5) == 0)
            for (const char* p = argv[i] + 5; *p;) {
                char* end = nullptr;
                holes.push_back(std::strtoul(p, &end, 10));
                p = (*end == ',') ? end + 1 : end;
            }
        if (std::strncmp(argv[i], "dump=", 5) == 0) {
            dump_path = argv[i] + 5;
            continue;
        }
        if (std::strncmp(argv[i], "expect=", 7) == 0) {
            std::ifstream exp(argv[i] + 7);
            if (!exp) {
                std::fprintf(stderr, "cannot open %s\n", argv[i] + 7);
                return 2;
            }
            int v;
            while (exp >> v)
                expected_grey.push_back(v);
        }
    }
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    const std::vector<uint8_t> stream((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto units = split_access_units(stream);
    std::printf("access units: %zu surfaces=%u holes=%zu\n", units.size(), surfaces, holes.size());
    if (units.size() < 8)
        return 2;
    auto is_hole = [&](unsigned k) { return std::find(holes.begin(), holes.end(), k) != holes.end(); };

    const int vfd = open(argv[1], O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (vfd < 0) {
        std::fprintf(stderr, "open: %s\n", std::strerror(errno));
        return 2;
    }
    v4l2_event_subscription sub = {};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    xioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE_EVENT");

    v4l2_format out = {};
    out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    out.fmt.pix_mp.width = 1920;
    out.fmt.pix_mp.height = 1080;
    out.fmt.pix_mp.num_planes = 1;
    out.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
    if (xioctl(vfd, VIDIOC_S_FMT, &out, "S_FMT OUTPUT") < 0)
        return 2;
    v4l2_control delay = { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY, 0 };
    v4l2_control delay_on = { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1 };
    const bool decode_order = xioctl(vfd, VIDIOC_S_CTRL, &delay, "S_CTRL delay") == 0
        && xioctl(vfd, VIDIOC_S_CTRL, &delay_on, "S_CTRL delay_on") == 0;
    std::printf("decode-order control: %s\n", decode_order ? "yes" : "NO");
    if (!decode_order) {
        std::fprintf(stderr, "this probe requires a decode-order kernel module\n");
        return 2;
    }

    constexpr unsigned kOutputSlots = 4;
    v4l2_requestbuffers out_req = {};
    out_req.count = kOutputSlots;
    out_req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &out_req, "REQBUFS OUTPUT") < 0)
        return 2;
    std::vector<void*> out_map(kOutputSlots);
    for (unsigned i = 0; i < kOutputSlots; ++i) {
        v4l2_plane plane = {};
        v4l2_buffer b = {};
        b.type = out_req.type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = &plane;
        if (xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF OUTPUT") < 0)
            return 2;
        out_map[i] = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, plane.m.mem_offset);
    }
    uint32_t out_type = out_req.type;
    if (xioctl(vfd, VIDIOC_STREAMON, &out_type, "STREAMON OUTPUT") < 0)
        return 2;
    // First AU: raises SOURCE_CHANGE once the OUTPUT queue is live.
    std::memcpy(out_map[0], stream.data() + units[0].first, units[0].second);
    if (qbuf(vfd, out_req.type, 0, V4L2_MEMORY_MMAP, -1, 0, units[0].second, 0) < 0)
        return 2;
    bool saw_source_change = false;
    for (int i = 0; i < 500 && !saw_source_change; ++i) {
        v4l2_event event = {};
        if (xioctl(vfd, VIDIOC_DQEVENT, &event, "DQEVENT") == 0 && event.type == V4L2_EVENT_SOURCE_CHANGE)
            saw_source_change = true;
        else
            usleep(10000);
    }
    std::printf("source change: %s\n", saw_source_change ? "yes" : "NO");
    if (!saw_source_change)
        return 2;

    v4l2_format cap = {};
    cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(vfd, VIDIOC_G_FMT, &cap, "G_FMT CAPTURE") < 0)
        return 2;
    cap.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    cap.fmt.pix_mp.num_planes = 1;
    if (xioctl(vfd, VIDIOC_S_FMT, &cap, "S_FMT CAPTURE") < 0)
        return 2;
    const unsigned width = cap.fmt.pix_mp.width, height = cap.fmt.pix_mp.height;
    const unsigned stride = cap.fmt.pix_mp.plane_fmt[0].bytesperline;
    const unsigned sizeimage = cap.fmt.pix_mp.plane_fmt[0].sizeimage;
    std::printf("capture %ux%u stride=%u size=%u\n", width, height, stride, sizeimage);

    constexpr unsigned kCaptureSlots = 16;
    v4l2_requestbuffers cap_req = {};
    cap_req.count = kCaptureSlots;
    cap_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(vfd, VIDIOC_REQBUFS, &cap_req, "REQBUFS CAPTURE DMABUF") < 0) {
        std::fprintf(stderr, "kernel refused a DMABUF CAPTURE queue\n");
        return 2;
    }
    std::vector<int> client(surfaces, -1);
    std::vector<uint8_t*> client_map(surfaces, nullptr);
    for (unsigned i = 0; i < surfaces; ++i) {
        client[i] = allocate_heap(sizeimage);
        if (client[i] < 0)
            return 2;
        client_map[i] = static_cast<uint8_t*>(
            mmap(nullptr, sizeimage, PROT_READ | PROT_WRITE, MAP_SHARED, client[i], 0));
        std::memset(client_map[i], 0xEE, sizeimage);
    }
    // Fill the free list so that back() is always the LOWEST free index:
    // the driver queues client buffers into the lowest free slot, and this
    // probe must reproduce that to test whether the firmware consumes the
    // queue in index order or in QBUF order.
    std::vector<unsigned> free_slots;
    for (unsigned i = kCaptureSlots; i-- > 0;)
        free_slots.push_back(kCaptureSlots - 1 - i);
    std::vector<int> slot_fd(kCaptureSlots, -1);
    std::vector<uint64_t> slot_token(kCaptureSlots, 0);
    std::vector<bool> surface_busy(surfaces, false);
    unsigned max_in_flight = 0, in_flight = 0;

    // The driver queues the first pictures' buffers, then streams CAPTURE on.
    auto queue_client_for = [&](unsigned k) -> bool {
        if (is_hole(k))
            return true;
        if (free_slots.empty() || surface_busy[k % surfaces])
            return false;
        const unsigned slot = free_slots.back();
        free_slots.pop_back();
        const int fd = client[k % surfaces];
        if (qbuf(vfd, cap_req.type, slot, V4L2_MEMORY_DMABUF, fd, sizeimage, 0, 0) != 0) {
            free_slots.push_back(slot);
            return false;
        }
        slot_fd[slot] = fd;
        slot_token[slot] = k;
        surface_busy[k % surfaces] = true;
        ++in_flight;
        max_in_flight = std::max(max_in_flight, in_flight);
        return true;
    };
    if (!is_hole(0) && !queue_client_for(0))
        return 2;
    uint32_t cap_type = cap_req.type;
    if (xioctl(vfd, VIDIOC_STREAMON, &cap_type, "STREAMON CAPTURE") < 0)
        return 2;

    unsigned next_au = 1, next_out = 1, completed = 0, misplaced = 0, holes_skipped = 0;
    unsigned assoc_wrong = 0, content_wrong = 0;
    std::vector<int> shift_hist;
    std::vector<unsigned> completed_tokens;
    const uint64_t deadline = static_cast<uint64_t>(units.size()) * 200; // ms budget
    const auto start = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };

    unsigned free_out = kOutputSlots - 1; // AU 0 is in flight
    while (completed < units.size() - holes.size() && elapsed_ms() < static_cast<int64_t>(deadline)) {
        while (free_out > 0 && next_au < units.size()) {
            // Queue the client buffer for this AU first, then the bitstream;
            // the same surface cannot be queued twice at once.
            if (is_hole(next_au))
                ++holes_skipped;
            else if (!queue_client_for(next_au))
                break;
            const unsigned oslot = next_out % kOutputSlots;
            std::memcpy(out_map[oslot], stream.data() + units[next_au].first, units[next_au].second);
            if (qbuf(vfd, out_req.type, oslot, V4L2_MEMORY_MMAP, -1, 0, units[next_au].second, next_au) != 0)
                break;
            ++next_out;
            ++next_au;
            --free_out;
        }
        pollfd pfd = { vfd, POLLIN | POLLPRI | POLLOUT, 0 };
        poll(&pfd, 1, 10);
        // Drain completions.
        for (;;) {
            v4l2_plane plane = {};
            v4l2_buffer b = {};
            b.type = cap_req.type;
            b.memory = V4L2_MEMORY_DMABUF;
            b.length = 1;
            b.m.planes = &plane;
            if (xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF CAPTURE") < 0)
                break;
            const unsigned idx = b.index;
            const uint64_t token = b.timestamp.tv_sec * 1000000ull + b.timestamp.tv_usec;
            const int fd = slot_fd[idx];
            int grey = -1;
            if (fd >= 0) {
                const unsigned ci = static_cast<unsigned>(std::find(client.begin(), client.end(), fd) - client.begin());
                if (ci < surfaces) {
                    const uint8_t* p = client_map[ci];
                    grey = p[(height / 2) * stride + (width / 2)];
                }
            }
            ++completed;
            completed_tokens.push_back(static_cast<unsigned>(token));
            const int expected = expected_grey.empty()
                ? -1
                : (token < expected_grey.size() ? expected_grey[token] : -1);
            const bool assoc_bad = slot_token[idx] != token;
            const bool content_bad = expected >= 0 && (grey < 0 || std::abs(grey - expected) > 6);
            if (!dump_path.empty())
                std::printf("%llu %u %d\n", static_cast<unsigned long long>(token), idx, grey);
            if (assoc_bad)
                ++assoc_wrong;
            if (content_bad)
                ++content_wrong;
            if ((assoc_bad || content_bad) && misplaced < 16) {
                ++misplaced;
                std::printf("  BAD completion token=%llu buffer=%u queued_for=%llu grey=%d expected=%d%s%s\n",
                    static_cast<unsigned long long>(token), idx,
                    static_cast<unsigned long long>(slot_token[idx]), grey, expected,
                    assoc_bad ? " [association]" : "", content_bad ? " [content]" : "");
            } else if (assoc_bad || content_bad) {
                ++misplaced;
            }
            shift_hist.push_back(static_cast<int>(slot_token[idx]) - static_cast<int>(token));
            if (fd >= 0) {
                const unsigned si = static_cast<unsigned>(std::find(client.begin(), client.end(), fd) - client.begin());
                if (si < surfaces)
                    surface_busy[si] = false;
            }
            --in_flight;
            slot_fd[idx] = -1;
            slot_token[idx] = 0;
            free_slots.insert(free_slots.begin(), idx); // keep back() = lowest
        }
        // Recycle OUTPUT slots.
        for (;;) {
            v4l2_plane plane = {};
            v4l2_buffer b = {};
            b.type = out_req.type;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = &plane;
            if (xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF OUTPUT") < 0)
                break;
            ++free_out;
        }
        std::fflush(stdout);
    }
    bool ordered = true;
    for (size_t i = 1; i < completed_tokens.size(); ++i)
        if (completed_tokens[i] < completed_tokens[i - 1])
            ordered = false;
    std::printf("completions=%u misplaced=%u assoc_wrong=%u content_wrong=%u holes=%u max_in_flight=%u "
                "tokens_in_order=%s elapsed=%lldms\n",
        completed, misplaced, assoc_wrong, content_wrong, holes_skipped, max_in_flight, ordered ? "yes" : "no",
        static_cast<long long>(elapsed_ms()));
    {
        int hist[9] = {};
        int other = 0;
        for (int d : shift_hist)
            if (d >= -4 && d <= 4)
                ++hist[d + 4];
            else
                ++other;
        std::printf("slot_token - token: ");
        for (int d = -4; d <= 4; ++d)
            if (hist[d + 4])
                std::printf("%+d:%d ", d, hist[d + 4]);
        std::printf("other:%d\n", other);
    }
    if (!completed_tokens.empty())
        std::printf("first completions: ");
    for (size_t i = 0; i < std::min<size_t>(completed_tokens.size(), 24); ++i)
        std::printf("%u ", completed_tokens[i]);
    std::printf("\n");
    xioctl(vfd, VIDIOC_STREAMOFF, &cap_type, "STREAMOFF CAPTURE");
    xioctl(vfd, VIDIOC_STREAMOFF, &out_type, "STREAMOFF OUTPUT");
    const unsigned expected_completions = static_cast<unsigned>(units.size());
    if (assoc_wrong || content_wrong || completed < expected_completions)
        return 1;
    return 0;
}
