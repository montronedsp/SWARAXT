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
        // R7/R15 output gain. LF coupling is supplied by existing host 3.5Hz DC
        // blocker, not duplicated here. Host FIR still performs reconstruction.
        return static_cast<float>(response_.process(static_cast<double>(sample) / 2048, outputSos) * (-10.0 / 22.0));
    }
private:
    BoardResponse<outputSos.size()> response_;
};
}
