// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardEffects.h"
#include <cmath>
#include <limits>

namespace swaraxt::board {
enum class Model : std::uint8_t { classic, dspBoard };
enum class Route : std::uint8_t { lowPassFirst, highPassFirst, lowPassLast, highPassLast, fxOnly };
struct BoardControl {
    Model model = Model::classic;
    Route route = Route::lowPassFirst;
    Effect effect = Effect::off;
    std::uint8_t cutoff = 254, resonance = 0, dca = 0, cv1 = 128, cv2 = 0, tempo = 120;

    static std::uint8_t nativeCv(int value) noexcept { return static_cast<std::uint8_t>(std::clamp(value, 0, 254)); }
    static std::uint8_t tempoCode(double value) noexcept
    {
        return static_cast<std::uint8_t>(std::clamp(std::isfinite(value) ? std::round(value) : 120.0, 40.0, 240.0));
    }
    static std::uint8_t cutoffCode(double musicalHz) noexcept
    {
        // Invert the original resource generator's self-oscillation tuning law.
        const double hz = std::clamp(std::isfinite(musicalHz) ? musicalHz : 10.0, 1.0, sampleRate * .5);
        return nativeCv(static_cast<int>(std::lround(256 + 24 * std::log2(hz / (sampleRate * .75)))));
    }
    bool sameTopology(const BoardControl& other) const noexcept
    {
        return model == other.model && effect == other.effect && (model == Model::classic || route == other.route);
    }
    bool postDcaFilter() const noexcept
    {
        return model == Model::dspBoard && (route == Route::lowPassLast || route == Route::highPassLast);
    }
};
}
