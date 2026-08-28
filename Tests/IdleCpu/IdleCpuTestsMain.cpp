// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"

namespace {

constexpr int kBenchmarkBlockSize = 256;
constexpr int kBenchmarkBlocks = 2000;
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

bool isExactZeroAndFinite(const juce::AudioBuffer<float>& buffer, int begin = 0)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = begin; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = buffer.getSample(channel, sample);
            if (! std::isfinite(value) || value != 0.0f)
                return false;
        }
    return true;
}

bool isFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            if (! std::isfinite(buffer.getSample(channel, sample)))
                return false;
    return true;
}

double peak(const juce::AudioBuffer<float>& buffer, int begin = 0)
{
    double result = 0.0;
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = begin; sample < buffer.getNumSamples(); ++sample)
            result = juce::jmax(result,
                                std::abs(static_cast<double>(buffer.getSample(channel, sample))));
    return result;
}

void configureFastEnvelope(SwaraXtAudioProcessor& processor)
{
    setParameter(processor, swaraxt::IDs::env2Attack, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Decay, 0.0f);
    setParameter(processor, swaraxt::IDs::env2Sustain, 127.0f);
    setParameter(processor, swaraxt::IDs::env2Release, 0.0f);
}

void runBenchmarkScenario(const char* name, bool noteOn, bool noteOff, int silentLeadBlocks)
{
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    processor.prepareToPlay(48000.0, kBenchmarkBlockSize);
    juce::AudioBuffer<float> buffer(2, kBenchmarkBlockSize);
    juce::MidiBuffer midi;

    if (noteOn)
    {
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
    }

    if (noteOff)
    {
        for (int i = 0; i < 100; ++i)
            processor.processBlock(buffer, midi);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
        midi.clear();
    }

    for (int i = 0; i < silentLeadBlocks; ++i)
        processor.processBlock(buffer, midi);

    auto& engine = processor.engineForTests();
    engine.resetCpuProfileForTests();
    const auto start = std::chrono::steady_clock::now();
    double measuredPeak = 0.0;
    for (int block = 0; block < kBenchmarkBlocks; ++block)
    {
        processor.processBlock(buffer, midi);
        measuredPeak = juce::jmax(measuredPeak, peak(buffer));
    }
    const auto wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    const auto& profile = engine.cpuProfileForTests();
    const auto percentage = [total = static_cast<double>(profile.processNanoseconds)](uint64_t value) {
        return total > 0.0 ? 100.0 * static_cast<double>(value) / total : 0.0;
    };
    std::printf(
        "%s wall_ms=%.3f process_ms=%.3f midi_clock_ms=%.3f voice_ms=%.3f "
        "filter_ms=%.3f src_output_ms=%.3f voice_pct=%.2f filter_pct=%.2f "
        "src_output_pct=%.2f native_blocks=%llu filter_samples=%llu "
        "control_blocks=%llu skipped_samples=%llu peak=%.9f\n",
        name,
        static_cast<double>(wallNs) / 1.0e6,
        static_cast<double>(profile.processNanoseconds) / 1.0e6,
        static_cast<double>(profile.midiAndClockNanoseconds) / 1.0e6,
        static_cast<double>(profile.nativeVoiceNanoseconds) / 1.0e6,
        static_cast<double>(profile.filterNanoseconds) / 1.0e6,
        static_cast<double>(profile.srcAndOutputNanoseconds) / 1.0e6,
        percentage(profile.nativeVoiceNanoseconds), percentage(profile.filterNanoseconds),
        percentage(profile.srcAndOutputNanoseconds),
        static_cast<unsigned long long>(profile.nativeBlocksRendered),
        static_cast<unsigned long long>(profile.filterSamplesProcessed),
        static_cast<unsigned long long>(profile.dormantControlBlocksAdvanced),
        static_cast<unsigned long long>(profile.dormantHostSamplesSkipped), measuredPeak);

    if (! noteOn || noteOff)
    {
        expect(profile.nativeBlocksRendered == 0, "settled idle renders no native audio blocks");
        expect(profile.filterSamplesProcessed == 0, "settled idle runs no filter samples");
        expect(profile.dormantHostSamplesSkipped
                   == static_cast<uint64_t>(kBenchmarkBlocks * kBenchmarkBlockSize),
               "settled idle skips every host sample");
        expect(measuredPeak == 0.0, "settled idle output is exact zero");
    }
    else
    {
        expect(profile.nativeBlocksRendered > 0, "held note renders native audio blocks");
        expect(profile.filterSamplesProcessed > 0, "held note runs nonlinear filter");
        expect(measuredPeak > 0.0, "held note produces audio");
    }
}

void testActiveRenderEquivalence()
{
    SwaraXtAudioProcessor optimized;
    SwaraXtAudioProcessor continuous;
    configureFastEnvelope(optimized);
    configureFastEnvelope(continuous);
    optimized.prepareToPlay(48000.0, 129);
    continuous.prepareToPlay(48000.0, 129);
    continuous.engineForTests().setDormancyEnabledForTests(false);

    juce::AudioBuffer<float> optimizedBuffer(2, 129);
    juce::AudioBuffer<float> continuousBuffer(2, 129);
    juce::MidiBuffer optimizedMidi;
    juce::MidiBuffer continuousMidi;
    optimizedMidi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    continuousMidi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);

    for (int block = 0; block < 64; ++block)
    {
        optimized.processBlock(optimizedBuffer, optimizedMidi);
        continuous.processBlock(continuousBuffer, continuousMidi);
        optimizedMidi.clear();
        continuousMidi.clear();
        expect(std::memcmp(optimizedBuffer.getReadPointer(0), continuousBuffer.getReadPointer(0),
                           static_cast<size_t>(129) * sizeof(float)) == 0,
               "continuously active optimized render is sample-identical");
    }
}

void testLifecycle(double sampleRate, int blockSize)
{
    SwaraXtAudioProcessor processor;
    configureFastEnvelope(processor);
    processor.prepareToPlay(sampleRate, blockSize);
    auto& engine = processor.engineForTests();
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;

    engine.resetCpuProfileForTests();
    for (int block = 0; block < 4; ++block)
    {
        processor.processBlock(buffer, midi);
        expect(isExactZeroAndFinite(buffer), "fresh idle is finite exact silence");
    }
    expect(engine.dormantForTests(), "fresh instance remains dormant");
    expect(engine.cpuProfileForTests().nativeBlocksRendered == 0,
           "fresh idle skips native audio renderer");

    // Automation remains live without waking the expensive audio path.
    setParameter(processor, swaraxt::IDs::filterCutoff, 8765.0f);
    processor.processBlock(buffer, midi);
    expect(engine.dormantForTests(), "parameter automation does not wake a silent voice");
    expect(isExactZeroAndFinite(buffer), "automation during idle remains silent");

    const int noteOffset = juce::jlimit(0, blockSize - 1, blockSize / 2);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), noteOffset);
    engine.resetCpuProfileForTests();
    processor.processBlock(buffer, midi);
    midi.clear();
    expect(isFinite(buffer), "note wake output remains finite");
    for (int sample = 0; sample < noteOffset; ++sample)
        expect(buffer.getSample(0, sample) == 0.0f,
               "note inside idle block produces no audio before its sample offset");
    expect(! engine.dormantForTests(), "note-on wakes heavy audio processing");
    expect(engine.cpuProfileForTests().nativeBlocksRendered > 0,
           "note-on resumes native audio renderer");

    double activePeak = peak(buffer, noteOffset);
    for (int block = 0; block < 12 && activePeak == 0.0; ++block)
    {
        processor.processBlock(buffer, midi);
        activePeak = juce::jmax(activePeak, peak(buffer));
    }
    expect(activePeak > 0.0, "first note after idle produces audio");

    const int offOffset = juce::jlimit(0, blockSize - 1, blockSize / 3);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), offOffset);
    processor.processBlock(buffer, midi);
    midi.clear();

    // The preserved 0.9999 reference pole needs about 3.65 seconds to decay
    // from unity to the -140 dBFS dormancy threshold. Allow deterministic
    // scheduling headroom without weakening the eventual-idle requirement.
    const int maxReleaseBlocks = static_cast<int>(std::ceil(sampleRate * 6.0 / blockSize));
    bool reachedDormant = engine.dormantForTests();
    for (int block = 0; block < maxReleaseBlocks && ! reachedDormant; ++block)
    {
        processor.processBlock(buffer, midi);
        reachedDormant = engine.dormantForTests();
    }
    expect(reachedDormant, "completed release returns to dormant state");

    engine.resetCpuProfileForTests();
    const int tenSecondBlocks = static_cast<int>(std::ceil(sampleRate * 10.0 / blockSize));
    for (int block = 0; block < tenSecondBlocks; ++block)
        processor.processBlock(buffer, midi);
    expect(engine.cpuProfileForTests().nativeBlocksRendered == 0,
           "ten-second post-release idle renders no heavy audio");
    expect(engine.cpuProfileForTests().filterSamplesProcessed == 0,
           "ten-second post-release idle runs no filter samples");

    midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), noteOffset);
    processor.processBlock(buffer, midi);
    midi.clear();
    expect(! engine.dormantForTests(), "note after long dormant interval wakes immediately");
    expect(std::isfinite(peak(buffer)), "note after long dormant interval is finite");
}

void testRateAndBlockMatrix()
{
    constexpr std::array<double, 6> sampleRates { 44100.0, 48000.0, 88200.0,
                                                  96000.0, 176400.0, 192000.0 };
    constexpr std::array<int, 7> blockSizes { 63, 64, 65, 127, 128, 129, 257 };
    for (const auto sampleRate : sampleRates)
        for (const auto blockSize : blockSizes)
            testLifecycle(sampleRate, blockSize);
}

void testLongIdleAndAudioSourcePolicy()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 257;
    juce::MidiBuffer midi;

    SwaraXtAudioProcessor longIdle;
    configureFastEnvelope(longIdle);
    longIdle.prepareToPlay(sampleRate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);
    const int sixtySecondBlocks = static_cast<int>(std::ceil(sampleRate * 60.0 / blockSize));
    for (int block = 0; block < sixtySecondBlocks; ++block)
        longIdle.processBlock(buffer, midi);
    expect(longIdle.engineForTests().dormantForTests(),
           "fresh instance remains dormant for sixty seconds");
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)), 73);
    longIdle.processBlock(buffer, midi);
    midi.clear();
    expect(! longIdle.engineForTests().dormantForTests(),
           "note at sample 73 wakes after sixty-second idle");
    expect(isFinite(buffer), "sixty-second idle wake is finite");

    // MOD_SRC_AUDIO is defined by the continuously rendered oscillator buffer.
    // A patch that explicitly selects it therefore opts out of dormancy.
    SwaraXtAudioProcessor audioSourcePatch;
    configureFastEnvelope(audioSourcePatch);
    setParameter(audioSourcePatch, "mod.row1.source", static_cast<float>(shruthi::MOD_SRC_AUDIO));
    setParameter(audioSourcePatch, "mod.row1.destination", static_cast<float>(shruthi::MOD_DST_PWM_1));
    setParameter(audioSourcePatch, "mod.row1.amount", 32.0f);
    audioSourcePatch.prepareToPlay(sampleRate, blockSize);
    audioSourcePatch.engineForTests().resetCpuProfileForTests();
    audioSourcePatch.processBlock(buffer, midi);
    expect(! audioSourcePatch.engineForTests().dormantForTests(),
           "audio-rate modulation source prevents semantic phase freezing");
    expect(audioSourcePatch.engineForTests().cpuProfileForTests().nativeBlocksRendered > 0,
           "audio-rate modulation route retains full native rendering");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    runBenchmarkScenario("fresh_idle", false, false, 0);
    runBenchmarkScenario("held_note", true, false, 0);
    runBenchmarkScenario("post_release", true, true, 4000);
    testActiveRenderEquivalence();
    testRateAndBlockMatrix();
    testLongIdleAndAudioSourcePolicy();
    std::printf(failures == 0 ? "Swara XT idle CPU tests: PASSED\n"
                              : "Swara XT idle CPU tests: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
