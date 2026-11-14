// SPDX-License-Identifier: MIT
//
// How a completed CAPTURE frame reaches the client (docs/architecture.md
// §3.3). Direct hands the Frame itself to the surface. Copy writes the frame
// into the surface's StableBuffer with the GPU (EGL blit on Adreno) or the
// CPU, then releases the slot.

#pragma once

#include "../util/options.h"
#include "alloc.h"
#include "slots.h"

#include <functional>
#include <memory>

namespace iris {

class CopyEngine {
public:
    virtual ~CopyEngine() = default;
    virtual const char* name() const = 0;
    // Copy the visible width x height region from source to destination.
    // Both layouts describe NV12 or P010 with a single object.
    virtual void copy(StableBuffer& destination, Frame& source) = 0;
    // Drop any cached import of the buffer with this id (Frame::id() or
    // StableBuffer::id()). A cached EGLImage holds a DMA-BUF reference, and
    // vb2 refuses REQBUFS(0) while one is open, so the session calls this
    // for every Frame before it releases a CAPTURE pool and the VA surface
    // calls it for its StableBuffer on destruction.
    virtual void forget(uint64_t id) = 0;
};

std::unique_ptr<CopyEngine> make_cpu_copy();
// render_fd is the VA display's DRM render node; the engine duplicates it.
// Returns nullptr if EGL/GBM cannot be initialised, so the caller can fall
// back to the CPU engine.
std::unique_ptr<CopyEngine> make_gpu_copy(int render_fd);

// A CopyEngine that picks its backend on the first copy. Initialising EGL
// costs about 0.1 s on SM8550, and a client that publishes Direct or Import
// never copies: with this engine vaCreateContext pays nothing and the first
// Copy pays for the backend. Thread-safe; the backend is built once.
std::unique_ptr<CopyEngine> make_deferred_copy(int render_fd, CopyMode mode);
// Same, with the backend built by an explicit factory (tests, and any caller
// that wants a backend other than the two above).
std::unique_ptr<CopyEngine> make_deferred_copy(std::function<std::unique_ptr<CopyEngine>()> factory);

} // namespace iris
