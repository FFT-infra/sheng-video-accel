// SPDX-License-Identifier: MIT
//
// Experimental AV1 translator with an internal eight-slot DPB. Unlike the
// shipped held-picture translator, every vaEndPicture produces a complete
// temporal unit immediately: when all eight reference slots are live, the
// previous uncommitted picture is replayed as a hidden NOSHOW frame inside
// the same temporal unit before the shown frame.
//
// This is an experiment, not a capability. Nothing in src/va/ selects it yet;
// make_av1() still returns the qualified held-picture translator.

#pragma once

#include <memory>

extern "C" {
#include <va/va.h>
}

#include "translator.h"

namespace iris::codec {

std::unique_ptr<Translator> make_av1_dpb(VAProfile profile);

} // namespace iris::codec
