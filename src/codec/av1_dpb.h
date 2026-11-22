// SPDX-License-Identifier: MIT
//
// AV1 translator with an internal eight-slot DPB. Every vaEndPicture
// produces a complete temporal unit immediately: when all eight reference
// slots are live, the previous uncommitted picture is replayed as a hidden
// NOSHOW frame inside the same temporal unit before the shown frame.
//
// make_av1() remains the one-line rollback path to the former translator.

#pragma once

#include <memory>

extern "C" {
#include <va/va.h>
}

#include "translator.h"

namespace iris::codec {

// `hardware_grain` lets the rebuilt frame header carry the client's film
// grain parameters, which this firmware applies itself; the completion then
// holds the grained picture and the VA glue publishes it into
// current_display_picture. Off, the header asks for no grain and the glue
// blends it in software.
std::unique_ptr<Translator> make_av1_dpb(VAProfile profile, bool hardware_grain = false);

} // namespace iris::codec
