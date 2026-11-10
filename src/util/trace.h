// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace iris {

// Trace lines are an API consumed by test/iris-*-check.sh; see
// docs/architecture.md §8 before renaming a record.
class Trace {
public:
    explicit Trace(bool enabled)
        : enabled_(enabled)
    {
    }
    bool enabled() const { return enabled_; }
    // Every record ends with ` t=<ms>` since the first record of the
    // process: the checkers match substrings, so the suffix is additive.
    void operator()(const char* format, ...) const __attribute__((format(printf, 2, 3)))
    {
        if (!enabled_)
            return;
        char body[1024];
        va_list args;
        va_start(args, format);
        int length = std::vsnprintf(body, sizeof(body), format, args);
        va_end(args);
        if (length < 0)
            return;
        // One record is one write. Two fprintf() calls here let a concurrent
        // context splice its own record between the body and the timestamp,
        // which makes the checker see `publish=invisibleva end_picture` and
        // silently loses a trace vocabulary entry.
        std::lock_guard<std::mutex> lock(output_mutex());
        std::fprintf(stderr, "%.*s t=%lld\n", static_cast<int>(sizeof(body) - 1) < length
                ? static_cast<int>(sizeof(body) - 1) : length,
            body, static_cast<long long>(now_ms()));
    }

private:
    // All records go to the same stderr, so the lock is process-wide. Keeping
    // it out of the object leaves Trace copyable: sessions and encoder take a
    // Trace by value.
    static std::mutex& output_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    static int64_t now_ms()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    }
    bool enabled_;
};

} // namespace iris
