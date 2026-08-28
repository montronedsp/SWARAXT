// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "shruthi/patch.h"

namespace swaraxt::ui {

/** Display names for Shruthi modulation enums (IDs 0..31 remain stable). */
inline juce::StringArray modulationSourceNames()
{
    return {
        "LFO 1", "LFO 2", "Seq", "Seq 1", "Seq 2", "Step",
        "Mod Wheel", "Aftertouch", "Pitch Bend", "Offset",
        "CV 1", "CV 2", "CV 3", "CV 4",
        "CC A", "CC B", "CC C", "CC D",
        "Noise", "Env 1", "Env 2", "Velocity", "Random", "Note", "Gate", "Audio",
        "Op 1", "Op 2", "Value 4", "Value 8", "Value 16", "Value 32"
    };
}

/** Oscillator model names. Index 35 (WAVEFORM_LAST) is not a waveform. */
inline juce::StringArray oscillatorNames()
{
    return {
        "None", "Analog Saw", "Analog Square", "Analog Triangle",
        "CZ Saw", "CZ Resonant", "CZ Triangle", "CZ Pulse", "CZ Sync",
        "Quad Saw Pad", "Linear FM",
        "Wavetable 1", "Wavetable 2", "Wavetable 3", "Wavetable 4",
        "Wavetable 5", "Wavetable 6", "Wavetable 7", "Wavetable 8",
        "User Wavetable", "8-bit Land", "Crushed Sine", "Dirty PWM",
        "Filtered Noise", "Vowel",
        "Wavetable 9", "Wavetable 10", "Wavetable 11", "Wavetable 12",
        "Wavetable 13", "Wavetable 14", "Wavetable 15", "Wavetable 16",
        "Wavetable 17", "Wavetable 18"
    };
}

// Only the 27 destinations Shruthi defines are listed. The parameter range is
// clamped to these values at engine ingress so restored automation cannot
// select an undefined destination. PWM 1/2 are displayed as Timbre 1/2.
inline juce::StringArray modulationDestinationNames()
{
    return {
        "Filter Cutoff", "VCA", "Timbre 1", "Timbre 2",
        "VCO 1", "VCO 2", "VCO Coarse", "VCO Fine",
        "Mix Balance", "Mix Noise", "Mix Sub", "Filter Res",
        "CV 1", "CV 2", "Attack", "LFO 1 Rate", "LFO 2 Rate",
        "Trig Env 1", "Trig Env 2",
        "Env1 A", "Env1 D", "Env1 S", "Env1 R",
        "Env2 A", "Env2 D", "Env2 S", "Env2 R"
    };
}

inline bool isHardwareOnlyModulationSource(int engineId) noexcept
{
    return engineId == static_cast<int>(shruthi::MOD_SRC_CV_1)
        || engineId == static_cast<int>(shruthi::MOD_SRC_CV_2)
        || engineId == static_cast<int>(shruthi::MOD_SRC_CV_3)
        || engineId == static_cast<int>(shruthi::MOD_SRC_CV_4);
}

inline bool isHardwareOnlyModulationDestination(int engineId) noexcept
{
    return engineId == static_cast<int>(shruthi::MOD_DST_CV_1)
        || engineId == static_cast<int>(shruthi::MOD_DST_CV_2);
}

inline juce::String modulationSourceName(int engineId)
{
    const auto names = modulationSourceNames();
    if (engineId >= 0 && engineId < names.size())
        return names[engineId];
    return "Source " + juce::String(engineId);
}

inline juce::String modulationDestinationName(int engineId)
{
    const auto names = modulationDestinationNames();
    if (engineId >= 0 && engineId < names.size())
        return names[engineId];
    return "Dest " + juce::String(engineId);
}

inline int pluginVisibleModulationSourceCount() noexcept
{
    return static_cast<int>(shruthi::kNumModulationSources) - 4;
}

inline int pluginVisibleModulationDestinationCount() noexcept
{
    return static_cast<int>(shruthi::kNumModulationDestinations) - 2;
}

inline void populatePluginModulationSourceCombo(juce::ComboBox& box)
{
    box.clear(juce::dontSendNotification);
    const auto names = modulationSourceNames();
    for (int id = 0; id < names.size(); ++id)
        if (! isHardwareOnlyModulationSource(id))
            box.addItem(names[id], id + 1);
}

inline void populatePluginModulationDestinationCombo(juce::ComboBox& box)
{
    box.clear(juce::dontSendNotification);
    const auto names = modulationDestinationNames();
    for (int id = 0; id < names.size(); ++id)
        if (! isHardwareOnlyModulationDestination(id))
            box.addItem(names[id], id + 1);
}

// ComboBoxAttachment maps by visible index, which breaks as soon as hardware-only
// IDs are omitted. Bind ComboBox item IDs to stable engine enums instead.
class EngineIdComboAttachment : private juce::ComboBox::Listener
{
 public:
    EngineIdComboAttachment(juce::AudioProcessorValueTreeState& apvts,
                            const juce::String& parameterId,
                            juce::ComboBox& combo,
                            juce::String (*nameForId)(int))
        : combo_(combo),
          parameter_(*apvts.getParameter(parameterId)),
          nameForId_(nameForId),
          attachment_(parameter_, [this](float value) { setFromParameter(value); })
    {
        combo_.addListener(this);
        attachment_.sendInitialUpdate();
    }

    ~EngineIdComboAttachment() override
    {
        combo_.removeListener(this);
    }

    EngineIdComboAttachment(const EngineIdComboAttachment&) = delete;
    EngineIdComboAttachment& operator=(const EngineIdComboAttachment&) = delete;

 private:
    void setFromParameter(float value)
    {
        const int engineId = juce::roundToInt(value);
        const int itemId = engineId + 1;
        const juce::ScopedValueSetter<bool> scoped(ignoreCallbacks_, true);
        if (combo_.indexOfItemId(itemId) >= 0)
            combo_.setSelectedId(itemId, juce::dontSendNotification);
        else
        {
            combo_.setSelectedId(0, juce::dontSendNotification);
            combo_.setText(nameForId_(engineId), juce::dontSendNotification);
        }
    }

    void comboBoxChanged(juce::ComboBox*) override
    {
        if (ignoreCallbacks_)
            return;
        const int itemId = combo_.getSelectedId();
        if (itemId <= 0)
            return;
        attachment_.setValueAsCompleteGesture(static_cast<float>(itemId - 1));
    }

    juce::ComboBox& combo_;
    juce::RangedAudioParameter& parameter_;
    juce::String (*nameForId_)(int);
    juce::ParameterAttachment attachment_;
    bool ignoreCallbacks_ = false;
};

}  // namespace swaraxt::ui
