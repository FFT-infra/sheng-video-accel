// SPDX-License-Identifier: MIT
//
// The id -> object lookups every entry point shares. They are pure functions
// over DriverData: no VA types are dereferenced beyond the id, so the tests
// that exercise the glue logic construct a DriverData and call these instead
// of opening a display.

#include "driver.h"

#include "../util/error.h"

namespace va {

std::shared_ptr<Surface> surface(DriverData& d, VASurfaceID id)
{
    auto it = d.surfaces.find(id);
    iris::require(it != d.surfaces.end(), "invalid surface", VA_STATUS_ERROR_INVALID_SURFACE);
    return it->second;
}

std::shared_ptr<Context> context(DriverData& d, VAContextID id)
{
    auto it = d.contexts.find(id);
    iris::require(it != d.contexts.end(), "invalid context", VA_STATUS_ERROR_INVALID_CONTEXT);
    return it->second;
}

std::shared_ptr<Buffer> buffer(DriverData& d, VABufferID id)
{
    auto it = d.buffers.find(id);
    iris::require(it != d.buffers.end(), "invalid buffer", VA_STATUS_ERROR_INVALID_BUFFER);
    return it->second;
}

std::shared_ptr<Context> owner_of(DriverData& d, const Surface& surface)
{
    if (surface.owner == VA_INVALID_ID)
        return nullptr;
    auto it = d.contexts.find(surface.owner);
    return it == d.contexts.end() ? nullptr : it->second;
}

} // namespace va
