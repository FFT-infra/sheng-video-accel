// SPDX-License-Identifier: MIT

#include "options.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace iris {

namespace {

const char* env(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

std::optional<long> env_long(const char* name)
{
    const char* value = env(name);
    if (!value)
        return std::nullopt;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return std::nullopt;
    return parsed;
}

} // namespace

Options Options::from_environment()
{
    Options options;
    if (const char* path = env("LIBVA_V4L2_VIDEO_PATH"))
        options.video_path = path;
    if (const char* path = env("LIBVA_V4L2_ENCODER_PATH"))
        options.encoder_path = path;
    options.trace = env("V4L2_VA_TRACE") != nullptr;
    if (auto timeout = env_long("V4L2_VA_SYNC_TIMEOUT_MS")) {
        options.sync_timeout_ms = static_cast<int>(std::clamp(*timeout, 50L, 60000L));
        options.sync_timeout_overridden = true;
    }
    if (const char* copy = env("V4L2_VA_COPY"))
        options.copy_mode = std::strcmp(copy, "cpu") == 0 ? CopyMode::Cpu : CopyMode::Gpu;
    if (const char* publish = env("V4L2_VA_PUBLISH")) {
        if (std::strcmp(publish, "copy") == 0)
            options.publish = PublishOverride::Copy;
        else if (std::strcmp(publish, "direct") == 0)
            options.publish = PublishOverride::Direct;
        else if (std::strcmp(publish, "import") == 0)
            options.publish = PublishOverride::Import;
    }
    if (const char* dump = env("V4L2_VA_DUMP"))
        options.dump_dir = dump;
    return options;
}

} // namespace iris
