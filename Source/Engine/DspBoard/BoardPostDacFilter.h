// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace swaraxt::board {

// Post-MCP4921 reconstruction response for the *active FX path*.
// v03: 4-pole Chebyshev, 3 dB passband ripple, ~19 kHz corner.
// These stable native-rate SOS were fitted offline to the ANALOG Chebyshev
// magnitude response over 0..19.1 kHz, with a unity-DC plugin-level trim.
// Do not substitute BoardResponse::outputSos: its near-Nyquist fit is wrong.
//
// The native stream is 20 MHz / 510 ~= 39.216 kHz. It cannot represent the
// analog 39 kHz DAC image, so host SRC/FIR remains responsible for images.
// This stage only models the board's *in-band* coloration.
// The coefficients are fixed: do not redesign them at the host sample rate.
class BoardPostDacFilter {
public:
    static constexpr double nativeRate = 20000000.0 / 510.0;
    static constexpr double dcCompensation = 1.167825748315479;

    void reset() noexcept { state_ = {}; }

    float process(float input) noexcept
    {
        if (!std::isfinite(input))
        {
            reset();
            return 0.0f;
        }

        double sample = static_cast<double>(input);
        for (std::size_t i = 0; i < sections_.size(); ++i)
        {
            const auto& c = sections_[i];
            auto& s = state_[i];
            const double output = c[0] * sample + s[0];
            s[0] = c[1] * sample - c[3] * output + s[1];
            s[1] = c[2] * sample - c[4] * output;
            sample = output;
        }
        sample *= dcCompensation;
        if (!std::isfinite(sample))
        {
            reset();
            return 0.0f;
        }
        return static_cast<float>(sample);
    }

    template <typename Block>
    void process(Block& samples, std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            samples[i] = process(samples[i]);
    }

    using Section = std::array<double, 5>;  // b0,b1,b2,a1,a2; a0=1
    inline static constexpr std::array<Section, 2> sections_ {{
        {{ 1.0, -0.16537871671554064, 0.055594123078070302,
           -0.42482119419361325, 0.30372353533545937 }},
        {{ 1.0,  1.2788983152068709,  0.40889522515991789,
            1.5343998916539368,  0.64487761171413138 }}
    }};

private:
    std::array<std::array<double, 2>, 2> state_ {};
};
} // namespace swaraxt::board
