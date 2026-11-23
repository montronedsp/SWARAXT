// Copyright 2010 Emilie Gillet.
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
// Polyphonic voice allocator. This is used when the Shruthi-1 is configured
// for poly-chaining.

#ifndef SHRUTHI_VOICE_ALLOCATOR_H_
#define SHRUTHI_VOICE_ALLOCATOR_H_

#include "avrlib/base.h"

static const uint8_t kMaxPolyphony = 8;

namespace shruthi {

struct VoiceEntry {
  uint8_t note;
  uint8_t active;
};

class VoiceAllocator {
 public:
  VoiceAllocator() { }
  void Init() { size_ = 0; Clear(); }
  void Clear();
  void set_size(uint8_t size) {
    size_ = size;
  }
  uint8_t NoteOn(uint8_t note);
  uint8_t NoteOff(uint8_t note);

 private:
  void Touch(uint8_t voice);

  VoiceEntry pool_[kMaxPolyphony] {};
  // Holds the indices of the voices sorted by most recent usage.
  uint8_t lru_[kMaxPolyphony] {};
  uint8_t size_ = 0;

  DISALLOW_COPY_AND_ASSIGN(VoiceAllocator);
};

}  // namespace shruthi

#endif // SHRUTHI_VOICE_ALLOCATOR_H_
