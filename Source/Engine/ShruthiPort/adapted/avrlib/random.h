// Copyright 2009 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
// Host adaptation for Swara XT by MontroneDSP; based on Shruthi commit 56bfe78.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// -----------------------------------------------------------------------------
//
// Fast 16-bit pseudo random number generator.

#ifndef AVRLIB_RANDOM_H_
#define AVRLIB_RANDOM_H_

#include "avrlib/base.h"

namespace avrlib {

class Random {
 public:
  Random() { }

  void Update() {
    // Galois LFSR with feedback polynomial = x^16 + x^14 + x^13 + x^11.
    // Period: 65535.
    rng_state_ = (rng_state_ >> 1) ^ (-(rng_state_ & 1) & 0xb400);
  }

  inline uint16_t state() const { return rng_state_; }

  inline void Seed(uint16_t seed) {
    rng_state_ = seed;
  }

  inline uint8_t state_msb() const {
    return static_cast<uint8_t>(rng_state_ >> 8);
  }

  inline uint8_t GetByte() {
    Update();
    return state_msb();
  }

  inline uint16_t GetWord() {
    Update();
    return state();
  }

 private:
  uint16_t rng_state_ = 0x21;

  DISALLOW_COPY_AND_ASSIGN(Random);
};

}  // namespace avrlib

#endif  // AVRLIB_RANDOM_H_
