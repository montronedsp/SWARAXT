// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 3 / 4.
//
// 64-bit fixed-point phase accumulator. The integer 2^N-per-cycle representation
// keeps oscillator tuning drift-free and wavetable indexing deterministic (no
// float mantissa loss over long glides). Wrapped() is the explicit, sequenced
// replacement for the Classic carry-bit idiom.

#ifndef AVRLIB_HD_PHASE_H_
#define AVRLIB_HD_PHASE_H_

#include <cmath>
#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

class Phase {
 public:
  static constexpr int kDefaultFracBits = 32;

  Phase() = default;

  void Reset() {
    v_ = 0;
  }

  void SetStep(uint64_t step) {
    step_ = step;
  }

  void Add() {
    v_ += step_;
  }

  bool Wrapped() const {
    return v_ < step_;
  }

  uint32_t intPart() const {
    return static_cast<uint32_t>(v_ >> kDefaultFracBits);
  }

  uint32_t fracPart() const {
    return static_cast<uint32_t>(v_ & 0xffffffffull);
  }

  uint16_t tableIndexBits(int idxBits) const {
    if (idxBits < 1) {
      idxBits = 1;
    }
    if (idxBits > 16) {
      idxBits = 16;
    }
    return static_cast<uint16_t>(fracPart() >> (kDefaultFracBits - idxBits));
  }

  uint64_t state() const {
    return v_;
  }

  uint64_t step() const {
    return step_;
  }

  static uint64_t Step(float freq_hz, float sample_rate, int fracBits = kDefaultFracBits) {
    if (!(freq_hz > 0.0f) || !(sample_rate > 0.0f)) {
      return 0;
    }
    if (fracBits < 1) {
      fracBits = 1;
    }
    if (fracBits > 63) {
      fracBits = 63;
    }
    const double step =
        std::ldexp(static_cast<double>(freq_hz) / static_cast<double>(sample_rate),
                   fracBits);
    if (!(step >= 1.0)) {
      return 0;
    }
    return static_cast<uint64_t>(step + 0.5);
  }

 private:
  uint64_t v_ = 0;
  uint64_t step_ = 0;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(Phase);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_PHASE_H_