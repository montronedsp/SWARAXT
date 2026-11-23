// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 6.
//
// Non-owning typed view over a float table plus integer/fractional reads.
// The fractional read mirrors Classic InterpolateSample (op.h) semantics
// without the 8-bit phase/lookup quantisation.

#ifndef AVRLIB_HD_RESOURCES_H_
#define AVRLIB_HD_RESOURCES_H_

#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

class FloatTable {
 public:
  FloatTable() = default;

  FloatTable(const float* data, uint32_t size) : data_(data), size_(size) {}

  bool valid() const {
    return data_ != nullptr && size_ > 0;
  }

  uint32_t size() const {
    return size_;
  }

  const float* data() const {
    return data_;
  }

  float operator()(int i) const {
    return data_[i];
  }

  float Sample(float index) const {
    const int i0 = static_cast<int>(index);
    const int i1 = i0 + 1;
    const int j1 = i1 < static_cast<int>(size_) ? i1 : i0;
    const float frac = index - static_cast<float>(i0);
    const float v0 = data_[i0];
    const float v1 = data_[j1];
    return v0 + (v1 - v0) * frac;
  }

 private:
  const float* data_ = nullptr;
  uint32_t size_ = 0;
};

inline float Lookup(const FloatTable& table, int i) {
  return table(i);
}

inline float Lookup(const FloatTable& table, float i) {
  return table.Sample(i);
}

template <typename Index>
inline float Lookup(const float* table, Index i, float lerp = 0.0f) {
  const float v0 = table[i];
  const float v1 = table[i + 1];
  return v0 + (v1 - v0) * lerp;
}

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_RESOURCES_H_