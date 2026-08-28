// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <cstring>

#include "Plugin/ApvtsFactoryPresetData.h"

namespace swaraxt {

class ApvtsFactoryPresets {
 public:
    static void applyParams(const ApvtsFactoryParam* params,
                            std::size_t count,
                            juce::AudioProcessorValueTreeState& apvts) noexcept
    {
        if (params == nullptr || count == 0)
            return;

        for (std::size_t i = 0; i < count; ++i)
        {
            auto* parameter = apvts.getParameter(params[i].id);
            if (parameter == nullptr)
                continue;

            if (auto* asFloat = dynamic_cast<juce::AudioParameterFloat*>(parameter))
            {
                *asFloat = params[i].value;
                continue;
            }
            if (auto* asInt = dynamic_cast<juce::AudioParameterInt*>(parameter))
            {
                *asInt = static_cast<int>(std::lround(params[i].value));
                continue;
            }
            if (auto* asBool = dynamic_cast<juce::AudioParameterBool*>(parameter))
            {
                *asBool = params[i].value >= 0.5f;
                continue;
            }
            if (auto* asChoice = dynamic_cast<juce::AudioParameterChoice*>(parameter))
            {
                *asChoice = static_cast<int>(std::lround(params[i].value));
            }
        }
    }

    static const ApvtsFactoryPreset* findMutableOverride(const char* displayName) noexcept
    {
        if (displayName == nullptr)
            return nullptr;
        for (std::size_t i = 0; i < kMutableFactoryOverrideCount; ++i)
        {
            if (std::strcmp(kMutableFactoryOverrides[i].displayName, displayName) == 0)
                return &kMutableFactoryOverrides[i];
        }
        return nullptr;
    }
};

}  // namespace swaraxt
