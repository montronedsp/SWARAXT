// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "Engine/FreeCvPitch.h"
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

void testFreeCvDoesNotRewriteLegacyNotes()
{
    expect(swaraxt::FreeCvPitch::toVoicePitch(60 * swaraxt::FreeCvPitch::kUnitsPerSemitone)
               == 60 * 128,
           "an integer semitone uses the native 128-unit pitch word");
    const auto centish = swaraxt::FreeCvPitch::toVoicePitch(
        60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 10);
    expect(centish != 60 * 128, "about one cent moves the voice pitch word");
    expect(centish != 61 * 128, "about one cent is not a whole semitone");
    expect(swaraxt::FreeCvPitch::toVoicePitch(0) == 128,
           "a zero CV target does not become the muted pitch sentinel");

    swaraxt::SequenceState state;
    const auto packed = state.snapshot().steps[0];
    state.setPitchMode(1);
    auto linear = state.snapshot();
    expect(linear.pitchMode == 1, "linear mode is stored");
    expect(linear.freePitchReady == 1, "first enable initialises CV");
    expect(linear.steps[0] == packed, "enabling linear keeps the packed note");
    expect(linear.freePitch[0] == swaraxt::FreeCvPitch::fromMidiNote(
               swaraxt::SequenceSnapshot::dataA(packed) & 0x7f),
           "first enable copies the integer note into CV");
    const int32_t edited = 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512;
    state.setFreePitch(0, edited);
    state.setPitchMode(0);
    auto legacy = state.snapshot();
    expect(legacy.pitchMode == 0, "linear can be switched off");
    expect(legacy.steps[0] == packed, "switching off keeps the packed note");
    expect(legacy.freePitch[0] == edited, "switching off keeps the CV value");
    state.setPitchMode(1);
    expect(state.snapshot().freePitch[0] == edited, "switching on again restores CV");

    juce::ValueTree oldTree("SEQUENCE");
    oldTree.setProperty("length", 16, nullptr);
    oldTree.setProperty("rotation", 0, nullptr);
    oldTree.setProperty("grooveTemplate", 0, nullptr);
    oldTree.setProperty("arpPattern", 1, nullptr);
    juce::ValueTree step("STEP");
    step.setProperty("index", 0, nullptr);
    step.setProperty("dataA", 0xb0, nullptr);
    step.setProperty("dataB", 0x70, nullptr);
    oldTree.addChild(step, -1, nullptr);
    swaraxt::SequenceState loaded;
    loaded.setPitchMode(1);
    expect(loaded.restoreFromValueTree(oldTree), "an old sequence tree still loads");
    expect(loaded.snapshot().pitchMode == 0, "a tree without pitchMode stays legacy");
    expect((swaraxt::SequenceSnapshot::dataA(loaded.snapshot().steps[0]) & 0x7f) == 0x30,
           "an old sequence tree keeps its note byte");

    juce::Random random(1);
    const auto beforeNotes = loaded.snapshot().steps;
    loaded.randomize(swaraxt::SequenceState::Randomize::notes, random);
    expect(loaded.snapshot().steps != beforeNotes, "legacy note random rewrites packed notes");
    loaded.setPitchMode(1);
    const auto heldNotes = loaded.snapshot().steps;
    const auto heldCv = loaded.snapshot().freePitch;
    loaded.randomize(swaraxt::SequenceState::Randomize::notes, random);
    expect(loaded.snapshot().steps == heldNotes, "linear note random does not rewrite packed notes");
    expect(loaded.snapshot().freePitch != heldCv, "linear note random updates CV");
}

bool isPentatonicPitchClass(int midiNote) noexcept
{
    constexpr int scale[5] = { 0, 2, 4, 7, 9 };
    const int degree = ((midiNote % 12) + 12) % 12;
    for (int value : scale)
        if (value == degree)
            return true;
    return false;
}

void testLegacyRandomizationSeedParity()
{
    // Fixed seed + default factory steps must remain bit-exact in LEGACY mode.
    swaraxt::SequenceState first;
    swaraxt::SequenceState second;
    juce::Random a(0xC0FFEEu);
    juce::Random b(0xC0FFEEu);
    const swaraxt::SequenceState::NoteRange range { -12, 12 };
    first.randomize(swaraxt::SequenceState::Randomize::notes, a, range);
    second.randomize(swaraxt::SequenceState::Randomize::notes, b, range);
    expect(first.snapshot().steps == second.snapshot().steps,
           "identical legacy seeds reproduce packed notes");

    const int root = swaraxt::SequenceSnapshot::dataA(
        swaraxt::SequenceState::defaultSnapshot().steps[0]) & 0x7f;
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        const int note = swaraxt::SequenceSnapshot::dataA(
            first.snapshot().steps[static_cast<size_t>(i)]) & 0x7f;
        expect(isPentatonicPitchClass(note - root),
               "legacy random offsets stay on Type 1 pentatonic degrees");
    }

    swaraxt::SequenceState seqA;
    swaraxt::SequenceState seqB;
    juce::Random c(0xC0FFEEu);
    juce::Random d(0xC0FFEEu);
    seqA.randomize(swaraxt::SequenceState::Randomize::sequence, c, range);
    seqB.randomize(swaraxt::SequenceState::Randomize::sequence, d, range);
    expect(seqA.snapshot().steps == seqB.snapshot().steps,
           "identical legacy seeds reproduce RANDOM SEQ");
}

void testFreeCvContinuousRandomization()
{
    constexpr int32_t kRoot =
        60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 358; // 60.35
    swaraxt::SequenceState state;
    auto base = state.snapshot();
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        base.steps[static_cast<size_t>(i)] = swaraxt::SequenceSnapshot::pack(0x80 | 60, 0x70);
        base.freePitch[static_cast<size_t>(i)] = kRoot;
    }
    base.pitchMode = 1;
    base.freePitchReady = 1;
    state.store(base);
    const auto packedBefore = state.snapshot().steps;

    swaraxt::SequenceState::NoteRange range { -12, 12 };
    juce::Random seedA(0xA5A5A5u);
    juce::Random seedB(0xA5A5A5u);
    state.randomize(swaraxt::SequenceState::Randomize::notes, seedA, range);
    auto once = state.snapshot();
    expect(once.steps == packedBefore, "LINEAR RANDOM NOTES preserves packed notes");
    state.store(base);
    state.randomize(swaraxt::SequenceState::Randomize::notes, seedB, range);
    expect(state.snapshot().freePitch == once.freePitch,
           "identical LINEAR seeds reproduce continuous CV");

    const int32_t lo = kRoot + range.lower * swaraxt::FreeCvPitch::kUnitsPerSemitone;
    const int32_t hi = kRoot + range.upper * swaraxt::FreeCvPitch::kUnitsPerSemitone;
    int fractional = 0;
    int nonPentatonic = 0;
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        const int32_t cv = once.freePitch[static_cast<size_t>(i)];
        expect(cv >= lo && cv <= hi, "LINEAR random CV stays inside the selected range");
        expect(cv >= swaraxt::FreeCvPitch::kMinUnits && cv <= swaraxt::FreeCvPitch::kMaxUnits,
               "LINEAR random CV stays inside the supported pitch domain");
        if ((cv % swaraxt::FreeCvPitch::kUnitsPerSemitone) != 0)
            ++fractional;
        const int midiFloor = cv / swaraxt::FreeCvPitch::kUnitsPerSemitone;
        if (! isPentatonicPitchClass(midiFloor))
            ++nonPentatonic;
    }
    expect(fractional > 0, "LINEAR random produces fractional semitone CV values");
    expect(nonPentatonic > 0, "LINEAR random is not restricted to pentatonic pitch classes");

    // Zero-width range freezes every step on the clamped root.
    state.store(base);
    juce::Random zeroSeed(7);
    state.randomize(swaraxt::SequenceState::Randomize::notes, zeroSeed,
                    swaraxt::SequenceState::NoteRange { 0, 0 });
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
        expect(state.snapshot().freePitch[static_cast<size_t>(i)] == kRoot,
               "zero-width LINEAR range keeps the frozen root");

    // Statistical sample: many draws must not collapse onto integers or a 5-note set.
    int sampleFractional = 0;
    int sampleInteger = 0;
    int samplePentatonicClass = 0;
    int sampleNonPentatonicClass = 0;
    constexpr int kPasses = 64;
    for (int pass = 0; pass < kPasses; ++pass)
    {
        state.store(base);
        juce::Random passRandom(0x600D0000u + static_cast<juce::uint32>(pass));
        state.randomize(swaraxt::SequenceState::Randomize::notes, passRandom, range);
        for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
        {
            const int32_t cv = state.snapshot().freePitch[static_cast<size_t>(i)];
            if ((cv % swaraxt::FreeCvPitch::kUnitsPerSemitone) == 0)
                ++sampleInteger;
            else
                ++sampleFractional;
            const int midiFloor = cv / swaraxt::FreeCvPitch::kUnitsPerSemitone;
            if (isPentatonicPitchClass(midiFloor))
                ++samplePentatonicClass;
            else
                ++sampleNonPentatonicClass;
        }
    }
    const int total = kPasses * swaraxt::SequenceSnapshot::kNumSteps;
    expect(sampleFractional > total / 2, "most LINEAR samples are fractional, not integer semitones");
    expect(sampleInteger < total / 10, "LINEAR samples are not concentrated on integer semitones");
    expect(sampleNonPentatonicClass > total / 10,
           "LINEAR samples are not concentrated on the Type 1 pentatonic set");
    expect(samplePentatonicClass > 0 && sampleNonPentatonicClass > 0,
           "LINEAR samples span both pentatonic and non-pentatonic pitch classes");
    std::printf("LINEAR random sample: total=%d fractional=%d integer=%d pent=%d nonpent=%d\n",
                total, sampleFractional, sampleInteger, samplePentatonicClass,
                sampleNonPentatonicClass);

    // RANDOM SEQ: CV changes, events/velocity/value may change, packed notes stay.
    state.store(base);
    const auto beforeSeq = state.snapshot();
    juce::Random seqRandom(0x51ECu);
    state.randomize(swaraxt::SequenceState::Randomize::sequence, seqRandom, range);
    auto afterSeq = state.snapshot();
    for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
    {
        expect((swaraxt::SequenceSnapshot::dataA(afterSeq.steps[static_cast<size_t>(i)]) & 0x7f)
                   == (swaraxt::SequenceSnapshot::dataA(beforeSeq.steps[static_cast<size_t>(i)]) & 0x7f),
               "LINEAR RANDOM SEQ preserves packed note nibbles");
    }
    expect(afterSeq.freePitch != beforeSeq.freePitch, "LINEAR RANDOM SEQ updates CV");

    // Velocity-only must not touch CV.
    state.store(afterSeq);
    const auto heldCv = state.snapshot().freePitch;
    juce::Random velRandom(99);
    state.randomize(swaraxt::SequenceState::Randomize::velocity, velRandom, range);
    expect(state.snapshot().freePitch == heldCv, "LINEAR RANDOM VELOCITY leaves CV unchanged");

    // LINEAR off keeps packed notes and leaves CV values intact for later re-entry.
    state.store(afterSeq);
    state.setPitchMode(0);
    expect(state.snapshot().pitchMode == 0, "LINEAR off clears pitchMode");
    expect(state.snapshot().steps == afterSeq.steps,
           "switching LINEAR off keeps the packed note pattern");
    expect(state.snapshot().freePitch == afterSeq.freePitch,
           "switching LINEAR off keeps the generated CV values");

    // Save/restore preserves generated CV.
    state.setPitchMode(1);
    juce::Random saveRandom(0x5AFEu);
    state.store(base);
    state.randomize(swaraxt::SequenceState::Randomize::notes, saveRandom, range);
    const auto savedCv = state.snapshot().freePitch;
    const auto savedTree = state.toValueTree();
    swaraxt::SequenceState restored;
    expect(restored.restoreFromValueTree(savedTree), "LINEAR CV tree restores");
    expect(restored.snapshot().pitchMode == 1, "restored pitchMode stays LINEAR");
    expect(restored.snapshot().freePitch == savedCv, "restored freePitch matches generated CV");

    // Session restore with LOCK set preserves generated LINEAR CV.
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    processor.sequenceState().store(state.snapshot());
    processor.setSequenceLocked(true);
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    SwaraXtAudioProcessor other;
    other.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
    expect(other.sequenceLocked(), "restored session keeps LOCK");
    expect(other.sequenceState().snapshot().freePitch == savedCv,
           "sequence LOCK / session restore preserves generated LINEAR CV");
}

void testFreeCvReachesVoicePitch()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 240);
    const int32_t units = 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512;
    processor.sequenceState().setPitchMode(1);
    for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
    {
        const auto packed = processor.sequenceState().snapshot().steps[static_cast<size_t>(step)];
        const auto dataA = static_cast<uint8_t>(swaraxt::SequenceSnapshot::dataA(packed) | 0x80);
        const auto dataB = static_cast<uint8_t>(swaraxt::SequenceSnapshot::dataB(packed) & 0x7f);
        processor.sequenceState().setStep(step, dataA, dataB);
        processor.sequenceState().setFreePitch(step, units);
    }
    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Start(false);
    const auto target = swaraxt::FreeCvPitch::toVoicePitch(units);
    bool heard = false;
    for (int tick = 0; tick < 128 && ! heard; ++tick)
    {
        part.Clock(false);
        if (part.voice().pitch_value() == target)
            heard = true;
    }
    expect(heard, "sequence free CV reaches the oscillator pitch word");
}

bool writePng(const juce::Image& image, const juce::File& file)
{
    if (! file.getParentDirectory().createDirectory())
        return false;
    juce::FileOutputStream stream(file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream(image, stream);
}

bool controlsWithinParent(const juce::Component& parent)
{
    for (int i = 0; i < parent.getNumChildComponents(); ++i)
    {
        const auto* child = parent.getChildComponent(i);
        if (child == nullptr || ! child->isVisible())
            continue;
        if (child->getBounds().isEmpty())
            return false;
        if (! parent.getLocalBounds().contains(child->getBounds()))
            return false;
    }
    return true;
}

void setNormalized(SwaraXtAudioProcessor& processor, const char* id, float normalized)
{
    if (auto* parameter = processor.getApvts().getParameter(id))
        parameter->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, normalized));
}

void prepareSimpleSawPatch(SwaraXtAudioProcessor& processor)
{
    setInt(processor, swaraxt::IDs::osc1Shape, 1); // SAW
    setInt(processor, swaraxt::IDs::osc2Shape, 0); // NONE
    setInt(processor, swaraxt::IDs::mixBalance, 0);
    setInt(processor, swaraxt::IDs::mixSub, 0);
    setInt(processor, swaraxt::IDs::mixNoise, 0);
    setInt(processor, swaraxt::IDs::env1Attack, 0);
    setInt(processor, swaraxt::IDs::env1Decay, 0);
    setInt(processor, swaraxt::IDs::env1Sustain, 127);
    setInt(processor, swaraxt::IDs::env1Release, 0);
    setInt(processor, swaraxt::IDs::env2Attack, 0);
    setInt(processor, swaraxt::IDs::env2Decay, 0);
    setInt(processor, swaraxt::IDs::env2Sustain, 127);
    setInt(processor, swaraxt::IDs::env2Release, 0);
    setNormalized(processor, swaraxt::IDs::filterCutoff, 1.0f);
    setNormalized(processor, swaraxt::IDs::filterResonance, 0.0f);
    setInt(processor, swaraxt::IDs::filterEnvDepth, 0);
    setInt(processor, swaraxt::IDs::filterLfoDepth, 0);
    setInt(processor, swaraxt::IDs::dspFxProgram, 0);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::perfLegato, 0);
    setNormalized(processor, swaraxt::IDs::master, 0.75f);
}

std::vector<float> renderSustainedFreeCvAudio(SwaraXtAudioProcessor& processor,
                                              int32_t units,
                                              bool linearMode,
                                              double sampleRate,
                                              int blockSize,
                                              int totalSamples,
                                              int warmupSamples)
{
    processor.prepareToPlay(sampleRate, blockSize);
    prepareSimpleSawPatch(processor);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 40);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::perfLegato, 0);

    processor.sequenceState().setPitchMode(linearMode ? 1 : 0);
    for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
    {
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        processor.sequenceState().setFreePitch(step, units);
    }
    auto snap = processor.sequenceState().snapshot();
    snap.length = 1;
    snap.rotation = 0;
    processor.sequenceState().store(snap);
    processor.sequenceState().setPitchMode(linearMode ? 1 : 0);
    for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
    {
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        processor.sequenceState().setFreePitch(step, units);
    }

    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Start(false);
    const auto target = linearMode ? swaraxt::FreeCvPitch::toVoicePitch(units)
                                   : static_cast<uint16_t>(60 * 128);
    bool armed = false;
    for (int tick = 0; tick < 64 && ! armed; ++tick)
    {
        part.Clock(false);
        if (part.voice().pitch_value() == target)
            armed = true;
    }
    expect(armed, "sequencer armed the requested pitch word before audio capture");

    // Hold the first step without further Clock so the gate stays open.
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(totalSamples));
    float peak = 0.0f;
    int generated = 0;
    while (generated < totalSamples)
    {
        const int n = std::min(blockSize, totalSamples - generated);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (int i = 0; i < n; ++i)
        {
            const float sample = 0.5f * (buffer.getSample(0, i) + buffer.getSample(1, i));
            mono.push_back(sample);
            peak = std::max(peak, std::abs(sample));
        }
        generated += n;
    }
    expect(part.voice().pitch_value() == target,
           "held step keeps a stable pitch word while rendering");
    expect(peak > 1.0e-3f, "sustained render produces audible energy");
    part.Stop(false);
    if (warmupSamples > 0 && warmupSamples < static_cast<int>(mono.size()))
        mono.erase(mono.begin(), mono.begin() + warmupSamples);
    return mono;
}

double estimateFundamentalHz(const std::vector<float>& mono, double sampleRate)
{
    if (mono.size() < 2048)
        return 0.0;
    const int n = static_cast<int>(mono.size());
    double mean = 0.0;
    for (float s : mono)
        mean += s;
    mean /= static_cast<double>(n);

    const int minLag = static_cast<int>(sampleRate / 900.0);
    const int maxLag = static_cast<int>(sampleRate / 80.0);
    double bestCorr = -1.0e300;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double corr = 0.0;
        double energy0 = 0.0;
        double energy1 = 0.0;
        const int count = n - lag;
        for (int i = 0; i < count; ++i)
        {
            const double a = mono[static_cast<size_t>(i)] - mean;
            const double b = mono[static_cast<size_t>(i + lag)] - mean;
            corr += a * b;
            energy0 += a * a;
            energy1 += b * b;
        }
        const double denom = std::sqrt(energy0 * energy1);
        if (denom <= 1.0e-20)
            continue;
        corr /= denom;
        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestLag = lag;
        }
    }
    if (bestLag <= 0 || bestCorr < 0.2)
        return 0.0;
    return sampleRate / static_cast<double>(bestLag);
}

void writeWavMono(const juce::File& file, const std::vector<float>& mono, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat format;
    auto stream = std::unique_ptr<juce::OutputStream>(file.createOutputStream());
    expect(stream != nullptr, "WAV output stream");
    if (stream == nullptr)
        return;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate(sampleRate)
                             .withNumChannels(1)
                             .withBitsPerSample(16);
    auto writer = format.createWriterFor(stream, options);
    expect(writer != nullptr, "WAV writer");
    if (writer == nullptr)
        return;
    juce::AudioBuffer<float> buffer(1, static_cast<int>(mono.size()));
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample(0, i, mono[static_cast<size_t>(i)]);
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

void testLinearGuiVerification()
{
    const juce::File outDir(
        "D:/MontroneDSP_Studio/research/notes/SWARAXT/linear-final-verify/screenshots");
    expect(outDir.createDirectory().wasOk() || outDir.isDirectory(),
           "LINEAR screenshot directory outside Git");

    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 64);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    processor.sequenceState().setPitchMode(0);
    for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
    {
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        processor.sequenceState().setFreePitch(
            step, 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512);
    }

    SwaraXtAudioProcessorEditor editor(processor);
    editor.setModuleViewsForTests(false, true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);

    auto& seq = editor.mainPanelForTests().sequencerForTests();
    const auto sizes = { swaraxt::ui::GuiSize::small,
                         swaraxt::ui::GuiSize::medium,
                         swaraxt::ui::GuiSize::large };
    const char* sizeNames[] = { "small", "medium", "large" };
    int sizeIndex = 0;
    for (auto size : sizes)
    {
        editor.setGuiSizeForTests(size);
        editor.resized();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        seq.reflow();
        seq.pitchModeBoxForTests().setSelectedId(1, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        expect(processor.sequenceState().snapshot().pitchMode == 0, "LEGACY selection clears pitchMode");
        expect(seq.noteLabelForTests() == "NOTE", "NOTE label while LEGACY is selected");
        expect(seq.pitchModeBoxForTests().getSelectedId() == 1, "LEGACY visual state");

        const auto heading = seq.sequenceHeadingForTests().getBounds();
        const auto pitch = seq.pitchModeBoxForTests().getBounds();
        const auto lock = seq.lockButtonForTests().getBounds();
        expect(heading.getRight() <= pitch.getX() + 2,
               "pitch mode sits to the right of SEQUENCER heading");
        expect(pitch.getRight() <= lock.getX() + 2,
               "pitch mode sits to the left of LOCK");
        expect(std::abs(pitch.getCentreY() - lock.getCentreY()) <= 2,
               "pitch mode and LOCK share the heading row");
        expect(controlsWithinParent(seq), "SeqPanel children stay inside bounds");
        for (int i = 0; i < swaraxt::SequenceSnapshot::kNumSteps; ++i)
        {
            expect(seq.stepButtonForTests(i).isVisible(), "all 16 step buttons visible");
            expect(! seq.stepButtonForTests(i).getBounds().isEmpty(), "step button usable");
        }

        const auto offImage = editor.createComponentSnapshot(editor.getLocalBounds());
        expect(writePng(offImage, outDir.getChildFile(
                   juce::String("linear-off-") + sizeNames[sizeIndex] + ".png")),
               "LINEAR OFF screenshot written");

        seq.pitchModeBoxForTests().setSelectedId(2, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        expect(processor.sequenceState().snapshot().pitchMode == 1, "LINEAR selection sets pitchMode");
        expect(seq.noteLabelForTests() == "CV", "NOTE changes to CV when LINEAR is active");
        expect(seq.pitchModeBoxForTests().getSelectedId() == 2, "LINEAR visual state");
        const auto cvText = seq.noteSliderForTests().getTextFromValue(
            seq.noteSliderForTests().getValue());
        expect(cvText.contains("."), "fractional CV text shows a decimal");
        expect(controlsWithinParent(seq), "SeqPanel children stay inside bounds with LINEAR on");

        const auto onImage = editor.createComponentSnapshot(editor.getLocalBounds());
        expect(writePng(onImage, outDir.getChildFile(
                   juce::String("linear-on-") + sizeNames[sizeIndex] + ".png")),
               "LINEAR ON screenshot written");

        const int32_t heldNote = processor.sequenceState().snapshot().steps[0];
        const int32_t heldCv = processor.sequenceState().snapshot().freePitch[0];
        seq.pitchModeBoxForTests().setSelectedId(1, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        expect(processor.sequenceState().snapshot().steps[0] == heldNote,
               "mode switch preserves legacy packed notes");
        expect(processor.sequenceState().snapshot().freePitch[0] == heldCv,
               "mode switch preserves CV values");
        seq.pitchModeBoxForTests().setSelectedId(2, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        expect(processor.sequenceState().snapshot().freePitch[0] == heldCv,
               "re-enabling LINEAR restores the same CV");
        ++sizeIndex;
    }

    // HiDPI: recreate at medium with 1.5 desktop scale and capture again.
    {
        const float previous = juce::Desktop::getInstance().getGlobalScaleFactor();
        juce::Desktop::getInstance().setGlobalScaleFactor(1.5f);
        SwaraXtAudioProcessorEditor hidpi(processor);
        hidpi.setModuleViewsForTests(false, true);
        hidpi.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
        hidpi.resized();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
        auto& hidpiSeq = hidpi.mainPanelForTests().sequencerForTests();
        hidpiSeq.pitchModeBoxForTests().setSelectedId(2, juce::sendNotificationSync);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        expect(hidpiSeq.noteLabelForTests() == "CV", "HiDPI LINEAR ON shows CV");
        expect(controlsWithinParent(hidpiSeq), "HiDPI SeqPanel children inside bounds");
        const auto image = hidpi.createComponentSnapshot(hidpi.getLocalBounds());
        expect(writePng(image, outDir.getChildFile("linear-on-medium-hidpi-1p5.png")),
               "HiDPI LINEAR screenshot written");
        juce::Desktop::getInstance().setGlobalScaleFactor(previous);
    }

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(restored.sequenceState().snapshot().pitchMode == 1,
           "saved LINEAR state restores pitchMode");
    expect(restored.sequenceState().snapshot().freePitch[0]
               == 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512,
           "saved LINEAR state restores CV");
    SwaraXtAudioProcessorEditor restoredEditor(restored);
    restoredEditor.setModuleViewsForTests(false, true);
    restoredEditor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    restoredEditor.resized();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
    auto& restoredSeq = restoredEditor.mainPanelForTests().sequencerForTests();
    expect(restoredSeq.pitchModeBoxForTests().getSelectedId() == 2,
           "reopened editor shows LINEAR");
    expect(restoredSeq.noteLabelForTests() == "CV", "reopened editor shows CV label");
    const auto restoreImage =
        restoredEditor.createComponentSnapshot(restoredEditor.getLocalBounds());
    expect(writePng(restoreImage, outDir.getChildFile("linear-on-medium-restored.png")),
           "restored LINEAR screenshot written");
    std::printf("LINEAR GUI screenshots written under %s\n",
                outDir.getFullPathName().toRawUTF8());
}

void testFreeCvMeasuredPitchAudio()
{
    const juce::File audioDir(
        "D:/MontroneDSP_Studio/research/notes/SWARAXT/linear-final-verify/audio");
    expect(audioDir.createDirectory().wasOk() || audioDir.isDirectory(),
           "LINEAR audio evidence directory outside Git");

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 64;
    constexpr int totalSamples = 48000; // 1 s
    constexpr int warmup = 12000;

    struct Case {
        const char* name;
        int32_t units;
    };
    const Case cases[] = {
        { "60.00", 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone },
        { "60.50", 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512 },
        { "61.00", 61 * swaraxt::FreeCvPitch::kUnitsPerSemitone },
    };
    double frequencies[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        SwaraXtAudioProcessor processor;
        const auto voicePitch = swaraxt::FreeCvPitch::toVoicePitch(cases[i].units);
        auto mono = renderSustainedFreeCvAudio(
            processor, cases[i].units, true, sampleRate, blockSize, totalSamples, warmup);
        frequencies[i] = estimateFundamentalHz(mono, sampleRate);
        writeWavMono(audioDir.getChildFile(juce::String("free-cv-") + cases[i].name + ".wav"),
                     mono, sampleRate);
        std::printf("FREE CV %s fundamental ~= %.6f Hz (voice word %u)\n",
                    cases[i].name,
                    frequencies[i],
                    static_cast<unsigned>(voicePitch));
        expect(frequencies[i] > 100.0 && frequencies[i] < 600.0,
               "measured fundamental is in a plausible pitched range");
    }

    const double ratioHalf = frequencies[1] / frequencies[0];
    const double ratioWhole = frequencies[2] / frequencies[0];
    const double expectedHalf = std::pow(2.0, 0.5 / 12.0);
    const double expectedWhole = std::pow(2.0, 1.0 / 12.0);
    const double errHalf = std::abs(ratioHalf - expectedHalf) / expectedHalf;
    const double errWhole = std::abs(ratioWhole - expectedWhole) / expectedWhole;
    std::printf("Ratio 60.50/60.00 = %.8f (expected %.8f, rel err %.6f)\n",
                ratioHalf, expectedHalf, errHalf);
    std::printf("Ratio 61.00/60.00 = %.8f (expected %.8f, rel err %.6f)\n",
                ratioWhole, expectedWhole, errWhole);
    // Pitch-word path is exact for 0.5 and 1.0 semitone; allow oscillator/
    // autocorrelation resolution (~0.5%%) without retuning.
    expect(errHalf < 0.005, "60.50/60.00 frequency ratio within pitch-word tolerance");
    expect(errWhole < 0.005, "61.00/60.00 frequency ratio within pitch-word tolerance");

    // Fractional step change: adjacent gated steps 60.00 then 60.50.
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        prepareSimpleSawPatch(processor);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 240);
        setInt(processor, swaraxt::IDs::seqGate, 100);
        processor.sequenceState().setPitchMode(1);
        auto snap = processor.sequenceState().snapshot();
        snap.length = 2;
        snap.rotation = 0;
        processor.sequenceState().store(snap);
        processor.sequenceState().setPitchMode(1);
        processor.sequenceState().setStep(0, 0x80 | 60, 0x70);
        processor.sequenceState().setStep(1, 0x80 | 60, 0x70);
        processor.sequenceState().setFreePitch(0, cases[0].units);
        processor.sequenceState().setFreePitch(1, cases[1].units);
        processor.engineForTests().applyParameters();
        auto& part = processor.engineForTests().shruthiPart();
        part.Start(false);
        const auto word0 = swaraxt::FreeCvPitch::toVoicePitch(cases[0].units);
        const auto word1 = swaraxt::FreeCvPitch::toVoicePitch(cases[1].units);
        bool saw0 = false;
        bool saw1 = false;
        for (int tick = 0; tick < 256; ++tick)
        {
            part.Clock(false);
            juce::AudioBuffer<float> buffer(2, blockSize);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            const auto pitch = part.voice().pitch_value();
            if (pitch == word0)
                saw0 = true;
            if (pitch == word1)
                saw1 = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < blockSize; ++s)
                    expect(std::isfinite(buffer.getSample(ch, s)),
                           "fractional step change stays finite");
        }
        expect(saw0 && saw1, "fractional pitch change between consecutive steps");
        part.Stop(false);
    }

    // Portamento transition between free-CV steps.
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        prepareSimpleSawPatch(processor);
        setInt(processor, swaraxt::IDs::perfGlide, 40);
        setInt(processor, swaraxt::IDs::perfLegato, 1);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 180);
        setInt(processor, swaraxt::IDs::seqGate, 100);
        auto snap = processor.sequenceState().snapshot();
        snap.length = 2;
        processor.sequenceState().store(snap);
        processor.sequenceState().setPitchMode(1);
        processor.sequenceState().setStep(0, 0x80 | 60, 0x70);
        processor.sequenceState().setStep(1, 0x80 | 61, 0xf0); // tie/legato + vel
        processor.sequenceState().setFreePitch(0, cases[0].units);
        processor.sequenceState().setFreePitch(1, cases[2].units);
        processor.engineForTests().applyParameters();
        auto& part = processor.engineForTests().shruthiPart();
        part.Start(false);
        const auto word0 = swaraxt::FreeCvPitch::toVoicePitch(cases[0].units);
        const auto word1 = swaraxt::FreeCvPitch::toVoicePitch(cases[2].units);
        bool sawStart = false;
        bool sawBetween = false;
        bool sawEnd = false;
        for (int tick = 0; tick < 400; ++tick)
        {
            part.Clock(false);
            juce::AudioBuffer<float> buffer(2, blockSize);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            const auto pitch = part.voice().pitch_value();
            if (pitch == word0)
                sawStart = true;
            if (pitch > word0 && pitch < word1)
                sawBetween = true;
            if (pitch == word1)
                sawEnd = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < blockSize; ++s)
                    expect(std::isfinite(buffer.getSample(ch, s)),
                           "portamento free-CV stays finite");
        }
        expect(sawStart, "portamento starts at first free-CV pitch");
        expect(sawBetween || sawEnd, "portamento moves toward the second free-CV pitch");
        expect(sawEnd || sawBetween, "portamento does not stick silently");
        part.Stop(false);
        std::printf("Portamento free-CV: start=%d between=%d end=%d\n",
                    sawStart ? 1 : 0, sawBetween ? 1 : 0, sawEnd ? 1 : 0);
    }

    // Rest followed by a note.
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        prepareSimpleSawPatch(processor);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 240);
        setInt(processor, swaraxt::IDs::seqGate, 100);
        auto snap = processor.sequenceState().snapshot();
        snap.length = 2;
        processor.sequenceState().store(snap);
        processor.sequenceState().setPitchMode(1);
        processor.sequenceState().setStep(0, 60, 0x00); // rest
        processor.sequenceState().setStep(1, 0x80 | 60, 0x70);
        processor.sequenceState().setFreePitch(0, cases[0].units);
        processor.sequenceState().setFreePitch(1, cases[0].units);
        processor.engineForTests().applyParameters();
        auto& part = processor.engineForTests().shruthiPart();
        part.Start(false);
        bool heardAfterRest = false;
        float peakDuringRestWindow = 0.0f;
        float peakAfterNote = 0.0f;
        for (int tick = 0; tick < 128; ++tick)
        {
            part.Clock(false);
            juce::AudioBuffer<float> buffer(2, blockSize);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            float peak = 0.0f;
            for (int s = 0; s < blockSize; ++s)
                peak = std::max(peak, std::abs(buffer.getSample(0, s)));
            if (tick < 8)
                peakDuringRestWindow = std::max(peakDuringRestWindow, peak);
            if (part.voice().pitch_value()
                    == swaraxt::FreeCvPitch::toVoicePitch(cases[0].units)
                && peak > 1.0e-4f)
            {
                heardAfterRest = true;
                peakAfterNote = std::max(peakAfterNote, peak);
            }
        }
        expect(heardAfterRest, "rest followed by free-CV note produces audio");
        expect(peakAfterNote > peakDuringRestWindow,
               "note after rest is louder than the rest window");
        part.Stop(false);
    }

    // LINEAR OFF restores exact legacy integer pitch word through the sequencer.
    {
        SwaraXtAudioProcessor processor;
        processor.prepareToPlay(sampleRate, blockSize);
        prepareSimpleSawPatch(processor);
        setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
        setInt(processor, swaraxt::IDs::seqClockMode, 0);
        setInt(processor, swaraxt::IDs::seqTempo, 240);
        setInt(processor, swaraxt::IDs::seqGate, 100);
        processor.sequenceState().setPitchMode(0);
        for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
        {
            processor.sequenceState().setStep(step, 0x80 | 60, 0x70);
            processor.sequenceState().setFreePitch(
                step, 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone + 512);
        }
        auto snap = processor.sequenceState().snapshot();
        snap.length = 1;
        snap.rotation = 0;
        processor.sequenceState().store(snap);
        processor.sequenceState().setPitchMode(0);
        for (int step = 0; step < swaraxt::SequenceSnapshot::kNumSteps; ++step)
            processor.sequenceState().setStep(step, 0x80 | 60, 0x70);
        processor.engineForTests().applyParameters();
        auto& part = processor.engineForTests().shruthiPart();
        part.Start(false);
        bool heardLegacy = false;
        for (int tick = 0; tick < 128 && ! heardLegacy; ++tick)
        {
            part.Clock(false);
            juce::AudioBuffer<float> buffer(2, blockSize);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            if (part.voice().pitch_value() == 60 * 128)
                heardLegacy = true;
        }
        expect(heardLegacy, "LINEAR OFF restores exact legacy sequencer pitch word");
        part.Stop(false);

        SwaraXtAudioProcessor legacyAudio;
        auto mono = renderSustainedFreeCvAudio(
            legacyAudio,
            60 * swaraxt::FreeCvPitch::kUnitsPerSemitone,
            false,
            sampleRate,
            blockSize,
            totalSamples,
            warmup);
        const double legacyHz = estimateFundamentalHz(mono, sampleRate);
        writeWavMono(audioDir.getChildFile("legacy-off-60.wav"), mono, sampleRate);
        const double legacyErr = std::abs(legacyHz - frequencies[0]) / frequencies[0];
        std::printf("Legacy OFF 60 Hz ~= %.6f (FREE 60.00 ~= %.6f, rel err %.6f)\n",
                    legacyHz, frequencies[0], legacyErr);
        expect(legacyErr < 0.005, "LINEAR OFF matches FREE integer 60.00 fundamental");
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testFreeCvDoesNotRewriteLegacyNotes();
    testLegacyRandomizationSeedParity();
    testFreeCvContinuousRandomization();
    testFreeCvReachesVoicePitch();
    testLinearGuiVerification();
    testFreeCvMeasuredPitchAudio();
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
