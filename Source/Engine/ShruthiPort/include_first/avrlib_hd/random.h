// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi support library. Created independently for the HD
// engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
// See docs/HD_AVRLIB_PLAN.md section 5.
//
// Random: Galois LFSR-16 with the exact Classic feedback polynomial (kept for
// parity in the naive HD modes) plus a symmetric float accessor; NormRandom: an
// mt19937-backed, sanitizer-clean float generator for the HD modes.

#ifndef AVRLIB_HD_RANDOM_H_
#define AVRLIB_HD_RANDOM_H_

#include <cstdint>
#include <random>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

class Random {
 public:
  Random() = default;

  void Seed(uint16_t seed) {
    lfsr_ = seed;
  }

  void Update() {
    lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) ^ (-(lfsr_ & 1) & 0xb400));
  }

  uint16_t state() const {
    return lfsr_;
  }

  uint8_t state_msb() const {
    return static_cast<uint8_t>(lfsr_ >> 8);
  }

  uint8_t GetByte() {
    Update();
    return state_msb();
  }

  uint16_t GetWord() {
    Update();
    return state();
  }

  float next_float() {
    return (2.0f * static_cast<float>(GetWord()) - 65535.0f) / 65535.0f;
  }

 private:
  uint16_t lfsr_ = 0x21;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(Random);
};

class NormRandom {
 public:
  explicit NormRandom(uint32_t seed = 5489u) : engine_(seed) {}

  void Seed(uint32_t seed) {
    engine_.seed(seed);
  }

  float next_float() {
    return (2.0f * engine_()) / 4294967296.0f - 1.0f;
  }

  float next_unit() {
    return engine_() / 4294967296.0f;
  }

 private:
  std::mt19937 engine_;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(NormRandom);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_RANDOM_H_