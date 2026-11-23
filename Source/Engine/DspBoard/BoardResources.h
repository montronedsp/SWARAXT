// Copyright 2011 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
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
// Resources definitions.
//
// Table bits from Shruthi 56bfe78a27cd7430ab4531439d4efc2834353b17.
#pragma once
#include <cstdint>

namespace swaraxt::board::resources {
extern const std::uint8_t waveform_res_resonance_response[256];
extern const std::uint8_t waveform_res_sine[257];
extern const std::uint16_t lut_res_distortion[4096];
extern const std::uint16_t lut_res_fold[4096];
extern const std::uint16_t lut_res_integrator_gain[256];
extern const std::uint16_t lut_res_comb_delays[256];
extern const std::uint16_t lut_res_phase_increment[256];
extern const std::uint16_t lut_res_delay_duration[256];
extern const std::uint16_t lut_res_delay_decimation[256];
extern const std::uint16_t lut_res_delay_filter_gain[256];
extern const std::uint16_t lut_res_delay_phase_scaling[256];
extern const std::uint16_t lut_res_tap_delay_duration[681];
extern const std::uint16_t lut_res_tap_delay_decimation[681];
extern const std::uint16_t lut_res_tap_delay_filter_gain[681];
extern const std::uint16_t lut_res_pitch_ratio[256];
}
