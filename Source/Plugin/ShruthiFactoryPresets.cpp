// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Plugin/ShruthiFactoryPresets.h"

#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"

#include <cmath>
#include <cstring>

namespace swaraxt {

namespace {

constexpr float kMinimumPanelCutoffHz = 10.0f;
constexpr float kMaximumPanelCutoffHz = 20000.0f;

void setIntParam(juce::AudioProcessorValueTreeState& apvts, const char* id, int value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(apvts.getParameter(id)))
        *param = value;
}

void setFloatParam(juce::AudioProcessorValueTreeState& apvts, const char* id, float value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(apvts.getParameter(id)))
        *param = value;
}

int mixerOperatorFromPatch(const shruthi::Patch& patch) noexcept
{
    const int option = static_cast<int>(patch.osc[0].option);
    const int op = static_cast<int>(patch.ops_[0].op & 0x0f);
    return op != 0 ? op : option;
}

}  // namespace

float ShruthiFactoryPresets::cutoffHzFromShruthiCode(std::uint8_t code) noexcept
{
    const float semitonesBelowMaximum = static_cast<float>(code) - 127.0f;
    const float hz = kMaximumPanelCutoffHz * std::pow(2.0f, semitonesBelowMaximum / 12.0f);
    if (hz < kMinimumPanelCutoffHz)
        return kMinimumPanelCutoffHz;
    if (hz > kMaximumPanelCutoffHz)
        return kMaximumPanelCutoffHz;
    return hz;
}

std::uint8_t ShruthiFactoryPresets::shruthiCutoffCodeFromHz(float cutoffHz) noexcept
{
    float bounded = cutoffHz;
    if (bounded < kMinimumPanelCutoffHz)
        bounded = kMinimumPanelCutoffHz;
    if (bounded > kMaximumPanelCutoffHz)
        bounded = kMaximumPanelCutoffHz;
    const float semitonesBelowMaximum = 12.0f * std::log2(bounded / kMaximumPanelCutoffHz);
    int code = static_cast<int>(std::lround(127.0f + semitonesBelowMaximum));
    if (code < 0)
        code = 0;
    if (code > 127)
        code = 127;
    return static_cast<std::uint8_t>(code);
}

bool ShruthiFactoryPresets::decodePatch(const std::uint8_t* bytes,
                                        std::size_t length,
                                        shruthi::Patch& patch) noexcept
{
    constexpr std::size_t kSavedPatchBytes = 92;
    if (bytes == nullptr || length < kSavedPatchBytes)
        return false;

    std::memcpy(patch.saved_data(), bytes, kSavedPatchBytes);
    if (! patch.CheckBuffer(patch.saved_data()))
        return false;

    patch.Update();
    return true;
}

void ShruthiFactoryPresets::applyPatchToApvts(const shruthi::Patch& patch,
                                              juce::AudioProcessorValueTreeState& apvts)
{
    setIntParam(apvts, IDs::osc1Shape, patch.osc[0].shape);
    setIntParam(apvts, IDs::osc1Param, patch.osc[0].parameter);
    setIntParam(apvts, IDs::osc1Range, patch.osc[0].range);
    setIntParam(apvts, IDs::osc1Option, mixerOperatorFromPatch(patch));

    setIntParam(apvts, IDs::osc2Shape, patch.osc[1].shape);
    setIntParam(apvts, IDs::osc2Param, patch.osc[1].parameter);
    setIntParam(apvts, IDs::osc2Range, patch.osc[1].range);
    setIntParam(apvts, IDs::osc2Option, patch.osc[1].option);

    setIntParam(apvts, IDs::mixBalance, patch.mix_balance);
    setIntParam(apvts, IDs::mixSub, patch.mix_sub_osc);
    setIntParam(apvts, IDs::mixNoise, patch.mix_noise);
    setIntParam(apvts, IDs::mixSubShape, patch.mix_sub_osc_shape);

    setFloatParam(apvts, IDs::filterCutoff, cutoffHzFromShruthiCode(patch.filter_cutoff));
    setFloatParam(apvts, IDs::filterResonance,
                  static_cast<float>(patch.filter_resonance) / 63.0f);
    setIntParam(apvts, IDs::filterShruthiEnv, patch.filter_env);
    setIntParam(apvts, IDs::filterShruthiLfo, patch.filter_lfo);
    setFloatParam(apvts, IDs::filterEnvAmount, 0.0f);
    setFloatParam(apvts, IDs::filterModAmount, 0.0f);
    setFloatParam(apvts, IDs::filterKeyTracking, 0.5f);

    setIntParam(apvts, IDs::env1Attack, patch.env[0].attack);
    setIntParam(apvts, IDs::env1Decay, patch.env[0].decay);
    setIntParam(apvts, IDs::env1Sustain, patch.env[0].sustain);
    setIntParam(apvts, IDs::env1Release, patch.env[0].release);

    setIntParam(apvts, IDs::env2Attack, patch.env[1].attack);
    setIntParam(apvts, IDs::env2Decay, patch.env[1].decay);
    setIntParam(apvts, IDs::env2Sustain, patch.env[1].sustain);
    setIntParam(apvts, IDs::env2Release, patch.env[1].release);

    setIntParam(apvts, IDs::lfo1Wave, patch.lfo[0].waveform);
    setIntParam(apvts, IDs::lfo1Rate, patch.lfo[0].rate);
    setIntParam(apvts, IDs::lfo1Attack, patch.lfo[0].attack);
    setIntParam(apvts, IDs::lfo1Retrig, patch.lfo[0].retrigger_mode);
    setIntParam(apvts, IDs::lfo1Sync, 0);
    setIntParam(apvts, IDs::lfo1Division, 3);

    setIntParam(apvts, IDs::lfo2Wave, patch.lfo[1].waveform);
    setIntParam(apvts, IDs::lfo2Rate, patch.lfo[1].rate);
    setIntParam(apvts, IDs::lfo2Attack, patch.lfo[1].attack);
    setIntParam(apvts, IDs::lfo2Retrig, patch.lfo[1].retrigger_mode);
    setIntParam(apvts, IDs::lfo2Sync, 0);
    setIntParam(apvts, IDs::lfo2Division, 3);

    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        const juce::String prefix = "mod.row" + juce::String(row + 1) + ".";
        setIntParam(apvts,
                    (prefix + "source").toRawUTF8(),
                    patch.modulation_matrix.modulation[row].source);
        setIntParam(apvts,
                    (prefix + "destination").toRawUTF8(),
                    patch.modulation_matrix.modulation[row].destination);
        setIntParam(apvts,
                    (prefix + "amount").toRawUTF8(),
                    patch.modulation_matrix.modulation[row].amount);
    }

    setIntParam(apvts, IDs::perfGlide, 0);
    setIntParam(apvts, IDs::perfLegato, 0);
    setIntParam(apvts, IDs::seqMode, 0);
    setIntParam(apvts, IDs::seqClockMode, 0);
    setIntParam(apvts, IDs::seqTempo, 120);
    setIntParam(apvts, IDs::seqSwing, 0);
    setIntParam(apvts, IDs::seqGate, 50);
    setIntParam(apvts, IDs::arpDirection, 0);
    setIntParam(apvts, IDs::arpPattern, 1);
    setIntParam(apvts, IDs::arpOctaves, 1);
    setIntParam(apvts, IDs::arpGate, 7);
    setFloatParam(apvts, IDs::master, 0.85f);
}

}  // namespace swaraxt
