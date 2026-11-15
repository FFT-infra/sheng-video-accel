// SPDX-License-Identifier: MIT
//
// Constants shared by the session translation units. Not interface: nothing
// outside src/iris/ includes this.

#pragma once

namespace iris::detail {

// Bound on the client buffer's implicit fence before it is handed to the
// firmware: the compositor may still be sampling the previous picture.
constexpr int kImportFenceMs = 2000;

// How long submit waits for a resolution change to clear; the client's own
// held CAPTURE frames are what block it.
constexpr int kSourceChangeWaitMs = 5000;

} // namespace iris::detail
