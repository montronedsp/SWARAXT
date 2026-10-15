// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/SequencerMidiRoute.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
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

void setBool(SwaraXtAudioProcessor& processor, const char* id, bool value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(value ? 1.0f : 0.0f);
}

struct HostPlayHead : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setIsPlaying(playing);
        info.setPpqPosition(ppq);
        return info;
    }

    double bpm = 120.0;
    double ppq = 0.0;
    bool playing = true;
};

struct MidiNote
{
    int offset = 0;
    int note = 0;
    int velocity = 0;
    bool noteOn = false;
};

std::vector<MidiNote> notesIn(const juce::MidiBuffer& midi)
{
    std::vector<MidiNote> notes;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (! message.isNoteOnOrOff())
            continue;
        notes.push_back({ metadata.samplePosition, message.getNoteNumber(),
                          message.getVelocity(), message.isNoteOn() });
    }
    return notes;
}

void armLegacySequence(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 160);
    setInt(processor, swaraxt::IDs::seqGate, 50);
    processor.sequenceState().setPitchMode(0);
    for (int step = 0; step < 16; ++step)
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | (72 + (step % 3))), 0x70);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 4;
    snapshot.rotation = 0;
    processor.sequenceState().store(snapshot);
    processor.sequenceState().setPitchMode(0);
}

void testPolicy()
{
    expect(swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::SequencerPitchRoute::legacy),
           "legacy route may emit MIDI notes");
    expect(! swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::SequencerPitchRoute::linear),
           "linear route must not emit MIDI notes");
    expect(! swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::SequencerPitchRoute::scale),
           "scale route must not emit MIDI notes");
    expect(swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::SequencerPitchRoute::quantizedCv),
           "quantized CV is allowed by policy for Phase C");
    expect(swaraxt::routeFromStoredPitchMode(0) == swaraxt::SequencerPitchRoute::legacy, "mode 0 is legacy");
    expect(swaraxt::routeFromStoredPitchMode(1) == swaraxt::SequencerPitchRoute::linear, "mode 1 is linear");
}

void testMidiOutOffKeepsInputAudioAndEmitsNothing()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    armLegacySequence(processor);
    setBool(processor, swaraxt::IDs::seqMidiOut, false);
    juce::AudioBuffer<float> buffer(2, 256);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 40, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, midi);
    expect(notesIn(midi).empty(), "MIDI OUT off publishes an empty output buffer");
    float peak = 0.0f;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        peak = std::max(peak, std::abs(buffer.getSample(0, i)));
    for (int block = 0; block < 30 && peak < 1.0e-4f; ++block)
    {
        midi.clear();
        processor.processBlock(buffer, midi);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            peak = std::max(peak, std::abs(buffer.getSample(0, i)));
        expect(notesIn(midi).empty(), "later blocks stay silent on MIDI OUT off");
    }
    expect(peak > 1.0e-4f, "incoming MIDI still drives audio while MIDI OUT is off");
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 40), 0);
    processor.processBlock(buffer, midi);

    SwaraXtAudioProcessor step;
    step.prepareToPlay(48000.0, 128);
    setInt(step, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setBool(step, swaraxt::IDs::seqMidiOut, true);
    juce::AudioBuffer<float> stepBuffer(2, 128);
    juce::MidiBuffer thru;
    thru.addEvent(juce::MidiMessage::noteOn(1, 10, static_cast<juce::uint8>(80)), 4);
    step.processBlock(stepBuffer, thru);
    expect(notesIn(thru).empty(), "keyboard notes are not copied to MIDI output");
}

void testLegacyEmitsAndLinearReleases()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 512);
    armLegacySequence(processor);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    juce::AudioBuffer<float> buffer(2, 512);
    std::vector<MidiNote> ons;
    std::vector<MidiNote> offs;
    int nonZero = 0;
    for (int block = 0; block < 40; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            expect(note.note >= 72 && note.note <= 74, "output notes are sequencer pitches, not the held key");
            expect(note.offset >= 0 && note.offset < 512, "MIDI offset stays inside the host block");
            if (note.offset > 0)
                ++nonZero;
            if (note.noteOn)
                ons.push_back(note);
            else
                offs.push_back(note);
        }
    }
    expect(! ons.empty(), "legacy MIDI OUT emits note ons");
    expect(! offs.empty(), "legacy MIDI OUT emits matching note offs");
    expect(nonZero > 0, "MIDI offsets are not all pinned to the start of the block");
    std::printf("legacy MIDI events: ons=%d offs=%d nonzeroOffsets=%d firstOffset=%d\n",
                static_cast<int>(ons.size()), static_cast<int>(offs.size()), nonZero,
                ons.empty() ? -1 : ons.front().offset);

    processor.sequenceState().setPitchMode(1);
    bool sawOff = false;
    int linearOns = 0;
    for (int block = 0; block < 8; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (note.noteOn)
                ++linearOns;
            else
                sawOff = true;
        }
    }
    expect(linearOns == 0, "LINEAR emits no new MIDI notes");
    expect(sawOff || offs.size() >= ons.size(), "leaving LEGACY does not leave an open note without a note off");
}

void testHostSyncTiming()
{
    SwaraXtAudioProcessor processor;
    HostPlayHead playHead;
    processor.setPlayHead(&playHead);
    processor.prepareToPlay(44100.0, 256);
    armLegacySequence(processor);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    juce::AudioBuffer<float> buffer(2, 256);
    int events = 0;
    int spread = 0;
    int lastOffset = -1;
    for (int block = 0; block < 80; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            ++events;
            if (lastOffset >= 0 && note.offset != lastOffset)
                ++spread;
            lastOffset = note.offset;
            expect(note.offset >= 0 && note.offset < 256, "host-sync MIDI offset is inside the block");
        }
        playHead.ppq += 120.0 / 60.0 * 256.0 / 44100.0;
    }
    playHead.playing = false;
    juce::MidiBuffer stop;
    processor.processBlock(buffer, stop);
    int stuckOns = 0;
    for (const auto& note : notesIn(stop))
        if (note.noteOn)
            ++stuckOns;
    expect(events > 0, "host sync emits sequencer MIDI");
    expect(spread > 0, "host-sync MIDI offsets move inside the block");
    expect(stuckOns == 0, "host stop does not emit new notes");
    std::printf("host-sync MIDI events=%d offsetChanges=%d\n", events, spread);
    processor.setPlayHead(nullptr);
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testPolicy();
    testMidiOutOffKeepsInputAudioAndEmitsNothing();
    testLegacyEmitsAndLinearReleases();
    testHostSyncTiming();
    if (failures == 0)
    {
        std::printf("sequencer MIDI tests: PASSED\n");
        return 0;
    }
    std::printf("sequencer MIDI tests: FAILED (%d)\n", failures);
    return 1;
}
