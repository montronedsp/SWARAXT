// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"
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
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, id);
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

int getInt(const SwaraXtAudioProcessor& processor, const char* id)
{
    if (const auto* value = processor.getApvts().getRawParameterValue(id))
        return static_cast<int>(std::lround(value->load()));
    return -1;
}

swaraxt::SequenceSnapshot makeSequence(int length, int rotation, int offset)
{
    auto sequence = swaraxt::SequenceState::defaultSnapshot();
    sequence.length = static_cast<uint8_t>(length);
    sequence.rotation = static_cast<uint8_t>(rotation);
    sequence.grooveTemplate = static_cast<uint8_t>(offset % 6);
    sequence.arpPattern = shruthi::kNumArpeggiatorPatterns;
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        const auto note = static_cast<uint8_t>((36 + offset + i) & 0x7f);
        const bool rest = (i % 5) == 4;
        const bool tie = (i % 3) == 1;
        const auto dataA = static_cast<uint8_t>(note | (rest ? 0x00 : 0x80));
        const auto dataB = static_cast<uint8_t>((tie && ! rest ? 0x80 : 0x00)
                                               | ((i & 0x07) << 4)
                                               | ((i + offset) & 0x0f));
        sequence.steps[static_cast<size_t>(i)] =
            swaraxt::SequenceSnapshot::pack(dataA, dataB);
    }
    return sequence;
}

bool sameSequence(const swaraxt::SequenceSnapshot& a,
                  const swaraxt::SequenceSnapshot& b)
{
    return a.length == b.length
        && a.rotation == b.rotation
        && a.grooveTemplate == b.grooveTemplate
        && a.arpPattern == b.arpPattern
        && a.steps == b.steps;
}

void expectEngineSequence(const SwaraXtAudioProcessor& processor,
                          const swaraxt::SequenceSnapshot& expected)
{
    const auto& sequence = processor.engineForTests().shruthiPart().sequencer_settings();
    expect(sequence.pattern_size == expected.length, "engine sequence length mirrors state");
    expect(sequence.pattern_rotation == expected.rotation, "engine sequence rotation mirrors state");
    expect(sequence.seq_groove_template == expected.grooveTemplate,
           "engine groove template mirrors state");
    expect(sequence.arp_pattern == expected.arpPattern,
           "engine receives the complete Shruthi arpeggiator pattern code");
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        const auto packed = expected.steps[static_cast<size_t>(i)];
        expect(sequence.steps[i].data_[0] == swaraxt::SequenceSnapshot::dataA(packed),
               "engine sequence data A is bit-exact");
        expect(sequence.steps[i].data_[1] == swaraxt::SequenceSnapshot::dataB(packed),
               "engine sequence data B is bit-exact");
    }
}

void testStateAndEngineBridge()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 128);
    const auto sequence = makeSequence(5, 3, 0);
    processor.sequenceState().store(sequence);
    processor.engineForTests().applyParameters();
    expect(sameSequence(processor.sequenceState().snapshot(), sequence),
           "all sequence bytes survive the lock-free state model");
    expectEngineSequence(processor, sequence);

    auto& part = processor.engineForTests().shruthiPart();
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setInt(processor, swaraxt::IDs::arpGate, 7);
    processor.engineForTests().applyParameters();
    part.Start(false);
    const int clocksPerStep = part.clock_ticks_per_step();
    for (int logicalStep = 0; logicalStep < sequence.length * 2; ++logicalStep)
    {
        part.Clock(false);
        const int physical = ((logicalStep % sequence.length) + sequence.rotation) & 0x0f;
        const int seq1Physical = physical & 0x07;
        const int seq2Physical = seq1Physical + 8;
        const auto controllerAt = [&](int step) {
            return (swaraxt::SequenceSnapshot::dataB(sequence.steps[static_cast<size_t>(step)]) & 0x0f) << 4;
        };
        expect(part.voice().modulation_source(shruthi::MOD_SRC_SEQ) == controllerAt(physical),
               "Seq follows length, rotation, order, and wrap");
        expect(part.voice().modulation_source(shruthi::MOD_SRC_SEQ_1) == controllerAt(seq1Physical),
               "Seq 1 reads the rotated first eight-step lane");
        expect(part.voice().modulation_source(shruthi::MOD_SRC_SEQ_2) == controllerAt(seq2Physical),
               "Seq 2 reads the rotated second eight-step lane");
        const auto dataA = swaraxt::SequenceSnapshot::dataA(
            sequence.steps[static_cast<size_t>(physical)]);
        const auto dataB = swaraxt::SequenceSnapshot::dataB(
            sequence.steps[static_cast<size_t>(physical)]);
        const bool hasIndependentStep = (dataA & 0x80u) != 0u && (dataB & 0x80u) == 0u;
        expect(part.voice().modulation_source(shruthi::MOD_SRC_STEP)
                   == (hasIndependentStep ? 255 : 0),
               "Step follows the source gate and legato semantics");
        for (int tick = 1; tick < clocksPerStep; ++tick)
            part.Clock(false);
    }
    part.Stop(false);
}

void testFullStepSemanticsAndLfo()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    const auto sequence = makeSequence(16, 0, 0);
    processor.sequenceState().store(sequence);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 1);
    setInt(processor, swaraxt::IDs::lfo1Wave, shruthi::LFO_WAVEFORM_STEP_SEQUENCER);
    setInt(processor, swaraxt::IDs::lfo1Rate, 127);
    setInt(processor, swaraxt::IDs::osc1Option, shruthi::OP_PING_PONG_SEQ);
    processor.engineForTests().applyParameters();

    const auto& engineSequence = processor.engineForTests().shruthiPart().sequencer_settings();
    expect(engineSequence.steps[4].gate() == 0, "rest event reaches Shruthi gate bit");
    expect(engineSequence.steps[1].gate() != 0 && engineSequence.steps[1].legato() != 0,
           "tie event reaches Shruthi gate and legato bits");
    expect(engineSequence.steps[7].velocity() == 0x70,
           "three-bit velocity reaches Shruthi fixed-width field");
    expect(engineSequence.steps[15].controller() == 15,
           "controller step endpoint 15 reaches Shruthi");
    expect(processor.engineForTests().shruthiPart().patch().osc[0].option == shruthi::OP_PING_PONG_SEQ,
           "Ping Seq operator remains selected while sequence state is editable");

    std::set<int> lfoValues;
    for (int block = 0; block < 4096; ++block)
    {
        processor.engineForTests().shruthiPart().ProcessControlBlock();
        lfoValues.insert(processor.engineForTests().shruthiPart().voice()
                             .modulation_source(shruthi::MOD_SRC_LFO_1));
    }
    for (int value = 0; value < 16; ++value)
    {
        const auto raw = static_cast<uint8_t>(value << 4);
        const auto centered = static_cast<int8_t>(static_cast<int8_t>(raw) - 128);
        const int expected = avrlib::S8U8MulShift8(centered, 255) + 128;
        expect(lfoValues.count(expected) != 0,
                "LFO Step Seq renders every editable controller nibble");
    }
}

juce::MemoryBlock saveState(SwaraXtAudioProcessor& processor,
                            const swaraxt::SequenceSnapshot& sequence)
{
    processor.sequenceState().store(sequence);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return state;
}

void restoreState(SwaraXtAudioProcessor& processor, const juce::MemoryBlock& state)
{
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

void testStateRoundtripAndPresetSwitching()
{
    SwaraXtAudioProcessor source;
    const auto a = makeSequence(16, 0, 0);
    const auto b = makeSequence(5, 11, 7);
    const auto stateA = saveState(source, a);
    const auto stateB = saveState(source, b);

    SwaraXtAudioProcessor restored;
    for (const auto* state : { &stateA, &stateB, &stateA, &stateB, &stateA })
    {
        restoreState(restored, *state);
        const auto& expected = state == &stateA ? a : b;
        expect(sameSequence(restored.sequenceState().snapshot(), expected),
               "A/B/A preset switching restores the exact complete sequence");
        restored.prepareToPlay(44100.0, 97);
        restored.engineForTests().applyParameters();
        expectEngineSequence(restored, expected);
    }

    auto legacyXml = juce::AudioProcessor::getXmlFromBinary(
        stateA.getData(), static_cast<int>(stateA.getSize()));
    expect(legacyXml != nullptr, "state XML decodes for legacy compatibility test");
    if (legacyXml != nullptr)
    {
        auto legacy = juce::ValueTree::fromXml(*legacyXml);
        legacy.setProperty("stateVersion", 2, nullptr);
        if (const auto child = legacy.getChildWithName("SEQUENCE"); child.isValid())
            legacy.removeChild(child, nullptr);
        juce::MemoryBlock legacyData;
        if (auto xml = legacy.createXml())
            juce::AudioProcessor::copyXmlToBinary(*xml, legacyData);
        restoreState(restored, legacyData);
        expect(sameSequence(restored.sequenceState().snapshot(),
                            swaraxt::SequenceState::defaultSnapshot()),
               "version 1/2 states receive the product-compatible default sequence");
    }

    restored.sequenceState().store(b);
    SwaraXtAudioProcessor::PresetEntry factoryPreset;
    factoryPreset.isFactory = true;
    factoryPreset.factoryIndex = 1;
    juce::String error;
    expect(restored.loadPresetEntry(factoryPreset, error),
           "explicit factory preset load succeeds after a restored host state");
    expect(sameSequence(restored.sequenceState().snapshot(),
                        swaraxt::SequenceState::defaultSnapshot()),
           "factory program load clears stale sequence state");
}

void testEditorRecreationAndViewIsolation()
{
    SwaraXtAudioProcessor processor;
    const auto sequence = makeSequence(9, 6, 2);
    processor.sequenceState().store(sequence);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(processor, swaraxt::IDs::arpPattern, 4);
    expect(processor.sequenceState().snapshot().arpPattern == 4,
           "existing host automation updates the complete sequence state");

    for (int pass = 0; pass < 2; ++pass)
    {
        std::unique_ptr<juce::AudioProcessorEditor> base(processor.createEditor());
        auto* editor = dynamic_cast<SwaraXtAudioProcessorEditor*>(base.get());
        expect(editor != nullptr, "sequencer editor constructs");
        if (editor != nullptr)
        {
            editor->setModuleViewsForTests(false, true);
            editor->setSequencerEditorViewForTests(true);
            const int visibleMode = getInt(processor, swaraxt::IDs::seqMode);
            expect(visibleMode == shruthi::SEQUENCER_MODE_STEP,
                   "ARP/SEQ editor view does not change runtime mode");
            if (pass == 0)
            {
                editor->setSequenceLayoutForTests(8, 2, 4);
                auto edited = processor.sequenceState().snapshot();
                expect(edited.length == 8 && edited.rotation == 2 && edited.grooveTemplate == 4,
                       "visible length, start, and groove controls update sequence state");
                editor->setSequenceStepForTests(0, 72, 2, 5, 14);
                edited = processor.sequenceState().snapshot();
                const auto packed = edited.steps[0];
                expect(swaraxt::SequenceSnapshot::dataA(packed) == (0x80 | 72)
                           && swaraxt::SequenceSnapshot::dataB(packed) == (0x80 | 0x50 | 14),
                       "visible note, event, velocity, and value controls write exact Shruthi bits");
                editor->setSequenceLayoutForTests(sequence.length, sequence.rotation,
                                                  sequence.grooveTemplate);
                const auto original = sequence.steps[0];
                const int originalEvent = (swaraxt::SequenceSnapshot::dataA(original) & 0x80) == 0
                    ? 0
                    : ((swaraxt::SequenceSnapshot::dataB(original) & 0x80) != 0 ? 2 : 1);
                editor->setSequenceStepForTests(
                    0,
                    swaraxt::SequenceSnapshot::dataA(original) & 0x7f,
                    originalEvent,
                    (swaraxt::SequenceSnapshot::dataB(original) & 0x70) >> 4,
                    swaraxt::SequenceSnapshot::dataB(original) & 0x0f);
            }
            const int visiblePattern = editor->sequencerPatternForTests();
            expect(visiblePattern == (pass == 0 ? 4 : 15),
                   "editor reflects the persisted complete Shruthi pattern code");
            if (pass == 0)
            {
                editor->setSequencerPatternForTests(shruthi::kNumArpeggiatorPatterns);
                const auto afterPattern = processor.sequenceState().snapshot();
                expect(afterPattern.arpPattern
                           == shruthi::kNumArpeggiatorPatterns,
                       "Sequence pattern code 15 is editable through the GUI");
                const int hostPattern = getInt(processor, swaraxt::IDs::arpPattern);
                expect(hostPattern == 4,
                       "extended pattern selection preserves the public parameter domain");
            }
            else
            {
                setInt(processor, swaraxt::IDs::arpPattern, 3);
                expect(editor->sequencerPatternForTests() == 3,
                       "host automation replaces an extended GUI pattern deterministically");
                expect(processor.sequenceState().snapshot().arpPattern == 3,
                       "host automation reaches the complete sequence state while the editor is open");
                editor->setSequencerPatternForTests(shruthi::kNumArpeggiatorPatterns);
            }
            editor->setSequencerEditorViewForTests(false);
            editor->setSequencerEditorViewForTests(true);
        }
        expect(sameSequence(processor.sequenceState().snapshot(), sequence),
               "editor close/reopen does not mutate sequence state");
        base.reset();
    }
}

void testRuntimeModesAndAudioSafety()
{
    for (int mode = shruthi::SEQUENCER_MODE_STEP;
         mode <= shruthi::SEQUENCER_MODE_SEQ;
         ++mode)
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(48000.0, 127);
        processor.sequenceState().store(makeSequence(7, 13, mode));
        setInt(processor, swaraxt::IDs::seqMode, mode);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 174);

        float peak = 0.0f;
        int maxGeneratedNotes = 0;
        for (int block = 0; block < 300; ++block)
        {
            juce::AudioBuffer<float> buffer(2, 127);
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            if (block == 220)
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(buffer, midi);
            maxGeneratedNotes = std::max(
                maxGeneratedNotes,
                static_cast<int>(processor.engineForTests().shruthiPart()
                                     .generated_note_count_for_tests()));
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    const float value = buffer.getSample(channel, sample);
                    expect(std::isfinite(value), "sequencer modes never produce NaN or Inf");
                    peak = std::max(peak, std::abs(value));
                }
        }
        expect(peak < 4.0f, "sequencer modes remain amplitude bounded");
        if (mode != shruthi::SEQUENCER_MODE_STEP)
            expect(maxGeneratedNotes > 0, "ARP and SEQ modes generate scheduled notes");
        processor.engineForTests().shruthiPart().Stop(false);
        expect(processor.engineForTests().shruthiPart().generated_note_count_for_tests() == 0,
               "stopping sequencer releases generated notes");
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testStateAndEngineBridge();
    testFullStepSemanticsAndLfo();
    testStateRoundtripAndPresetSwitching();
    testEditorRecreationAndViewIsolation();
    testRuntimeModesAndAudioSafety();

    if (failures == 0)
    {
        std::printf("Swara XT sequencer tests: PASSED\n");
        return 0;
    }
    std::printf("Swara XT sequencer tests: FAILED (%d)\n", failures);
    return 1;
}
