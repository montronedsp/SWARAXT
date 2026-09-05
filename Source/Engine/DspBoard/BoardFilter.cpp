// Copyright 2011 Emilie Gillet; desktop adaptation Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// RenderDcf/RenderDca from Shruthi 56bfe78, with explicit AVR-width arithmetic.
#include "BoardFilter.h"
#include "BoardResources.h"

namespace swaraxt::board {
using namespace arithmetic;
void BoardFilter::process(Block& samples, std::uint8_t cutoff, std::uint8_t resonanceCode, bool highPass) noexcept
{
    if (highPass) cutoff = std::min<std::uint8_t>(cutoff, 240);
    auto resonance = resources::waveform_res_resonance_response[resonanceCode];
    const auto gain = static_cast<std::uint8_t>(255 - U8U8MulShift8(resonance, 208));
    const auto integrator = resources::lut_res_integrator_gain[cutoff];
    const auto compensation = static_cast<std::uint8_t>(96 - U8U8MulShift8(static_cast<std::uint8_t>(integrator >> 8), 96));
    resonance = static_cast<std::uint8_t>(resonance > compensation ? resonance - compensation : 0);
    for (auto& in : samples)
    {
        const auto feedback = s16(S16U8MulShift8(sub(poles_[0], poles_[1]), resonance) * 4);
        poles_[0] = add(poles_[0], S16U16MulShift16(add(sub(in, poles_[0]), feedback), integrator));
        poles_[1] = add(poles_[1], S16U16MulShift16(sub(poles_[0], poles_[1]), integrator));
        poles_[2] = add(poles_[2], S16U16MulShift16(sub(poles_[1], poles_[2]), integrator));
        // The original clamps p0 only AFTER updating p1 and p2.
        poles_[0] = std::clamp<std::int16_t>(poles_[0], -8191, 8191);
        in = clip12(S16U8MulShift8(highPass ? sub(in, poles_[2]) : poles_[2], gain));
    }
}
void BoardFilter::dca(Block& samples, std::uint8_t gain) noexcept
{
    for (auto& sample : samples) sample = S16U8MulShift8(sample, gain);
}
}
