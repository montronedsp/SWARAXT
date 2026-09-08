// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardResponse.h"
#include <cstdint>

namespace swaraxt::board {
class BoardOutputModel {
public:
    void reset() noexcept { response_.reset(); }
    float process(std::int16_t sample) noexcept
    {
        // Character: output SOS plus the historical R7/R15 polarity.
        // LF coupling is supplied by the existing host 3.5 Hz DC blocker, not
        // duplicated here. The physical 10/22 voltage ratio is then restored so
        // plugin nominal level does not inherit PCB output-amplifier attenuation.
        // Compensation is after the musical DCF/DCA/FX and after the character SOS.
        constexpr double kPhysicalOutputRatio = -10.0 / 22.0;
        constexpr double kNominalCompensation = 22.0 / 10.0;
        return static_cast<float>(response_.process(static_cast<double>(sample) / 2048, outputSos)
            * kPhysicalOutputRatio * kNominalCompensation);
    }
private:
    BoardResponse<outputSos.size()> response_;
};
}
