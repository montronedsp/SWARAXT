// Copyright 2011 Emilie Gillet.
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
// Global clock. This works as a 31-bit phase increment counter. To implement
// swing, the value at which the counter wraps is (1 << 31) times a swing
// factor.

#ifndef SHRUTHI_CLOCK_H_
#define SHRUTHI_CLOCK_H_

#include "avrlib/base.h"

namespace shruthi {

class Clock {
 public:
  Clock() { }
  ~Clock() { }

  inline void Init() {
    Update(120);
  }
  void Update(uint8_t bpm);

  inline void Reset() {
    phase_ = 0;
  }

  inline void Tick() { phase_ += phase_increment_; }
  inline bool Wrap(int8_t amount) {
    LongWord* w = (LongWord*)(&phase_);
    if (w->bytes[3] >= 128 + amount) {
      w->bytes[3] -= (128 + amount);
      return true;
    } else {
      return false;
    }
  }

 private:
  uint16_t bpm_ = 0;
  uint32_t phase_ = 0;
  uint32_t phase_increment_ = 0;

  DISALLOW_COPY_AND_ASSIGN(Clock);
};

}  // namespace shruthi

#endif // SHRUTHI_CLOCK_H_
