// Copyright 2011 Emilie Gillet; desktop adaptation Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardArithmetic.h"

namespace swaraxt::board {
class BoardFilter {
public:
    void reset() noexcept { poles_ = {}; }
    void process(Block& samples, std::uint8_t cutoff, std::uint8_t resonance, bool highPass) noexcept;
    static void dca(Block& samples, std::uint8_t gain) noexcept;
    static bool hasFeedback(std::uint8_t cutoff, std::uint8_t resonance, bool highPass) noexcept;
    const std::array<std::int16_t, 3>& poles() const noexcept { return poles_; }
private:
    std::array<std::int16_t, 3> poles_{};
};
}
