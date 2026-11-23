// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>

namespace swaraxt::ui {

enum class TextRole {
    sectionHeader, parameterLabel, comboValue, presetName, actionText,
    numericValue, smallIndex, aboutHeading, aboutBody
};

inline void setTextRole(juce::Component& component, TextRole role)
{
    component.getProperties().set("swaraxtTextRole", static_cast<int>(role));
    if (auto* label = dynamic_cast<juce::Label*>(&component))
        label->setBorderSize({});
}

inline TextRole textRole(const juce::Component& component, TextRole fallback)
{
    return static_cast<TextRole>(static_cast<int>(component.getProperties()
        .getWithDefault("swaraxtTextRole", static_cast<int>(fallback))));
}

class Typography {
 public:
    Typography(juce::Typeface::Ptr medium, juce::Typeface::Ptr condensedBold)
        : medium_(std::move(medium)), condensedBold_(std::move(condensedBold))
    {
        jassert(medium_ != nullptr && condensedBold_ != nullptr);
    }

    juce::Font medium(float height) const
    {
        return juce::Font(juce::FontOptions { medium_ }.withHeight(height));
    }

    juce::Font font(TextRole role) const
    {
        float height = 12.0f;
        float tracking = 0.0f;
        bool bold = false;
        switch (role)
        {
            case TextRole::sectionHeader: height = 13.0f; tracking = 0.015f; bold = true; break;
            case TextRole::parameterLabel: height = 11.5f; tracking = 0.025f; break;
            case TextRole::comboValue: height = 12.0f; break;
            case TextRole::presetName: height = 13.0f; break;
            case TextRole::actionText: height = 11.5f; bold = true; break;
            case TextRole::numericValue: height = 11.5f; break;
            case TextRole::smallIndex: height = 11.0f; break;
            case TextRole::aboutHeading: height = 21.0f; bold = true; break;
            case TextRole::aboutBody: height = 12.5f; break;
        }
        auto options = juce::FontOptions { bold ? condensedBold_ : medium_ }.withHeight(height);
        if (role == TextRole::numericValue || role == TextRole::smallIndex)
            options = options.withFeatureEnabled(juce::FontFeatureTag { "tnum" });
        return juce::Font(options).withExtraKerningFactor(tracking);
    }

 private:
    juce::Typeface::Ptr medium_, condensedBold_;
};
}
