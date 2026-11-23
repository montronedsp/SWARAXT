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
// System settings.

#include "shruthi/system_settings.h"

#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <string.h>

namespace shruthi {

void SystemSettings::EepromSave() const {
}

uint8_t SystemSettings::EepromLoad() {
  return 0;
}

}  // shruthi
