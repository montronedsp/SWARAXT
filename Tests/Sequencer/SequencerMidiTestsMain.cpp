// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "Engine/FreeCvPitch.h"
#include "Engine/SequencerMidiRoute.h"
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
    int channel = 1;
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
                          message.getVelocity(), message.getChannel(), message.isNoteOn() });
    }
    return notes;
}

struct OpenTracker
{
    void onEvent(const MidiNote& note)
    {
        const auto key = (static_cast<uint32_t>(note.channel) << 8) | static_cast<uint32_t>(note.note);
        if (note.noteOn)
            ++open[key];
        else
        {
            auto it = open.find(key);
            if (it == open.end() || it->second == 0)
                ++orphanOffs;
            else if (--it->second == 0)
                open.erase(it);
        }
    }

    int stuck() const
    {
        int total = 0;
        for (const auto& entry : open)
            total += entry.second;
        return total;
    }

    std::map<uint32_t, int> open;
    int orphanOffs = 0;
};

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

void armLegacyHeldNote(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 90);
    setInt(processor, swaraxt::IDs::seqGate, 127);
    processor.sequenceState().setPitchMode(0);
    for (int step = 0; step < 16; ++step)
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 72), 0x70);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
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
    expect(swaraxt::routeFromStoredPitchMode(2) == swaraxt::SequencerPitchRoute::scale, "mode 2 is scale");
    expect(swaraxt::routeFromStoredPitchMode(3) == swaraxt::SequencerPitchRoute::quantizedCv,
           "mode 3 is quantized CV");
    expect(swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::routeFromStoredPitchMode(3)),
           "stored QUANTIZED CV mode emits MIDI");
    expect(! swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::routeFromStoredPitchMode(2)),
           "stored SCALE mode does not emit MIDI");
    expect(swaraxt::routeFromStoredPitchMode(4) == swaraxt::SequencerPitchRoute::invalid,
           "unknown mode is not legacy");
    expect(! swaraxt::sequencerRouteEmitsMidiNotes(swaraxt::routeFromStoredPitchMode(9)),
           "corrupted mode does not emit MIDI");
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

    SwaraXtAudioProcessor scaleProcessor;
    scaleProcessor.prepareToPlay(48000.0, 256);
    armLegacySequence(scaleProcessor);
    setBool(scaleProcessor, swaraxt::IDs::seqMidiOut, true);
    juce::AudioBuffer<float> scaleBuffer(2, 256);
    juce::MidiBuffer prime;
    prime.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    for (int block = 0; block < 8; ++block)
        scaleProcessor.processBlock(scaleBuffer, prime);
    scaleProcessor.sequenceState().setPitchMode(2);
    int scaleOns = 0;
    int scaleOffs = 0;
    for (int block = 0; block < 6; ++block)
    {
        juce::MidiBuffer midi;
        scaleProcessor.processBlock(scaleBuffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (note.noteOn)
                ++scaleOns;
            else
                ++scaleOffs;
        }
    }
    expect(scaleOns == 0, "SCALE emits no new MIDI notes");
    expect(scaleOffs > 0, "entering SCALE releases notes already sent");
}

void expectChannelLifetime(SwaraXtAudioProcessor& processor,
                         int fromChannel,
                         int toChannel,
                         const char* label)
{
    armLegacyHeldNote(processor);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    setInt(processor, swaraxt::IDs::midiChannel, fromChannel);
    processor.engineForTests().applyParameters();
    // Incoming hold-key must match the configured channel filter (Omni accepts any).
    const int inputChannel = fromChannel >= 1 && fromChannel <= 16 ? fromChannel : 1;
    juce::AudioBuffer<float> buffer(2, 256);
    int onNote = -1;
    int onChannel = -1;
    OpenTracker live;
    for (int block = 0; block < 48 && onNote < 0; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(inputChannel, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            live.onEvent(note);
            if (note.noteOn)
            {
                onNote = note.note;
                onChannel = note.channel;
            }
        }
    }
    expect(onNote >= 0, label);
    expect(live.stuck() > 0, label);
    const int expectedOnChannel = fromChannel >= 1 && fromChannel <= 16 ? fromChannel : 1;
    expect(onChannel == expectedOnChannel, label);

    setInt(processor, swaraxt::IDs::midiChannel, toChannel);
    processor.engineForTests().applyParameters();

    setBool(processor, swaraxt::IDs::seqMidiOut, false);
    processor.engineForTests().applyParameters();

    int matchingOff = 0;
    int wrongChannelOff = 0;
    int newChannelOn = 0;
    for (int block = 0; block < 8; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            live.onEvent(note);
            if (note.noteOn)
            {
                const int expectedNew = toChannel >= 1 && toChannel <= 16 ? toChannel : 1;
                if (note.channel == expectedNew)
                    ++newChannelOn;
            }
            else if (note.note == onNote)
            {
                if (note.channel == onChannel)
                    ++matchingOff;
                else
                    ++wrongChannelOff;
            }
        }
    }
    expect(matchingOff > 0, label);
    expect(wrongChannelOff == 0, label);
    expect(newChannelOn == 0, label);
    expect(live.stuck() == 0 && live.orphanOffs == 0, label);
}

void testChannelLifetimeAcrossParameterChanges()
{
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 2, 5, "LEGACY channel 2->5 preserves Note Off channel");
    }
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 16, 1, "LEGACY channel 16->1 preserves Note Off channel");
    }
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 0, 4, "LEGACY Omni->4 preserves Omni Note Off channel");
    }
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 7, 0, "LEGACY 7->Omni preserves Note Off channel");
    }

    SwaraXtAudioProcessor quantized;
    quantized.prepareToPlay(48000.0, 256);
    expect(quantized.loadBuiltinTuning(1), "19-EDO channel fixture");
    setInt(quantized, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(quantized, swaraxt::IDs::seqClockMode, 0);
    setInt(quantized, swaraxt::IDs::seqTempo, 160);
    setInt(quantized, swaraxt::IDs::seqGate, 100);
    setInt(quantized, swaraxt::IDs::perfGlide, 0);
    setInt(quantized, swaraxt::IDs::dspFxProgram, 0);
    quantized.sequenceState().setPitchMode(3);
    const int32_t units = 60 * 1024 + static_cast<int32_t>(std::lround(0.73 * 1024.0));
    for (int step = 0; step < 16; ++step)
    {
        quantized.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        quantized.sequenceState().setFreePitch(step, units);
    }
    auto snapshot = quantized.sequenceState().snapshot();
    snapshot.length = 1;
    snapshot.pitchMode = 3;
    quantized.sequenceState().store(snapshot);
    quantized.sequenceState().setPitchMode(3);
    setBool(quantized, swaraxt::IDs::seqMidiOut, true);
    const auto chosen = quantized.quantizeActiveHz(swaraxt::FreeCvPitch::hzFromUnits(units));
    const int external = swaraxt::ScalePitch::nearestMidiNote(chosen.hz);
    expect(chosen.valid, "QUANTIZED CV channel fixture quantizes");

    setInt(quantized, swaraxt::IDs::midiChannel, 2);
    quantized.engineForTests().applyParameters();
    juce::AudioBuffer<float> buffer(2, 256);
    int onChannel = -1;
    int onNote = -1;
    for (int block = 0; block < 48 && onNote < 0; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(2, 60, static_cast<juce::uint8>(100)), 0);
        quantized.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (! note.noteOn)
                continue;
            onNote = note.note;
            onChannel = note.channel;
            expect(note.note == external, "QUANTIZED CV Note On uses external pitch");
            expect(note.channel == 2, "QUANTIZED CV Note On uses channel 2");
        }
    }
    expect(onNote == external && onChannel == 2, "QUANTIZED CV emitted on channel 2");

    setInt(quantized, swaraxt::IDs::midiChannel, 5);
    quantized.engineForTests().applyParameters();
    quantized.sequenceState().setPitchMode(1);
    quantized.engineForTests().applyParameters();
    int matchingOff = 0;
    int wrong = 0;
    for (int block = 0; block < 8; ++block)
    {
        juce::MidiBuffer midi;
        quantized.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (note.noteOn)
                ++wrong;
            else if (note.note == onNote && note.channel == 2)
                ++matchingOff;
            else
                ++wrong;
        }
    }
    expect(matchingOff > 0 && wrong == 0, "QUANTIZED CV->LINEAR Note Off stays on channel 2");

    // New note after channel change must use the new channel.
    SwaraXtAudioProcessor next;
    next.prepareToPlay(48000.0, 256);
    armLegacySequence(next);
    setBool(next, swaraxt::IDs::seqMidiOut, true);
    setInt(next, swaraxt::IDs::midiChannel, 5);
    next.engineForTests().applyParameters();
    int sawChannel5 = 0;
    for (int block = 0; block < 40; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(5, 60, static_cast<juce::uint8>(100)), 0);
        next.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
            if (note.noteOn && note.channel == 5)
                ++sawChannel5;
    }
    expect(sawChannel5 > 0, "subsequent notes use the updated MIDI channel");
}

void testModeTransitionCleanupChannels()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    armLegacyHeldNote(processor);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    setInt(processor, swaraxt::IDs::midiChannel, 11);
    processor.engineForTests().applyParameters();
    juce::AudioBuffer<float> buffer(2, 256);
    OpenTracker tracker;
    int onChannel = -1;
    for (int block = 0; block < 32 && tracker.stuck() == 0; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(11, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            tracker.onEvent(note);
            if (note.noteOn)
                onChannel = note.channel;
        }
    }
    expect(onChannel == 11, "LEGACY held note used channel 11");
    expect(tracker.stuck() > 0, "LEGACY note remains held before mode change");

    processor.sequenceState().setPitchMode(2);
    processor.engineForTests().applyParameters();
    int scaleWrong = 0;
    for (int block = 0; block < 6; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            tracker.onEvent(note);
            if (note.noteOn)
                ++scaleWrong;
            else if (note.channel != 11)
                ++scaleWrong;
        }
    }
    expect(scaleWrong == 0, "LEGACY->SCALE cleanup keeps original channel");
    expect(tracker.stuck() == 0 && tracker.orphanOffs == 0, "LEGACY->SCALE leaves no stuck notes");

    {
        SwaraXtAudioProcessor stopProcessor;
        stopProcessor.prepareToPlay(48000.0, 256);
        HostPlayHead playHead;
        playHead.playing = true;
        stopProcessor.setPlayHead(&playHead);
        armLegacyHeldNote(stopProcessor);
        setBool(stopProcessor, swaraxt::IDs::seqMidiOut, true);
        setInt(stopProcessor, swaraxt::IDs::midiChannel, 3);
        setInt(stopProcessor, swaraxt::IDs::seqClockMode, 1);
        stopProcessor.engineForTests().applyParameters();
        OpenTracker resetTracker;
        for (int block = 0; block < 24 && resetTracker.stuck() == 0; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(3, 60, static_cast<juce::uint8>(100)), 0);
            stopProcessor.processBlock(buffer, midi);
            for (const auto& note : notesIn(midi))
                resetTracker.onEvent(note);
            playHead.ppq += 120.0 / 60.0 * 256.0 / 48000.0;
        }
        expect(resetTracker.stuck() > 0, "host-sync note is held before stop");
        playHead.playing = false;
        juce::MidiBuffer stop;
        stopProcessor.processBlock(buffer, stop);
        for (const auto& note : notesIn(stop))
        {
            resetTracker.onEvent(note);
            if (! note.noteOn)
                expect(note.channel == 3, "transport stop Note Off uses held channel");
        }
        expect(resetTracker.stuck() == 0, "transport stop clears held notes");
        stopProcessor.setPlayHead(nullptr);
    }

    {
        SwaraXtAudioProcessor resetProcessor;
        resetProcessor.prepareToPlay(48000.0, 256);
        armLegacyHeldNote(resetProcessor);
        setBool(resetProcessor, swaraxt::IDs::seqMidiOut, true);
        setInt(resetProcessor, swaraxt::IDs::midiChannel, 4);
        resetProcessor.engineForTests().applyParameters();
        OpenTracker beforeReset;
        for (int block = 0; block < 24 && beforeReset.stuck() == 0; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(4, 60, static_cast<juce::uint8>(100)), 0);
            resetProcessor.processBlock(buffer, midi);
            for (const auto& note : notesIn(midi))
                beforeReset.onEvent(note);
        }
        expect(beforeReset.stuck() > 0, "note held before engine reset");
        const int heldNote = [&] {
            for (const auto& entry : beforeReset.open)
                return static_cast<int>(entry.first & 0xFFu);
            return -1;
        }();
        resetProcessor.engineForTests().reset();
        juce::MidiBuffer afterReset;
        resetProcessor.processBlock(buffer, afterReset);
        int resetOffs = 0;
        int resetWrong = 0;
        for (const auto& note : notesIn(afterReset))
        {
            beforeReset.onEvent(note);
            if (note.noteOn)
                ++resetWrong;
            else if (note.note == heldNote && note.channel == 4)
                ++resetOffs;
            else
                ++resetWrong;
        }
        expect(resetOffs > 0 && resetWrong == 0, "engine reset flushes Note Off on the held channel");
        expect(beforeReset.stuck() == 0, "engine reset clears held external notes");
    }
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
    testChannelLifetimeAcrossParameterChanges();
    testModeTransitionCleanupChannels();
    testHostSyncTiming();
    if (failures == 0)
    {
        std::printf("sequencer MIDI tests: PASSED\n");
        return 0;
    }
    std::printf("sequencer MIDI tests: FAILED (%d)\n", failures);
    return 1;
}
