// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "Engine/SampleRate/HostResampler.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"

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

constexpr double kCutoffHz = 0.7;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr std::array<double, 12> kMatrixRates {
    8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0,
    48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0
};

double derivedCutoffHz(double sampleRate, float pole) noexcept
{
    return -sampleRate * static_cast<double>(std::log(static_cast<double>(pole))) / kTwoPi;
}

template <typename Blocker>
double measuredSineGainDb(Blocker& blocker, double sampleRate, double frequencyHz)
{
    blocker.reset();
    const int settleSamples = static_cast<int>(std::ceil(sampleRate * 8.0));
    const int measureSamples = static_cast<int>(std::lround(sampleRate * 8.0 / frequencyHz));
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
        // The board output pole may already settle the host DC state, making
        // the DC-only phase shorter than this host block. Count actual work.
        sawDcDrain = sawDcDrain || engine.cpuProfileForTests().dcDrainHostSamples > 0;
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
                "dc_residual_5s,step,reset,extreme,dormancy,result\n");

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

        expect(std::abs(row.cutoffErrorHz) < 0.002,
               "derived cutoff remains 0.7 Hz across host sample rates including float quantization");
        const float referencePole = static_cast<float>(std::exp(-kTwoPi * kCutoffHz / rate));
        expect(row.storedPole == referencePole, "stored pole exactly matches the reconstructed 0.7 Hz formula");

        for (const float dc : { 0.25f, -0.25f, 1.0f, -1.0f })
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
            {
                dcBlocker.reset();
                const double residual = dcResidualAtTime(dcBlocker, rate, dc, checkpoint);
                const double expected = std::abs(static_cast<double>(dc))
                    * std::pow(static_cast<double>(row.storedPole), std::ceil(rate * checkpoint) - 1.0);
                expect(std::abs(residual - expected) < 2.0e-5,
                       "constant DC follows the analytically predicted pole decay");
            }
        }
        worstDcResidual = std::max(worstDcResidual, row.worstDcResidual);
        row.dcReject = row.worstDcResidual < kSettlementThreshold;

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
        expect(std::abs(gainAtCutoff + 3.0103) < 0.03, "0.7 Hz is near the -3 dB point");
        expect(measuredSineGainDb(responseBlocker, rate, 20.0) > -0.01,
               "20 Hz attenuation remains small");
        expect(measuredSineGainDb(responseBlocker, rate, 40.0) > -0.003,
               "40 Hz attenuation remains negligible");

        row.dormancy = testDormancyAtRate(rate);

        const bool pass = row.dcReject && row.step && row.reset && row.extreme && row.dormancy
                          && std::abs(row.cutoffErrorHz) < 0.002;
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

double theoreticalGainDb(double pole, double rate, double frequency)
{
    const double sine = std::sin(0.5 * kTwoPi * frequency / rate);
    const double numerator = 4.0 * sine * sine;
    return 10.0 * std::log10(numerator / ((1.0 - pole) * (1.0 - pole) + pole * numerator));
}

void testFrequencyResponseAndSafety()
{
    constexpr double rate = 48000;
    struct OldBlocker {
        float pole = static_cast<float>(std::exp(-kTwoPi * 3.5 / rate));
        float x = 0, y = 0;
        void reset() { x = y = 0; }
        float process(float input) { const float output = input - x + pole * y; x = input; y = output; return output; }
    } old;
    swaraxt::DcBlocker restored;
    restored.prepare(rate);
    std::printf("RESPONSE_HZ,old_measured_db,new_measured_db,old_theoretical_db,new_theoretical_db\n");
    for (const double frequency : {0.1, 0.5, kCutoffHz, 1.0, 3.5, 5.0, 10.0, 20.0, 30.0, 50.0})
    {
        const double before = measuredSineGainDb(old, rate, frequency);
        const double after = measuredSineGainDb(restored, rate, frequency);
        const double oldTheory = theoreticalGainDb(old.pole, rate, frequency);
        const double newTheory = theoreticalGainDb(restored.poleForTests(), rate, frequency);
        expect(std::abs(before - oldTheory) < 0.025 && std::abs(after - newTheory) < 0.025,
               "measured before/after response agrees with the discrete transfer function");
        std::printf("%.6f,%.6f,%.6f,%.6f,%.6f\n", frequency, before, after, oldTheory, newTheory);
    }
    juce::ScopedNoDenormals noDenormals;
    for (const float input : {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                              1.0f / 2048, -1.0f / 2048})
    {
        restored.reset();
        float output = restored.process(input);
        for (int i = 0; i < static_cast<int>(rate * 25); ++i)
        {
            output = restored.process(input);
            if (!std::isfinite(output)) { expect(false, "finite maximum/quantized-bias step"); break; }
        }
        expect(std::abs(output) <= kSettlementThreshold, "25 seconds covers maximum finite and quantized-bias DC");
        restored.reset();
        expect(restored.process(0) == 0, "extreme-state reset is exact zero");
    }
    restored.reset();
    const auto begin = std::chrono::steady_clock::now();
    float tiny = 0;
    for (int i = 0; i < 1000000; ++i)
        tiny = restored.process((i & 1) != 0 ? 1.e-40f : -1.e-40f);
    expect(std::isfinite(tiny) && tiny == 0, "production denormal guard flushes denormal-scale values");
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::printf("Denormal-guarded million samples: %.6f seconds\n", elapsed);
    for (const double hostRate : kMatrixRates)
    {
        swaraxt::DcBlocker dc; dc.prepare(hostRate);
        const double decay = -hostRate * std::log(static_cast<double>(dc.poleForTests()));
        const double unitySeconds = std::log(1.e7) / decay;
        const double maximumSeconds = std::log(static_cast<double>(std::numeric_limits<float>::max()) / 1.e-7) / decay;
        expect(maximumSeconds < 25, "25-second backstop covers float pole quantization at every rate");
        std::printf("SETTLEMENT,%.0f,unity_seconds=%.6f,maxfloat_seconds=%.6f\n", hostRate, unitySeconds, maximumSeconds);
    }
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
        sawDcDrain = sawDcDrain || engine.cpuProfileForTests().dcDrainHostSamples > 0;
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

void testWakeDuringOutputTail()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 1;
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    // Hardware can finish the host DC drain in a single sample. Wake during
    // its preceding FIR tail, independently of host buffer partitioning.
    processor.prepareToPlay(sampleRate, blockSize);
    auto& engine = processor.engineForTests();
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    startAndRelease(processor, buffer, 12800);

    const int findTailBlocks = static_cast<int>(std::ceil(sampleRate * 5.0 / blockSize));
    int block = 0;
    for (; block < findTailBlocks && ! engine.drainingForTests()
           && ! engine.dormantForTests(); ++block)
    {
        buffer.clear();
        processor.processBlock(buffer, midi);
    }
    expect(engine.drainingForTests(), "wake test reaches the output drain phase");
    if (! engine.drainingForTests())
        return;

    engine.resetCpuProfileForTests();
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(110)), 0);
    buffer.clear();
    processor.processBlock(buffer, midi);
    midi.clear();
    expect(! engine.dormantForTests(), "note-on during DC drain remains active");
    expect(! engine.dcDrainingForTests(), "note-on cancels stale DC drain state");
    expect(finite(buffer), "wake-during-tail output is finite");

    float attackPeak = peak(buffer);
    for (int sustain = 0; sustain < 2560; ++sustain)
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

// Optional deterministic artifacts compare final-output changes against an
// earlier binary while retaining an exact pre-SRC analog/source boundary.
void lowEndRenders(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);
    std::ofstream report(directory / "metrics.csv");
    report << "target_hz,note,fine_amount,resonance,fx,rms,peak,dc\n";
    for (const double frequency : {20.0, 25.0, 30.0, 40.0, 50.0})
        for (const float resonance : {0.0f, 0.85f})
            for (const int fx : {0, 6})
            {
                SwaraXtAudioProcessor processor;
                configureFastEnvelope(processor);
                setParameter(processor, swaraxt::IDs::seqMode, 0);
                setParameter(processor, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_FM);
                setParameter(processor, swaraxt::IDs::osc1Param, 0);
                setParameter(processor, swaraxt::IDs::osc1Range, 0);
                setParameter(processor, swaraxt::IDs::mixBalance, 0);
                setParameter(processor, swaraxt::IDs::mixSub, 0);
                setParameter(processor, swaraxt::IDs::mixNoise, 0);
                setParameter(processor, swaraxt::IDs::filterCutoff, 20000);
                setParameter(processor, swaraxt::IDs::filterResonance, resonance);
                setParameter(processor, swaraxt::IDs::filterKeyTracking, 0);
                setParameter(processor, swaraxt::IDs::filterEnvDepth, 0);
                setParameter(processor, swaraxt::IDs::filterLfoDepth, 0);
                setParameter(processor, swaraxt::IDs::dspFxProgram, static_cast<float>(fx));
                setParameter(processor, swaraxt::IDs::dspFxParam1, 80);
                setParameter(processor, swaraxt::IDs::dspFxParam2, 35);
                for (int row = 0; row < 12; ++row)
                {
                    const auto id = "mod.row" + juce::String(row + 1) + ".amount";
                    setParameter(processor, id.toRawUTF8(), 0);
                }
                setParameter(processor, "mod.row9.source", shruthi::MOD_SRC_ENV_2);
                setParameter(processor, "mod.row9.destination", shruthi::MOD_DST_VCA);
                setParameter(processor, "mod.row9.amount", 63);
                const double pitch = 69.0 + 12.0 * std::log2(frequency / 440.0);
                const int note = static_cast<int>(std::floor(pitch));
                const int fine = static_cast<int>(std::lround((pitch - note) * 64.0));
                setParameter(processor, "mod.row1.source", shruthi::MOD_SRC_OFFSET);
                setParameter(processor, "mod.row1.destination", shruthi::MOD_DST_VCO_1_2_FINE);
                setParameter(processor, "mod.row1.amount", static_cast<float>(fine));
                processor.prepareToPlay(48000, 128);
                std::vector<float> upstream;
                processor.engineForTests().setDebugTapSink(&upstream,
                    [](void* context, const swaraxt::SwaraXtEngine::DebugBlockCapture& block) {
                        auto& values = *static_cast<std::vector<float>*>(context);
                        values.insert(values.end(), block.filterOutput, block.filterOutput + block.samples);
                    });
                std::vector<float> host;
                host.reserve(48000 * 6);
                juce::AudioBuffer<float> buffer(2, 128);
                for (int block = 0; block < 2250; ++block)
                {
                    juce::MidiBuffer midi;
                    if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, juce::uint8(110)), 0);
                    processor.processBlock(buffer, midi);
                    expect(finite(buffer), "low-end complete production render is finite");
                    host.insert(host.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 128);
                }
                double square = 0.0, sum = 0.0;
                float maximum = 0.0f;
                for (size_t i = 48000 * 4; i < host.size(); ++i)
                {
                    const double value = host[i];
                    square += value * value; sum += value;
                    maximum = std::max(maximum, std::abs(host[i]));
                }
                const double count = static_cast<double>(host.size() - 48000 * 4);
                expect(maximum > 1.0e-5f, "low-end complete production render is audible");
                report << frequency << ',' << note << ',' << fine << ',' << resonance << ',' << fx
                       << ',' << std::sqrt(square / count) << ',' << maximum << ',' << sum / count << '\n';
                const auto name = std::to_string(static_cast<int>(frequency)) + "-"
                    + std::to_string(resonance) + "-" + std::to_string(fx);
                for (const auto& item : {std::make_pair("host", &host), std::make_pair("upstream", &upstream)})
                {
                    std::ofstream file(directory / (name + "-" + item.first + ".f32"), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(item.second->data()),
                               static_cast<std::streamsize>(item.second->size() * sizeof(float)));
                    expect(file.good(), "low-end artifact was saved");
                }
            }
}

}  // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc == 3 && juce::String(argv[1]) == "--low-end")
    {
        lowEndRenders(argv[2]);
        return failures == 0 ? 0 : 1;
    }
    testDcBlockerAtRates();
    testFrequencyResponseAndSafety();
    testNaturalDormancyContinuityAndLongIdle();
    testWakeDuringOutputTail();
    std::printf(failures == 0 ? "Swara XT DC dormancy tests: PASSED\n"
                              : "Swara XT DC dormancy tests: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
