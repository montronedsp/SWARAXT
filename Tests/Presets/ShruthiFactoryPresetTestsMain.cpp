// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Plugin/ApvtsFactoryPresetData.h"
#include "Plugin/ApvtsFactoryPresets.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/ShruthiFactoryPresetData.h"
#include "Plugin/ShruthiFactoryPresets.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include "shruthi/patch.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_set>

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

float getFloat(const SwaraXtAudioProcessor& proc, const char* id)
{
    if (auto* param = proc.getApvts().getRawParameterValue(id))
        return param->load();
    return 0.0f;
}

int getInt(const SwaraXtAudioProcessor& proc, const char* id)
{
    return static_cast<int>(std::lround(getFloat(proc, id)));
}

std::string stateSignature(const SwaraXtAudioProcessor& proc)
{
    juce::MemoryBlock block;
    const_cast<SwaraXtAudioProcessor&>(proc).getStateInformation(block);
    return block.toBase64Encoding().toStdString();
}

bool nearlyEqual(float a, float b) noexcept
{
    const float absDiff = std::fabs(a - b);
    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
    return absDiff <= 1.0e-4f * scale;
}

void renderSmoke(SwaraXtAudioProcessor& proc, int midiNote)
{
    proc.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512 * 8);
    buffer.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, midiNote, static_cast<juce::uint8>(100)), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, midiNote), 512 * 4);
    proc.processBlock(buffer, midi);
    proc.processBlock(buffer, midi);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* data = buffer.getReadPointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            expect(std::isfinite(data[sample]), "audio smoke finite output");
            expect(std::fabs(data[sample]) < 8.0f, "audio smoke bounded output");
        }
    }
}

void verifyOverrideParity()
{
    expect(static_cast<int>(swaraxt::kMutableFactoryOverrideCount) == 11,
           "eleven Mutable FIXED/FINALFIX overrides");
    bool foundDigobass = false;
    for (std::size_t i = 0; i < swaraxt::kMutableFactoryOverrideCount; ++i)
    {
        const auto& overridePreset = swaraxt::kMutableFactoryOverrides[i];
        if (std::strcmp(overridePreset.displayName, "digobass") == 0)
            foundDigobass = true;

        int factoryIndex = -1;
        for (std::size_t s = 0; s < swaraxt::kShruthiFactoryPresetCount; ++s)
        {
            if (std::strcmp(swaraxt::kShruthiFactoryPresets[s].displayName,
                            overridePreset.displayName) == 0)
            {
                factoryIndex = swaraxt::kShruthiFactoryPresetStart + static_cast<int>(s);
                break;
            }
        }
        expect(factoryIndex >= 0, "override maps to Mutable factory preset");
        if (factoryIndex < 0)
            continue;

        SwaraXtAudioProcessor factoryProc;
        factoryProc.setCurrentProgram(factoryIndex);

        for (std::size_t p = 0; p < overridePreset.paramCount; ++p)
        {
            const char* id = overridePreset.params[p].id;
            const float expected = overridePreset.params[p].value;
            const float actual = getFloat(factoryProc, id);
            if (! nearlyEqual(actual, expected))
            {
                std::printf("FAIL: %s param %s expected %g got %g\n",
                            overridePreset.displayName, id, expected, actual);
                ++failures;
            }
        }
        for (const int note : { 36, 48, 60 })
            renderSmoke(factoryProc, note);
    }
    expect(foundDigobass, "digobass FINALFIX override present");
}

void verifyUserFactoryParity()
{
    expect(static_cast<int>(swaraxt::kUserFactoryPresetCount) == 25,
           "twenty-five user-designed factory presets");
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < swaraxt::kUserFactoryPresetCount; ++i)
    {
        const auto& record = swaraxt::kUserFactoryPresets[i];
        expect(record.displayName != nullptr && std::strlen(record.displayName) > 0,
               "user factory name non-empty");
        expect(names.insert(record.displayName).second, "user factory names unique");
        const juce::String lower = juce::String(record.displayName).toLowerCase();
        expect(! lower.contains("fixed"), "user factory names are not corrections");
        expect(! lower.contains("finalfix"), "user factory names are not corrections");
        expect(lower != "lfo mod" && ! lower.contains("lfo mod"), "LFO MOD excluded");
        expect(! lower.contains("duo pong"), "duo pong excluded");
        expect(! lower.contains("voweano"), "voweano excluded");

        SwaraXtAudioProcessor proc;
        proc.setCurrentProgram(swaraxt::kUserFactoryPresetStart + static_cast<int>(i));
        expect(proc.getProgramName(proc.getCurrentProgram()) == record.displayName,
               "user factory program name");

        for (std::size_t p = 0; p < record.paramCount; ++p)
        {
            const float actual = getFloat(proc, record.params[p].id);
            if (! nearlyEqual(actual, record.params[p].value))
            {
                std::printf("FAIL: user %s param %s expected %g got %g\n",
                            record.displayName, record.params[p].id,
                            record.params[p].value, actual);
                ++failures;
            }
        }
        for (const int note : { 36, 48, 60 })
            renderSmoke(proc, note);

        juce::MemoryBlock saved;
        proc.getStateInformation(saved);
        SwaraXtAudioProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        for (std::size_t p = 0; p < record.paramCount; ++p)
        {
            const float expected = getFloat(proc, record.params[p].id);
            const float actual = getFloat(restored, record.params[p].id);
            if (! nearlyEqual(actual, expected))
            {
                std::printf("FAIL: roundtrip %s param %s expected %g got %g\n",
                            record.displayName, record.params[p].id, expected, actual);
                ++failures;
            }
        }
        expect(restored.getCurrentProgram() == proc.getCurrentProgram(),
               "user factory program index roundtrip");
    }
}

void verifyMutableBank()
{
    std::set<std::string> signatures;
    for (std::size_t i = 0; i < swaraxt::kShruthiFactoryPresetCount; ++i)
    {
        const auto& record = swaraxt::kShruthiFactoryPresets[i];
        const bool hasOverride =
            swaraxt::ApvtsFactoryPresets::findMutableOverride(record.displayName) != nullptr;
        shruthi::Patch patch {};
        expect(swaraxt::ShruthiFactoryPresets::decodePatch(record.bytes.data(),
                                                           record.bytes.size(),
                                                           patch),
               "patch decode succeeds");

        SwaraXtAudioProcessor proc;
        proc.setCurrentProgram(swaraxt::kShruthiFactoryPresetStart + static_cast<int>(i));
        if (! hasOverride)
        {
            expect(getInt(proc, swaraxt::IDs::osc1Shape) == patch.osc[0].shape,
                   "osc1 shape roundtrip");
            expect(getInt(proc, swaraxt::IDs::mixBalance) == patch.mix_balance,
                   "mix balance roundtrip");
        }
        signatures.insert(stateSignature(proc));
        for (const int note : { 36, 48, 60 })
            renderSmoke(proc, note);
    }
    expect(static_cast<int>(signatures.size()) == 40, "40 unique Mutable states");
}

void verifyProgramLayout()
{
    SwaraXtAudioProcessor proc;
    const int expected = swaraxt::kUserFactoryPresetStart
        + static_cast<int>(swaraxt::kUserFactoryPresetCount);
    expect(proc.getNumPrograms() == expected, "total factory program count");
    expect(expected == 76, "11 native + 40 Mutable + 25 user = 76");
    expect(std::strcmp(proc.getProgramName(0).toRawUTF8(), "Saw Bass") == 0,
           "native indices preserved");
    expect(std::strcmp(proc.getProgramName(swaraxt::kShruthiFactoryPresetStart).toRawUTF8(),
                       "Flo bass") == 0,
           "Mutable bank starts at Flo bass");
    expect(std::strcmp(proc.getProgramName(swaraxt::kUserFactoryPresetStart).toRawUTF8(),
                       "1996 Sub Bass") == 0,
           "user bank starts after Mutable");

    for (int i = 0; i < proc.getNumPrograms(); ++i)
    {
        const auto name = proc.getProgramName(i).toLowerCase();
        expect(name != "lfo mod", "LFO MOD not in programs");
        expect(! name.contains("duo pong"), "duo pong not in programs");
        expect(! name.contains("voweano"), "voweano not in programs");
        expect(! name.endsWithIgnoreCase("fixed"), "FIXED suffix not in programs");
        expect(! name.endsWithIgnoreCase("finalfix"), "FINALFIX suffix not in programs");
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    juce::ignoreUnused(argc, argv);
    verifyProgramLayout();
    verifyOverrideParity();
    verifyUserFactoryParity();
    verifyMutableBank();

    if (failures == 0)
        std::printf("ShruthiFactoryPresetTests: PASS\n");
    else
        std::printf("ShruthiFactoryPresetTests: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
