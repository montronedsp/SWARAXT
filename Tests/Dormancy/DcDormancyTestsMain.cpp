// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "Engine/SampleRate/HostResampler.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

namespace {

constexpr float kSettlementThreshold = 1.0e-7f;
int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void setParameter(SwaraXtAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, "parameter exists");
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void configureFastEnvelope(SwaraXtAudioProcessor& processor)
{
    setParameter(processor, swaraxt::IDs::env2Attack, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Decay, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Sustain, 127.0f);
    setParameter(processor, swaraxt::IDs::env2Release, 0.0f);
}

bool finite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (! std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

float peak(const juce::AudioBuffer<float>& buffer)
{
    float result = 0.0f;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            result = std::max(result, std::abs(buffer.getSample(channel, sample)));
    return result;
}

void startAndRelease(SwaraXtAudioProcessor& processor,
                     juce::AudioBuffer<float>& buffer,
                     int sustainBlocks);

constexpr double kCutoffHz = 3.5;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr std::array<double, 6> kMatrixRates {
    44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0
};

double derivedCutoffHz(double sampleRate, float pole) noexcept
{
    return -sampleRate * static_cast<double>(std::log(static_cast<double>(pole))) / kTwoPi;
}

double measuredSineGainDb(swaraxt::DcBlocker& blocker, double sampleRate, double frequencyHz)
{
    const int settleSamples = static_cast<int>(std::ceil(sampleRate * 0.5));
    const int measureSamples = static_cast<int>(std::ceil(sampleRate * 1.0));
    for (int i = 0; i < settleSamples; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        blocker.process(static_cast<float>(std::sin(kTwoPi * frequencyHz * t)));
    }

    double inSq = 0.0;
    double outSq = 0.0;
    for (int i = 0; i < measureSamples; ++i)
    {
        const double t = static_cast<double>(settleSamples + i) / sampleRate;
        const float input = static_cast<float>(std::sin(kTwoPi * frequencyHz * t));
        const float output = blocker.process(input);
        inSq += static_cast<double>(input) * input;
        outSq += static_cast<double>(output) * output;
    }
    if (inSq <= 0.0 || outSq <= 0.0)
        return -200.0;
    return 20.0 * std::log10(std::sqrt(outSq / inSq));
}

struct RateMatrixResult {
    double sampleRate = 0.0;
    float storedPole = 0.0f;
    double effectiveCutoffHz = 0.0;
    double cutoffErrorHz = 0.0;
    double cutoffErrorPct = 0.0;
    double worstDcResidual = 0.0;
    bool dcReject = false;
    bool step = false;
    bool reset = false;
    bool dormancy = false;
    bool extreme = false;
    bool finite = true;
};

double dcResidualAtTime(swaraxt::DcBlocker& blocker, double sampleRate, float dc, double seconds)
{
    const int samples = static_cast<int>(std::ceil(sampleRate * seconds));
    float output = 0.0f;
    for (int i = 0; i < samples; ++i)
        output = blocker.process(dc);
    return static_cast<double>(std::abs(output));
}

bool testStepResponse(swaraxt::DcBlocker& blocker, double sampleRate)
{
    blocker.reset();
    for (int i = 0; i < 64; ++i)
        blocker.process(0.0f);

    const float firstStep = blocker.process(1.0f);
    if (std::abs(firstStep - 1.0f) > 1.0e-6f)
        return false;

    const int holdSamples = static_cast<int>(std::ceil(sampleRate * 0.05));
    float held = firstStep;
    for (int i = 1; i < holdSamples; ++i)
        held = blocker.process(1.0f);
    if (! std::isfinite(held) || std::abs(held) > 2.0f)
        return false;

    const float firstReturn = blocker.process(0.0f);
    if (! std::isfinite(firstReturn))
        return false;

    float tail = firstReturn;
    const int tailSamples = static_cast<int>(std::ceil(sampleRate * 2.0));
    for (int i = 0; i < tailSamples; ++i)
        tail = blocker.process(0.0f);
    return std::isfinite(tail) && std::abs(tail) < 1.0e-3;
}

bool testResetSemantics(swaraxt::DcBlocker& blocker)
{
    for (int i = 0; i < 128; ++i)
        blocker.process(0.5f);
    blocker.reset();
    if (blocker.process(0.0f) != 0.0f)
        return false;
    const float restarted = blocker.process(0.25f);
    return std::isfinite(restarted) && std::abs(restarted - 0.25f) < 1.0e-6f;
}

bool testExtremeInput(swaraxt::DcBlocker& blocker, double sampleRate)
{
    const int samples = static_cast<int>(std::ceil(sampleRate * 0.25));
    for (int i = 0; i < samples; ++i)
    {
        const float input = (i & 1) == 0 ? 1.0f : -1.0f;
        const float output = blocker.process(input);
        if (! std::isfinite(output) || std::abs(output) > 4.0f)
            return false;
    }
    return true;
}

bool testDormancyAtRate(double sampleRate)
{
    constexpr int blockSize = 64;
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    processor.prepareToPlay(sampleRate, blockSize);
    auto& engine = processor.engineForTests();
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer empty;
    startAndRelease(processor, buffer, 200);

    bool sawDcDrain = false;
    float lastNonZero = 0.0f;
    const int maxBlocks = static_cast<int>(std::ceil(sampleRate * 10.0 / blockSize));
    for (int block = 0; block < maxBlocks && ! engine.dormantForTests(); ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, empty);
        if (! finite(buffer))
            return false;
        sawDcDrain = sawDcDrain || engine.dcDrainingForTests();
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = buffer.getSample(0, sample);
            if (value != 0.0f)
                lastNonZero = value;
        }
    }

    if (! sawDcDrain || ! engine.dormantForTests())
        return false;
    if (std::abs(lastNonZero) > kSettlementThreshold)
        return false;

    const int idleBlocks = static_cast<int>(std::ceil(sampleRate * 1.0 / blockSize));
    for (int block = 0; block < idleBlocks; ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, empty);
        if (peak(buffer) != 0.0f || ! finite(buffer))
            return false;
    }
    return true;
}

double worstCutoffErrorHz = 0.0;
double worstDcResidual = 0.0;
int nonFiniteCount = 0;

void testDcBlockerMatrix()
{
    std::printf("DC_BLOCKER_MATRIX\n");
    std::printf("rate_hz,stored_pole,effective_cutoff_hz,cutoff_err_hz,cutoff_err_pct,"
                "dc_residual_2s,step,reset,extreme,dormancy,result\n");

    for (const double rate : kMatrixRates)
    {
        RateMatrixResult row;
        row.sampleRate = rate;

        swaraxt::DcBlocker blocker;
        blocker.prepare(rate);
        row.storedPole = blocker.poleForTests();
        row.effectiveCutoffHz = derivedCutoffHz(rate, row.storedPole);
        row.cutoffErrorHz = row.effectiveCutoffHz - kCutoffHz;
        row.cutoffErrorPct = 100.0 * row.cutoffErrorHz / kCutoffHz;
        worstCutoffErrorHz = std::max(worstCutoffErrorHz, std::abs(row.cutoffErrorHz));

        expect(std::abs(row.cutoffErrorHz) < 0.05,
               "derived cutoff remains 3.5 Hz across host sample rates");

        for (const float dc : { 0.25f, -0.25f })
        {
            swaraxt::DcBlocker dcBlocker;
            dcBlocker.prepare(rate);
            float output = 0.0f;
            const int dcSamples = static_cast<int>(std::ceil(rate * 5.0));
            for (int sample = 0; sample < dcSamples; ++sample)
            {
                output = dcBlocker.process(dc);
                if (! std::isfinite(output))
                    ++nonFiniteCount;
            }
            row.worstDcResidual = std::max(row.worstDcResidual, static_cast<double>(std::abs(output)));
            for (const double checkpoint : { 0.1, 0.25, 0.5, 1.0, 2.0 })
                row.worstDcResidual = std::max(
                    row.worstDcResidual,
                    dcResidualAtTime(dcBlocker, rate, dc, checkpoint));
        }
        worstDcResidual = std::max(worstDcResidual, row.worstDcResidual);
        row.dcReject = row.worstDcResidual < 1.0e-3;

        swaraxt::DcBlocker stepBlocker;
        stepBlocker.prepare(rate);
        row.step = testStepResponse(stepBlocker, rate);

        swaraxt::DcBlocker resetBlocker;
        resetBlocker.prepare(rate);
        row.reset = testResetSemantics(resetBlocker);

        swaraxt::DcBlocker extremeBlocker;
        extremeBlocker.prepare(rate);
        row.extreme = testExtremeInput(extremeBlocker, rate);

        swaraxt::DcBlocker responseBlocker;
        responseBlocker.prepare(rate);
        const double gainAtCutoff = measuredSineGainDb(responseBlocker, rate, kCutoffHz);
        expect(std::abs(gainAtCutoff + 3.0) < 1.5, "3.5 Hz is near the -3 dB point");
        expect(measuredSineGainDb(responseBlocker, rate, 20.0) > -0.5,
               "20 Hz attenuation remains small");
        expect(measuredSineGainDb(responseBlocker, rate, 40.0) > -0.1,
               "40 Hz attenuation remains negligible");

        row.dormancy = testDormancyAtRate(rate);

        const bool pass = row.dcReject && row.step && row.reset && row.extreme && row.dormancy
                          && std::abs(row.cutoffErrorHz) < 0.05;
        std::printf("%.1f,%.10f,%.6f,%.6f,%.4f,%.3e,%d,%d,%d,%d,%s\n",
                    row.sampleRate,
                    static_cast<double>(row.storedPole),
                    row.effectiveCutoffHz,
                    row.cutoffErrorHz,
                    row.cutoffErrorPct,
                    row.worstDcResidual,
                    row.step ? 1 : 0,
                    row.reset ? 1 : 0,
                    row.extreme ? 1 : 0,
                    row.dormancy ? 1 : 0,
                    pass ? "PASS" : "FAIL");
        expect(pass, "DC blocker matrix row passes");

        if (rate >= 176400.0)
        {
            const float pole = row.storedPole;
            const double doublePole = std::exp(-kTwoPi * kCutoffHz / rate);
            const double poleQuantization = static_cast<double>(pole) - doublePole;
            std::printf("Float pole at %.0f Hz: stored=%.10f double_ref=%.10f delta=%.3e cutoff=%.6f Hz\n",
                        rate,
                        static_cast<double>(pole),
                        doublePole,
                        poleQuantization,
                        row.effectiveCutoffHz);
            expect(std::abs(poleQuantization) < 5.0e-8,
                   "float pole quantization at high sample rates remains negligible");
        }
    }

    std::printf("WORST_CUTOFF_ERROR_HZ=%.6f\n", worstCutoffErrorHz);
    std::printf("WORST_DC_RESIDUAL=%.6e\n", worstDcResidual);
    std::printf("NON_FINITE_COUNT=%d\n", nonFiniteCount);
}

void testDcBlockerAtRates()
{
    testDcBlockerMatrix();
}

void startAndRelease(SwaraXtAudioProcessor& processor,
                     juce::AudioBuffer<float>& buffer,
                     int sustainBlocks)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(120)), 0);
    processor.processBlock(buffer, midi);
    midi.clear();
    for (int block = 0; block < sustainBlocks; ++block)
        processor.processBlock(buffer, midi);
    midi.addEvent(juce::MidiMessage::noteOff(1, 48), 0);
    processor.processBlock(buffer, midi);
}

void testNaturalDormancyContinuityAndLongIdle()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    processor.prepareToPlay(sampleRate, blockSize);
    auto& engine = processor.engineForTests();
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer empty;
    startAndRelease(processor, buffer, 200);

    bool sawDcDrain = false;
    float lastNonZero = 0.0f;
    const int maxBlocks = static_cast<int>(std::ceil(sampleRate * 10.0 / blockSize));
    for (int block = 0; block < maxBlocks && ! engine.dormantForTests(); ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, empty);
        expect(finite(buffer), "natural dormancy transition remains finite");
        sawDcDrain = sawDcDrain || engine.dcDrainingForTests();
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = buffer.getSample(0, sample);
            if (value != 0.0f)
                lastNonZero = value;
        }
    }

    expect(sawDcDrain, "natural dormancy passes through a DC-only drain phase");
    expect(engine.dormantForTests(), "natural dormancy settles within the bounded window");
    const float finalDiscontinuity = std::abs(lastNonZero);
    expect(finalDiscontinuity <= kSettlementThreshold,
           "final transition to digital zero is below the settlement threshold");

    engine.resetCpuProfileForTests();
    const int idleBlocks = static_cast<int>(std::ceil(sampleRate * 5.0 / blockSize));
    for (int block = 0; block < idleBlocks; ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, empty);
        expect(peak(buffer) == 0.0f, "settled long idle remains exact digital zero");
    }
    const auto profile = engine.cpuProfileForTests();
    expect(profile.nativeBlocksRendered == 0, "long idle renders no native voice blocks");
    expect(profile.filterSamplesProcessed == 0, "long idle renders no filter samples");
    expect(profile.dcDrainHostSamples == 0, "long idle does not keep processing the DC blocker");
    std::printf("Natural dormancy final discontinuity: %.9g\n",
                static_cast<double>(finalDiscontinuity));
}

void testWakeDuringDcTail()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    processor.prepareToPlay(sampleRate, blockSize);
    auto& engine = processor.engineForTests();
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    startAndRelease(processor, buffer, 200);

    const int findTailBlocks = static_cast<int>(std::ceil(sampleRate * 5.0 / blockSize));
    int block = 0;
    for (; block < findTailBlocks && ! engine.dcDrainingForTests()
           && ! engine.dormantForTests(); ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    expect(engine.dcDrainingForTests(), "wake test reaches the DC-only drain phase");
    if (! engine.dcDrainingForTests())
        return;

    engine.resetCpuProfileForTests();
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(110)), 17);
    buffer.clear();
    processor.processBlock(buffer, midi);
    midi.clear();
    expect(! engine.dormantForTests(), "note-on during DC drain remains active");
    expect(! engine.dcDrainingForTests(), "note-on cancels stale DC drain state");
    expect(finite(buffer), "wake-during-tail output is finite");

    float attackPeak = peak(buffer);
    for (int sustain = 0; sustain < 40; ++sustain)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
        attackPeak = std::max(attackPeak, peak(buffer));
    }
    expect(attackPeak > 1.0e-4f, "note-on during DC drain produces an audible attack");
    const auto wakeProfile = engine.cpuProfileForTests();
    expect(wakeProfile.nativeBlocksRendered > 0, "wake resumes native voice processing");
    expect(wakeProfile.filterSamplesProcessed > 0, "wake resumes nonlinear filter processing");

    midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
    processor.processBlock(buffer, midi);
    midi.clear();
    const int settleBlocks = static_cast<int>(std::ceil(sampleRate * 10.0 / blockSize));
    for (int settle = 0; settle < settleBlocks && ! engine.dormantForTests(); ++settle)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    expect(engine.dormantForTests(), "wake-during-tail path later returns to dormancy");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testDcBlockerAtRates();
    testNaturalDormancyContinuityAndLongIdle();
    testWakeDuringDcTail();
    std::printf(failures == 0 ? "Swara XT DC dormancy tests: PASSED\n"
                              : "Swara XT DC dormancy tests: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
