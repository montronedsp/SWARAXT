// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plugin/SwaraXtParameterLayout.h"

#include "shruthi/patch.h"

#include <cmath>

namespace swaraxt {

namespace {

juce::AudioParameterInt* intParam(const juce::String& id,
                                 const juce::String& name,
                                 int minV,
                                 int maxV,
                                 int defaultV)
{
    return new juce::AudioParameterInt(juce::ParameterID { id, 1 }, name, minV, maxV, defaultV);
}

juce::AudioParameterFloat* floatParam(const juce::String& id,
                                      const juce::String& name,
                                      float minV,
                                      float maxV,
                                      float defaultV,
                                      float step = 0.0f,
                                      float skew = 1.0f)
{
    juce::NormalisableRange<float> range { minV, maxV, step, skew };
    return new juce::AudioParameterFloat(juce::ParameterID { id, 1 }, name, range, defaultV);
}

juce::AudioParameterFloat* cutoffParam()
{
    constexpr float minimumHz = 10.0f;
    constexpr float maximumHz = 20000.0f;
    juce::NormalisableRange<float> range {
        minimumHz,
        maximumHz,
        [](float start, float end, float normalized) {
            return start * std::pow(end / start, normalized);
        },
        [](float start, float end, float physical) {
            return std::log(juce::jlimit(start, end, physical) / start) / std::log(end / start);
        },
        [](float start, float end, float physical) {
            return juce::jlimit(start, end, physical);
        }
    };

    auto attributes = juce::AudioParameterFloatAttributes {}
        .withStringFromValueFunction([](float value, int) {
            if (value < 1000.0f)
                return juce::String(value, value < 100.0f ? 1 : 0) + " Hz";
            return juce::String(value / 1000.0f, value < 10000.0f ? 2 : 1) + " kHz";
        })
        .withValueFromStringFunction([](const juce::String& text) {
            const auto lower = text.trim().toLowerCase();
            const float scale = lower.contains("khz") ? 1000.0f : 1.0f;
            return juce::jlimit(minimumHz, maximumHz, lower.getFloatValue() * scale);
        });

    return new juce::AudioParameterFloat(juce::ParameterID { IDs::filterCutoff, 1 },
                                         "Filter Cutoff",
                                         range,
                                         8000.0f,
                                         attributes);
}

}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createSwaraXtParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        floatParam(IDs::master, "Master", 0.0f, 1.0f, 0.85f, 0.001f)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        cutoffParam()));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        floatParam(IDs::filterResonance, "Filter Resonance", 0.0f, 1.0f, 0.15f, 0.0f)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        floatParam(IDs::filterEnvAmount, "Filter Env Amount", 0.0f, 1.0f, 0.35f, 0.0f)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        floatParam(IDs::filterKeyTracking, "Filter Key Track", 0.0f, 1.0f, 0.5f, 0.0f)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        floatParam(IDs::filterModAmount, "Filter Mod Amount", 0.0f, 1.0f, 0.0f, 0.0f)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::filterShruthiEnv, "Filter Env Depth", -128, 127, 32)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::filterShruthiLfo, "Filter LFO Depth", -128, 127, 0)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc1Shape, "Osc 1 Shape", 0, static_cast<int>(shruthi::WAVEFORM_LAST) - 1, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc1Param, "Osc 1 Parameter", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc1Range, "Osc 1 Range", -48, 48, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc1Option, "Mixer Operator", 0, 13, 0)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc2Shape, "Osc 2 Shape", 0, static_cast<int>(shruthi::WAVEFORM_LAST) - 1, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc2Param, "Osc 2 Parameter", 0, 127, 16)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc2Range, "Osc 2 Range", -48, 48, -12)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::osc2Option, "Osc 2 Detune", 0, 127, 12)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::mixBalance, "Mix Balance", 0, 127, 32)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::mixSub, "Sub Osc", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::mixNoise, "Noise", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::mixSubShape, "Sub Shape", 0, 10, 0)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env1Attack, "Env1 Attack", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env1Decay, "Env1 Decay", 0, 127, 50)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env1Sustain, "Env1 Sustain", 0, 127, 20)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env1Release, "Env1 Release", 0, 127, 60)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env2Attack, "Env2 Attack", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env2Decay, "Env2 Decay", 0, 127, 40)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env2Sustain, "Env2 Sustain", 0, 127, 90)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::env2Release, "Env2 Release", 0, 127, 30)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Wave, "LFO1 Wave", 0,
                  static_cast<int>(shruthi::LFO_WAVEFORM_LAST) - 1, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Rate, "LFO1 Rate", 0, 127, 80)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Attack, "LFO1 Attack", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Retrig, "LFO1 Retrigger", 0, 3, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Sync, "LFO1 Timing", 0, 1, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo1Division, "LFO1 Beat Division", 0, 7, 3)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Wave, "LFO2 Wave", 0,
                  static_cast<int>(shruthi::LFO_WAVEFORM_LAST) - 1, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Rate, "LFO2 Rate", 0, 127, 3)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Attack, "LFO2 Attack", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Retrig, "LFO2 Retrigger", 0, 3, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Sync, "LFO2 Timing", 0, 1, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::lfo2Division, "LFO2 Beat Division", 0, 7, 3)));

    // Defaults mirror Shruthi init_patch modulation matrix (part.cc).
    // MOD_SRC_ENV_2=20, MOD_DST_VCA=1, MOD_SRC_VELOCITY=21, MOD_SRC_PITCH_BEND=8,
    // MOD_DST_VCO_1_2_COARSE=6, MOD_SRC_LFO_1=0.
    const int defaultSource[12] = { 0, 19, 19, 0, 1, 2, 21, 21, 20, 21, 8, 0 };
    const int defaultDest[12] = { 4, 5, 2, 3, 8, 8, 2, 3, 1, 1, 6, 6 };
    const int defaultAmount[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 63, 16, 32, 16 };

    for (int row = 0; row < 12; ++row)
    {
        const juce::String prefix = "mod.row" + juce::String(row + 1) + ".";
        params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
            intParam(prefix + "source",
                     "Mod Source " + juce::String(row + 1),
                     0,
                     static_cast<int>(shruthi::kNumModulationSources) - 1,
                     defaultSource[row])));
        params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
            intParam(prefix + "destination",
                     "Mod Dest " + juce::String(row + 1),
                     0,
                     static_cast<int>(shruthi::kNumModulationDestinations) - 1,
                     defaultDest[row])));
        params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
            intParam(prefix + "amount",
                     "Mod Amount " + juce::String(row + 1),
                     -63,
                     63,
                     defaultAmount[row])));
    }

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::perfGlide, "Portamento", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::perfLegato, "Legato", 0, 1, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::midiChannel, "MIDI Channel", 0, 16, 0)));

    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::seqMode, "Sequencer Mode", 0, 2, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::seqTempo, "Sequencer Tempo", 40, 240, 120)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::seqSwing, "Sequencer Swing", 0, 127, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::seqClockMode, "Sequencer Clock", 0, 1, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::seqGate, "Sequencer Gate", 5, 100, 50)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::arpDirection, "Arp Direction", 0, 4, 0)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::arpPattern, "Arp Pattern", 0, 7, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::arpOctaves, "Arp Octaves", 1, 4, 1)));
    params.push_back(std::unique_ptr<juce::RangedAudioParameter>(
        intParam(IDs::arpGate, "Sequencer Division", 0, 11, 7)));

    return { params.begin(), params.end() };
}

float getParameterValue(const juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    if (auto* param = apvts.getRawParameterValue(id))
        return param->load();
    return 0.0f;
}

int getParameterInt(const juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    return static_cast<int>(std::lround(getParameterValue(apvts, id)));
}

}  // namespace swaraxt
