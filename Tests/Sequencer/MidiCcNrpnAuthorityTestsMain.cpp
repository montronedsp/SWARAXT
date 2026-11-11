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
    twoInstancesStayIndependent();
    std::printf("MIDI CC/NRPN authority tests: %s (%d failures)\n",
                failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
