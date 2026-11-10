// SPDX-License-Identifier: MIT

// A trace record is one line: body + ` t=<ms>`. The checkers match
// `publish=invisible` at end of line, so two fprintf() calls per record let a
// concurrent context splice `publish=invisibleva end_picture` into the log and
// silently lose a vocabulary entry. Exercise concurrent writers from several
// Trace copies, as sessions and the encoder do.

#include "../src/util/trace.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

int fail(const char* what)
{
    std::fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

} // namespace

int main()
{
    char path[] = "trace-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return fail("mkstemp");
    int saved = dup(STDERR_FILENO);
    if (saved < 0 || dup2(fd, STDERR_FILENO) < 0)
        return fail("dup2");

    constexpr int writers = 8;
    constexpr int records = 200;
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w) {
        threads.emplace_back([w] {
            iris::Trace trace(true);
            for (int i = 0; i < records; ++i)
                trace("capture token=%d index=%d flags=%#x publish=invisible", w * records + i, i, 0x4040);
        });
    }
    // A fixed 1024-byte body truncated long records; the free-text
    // `import rejected: <why>` record must survive with its tail intact.
    const std::string padding(1200, 'x');
    threads.emplace_back([&padding] {
        iris::Trace trace(true);
        trace("import rejected: %s (client stride=%u chroma=%u size=%u; capture stride=%u chroma=%u size=%u)",
            padding.c_str(), 1280u, 942080u, 1413120u, 896u, 430080u, 647168u);
    });
    for (auto& thread : threads)
        thread.join();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(fd);

    FILE* file = std::fopen(path, "r");
    if (!file)
        return fail("fopen");
    char line[2048];
    int count = 0;
    int long_records = 0;
    while (std::fgets(line, sizeof(line), file)) {
        ++count;
        const std::string text(line);
        if (text.find('\n') == std::string::npos)
            return fail("unterminated trace record");
        if (text.find("import rejected: ") != std::string::npos) {
            ++long_records;
            if (text.find("capture stride=896 chroma=430080 size=647168) t=") == std::string::npos)
                return fail("long trace record lost its tail");
            continue;
        }
        if (text.find("publish=invisibleva") != std::string::npos
            || text.find("capture token=") == std::string::npos
            || text.find("publish=invisible t=") == std::string::npos)
            return fail("interleaved or truncated trace record");
    }
    std::fclose(file);
    std::remove(path);
    if (count != writers * records + 1 || long_records != 1)
        return fail("record count");
    std::fprintf(stderr, "PASS %d concurrent trace records are intact\n", count);
    return 0;
}
