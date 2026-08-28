// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace shruthi {
class Patch;
}  // namespace shruthi

namespace swaraxt {

class ShruthiFactoryPresets {
 public:
    static bool decodePatch(const std::uint8_t* bytes,
                            std::size_t length,
                            shruthi::Patch& patch) noexcept;

    static void applyPatchToApvts(const shruthi::Patch& patch,
                                  juce::AudioProcessorValueTreeState& apvts);

    static float cutoffHzFromShruthiCode(std::uint8_t code) noexcept;
    static std::uint8_t shruthiCutoffCodeFromHz(float cutoffHz) noexcept;
};

}  // namespace swaraxt
