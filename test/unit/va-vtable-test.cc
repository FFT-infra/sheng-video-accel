// SPDX-License-Identifier: MIT
//
// The VA vtable has 60 slots in libva 2.23 and libva dispatches only 50 of
// them behind a null check. An empty unguarded slot is not an unimplemented
// feature: the client dereferences NULL and dies. vaQuerySurfaceError was
// exactly that until it was implemented.
//
// This links the real driver and drives fill_context on a zeroed
// VADriverContext, so the assertions run against the real assignments rather
// than against a copy. It pins three things: the 50 slots that must be
// non-null, the 10 that must stay null because libva guards them, and the VPP
// sub-table, which follows vpp_available() and the client's own pointer.
//
// A libva upgrade that adds a slot fails the source check
// (test/iris-va-vtable-surface-check.py), not this test; this one proves the
// shipped build still fills what the guard list claims.

#include "../../src/va/driver.h"

#include <cstring>
#include <iostream>

extern "C" {
#include <va/va_backend.h>
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

} // namespace

int main()
{
    VADriverContext ctx {};
    VADriverVTable vtable {};
    ctx.vtable = &vtable;

    va::DriverData d;
    va::fill_context(&ctx, d);

    // Field-by-field: the required list, then the guarded list. Spelled out
    // rather than table-driven because a typo in a table would silently
    // compare the wrong slot.
    check(vtable.vaTerminate, "vaTerminate");
    check(vtable.vaQueryConfigProfiles, "vaQueryConfigProfiles");
    check(vtable.vaQueryConfigEntrypoints, "vaQueryConfigEntrypoints");
    check(vtable.vaQueryConfigAttributes, "vaQueryConfigAttributes");
    check(vtable.vaGetConfigAttributes, "vaGetConfigAttributes");
    check(vtable.vaCreateConfig, "vaCreateConfig");
    check(vtable.vaDestroyConfig, "vaDestroyConfig");
    check(vtable.vaQueryDisplayAttributes, "vaQueryDisplayAttributes");
    check(vtable.vaGetDisplayAttributes, "vaGetDisplayAttributes");
    check(vtable.vaSetDisplayAttributes, "vaSetDisplayAttributes");
    check(vtable.vaCreateSurfaces, "vaCreateSurfaces");
    check(vtable.vaCreateSurfaces2, "vaCreateSurfaces2");
    check(vtable.vaDestroySurfaces, "vaDestroySurfaces");
    check(vtable.vaSyncSurface, "vaSyncSurface");
    check(vtable.vaSyncBuffer, "vaSyncBuffer");
    check(vtable.vaQuerySurfaceStatus, "vaQuerySurfaceStatus");
    check(vtable.vaQuerySurfaceError, "vaQuerySurfaceError");
    check(vtable.vaQuerySurfaceAttributes, "vaQuerySurfaceAttributes");
    check(vtable.vaExportSurfaceHandle, "vaExportSurfaceHandle");
    check(vtable.vaPutSurface, "vaPutSurface");
    check(vtable.vaLockSurface, "vaLockSurface");
    check(vtable.vaUnlockSurface, "vaUnlockSurface");
    check(vtable.vaCreateContext, "vaCreateContext");
    check(vtable.vaDestroyContext, "vaDestroyContext");
    check(vtable.vaBeginPicture, "vaBeginPicture");
    check(vtable.vaRenderPicture, "vaRenderPicture");
    check(vtable.vaEndPicture, "vaEndPicture");
    check(vtable.vaCreateBuffer, "vaCreateBuffer");
    check(vtable.vaDestroyBuffer, "vaDestroyBuffer");
    check(vtable.vaMapBuffer, "vaMapBuffer");
    check(vtable.vaUnmapBuffer, "vaUnmapBuffer");
    check(vtable.vaBufferSetNumElements, "vaBufferSetNumElements");
    check(vtable.vaBufferInfo, "vaBufferInfo");
    check(vtable.vaAcquireBufferHandle, "vaAcquireBufferHandle");
    check(vtable.vaReleaseBufferHandle, "vaReleaseBufferHandle");
    check(vtable.vaQueryImageFormats, "vaQueryImageFormats");
    check(vtable.vaCreateImage, "vaCreateImage");
    check(vtable.vaDeriveImage, "vaDeriveImage");
    check(vtable.vaDestroyImage, "vaDestroyImage");
    check(vtable.vaSetImagePalette, "vaSetImagePalette");
    check(vtable.vaGetImage, "vaGetImage");
    check(vtable.vaPutImage, "vaPutImage");
    check(vtable.vaQuerySubpictureFormats, "vaQuerySubpictureFormats");
    check(vtable.vaCreateSubpicture, "vaCreateSubpicture");
    check(vtable.vaDestroySubpicture, "vaDestroySubpicture");
    check(vtable.vaSetSubpictureImage, "vaSetSubpictureImage");
    check(vtable.vaSetSubpictureChromakey, "vaSetSubpictureChromakey");
    check(vtable.vaSetSubpictureGlobalAlpha, "vaSetSubpictureGlobalAlpha");
    check(vtable.vaAssociateSubpicture, "vaAssociateSubpicture");
    check(vtable.vaDeassociateSubpicture, "vaDeassociateSubpicture");

    check(!vtable.vaCopy, "vaCopy stays empty: libva guards it");
    check(!vtable.vaCreateBuffer2, "vaCreateBuffer2 stays empty: libva guards it");
    check(!vtable.vaCreateMFContext, "vaCreateMFContext stays empty: libva guards it");
    check(!vtable.vaGetSurfaceAttributes, "vaGetSurfaceAttributes stays empty: libva guards it");
    check(!vtable.vaMFAddContext, "vaMFAddContext stays empty: libva guards it");
    check(!vtable.vaMFReleaseContext, "vaMFReleaseContext stays empty: libva guards it");
    check(!vtable.vaMFSubmit, "vaMFSubmit stays empty: libva guards it");
    check(!vtable.vaMapBuffer2, "vaMapBuffer2 stays empty: libva guards it");
    check(!vtable.vaQueryProcessingRate, "vaQueryProcessingRate stays empty: libva guards it");
    check(!vtable.vaSyncSurface2, "vaSyncSurface2 stays empty: libva guards it");

    // The context fields a client reads before it calls anything.
    check(ctx.version_major == VA_MAJOR_VERSION && ctx.version_minor == VA_MINOR_VERSION, "version");
    check(ctx.str_vendor != nullptr && std::strcmp(ctx.str_vendor, va::kVendor) == 0, "vendor string");
    check(ctx.max_profiles == va::kMaxProfiles, "max_profiles");
    check(ctx.max_entrypoints == va::kMaxEntrypoints, "max_entrypoints");

    // No VPP sub-table: nothing to fill, and no crash.
    check(ctx.vtable_vpp == nullptr, "a client that offers no VPP table gets none");

    // With a sub-table, the answer follows vpp_available(d), not a constant.
    VADriverVTableVPP vpp {};
    ctx.vtable_vpp = &vpp;
    va::fill_context(&ctx, d);
    const bool expected = va::vpp_available(d);
    check((vpp.vaQueryVideoProcFilters != nullptr) == expected, "VPP filters follow vpp_available");
    check((vpp.vaQueryVideoProcPipelineCaps != nullptr) == expected, "VPP pipeline caps follow vpp_available");
    if (expected)
        check(vpp.version == VA_DRIVER_VTABLE_VPP_VERSION, "VPP table version");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "va-vtable: all cases passed\n";
    return 0;
}
