// SPDX-License-Identifier: MIT
//
// The calls GStreamer's VA filter makes that FFmpeg's does not. Each is legal
// by va.h and was answered wrongly, so vapostproc and vacompositor either
// never registered or never produced a picture:
//
//   - vaQueryImageFormats with num_formats left at zero (gst_va_filter_open);
//   - a VideoProc vaCreateContext with no picture size (the same function);
//   - a blend state on every composed layer, the bottom one included
//     (gst_va_filter_compose);
//   - vaGetImage before the first write into a fresh surface (the allocator's
//     map of an image it could not derive).

#include "../../src/va/driver.h"

#include <cstring>
#include <iostream>
#include <memory>

extern "C" {
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_vpp.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

constexpr uint32_t kPoison = 0xAAAAAAAA;
constexpr VAConfigID kProcConfig = 1;
constexpr VAConfigID kDecodeConfig = 2;
constexpr VASurfaceID kSource = 10;
constexpr VASurfaceID kTarget = 11;

struct Driver {
    va::DriverData d;
    VADriverContext ctx;
    Driver()
    {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pDriverData = &d;
        d.next_id = 100;
        d.configs[kProcConfig] = va::Config { VAProfileNone, VAEntrypointVideoProc, VA_RT_FORMAT_YUV420 };
        d.configs[kDecodeConfig] = va::Config { VAProfileH264High, VAEntrypointVLD, VA_RT_FORMAT_YUV420 };
        for (VASurfaceID id : { kSource, kTarget }) {
            auto s = std::make_shared<va::Surface>();
            s->width = 64;
            s->height = 64;
            d.surfaces[id] = s;
        }
    }
};

void test_image_formats_ignore_the_incoming_count(bool p010, int expected)
{
    Driver t;
    t.d.capabilities.p010 = p010;
    VAImageFormat formats[va::kMaxImageFormats];
    std::memset(formats, 0xAA, sizeof(formats));
    int count = 0;
    check(va::queryImageFormats(&t.ctx, formats, &count) == VA_STATUS_SUCCESS, "image format query");
    check(count == expected, "the count is the number of formats");
    check(formats[0].fourcc == VA_FOURCC_NV12, "a zero count on entry still gets the list");
    check(formats[expected - 1].fourcc == VA_FOURCC_RGBA, "the list is written to its last entry");
    for (unsigned i = static_cast<unsigned>(expected); i < va::kMaxImageFormats; ++i)
        check(formats[i].fourcc == kPoison, "nothing is written past the count");
}

void test_videoproc_context_needs_no_geometry()
{
    Driver t;
    VAContextID id = VA_INVALID_ID;
    check(va::createContext(&t.ctx, kProcConfig, 0, 0, 0, nullptr, 0, &id) == VA_STATUS_SUCCESS,
        "a VideoProc context opens without a picture size");
    check(id != VA_INVALID_ID && t.d.contexts.count(id) == 1, "and is in the table");
    check(va::destroyContext(&t.ctx, id) == VA_STATUS_SUCCESS, "and closes");

    id = VA_INVALID_ID;
    check(va::createContext(&t.ctx, kProcConfig, -1, 64, 0, nullptr, 0, &id) == VA_STATUS_ERROR_INVALID_PARAMETER,
        "a negative size is still refused");
    check(va::createContext(&t.ctx, kDecodeConfig, 0, 0, 0, nullptr, 0, &id) == VA_STATUS_ERROR_INVALID_PARAMETER,
        "a decode context still needs its picture size");
    check(t.d.contexts.empty(), "a refused context leaves no entry");
}

// Submits one pipeline buffer the way the compositor does: create, render,
// destroy, with the blend state on the caller's stack.
VAStatus render_layer(Driver& t, VAContextID context, uint32_t flags, float alpha)
{
    VABlendState blend = {};
    blend.flags = flags;
    blend.global_alpha = alpha;
    VAProcPipelineParameterBuffer p = {};
    p.surface = kSource;
    p.blend_state = &blend;
    VABufferID buffer = VA_INVALID_ID;
    check(va::createBuffer(&t.ctx, context, VAProcPipelineParameterBufferType, sizeof(p), 1, &p, &buffer)
            == VA_STATUS_SUCCESS,
        "pipeline buffer");
    const VAStatus status = va::renderPicture(&t.ctx, context, &buffer, 1);
    va::destroyBuffer(&t.ctx, buffer);
    return status;
}

void test_bottom_layer_may_carry_an_idle_blend_state()
{
    Driver t;
    VAContextID context = VA_INVALID_ID;
    check(va::createContext(&t.ctx, kProcConfig, 0, 0, 0, nullptr, 0, &context) == VA_STATUS_SUCCESS, "context");
    check(va::beginPicture(&t.ctx, context, kTarget) == VA_STATUS_SUCCESS, "begin");
    check(render_layer(t, context, 0, 1.0f) == VA_STATUS_SUCCESS,
        "a bottom layer whose blend state sets no flag is the main pass");
    check(render_layer(t, context, VA_BLEND_GLOBAL_ALPHA, 0.5f) == VA_STATUS_SUCCESS, "a layer blends onto it");
    check(render_layer(t, context, 0, 1.0f) == VA_STATUS_SUCCESS, "an opaque layer above the bottom one blends too");
    const auto& c = *t.d.contexts.at(context);
    check(c.vpp.has_value() && c.vpp_overlays.size() == 2, "one main pass and two blending passes");
    if (c.vpp_overlays.size() == 2)
        check(c.vpp_overlays[0].global_alpha == 0.5f && c.vpp_overlays[1].global_alpha == 1.0f,
            "each keeps its own alpha");

    Driver u;
    check(va::createContext(&u.ctx, kProcConfig, 0, 0, 0, nullptr, 0, &context) == VA_STATUS_SUCCESS, "context");
    check(va::beginPicture(&u.ctx, context, kTarget) == VA_STATUS_SUCCESS, "begin");
    check(render_layer(u, context, VA_BLEND_GLOBAL_ALPHA, 0.5f) == VA_STATUS_ERROR_UNSUPPORTED_FILTER,
        "a translucent bottom layer has nothing to blend onto");
}

void test_fetch_before_the_first_write()
{
    Driver t;
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_I420;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    check(va::createImage(&t.ctx, &format, 64, 64, &image) == VA_STATUS_SUCCESS, "I420 image");
    void* data = nullptr;
    check(va::mapBuffer(&t.ctx, image.buf, &data) == VA_STATUS_SUCCESS && data != nullptr, "map");
    if (!data)
        return;
    std::memset(data, 0xAA, image.data_size);
    check(va::getImage(&t.ctx, kSource, 0, 0, 64, 64, image.image_id) == VA_STATUS_SUCCESS,
        "a surface nothing has written reads without an error");
    const auto* bytes = static_cast<const uint8_t*>(data);
    bool cleared = true;
    for (uint32_t i = 0; i < image.data_size; ++i)
        cleared = cleared && bytes[i] == 0;
    check(cleared, "and reads as zeroes, not as the image's previous contents");

    t.d.surfaces.at(kSource)->failed = true;
    check(va::getImage(&t.ctx, kSource, 0, 0, 64, 64, image.image_id) == VA_STATUS_ERROR_OPERATION_FAILED,
        "a failed picture is still an error");
}

} // namespace

int main()
{
    test_image_formats_ignore_the_incoming_count(true, 5);
    test_image_formats_ignore_the_incoming_count(false, 4);
    test_videoproc_context_needs_no_geometry();
    test_bottom_layer_may_carry_an_idle_blend_state();
    test_fetch_before_the_first_write();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "va-gst-filter: ok\n";
    return 0;
}
