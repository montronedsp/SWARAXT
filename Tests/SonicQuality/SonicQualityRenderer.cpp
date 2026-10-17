// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SonicQuality/SonicQualityRenderer.h"

#include "SonicQuality/SonicQualityThresholds.h"

#include "Engine/DspBoard/BoardPostDacFilter.h"
#include "Engine/DspBoard/BoardProcessor.h"
#include "Engine/Filter/Circuit/Ir3109BoardCore.h"
#include "Engine/Filter/SwaraXtFilter.h"
#include "Engine/SwaraXtEngine.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

#include <JuceHeader.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "shruthi/patch.h"

namespace swaraxt::sonic {
namespace {

void setInt(SwaraXtAudioProcessor& processor, const char* id, int value)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterInt*>(processor.getApvts().getParameter(id)))
        *parameter = value;
}

void setFloat(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(processor.getApvts().getParameter(id)))
        *parameter = value;
}

void setChoice(SwaraXtAudioProcessor& processor, const char* id, int index)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(processor.getApvts().getParameter(id)))
        *parameter = index;
}

void silenceMatrix(SwaraXtAudioProcessor& processor)
{
    for (int row = 1; row <= 12; ++row)
    {
        const juce::String prefix = "mod.row" + juce::String(row) + ".";
        setInt(processor, (prefix + "amount").toRawUTF8(), 0);
    }
    const juce::String vca = "mod.row9.";
    setInt(processor, (vca + "source").toRawUTF8(), shruthi::MOD_SRC_ENV_2);
    setInt(processor, (vca + "destination").toRawUTF8(), shruthi::MOD_DST_VCA);
    setInt(processor, (vca + "amount").toRawUTF8(), 63);
}

float cutoffAt(const PluginTone& tone, int sample)
{
    const double span = std::max(1, tone.totalSamples - 1);
    const double position = std::clamp(static_cast<double>(sample) / span, 0.0, 1.0);
    switch (tone.cutoffLaw)
    {
        case CutoffLaw::linear:
            return tone.cutoffHz + static_cast<float>(position) * (tone.cutoffEndHz - tone.cutoffHz);
        case CutoffLaw::logarithmic:
        {
            const float start = std::max(tone.cutoffHz, 10.0f);
            const float end = std::max(tone.cutoffEndHz, 10.0f);
            return start * std::pow(end / start, static_cast<float>(position));
        }
        case CutoffLaw::jump:
            return position < 0.5 ? tone.cutoffHz : tone.cutoffEndHz;
        case CutoffLaw::hold:
            break;
    }
    return tone.cutoffHz;
}

}  // namespace

const char* qualityName(swaraxt::FilterQuality quality) noexcept
{
    switch (quality)
    {
        case swaraxt::FilterQuality::high: return "high";
        case swaraxt::FilterQuality::normal: return "normal";
        case swaraxt::FilterQuality::eco: return "eco";
    }
    return "normal";
}

const char* effectName(int choice) noexcept
{
    static const char* names[] = {
        "Off", "Distortion", "Crush", "Comb +", "Comb -", "Ring Mod",
        "Delay", "Delay FB", "Delay Dub", "Crush Delay FB", "Crush Delay Dub",
        "Delay 1/16", "Delay 1/12", "Delay 1/8", "Delay 3/16", "Looper", "Pitch"
    };
    if (choice < 0 || choice > 16)
        return "invalid";
    return names[choice];
}

bool effectCanBeTransparent(int choice) noexcept
{
    return choice == 0;
}

double schematicOpenFilterGain(double frequencyHz) noexcept
{
    using Core = swaraxt::Ir3109BoardCore;
    constexpr double dcGain = 0.68 * (5.0 / 33000.0) * 10000.0 * Core::kVcaDivider
        / (2.0 * swaraxt::kOtaThermalVoltageVolts) * (68000.0 / 33000.0);
    const double corner = Core::cutoffHzFromCv(5.0 * 254.0 / 255.0);
    const double fourPole = std::pow(1.0 + std::pow(frequencyHz / corner, 2.0), 2.0);
    const double inputPole = 1.0 + std::pow(2.0 * kPi * frequencyHz * Core::kInputPoleTau, 2.0);
    return dcGain / fourPole / inputPole;
}

double highpassMagnitude(double frequencyHz, double tau) noexcept
{
    const double wTau = 2.0 * kPi * frequencyHz * tau;
    return wTau / std::sqrt(1.0 + wTau * wTau);
}

double lowpassMagnitude(double frequencyHz, double tau) noexcept
{
    const double wTau = 2.0 * kPi * frequencyHz * tau;
    return 1.0 / std::sqrt(1.0 + wTau * wTau);
}

double coupledOpenFilterGain(double frequencyHz) noexcept
{
    using Core = swaraxt::Ir3109BoardCore;
    constexpr double outputAcTau = 33000.0 * 4.7e-6;
    constexpr double vcaIvTau = 10000.0 * 10.0e-12;
    return schematicOpenFilterGain(frequencyHz)
        * highpassMagnitude(frequencyHz, Core::kInputTau)
        * highpassMagnitude(frequencyHz, Core::kSharedOutputTau)
        * lowpassMagnitude(frequencyHz, vcaIvTau)
        * highpassMagnitude(frequencyHz, outputAcTau)
        * lowpassMagnitude(frequencyHz, Core::kOutputPoleTau);
}

float requestedCutoff(const PluginTone& tone, int sample) noexcept
{
    return cutoffAt(tone, sample);
}

BoardRender renderBoardTone(const BoardTone& tone)
{
    BoardRender render;
    render.sampleRate = swaraxt::SwaraXtEngine::kInternalSampleRate;
    swaraxt::SwaraXtFilter filter;
    filter.prepare(render.sampleRate);
    filter.setQuality(tone.quality);
    if (tone.oversampleOverride > 0)
        filter.setOversampleFactorForTests(tone.oversampleOverride);
    swaraxt::SwaraXtFilterParams params;
    params.cutoffHz = tone.cutoffHz;
    params.resonance = tone.resonance;
    params.drive = 1.0f;
    filter.setParams(params);
    render.latencySamples = filter.latencySamples();
    render.oversample = filter.oversamplingFactor();
    const int total = std::max(0, tone.warmupSamples) + std::max(0, tone.captureSamples);
    render.samples.reserve(static_cast<size_t>(std::max(0, tone.captureSamples)));
    for (int i = 0; i < total; ++i)
    {
        const float input = static_cast<float>(
            static_cast<double>(tone.amplitude) * std::sin(2.0 * kPi * tone.frequencyHz
                                                           * static_cast<double>(i) / render.sampleRate));
        const float output = tone.instrumentPath
            ? filter.processInstrumentSample(input, tone.vcaTarget)
            : filter.processSample(input);
        if (!std::isfinite(output))
            render.finite = false;
        if (i >= tone.warmupSamples)
            render.samples.push_back(output);
    }
    return render;
}

FxRender renderClassicFxInsert(const FxInsert& insert)
{
    FxRender render;
    render.sampleRate = swaraxt::board::sampleRate;
    swaraxt::board::BoardProcessor processor;
    swaraxt::board::BoardPostDacFilter postDac;
    swaraxt::board::BoardControl control;
    control.effect = swaraxt::board::effectFromChoice(insert.effectChoice);
    control.cv1 = insert.cv1;
    control.cv2 = insert.cv2;
    processor.reset(control.effect);
    const int total = insert.blocks * static_cast<int>(swaraxt::board::blockSize);
    render.samples.reserve(static_cast<size_t>(std::max(0, total)));
    int rendered = 0;
    std::uint32_t noise = 0x12345678u;
    for (int block = 0; block < insert.blocks; ++block)
    {
        swaraxt::board::FloatBlock frame {};
        for (std::size_t i = 0; i < frame.size(); ++i)
        {
            const int index = rendered + static_cast<int>(i);
            const double time = static_cast<double>(index) / render.sampleRate;
            double sample = 0.0;
            switch (insert.stimulus)
            {
                case FxStimulus::saw:
                {
                    const double phase = time * insert.frequencyHz;
                    sample = (2.0 * (phase - std::floor(phase)) - 1.0) * insert.amplitude;
                    break;
                }
                case FxStimulus::impulse:
                    sample = index == 0 ? insert.amplitude : 0.0;
                    break;
                case FxStimulus::noise:
                    noise = noise * 1664525u + 1013904223u;
                    sample = (static_cast<int32_t>(noise) / 2147483648.0) * insert.amplitude;
                    break;
                case FxStimulus::sine:
                    sample = static_cast<double>(insert.amplitude)
                        * std::sin(2.0 * kPi * insert.frequencyHz * time);
                    break;
            }
            frame[i] = static_cast<float>(sample);
        }
        processor.processClassicFx(frame, control);
        if (insert.applyPostDac && control.effect != swaraxt::board::Effect::off)
            postDac.process(frame, frame.size());
        for (float sample : frame)
        {
            if (!std::isfinite(sample))
                render.finite = false;
            render.samples.push_back(sample);
        }
        rendered += static_cast<int>(frame.size());
    }
    return render;
}

PluginRender renderPluginTone(const PluginTone& tone)
{
    PluginRender render;
    const auto started = std::chrono::steady_clock::now();
    SwaraXtAudioProcessor processor;
    processor.setNonRealtime(true);
    processor.setFilterQuality(tone.quality);
    silenceMatrix(processor);
    setFloat(processor, swaraxt::IDs::master, tone.master);
    setFloat(processor, swaraxt::IDs::postMixer, tone.postMixerDb);
    setFloat(processor, swaraxt::IDs::filterCutoff, tone.cutoffHz);
    setFloat(processor, swaraxt::IDs::filterResonance, tone.resonance);
    setFloat(processor, swaraxt::IDs::filterKeyTracking, 0.0f);
    setInt(processor, swaraxt::IDs::filterEnvDepth, tone.filterEnv);
    setInt(processor, swaraxt::IDs::filterLfoDepth, tone.filterLfo);
    setInt(processor, swaraxt::IDs::lfo1Rate, tone.lfoRate);
    setInt(processor, swaraxt::IDs::lfo1Sync, 0);
    setInt(processor, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
    setInt(processor, swaraxt::IDs::osc1Param, 0);
    setInt(processor, swaraxt::IDs::osc1Range, 0);
    setInt(processor, swaraxt::IDs::osc2Shape, tone.osc2Shape);
    setInt(processor, swaraxt::IDs::mixBalance, 0);
    setInt(processor, swaraxt::IDs::mixSub, tone.mixSub);
    setInt(processor, swaraxt::IDs::mixNoise, tone.mixNoise);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::env2Release, 20);
    setInt(processor, swaraxt::IDs::env1Attack, 0);
    setInt(processor, swaraxt::IDs::env1Decay, 40);
    setInt(processor, swaraxt::IDs::env1Sustain, 100);
    setInt(processor, swaraxt::IDs::env1Release, 20);
    setInt(processor, swaraxt::IDs::seqMode, 0);
    setInt(processor, swaraxt::IDs::midiChannel, 0);
    setChoice(processor, swaraxt::IDs::dspFxProgram, tone.fxChoice);
    setInt(processor, swaraxt::IDs::dspFxParam1, tone.fxParam1);
    setInt(processor, swaraxt::IDs::dspFxParam2, tone.fxParam2);
    if (tone.matrixCutoff)
    {
        const juce::String row = "mod.row1.";
        setInt(processor, (row + "source").toRawUTF8(), shruthi::MOD_SRC_LFO_1);
        setInt(processor, (row + "destination").toRawUTF8(), shruthi::MOD_DST_FILTER_CUTOFF);
        setInt(processor, (row + "amount").toRawUTF8(), 40);
    }
    processor.prepareToPlay(tone.hostRate, std::max(1, tone.blockSize));
    render.prepared = processor.getLatencySamples() >= 0 && tone.hostRate >= 8000.0;
    render.reportedLatency = processor.getLatencySamples();
    render.expectedLatency = swaraxt::SwaraXtEngine::supportsHostSampleRate(tone.hostRate)
        ? static_cast<int>(std::ceil(swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples
                                     * tone.hostRate / swaraxt::SwaraXtEngine::kInternalSampleRate))
        : 0;
    const int total = std::max(0, tone.totalSamples);
    const int blockSize = std::max(1, tone.blockSize);
    render.left.reserve(static_cast<size_t>(total));
    render.right.reserve(static_cast<size_t>(total));
    juce::AudioBuffer<float> buffer(2, blockSize);
    int rendered = 0;
    while (rendered < total)
    {
        const int count = std::min(blockSize, total - rendered);
        buffer.setSize(2, count, false, false, true);
        buffer.clear();
        setFloat(processor, swaraxt::IDs::filterCutoff, requestedCutoff(tone, rendered));
        juce::MidiBuffer midi;
        if (tone.note >= 0 && rendered == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, tone.note, (juce::uint8) 100), 0);
        if (tone.note >= 0 && tone.noteOffSample >= rendered && tone.noteOffSample < rendered + count)
            midi.addEvent(juce::MidiMessage::noteOff(1, tone.note), tone.noteOffSample - rendered);
        processor.processBlock(buffer, midi);
        for (int i = 0; i < count; ++i)
        {
            const float left = buffer.getSample(0, i);
            const float right = buffer.getSample(1, i);
            if (!std::isfinite(left) || !std::isfinite(right))
                render.finite = false;
            if (left != right)
                render.rightMatchesLeft = false;
            render.left.push_back(left);
            render.right.push_back(right);
        }
        rendered += count;
    }
    processor.releaseResources();
    render.renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return render;
}

ReprepareRender renderPluginReprepare()
{
    ReprepareRender render;
    SwaraXtAudioProcessor processor;
    processor.setNonRealtime(true);
    processor.setFilterQuality(swaraxt::FilterQuality::normal);
    silenceMatrix(processor);
    setFloat(processor, swaraxt::IDs::master, 0.85f);
    setFloat(processor, swaraxt::IDs::filterCutoff, 8000.0f);
    setFloat(processor, swaraxt::IDs::filterResonance, 0.2f);
    setInt(processor, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
    setInt(processor, swaraxt::IDs::osc2Shape, shruthi::WAVEFORM_NONE);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::seqMode, 0);
    setChoice(processor, swaraxt::IDs::dspFxProgram, 0);
    const auto renderRate = [&](double rate, int& latency, int& expected, bool& audible) {
        processor.prepareToPlay(rate, 64);
        latency = processor.getLatencySamples();
        expected = static_cast<int>(std::ceil(swaraxt::SwaraXtEngine::kOutputLatencyNativeSamples
                                              * rate / swaraxt::SwaraXtEngine::kInternalSampleRate));
        juce::AudioBuffer<float> buffer(2, 64);
        double energy = 0.0;
        for (int block = 0; block < 80; ++block)
        {
            buffer.clear();
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            processor.processBlock(buffer, midi);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float sample = buffer.getSample(0, i);
                if (!std::isfinite(sample) || sample != buffer.getSample(1, i))
                    render.finite = false;
                energy += static_cast<double>(sample) * static_cast<double>(sample);
            }
        }
        audible = energy > 1.0e-8;
    };
    renderRate(48000.0, render.latencyBefore, render.expectedBefore, render.audibleBefore);
    renderRate(96000.0, render.latencyAfter, render.expectedAfter, render.audibleAfter);
    processor.releaseResources();
    return render;
}

}  // namespace swaraxt::sonic
