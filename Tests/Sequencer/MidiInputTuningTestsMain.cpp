// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "Engine/MidiInputTuning.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/ScalePitch.h"
#include "Tuning/TuningTable.h"
#include "Ui/SwaraXtPanels.h"
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

double hzFromWord(int word)
{
    const double semis = static_cast<double>(word) / 128.0;
    return 440.0 * std::pow(2.0, (semis - 69.0) / 12.0);
}

uint16_t expectedTuned(SwaraXtAudioProcessor& processor, int note)
{
    int16_t degrees[128];
    processor.copyTuningDegrees(degrees, 128);
    if (note < 0 || note > 127 || degrees[note] < 0)
        return 0;
    return swaraxt::ScalePitch::toVoicePitchFromHz(processor.hzForActiveDegree(degrees[note]));
}

uint16_t playNote(SwaraXtAudioProcessor& processor, int note, int channel = 1, int blocks = 4)
{
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    {
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allNotesOff(channel), 0);
        processor.processBlock(buffer, off);
    }
    uint16_t word = 0;
    for (int block = 0; block < blocks; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        word = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    return word;
}

void setStepMode(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::perfLegato, 0);
    setInt(processor, swaraxt::IDs::midiChannel, 0);
}

void testOffIsOriginalTune()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 0);
    const auto word = playNote(processor, 60);
    expect(word == 60 * 128, "OFF MIDI 60 uses the original 128-unit Tune word");
    std::printf("OFF MIDI 60 word=%u hz=%.6f\n", word, hzFromWord(word));

    processor.engineForTests().shruthiPart().mutable_system_settings()->raga = 1;
    const auto ragaWord = playNote(processor, 61);
    expect(ragaWord == 61 * 128 + 15, "OFF applies the just-raga offset from Tune()");
    std::printf("OFF raga MIDI 61 word=%u hz=%.6f\n", ragaWord, hzFromWord(ragaWord));

    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadBuiltinTuning(0), "12-EDO over raga");
    const auto tuned = playNote(processor, 61);
    expect(tuned == expectedTuned(processor, 61), "TUNED ignores the stored raga offset");
    expect(tuned != ragaWord, "TUNED pitch is not the raga pitch");
    expect(processor.engineForTests().shruthiPart().system_settings().raga == 1,
           "switching to TUNED keeps the raga setting stored");
    setInt(processor, swaraxt::IDs::midiInputTuning, 0);
    processor.engineForTests().shruthiPart().mutable_system_settings()->raga = 0;
    expect(playNote(processor, 61) == 61 * 128, "clearing raga restores the original Tune word");
}

void testTunedMapping()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);

    expect(processor.loadBuiltinTuning(0), "12-EDO");
    const auto a4 = playNote(processor, 69);
    expect(a4 == expectedTuned(processor, 69), "TUNED A4 follows the SWT1 keyboard degree");
    std::printf("TUNED 12-EDO A4 word=%u hz=%.6f\n", a4, hzFromWord(a4));

    expect(processor.loadBuiltinTuning(1), "19-EDO");
    const auto key = playNote(processor, 61);
    expect(key == expectedTuned(processor, 61), "TUNED 19-EDO uses the mapped degree");
    expect(key != 61 * 128, "TUNED 19-EDO is not ordinary 12-TET");
    std::printf("TUNED 19-EDO key 61 word=%u hz=%.6f\n", key, hzFromWord(key));

    expect(processor.loadBuiltinTuning(2), "24-EDO");
    const auto edo24 = playNote(processor, 63);
    expect(edo24 == expectedTuned(processor, 63), "TUNED 24-EDO");

    expect(processor.loadBuiltinTuning(3), "31-EDO");
    const auto edo31 = playNote(processor, 70);
    expect(edo31 == expectedTuned(processor, 70), "TUNED 31-EDO");
    std::printf("TUNED 31-EDO key 70 word=%u hz=%.6f\n", edo31, hzFromWord(edo31));

    expect(processor.loadBuiltinTuning(4), "just");
    const auto justWord = playNote(processor, 67);
    expect(justWord == expectedTuned(processor, 67), "TUNED just intonation follows the keyboard map");
    std::printf("TUNED just key 67 word=%u hz=%.6f\n", justWord, hzFromWord(justWord));

    expect(processor.loadBuiltinTuning(5), "tritave");
    const auto tritaveWord = playNote(processor, 62);
    expect(tritaveWord == expectedTuned(processor, 62), "TUNED tritave follows its map");
    std::printf("TUNED tritave key 62 word=%u hz=%.6f\n", tritaveWord, hzFromWord(tritaveWord));
}

void testQuantizedInput()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    const int tunings[] = { 1, 2, 3, 4, 5 };
    const char* names[] = { "19-EDO", "24-EDO", "31-EDO", "just", "tritave" };
    const int notes[] = { 60, 61, 63, 67, 70 };
    for (int index = 0; index < 5; ++index)
    {
        expect(processor.loadBuiltinTuning(tunings[index]), names[index]);
        const auto word = playNote(processor, notes[index]);
        const double source = 440.0 * std::pow(2.0, (static_cast<double>(notes[index]) - 69.0) / 12.0);
        const auto chosen = processor.quantizeActiveHz(source);
        const auto expected = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
        expect(chosen.valid && word == expected, names[index]);
        std::printf("QUANTIZED %s MIDI %d -> %.6f Hz word=%u rendered=%.6f\n",
                    names[index], notes[index], chosen.hz, word, hzFromWord(word));
    }

    expect(processor.loadBuiltinTuning(4), "just");
    std::printf("QUANTIZED families complete\n");
}

void testUnmappedAndAllocation()
{
    const char* scale =
        "! partial\n"
        "partial map\n"
        "12\n"
        "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n"
        "700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";
    const char* kbm =
        "2\n60\n127\n60\n60\n261.6255653006\n12\n0\nx\n";
    swaraxt::tuning::ParseError error;
    swaraxt::tuning::TuningSnapshot snapshot;
    expect(swaraxt::tuning::parseScalaAndKeyboard(scale, kbm, snapshot, error), "partial map parses");
    std::vector<uint8_t> bytes;
    expect(swaraxt::tuning::writeTuningTable(snapshot, bytes, error), "partial map encodes");

    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadTuningBytes(bytes.data(), bytes.size()), "partial map loads");
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    const auto mapped = expectedTuned(processor, 60);
    expect(expectedTuned(processor, 61) == 0, "key 61 is unmapped");
    uint16_t held = 0;
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        held = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    expect(held == mapped, "mapped key attacks at its SWT1 pitch");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(90)), 0);
        processor.processBlock(buffer, midi);
    }
    expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
           "unmapped Note On does not steal the sounding note");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 61), 0);
        processor.processBlock(buffer, midi);
        expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
               "unmapped Note Off is harmless");
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
    }

    expect(processor.loadBuiltinTuning(1), "19-EDO allocation");
    const auto c = expectedTuned(processor, 60);
    const auto e = expectedTuned(processor, 64);
    uint16_t words[3] {};
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        words[0] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        words[1] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
        processor.processBlock(buffer, midi);
        words[2] = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
    }
    expect(words[0] == c && words[1] == e && words[2] == c, "returning to held C restores its tuned pitch");
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(buffer, midi);
    }
}

void testSustainBendAndPortamento()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(1), "19-EDO lifecycle");
    setInt(processor, swaraxt::IDs::perfGlide, 70);
    processor.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer first;
    first.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, first);
    const auto start = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer second;
    second.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, second);
    const auto mid = processor.engineForTests().shruthiPart().voice().pitch_value();
    const double source = 440.0 * std::pow(2.0, (67.0 - 69.0) / 12.0);
    const auto chosen = processor.quantizeActiveHz(source);
    const auto target = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
    expect(start != target, "portamento has two different endpoints");
    expect(mid != start && mid != target, "portamento moves between tuned endpoints");
    std::printf("portamento start=%d mid=%d target=%u\n", start, mid, target);

    setInt(processor, swaraxt::IDs::perfGlide, 0);
    juce::MidiBuffer clear;
    clear.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, clear);
    juce::MidiBuffer fresh;
    fresh.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, fresh);
    int stable = processor.engineForTests().shruthiPart().voice().pitch_value();
    for (int block = 0; block < 6; ++block)
    {
        juce::MidiBuffer idle;
        processor.processBlock(buffer, idle);
        stable = processor.engineForTests().shruthiPart().voice().pitch_value();
    }
    juce::MidiBuffer bend;
    bend.addEvent(juce::MidiMessage::pitchWheel(1, 14000), 0);
    processor.processBlock(buffer, bend);
    const auto bent = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer center;
    center.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 0);
    processor.processBlock(buffer, center);
    int returned = stable;
    for (int block = 0; block < 4; ++block)
    {
        juce::MidiBuffer idle;
        processor.processBlock(buffer, idle);
        returned = processor.engineForTests().shruthiPart().voice().pitch_value();
    }
    expect(returned == stable, "pitch bend release restores the tuned note target");
    std::printf("pitch bend stable=%d bent=%d returned=%d\n", stable, bent, returned);

    juce::MidiBuffer sustain;
    sustain.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    sustain.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
    processor.processBlock(buffer, sustain);
    const auto sustained = processor.engineForTests().shruthiPart().voice().pitch_value();
    juce::MidiBuffer release;
    release.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    processor.processBlock(buffer, release);
    expect(sustained != 0, "sustain keeps a pitched note until the pedal is released");

    juce::MidiBuffer panic;
    panic.addEvent(juce::MidiMessage::allNotesOff(1), 0);
    processor.processBlock(buffer, panic);
}

void testHeldPitchAndSequencerIsolation()
{
    SwaraXtAudioProcessor processor;
    setStepMode(processor);
    setInt(processor, swaraxt::IDs::midiInputTuning, 1);
    expect(processor.loadBuiltinTuning(1), "19-EDO hold");
    processor.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 0);
    processor.processBlock(buffer, on);
    const auto held = processor.engineForTests().shruthiPart().voice().pitch_value();
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(3), "31-EDO while held");
    juce::MidiBuffer idle;
    processor.processBlock(buffer, idle);
    expect(processor.engineForTests().shruthiPart().voice().pitch_value() == held,
           "held notes keep their attack pitch across tuning and mode changes");
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(buffer, off);
}

void testLegacySequenceIgnoresInputTuning()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 256);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 180);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    expect(processor.loadBuiltinTuning(1), "19-EDO must not retune LEGACY");
    processor.sequenceState().setPitchMode(0);
    for (int step = 0; step < 16; ++step)
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 72), 0x70);
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
    snapshot.pitchMode = 0;
    processor.sequenceState().store(snapshot);
    juce::AudioBuffer<float> buffer(2, 256);
    bool sawLegacy = false;
    uint16_t sequenceWord = 0;
    for (int block = 0; block < 24; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        sequenceWord = static_cast<uint16_t>(processor.engineForTests().shruthiPart().voice().pitch_value());
        if (sequenceWord == 72 * 128)
            sawLegacy = true;
    }
    expect(sawLegacy, "LEGACY sequence notes ignore input tuning");
    std::printf("LEGACY under QUANTIZED input word=%u\n", sequenceWord);
}

void testStateDefaultsOff()
{
    SwaraXtAudioProcessor processor;
    setInt(processor, swaraxt::IDs::midiInputTuning, 2);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    auto xml = juce::parseXML(juce::String::fromUTF8(static_cast<const char*>(state.getData()) + 8,
                                                     static_cast<int>(state.getSize()) - 9));
    expect(xml != nullptr, "state xml parses");
    if (xml == nullptr)
        return;
    auto tree = juce::ValueTree::fromXml(*xml);
    for (int index = tree.getNumChildren(); --index >= 0;)
    {
        const auto child = tree.getChild(index);
        if (child.hasType("PARAM") && child["id"].toString() == swaraxt::IDs::midiInputTuning)
            tree.removeChild(index, nullptr);
    }
    auto strippedXml = tree.createXml();
    juce::MemoryBlock stripped;
    {
        juce::MemoryOutputStream out(stripped, false);
        out.writeInt(0x21324356);
        out.writeInt(0);
        strippedXml->writeTo(out, juce::XmlElement::TextFormat().singleLine());
        out.writeByte(0);
    }
    static_cast<uint32_t*>(stripped.getData())[1] = juce::ByteOrder::swapIfBigEndian(
        static_cast<uint32_t>(stripped.getSize() - 9));
    processor.setStateInformation(stripped.getData(), static_cast<int>(stripped.getSize()));
    const int restored = static_cast<int>(std::lround(
        processor.getApvts().getRawParameterValue(swaraxt::IDs::midiInputTuning)->load()));
    expect(restored == 0, "missing input-tuning state restores OFF");
}

void testLayout()
{
    SwaraXtAudioProcessor processor;
    SwaraXtAudioProcessorEditor editor(processor);
    for (auto size : { swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large })
    {
        editor.setGuiSizeForTests(size);
        editor.resized();
        auto& panel = editor.mainPanelForTests();
        panel.reflow();
        auto* combo = &panel.inputTuneBoxForTests();
        auto* selector = combo->getParentComponent();
        const auto bounds = panel.getLocalArea(selector, selector->getLocalBounds());
        expect(panel.getLocalBounds().contains(bounds), "INPUT TUNE stays inside the editor");
        expect(bounds.getWidth() > 20 && bounds.getHeight() > 20, "INPUT TUNE has a visible hit area");
    }
    const float previous = juce::Desktop::getInstance().getGlobalScaleFactor();
    juce::Desktop::getInstance().setGlobalScaleFactor(1.5f);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    editor.resized();
    editor.mainPanelForTests().reflow();
    auto* combo = &editor.mainPanelForTests().inputTuneBoxForTests();
    const auto hidpi = editor.mainPanelForTests().getLocalArea(
        combo->getParentComponent(), combo->getParentComponent()->getLocalBounds());
    expect(editor.mainPanelForTests().getLocalBounds().contains(hidpi), "HiDPI INPUT TUNE stays inside");
    juce::Desktop::getInstance().setGlobalScaleFactor(previous);
    expect(editor.mainPanelForTests().inputTuneBoxForTests().getNumItems() == 3,
           "INPUT TUNE has OFF, TUNED and QUANTIZED");
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testOffIsOriginalTune();
    testTunedMapping();
    testQuantizedInput();
    testUnmappedAndAllocation();
    testSustainBendAndPortamento();
    testHeldPitchAndSequencerIsolation();
    testLegacySequenceIgnoresInputTuning();
    testStateDefaultsOff();
    testLayout();
    if (failures == 0)
    {
        std::printf("midi input tuning tests: PASSED\n");
        return 0;
    }
    std::printf("midi input tuning tests: FAILED (%d)\n", failures);
    return 1;
}
