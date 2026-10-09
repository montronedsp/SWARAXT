// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Exact end-to-end plugin output with production DSP FX after Classic.

#include <JuceHeader.h>

#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Engine/SequenceState.h"
#include "Parity/ExactParityGoldens.h"

#include "shruthi/patch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kHostRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr int kRenderSamples = 4096;
int gFailures = 0;

void expect(bool ok, const char* name)
{
    if (! ok)
    {
        std::printf("FAIL: %s\n", name);
        ++gFailures;
        return;
    }
    std::printf("PASS: %s\n", name);
}

std::uint64_t hashBits(const std::vector<float>& samples)
{
    std::uint64_t h = 1469598103934665603ull;
    for (float sample : samples)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int i = 0; i < 4; ++i)
        {
            h ^= static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu);
            h *= 1099511628211ull;
        }
    }
    return h;
}

void setInt(SwaraXtAudioProcessor& proc, const char* id, int value)
{
    auto* param = proc.getApvts().getParameter(id);
    expect(param != nullptr, id);
    if (param != nullptr)
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(value)));
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    auto* param = proc.getApvts().getParameter(id);
    expect(param != nullptr, id);
    if (param != nullptr)
        param->setValueNotifyingHost(param->convertTo0to1(value));
}

void configureSaw(SwaraXtAudioProcessor& proc)
{
    setFloat(proc, swaraxt::IDs::master, 0.85f);
    setInt(proc, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
    setInt(proc, swaraxt::IDs::osc1Param, 0);
    setInt(proc, swaraxt::IDs::osc2Shape, shruthi::WAVEFORM_NONE);
    setInt(proc, swaraxt::IDs::mixBalance, 0);
    setInt(proc, swaraxt::IDs::mixNoise, 0);
    setFloat(proc, swaraxt::IDs::filterCutoff, 20000.0f);
    setFloat(proc, swaraxt::IDs::filterResonance, 0.0f);
    setInt(proc, swaraxt::IDs::env2Attack, 0);
    setInt(proc, swaraxt::IDs::env2Decay, 30);
    setInt(proc, swaraxt::IDs::env2Sustain, 110);
    setInt(proc, swaraxt::IDs::env2Release, 25);
}

std::vector<float> render(SwaraXtAudioProcessor& proc, int note, bool releaseNote)
{
    proc.prepareToPlay(kHostRate, kBlockSize);
    std::vector<float> output;
    output.reserve(kRenderSamples);
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    int rendered = 0;
    while (rendered < kRenderSamples)
    {
        const int n = std::min(kBlockSize, kRenderSamples - rendered);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        juce::MidiBuffer midi;
        if (rendered == 0 && note >= 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
        if (releaseNote && note >= 0 && rendered <= kRenderSamples * 3 / 5
            && rendered + n > kRenderSamples * 3 / 5)
        {
            midi.addEvent(juce::MidiMessage::noteOff(1, note),
                          kRenderSamples * 3 / 5 - rendered);
        }
        proc.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
        {
            const float s = buffer.getSample(0, i);
            if (! std::isfinite(s))
            {
                expect(false, "integrated sample finite");
                output.push_back(0.0f);
            }
            else
                output.push_back(s);
        }
        rendered += n;
    }
    proc.releaseResources();
    return output;
}

void golden(const char* name, const std::vector<float>& audio, std::size_t expectedCount)
{
    const auto h = hashBits(audio);
    std::printf("GOLDEN integrated %s samples=%zu hash=%016llx\n",
                name, audio.size(), static_cast<unsigned long long>(h));
    expect(audio.size() == expectedCount, name);
    const auto* expected = swaraxt_golden::find(swaraxt_golden::kIntegrated, swaraxt_golden::kIntegratedCount, name);
    expect(expected != nullptr, name);
    if (expected != nullptr)
        expect(h == expected->hash, name);
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const char* programName[] = {
        "off", "distortion", "crush", "combPositive", "combNegative", "ringMod",
        "delay", "delayFeedback", "delayDub", "crushDelayFeedback", "crushDelayDub",
        "delay16", "delay12", "delay8", "delay3_16", "looper", "pitch"
    };

    for (int choice = 0; choice <= 16; ++choice)
    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::dspFxProgram, choice);
        setInt(proc, swaraxt::IDs::dspFxParam1, 64);
        setInt(proc, swaraxt::IDs::dspFxParam2, choice == 15 ? 0 : 16);
        char name[80];
        std::snprintf(name, sizeof(name), "%s_default", programName[choice]);
        golden(name, render(proc, 60, true), static_cast<std::size_t>(kRenderSamples));
    }

    {
        SwaraXtAudioProcessor a, b;
        configureSaw(a);
        configureSaw(b);
        setInt(a, swaraxt::IDs::dspFxProgram, 7);
        setInt(b, swaraxt::IDs::dspFxProgram, 7);
        setInt(a, swaraxt::IDs::dspFxParam1, 100);
        setInt(b, swaraxt::IDs::dspFxParam1, 100);
        setInt(a, swaraxt::IDs::dspFxParam2, 40);
        setInt(b, swaraxt::IDs::dspFxParam2, 40);
        expect(hashBits(render(a, 48, true)) == hashBits(render(b, 48, true)),
               "integrated delayFeedback repeatable");
    }

    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::dspFxProgram, 1);
        setInt(proc, swaraxt::IDs::dspFxParam1, 0);
        golden("distortion_cv1min", render(proc, 60, true), static_cast<std::size_t>(kRenderSamples));
    }
    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::dspFxProgram, 1);
        setInt(proc, swaraxt::IDs::dspFxParam1, 127);
        golden("distortion_cv1max", render(proc, 60, true), static_cast<std::size_t>(kRenderSamples));
    }
    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::dspFxProgram, 7);
        setInt(proc, swaraxt::IDs::dspFxParam1, 64);
        setInt(proc, swaraxt::IDs::dspFxParam2, 63);
        golden("delayFeedback_cv2max", render(proc, 36, true), static_cast<std::size_t>(kRenderSamples));
    }

    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::dspFxProgram, 15);
        setInt(proc, swaraxt::IDs::dspFxParam1, 64);
        setInt(proc, swaraxt::IDs::dspFxParam2, 0);
        proc.prepareToPlay(kHostRate, kBlockSize);
        juce::AudioBuffer<float> buffer(2, kBlockSize);
        for (int block = 0; block < 48; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            proc.processBlock(buffer, midi);
        }
        expect(proc.engineForTests().boardProcessorForTests().hasValidLoop(),
               "integrated looper recorded");
        setInt(proc, swaraxt::IDs::dspFxParam2, 63);
        std::vector<float> replay;
        for (int block = 0; block < 32; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            proc.processBlock(buffer, midi);
            for (int i = 0; i < kBlockSize; ++i)
                replay.push_back(buffer.getSample(0, i));
        }
        proc.releaseResources();
        golden("looper_replay", replay, replay.size());
    }

    {
        SwaraXtAudioProcessor proc;
        configureSaw(proc);
        setInt(proc, swaraxt::IDs::seqMode, 2);
        setInt(proc, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
        setInt(proc, swaraxt::IDs::dspFxProgram, 5);
        auto pattern = swaraxt::SequenceState::defaultSnapshot();
        pattern.length = 8;
        for (int i = 0; i < 8; ++i)
            pattern.steps[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(0x7080 + (i % 12));
        proc.sequenceState().store(pattern);
        golden("seq_ringmod", render(proc, 48, false), static_cast<std::size_t>(kRenderSamples));
    }

    std::printf("integrated FX exact parity failures=%d\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
