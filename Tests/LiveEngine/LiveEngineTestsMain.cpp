// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Live-engine regression harness: real plugin processor, full signal chain.

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "Plugin/PluginProcessor.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* msg)
{
    if (! ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFailures;
    }
}

float bufferPeak(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            peak = std::max(peak, std::fabs(buffer.getSample(ch, i)));
    return peak;
}

bool bufferFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite(buffer.getSample(ch, i)))
                return false;
    return true;
}

float processNoteEnergy(SwaraXtAudioProcessor& proc, int blockSize, int blocks = -1)
{
    if (blocks < 0)
    {
        // Tiny host blocks need more iterations for envelope/SRC to produce energy.
        const int minSamples = 256;
        blocks = juce::jmax(8, (minSamples + blockSize - 1) / blockSize);
    }

    juce::AudioBuffer<float> buffer(2, blockSize);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        if (b == blocks - 2)
            midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        if (b == juce::jmax(1, blocks / 4))
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 90), blockSize > 1 ? blockSize / 2 : 0);
        proc.processBlock(buffer, midi);
        expect(bufferFinite(buffer), "finite samples");
        peak = std::max(peak, bufferPeak(buffer));
    }
    return peak;
}

void testBlockSizes(SwaraXtAudioProcessor& proc)
{
    const int sizes[] = {
        1, 7, 16, 31, 32, 63, 64, 65, 96, 127, 128, 129,
        255, 256, 257, 511, 512, 513, 1024, 2048, 4096
    };

    for (int bs : sizes)
    {
        std::printf("block %d...\n", bs);
        std::fflush(stdout);
        proc.prepareToPlay(44100.0, bs);
        const float peak = processNoteEnergy(proc, bs);
        char msg[64];
        std::snprintf(msg, sizeof(msg), "non-silent block %d (peak=%g)", bs, peak);
        expect(peak > 1.0e-4f, msg);
        std::printf("  peak=%g\n", peak);
        std::fflush(stdout);
        proc.releaseResources();
    }
}

void testSampleRateTransitions(SwaraXtAudioProcessor& proc)
{
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    const int sequences[][4] = {
        { 0, 1, 3, 0 },
        { 1, 5, 2, 1 },
        { 3, 0, 4, 3 }
    };

    for (const auto& seq : sequences)
    {
        for (int i = 0; i < 4; ++i)
        {
            const double sr = rates[seq[i]];
            const int bs = 128 + i * 64;
            std::printf("rate transition %.1f Hz block %d...\n", sr, bs);
            std::fflush(stdout);
            proc.prepareToPlay(sr, bs);
            // Repeat prepare at same rate.
            proc.prepareToPlay(sr, bs);
            const float peak = processNoteEnergy(proc, bs, 6);
            expect(peak > 1.0e-4f, "non-silent after rate change");
            expect(std::isfinite(peak), "finite after rate change");
            proc.releaseResources();
        }
    }
}

void testMidiBoundaries(SwaraXtAudioProcessor& proc)
{
    const int sizes[] = { 63, 64, 65, 127, 128, 129 };
    const int offsets[] = { 0, 1, 62, 63, 64, 65 };

    for (int bs : sizes)
    {
        proc.prepareToPlay(44100.0, bs);
        for (int off : offsets)
        {
            if (off >= bs)
                continue;
            juce::AudioBuffer<float> buffer(2, bs);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 110), off);
            buffer.clear();
            proc.processBlock(buffer, midi);
            expect(bufferFinite(buffer), "midi boundary finite");

            // Process sustain blocks then note-off at last sample.
            for (int b = 0; b < 4; ++b)
            {
                juce::MidiBuffer empty;
                if (b == 3)
                    empty.addEvent(juce::MidiMessage::noteOff(1, 64), bs - 1);
                buffer.clear();
                proc.processBlock(buffer, empty);
                expect(bufferFinite(buffer), "midi sustain finite");
            }
        }
        proc.releaseResources();
    }
}

float renderTimeline(SwaraXtAudioProcessor& proc,
                     int totalSamples,
                     const std::vector<int>& blockSchedule)
{
    proc.prepareToPlay(44100.0, 512);
    // Deterministic reset of engine path.
    proc.prepareToPlay(44100.0, 512);

    juce::MidiBuffer fullMidi;
    fullMidi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    fullMidi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 80), 40);
    fullMidi.addEvent(juce::MidiMessage::noteOff(1, 60), totalSamples / 2);

    std::vector<float> out(static_cast<size_t>(totalSamples), 0.0f);
    int offset = 0;
    size_t scheduleIndex = 0;
    while (offset < totalSamples)
    {
        int bs = blockSchedule[scheduleIndex % blockSchedule.size()];
        if (offset + bs > totalSamples)
            bs = totalSamples - offset;

        juce::AudioBuffer<float> buffer(2, bs);
        juce::MidiBuffer slice;
        for (const auto metadata : fullMidi)
        {
            const int pos = metadata.samplePosition;
            if (pos >= offset && pos < offset + bs)
                slice.addEvent(metadata.getMessage(), pos - offset);
        }
        buffer.clear();
        proc.processBlock(buffer, slice);
        for (int i = 0; i < bs; ++i)
            out[static_cast<size_t>(offset + i)] = buffer.getSample(0, i);
        offset += bs;
        ++scheduleIndex;
    }
    proc.releaseResources();

    float peak = 0.0f;
    for (float s : out)
        peak = std::max(peak, std::fabs(s));
    return peak;
}

void testRenderEquivalence(SwaraXtAudioProcessor& proc)
{
    // Compare energy of identical timelines under different host chunkings.
    // Exact sample match is not required across independent prepare cycles with
    // global Shruthi RNG/state; require all paths produce comparable energy.
    const float one = renderTimeline(proc, 128, { 128 });
    const float two = renderTimeline(proc, 128, { 64, 64 });
    const float three = renderTimeline(proc, 128, { 64, 63, 1 });

    expect(one > 1.0e-4f, "128-block energy");
    expect(two > 1.0e-4f, "64+64 energy");
    expect(three > 1.0e-4f, "64+63+1 energy");

    const float maxE = std::max(one, std::max(two, three));
    const float minE = std::min(one, std::min(two, three));
    expect(maxE > 0.0f && (maxE - minE) / maxE < 0.75f, "chunking energy within tolerance");
    std::printf("equivalence peaks: 128=%g 64+64=%g 64+63+1=%g\n", one, two, three);
}

void testFactoryPresetDeterminism()
{
    for (int target = 0; target < 11; ++target)
    {
        SwaraXtAudioProcessor direct;
        direct.setCurrentProgram(target);
        juce::MemoryBlock directState;
        direct.getStateInformation(directState);

        SwaraXtAudioProcessor traversed;
        traversed.setCurrentProgram((target + 5) % 11);
        traversed.setCurrentProgram(target);
        juce::MemoryBlock traversedState;
        traversed.getStateInformation(traversedState);

        expect(directState == traversedState, "factory preset deterministic from any prior preset");
    }
}

void testPanicReachesIdle()
{
    SwaraXtAudioProcessor proc;
    constexpr int blockSize = 128;
    proc.prepareToPlay(48000.0, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize);

    juce::MidiBuffer noteOn;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    proc.processBlock(buffer, noteOn);

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    proc.processBlock(buffer, panic);
    for (int block = 0; block < 512; ++block)
    {
        juce::MidiBuffer empty;
        buffer.clear();
        proc.processBlock(buffer, empty);
    }

    expect(proc.engineForTests().shruthiPart().voice().vca() == 0,
           "all-sound-off reaches idle voice");
    proc.releaseResources();
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("SwaraXtLiveEngineTests start\n");
    std::fflush(stdout);

    SwaraXtAudioProcessor proc;

    testBlockSizes(proc);
    testSampleRateTransitions(proc);
    testMidiBoundaries(proc);
    testRenderEquivalence(proc);
    testFactoryPresetDeterminism();
    testPanicReachesIdle();

    std::printf(gFailures == 0 ? "SwaraXtLiveEngineTests: PASSED\n"
                               : "SwaraXtLiveEngineTests: FAILED (%d)\n",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
