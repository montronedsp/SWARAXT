// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 6.
//
// One-time conversion of the Classic Shruthi prog_* tables into float arrays.
// The conversion is exact (integer value -> identical float value); scaling to
// the HD numeric model is a separate, engine-level concern.

#ifndef AVRLIB_HD_RESOURCES_LOADER_H_
#define AVRLIB_HD_RESOURCES_LOADER_H_

#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

class ResourcesLoader {
 public:
  static uint32_t ConvertU8(const uint8_t* src, uint32_t size, float* dst) {
    for (uint32_t i = 0; i < size; ++i) {
      dst[i] = static_cast<float>(src[i]);
    }
    return size;
  }

  static uint32_t ConvertU16(const uint16_t* src, uint32_t size, float* dst) {
    for (uint32_t i = 0; i < size; ++i) {
      dst[i] = static_cast<float>(src[i]);
    }
    return size;
  }

  template <size_t N>
  static uint32_t ConvertU8(const uint8_t (&src)[N], float* dst) {
    return ConvertU8(src, static_cast<uint32_t>(N), dst);
  }

  template <size_t N>
  static uint32_t ConvertU16(const uint16_t (&src)[N], float* dst) {
    return ConvertU16(src, static_cast<uint32_t>(N), dst);
  }
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_RESOURCES_LOADER_H_