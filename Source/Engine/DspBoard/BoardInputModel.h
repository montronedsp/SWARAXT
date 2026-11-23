// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BoardArithmetic.h"
#include "BoardResponse.h"
#include <cmath>

namespace swaraxt::board {
class BoardInputModel {
public:
    // Native mixer is nominally [-1,1]. The 0.4 board-level mapping reserves
    // headroom for the schematic response (LP impulse L1=2.387), not a limiter.
    static constexpr double inputGain = -0.4;
    void reset() noexcept { response_.reset(); previousInput_ = coupling_ = 0; clipped_ = samples_ = 0; }
    static std::uint8_t quantize(double x) noexcept
    {
        if (!std::isfinite(x)) return 128;
        return static_cast<std::uint8_t>(std::clamp(std::floor(128 + 128 * x), 0.0, 255.0));
    }
    // Schematic C10/R14 + 8-pole Chebyshev-style SOS + board-level mapping.
    // hardwareMix=1 is the analog pre-ADC path; 0 keeps ADC quantize only.
    double analogProcess(double in) noexcept
    {
        // C10/R14 input coupling, 0.339 Hz, distinct from final host DC protection.
        constexpr double pole = 0.999945745790407;
        coupling_ = (1.0 + pole) * 0.5 * (in - previousInput_) + pole * coupling_;
        previousInput_ = in;
        return response_.process(coupling_, inputSos) * inputGain;
    }
    std::uint8_t process(float sample, float hardwareMix = 1.0f) noexcept
    {
        const double in = std::isfinite(sample) ? sample : 0.0;
        const double analog = analogProcess(in);
        const double mix = hardwareMix <= 0.0f ? 0.0 : hardwareMix >= 1.0f ? 1.0
            : static_cast<double>(hardwareMix);
        const double x = in + mix * (analog - in);
        ++samples_;
        if (x < -1 || x >= 1) ++clipped_;
        return quantize(x);
    }
    std::uint64_t clippedSamples() const noexcept { return clipped_; }
    std::uint64_t processedSamples() const noexcept { return samples_; }
private:
    BoardResponse<inputSos.size()> response_;
    double previousInput_ = 0, coupling_ = 0;
    std::uint64_t clipped_ = 0, samples_ = 0;
};
}
