// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/FreeCvPitch.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/ScalePitch.h"
#include "shruthi/sequencer_settings.h"

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

void setInt(SwaraXtAudioProcessor& processor, const char* id, int value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

double estimateHz(const std::vector<float>& mono, double sampleRate)
{
    if (mono.size() < 2048)
        return 0.0;
    double mean = 0.0;
    for (float sample : mono)
        mean += sample;
    mean /= static_cast<double>(mono.size());
    const int minLag = static_cast<int>(sampleRate / 1200.0);
    const int maxLag = static_cast<int>(sampleRate / 40.0);
    double best = -1.0;
    int lag = 0;
    for (int candidate = minLag; candidate <= maxLag; ++candidate)
    {
        double corr = 0.0;
        double e0 = 0.0;
        double e1 = 0.0;
        const int count = static_cast<int>(mono.size()) - candidate;
        for (int i = 0; i < count; ++i)
        {
            const double a = mono[static_cast<size_t>(i)] - mean;
            const double b = mono[static_cast<size_t>(i + candidate)] - mean;
            corr += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        const double denom = std::sqrt(e0 * e1);
        if (denom > 1.0e-12 && corr / denom > best)
        {
            best = corr / denom;
            lag = candidate;
        }
    }
    return lag > 0 ? sampleRate / static_cast<double>(lag) : 0.0;
}

void prepareSaw(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::osc1Shape, 1);
    setInt(processor, swaraxt::IDs::osc2Shape, 0);
    setInt(processor, swaraxt::IDs::mixBalance, 0);
    setInt(processor, swaraxt::IDs::mixSub, 0);
    setInt(processor, swaraxt::IDs::mixNoise, 0);
    setInt(processor, swaraxt::IDs::env1Attack, 0);
    setInt(processor, swaraxt::IDs::env1Decay, 0);
    setInt(processor, swaraxt::IDs::env1Sustain, 127);
    setInt(processor, swaraxt::IDs::env1Release, 0);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::env2Release, 0);
    setInt(processor, swaraxt::IDs::filterEnvDepth, 0);
    setInt(processor, swaraxt::IDs::filterLfoDepth, 0);
    setInt(processor, swaraxt::IDs::dspFxProgram, 0);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    if (auto* cutoff = processor.getApvts().getParameter(swaraxt::IDs::filterCutoff))
        cutoff->setValueNotifyingHost(1.0f);
    if (auto* resonance = processor.getApvts().getParameter(swaraxt::IDs::filterResonance))
        resonance->setValueNotifyingHost(0.0f);
}

double renderDegreeHz(SwaraXtAudioProcessor& processor, int degree)
{
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setScaleDegree(0, degree);
    for (int step = 0; step < 16; ++step)
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
    processor.sequenceState().store(snapshot);
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setScaleDegree(0, degree);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 40);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    processor.prepareToPlay(48000.0, 64);
    prepareSaw(processor);
    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Start(false);
    bool armed = false;
    for (int tick = 0; tick < 32 && ! armed; ++tick)
    {
        part.Clock(false);
        armed = part.voice().pitch_value() != 0;
    }
    std::vector<float> mono;
    mono.reserve(48000);
    for (int generated = 0; generated < 48000; generated += 64)
    {
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (int i = 0; i < 64; ++i)
            mono.push_back(0.5f * (buffer.getSample(0, i) + buffer.getSample(1, i)));
    }
    if (mono.size() > 12000)
        mono.erase(mono.begin(), mono.begin() + 12000);
    const auto word = part.voice().pitch_value();
    const double hz = estimateHz(mono, 48000.0);
    std::printf("degree %d voice=%u measured=%.3f Hz\n", degree, static_cast<unsigned>(word), hz);
    part.Stop(false);
    return hz;
}

void testTwelveEdoReference()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(0), "12-EDO builtin loads");
    const double target = processor.hzForActiveDegree(9);
    expect(std::abs(target - 440.0) < 1.0e-4, "12-EDO degree 9 is A4");
    const double tonic = processor.hzForActiveDegree(0);
    expect(std::abs(tonic / 440.0 - std::pow(2.0, -9.0 / 12.0)) < 1.0e-6, "degree 0 is nine semitones below A4");
    const auto word = swaraxt::ScalePitch::toVoicePitchFromHz(target);
    expect(word == 69 * 128, "A4 uses the integer 69 voice word");
    const double measured = renderDegreeHz(processor, 9);
    const double cents = 1200.0 * std::log2(measured / target);
    std::printf("12-EDO A4 target=%.3f measured=%.3f cents=%.3f\n", target, measured, cents);
    expect(std::abs(cents) < 8.0, "rendered A4 is within the voice-word tolerance");
}

void testNonOctaveAndEdo()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(1), "19-EDO loads");
    const double root = processor.hzForActiveDegree(0);
    const double period = processor.hzForActiveDegree(19);
    expect(std::abs(period / root - 2.0) < 1.0e-4, "19 degrees of 19-EDO double the frequency");
    const double below = processor.hzForActiveDegree(-1);
    expect(below < root && below > 0.0, "negative degree is below the tonic");

    expect(processor.loadBuiltinTuning(5), "tritave loads");
    const double tritaveRoot = processor.hzForActiveDegree(0);
    const double tritave = processor.hzForActiveDegree(3);
    expect(std::abs(tritave / tritaveRoot - 3.0) < 1.0e-3, "tritave period is 3/1");
    expect(std::abs(1200.0 * std::log2(tritave / tritaveRoot) - 1200.0) > 100.0,
           "tritave period is not an octave");
}

void testDegreeChangeWithSameIdentity()
{
    SwaraXtAudioProcessor processor;
    processor.loadBuiltinTuning(0);
    processor.prepareToPlay(48000.0, 64);
    prepareSaw(processor);
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setStep(0, 0x80 | 60, 0x70);
    processor.sequenceState().setStep(1, 0x80 | 60, 0xf0);
    processor.sequenceState().setScaleDegree(0, 0);
    processor.sequenceState().setScaleDegree(1, 9);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 2;
    const auto packed = snapshot.steps;
    processor.sequenceState().store(snapshot);
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setScaleDegree(0, 0);
    processor.sequenceState().setScaleDegree(1, 9);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqTempo, 200);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Start(false);
    bool heardRoot = false;
    bool heardFifth = false;
    const auto rootWord = swaraxt::ScalePitch::toVoicePitchFromHz(processor.hzForActiveDegree(0));
    const auto aWord = swaraxt::ScalePitch::toVoicePitchFromHz(processor.hzForActiveDegree(9));
    for (int tick = 0; tick < 80; ++tick)
    {
        part.Clock(false);
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        const auto pitch = part.voice().pitch_value();
        if (pitch == rootWord)
            heardRoot = true;
        if (pitch == aWord)
            heardFifth = true;
    }
    expect(heardRoot && heardFifth, "same packed note still reaches a new scale degree");
    expect(processor.sequenceState().snapshot().steps == packed, "scale playback does not rewrite packed notes");
    part.Stop(false);
}

void testModeIsolationAndRandom()
{
    swaraxt::SequenceState state;
    const auto packed = state.snapshot().steps;
    state.setPitchMode(1);
    const auto cv = state.snapshot().freePitch;
    int16_t map[128];
    for (int note = 0; note < 128; ++note)
        map[note] = static_cast<int16_t>(note - 60);
    state.setPitchMode(2, map);
    expect(state.snapshot().steps == packed, "entering SCALE keeps packed notes");
    expect(state.snapshot().freePitch == cv, "entering SCALE keeps LINEAR CV");
    expect(state.snapshot().scaleReady == 1, "SCALE degrees initialise once");
    state.setPitchMode(0);
    expect(state.snapshot().scaleDegree == state.snapshot().scaleDegree, "legacy switch keeps degrees");
    const auto degrees = state.snapshot().scaleDegree;
    state.setPitchMode(2);
    expect(state.snapshot().scaleDegree == degrees, "returning to SCALE restores degrees");

    juce::Random random(0x5CA1Eu);
    swaraxt::SequenceState::ScaleRandomView view;
    view.formalPeriod = 12;
    view.periodCents = 1200.0;
    const double cents[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 };
    view.degreeCents = cents;
    view.degreeCount = 11;
    const auto before = state.snapshot().steps;
    const auto beforeCv = state.snapshot().freePitch;
    state.randomize(swaraxt::SequenceState::Randomize::notes, random,
                    { -12, 12 }, swaraxt::SequenceState::RandomizationType::type1, &view);
    expect(state.snapshot().steps == before, "SCALE random notes keep packed notes");
    expect(state.snapshot().freePitch == beforeCv, "SCALE random notes keep CV");
    expect(state.snapshot().scaleDegree != degrees, "SCALE random notes change degrees");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testTwelveEdoReference();
    testNonOctaveAndEdo();
    testDegreeChangeWithSameIdentity();
    testModeIsolationAndRandom();
    if (failures == 0)
    {
        std::printf("scale sequencer tests: PASSED\n");
        return 0;
    }
    std::printf("scale sequencer tests: FAILED (%d)\n", failures);
    return 1;
}
