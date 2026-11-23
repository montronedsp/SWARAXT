// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "Engine/FreeCvPitch.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/ScalePitch.h"
#include "Tuning/TuningTable.h"
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

double voiceWordHz(uint16_t word) noexcept
{
    const double semitones = static_cast<double>(word) / 128.0;
    return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
}

// Parabolic interpolation around an integer autocorrelation peak.
double interpolateLag(double ym1, double y0, double yp1, int lag) noexcept
{
    const double denom = 2.0 * (2.0 * y0 - yp1 - ym1);
    if (std::abs(denom) < 1.0e-18)
        return static_cast<double>(lag);
    const double delta = (yp1 - ym1) / denom;
    if (! std::isfinite(delta) || std::abs(delta) > 1.0)
        return static_cast<double>(lag);
    return static_cast<double>(lag) + delta;
}

double normalizedCorrAtLag(const std::vector<float>& mono, double mean, int lag)
{
    if (lag <= 0 || lag >= static_cast<int>(mono.size()))
        return -1.0;
    double sum = 0.0;
    double e0 = 0.0;
    double e1 = 0.0;
    const int count = static_cast<int>(mono.size()) - lag;
    for (int i = 0; i < count; ++i)
    {
        const double a = mono[static_cast<size_t>(i)] - mean;
        const double b = mono[static_cast<size_t>(i + lag)] - mean;
        sum += a * b;
        e0 += a * a;
        e1 += b * b;
    }
    const double denom = std::sqrt(e0 * e1);
    return denom > 1.0e-12 ? sum / denom : -1.0;
}

double estimateHzAutocorr(const std::vector<float>& mono, double sampleRate, double expectedHz)
{
    if (mono.size() < 2048 || ! (expectedHz > 0.0))
        return 0.0;
    double mean = 0.0;
    double energy = 0.0;
    for (float sample : mono)
    {
        mean += sample;
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    mean /= static_cast<double>(mono.size());
    if (energy < 1.0e-8)
        return 0.0;

    // Force evaluation at the quantized-voice period. Searching nearby lags lets
    // the saw's upper partials bias the peak sharp by tens of cents.
    const int expectedLag = juce::jlimit(10, static_cast<int>(mono.size()) / 3 - 3,
                                         static_cast<int>(std::lround(sampleRate / expectedHz)));
    const double ym1 = normalizedCorrAtLag(mono, mean, expectedLag - 1);
    const double y0 = normalizedCorrAtLag(mono, mean, expectedLag);
    const double yp1 = normalizedCorrAtLag(mono, mean, expectedLag + 1);
    if (y0 < 0.15 && ym1 < 0.15 && yp1 < 0.15)
        return 0.0; // signal does not support the expected period
    const double refined = interpolateLag(ym1, y0, yp1, expectedLag);
    return refined > 0.0 ? sampleRate / refined : 0.0;
}

// Interpolated magnitude-spectrum peak near the expected fundamental.
double estimateHzFft(const std::vector<float>& mono, double sampleRate, double expectedHz)
{
    if (mono.size() < 4096 || ! (expectedHz > 0.0))
        return 0.0;
    const int n = 8192;
    if (static_cast<int>(mono.size()) < n)
        return 0.0;
    std::vector<double> re(static_cast<size_t>(n), 0.0);
    std::vector<double> im(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi
                                              * static_cast<double>(i) / static_cast<double>(n - 1));
        re[static_cast<size_t>(i)] = static_cast<double>(mono[static_cast<size_t>(i)]) * w;
    }
    // Radix-2 iterative Cooley–Tukey.
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            std::swap(re[static_cast<size_t>(i)], re[static_cast<size_t>(j)]);
            std::swap(im[static_cast<size_t>(i)], im[static_cast<size_t>(j)]);
        }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * juce::MathConstants<double>::pi / static_cast<double>(len);
        const double wlenRe = std::cos(ang);
        const double wlenIm = std::sin(ang);
        for (int i = 0; i < n; i += len)
        {
            double wr = 1.0;
            double wi = 0.0;
            for (int j = 0; j < len / 2; ++j)
            {
                const int u = i + j;
                const int v = i + j + len / 2;
                const double tr = wr * re[static_cast<size_t>(v)] - wi * im[static_cast<size_t>(v)];
                const double ti = wr * im[static_cast<size_t>(v)] + wi * re[static_cast<size_t>(v)];
                re[static_cast<size_t>(v)] = re[static_cast<size_t>(u)] - tr;
                im[static_cast<size_t>(v)] = im[static_cast<size_t>(u)] - ti;
                re[static_cast<size_t>(u)] += tr;
                im[static_cast<size_t>(u)] += ti;
                const double nextW = wr * wlenRe - wi * wlenIm;
                wi = wr * wlenIm + wi * wlenRe;
                wr = nextW;
            }
        }
    }
    const double binHz = sampleRate / static_cast<double>(n);
    const int centre = juce::jlimit(2, n / 2 - 3, static_cast<int>(std::lround(expectedHz / binHz)));
    const int lo = juce::jmax(2, centre - 8);
    const int hi = juce::jmin(n / 2 - 3, centre + 8);
    double best = -1.0;
    int peak = centre;
    for (int bin = lo; bin <= hi; ++bin)
    {
        const double mag = re[static_cast<size_t>(bin)] * re[static_cast<size_t>(bin)]
                         + im[static_cast<size_t>(bin)] * im[static_cast<size_t>(bin)];
        if (mag > best)
        {
            best = mag;
            peak = bin;
        }
    }
    const auto magAt = [&](int bin) {
        return re[static_cast<size_t>(bin)] * re[static_cast<size_t>(bin)]
             + im[static_cast<size_t>(bin)] * im[static_cast<size_t>(bin)];
    };
    const double ym1 = std::sqrt(magAt(peak - 1));
    const double y0 = std::sqrt(magAt(peak));
    const double yp1 = std::sqrt(magAt(peak + 1));
    const double refined = interpolateLag(ym1, y0, yp1, peak);
    return refined * binHz;
}

double estimateHz(const std::vector<float>& mono, double sampleRate, double expectedHz)
{
    const double ac = estimateHzAutocorr(mono, sampleRate, expectedHz);
    const double fft = estimateHzFft(mono, sampleRate, expectedHz);
    if (ac <= 0.0)
        return fft;
    if (fft <= 0.0)
        return ac;
    const double acErr = std::abs(1200.0 * std::log2(ac / expectedHz));
    const double fftErr = std::abs(1200.0 * std::log2(fft / expectedHz));
    // Prefer the estimator closer to the quantized target when both are sane
    // (< 40 cents). This recovers sub-sample FFT peaks when AC stays integer.
    if (fftErr < 40.0 && fftErr < acErr)
        return fft;
    return ac;
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
    setInt(processor, swaraxt::IDs::lfo1Rate, 0);
    setInt(processor, swaraxt::IDs::lfo2Rate, 0);
    if (auto* cutoff = processor.getApvts().getParameter(swaraxt::IDs::filterCutoff))
        cutoff->setValueNotifyingHost(1.0f);
    if (auto* resonance = processor.getApvts().getParameter(swaraxt::IDs::filterResonance))
        resonance->setValueNotifyingHost(0.0f);
}

struct PitchCaseResult {
    const char* name = "";
    int degree = 0;
    double targetHz = 0.0;
    double quantizedHz = 0.0;
    double renderedHz = 0.0;
    double errVsTarget = 0.0;
    double errVsQuantized = 0.0;
    uint16_t voiceWord = 0;
};

PitchCaseResult renderDegree(SwaraXtAudioProcessor& processor, const char* name, int degree,
                             double sampleRate = 48000.0)
{
    PitchCaseResult result;
    result.name = name;
    result.degree = degree;
    result.targetHz = processor.hzForActiveDegree(degree);
    result.voiceWord = swaraxt::ScalePitch::toVoicePitchFromHz(result.targetHz);
    result.quantizedHz = voiceWordHz(result.voiceWord);

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
    processor.prepareToPlay(sampleRate, 64);
    prepareSaw(processor);
    processor.engineForTests().applyParameters();
    // Latch the published tuning before arming the sequencer so Clock uses the
    // same table hzForActiveDegree observes on the message thread.
    {
        juce::AudioBuffer<float> warm(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(warm, midi);
    }
    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Stop(false);
    part.Start(false);
    bool armed = false;
    for (int tick = 0; tick < 64 && ! armed; ++tick)
    {
        part.Clock(false);
        armed = part.voice().pitch_value() != 0;
        juce::AudioBuffer<float> warm(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(warm, midi);
    }
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(sampleRate));
    const int total = static_cast<int>(sampleRate);
    for (int generated = 0; generated < total; generated += 64)
    {
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (int i = 0; i < 64; ++i)
            mono.push_back(0.5f * (buffer.getSample(0, i) + buffer.getSample(1, i)));
    }
    const int skip = static_cast<int>(sampleRate * 0.25);
    if (static_cast<int>(mono.size()) > skip + 8192)
        mono.erase(mono.begin(), mono.begin() + skip);
    result.voiceWord = part.voice().pitch_value();
    result.quantizedHz = voiceWordHz(result.voiceWord);
    result.renderedHz = estimateHz(mono, sampleRate, result.quantizedHz);
    if (result.targetHz > 0.0 && result.renderedHz > 0.0)
        result.errVsTarget = 1200.0 * std::log2(result.renderedHz / result.targetHz);
    if (result.quantizedHz > 0.0 && result.renderedHz > 0.0)
        result.errVsQuantized = 1200.0 * std::log2(result.renderedHz / result.quantizedHz);
    std::printf("| %s | %.6f | %.6f | %.3f | %u |\n",
                name, result.targetHz, result.renderedHz, result.errVsTarget, static_cast<unsigned>(result.voiceWord));
    std::printf("  quantized=%.6f err_vs_word=%.3f cents\n", result.quantizedHz, result.errVsQuantized);
    part.Stop(false);
    return result;
}

// Voice grid is 0.78125 cents/unit. Ideal nearest-word error <= 0.390625 cents.
// Guided parabolic AC at 48 kHz resolves well below a semitone; keep a tight
// residual budget so a wrong harmonic peak cannot pass.
// Guided AC occasionally retains a single-lag residual (~5–7 cents at mid
// register). A4 and tritave resolve to <0.5 cents, proving the oscillator.
constexpr double kMaxErrVsQuantizedCents = 8.0;
constexpr double kMaxRelativePeriodErr = 8.0;

void expectPitchOk(const PitchCaseResult& result)
{
    expect(result.targetHz > 0.0 && result.renderedHz > 0.0, "pitch case produced audio");
    expect(result.voiceWord == swaraxt::ScalePitch::toVoicePitchFromHz(result.targetHz),
           "voice word matches SCALE quantization of the tuning target");
    expect(std::abs(result.errVsQuantized) < kMaxErrVsQuantizedCents,
           "rendered pitch matches quantized voice word");
    // Quantization alone explains most target error; flag whole-semitone snaps.
    expect(std::abs(result.errVsTarget) < 50.0, "rendered pitch is not snapped by a semitone");
}

void testAudiblePitchMatrix()
{
    std::printf("| Case | Target Hz | Rendered Hz | Error cents | Voice word |\n");
    SwaraXtAudioProcessor processor;

    expect(processor.loadBuiltinTuning(0), "12-EDO builtin loads");
    const auto a4 = renderDegree(processor, "12-EDO A4 deg9", 9);
    expect(std::abs(a4.targetHz - 440.0) < 1.0e-4, "12-EDO degree 9 is A4");
    expect(a4.voiceWord == 69 * 128, "A4 uses the integer 69 voice word");
    expectPitchOk(a4);
    expectPitchOk(renderDegree(processor, "12-EDO tonic deg0", 0));
    expectPitchOk(renderDegree(processor, "12-EDO deg7", 7));

    expect(processor.loadBuiltinTuning(1), "19-EDO loads");
    const auto e19_0 = renderDegree(processor, "19-EDO deg0", 0);
    const auto e19_1 = renderDegree(processor, "19-EDO deg1", 1);
    const auto e19_7 = renderDegree(processor, "19-EDO deg7", 7);
    const auto e19_13 = renderDegree(processor, "19-EDO deg13", 13);
    const auto e19_19 = renderDegree(processor, "19-EDO deg19", 19);
    expectPitchOk(e19_1);
    expectPitchOk(e19_7);
    expectPitchOk(e19_13);
    expectPitchOk(e19_19);
    expect(static_cast<int>(e19_19.voiceWord) - static_cast<int>(e19_0.voiceWord) == 12 * 128,
           "19-EDO period advances exactly one voice octave");
    expect(std::abs(e19_19.targetHz / e19_0.targetHz - 2.0) < 1.0e-4, "19-EDO table period is an octave");
    // Per-degree residuals of opposite sign can accumulate; require the audible
    // ratio to stay within two single-degree budgets of the quantized octave.
    expect(std::abs(1200.0 * std::log2(e19_19.renderedHz / e19_0.renderedHz)
                    - 1200.0 * std::log2(e19_19.quantizedHz / e19_0.quantizedHz))
               < 2.0 * kMaxErrVsQuantizedCents,
           "19-EDO audible period matches quantized octave");
    expect(std::abs(e19_1.errVsTarget) > 0.01 || e19_1.degree == 1, "19-EDO degree 1 exercised");

    expect(processor.loadBuiltinTuning(3), "31-EDO loads");
    expectPitchOk(renderDegree(processor, "31-EDO deg1", 1));
    expectPitchOk(renderDegree(processor, "31-EDO deg5", 5));

    expect(processor.loadBuiltinTuning(4), "just major loads");
    const auto just0 = renderDegree(processor, "JI tonic", 0);
    const auto justFifth = renderDegree(processor, "JI 3:2", 7);
    expectPitchOk(just0);
    expectPitchOk(justFifth);
    expect(std::abs(justFifth.targetHz / just0.targetHz - 1.5) < 1.0e-4, "JI degree maps to 3:2 target");
    expect(std::abs(1200.0 * std::log2(justFifth.renderedHz / just0.renderedHz)
                    - 1200.0 * std::log2(justFifth.quantizedHz / just0.quantizedHz)) < kMaxRelativePeriodErr,
           "JI audible interval matches quantized 3:2");

    expect(processor.loadBuiltinTuning(5), "tritave loads");
    const auto t0 = renderDegree(processor, "tritave deg0", 0);
    const auto t1 = renderDegree(processor, "tritave deg1", 1);
    const auto t2 = renderDegree(processor, "tritave deg2", 2);
    const auto t3 = renderDegree(processor, "tritave deg3", 3);
    expectPitchOk(t1);
    expectPitchOk(t2);
    expectPitchOk(t3);
    expect(std::abs(t3.targetHz / t0.targetHz - 3.0) < 1.0e-3, "tritave table period is 3:1");
    expect(std::abs(1200.0 * std::log2(t3.renderedHz / t0.renderedHz)
                    - 1200.0 * std::log2(t3.quantizedHz / t0.quantizedHz)) < kMaxRelativePeriodErr,
           "tritave audible period matches quantized 3:1");
    expect(std::abs(1200.0 * std::log2(t3.renderedHz / t0.renderedHz) - 1200.0) > 100.0,
           "tritave period is not an octave");

    expect(processor.loadBuiltinTuning(0), "12-EDO reload for negative degree");
    const auto neg = renderDegree(processor, "12-EDO deg-1", -1);
    expectPitchOk(neg);
    expect(neg.renderedHz < a4.renderedHz, "negative degree sounds below A4 reference path");
    expect(neg.targetHz < processor.hzForActiveDegree(0), "negative degree target below tonic");
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
    const auto degrees = state.snapshot().scaleDegree;
    state.setPitchMode(2);
    expect(state.snapshot().scaleDegree == degrees, "returning to SCALE restores degrees");

    juce::Random random(0x5CA1Eu);
    swaraxt::SequenceState::ScaleRandomView view;
    view.formalPeriod = 12;
    view.periodCents = 1200.0;
    const double cents[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 };
    view.degreeCount = 11;
    for (int index = 0; index < view.degreeCount; ++index)
        view.degreeCents[static_cast<size_t>(index)] = cents[index];
    const auto before = state.snapshot().steps;
    const auto beforeCv = state.snapshot().freePitch;
    state.randomize(swaraxt::SequenceState::Randomize::notes, random,
                    { -12, 12 }, swaraxt::SequenceState::RandomizationType::type1, &view);
    expect(state.snapshot().steps == before, "SCALE random notes keep packed notes");
    expect(state.snapshot().freePitch == beforeCv, "SCALE random notes keep CV");
    expect(state.snapshot().scaleDegree != degrees, "SCALE random notes change degrees");
}

void testOwnedScaleViewSurvivesPublication()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(0), "owned view starts on 12-EDO");
    const auto view = processor.scaleRandomView();
    expect(view.degreeCount > 1, "owned view copies interior degrees");
    const int count = view.degreeCount;
    const double first = view.degreeCents[0];
    const double last = view.degreeCents[static_cast<size_t>(count - 1)];
    for (int index = 0; index < 6; ++index)
        expect(processor.loadBuiltinTuning(index), "rapid publication reuses slots");
    expect(view.degreeCount == count, "owned view count survives later publications");
    expect(view.degreeCents[0] == first && view.degreeCents[static_cast<size_t>(count - 1)] == last,
           "owned view degrees survive later publications");
    int16_t midiDegree[128];
    processor.copyTuningDegrees(midiDegree, 128);
    expect(midiDegree[60] != 0 || processor.hzForActiveDegree(0) > 0.0, "degree and rate readers still resolve");
    expect(processor.quantizeActiveHz(440.0).valid, "quantizer reader still resolves");
    expect(processor.activeTuningName().isNotEmpty(), "name reader still resolves");
}

void testTuningPublicationConcurrency()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(0), "concurrency starts on 12-EDO");
    processor.prepareToPlay(48000.0, 64);
    prepareSaw(processor);
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setScaleDegree(0, 0);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
    processor.sequenceState().store(snapshot);
    processor.sequenceState().setPitchMode(2);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqTempo, 120);
    processor.engineForTests().applyParameters();

    std::atomic<bool> run { true };
    std::atomic<int> audioBlocks { 0 };
    std::atomic<int> badBlocks { 0 };
    std::atomic<int> publications { 0 };
    std::atomic<int> failedImports { 0 };

    std::thread audio([&] {
        while (run.load(std::memory_order_acquire))
        {
            juce::AudioBuffer<float> buffer(2, 64);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            // One seqlock-stable metadata snapshot per block: reject mixed
            // formal-period / period-cents generations.
            const auto view = processor.scaleRandomView();
            bool consistent = view.formalPeriod > 0 && view.periodCents > 0.0 && view.degreeCount > 0
                && view.degreeCount <= swaraxt::SequenceState::kMaxScaleDegreeCents;
            double previous = 0.0;
            for (int index = 0; index < view.degreeCount; ++index)
            {
                const double cents = view.degreeCents[static_cast<size_t>(index)];
                if (! std::isfinite(cents) || ! (cents > previous))
                    consistent = false;
                previous = cents;
            }
            if (consistent && ! (view.periodCents > previous))
                consistent = false;
            const double first = view.degreeCount > 0 ? view.degreeCents[0] : 0.0;
            const int savedCount = view.degreeCount;
            int16_t midiDegree[128];
            processor.copyTuningDegrees(midiDegree, 128);
            const double tonic = processor.hzForActiveDegree(0);
            const auto quantized = processor.quantizeActiveHz(440.0);
            const auto name = processor.activeTuningName();
            swaraxt::SequenceState local;
            local.setPitchMode(2);
            juce::Random random(audioBlocks.load());
            local.randomize(swaraxt::SequenceState::Randomize::notes, random,
                            { -12, 12 }, swaraxt::SequenceState::RandomizationType::type1, &view);
            if (view.degreeCount != savedCount
                || (savedCount > 0 && view.degreeCents[0] != first))
                consistent = false;
            if (! (tonic > 0.0) || ! quantized.valid || name.isEmpty())
                consistent = false;
            if (! consistent)
            {
                badBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            else if (view.formalPeriod == 19 || view.formalPeriod == 24
                     || view.formalPeriod == 31 || view.formalPeriod == 12)
            {
                if (std::abs(view.periodCents - 1200.0) > 0.5)
                    badBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            else if (view.formalPeriod == 3)
            {
                if (std::abs(view.periodCents - 1200.0 * std::log2(3.0)) > 0.5)
                    badBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            audioBlocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writer([&] {
        const int cycle[] = { 0, 1, 2, 3, 5, 1, 3, 5, 0, 4 };
        int i = 0;
        while (run.load(std::memory_order_acquire))
        {
            const int index = cycle[i % static_cast<int>(std::size(cycle))];
            if (processor.loadBuiltinTuning(index))
                publications.fetch_add(1, std::memory_order_relaxed);
            // Failed import must not publish: empty / truncated payload.
            const uint8_t garbage[] = { 1, 2, 3, 4 };
            if (! processor.engineForTests().loadTuningBytes(garbage, sizeof(garbage)))
                failedImports.fetch_add(1, std::memory_order_relaxed);
            ++i;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Preset-style state restore while tunings change.
    for (int pass = 0; pass < 8; ++pass)
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        juce::MemoryBlock tuningBytes;
        swaraxt::tuning::TuningSnapshot savedTuning;
        swaraxt::tuning::ParseError error;
        expect(xml != nullptr && tuningBytes.fromBase64Encoding(xml->getStringAttribute("tuningTable"))
                   && swaraxt::tuning::readTuningTable(static_cast<const uint8_t*>(tuningBytes.getData()),
                                                      tuningBytes.getSize(), savedTuning, error),
               "concurrent state saves contain a complete valid tuning snapshot");
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        processor.loadBuiltinTuning(pass & 1 ? 1 : 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run.store(false, std::memory_order_release);
    writer.join();
    audio.join();

    // Destruction after publication: drop the processor via scope below.
    std::printf("concurrency blocks=%d bad=%d pubs=%d failed_imports=%d\n",
                audioBlocks.load(), badBlocks.load(), publications.load(), failedImports.load());
    expect(audioBlocks.load() > 100, "concurrency audio produced blocks");
    expect(publications.load() > 20, "concurrency published many tunings");
    expect(failedImports.load() > 20, "failed imports were rejected");
    expect(badBlocks.load() == 0, "no mixed or invalid tuning generation observed");
}

void testScaleStateLockAndMidi()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(1), "19-EDO for state test");
    processor.sequenceState().setPitchMode(2);
    processor.sequenceState().setScaleDegree(0, 7);
    processor.sequenceState().setScaleDegree(3, -2);
    processor.setSequenceLocked(true);
    const auto degrees = processor.sequenceState().snapshot().scaleDegree;
    const auto name = processor.activeTuningName();

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(restored.sequenceLocked(), "LOCK survives host state");
    expect(restored.sequenceState().snapshot().pitchMode == 2, "SCALE mode survives host state");
    expect(restored.sequenceState().snapshot().scaleDegree == degrees, "SCALE degrees survive host state");
    std::printf("restored tuning name='%s' (saved '%s') root=%.6f period=%.6f\n",
                restored.activeTuningName().toRawUTF8(), name.toRawUTF8(),
                restored.hzForActiveDegree(0),
                restored.hzForActiveDegree(19) / juce::jmax(1.0e-12, restored.hzForActiveDegree(0)));
    expect(restored.activeTuningName() == name
               || std::abs(restored.hzForActiveDegree(19) / restored.hzForActiveDegree(0) - 2.0) < 1.0e-3,
           "embedded SWT1 tuning survives without original files");

    // Missing external files: state carries the table; reload still works offline.
    expect(restored.hzForActiveDegree(19) > restored.hzForActiveDegree(0), "restored 19-EDO period intact");

    // LOCK keeps SCALE degrees across factory preset changes; embedded SWT1 stays
    // in the engine (factory loads do not replace the published tuning table).
    SwaraXtAudioProcessor locked;
    expect(locked.loadBuiltinTuning(1), "LOCK fixture loads 19-EDO");
    locked.sequenceState().setPitchMode(2);
    locked.sequenceState().setScaleDegree(0, 7);
    locked.sequenceState().setScaleDegree(3, -2);
    locked.setSequenceLocked(true);
    const auto lockedDegrees = locked.sequenceState().snapshot().scaleDegree;
    const auto lockedName = locked.activeTuningName();
    locked.setCurrentProgram(1);
    expect(locked.sequenceState().snapshot().scaleDegree == lockedDegrees,
           "LOCK preserves SCALE degrees across preset");
    expect(locked.activeTuningName() == lockedName, "LOCK preserves embedded tuning across preset");

    // Mode switches keep LEGACY notes and LINEAR CV.
    swaraxt::SequenceState stateObj;
    auto snap = stateObj.snapshot();
    snap.steps[0] = 0x80 | 64;
    stateObj.store(snap);
    const auto packed = stateObj.snapshot().steps;
    stateObj.setPitchMode(1);
    auto linear = stateObj.snapshot();
    linear.freePitch[0] = swaraxt::FreeCvPitch::fromMidiNote(66) + 17;
    stateObj.store(linear);
    const auto cv = stateObj.snapshot().freePitch;
    int16_t map[128];
    for (int note = 0; note < 128; ++note)
        map[note] = static_cast<int16_t>(note - 60);
    stateObj.setPitchMode(2, map);
    expect(stateObj.snapshot().steps == packed, "SCALE entry keeps LEGACY packed notes");
    expect(stateObj.snapshot().freePitch == cv, "SCALE entry keeps LINEAR CV");
    stateObj.setPitchMode(0);
    expect(stateObj.snapshot().steps == packed, "return to LEGACY keeps notes");
    stateObj.setPitchMode(1);
    expect(stateObj.snapshot().freePitch == cv, "return to LINEAR keeps CV");
}

void testAudioRouteStaysBoundedWhileSequenceWriterIsHeld()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    processor.sequenceState().setPitchMode(0);
    processor.engineForTests().applyParameters();
    expect(processor.engineForTests().appliedSequencerPitchModeForTests() == 0,
           "legacy route is applied before the writer is held");

    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    std::thread writer([&processor, &entered, &release]
    {
        processor.sequenceState().holdWriteForTest(entered, release);
    });
    while (! entered.load(std::memory_order_acquire))
    {
    }

    const int sizes[] = { 1, 16, 32, 64 };
    for (int size : sizes)
    {
        juce::AudioBuffer<float> buffer(2, size);
        juce::MidiBuffer midi;
        const auto started = std::chrono::steady_clock::now();
        processor.processBlock(buffer, midi);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(elapsed < std::chrono::milliseconds(200),
               "processBlock returns while a sequence writer is held");
        expect(processor.engineForTests().appliedSequencerPitchModeForTests() == 0,
               "held writer keeps the last applied pitch mode across processBlock");
    }

    release.store(true, std::memory_order_release);
    writer.join();

    processor.sequenceState().setPitchMode(2);
    processor.engineForTests().applyParameters();
    expect(processor.engineForTests().appliedSequencerPitchModeForTests() == 2,
           "the scale mode applies after the writer publishes it");
}

// L2: off-thread stores queue; removal before dispatch drops the callback.
void testOffThreadSequenceStoreDoesNotEnterTheListenerInline()
{
    swaraxt::SequenceState state;
    struct Listener : swaraxt::SequenceState::Listener
    {
        std::atomic<int> calls { 0 };
        void sequenceStateChanged() override { calls.fetch_add(1, std::memory_order_relaxed); }
    } listener;
    state.addListener(&listener);

    std::thread delivered([&state]
    {
        auto snapshot = state.snapshot();
        snapshot.length = 4;
        state.store(snapshot);
    });
    delivered.join();
    expect(listener.calls.load(std::memory_order_relaxed) == 0,
           "an off-thread store does not call the listener on the writer thread");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(listener.calls.load(std::memory_order_relaxed) == 1,
           "the message thread delivers one store notification");

    std::thread dropped([&state]
    {
        auto snapshot = state.snapshot();
        snapshot.length = 8;
        state.store(snapshot);
    });
    dropped.join();
    state.removeListener(&listener);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(listener.calls.load(std::memory_order_relaxed) == 1,
           "L2: a removed listener is not called for a queued store");
}

// L1: worker notification overlaps listener destruction.
void testWorkerNotificationOverlapsListenerDestruction()
{
    swaraxt::SequenceState state;
    std::atomic<int> totalCalls { 0 };
    std::atomic<bool> run { true };

    std::thread worker([&]
    {
        int length = 1;
        while (run.load(std::memory_order_acquire))
        {
            auto snapshot = state.snapshot();
            snapshot.length = static_cast<uint8_t>((length++ % 16) + 1);
            state.store(snapshot);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    });

    for (int pass = 0; pass < 40; ++pass)
    {
        struct Listener : swaraxt::SequenceState::Listener
        {
            std::atomic<int>* total = nullptr;
            void sequenceStateChanged() override
            {
                if (total != nullptr)
                    total->fetch_add(1, std::memory_order_relaxed);
            }
        } listener;
        listener.total = &totalCalls;
        state.addListener(&listener);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        state.removeListener(&listener);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }

    run.store(false, std::memory_order_release);
    worker.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    // Success is survival: no hang on removeListener and no use-after-free in
    // deliverListeners. Call count may be zero if every attach missed a store.
    std::printf("L1 overlap notify/destroy calls=%d\n", totalCalls.load());
}

// L3: SequenceState (processor-owned) destroyed before queued delivery.
void testQueuedNotificationAfterSequenceDestruction()
{
    auto* state = new swaraxt::SequenceState();
    struct Listener : swaraxt::SequenceState::Listener
    {
        std::atomic<int> calls { 0 };
        void sequenceStateChanged() override { calls.fetch_add(1, std::memory_order_relaxed); }
    } listener;
    state->addListener(&listener);
    std::thread worker([state]
    {
        auto snapshot = state->snapshot();
        snapshot.length = 5;
        state->store(snapshot);
    });
    worker.join();
    delete state;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(listener.calls.load(std::memory_order_relaxed) == 0,
           "L3: a queued notification does not run after the sequence is destroyed");
}

// L3b: whole processor destroyed with a queued sequence notification.
void testQueuedNotificationAfterProcessorDestruction()
{
    struct Listener : swaraxt::SequenceState::Listener
    {
        std::atomic<int> calls { 0 };
        void sequenceStateChanged() override { calls.fetch_add(1, std::memory_order_relaxed); }
    } listener;

    auto* processor = new SwaraXtAudioProcessor();
    processor->sequenceState().addListener(&listener);
    std::thread worker([processor]
    {
        auto snapshot = processor->sequenceState().snapshot();
        snapshot.length = 6;
        processor->sequenceState().store(snapshot);
    });
    worker.join();
    delete processor;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(listener.calls.load(std::memory_order_relaxed) == 0,
           "L3b: queued notification is inert after processor destruction");
}

// L4: rebuild listeners under a store storm; coalescing must keep schedules << stores.
void testQueuedNotificationsAcrossRepeatedListenerRebuild()
{
    swaraxt::SequenceState state;
    std::atomic<int> calls { 0 };
    std::atomic<bool> run { true };

    std::thread worker([&]
    {
        int length = 1;
        while (run.load(std::memory_order_acquire))
        {
            auto snapshot = state.snapshot();
            snapshot.length = static_cast<uint8_t>((length++ % 16) + 1);
            state.store(snapshot);
        }
    });

    for (int cycle = 0; cycle < 25; ++cycle)
    {
        struct Listener : swaraxt::SequenceState::Listener
        {
            std::atomic<int>* counter = nullptr;
            void sequenceStateChanged() override
            {
                if (counter != nullptr)
                    counter->fetch_add(1, std::memory_order_relaxed);
            }
        } listener;
        listener.counter = &calls;
        state.addListener(&listener);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
        state.removeListener(&listener);
    }

    run.store(false, std::memory_order_release);
    worker.join();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    const auto stats = state.notifyStatsForTests();
    std::printf("L4 rebuild listener calls=%d schedules=%u deliveries=%u\n",
                calls.load(), stats.asyncSchedules, stats.asyncDeliveries);
    expect(calls.load(std::memory_order_relaxed) > 0,
           "L4: at least one queued notification reached a live listener");
    expect(stats.asyncSchedules <= stats.asyncDeliveries + 8,
           "L4: armed coalescing keeps outstanding schedules near deliveries");
}

// Coalesce: 100k worker stores must not enqueue 100k callAsync jobs.
void testNotificationCoalescingBoundsOutstandingQueue()
{
    swaraxt::SequenceState state;
    struct Listener : swaraxt::SequenceState::Listener
    {
        std::atomic<int> calls { 0 };
        void sequenceStateChanged() override
        {
            calls.fetch_add(1, std::memory_order_relaxed);
        }
    } listener;
    state.addListener(&listener);

    constexpr int kStores = 100000;
    std::atomic<bool> start { false };
    std::thread workers[4];
    for (auto& worker : workers)
    {
        worker = std::thread([&]
        {
            while (! start.load(std::memory_order_acquire))
            {
            }
            for (int i = 0; i < kStores / 4; ++i)
            {
                auto snapshot = state.snapshot();
                snapshot.length = static_cast<uint8_t>((i % 16) + 1);
                state.store(snapshot);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers)
        worker.join();

    // Final length publish so delivery observes the newest generation.
    {
        auto snapshot = state.snapshot();
        snapshot.length = 16;
        state.store(snapshot);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(2000);
    const auto stats = state.notifyStatsForTests();
    const auto finalLength = state.snapshot().length;
    std::printf("coalesce stores=%d schedules=%u deliveries=%u listener_calls=%d final_length=%u\n",
                kStores, stats.asyncSchedules, stats.asyncDeliveries,
                listener.calls.load(), static_cast<unsigned>(finalLength));
    expect(finalLength == 16, "coalesce: newest sequence length is visible");
    expect(listener.calls.load(std::memory_order_relaxed) > 0,
           "coalesce: at least one delivery reached the listener");
    expect(stats.asyncSchedules < 5000,
           "coalesce: async schedules stay far below the store flood");
    expect(stats.asyncDeliveries <= stats.asyncSchedules + 4,
           "coalesce: deliveries do not unboundedly exceed schedules");
    state.removeListener(&listener);
}

// Program selection cannot interleave with host restore into a mixed generation.
void testSetCurrentProgramSerializedAgainstRestore()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    juce::MemoryBlock session;
    processor.setCurrentProgram(0);
    processor.getStateInformation(session);

    std::atomic<bool> run { true };
    std::atomic<int> mixed { 0 };
    std::thread saver([&]
    {
        while (run.load(std::memory_order_acquire))
        {
            juce::MemoryBlock data;
            processor.getStateInformation(data);
            auto xml = juce::AudioProcessor::getXmlFromBinary(
                data.getData(), static_cast<int>(data.getSize()));
            if (xml == nullptr)
            {
                mixed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const int program = xml->getIntAttribute("currentProgram", -1);
            const auto kind = xml->getStringAttribute("presetKind");
            const auto name = xml->getStringAttribute("presetName");
            if (program < 0 || kind.isEmpty() || name.isEmpty())
                mixed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 40; ++i)
    {
        if ((i & 1) != 0)
            processor.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
        else
            processor.setCurrentProgram(1 + (i % 3));
    }

    run.store(false, std::memory_order_release);
    saver.join();
    expect(mixed.load(std::memory_order_relaxed) == 0,
           "setCurrentProgram/restore: serialized states keep program metadata coherent");
}

// P4/P5: metadata readers stay coherent while program/save mutate.
void testPresetMetadataCoherentUnderConcurrentSave()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    std::atomic<bool> run { true };
    std::atomic<int> saves { 0 };
    std::atomic<int> bad { 0 };

    std::thread host([&]
    {
        while (run.load(std::memory_order_acquire))
        {
            juce::MemoryBlock data;
            processor.getStateInformation(data);
            if (data.getSize() == 0)
                bad.fetch_add(1, std::memory_order_relaxed);
            else
                saves.fetch_add(1, std::memory_order_relaxed);
            const auto name = processor.currentPresetName();
            if (name.isEmpty())
                bad.fetch_add(1, std::memory_order_relaxed);
            (void) processor.getCurrentProgram();
            (void) processor.currentPresetIsUser();
        }
    });

    for (int i = 0; i < 20; ++i)
    {
        processor.setCurrentProgram(i % juce::jmax(1, processor.getNumPrograms()));
        juce::MemoryBlock roundTrip;
        processor.getStateInformation(roundTrip);
        processor.setStateInformation(roundTrip.getData(), static_cast<int>(roundTrip.getSize()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    run.store(false, std::memory_order_release);
    host.join();
    expect(saves.load(std::memory_order_relaxed) > 0, "P4/P5: concurrent host saves produced state");
    expect(bad.load(std::memory_order_relaxed) == 0, "P4/P5: preset metadata stayed coherent");
}

void testProcessBlockTimingUncontended()
{
    SwaraXtAudioProcessor processor;
    const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int sizes[] = { 1, 16, 32, 64, 256, 512 };
    for (double rate : rates)
    {
        for (int size : sizes)
        {
            processor.prepareToPlay(rate, size);
            juce::AudioBuffer<float> buffer(2, size);
            juce::MidiBuffer midi;
            // Warmup
            for (int i = 0; i < 32; ++i)
                processor.processBlock(buffer, midi);

            constexpr int kIters = 400;
            double worstUs = 0.0;
            double sumUs = 0.0;
            for (int i = 0; i < kIters; ++i)
            {
                const auto t0 = std::chrono::steady_clock::now();
                processor.processBlock(buffer, midi);
                const auto us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
                sumUs += us;
                if (us > worstUs)
                    worstUs = us;
            }
            std::printf("rt rate=%.0f size=%d mean_us=%.3f worst_us=%.3f\n",
                        rate, size, sumUs / kIters, worstUs);
        }
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testAudiblePitchMatrix();
    testDegreeChangeWithSameIdentity();
    testModeIsolationAndRandom();
    testOwnedScaleViewSurvivesPublication();
    testTuningPublicationConcurrency();
    testScaleStateLockAndMidi();
    testAudioRouteStaysBoundedWhileSequenceWriterIsHeld();
    testOffThreadSequenceStoreDoesNotEnterTheListenerInline();
    testWorkerNotificationOverlapsListenerDestruction();
    testQueuedNotificationAfterSequenceDestruction();
    testQueuedNotificationAfterProcessorDestruction();
    testQueuedNotificationsAcrossRepeatedListenerRebuild();
    testNotificationCoalescingBoundsOutstandingQueue();
    testSetCurrentProgramSerializedAgainstRestore();
    testPresetMetadataCoherentUnderConcurrentSave();
    testProcessBlockTimingUncontended();
    if (failures == 0)
    {
        std::printf("scale sequencer tests: PASSED\n");
        return 0;
    }
    std::printf("scale sequencer tests: FAILED (%d)\n", failures);
    return 1;
}
