// SPDX-License-Identifier: MIT
//
// The exception-to-VAStatus mapping every entry point shares. It lives in its
// own translation unit so a test that compiles one VA source file (to reach
// the glue logic) links without the whole driver.

#include "driver.h"

#include "../util/error.h"

namespace va {

VAStatus report(VADriverContextP ctx, const char* where, const std::exception& error)
{
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    if (const auto* iris_error = dynamic_cast<const iris::Error*>(&error))
        status = iris_error->status;
    if (ctx && ctx->error_callback)
        ctx->error_callback(ctx, const_cast<char*>((std::string(where) + ": " + error.what() + "\n").c_str()));
    data(ctx).trace("va error where=%s status=%d: %s", where, status, error.what());
    return status;
}

} // namespace va
