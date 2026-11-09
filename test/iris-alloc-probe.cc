// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): the stable-buffer allocation chain.
// docs/architecture.md §3.4 promises MSM GEM cached-coherent, then MSM GEM
// write-combined, then the system DMA heap. Every rung but the last needs an
// MSM render node, and the last is only reached when the first two are
// refused, so no unit test can walk the chain.
//
// Usage: iris-alloc-probe <render-node> [no-fault|coherent|both|coherent-enomem]
//
// Run it under test/alloc-fault.so with IRIS_TEST_ALLOC_FAULT set to the same
// mode and IRIS_TEST_ALLOC_LOG pointing at a file the shim can append to:
//
//   LD_PRELOAD=build/test/alloc-fault.so
//   IRIS_TEST_ALLOC_FAULT=both IRIS_TEST_ALLOC_LOG=$PWD/alloc.log
//     build/test/iris-alloc-probe /dev/dri/renderD128 both
//
// It asserts three things per mode: the resulting MemoryOrigin, that the
// allocation log shows exactly the rungs the chain is allowed to try, and
// that the buffer is usable (a written pattern reads back after a CPU sync).
// The log assertion is what separates "fell back correctly" from "the shim
// never fired and the fast path happened to work".

#include "../src/iris/alloc.h"

#include "../src/util/error.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <linux/dma-buf.h>
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

const char* origin_name(iris::MemoryOrigin origin)
{
    switch (origin) {
    case iris::MemoryOrigin::MsmCoherent:
        return "msm-coherent";
    case iris::MemoryOrigin::MsmWriteCombined:
        return "msm-write-combined";
    case iris::MemoryOrigin::DmaHeap:
        return "dma-heap";
    case iris::MemoryOrigin::Imported:
        return "imported";
    }
    return "unknown";
}

std::vector<std::string> read_log(const char* path)
{
    std::vector<std::string> lines;
    std::ifstream log(path);
    std::string line;
    while (std::getline(log, line))
        if (!line.empty())
            lines.push_back(line);
    return lines;
}

// The rungs the chain may try, given how the MSM attempts were made to fail.
// `coherent` is the documented retry; `both` is the documented fallthrough;
// `coherent-enomem` is a real error, so the write-combined retry must not
// happen (src/iris/msm_alloc.c masks only EINVAL and EOPNOTSUPP).
std::vector<std::string> expected_log(const std::string& mode)
{
    if (mode == "coherent")
        return { "coherent-einval", "wc-ok" };
    if (mode == "both")
        return { "coherent-einval", "wc-einval" };
    if (mode == "coherent-enomem")
        return { "coherent-enomem" };
    return {};
}

// Whether an origin is the one the chain must land on for `mode`. no-fault
// is the only mode with two acceptable answers: which MSM rung answers
// depends on whether this kernel accepts MSM_BO_CACHED_COHERENT, and both
// are the documented chain. Every other mode pins one rung.
bool origin_acceptable(const std::string& mode, iris::MemoryOrigin origin)
{
    if (mode == "no-fault")
        return origin == iris::MemoryOrigin::MsmCoherent || origin == iris::MemoryOrigin::MsmWriteCombined;
    if (mode == "coherent")
        return origin == iris::MemoryOrigin::MsmWriteCombined;
    return origin == iris::MemoryOrigin::DmaHeap;
}

// A written pattern that survives a CPU sync and a readback is the only
// cheap proof that the fallback memory is real memory the VPU can also be
// handed. The layout is the CAPTURE layout, so the same buffer is what the
// driver would queue.
bool buffer_round_trips(iris::StableBuffer& buffer)
{
    const iris::Layout& layout = buffer.layout();
    uint8_t* mapping = buffer.map();
    if (!mapping)
        return false;
    if (!iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW))
        return false;
    // Chroma, not luma: allocate_stable() writes black luma and 0x80 chroma
    // itself, so a luma pattern would also match an untouched buffer.
    const unsigned offset = layout.chroma_offset() + 64;
    if (offset + 64 > layout.size)
        return false;
    for (unsigned i = 0; i < 64; ++i)
        mapping[offset + i] = static_cast<uint8_t>(0x40 + i);
    iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    bool same = true;
    for (unsigned i = 0; i < 64; ++i)
        same = same && mapping[offset + i] == static_cast<uint8_t>(0x40 + i);
    iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    return same;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <render-node> [no-fault|coherent|both|coherent-enomem]\n", argv[0]);
        return 2;
    }
    const std::string node = argv[1];
    const std::string mode = argc == 3 ? argv[2] : "no-fault";
    const char* log_path = std::getenv("IRIS_TEST_ALLOC_LOG");
    const char* injected = std::getenv("IRIS_TEST_ALLOC_FAULT");

    // A mode argument that the shim was not told about would compare an
    // expected chain against a log the shim never wrote. Refuse that instead
    // of reporting a pass for the wrong reason.
    const bool expect_injection = mode != "no-fault";
    if (expect_injection && (!injected || mode != injected)) {
        std::fprintf(stderr,
            "mode=%s but IRIS_TEST_ALLOC_FAULT=%s; run under LD_PRELOAD with both set to the same value\n",
            mode.c_str(), injected ? injected : "(unset)");
        return 2;
    }
    if (expect_injection && !log_path) {
        std::fprintf(stderr, "IRIS_TEST_ALLOC_LOG is required for mode=%s\n", mode.c_str());
        return 2;
    }
    if (log_path)
        unlink(log_path);

    const int render_fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        std::perror(("open " + node).c_str());
        return 2;
    }

    const iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, 1280, 720);
    std::unique_ptr<iris::StableBuffer> buffer;
    try {
        buffer = iris::allocate_stable(render_fd, layout);
    } catch (const iris::Error& error) {
        std::printf("FAIL allocate_stable threw status=%#x: %s\n", error.status, error.what());
        close(render_fd);
        return 1;
    }
    close(render_fd);

    const iris::MemoryOrigin origin = buffer->origin();
    std::printf("origin=%s\n", origin_name(origin));
    check(origin_acceptable(mode, origin), "allocation chain lands on the expected rung");
    check(buffer->size() == layout.size, "buffer covers the CAPTURE layout");
    check(buffer_round_trips(*buffer), "buffer holds a written pattern across a CPU sync");

    if (log_path) {
        const std::vector<std::string> actual = read_log(log_path);
        const std::vector<std::string> expected = expected_log(mode);
        std::printf("log:");
        for (const auto& line : actual)
            std::printf(" %s", line.c_str());
        std::printf("\n");
        check(actual == expected, "the chain tried exactly the allowed rungs");
    } else {
        check(mode == "no-fault", "no allocation log requested outside the injected modes");
    }

    std::printf("%s node=%s mode=%s origin=%s\n", failures ? "FAIL" : "PASS", node.c_str(), mode.c_str(),
        origin_name(origin));
    return failures ? 1 : 0;
}
