// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>

#include "shruthi/patch.h"
#include "shruthi/resources.h"

namespace swaraxt::ui {

// One-cycle LFO visualizer mapping. y01 = 1 is the top of the display and
// corresponds to a high engine source value. The drawing code maps this with
// jmap(y01, bottom, top); do not invert that axis.
inline float lfoVisualizerY01(int waveform, float phase01)
{
    const float phase = phase01 - std::floor(phase01);
    if (waveform >= shruthi::LFO_WAVEFORM_WAVE_1
        && waveform < shruthi::LFO_WAVEFORM_LAST)
    {
        int shapeOffset = waveform - shruthi::LFO_WAVEFORM_WAVE_1;
        shapeOffset = shapeOffset == 0 ? 3 : shapeOffset + 16;
        const auto phaseIndex = static_cast<uint8_t>(std::floor(phase * 128.0f));
        const auto value = shruthi::ResourcesManager::Lookup<uint8_t, uint8_t>(
            shruthi::wav_res_waves + shapeOffset * 129,
            phaseIndex);
        return static_cast<float>(value) / 255.0f;
    }
    switch (waveform)
    {
        case 1: // Square: low for the first half-cycle, high for the second.
            return phase < 0.5f ? 0.15f : 0.85f;
        case 2: // Sample & hold — placeholder staircase.
            return std::floor(phase * 6.0f) / 5.0f;
        case 3: // Ramp: engine output rises through the cycle.
            return phase;
        case 4: // Step sequencer — placeholder staircase.
            return std::floor(phase * 8.0f) / 7.0f;
        default: // Triangle (and wavetable fallbacks): engine starts high and falls.
            return std::abs(phase * 2.0f - 1.0f);
    }
}

}  // namespace swaraxt::ui
