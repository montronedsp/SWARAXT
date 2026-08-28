// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

float maxParameterDifference(const SwaraXtAudioProcessor& a, const SwaraXtAudioProcessor& b)
{
    float maxDiff = 0.0f;
    const auto& paramsA = a.getParameters();
    const auto& paramsB = b.getParameters();
    expect(paramsA.size() == paramsB.size(), "parameter count matches");
    const int count = juce::jmin(static_cast<int>(paramsA.size()),
                                static_cast<int>(paramsB.size()));
    for (int i = 0; i < count; ++i)
    {
        if (paramsA[static_cast<size_t>(i)] != nullptr
            && paramsB[static_cast<size_t>(i)] != nullptr)
        {
            const float diff = std::abs(paramsA[static_cast<size_t>(i)]->getValue()
                                        - paramsB[static_cast<size_t>(i)]->getValue());
            maxDiff = std::max(maxDiff, diff);
        }
    }
    return maxDiff;
}

void setFloat(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void setInt(SwaraXtAudioProcessor& processor, const char* id, int value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

juce::MemoryBlock captureState(SwaraXtAudioProcessor& processor)
{
    juce::MemoryBlock data;
    processor.getStateInformation(data);
    return data;
}

void restoreState(SwaraXtAudioProcessor& processor, const juce::MemoryBlock& data)
{
    processor.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
}

void customizeSound(SwaraXtAudioProcessor& processor)
{
    processor.setCurrentProgram(0);
    setFloat(processor, swaraxt::IDs::filterCutoff, 1337.0f);
    setFloat(processor, swaraxt::IDs::filterResonance, 0.61f);
    setInt(processor, swaraxt::IDs::osc1Shape, 4);
    setInt(processor, swaraxt::IDs::osc2Range, -7);
    setFloat(processor, swaraxt::IDs::env1Attack, 12.0f);
    setFloat(processor, swaraxt::IDs::env1Decay, 88.0f);
    setFloat(processor, swaraxt::IDs::lfo1Rate, 77.0f);
    setInt(processor, swaraxt::IDs::lfo2Wave, 2);
}

float renderPeak(SwaraXtAudioProcessor& processor)
{
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 4096);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 2048);
    processor.processBlock(buffer, midi);
    return buffer.getMagnitude(0, buffer.getNumSamples());
}

void testProcessorOnlyRestore()
{
    SwaraXtAudioProcessor source;
    customizeSound(source);
    const auto saved = captureState(source);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    const float diff = maxParameterDifference(source, restored);
    expect(diff == 0.0f, "processor-only restore preserves every parameter");
}

void testHostProgramReassertDoesNotReload()
{
    SwaraXtAudioProcessor source;
    source.setCurrentProgram(swaraxt::kShruthiFactoryPresetStart + 3);
    setFloat(source, swaraxt::IDs::filterCutoff, 2222.0f);
    setFloat(source, swaraxt::IDs::filterResonance, 0.42f);
    const auto saved = captureState(source);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    expect(restored.getCurrentProgram() == source.getCurrentProgram(),
           "restored program index matches serialized metadata");
    restored.setCurrentProgram(0);
    const float diff = maxParameterDifference(source, restored);
    expect(diff == 0.0f,
           "post-restore setCurrentProgram does not reload factory preset data");
}

void testEditorOpenAfterRestore()
{
    SwaraXtAudioProcessor processor;
    customizeSound(processor);
    const auto saved = captureState(processor);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    const float before = maxParameterDifference(processor, restored);

    SwaraXtAudioProcessorEditor editor(restored);
    const float after = maxParameterDifference(processor, restored);
    expect(before == 0.0f, "restored state matches source before editor");
    expect(after == 0.0f, "editor open does not change synthesis state");
}

void testRepeatedEditorOpenClose()
{
    SwaraXtAudioProcessor processor;
    customizeSound(processor);
    const auto saved = captureState(processor);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);

    for (int i = 0; i < 4; ++i)
    {
        auto editor = std::make_unique<SwaraXtAudioProcessorEditor>(restored);
        editor.reset();
    }

    const float diff = maxParameterDifference(processor, restored);
    expect(diff == 0.0f, "repeated editor open/close leaves state unchanged");
}

void testExplicitProgramChangeAfterPrepare()
{
    SwaraXtAudioProcessor processor;
    customizeSound(processor);
    const auto saved = captureState(processor);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    restored.prepareToPlay(48000.0, 512);
    restored.setCurrentProgram(1);

    const float cutoff = swaraxt::getParameterValue(restored.getApvts(), swaraxt::IDs::filterCutoff);
    expect(std::abs(cutoff - 3500.0f) < 0.1f,
           "explicit setCurrentProgram after prepare loads target factory preset");

    restoreState(restored, saved);
    const float diff = maxParameterDifference(processor, restored);
    expect(diff == 0.0f, "subsequent DAW state restore overrides program selection");
}

void testMultiplePresetRoundtrips()
{
    const std::vector<int> programs {
        0,
        2,
        swaraxt::kShruthiFactoryPresetStart + 5,
        swaraxt::kUserFactoryPresetStart
    };

    for (const int program : programs)
    {
        SwaraXtAudioProcessor source;
        source.setCurrentProgram(program);
        setFloat(source, swaraxt::IDs::filterCutoff, 1500.0f + static_cast<float>(program));
        setInt(source, swaraxt::IDs::osc1Shape, (program % 8) + 1);
        const auto saved = captureState(source);

        SwaraXtAudioProcessor restored;
        restoreState(restored, saved);
        restored.setCurrentProgram(0);
        const float diff = maxParameterDifference(source, restored);
        expect(diff == 0.0f, "multiple preset indices survive host program reassert");
    }
}

void testCustomizedFactoryPresetRestore()
{
    SwaraXtAudioProcessor source;
    source.setCurrentProgram(swaraxt::kShruthiFactoryPresetStart + 10);
    setFloat(source, swaraxt::IDs::filterCutoff, 876.0f);
    setFloat(source, swaraxt::IDs::filterResonance, 0.73f);
    setInt(source, swaraxt::IDs::osc1Shape, 9);
    const auto saved = captureState(source);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    restored.setCurrentProgram(0);
    expect(std::abs(swaraxt::getParameterValue(restored.getApvts(), swaraxt::IDs::filterCutoff)
                    - 876.0f) < 0.1f,
           "customized cutoff survives restore");
    expect(std::abs(swaraxt::getParameterValue(restored.getApvts(), swaraxt::IDs::filterResonance)
                    - 0.73f) < 0.001f,
           "customized resonance survives restore");
}

void testAudioEquivalence()
{
    SwaraXtAudioProcessor source;
    customizeSound(source);
    const auto saved = captureState(source);
    const float sourcePeak = renderPeak(source);

    SwaraXtAudioProcessor restored;
    restoreState(restored, saved);
    restored.setCurrentProgram(0);
    const float restoredPeak = renderPeak(restored);
    expect(std::abs(sourcePeak - restoredPeak) < 1.0e-4f,
           "restored processor renders equivalent deterministic output");
}

}  // namespace

int main(int argc, char* argv[])
{
    juce::ignoreUnused(argc, argv);
    juce::ScopedJuceInitialiser_GUI juceInit;

    testProcessorOnlyRestore();
    testHostProgramReassertDoesNotReload();
    testEditorOpenAfterRestore();
    testRepeatedEditorOpenClose();
    testExplicitProgramChangeAfterPrepare();
    testMultiplePresetRoundtrips();
    testCustomizedFactoryPresetRestore();
    testAudioEquivalence();

    if (failures == 0)
    {
        std::printf("PASS: preset state restore regression\n");
        return 0;
    }

    std::printf("FAILURES: %d\n", failures);
    return 1;
}
