// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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

    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);

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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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

    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);

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
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 2, 5, "LEGACY channel 2->5 preserves Note Off channel");
    }
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 16, 1, "LEGACY channel 16->1 preserves Note Off channel");
    }
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 0, 4, "LEGACY Omni->4 preserves Omni Note Off channel");
    }
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 256);
        expectChannelLifetime(processor, 7, 0, "LEGACY 7->Omni preserves Note Off channel");
    }

    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);

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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
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

namespace swaraxt {
struct SequencerMidiTestAccess {
    static void on(SwaraXtEngine& e, juce::MidiBuffer& output, uint8_t identity, uint8_t note, int channel) {
        e.midiOutput_ = &output; e.midiOutEnabled_ = e.sequencerMidiAllowed_ = true;
        e.midiChannel_ = channel; e.onSequencerNoteOn(identity, 100, note); e.midiOutput_ = nullptr;
    }
    static void off(SwaraXtEngine& e, uint8_t identity) { e.onSequencerNoteOff(identity); }
    static bool queue(SwaraXtEngine& e, bool on, int note, int channel = 16) {
        return e.enqueueScheduledMidi(e.hostBlockOrigin_, on, static_cast<uint8_t>(note), 100,
                                      static_cast<uint8_t>(channel));
    }
    static void cleanup(SwaraXtEngine& e) { e.forceFlushSequencerMidiRelease(e.hostBlockOrigin_); }
    static void drain(SwaraXtEngine& e, juce::MidiBuffer& output) { e.flushSequencerMidi(output, 256); }
    static void future(SwaraXtEngine& e, bool on, int note, int channel = 16) {
        e.enqueueScheduledMidi(e.hostBlockOrigin_ + 4096, on, static_cast<uint8_t>(note), 100, static_cast<uint8_t>(channel));
    }
    static int count(const SwaraXtEngine& e) { return e.scheduledMidiCount_; }
    static int owners(const SwaraXtEngine& e, int note) { return e.externalOwners_[static_cast<size_t>(note)]; }
    static int identity(const SwaraXtEngine& e, int id) { return e.identityExternal_[static_cast<size_t>(id)]; }
};
}

namespace {
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool trackingMidiAllocations = false;
int midiCallbackAllocations = 0;
int midiAllocationHook(int type, void*, size_t, int, long, const unsigned char*, int)
{
    if (trackingMidiAllocations && (type == _HOOK_ALLOC || type == _HOOK_REALLOC))
        ++midiCallbackAllocations;
    return 1;
}
#endif
void checkedProcess(SwaraXtAudioProcessor& p, juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    trackingMidiAllocations = true;
#endif
    p.processBlock(audio, midi);
#if defined(_MSC_VER) && defined(_DEBUG)
    trackingMidiAllocations = false;
#endif
}

void testSchedulerCapacityAndRecovery()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    for (bool ons : {false, true}) for (int count : {0, 1, 2, 127, 128, 129, 256})
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
        auto& e = p.engineForTests();
        int accepted = 0;
        for (int i = 0; i < count; ++i) accepted += Access::queue(e, ons, i % 128);
        expect(accepted == std::min(count, 128) && Access::count(e) == accepted, "fixed scheduler capacity");
    }
    for (int cleanup = 0; cleanup < 4; ++cleanup)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
        armLegacySequence(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
        auto& e = p.engineForTests(); e.applyParameters();
        juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
        midi.ensureSize(swaraxt::kMidiOutputReserveBytes);
        const auto* storage = midi.data.begin();
        OpenTracker ledger;
        Access::on(e, midi, 9, 60, 3);
        checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi)) ledger.onEvent(event);
        expect(ledger.stuck() == 1, "reference ledger contains an actually emitted Note On");
        // This is a deliberately full all-Off reservoir. No injected event can
        // release channel 3/note 60, so only the requested release can close it.
        for (int i = 0; i < 128; ++i) expect(Access::queue(e, false, i), "fill all-Off reservoir");
        Access::off(e, 9);
        if (cleanup == 1) e.reset();
        if (cleanup == 2) { p.releaseResources(); p.prepareToPlay(96000, 256); }
        if (cleanup == 3) setBool(p, swaraxt::IDs::seqMidiOut, false);
        for (int block = 0; block < 6; ++block)
        {
            midi.clear(); checkedProcess(p, audio, midi);
            expect(midi.data.begin() == storage, "recovery retains reserved host storage");
            expect(midi.getNumEvents() <= 128, "recovery keeps bounded emitted event count");
            for (const auto& event : notesIn(midi))
            {
                expect(event.offset >= 0 && event.offset < 256, "recovery timestamp inside block");
                if (event.channel == 3 && event.note == 60) ledger.onEvent(event);
            }
        }
        std::printf("all-Off recovery cleanup=%d outstanding=%d queued=%d\n", cleanup, ledger.stuck(), Access::count(e));
        expect(ledger.stuck() == 0, "rejected release eventually terminates transmitted note");
    }
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
        auto& e = p.engineForTests(); juce::MidiBuffer midi;
        for (int i = 0; i < 128; ++i) Access::queue(e, true, i);
        Access::on(e, midi, 11, 70, 2);
        expect(Access::identity(e, 11) == 255 && Access::owners(e, 70) == 0,
               "rejected Note On does not acquire external ownership");
    }
}

void testRealIdentityReuseAcrossLifecycle()
{
    for(int lifecycle=0;lifecycle<3;++lifecycle) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p; HostPlayHead host;
        p.setPlayHead(&host);p.prepareToPlay(48000,64);armLegacyHeldNote(p);
        setBool(p,swaraxt::IDs::seqMidiOut,true);setInt(p,swaraxt::IDs::seqClockMode,1);
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(2048);
        OpenTracker ledger;int ons=0;bool newChannel=false;double rate=48000;
        const auto render=[&] {
            checkedProcess(p,audio,midi);
            for(const auto& event:notesIn(midi)) { ledger.onEvent(event); if(event.noteOn){++ons;newChannel|=event.channel==5;} }
            if(host.playing)host.ppq+=128.0/rate;
        };
        for(int b=0;b<100&&ons==0;++b){midi.clear();if(b==0)midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),0);render();}
        expect(ons>0,"real reuse starts with a transmitted firmware note");
        if(lifecycle==0){host.playing=false;midi.clear();render();host.playing=true;host.ppq=0;}
        if(lifecycle==1)host.ppq=2.0;
        if(lifecycle==2){p.releaseResources();p.prepareToPlay(96000,64);rate=96000;}
        setInt(p,swaraxt::IDs::midiChannel,5);
        for(int b=0;b<300;++b){midi.clear();if(b==0)midi.addEvent(juce::MidiMessage::noteOn(5,60,juce::uint8(100)),0);render();}
        host.playing=false;
        for(int b=0;b<8;++b){midi.clear();render();}
        expect(ons>=2&&newChannel,"firmware reuses identity with a new output channel");
        expect(ledger.stuck()==0,"real stop/seek/reprepare reuse leaves no external note outstanding");
        p.setPlayHead(nullptr);
    }
}

void testMaximumReleaseRecovery()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 64);
    armLegacySequence(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
    auto& e=p.engineForTests(); e.applyParameters();
    juce::AudioBuffer<float> audio(2,64); juce::MidiBuffer out; out.ensureSize(2048);
    OpenTracker ledger;
    for(int i=0;i<128;++i) Access::on(e,out,static_cast<uint8_t>(i),static_cast<uint8_t>(i),3);
    Access::drain(e,out);
    for(const auto& event:notesIn(out)) ledger.onEvent(event);
    expect(ledger.stuck()==128,"maximum recovery starts with128 actually emitted notes");
    for(int i=0;i<128;++i) Access::queue(e,false,i);
    for(int i=0;i<128;++i) Access::off(e,static_cast<uint8_t>(i));
    int completed=0;
    for(int b=0;b<6;++b) {
        out.clear(); checkedProcess(p,audio,out);
        expect(out.getNumEvents()<=128,"maximum recovery output bound");
        for(const auto& event:notesIn(out)) if(event.channel==3) ledger.onEvent(event);
        if(!completed && ledger.stuck()==0) completed=b+1;
    }
    expect(completed>0&&completed<=3,"maximum pending releases recover within3 callbacks");
    std::printf("maximum release recovery callbacks=%d outstanding=%d\n",completed,ledger.stuck());
}

void testRetriggerAdmissionMatrix()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    int cases = 0, rejectedReplacements = 0;
    for (int capacity : {0, 1, 2, 63, 127, 128, 129, 256})
        for (int contents = 0; contents < 3; ++contents)
            for (int kind = 0; kind < 4; ++kind)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
                SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
                auto& e = p.engineForTests(); juce::MidiBuffer out; out.ensureSize(2048);
                OpenTracker ledger;
                const auto observe = [&] {
                    for (const auto& event : notesIn(out))
                    {
                        expect(event.offset >= 0 && event.offset < 256 && event.channel >= 1 && event.channel <= 16,
                               "admission matrix legal timestamp/channel");
                        expect(event.note >= 0 && event.note < 128 && (!event.noteOn || event.velocity > 0),
                               "admission matrix legal pitch/velocity");
                        if (event.channel != 16) ledger.onEvent(event);
                    }
                };
                Access::on(e, out, 9, 60, 2); Access::drain(e, out); observe();
                expect(ledger.stuck() == 1, "matrix starts from transmitted note");
                for (int i = 0; i < capacity; ++i)
                    Access::queue(e, contents == 0 || (contents == 2 && i % 2 == 0), i % 128);
                const int identity = kind == 3 ? 10 : 9;
                const int note = kind == 1 ? 61 : 60;
                const int channel = kind == 2 ? 5 : 2;
                Access::on(e, out, static_cast<uint8_t>(identity), static_cast<uint8_t>(note), channel);
                const bool accepted = Access::identity(e, identity) != 255;
                if (!accepted) ++rejectedReplacements;
                // Ownership is an extra assertion, never the external-note oracle.
                if (!accepted && identity == 9) expect(Access::owners(e, note) == 0, "rejected replacement has no phantom owner");
                out.clear(); Access::drain(e, out); observe();
                Access::off(e, 9); Access::off(e, 10); Access::cleanup(e);
                out.clear(); Access::drain(e, out); observe();
                expect(ledger.stuck() == 0, "every emitted matrix note terminates on its original channel");
                expect(Access::identity(e, 9) == 255 && Access::identity(e, 10) == 255,
                       "matrix cleanup retires both identities");
                ++cases;
            }
    std::printf("retrigger admission matrix: %d cases, %d rejected replacements\n", cases, rejectedReplacements);
}

void testIdentityReuseBeforeDrain()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    for (bool failedOff : {false, true}) for (bool samePitch : {false, true}) for (bool cleanup : {false, true})
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
        auto& e = p.engineForTests(); juce::MidiBuffer out; out.ensureSize(2048);
        OpenTracker ledger;
        const auto observe = [&] { for (const auto& event : notesIn(out)) if (event.channel != 16) ledger.onEvent(event); };
        Access::on(e, out, 9, 60, 2); Access::drain(e, out); observe();
        if (failedOff) for (int i = 0; i < 128; ++i) Access::queue(e, false, i);
        if (cleanup) Access::cleanup(e); else Access::off(e, 9);
        // Firmware callbacks are synchronous. Reuse follows its preceding Off;
        // there is no delayed callback carrying an obsolete identity generation.
        Access::on(e, out, 9, static_cast<uint8_t>(samePitch ? 60 : 61), 5);
        Access::on(e, out, 10, 60, 2); // Another identity, same pitch, before drain.
        out.clear(); Access::drain(e, out); observe();
        Access::off(e, 9); Access::off(e, 10); Access::cleanup(e);
        out.clear(); Access::drain(e, out); observe();
        // A failed old release can survive the first full reservoir; actual
        // processor callbacks must recover it after storage becomes available.
        juce::AudioBuffer<float> audio(2, 256);
        for (int b = 0; b < 4; ++b) { out.clear(); checkedProcess(p, audio, out); observe(); }
        expect(ledger.stuck() == 0, "identity reuse before output drain retains all external releases");
        expect(Access::owners(e, 60) == 0 && Access::owners(e, 61) == 0, "reuse cleanup has no duplicate owner");
    }
}

void testCleanupRetiresOldIdentity()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
    armLegacySequence(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
    auto& e = p.engineForTests(); e.applyParameters();
    juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi; midi.ensureSize(2048);
    OpenTracker ledger;
    Access::on(e, midi, 9, 60, 3); checkedProcess(p, audio, midi);
    for (const auto& event : notesIn(midi)) ledger.onEvent(event);
    expect(ledger.stuck() == 1, "cleanup starts with a transmitted note");
    for (int i = 0; i < 128; ++i) Access::queue(e, false, i);
    Access::cleanup(e);
    for (int b = 0; b < 3; ++b)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi))
            if (event.channel == 3 && event.note == 60) ledger.onEvent(event);
    }
    expect(ledger.stuck() == 0, "cleanup releases the original note");
    Access::on(e, midi, 10, 60, 3);
    Access::off(e, 9); // An old firmware identity cannot own this new note.
    for (int b = 0; b < 3; ++b)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi))
            if (event.channel == 3 && event.note == 60) ledger.onEvent(event);
    }
    expect(ledger.stuck() == 1, "late old identity release cannot cut off a new owner");
    Access::off(e, 10);
    for (int b = 0; b < 3; ++b)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi))
            if (event.channel == 3 && event.note == 60) ledger.onEvent(event);
    }
    expect(ledger.stuck() == 0, "new owner retains its own release responsibility");
}

void testSaturatedRetriggerChannel()
{
    using Access = swaraxt::SequencerMidiTestAccess;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
    armLegacySequence(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
    auto& e=p.engineForTests();e.applyParameters();
    juce::AudioBuffer<float> audio(2,256);juce::MidiBuffer midi;midi.ensureSize(2048);
    OpenTracker ledger;Access::on(e,midi,4,63,2);checkedProcess(p, audio, midi);
    for(const auto& event:notesIn(midi))ledger.onEvent(event);
    for(int i=0;i<128;++i)Access::queue(e,false,i);
    Access::on(e,midi,4,63,5);Access::off(e,4);
    for(int b=0;b<6;++b){midi.clear();checkedProcess(p, audio, midi);
        for(const auto& event:notesIn(midi))if(event.note==63&&(event.channel==2||event.channel==5))ledger.onEvent(event);}
    expect(ledger.stuck()==0,"saturated channel-changing retrigger preserves old release channel");
    expect(Access::identity(e,4)==255&&Access::owners(e,63)==0,"failed retrigger converges to no identity owner");
}

void testDenseRealOutput()
{
    for(double rate:{8000.0,48000.0,96000.0,384000.0})
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        SwaraXtAudioProcessor p;HostPlayHead host;host.bpm=400;p.setPlayHead(&host);
        const int block=static_cast<int>(rate*2.0);
        p.prepareToPlay(rate,block);armLegacySequence(p);
        setInt(p,swaraxt::IDs::seqClockMode,1);setInt(p,swaraxt::IDs::arpGate,11);
        setBool(p,swaraxt::IDs::seqMidiOut,true);
        juce::AudioBuffer<float> audio(2,block);juce::MidiBuffer midi;midi.ensureSize(32768);
        for(int i=0;i<1024;++i)midi.addEvent(juce::MidiMessage::controllerEvent(1,1,0),i);
        const auto* storage=midi.data.begin();OpenTracker ledger;
        checkedProcess(p, audio, midi);
        expect(midi.getNumEvents()<=128&&midi.data.begin()==storage,"large real block output remains bounded and reserved");
        for(const auto& event:notesIn(midi)){expect(event.offset>=0&&event.offset<block,"large-block event offset");ledger.onEvent(event);}
        host.playing=false;host.ppq=400.0/30.0;
        for(int i=0;i<4;++i){midi.clear();checkedProcess(p, audio, midi);for(const auto& event:notesIn(midi))ledger.onEvent(event);}
        expect(ledger.stuck()==0,"dense actual sequencer output terminates on host stop");
        std::printf("dense real rate=%.0f block=%d outstanding=%d\n",rate,block,ledger.stuck());
        p.setPlayHead(nullptr);
    }
}

void testSeekReleasesPreviousStep()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    SwaraXtAudioProcessor p; HostPlayHead host;
    p.setPlayHead(&host); p.prepareToPlay(48000, 64);
    armLegacySequence(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
    setInt(p, swaraxt::IDs::seqClockMode, 1);
    setInt(p, swaraxt::IDs::seqGate, 100); setInt(p, swaraxt::IDs::arpGate, 4);
    auto sequence = p.sequenceState().snapshot(); sequence.length = 5;
    for (int i = 0; i < 16; ++i)
        sequence.steps[static_cast<size_t>(i)] = swaraxt::SequenceSnapshot::pack(static_cast<uint8_t>(0x80 | (48 + i)), 0x70);
    p.sequenceState().store(sequence, false);
    juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi; midi.ensureSize(2048);
    int initialNote = -1;
    for (int block = 0; block < 100 && initialNote < 0; ++block)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi)) if (event.noteOn) initialNote = event.note;
        host.ppq += 64.0 / 24000.0;
    }
    expect(initialNote >= 0, "seek fixture has an actually transmitted note");
    host.ppq = 2.0;
    bool released = false;
    for (int block = 0; block < 100; ++block)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi))
            if (!event.noteOn && event.note == initialNote && event.channel == 1) released = true;
        host.ppq += 64.0 / 24000.0;
    }
    expect(released, "timeline seek releases the previous generated step");
    p.setPlayHead(nullptr);
}

void testReleaseResourcesExternalNote()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    SwaraXtAudioProcessor p; p.prepareToPlay(48000, 256);
    armLegacyHeldNote(p); setBool(p, swaraxt::IDs::seqMidiOut, true);
    juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
    midi.ensureSize(swaraxt::kMidiOutputReserveBytes);
    OpenTracker ledger;
    for (int block = 0; block < 32 && ledger.stuck() == 0; ++block)
    {
        midi.clear();
        if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi)) ledger.onEvent(event);
    }
    expect(ledger.stuck() > 0, "real sequencer has emitted a held external note");
    p.releaseResources(); p.prepareToPlay(96000, 256);
    setBool(p, swaraxt::IDs::seqMidiOut, false);
    for (int block = 0; block < 8; ++block)
    {
        midi.clear(); checkedProcess(p, audio, midi);
        for (const auto& event : notesIn(midi)) ledger.onEvent(event);
    }
    std::printf("release/reprepare real output outstanding=%d\n", ledger.stuck());
    expect(ledger.stuck() == 0, "resource release retains external release responsibility until next callback");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previousHook = _CrtSetAllocHook(midiAllocationHook);
#endif
    testRealIdentityReuseAcrossLifecycle();
    testMaximumReleaseRecovery();
    testRetriggerAdmissionMatrix();
    testIdentityReuseBeforeDrain();
    testSchedulerCapacityAndRecovery();
    testReleaseResourcesExternalNote();
    testCleanupRetiresOldIdentity();
    testSaturatedRetriggerChannel();
    testDenseRealOutput();
    testSeekReleasesPreviousStep();
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previousHook);
    expect(midiCallbackAllocations == 0, "zero allocations during scheduler recovery callbacks");
    std::printf("MIDI recovery callback allocations: %d\n", midiCallbackAllocations);
#endif
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
