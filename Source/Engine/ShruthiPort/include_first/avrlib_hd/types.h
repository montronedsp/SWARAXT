// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 3.

#ifndef AVRLIB_HD_TYPES_H_
#define AVRLIB_HD_TYPES_H_

#include <cstdint>

namespace avrlib_hd {

using Sample       = float;   // 0-centred audio sample, nominally [-1, 1].
using ControlValue = float;   // control-path value, nominally [0, 1].
using ModSource    = float;   // bipolar modulation source, nominally [-1, 1].
using ModDest      = float;   // modulation destination in its natural unit.

// Per-block render size; equals the Classic kAudioBlockSize (40 samples).
constexpr int kAudioBlockSize = 40;

#define AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;                \
  void operator=(const TypeName&) = delete

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_TYPES_H_