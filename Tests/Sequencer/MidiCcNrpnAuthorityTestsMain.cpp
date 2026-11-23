// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Plugin/PluginProcessor.h"
#include "shruthi/patch.h"
#include "shruthi/system_settings.h"

#include <cstdio>
#include <vector>

namespace {

using Processor = SwaraXtAudioProcessor;
int failures = 0;

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

void setInt(Processor& processor, const char* id, int value)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

int getInt(const Processor& processor, const char* id)
{
    const auto* raw = processor.getApvts().getRawParameterValue(id);
    return raw != nullptr ? static_cast<int>(std::lround(raw->load())) : -1;
}

shruthi::Part& part(Processor& processor)
{
    return processor.engineForTests().shruthiPart();
}

const shruthi::Part& part(const Processor& processor)
{
    return processor.engineForTests().shruthiPart();
}

void render(Processor& processor, juce::MidiBuffer midi, int samples = 64)
{
    juce::AudioBuffer<float> audio(2, samples);
    processor.processBlock(audio, midi);
}

void renderCc(Processor& processor, int controller, int value, int channel = 1, int samples = 64)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(channel, controller, value), 0);
    render(processor, midi, samples);
}

void renderEmpty(Processor& processor, int samples = 64)
{
    render(processor, juce::MidiBuffer {}, samples);
}

void renderMessage(Processor& processor, const juce::MidiMessage& message)
{
    juce::MidiBuffer midi;
    midi.addEvent(message, 0);
    render(processor, midi);
}

int8_t octave(const Processor& processor)
{
    return part(processor).system_settings().octave;
}

uint8_t raga(const Processor& processor)
{
    return part(processor).system_settings().raga;
}

uint8_t oscShape(const Processor& processor)
{
    return part(processor).patch().osc[0].shape;
}

uint8_t wheel(const Processor& processor)
{
    return part(processor).voice().modulation_source(shruthi::MOD_SRC_WHEEL);
}

uint8_t bend(const Processor& processor)
{
    return part(processor).voice().modulation_source(shruthi::MOD_SRC_PITCH_BEND);
}

uint8_t aftertouch(const Processor& processor)
{
    return part(processor).voice().modulation_source(shruthi::MOD_SRC_AFTERTOUCH);
}

void cc82AndCc83AreNotHiddenPersistentAuthorities()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::osc1Shape, 4);
    setInt(processor, swaraxt::IDs::filterCutoff, 80);
    renderEmpty(processor);

    const auto initialOctave = octave(processor);
    const auto initialRaga = raga(processor);
    expect(initialOctave == 0, "factory system octave starts at 0");
    expect(initialRaga == 0, "factory raga starts at 0");

    renderCc(processor, 82, 127);
    expect(octave(processor) == initialOctave,
           "CC82 must not mutate firmware octave on the dispatch block");
    renderEmpty(processor);
    expect(octave(processor) == initialOctave, "CC82 must not persist as hidden octave state");
    expect(getInt(processor, swaraxt::IDs::osc1Shape) == 4, "CC82 does not alter APVTS oscillator shape");

    renderCc(processor, 83, 127);
    expect(raga(processor) == initialRaga,
           "CC83 must not mutate firmware raga on the dispatch block");
    renderEmpty(processor);
    expect(raga(processor) == initialRaga, "CC83 must not persist as hidden raga/tuning state");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    Processor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    restored.prepareToPlay(48000, 64);
    renderEmpty(restored);
    expect(octave(restored) == initialOctave, "reopened project does not inherit CC82 octave");
    expect(raga(restored) == initialRaga, "reopened project does not inherit CC83 raga");
    expect(getInt(restored, swaraxt::IDs::osc1Shape) == 4, "reopened project keeps the APVTS patch");
}

void persistentPatchCcCannotBypassApvts()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::osc1Shape, 6);
    renderEmpty(processor);
    const auto shape = oscShape(processor);
    expect(shape == 6, "recognizable oscillator shape A is applied");

    renderCc(processor, 20, 0);
    expect(oscShape(processor) == shape, "CC20 must not write firmware oscillator shape");
    renderEmpty(processor);
    expect(oscShape(processor) == shape, "next audio block still uses the APVTS oscillator shape");
    expect(getInt(processor, swaraxt::IDs::osc1Shape) == 6, "APVTS oscillator shape is unchanged");
}

void nrpnCannotWriteSystemOrPatchState()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    const auto initialOctave = octave(processor);
    const auto initialShape = oscShape(processor);

    auto send = [&](const std::vector<juce::MidiMessage>& messages, int samples = 32) {
        juce::MidiBuffer midi;
        int offset = 0;
        for (const auto& message : messages)
            midi.addEvent(message, offset++);
        render(processor, midi, samples);
    };
    const auto nrpnSelect = [](int parameterOffset) {
        return std::vector<juce::MidiMessage> {
            juce::MidiMessage::controllerEvent(1, 99, (parameterOffset >> 7) & 0x7f),
            juce::MidiMessage::controllerEvent(1, 98, parameterOffset & 0x7f)
        };
    };

    auto octaveSelect = nrpnSelect(static_cast<int>(shruthi::PRM_SYS_OCTAVE));
    octaveSelect.push_back(juce::MidiMessage::controllerEvent(1, 6, 0));
    octaveSelect.push_back(juce::MidiMessage::controllerEvent(1, 38, 2));
    send(octaveSelect);
    expect(octave(processor) == initialOctave, "assembled NRPN must not write system octave");

    auto ragaSelect = nrpnSelect(static_cast<int>(shruthi::PRM_SYS_RAGA));
    juce::MidiBuffer first;
    first.addEvent(ragaSelect[0], 0);
    first.addEvent(ragaSelect[1], 1);
    render(processor, first, 32);
    juce::MidiBuffer second;
    second.addEvent(juce::MidiMessage::controllerEvent(1, 38, 7), 0);
    render(processor, second, 32);
    expect(raga(processor) == 0, "NRPN spanning two blocks must not write raga");

    send({
        juce::MidiMessage::controllerEvent(1, 99, 0),
        juce::MidiMessage::controllerEvent(1, 98, 0),
        juce::MidiMessage::controllerEvent(1, 38, 12)
    });
    expect(oscShape(processor) == initialShape, "oscillator NRPN must not bypass APVTS");

    send(nrpnSelect(static_cast<int>(shruthi::PRM_SYS_OCTAVE)));
    send({ juce::MidiMessage::controllerEvent(1, 38, 1) });
    expect(octave(processor) == initialOctave, "incomplete NRPN without data-entry is ignored");
}

void ignoredChannelDoesNotApplyControllers()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::midiChannel, 2);
    renderEmpty(processor);
    renderCc(processor, 1, 127, 1);
    expect(wheel(processor) == 0, "wrong-channel mod wheel is ignored");
    renderCc(processor, 82, 127, 1);
    expect(octave(processor) == 0, "wrong-channel CC82 is ignored");
    renderCc(processor, 1, 127, 2);
    expect(wheel(processor) == static_cast<uint8_t>(127 << 1), "supported-channel mod wheel is accepted");
}

void performanceControllersRemainLive()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);

    renderCc(processor, 1, 0);
    expect(wheel(processor) == 0, "mod wheel 0");
    renderCc(processor, 1, 127);
    expect(wheel(processor) == static_cast<uint8_t>(127 << 1), "mod wheel 127");
    renderEmpty(processor);
    expect(wheel(processor) == static_cast<uint8_t>(127 << 1), "mod wheel survives the next APVTS apply");

    expect(! part(processor).latched(), "sustain starts released");
    renderCc(processor, 64, 127);
    expect(part(processor).latched(), "sustain pedal latches notes");
    renderCc(processor, 64, 0);
    expect(! part(processor).latched(), "sustain pedal release clears latch");

    juce::MidiBuffer bendOn;
    bendOn.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    render(processor, bendOn);
    expect(bend(processor) == static_cast<uint8_t>((16383u) >> 6), "pitch bend is a transient performance control");

    juce::MidiBuffer touch;
    touch.addEvent(juce::MidiMessage::channelPressureChange(1, 90), 0);
    render(processor, touch);
    expect(aftertouch(processor) == static_cast<uint8_t>(90 << 1), "channel aftertouch is a transient performance control");

    juce::MidiBuffer poly;
    poly.addEvent(juce::MidiMessage::aftertouchChange(1, 60, 40), 0);
    render(processor, poly);
    expect(aftertouch(processor) == static_cast<uint8_t>(40 << 1), "poly aftertouch is a transient performance control");

    juce::MidiBuffer expression;
    expression.addEvent(juce::MidiMessage::controllerEvent(1, 11, 90), 0);
    render(processor, expression);
    expect(getInt(processor, swaraxt::IDs::osc1Shape)
               == static_cast<int>(oscShape(processor)),
           "expression CC does not become a hidden patch write");

    juce::MidiBuffer notes;
    notes.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    render(processor, notes);
    juce::MidiBuffer allNotes;
    allNotes.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    render(processor, allNotes);
    juce::MidiBuffer allSound;
    allSound.addEvent(juce::MidiMessage::allSoundOff(1), 0);
    render(processor, allSound);
    juce::MidiBuffer reset;
    reset.addEvent(juce::MidiMessage::controllerEvent(1, 121, 0), 0);
    render(processor, reset);
    expect(wheel(processor) == 0, "Reset All Controllers restores the wheel");
    expect(bend(processor) == 128, "Reset All Controllers restores pitch bend");
}

// CC 123 must not cut a Roland-style sustained chord. Its live keys must
// nevertheless be flagged so lifting the pedal cannot leave a stuck note.
void allNotesOffDuringSustainReleasesOnPedalUp()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);

    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::allNotesOff(1));
    expect(part(processor).latched(), "CC123 must not disable an active sustain pedal");
    expect(part(processor).num_notes() == 1, "CC123 with sustain must keep the note held");
    renderEmpty(processor);
    expect(part(processor).num_notes() == 1, "CC123 must not prematurely release a sustained note");
    renderCc(processor, 64, 0);
    expect(! part(processor).latched(), "pedal-up clears sustain after CC123");
    expect(part(processor).num_notes() == 0, "pedal-up releases the key marked by CC123");

    // Repeated all-notes-off messages must not leave extra owners behind.
    renderMessage(processor, juce::MidiMessage::noteOn(1, 62, (juce::uint8) 96));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::allNotesOff(1));
    renderMessage(processor, juce::MidiMessage::allNotesOff(1));
    renderCc(processor, 64, 0);
    expect(part(processor).num_notes() == 0, "repeated CC123 remains idempotent under sustain");
}

void sustainChordAndPerNoteOffsRemainCompatible()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::noteOff(1, 60));
    renderMessage(processor, juce::MidiMessage::noteOff(1, 64));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::allNotesOff(1));
    renderEmpty(processor);
    expect(part(processor).num_notes() == 2,
           "sustained chord survives per-note offs, CC123 and an empty block");
    renderCc(processor, 64, 0);
    expect(part(processor).num_notes() == 0,
           "pedal-up releases every note from a sustained Roland-style chord");
}

void resetControllersReleasesOnlySustainOwnedNotes()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::noteOff(1, 64));
    expect(part(processor).num_notes() == 2, "sustain holds E4 after its physical note-off");

    renderCc(processor, 1, 127);
    renderMessage(processor, juce::MidiMessage::pitchWheel(1, 16383));
    renderCc(processor, 121, 0);
    expect(! part(processor).latched(), "CC121 resets sustain");
    expect(part(processor).num_notes() == 1,
           "CC121 releases latched E4 but preserves physically held C4");
    expect(wheel(processor) == 0, "CC121 resets mod wheel with a latched key");
    expect(bend(processor) == 128, "CC121 resets pitch bend with a latched key");
    renderMessage(processor, juce::MidiMessage::noteOff(1, 60));
    expect(part(processor).num_notes() == 0, "physical C4 note-off clears the remaining key");

    // CC121 must also release a pedal-held key when no physical key remains.
    renderMessage(processor, juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100));
    renderCc(processor, 64, 127);
    renderMessage(processor, juce::MidiMessage::noteOff(1, 67));
    renderCc(processor, 121, 0);
    expect(part(processor).num_notes() == 0, "CC121 releases a fully latched note");
    renderCc(processor, 121, 0);
    expect(part(processor).num_notes() == 0, "repeated CC121 does not recreate held notes");

    renderMessage(processor, juce::MidiMessage::noteOn(1, 72, (juce::uint8) 100));
    renderCc(processor, 121, 0);
    expect(part(processor).num_notes() == 1,
           "CC121 without pedal does not release a physically held key");
    renderMessage(processor, juce::MidiMessage::noteOff(1, 72));
    expect(part(processor).num_notes() == 0, "physical note-off still works after CC121");
}

void allNotesOffWithoutPedalRemainsOrdinaryRelease()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::allNotesOff(1));
    expect(part(processor).num_notes() == 0,
           "CC123 without sustain clears the pressed-key stack normally");
}

void allSoundOffClearsOwnersAndKillsVoice()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1, "Note On occupies the pressed-key stack");
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "CC120 clears the pressed-key stack");
    expect(! part(processor).latched(), "CC120 does not leave sustain latched");
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 kills the production HD amplitude envelope");
    expect(part(processor).generated_note_count_for_tests() == 0,
           "CC120 releases sequencer/arp generated-note owners");
}

void allSoundOffIgnoresSustainPedal()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderCc(processor, 64, 127);
    expect(part(processor).latched(), "sustain is down before CC120");
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(! part(processor).latched(), "CC120 clears an active sustain pedal");
    expect(part(processor).num_notes() == 0, "CC120 releases a sustain-held note");
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 kills the voice despite sustain");
    renderCc(processor, 64, 0);
    expect(part(processor).num_notes() == 0, "pedal-up after CC120 does not resurrect the note");
}

void allSoundOffClearsAHeldChord()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100));
    expect(part(processor).num_notes() == 3, "three physically held keys");
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "CC120 clears every pressed key");
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 kills the sounding voice of a chord");
}

void allSoundOffIsIdempotentAndAllowsRetrigger()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "CC120 without a note is harmless");
    expect(part(processor).voice().amplitude_envelope_dead(),
           "CC120 without a note leaves the amplitude envelope dead");

    renderMessage(processor, juce::MidiMessage::noteOn(1, 62, (juce::uint8) 96));
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 0, "repeated CC120 remains empty");
    expect(part(processor).voice().amplitude_envelope_dead(),
           "repeated CC120 keeps the envelope dead");

    renderMessage(processor, juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1, "Note On after CC120 occupies the stack");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "Note On after CC120 starts the amplitude envelope");
}

void allSoundOffOnIgnoredChannelDoesNothing()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::midiChannel, 2);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1, "accepted-channel Note On is held");
    renderMessage(processor, juce::MidiMessage::allSoundOff(1));
    expect(part(processor).num_notes() == 1, "wrong-channel CC120 is ignored");
    expect(! part(processor).voice().amplitude_envelope_dead(),
           "wrong-channel CC120 must not kill the voice");
    renderMessage(processor, juce::MidiMessage::allSoundOff(2));
    expect(part(processor).num_notes() == 0, "matching-channel CC120 clears the stack");
}

// Mission 16: channel-filter changes must retire input ownership before the
// old channel's releases become unreachable. A fresh note on the new channel
// must still play; the old channel must no longer be admitted.
void channelSwitchReleasesHeldInput()
{
    const auto exercise = [](int from, int oldInputChannel, int to, int newInputChannel) {
        Processor processor;
        processor.prepareToPlay(48000, 64);
        setInt(processor, swaraxt::IDs::midiChannel, from);
        renderEmpty(processor);

        renderMessage(processor,
                      juce::MidiMessage::noteOn(oldInputChannel, 60, (juce::uint8) 100));
        expect(part(processor).num_notes() == 1, "old input channel accepted the held note");
        setInt(processor, swaraxt::IDs::midiChannel, to);
        renderEmpty(processor);
        expect(part(processor).num_notes() == 0,
               "changing MIDI channel retires the old pressed-key owner");
        expect(part(processor).voice().amplitude_envelope_dead(),
               "channel switch kills old input-owned voice without a release tail");

        renderMessage(processor, juce::MidiMessage::noteOff(oldInputChannel, 60));
        expect(part(processor).num_notes() == 0,
               "late note-off from the previous channel is harmless");

        renderMessage(processor,
                      juce::MidiMessage::noteOn(newInputChannel, 65, (juce::uint8) 100));
        expect(part(processor).num_notes() == 1,
               "new channel accepts an independent fresh note");
        expect(! part(processor).voice().amplitude_envelope_dead(),
               "fresh note after channel switch wakes the voice");
        if (to != 0 && oldInputChannel != newInputChannel)
        {
            renderMessage(processor,
                          juce::MidiMessage::noteOn(oldInputChannel, 67, (juce::uint8) 100));
            expect(part(processor).num_notes() == 1,
                   "discarded old channel cannot acquire a new note owner");
        }
        renderMessage(processor, juce::MidiMessage::noteOff(newInputChannel, 65));
        expect(part(processor).num_notes() == 0,
               "new channel's matching note-off works normally");
    };

    exercise(1, 1, 2, 2);
    exercise(0, 5, 3, 3);
    exercise(16, 16, 1, 1);
    exercise(2, 2, 0, 7);
}

void channelSwitchReleasesSustainOwnership()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::midiChannel, 1);
    renderEmpty(processor);

    renderCc(processor, 64, 127, 1);
    expect(part(processor).latched(), "old-channel sustain-down precondition");
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    renderMessage(processor, juce::MidiMessage::noteOff(1, 60));
    expect(part(processor).num_notes() == 1 && part(processor).latched(),
           "note-off under the old pedal is genuinely sustain-owned");

    setInt(processor, swaraxt::IDs::midiChannel, 2);
    renderEmpty(processor);
    expect(! part(processor).latched() && part(processor).num_notes() == 0,
           "channel switch clears the pedal and all sustain-owned keys");
    renderCc(processor, 64, 0, 1);
    expect(! part(processor).latched() && part(processor).num_notes() == 0,
           "ignored old-channel pedal-up cannot resurrect note ownership");

    renderMessage(processor, juce::MidiMessage::noteOn(2, 62, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1,
           "new input channel starts with no inherited sustain");
    renderMessage(processor, juce::MidiMessage::noteOff(2, 62));
    expect(part(processor).num_notes() == 0,
           "new-channel note-off releases without an inherited pedal");

    // A pedal with no keys is still controller-owned state that needs clearing.
    renderCc(processor, 64, 127, 2);
    expect(part(processor).latched(), "pedal-only precondition on channel 2");
    setInt(processor, swaraxt::IDs::midiChannel, 3);
    renderEmpty(processor);
    expect(! part(processor).latched(),
           "pedal-only channel switch clears stale sustain ownership");
}

void unchangedChannelKeepsSounding()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::midiChannel, 1);
    renderEmpty(processor);
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    expect(part(processor).num_notes() == 1, "same-channel test holds one note");
    renderEmpty(processor);
    setInt(processor, swaraxt::IDs::midiChannel, 1);
    renderEmpty(processor);
    expect(part(processor).num_notes() == 1,
           "reapplying unchanged MIDI channel must not panic a held note");
    renderMessage(processor, juce::MidiMessage::noteOff(1, 60));
    expect(part(processor).num_notes() == 0,
           "unchanged-channel held note releases normally");
}

void sustainBeforeFirstNoteSurvivesClockStart()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    expect(! part(processor).running(), "internal firmware clock is initially stopped");
    renderCc(processor, 64, 127);
    expect(part(processor).latched(), "sustain pedal accepted before first note");
    renderMessage(processor, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100));
    expect(part(processor).running(), "first Note On starts the internal firmware clock");
    expect(part(processor).latched(), "clock Start must preserve the pressed pedal");
    renderMessage(processor, juce::MidiMessage::noteOff(1, 60));
    expect(part(processor).num_notes() == 1,
           "first note stays latched when Note Off arrives with sustain down");
    renderEmpty(processor);
    expect(part(processor).num_notes() == 1, "sustain survives subsequent process blocks");
    renderCc(processor, 64, 0);
    expect(! part(processor).latched() && part(processor).num_notes() == 0,
           "pedal-up after Start releases the originally sustained note");
}

void controllerSweepPreservesPersistentAuthority()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    renderEmpty(processor);
    const int values[] = { 0, 1, 63, 64, 127 };
    std::vector<float> parameterBefore;
    parameterBefore.reserve(static_cast<size_t>(processor.getParameters().size()));
    for (auto* parameter : processor.getParameters())
        parameterBefore.push_back(parameter->getValue());
    const auto originalOctave = octave(processor);
    const auto originalRaga = raga(processor);
    const auto originalShape = oscShape(processor);

    for (int controller = 0; controller < 128; ++controller)
        for (const int value : values)
        {
            renderCc(processor, controller, value);
            expect(octave(processor) == originalOctave && raga(processor) == originalRaga
                       && oscShape(processor) == originalShape,
                   "MIDI CC sweep cannot mutate hidden firmware patch/system authority");
            const auto& parameters = processor.getParameters();
            for (int index = 0; index < parameters.size(); ++index)
                expect(parameters[index]->getValue() == parameterBefore[static_cast<size_t>(index)],
                       "MIDI CC sweep cannot change any APVTS parameter");
        }
    // Positive control: supported transient CC1 must still reach the voice.
    renderCc(processor, 1, 127);
    expect(wheel(processor) == static_cast<uint8_t>(127 << 1),
           "positive control: MIDI CC sweep reaches performance controllers");
    renderCc(processor, 1, 0);
    expect(wheel(processor) == 0, "mod wheel clears normally after the CC sweep");
}

void twoInstancesStayIndependent()
{
    Processor a, b;
    a.prepareToPlay(48000, 64);
    b.prepareToPlay(48000, 64);
    renderCc(a, 1, 127);
    renderCc(b, 1, 0);
    renderCc(a, 82, 127);
    renderCc(b, 83, 127);
    expect(wheel(a) == static_cast<uint8_t>(127 << 1), "instance A keeps its mod wheel");
    expect(wheel(b) == 0, "instance B keeps its mod wheel");
    expect(octave(a) == 0 && octave(b) == 0, "neither instance accepts CC82 octave");
    expect(raga(a) == 0 && raga(b) == 0, "neither instance accepts CC83 raga");
}

}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI initialise;
    cc82AndCc83AreNotHiddenPersistentAuthorities();
    persistentPatchCcCannotBypassApvts();
    nrpnCannotWriteSystemOrPatchState();
    ignoredChannelDoesNotApplyControllers();
    performanceControllersRemainLive();
    allNotesOffDuringSustainReleasesOnPedalUp();
    sustainChordAndPerNoteOffsRemainCompatible();
    resetControllersReleasesOnlySustainOwnedNotes();
    allNotesOffWithoutPedalRemainsOrdinaryRelease();
    allSoundOffClearsOwnersAndKillsVoice();
    allSoundOffIgnoresSustainPedal();
    allSoundOffClearsAHeldChord();
    allSoundOffIsIdempotentAndAllowsRetrigger();
    allSoundOffOnIgnoredChannelDoesNothing();
    channelSwitchReleasesHeldInput();
    channelSwitchReleasesSustainOwnership();
    unchangedChannelKeepsSounding();
    sustainBeforeFirstNoteSurvivesClockStart();
    controllerSweepPreservesPersistentAuthority();
    twoInstancesStayIndependent();
    std::printf("MIDI CC/NRPN authority tests: %s (%d failures)\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
