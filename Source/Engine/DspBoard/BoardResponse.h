// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>

namespace swaraxt::board {
// Offline complex-response fit of the v03 schematic, NOT measured hardware.
// Input: four finite-2.8MHz-GBW Sallen-Key sections plus R6/C33.
// Output: two ideal-op-amp Sallen-Key sections and DAC hold baseband response.
// Native Fs=20e6/510; no explicit delay, no out-of-band DAC images.
inline constexpr std::array<std::array<double, 6>, 6> inputSos {{
    {{ -0.0031828192621562229, -0.017052489582021251, -0.052901911227907779, 1.0000000000000000, -1.1709312637341196, 0.44352588362977174 }},
    {{ 1.0000000000000000, -6.0133260151059780, -9.4697434733776440, 1.0000000000000000, 0.82195267086878721, 0.48837826338414436 }},
    {{ 1.0000000000000000, -0.014019908264158554, 0.012026115350227297, 1.0000000000000000, -0.37191297737169438, 0.50927534195050295 }},
    {{ 1.0000000000000000, 0.82992384954059351, 0.18859173541910218, 1.0000000000000000, 1.4714429877160828, 0.54091323451194007 }},
    {{ 1.0000000000000000, 1.5677520806524154, 0.63283172852640257, 1.0000000000000000, 1.3862383806206404, 0.85896067334057080 }},
    {{ 1.0000000000000000, 1.9005368978667514, 0.90621513393184161, 1.0000000000000000, 1.8589277734149054, 0.86143241727287623 }}
}};
inline constexpr std::array<std::array<double, 6>, 4> outputSos {{
    {{ -0.062436276288915751, 0.45414820984525606, 0.94410832123921995, 1.0000000000000000, 0.84813872163386184, 0.17854886147045490 }},
    {{ 1.0000000000000000, 0.42493423037435679, 0.065294042570708630, 1.0000000000000000, -0.35595872386610405, 0.29107091326776524 }},
    {{ 1.0000000000000000, 1.3799567351527899, 0.51239904825798144, 1.0000000000000000, 1.5347600438750857, 0.58791789551286877 }},
    {{ 1.0000000000000000, 1.8837496477054199, 0.89542787794873635, 1.0000000000000000, 1.8712384512716551, 0.87448160037862743 }}
}};

template<std::size_t N> class BoardResponse {
public:
    void reset() noexcept { state_ = {}; }
    double process(double sample, const std::array<std::array<double, 6>, N>& sos) noexcept
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            const auto& c = sos[i];
            auto& s = state_[i];
            const double out = c[0] * sample + s[0];
            s[0] = c[1] * sample - c[4] * out + s[1];
            s[1] = c[2] * sample - c[5] * out;
            sample = out;
        }
        return sample;
    }
private:
    std::array<std::array<double, 2>, N> state_{};
};
}

