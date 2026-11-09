// SPDX-License-Identifier: MIT
//
// Fourcc definitions the vendored kernel headers may predate. One place,
// because `iris/` and `codec/` both need them and neither includes the other.

#pragma once

#include <linux/videodev2.h>

// Sanitised kernel headers older than the target predate the stateful AV1
// fourcc; the firmware and kernel expose it regardless.
#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '0', '1')
#endif
