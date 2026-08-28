// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
// Processor-level multi-instance regression tests.

#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "Plugin/PluginProcessor.h"
#include "Ui/SwaraXtUiPalette.h"

namespace {

std::atomic<int> gFailures { 0 };

void expect(bool ok, const char* msg)
{
    if (! ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        gFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

void setInt(SwaraXtAudioProcessor& proc, const char* id, int value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterInt*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

void setFloat(SwaraXtAudioProcessor& proc, const char* id, float value)
{
    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(proc.getApvts().getParameter(id)))
        *param = value;
    else
        expect(false, id);
}

float peakOf(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            peak = std::max(peak, std::fabs(buffer.getSample(ch, i)));
    return peak;
}

bool finite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite(buffer.getSample(ch, i)))
                return false;
    return true;
}

float renderNote(SwaraXtAudioProcessor& proc,
                 int note,
                 int blockSize,
                 int blocks,
                 bool release = true)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
        if (release && b == blocks - 2)
            midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
        buffer.clear();
        proc.processBlock(buffer, midi);
        expect(finite(buffer), "finite renderNote buffer");
        peak = std::max(peak, peakOf(buffer));
    }
    return peak;
}

void processEmpty(SwaraXtAudioProcessor& proc, int blockSize, int blocks)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer midi;
        buffer.clear();
        proc.processBlock(buffer, midi);
        expect(finite(buffer), "finite empty buffer");
    }
}

void testIndependentPatchState()
{
    SwaraXtAudioProcessor a;
    SwaraXtAudioProcessor b;
    a.prepareToPlay(44100.0, 128);
    b.prepareToPlay(44100.0, 128);

    setInt(a, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_SAW);
    setInt(b, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_TRIANGLE);
    processEmpty(a, 128, 1);
    processEmpty(b, 128, 1);

    expect(a.engineForTests().shruthiPart().patch().osc[0].shape == shruthi::WAVEFORM_SAW,
           "A oscillator state");
    expect(b.engineForTests().shruthiPart().patch().osc[0].shape == shruthi::WAVEFORM_TRIANGLE,
           "B oscillator state");

    setInt(a, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_FM);
    processEmpty(a, 128, 1);
    expect(b.engineForTests().shruthiPart().patch().osc[0].shape == shruthi::WAVEFORM_TRIANGLE,
           "B patch unchanged after A edit");
}

void testIndependentNoteAndFilterState()
{
    SwaraXtAudioProcessor a;
    SwaraXtAudioProcessor b;
    a.prepareToPlay(44100.0, 64);
    b.prepareToPlay(44100.0, 256);

    setFloat(a, swaraxt::IDs::filterCutoff, 600.0f);
    setFloat(a, swaraxt::IDs::filterResonance, 0.25f);
    setFloat(b, swaraxt::IDs::filterCutoff, 6000.0f);
    setFloat(b, swaraxt::IDs::filterResonance, 0.65f);
    setInt(a, swaraxt::IDs::env1Decay, 24);
    setInt(b, swaraxt::IDs::env1Decay, 96);

    const float peakA = renderNote(a, 48, 64, 12);
    const float peakBStart = renderNote(b, 72, 256, 4, false);
    a.releaseResources();
    const float peakBAfterARelease = renderNote(b, 72, 256, 4, false);

    expect(peakA > 1.0e-4f, "A independent note energy");
    expect(peakBStart > 1.0e-4f, "B independent note energy");
    expect(peakBAfterARelease > 1.0e-4f, "B continues after A release");
}

void testIndependentRandomAndSequencerState()
{
    SwaraXtAudioProcessor a;
    SwaraXtAudioProcessor b;
    a.prepareToPlay(44100.0, 128);
    b.prepareToPlay(44100.0, 128);

    expect(a.engineForTests().randomStateForTests() == b.engineForTests().randomStateForTests(),
           "same seeded RNG state");
    (void) a.engineForTests().advanceRandomForTests();
    expect(a.engineForTests().randomStateForTests() != b.engineForTests().randomStateForTests(),
           "advancing A RNG does not advance B");
    (void) b.engineForTests().advanceRandomForTests();
    expect(a.engineForTests().randomStateForTests() == b.engineForTests().randomStateForTests(),
           "same seed reproduces RNG state");

    auto* seqA = a.engineForTests().shruthiPart().mutable_sequencer_settings();
    auto* seqB = b.engineForTests().shruthiPart().mutable_sequencer_settings();
    seqA->seq_mode = shruthi::SEQUENCER_MODE_ARP;
    seqA->arp_direction = shruthi::ARPEGGIO_DIRECTION_UP;
    seqA->arp_range = 1;
    seqA->arp_clock_division = 11;
    seqB->seq_mode = shruthi::SEQUENCER_MODE_ARP;
    seqB->arp_direction = shruthi::ARPEGGIO_DIRECTION_DOWN;
    seqB->arp_range = 3;
    seqB->arp_clock_division = 0;

    const uint8_t bStepBefore = b.engineForTests().shruthiPart().step();
    (void) renderNote(a, 60, 128, 64, false);
    expect(a.engineForTests().shruthiPart().running(), "A sequencer running");
    expect(b.engineForTests().shruthiPart().step() == bStepBefore, "B sequencer not advanced by A");
    expect(seqB->arp_direction == shruthi::ARPEGGIO_DIRECTION_DOWN, "B sequencer settings unchanged");
}

void testIndependentStateRestore()
{
    SwaraXtAudioProcessor a;
    SwaraXtAudioProcessor b;
    setInt(a, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_FM);
    setInt(a, swaraxt::IDs::arpDirection, shruthi::ARPEGGIO_DIRECTION_RANDOM);
    setInt(b, swaraxt::IDs::osc1Shape, shruthi::WAVEFORM_VOWEL);
    setInt(b, swaraxt::IDs::arpDirection, shruthi::ARPEGGIO_DIRECTION_DOWN);

    juce::MemoryBlock stateA;
    juce::MemoryBlock stateB;
    a.getStateInformation(stateA);
    b.getStateInformation(stateB);

    a.setStateInformation(stateB.getData(), static_cast<int>(stateB.getSize()));
    b.setStateInformation(stateA.getData(), static_cast<int>(stateA.getSize()));
    a.prepareToPlay(44100.0, 128);
    b.prepareToPlay(44100.0, 128);
    processEmpty(a, 128, 1);
    processEmpty(b, 128, 1);

    expect(a.engineForTests().shruthiPart().patch().osc[0].shape == shruthi::WAVEFORM_VOWEL,
           "A restored B patch");
    expect(b.engineForTests().shruthiPart().patch().osc[0].shape == shruthi::WAVEFORM_FM,
           "B restored A patch");
}

void testConcurrentProcessingAndParameterThread()
{
    SwaraXtAudioProcessor a;
    SwaraXtAudioProcessor b;
    a.prepareToPlay(48000.0, 96);
    b.prepareToPlay(44100.0, 257);

    std::atomic<bool> done { false };
    std::atomic<float> peakA { 0.0f };
    std::atomic<float> peakB { 0.0f };

    auto renderLoop = [&](SwaraXtAudioProcessor& proc, int note, int blockSize, std::atomic<float>& peak) {
        juce::AudioBuffer<float> buffer(2, blockSize);
        for (int block = 0; block < 96; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
            if (block == 90)
                midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            buffer.clear();
            proc.processBlock(buffer, midi);
            expect(finite(buffer), "finite concurrent processor buffer");
            peak.store(std::max(peak.load(std::memory_order_relaxed), peakOf(buffer)),
                       std::memory_order_relaxed);
        }
    };

    std::thread paramThread([&] {
        for (int i = 0; i < 128; ++i)
        {
            setFloat(a, swaraxt::IDs::filterCutoff, 100.0f + static_cast<float>((i * 137) % 18000));
            setFloat(b, swaraxt::IDs::filterResonance, static_cast<float>((i % 90) + 5) / 100.0f);
            setInt(a, swaraxt::IDs::osc1Param, i & 0x7f);
            setInt(b, swaraxt::IDs::osc2Param, (127 - i) & 0x7f);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread renderA(renderLoop, std::ref(a), 55, 96, std::ref(peakA));
    std::thread renderB(renderLoop, std::ref(b), 67, 257, std::ref(peakB));
    renderA.join();
    renderB.join();
    paramThread.join();

    expect(done.load(std::memory_order_acquire), "parameter thread completed");
    expect(peakA.load(std::memory_order_relaxed) > 1.0e-4f, "concurrent A non-silent");
    expect(peakB.load(std::memory_order_relaxed) > 1.0e-4f, "concurrent B non-silent");
}

void testDestructionOrder()
{
    auto a = std::make_unique<SwaraXtAudioProcessor>();
    auto b = std::make_unique<SwaraXtAudioProcessor>();
    a->prepareToPlay(44100.0, 128);
    b->prepareToPlay(44100.0, 128);
    (void) renderNote(*a, 60, 128, 4, false);
    (void) renderNote(*b, 64, 128, 4, false);

    a.reset();
    const float bPeak = renderNote(*b, 67, 128, 8);
    a = std::make_unique<SwaraXtAudioProcessor>();
    a->prepareToPlay(44100.0, 128);
    b.reset();
    const float aPeak = renderNote(*a, 55, 128, 8);

    expect(bPeak > 1.0e-4f, "B survives A destruction");
    expect(aPeak > 1.0e-4f, "A survives B destruction after recreate");
}

void testEditorLifecycle()
{
    SwaraXtAudioProcessor proc;
    proc.prepareToPlay(44100.0, 256);

    for (int cycle = 0; cycle < 120; ++cycle)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
        expect(editor != nullptr, "editor created");
        if (editor == nullptr)
            continue;

        expect(! editor->isResizable(), "editor is fixed-size");
        const auto editorBounds = editor->getLocalBounds();
        const bool supportedSize =
            editorBounds == swaraxt::ui::GuiGeometry::editorBounds(swaraxt::ui::GuiSize::small)
            || editorBounds == swaraxt::ui::GuiGeometry::editorBounds(swaraxt::ui::GuiSize::medium)
            || editorBounds == swaraxt::ui::GuiGeometry::editorBounds(swaraxt::ui::GuiSize::large);
        expect(supportedSize, "editor uses one of the three fixed GUI sizes");

        setInt(proc, swaraxt::IDs::osc1Shape, cycle % 35);
        setInt(proc, swaraxt::IDs::lfo1Wave, cycle % 8);
        setFloat(proc, swaraxt::IDs::filterCutoff, 120.0f + static_cast<float>((cycle * 173) % 12000));
        juce::Thread::sleep(1);
    }

    proc.releaseResources();
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("SwaraXtMultiInstanceTests start\n");
    std::fflush(stdout);

    testIndependentPatchState();
    testIndependentNoteAndFilterState();
    testIndependentRandomAndSequencerState();
    testIndependentStateRestore();
    testConcurrentProcessingAndParameterThread();
    testDestructionOrder();
    testEditorLifecycle();

    const int failures = gFailures.load(std::memory_order_relaxed);
    std::printf(failures == 0 ? "SwaraXtMultiInstanceTests: PASSED\n"
                              : "SwaraXtMultiInstanceTests: FAILED (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
