// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void expect(bool condition, const char* message)
{
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    SwaraXtAudioProcessor processor;
    auto* parameter = processor.getApvts().getParameter(swaraxt::IDs::seqMidiOut);
    auto* boolean = dynamic_cast<juce::AudioParameterBool*>(parameter);
    expect(boolean != nullptr, "MIDI Out retains AudioParameterBool compatibility");
    if (boolean == nullptr) return 1;
    expect(parameter->getParameterID() == "seq.midiOut", "MIDI Out ID unchanged");
    expect(parameter->getParameterIndex() == 93 && processor.getParameters().size() == 95,
           "MIDI Out host index and parameter count unchanged");
    expect(parameter->getDefaultValue() == 0.0f && parameter->isAutomatable()
           && parameter->isBoolean() && parameter->isDiscrete() && parameter->getNumSteps() == 2,
           "boolean default, domain and automation unchanged");
    expect(parameter->getNormalisableRange().start == 0.0f
           && parameter->getNormalisableRange().end == 1.0f, "boolean range unchanged");

    const float values[] { 0.0f, 0.001f, 0.331463f, 0.499f,
                          std::nextafter(0.5f, 0.0f), 0.5f,
                          std::nextafter(0.5f, 1.0f), 0.9f, 1.0f };
    juce::AudioProcessorParameter& api = *parameter;
    for (const bool notify : { false, true })
        for (const float value : values)
        {
            const float expected = value >= 0.5f ? 1.0f : 0.0f;
            if (notify) api.setValueNotifyingHost(value); else api.setValue(value);
            expect(api.getValue() == expected && boolean->get() == (expected == 1.0f),
                   "host readback agrees with inherited boolean threshold");
            if (!notify) continue;
            expect(processor.getApvts().getRawParameterValue(swaraxt::IDs::seqMidiOut)->load() == expected,
                   "APVTS agrees with boolean threshold including adjacent floats");
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            SwaraXtAudioProcessor restored;
            auto* restoredParameter = restored.getApvts().getParameter(swaraxt::IDs::seqMidiOut);
            restoredParameter->setValueNotifyingHost(1.0f - expected);
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            expect(restoredParameter->getValue() == expected, "host session restores logical MIDI Out value");
        }

    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("swara-midi-bool", {}, false);
    processor.setUserPresetDirectoryForTests(directory);
    for (const bool enabled : { false, true })
    {
        api.setValueNotifyingHost(enabled ? 0.9f : 0.331463f);
        juce::String error;
        expect(processor.saveUserPreset("Boolean", true, error), "save user preset");
        api.setValueNotifyingHost(enabled ? 0.0f : 1.0f);
        const auto entries = processor.getPresetEntries();
        bool found = false;
        for (const auto& entry : entries)
            if (!entry.isFactory && entry.name == "Boolean")
            {
                found = true;
                expect(processor.loadPresetEntry(entry, error), "load user preset");
                expect(api.getValue() == (enabled ? 1.0f : 0.0f), "preset restores logical MIDI Out value");
            }
        expect(found, "saved preset appears in catalog");
    }
    directory.deleteRecursively();
    if (failures == 0) std::puts("BOOLEAN_NORMALIZATION_PASS");
    return failures == 0 ? 0 : 1;
}
