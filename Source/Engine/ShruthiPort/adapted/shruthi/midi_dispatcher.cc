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
// Instance-owned host MIDI dispatcher.

#include "shruthi/midi_dispatcher.h"

namespace shruthi {

void MidiDispatcher::Send(uint8_t status, uint8_t* data, uint8_t size) {
  if ((status & 0xf0) == 0xf0) {
    running_status_ = 0;
  }
  if (size == 0) {
    writeOutput(status);
    return;
  } else {
    if (status != running_status_) {
      writeOutput(status);
      running_status_ = status;
    }
    if (size) {
      writeOutput(*data++);
      --size;
    }
    if (size) {
      writeOutput(*data++);
      --size;
    }
  }
}

void MidiDispatcher::SendNow(uint8_t byte) {
  writeOutput(byte);
}

void MidiDispatcher::Send3(uint8_t status, uint8_t a, uint8_t b) {
  if ((status & 0xf0) == 0xf0) {
    running_status_ = 0;
  }
  if (status != running_status_) {
    writeOutput(status);
    running_status_ = status;
  }
  writeOutput(a);
  writeOutput(b);
}

void MidiDispatcher::writeOutput(uint8_t byte) {
  output_[outputWrite_] = byte;
  outputWrite_ = static_cast<uint8_t>((outputWrite_ + 1) & (kOutputBufferSize - 1));
  if (outputWrite_ == outputRead_) {
    outputRead_ = static_cast<uint8_t>((outputRead_ + 1) & (kOutputBufferSize - 1));
  }
}

}  // namespace shruthi
