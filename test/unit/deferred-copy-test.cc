// SPDX-License-Identifier: MIT
//
// The copy engine must stay unbuilt until something copies. GPU copy starts
// EGL, which costs about 0.1 s on SM8550, and a Direct or Import client never
// copies: building it in vaCreateContext charged every such client for a
// backend it would not use.

#include "fake_device.h"

#include "../../src/iris/publish.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
}
#include <va/va.h>

using namespace iris;
using iris::test::FakeDevice;

namespace {

int failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                          \
            ++failures;                                                                                        \
        }                                                                                                      \
    } while (0)

class CountingCopy final : public CopyEngine {
public:
    explicit CountingCopy(int* live)
        : live_(live)
    {
        ++*live_;
    }
    ~CountingCopy() override { --*live_; }

    const char* name() const override { return "counting"; }
    void copy(StableBuffer&, Frame&) override { ++copies; }
    void forget(uint64_t id) override { forgotten.push_back(id); }

    int copies = 0;
    std::vector<uint64_t> forgotten;

private:
    int* live_;
};

Layout layout()
{
    Layout l;
    l.fourcc = VA_FOURCC_NV12;
    l.width = 64;
    l.height = 64;
    l.stride = 64;
    l.storage_height = 64;
    l.data_offset = 0;
    l.size = 64 * 64 * 3 / 2;
    return l;
}

int memfd(unsigned size)
{
    const int fd = memfd_create("deferred-copy", 0);
    CHECK(fd >= 0 && ftruncate(fd, size) == 0);
    return fd;
}

struct Harness {
    FakeDevice device;
    Layout l = layout();
    // Both objects close the fd they are given.
    StableBuffer destination { memfd(l.size), l.size, MemoryOrigin::DmaHeap, l };
    Frame source { device, 0, memfd(l.size), l.size };

    std::unique_ptr<CopyEngine> engine;
    int live = 0;
    CountingCopy* backend = nullptr;

    Harness()
    {
        source.layout() = l;
        engine = make_deferred_copy([this] {
            auto copy = std::make_unique<CountingCopy>(&live);
            backend = copy.get();
            return copy;
        });
    }
};

void test_construction_builds_nothing()
{
    Harness h;
    CHECK(h.live == 0);
    CHECK(h.backend == nullptr);
}

void test_name_before_the_first_copy()
{
    Harness h;
    CHECK(std::strcmp(h.engine->name(), "deferred") == 0);
    CHECK(h.live == 0);
}

void test_forget_before_the_first_copy()
{
    Harness h;
    h.engine->forget(7);
    h.engine->forget(8);
    CHECK(h.live == 0);
    CHECK(std::strcmp(h.engine->name(), "deferred") == 0);
}

void test_first_copy_builds_the_backend_once()
{
    Harness h;
    h.engine->copy(h.destination, h.source);
    CHECK(h.live == 1);
    CHECK(h.backend != nullptr && h.backend->copies == 1);
    h.engine->copy(h.destination, h.source);
    CHECK(h.live == 1);
    CHECK(h.backend->copies == 2);
    CHECK(std::strcmp(h.engine->name(), "counting") == 0);
}

void test_forget_reaches_the_backend_after_a_copy()
{
    Harness h;
    h.engine->copy(h.destination, h.source);
    h.engine->forget(7);
    CHECK(h.backend->forgotten.size() == 1 && h.backend->forgotten[0] == 7);
    CHECK(h.live == 1);
}

} // namespace

int main()
{
    test_construction_builds_nothing();
    test_name_before_the_first_copy();
    test_forget_before_the_first_copy();
    test_first_copy_builds_the_backend_once();
    test_forget_reaches_the_backend_after_a_copy();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("deferred-copy: ok\n");
    return 0;
}
