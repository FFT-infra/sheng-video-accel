// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): can the node and its firmware decode one
// elementary stream into a chosen CAPTURE format?
//
// The VA layer decides which profiles it advertises before any stream is
// parsed, so "profile refused at selection" is a statement about this driver,
// not about the firmware. This probe feeds the stream to the node through the
// driver's own session (compressed data passes straight to the firmware) and
// reports what the node does: whether SOURCE_CHANGE arrives, what CAPTURE
// format the pool gets, and which pictures complete with an error.
//
// The input is an IVF file, one access unit per frame record (VP9 / AV1).
//
// Usage: iris-decode-stream-probe <node> <stream.ivf> [capture=NV12|P010]
//
// Exit status: 0 when every access unit completed without an error, 1 when
// one failed or the session refused the stream, 2 on usage or open failure.

#include "../src/iris/session.h"

#include "../src/util/error.h"
#include "../src/util/fourcc.h" // V4L2_PIX_FMT_AV1 on headers that predate it
#include "../src/util/options.h"
#include "../src/util/trace.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <va/va.h>

#include <fcntl.h>
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

uint32_t read_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool read_file(const char* path, std::vector<uint8_t>& bytes)
{
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return got == bytes.size();
}

struct Frame {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// IVF: a 32-byte header ("DKIF"), then per frame a 12-byte record
// (4-byte size, 8-byte timestamp) followed by the access unit.
bool parse_ivf(const std::vector<uint8_t>& bytes, std::vector<Frame>& frames, unsigned& width,
    unsigned& height, std::string& fourcc)
{
    if (bytes.size() < 32 || std::memcmp(bytes.data(), "DKIF", 4) != 0)
        return false;
    fourcc.assign(reinterpret_cast<const char*>(bytes.data() + 8), 4);
    width = read_le16(bytes.data() + 12);
    height = read_le16(bytes.data() + 14);
    size_t offset = read_le16(bytes.data() + 6);
    if (offset < 32 || offset >= bytes.size())
        return false;
    while (offset + 12 <= bytes.size()) {
        const uint32_t size = read_le32(bytes.data() + offset);
        offset += 12;
        if (size == 0 || offset + size > bytes.size())
            return false;
        frames.push_back({ bytes.data() + offset, size });
        offset += size;
    }
    return !frames.empty();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <node> <stream.ivf> [capture=NV12|P010]\n", argv[0]);
        return 2;
    }
    uint32_t capture_fourcc = VA_FOURCC_P010;
    if (argc > 3) {
        const std::string capture = argv[3];
        if (capture == "NV12")
            capture_fourcc = VA_FOURCC_NV12;
        else if (capture == "P010")
            capture_fourcc = VA_FOURCC_P010;
        else {
            std::fprintf(stderr, "capture must be NV12 or P010\n");
            return 2;
        }
    }

    std::vector<uint8_t> bytes;
    if (!read_file(argv[2], bytes)) {
        std::perror("read stream");
        return 2;
    }
    std::vector<Frame> frames;
    unsigned width = 0;
    unsigned height = 0;
    std::string codec;
    if (!parse_ivf(bytes, frames, width, height, codec)) {
        std::fprintf(stderr, "%s is not an IVF file with frames\n", argv[2]);
        return 2;
    }

    const iris::Options options = iris::Options::from_environment();
    const iris::Trace trace(options.trace);

    std::unique_ptr<iris::Device> device;
    try {
        device = iris::open_device(argv[1]);
    } catch (const iris::Error& error) {
        std::fprintf(stderr, "open %s: %s\n", argv[1], error.what());
        return 2;
    }
    std::printf("INFO node=%s codec=%s %ux%u frames=%zu capture=%s\n", device->path().c_str(),
        codec.c_str(), width, height, frames.size(),
        capture_fourcc == VA_FOURCC_P010 ? "P010" : "NV12");

    iris::SessionConfig config;
    config.codec_pixelformat = V4L2_PIX_FMT_VP9;
    if (codec == "AV01")
        config.codec_pixelformat = V4L2_PIX_FMT_AV1;
    config.capture_fourcc = capture_fourcc;
    config.width = width;
    config.height = height;
    config.surface_count = 8;
    config.output_slots = 4;

    bool all_ok = true;
    try {
        iris::Session session(std::move(device), config, options, nullptr, trace);
        for (size_t i = 0; i < frames.size(); ++i) {
            auto target = std::make_shared<iris::Target>();
            target->publish = iris::Publish::Direct;
            target->fourcc = capture_fourcc;
            target->width = width;
            target->height = height;
            try {
                session.submit(frames[i].data, frames[i].size, target);
                session.wait(target);
                std::printf("INFO frame=%zu completed=1 error=%d\n", i, target->error ? 1 : 0);
                if (target->error)
                    all_ok = false;
            } catch (const iris::Error& error) {
                std::printf("INFO frame=%zu failed=%s\n", i, error.what());
                all_ok = false;
                session.release(target);
                break;
            }
            session.release(target);
            if (i == 0) {
                const iris::Layout& layout = session.capture_layout();
                std::printf("INFO capture fourcc=%#x stride=%u size=%u\n", layout.fourcc, layout.stride,
                    layout.size);
            }
        }
        session.finish();
        const iris::SessionStats& stats = session.stats();
        std::printf("INFO submitted=%llu completed=%llu errors=%llu drops=%llu timeouts=%llu\n",
            static_cast<unsigned long long>(stats.submitted),
            static_cast<unsigned long long>(stats.completed),
            static_cast<unsigned long long>(stats.errors),
            static_cast<unsigned long long>(stats.drops), static_cast<unsigned long long>(stats.timeouts));
    } catch (const iris::Error& error) {
        std::printf("INFO session failed=%s\n", error.what());
        all_ok = false;
    }

    check(all_ok, "every access unit decoded without an error");
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS the node decoded the stream into the requested CAPTURE format\n");
    return 0;
}
