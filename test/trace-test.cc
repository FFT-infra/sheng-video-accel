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
    for (auto& thread : threads)
        thread.join();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(fd);

    FILE* file = std::fopen(path, "r");
    if (!file)
        return fail("fopen");
    char line[512];
    int count = 0;
    while (std::fgets(line, sizeof(line), file)) {
        ++count;
        const std::string text(line);
        if (text.find("publish=invisibleva") != std::string::npos
            || text.find("capture token=") == std::string::npos
            || text.find("publish=invisible t=") == std::string::npos
            || text.find('\n') == std::string::npos)
            return fail("interleaved or truncated trace record");
    }
    std::fclose(file);
    std::remove(path);
    if (count != writers * records)
        return fail("record count");
    std::fprintf(stderr, "PASS %d concurrent trace records are intact\n", count);
    return 0;
}
