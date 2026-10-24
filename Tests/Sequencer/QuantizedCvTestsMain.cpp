// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "Engine/FreeCvPitch.h"
#include "Engine/SequencerMidiRoute.h"
#include "Plugin/PluginEditor.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "Tuning/PublishedTuning.h"
#include "Tuning/ScalePitch.h"
#include "Tuning/TuningTable.h"
#include "Ui/SwaraXtPanels.h"
#include "shruthi/sequencer_settings.h"
#include "../Tuning/QuantizerTestSupport.h"

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

struct MidiNote
{
    int offset = 0;
    int note = 0;
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
        notes.push_back({ metadata.samplePosition, message.getNoteNumber(), message.isNoteOn() });
    }
    return notes;
}

double bruteNearestHz(SwaraXtAudioProcessor& processor, double sourceHz, int* degreeOut)
{
    double bestHz = 0.0;
    double bestCents = 1.0e18;
    int bestDegree = 0;
    for (int degree = -700; degree <= 700; ++degree)
    {
        const double hz = processor.hzForActiveDegree(degree);
        if (! (hz > 0.0) || ! (sourceHz > 0.0))
            continue;
        const double cents = std::abs(1200.0 * std::log2(hz / sourceHz));
        const bool closer = cents < bestCents - 1.0e-6;
        const bool tieLower = std::abs(cents - bestCents) <= 1.0e-6 && (bestHz <= 0.0 || hz < bestHz);
        if (closer || tieLower)
        {
            bestCents = cents;
            bestHz = hz;
            bestDegree = degree;
        }
    }
    if (degreeOut != nullptr)
        *degreeOut = bestDegree;
    return bestHz;
}

void expectMatchesBrute(SwaraXtAudioProcessor& processor, double sourceHz, const char* label)
{
    const auto chosen = processor.quantizeActiveHz(sourceHz);
    const double brute = bruteNearestHz(processor, sourceHz, nullptr);
    expect(chosen.valid && brute > 0.0, label);
    if (chosen.valid && brute > 0.0)
        expect(std::abs(1200.0 * std::log2(chosen.hz / brute)) < 0.05, label);
}

void testQuantizerFamilies()
{
    SwaraXtAudioProcessor processor;
    const int tunings[] = { 0, 1, 2, 3, 4, 5 };
    const char* names[] = { "12-EDO", "19-EDO", "24-EDO", "31-EDO", "just", "tritave" };
    const double probes[] = { 220.0, 440.0, 261.625565, 450.0, 30.0, 900.0, 5000.0 };
    for (int index = 0; index < 6; ++index)
    {
        expect(processor.loadBuiltinTuning(tunings[index]), names[index]);
        for (double source : probes)
            expectMatchesBrute(processor, source, names[index]);
        const auto low = processor.quantizeActiveHz(1.0);
        const auto high = processor.quantizeActiveHz(40000.0);
        expect(low.valid && high.valid && low.hz <= high.hz, "extremes stay inside the candidate range");
        const double neg = processor.hzForActiveDegree(-1);
        expect(neg > 0.0, "negative degree remains playable");
        expectMatchesBrute(processor, neg, "negative degree source");
    }

    expect(processor.loadBuiltinTuning(0), "12-EDO tie fixture");
    const double lower = processor.hzForActiveDegree(0);
    const double upper = processor.hzForActiveDegree(1);
    const double halfway = std::sqrt(lower * upper);
    const auto tie = processor.quantizeActiveHz(halfway);
    expect(tie.valid && std::abs(tie.hz - std::min(lower, upper)) / lower < 1.0e-6,
           "exact halfway cents chooses the lower frequency");

    const double edo19 = processor.loadBuiltinTuning(1) ? processor.hzForActiveDegree(1) : 0.0;
    const auto micro = processor.quantizeActiveHz(edo19);
    const int midi = micro.valid ? swaraxt::ScalePitch::nearestMidiNote(micro.hz) : 0;
    const double midiHz = 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
    expect(micro.valid && std::abs(1200.0 * std::log2(micro.hz / edo19)) < 0.05,
           "19-EDO degree is selected by frequency");
    expect(std::abs(1200.0 * std::log2(micro.hz / midiHz)) > 1.0,
           "19-EDO quantization is not a rounded MIDI note");

    swaraxt::tuning::TuningSnapshot snapshot;
    snapshot.formalPeriod = 12;
    snapshot.degreeCount = 2;
    snapshot.referenceNote = 69;
    snapshot.referenceHz = 440.0;
    snapshot.periodCents = 1200.0;
    snapshot.degreeCents = { 700.0, 100.0 };
    snapshot.midiDegree[69] = 0;
    std::snprintf(snapshot.name, sizeof(snapshot.name), "bent");
    swaraxt::tuning::PublishedTuning table;
    expect(table.fillFrom(snapshot), "non-monotonic degree table fills");
    const double nearLow = table.hzForDegree(0) * std::pow(2.0, 100.0 / 1200.0);
    const auto bent = table.quantizeHz(nearLow);
    expect(bent.valid && std::abs(bent.hz - table.hzForDegree(2)) / bent.hz < 1.0e-6,
           "non-monotonic degrees quantize by frequency");
    snapshot.midiDegree[61] = -1;
    expect(table.fillFrom(snapshot), "unmapped key table fills");
    const auto unmapped = table.quantizeHz(table.hzForDegree(2));
    expect(unmapped.valid && unmapped.degree == 2, "unmapped MIDI keys do not remove scale degrees");

    const uint8_t garbage[] = { 1, 2, 3, 4 };
    const auto before = processor.activeTuningName();
    expect(! processor.loadTuningBytes(garbage, sizeof(garbage)), "invalid tuning is rejected");
    expect(processor.activeTuningName() == before, "failed import keeps the previous tuning");
    expect(! processor.quantizeActiveHz(0.0).valid, "non-positive source does not invent a pitch");
}

void testCvPreservationAndRandom()
{
    swaraxt::SequenceState state;
    state.setPitchMode(1);
    state.setFreePitch(0, 60 * 1024 + 370);
    state.setFreePitch(3, 60 * 1024 + 920);
    const auto cv = state.snapshot().freePitch;
    const auto packed = state.snapshot().steps;
    const auto degrees = state.snapshot().scaleDegree;
    state.setPitchMode(3);
    expect(state.snapshot().pitchMode == 3, "QUANTIZED CV mode is stored");
    expect(state.snapshot().freePitch == cv, "entering QUANTIZED CV keeps free CV");
    expect(state.snapshot().steps == packed, "entering QUANTIZED CV keeps packed notes");
    expect(state.snapshot().scaleDegree == degrees, "entering QUANTIZED CV keeps scale degrees");
    state.setPitchMode(1);
    expect(state.snapshot().freePitch == cv, "returning to LINEAR keeps the same CV");
    state.setPitchMode(3);

    juce::Random random(0x0C0Cu);
    const auto before = state.snapshot().freePitch;
    state.randomize(swaraxt::SequenceState::Randomize::notes, random, { -12, 12 });
    expect(state.snapshot().steps == packed, "QUANTIZED CV random keeps packed notes");
    expect(state.snapshot().scaleDegree == degrees, "QUANTIZED CV random keeps degrees");
    expect(state.snapshot().freePitch != before, "QUANTIZED CV random changes CV");
    int fractional = 0;
    for (int step = 0; step < 16; ++step)
        if ((state.snapshot().freePitch[static_cast<size_t>(step)] % 1024) != 0)
            ++fractional;
    expect(fractional > 0, "QUANTIZED CV random keeps fractional CV storage");

    juce::Random again(0x0C0Cu);
    swaraxt::SequenceState second;
    second.setPitchMode(3);
    second.store(state.snapshot());
    auto reset = state.snapshot();
    reset.freePitch = before;
    second.store(reset);
    second.setPitchMode(3);
    second.randomize(swaraxt::SequenceState::Randomize::notes, again, { -12, 12 });
    expect(second.snapshot().freePitch == state.snapshot().freePitch, "QUANTIZED CV random is seeded");

    swaraxt::SequenceState corrupt;
    auto tree = corrupt.toValueTree();
    tree.setProperty("pitchMode", 9, nullptr);
    expect(corrupt.restoreFromValueTree(tree), "corrupt mode restores");
    expect(corrupt.snapshot().pitchMode == 1, "corrupt mode becomes silent LINEAR");
    expect(! swaraxt::sequencerRouteEmitsMidiNotes(
               swaraxt::routeFromStoredPitchMode(corrupt.snapshot().pitchMode)),
           "corrupt mode does not enable MIDI OUT");
}

void armQuantized(SwaraXtAudioProcessor& processor, int32_t units)
{
    processor.sequenceState().setPitchMode(3);
    for (int step = 0; step < 16; ++step)
    {
        processor.sequenceState().setStep(step, static_cast<uint8_t>(0x80 | 60), 0x70);
        processor.sequenceState().setFreePitch(step, units);
    }
    auto snapshot = processor.sequenceState().snapshot();
    snapshot.length = 1;
    snapshot.pitchMode = 3;
    processor.sequenceState().store(snapshot);
    processor.sequenceState().setPitchMode(3);
    processor.sequenceState().setFreePitch(0, units);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 160);
    setInt(processor, swaraxt::IDs::seqGate, 100);
    setInt(processor, swaraxt::IDs::perfGlide, 0);
    setInt(processor, swaraxt::IDs::dspFxProgram, 0);
}

uint16_t renderVoice(SwaraXtAudioProcessor& processor)
{
    processor.prepareToPlay(48000.0, 64);
    processor.engineForTests().applyParameters();
    {
        juce::AudioBuffer<float> warm(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(warm, midi);
    }
    processor.engineForTests().applyParameters();
    auto& part = processor.engineForTests().shruthiPart();
    part.Stop(false);
    part.Start(false);
    uint16_t word = 0;
    for (int tick = 0; tick < 8; ++tick)
    {
        part.Clock(false);
        word = part.voice().pitch_value();
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
    return word;
}

void testVoiceWord()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(1), "19-EDO voice fixture");
    const int32_t units = 60 * 1024 + static_cast<int32_t>(std::lround(0.37 * 1024.0));
    armQuantized(processor, units);
    const auto chosen = processor.quantizeActiveHz(swaraxt::FreeCvPitch::hzFromUnits(units));
    const auto expected = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
    const auto word = renderVoice(processor);
    std::printf("quantized voice word=%u expected=%u hz=%.6f\n",
                static_cast<unsigned>(word), static_cast<unsigned>(expected), chosen.hz);
    expect(chosen.valid, "voice fixture quantizes");
    expect(word == expected, "voice word matches quantized frequency");
    const int nearest = swaraxt::ScalePitch::nearestMidiNote(chosen.hz);
    const double midiHz = 440.0 * std::pow(2.0, (static_cast<double>(nearest) - 69.0) / 12.0);
    expect(std::abs(1200.0 * std::log2(chosen.hz / midiHz)) > 1.0
               || std::abs(1200.0 * std::log2(chosen.hz / midiHz)) < 0.01,
           "voice target is the scale pitch");
    expect(std::abs(static_cast<int>(word) - nearest * 128) < 128,
           "voice word is within a semitone of the external MIDI approximation");

    const auto stored = processor.sequenceState().snapshot().freePitch[0];
    expect(stored == units, "playback does not rewrite stored CV");
}

void testDenseProductionPitchPaths()
{
    swaraxt::tuning::TuningSnapshot snapshot;
    swaraxt::tuning::ParseError error;
    expect(quantizer_test::makeEdo(256, snapshot, error), "production 256-EDO fixture parses");
    std::vector<uint8_t> bytes;
    expect(swaraxt::tuning::writeTuningTable(snapshot, bytes, error), "production 256-EDO encodes");

    SwaraXtAudioProcessor keyboard;
    setInt(keyboard, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
    setInt(keyboard, swaraxt::IDs::midiInputTuning, 2);
    setInt(keyboard, swaraxt::IDs::perfGlide, 0);
    expect(keyboard.loadTuningBytes(bytes.data(), bytes.size()), "QUANTIZED MIDI loads 256-EDO");
    keyboard.prepareToPlay(48000.0, 128);
    juce::AudioBuffer<float> buffer(2, 128);

    SwaraXtAudioProcessor cv;
    expect(cv.loadTuningBytes(bytes.data(), bytes.size()), "QUANTIZED CV loads 256-EDO");
    int midiMismatches = 0;
    int cvMismatches = 0;
    for (int note = 0; note < 128; ++note)
    {
        const auto expected = swaraxt::ScalePitch::toVoicePitchFromHz(quantizer_test::edo256Hz(note));
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        keyboard.processBlock(buffer, off);
        for (int block = 0; block < 4; ++block)
        {
            juce::MidiBuffer midi;
            if (block == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
            keyboard.processBlock(buffer, midi);
        }
        const auto midiWord = keyboard.engineForTests().shruthiPart().voice().pitch_value();
        armQuantized(cv, swaraxt::FreeCvPitch::fromMidiNote(note));
        const auto cvWord = renderVoice(cv);
        if (midiWord != expected)
            ++midiMismatches;
        if (cvWord != expected)
            ++cvMismatches;
        if (note == 108)
            std::printf("production 256-EDO C8 expectedWord=%u midiWord=%u cvWord=%u\n",
                        static_cast<unsigned>(expected), static_cast<unsigned>(midiWord),
                        static_cast<unsigned>(cvWord));
    }
    std::printf("production 256-EDO MIDI 0..127: MIDI mismatches=%d CV mismatches=%d\n",
                midiMismatches, cvMismatches);
    expect(midiMismatches == 0, "production QUANTIZED MIDI voice targets cover the full playable band");
    expect(cvMismatches == 0, "production QUANTIZED CV voice targets cover the full playable band");
}

void testQuantizedMidi()
{
    SwaraXtAudioProcessor processor;
    processor.loadBuiltinTuning(0);
    processor.prepareToPlay(48000.0, 256);
    const int32_t units = 60 * 1024 + static_cast<int32_t>(std::lround(0.73 * 1024.0));
    armQuantized(processor, units);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    const auto chosen = processor.quantizeActiveHz(swaraxt::FreeCvPitch::hzFromUnits(units));
    const int external = swaraxt::ScalePitch::nearestMidiNote(chosen.hz);
    std::printf("quantized MIDI example: CV 60.73 -> %.3f Hz -> MIDI %d (identity 60)\n",
                chosen.hz, external);
    juce::AudioBuffer<float> buffer(2, 256);
    int ons = 0;
    int offs = 0;
    int wrong = 0;
    for (int block = 0; block < 40; ++block)
    {
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (note.note != external)
                ++wrong;
            if (note.noteOn)
                ++ons;
            else
                ++offs;
        }
    }
    expect(ons > 0 && offs > 0, "QUANTIZED CV emits note on and note off");
    expect(wrong == 0, "external MIDI note follows the tuned frequency, not the packed identity");

    processor.sequenceState().setPitchMode(1);
    int linearOns = 0;
    int linearOffs = 0;
    for (int block = 0; block < 6; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
        {
            if (note.noteOn)
                ++linearOns;
            else
                ++linearOffs;
        }
    }
    expect(linearOns == 0, "LINEAR emits nothing after QUANTIZED CV");
    expect(linearOffs > 0, "leaving QUANTIZED CV releases the external note");

    processor.sequenceState().setPitchMode(3);
    setBool(processor, swaraxt::IDs::seqMidiOut, true);
    for (int block = 0; block < 4; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
    processor.sequenceState().setPitchMode(2);
    int scaleOns = 0;
    for (int block = 0; block < 4; ++block)
    {
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        for (const auto& note : notesIn(midi))
            if (note.noteOn)
                ++scaleOns;
    }
    expect(scaleOns == 0, "SCALE stays silent after QUANTIZED CV");
}

void testStateAndLayout()
{
    SwaraXtAudioProcessor processor;
    expect(processor.loadBuiltinTuning(3), "31-EDO state fixture");
    processor.sequenceState().setPitchMode(3);
    processor.sequenceState().setFreePitch(2, 61 * 1024 + 200);
    processor.setSequenceLocked(true);
    const auto cv = processor.sequenceState().snapshot().freePitch;
    const auto name = processor.activeTuningName();
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(restored.sequenceState().snapshot().pitchMode == 3, "QUANTIZED CV survives host state");
    expect(restored.sequenceState().snapshot().freePitch == cv, "CV survives host state");
    expect(restored.activeTuningName() == name, "embedded tuning survives with QUANTIZED CV");
    expect(restored.sequenceLocked(), "LOCK survives with QUANTIZED CV");

    SwaraXtAudioProcessorEditor editor(processor);
    editor.setModuleViewsForTests(false, true);
    const auto sizes = { swaraxt::ui::GuiSize::small, swaraxt::ui::GuiSize::medium, swaraxt::ui::GuiSize::large };
    for (auto size : sizes)
    {
        editor.setGuiSizeForTests(size);
        editor.resized();
        auto& seq = editor.mainPanelForTests().sequencerForTests();
        seq.reflow();
        seq.pitchModeBoxForTests().setSelectedId(4, juce::sendNotificationSync);
        const auto pitch = seq.pitchModeBoxForTests().getBounds();
        const auto lock = seq.lockButtonForTests().getBounds();
        const auto midi = seq.midiOutButtonForTests().getBounds();
        expect(seq.getLocalBounds().contains(pitch), "pitch mode stays inside the sequencer");
        expect(seq.getLocalBounds().contains(lock), "LOCK stays inside the sequencer");
        expect(seq.getLocalBounds().contains(midi), "MIDI OUT stays inside the sequencer");
        expect(! pitch.intersects(lock) && ! pitch.intersects(midi) && ! midi.intersects(lock),
               "pitch mode, MIDI OUT and LOCK do not overlap");
        expect(seq.noteLabelForTests() == "CV", "QUANTIZED CV edits the CV control");
        for (int step = 0; step < 16; ++step)
            expect(! seq.stepButtonForTests(step).getBounds().isEmpty(), "step remains usable");
    }
    const float previous = juce::Desktop::getInstance().getGlobalScaleFactor();
    juce::Desktop::getInstance().setGlobalScaleFactor(1.5f);
    editor.setGuiSizeForTests(swaraxt::ui::GuiSize::medium);
    editor.resized();
    editor.mainPanelForTests().sequencerForTests().reflow();
    expect(editor.mainPanelForTests().sequencerForTests().getLocalBounds().contains(
               editor.mainPanelForTests().sequencerForTests().pitchModeBoxForTests().getBounds()),
           "HiDPI pitch mode stays inside the sequencer");
    juce::Desktop::getInstance().setGlobalScaleFactor(previous);
}

void testTuningPublicationConcurrency()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000.0, 128);
    processor.sequenceState().setPitchMode(3);
    setInt(processor, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(processor, swaraxt::IDs::seqClockMode, 0);
    setInt(processor, swaraxt::IDs::seqTempo, 140);
    std::atomic<bool> stop { false };
    std::atomic<int> audioFailures { 0 };
    std::atomic<int> blocks { 0 };
    std::atomic<int> mode = 3;

    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 128);
        while (! stop.load(std::memory_order_acquire))
        {
            processor.sequenceState().setPitchMode(
                static_cast<uint8_t>(mode.load(std::memory_order_relaxed)));
            processor.engineForTests().applyParameters();
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            const auto chosen = processor.quantizeActiveHz(440.0);
            const double degree0 = processor.hzForActiveDegree(0);
            if (! chosen.valid || ! (degree0 > 0.0))
                audioFailures.fetch_add(1, std::memory_order_relaxed);
            else
            {
                // Scale table and embedded quantizer must be one coherent generation:
                // a 440 Hz probe must land on a playable degree frequency from the same table.
                const double again = processor.hzForActiveDegree(chosen.degree);
                if (! (again > 0.0) || std::abs(1200.0 * std::log2(again / chosen.hz)) > 0.05)
                    audioFailures.fetch_add(1, std::memory_order_relaxed);
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const int tunings[] = { 0, 1, 2, 3, 4, 5 };
    const int modes[] = { 3, 2, 1, 0, 3 };
    for (int round = 0; round < 40; ++round)
    {
        expect(processor.loadBuiltinTuning(tunings[round % 6]), "rapid builtin publication");
        mode.store(modes[round % 5], std::memory_order_relaxed);
        if ((round % 7) == 0)
        {
            const uint8_t garbage[] = { 9, 9, 9 };
            const auto before = processor.activeTuningName();
            expect(! processor.loadTuningBytes(garbage, sizeof(garbage)), "failed import during concurrency");
            expect(processor.activeTuningName() == before, "failed import keeps prior generation");
        }
        if ((round % 11) == 0)
        {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop.store(true, std::memory_order_release);
    audio.join();
    expect(blocks.load() > 20, "audio thread latched many blocks");
    expect(audioFailures.load() == 0, "audio never observed a torn tuning generation");
    std::printf("tuning concurrency: blocks=%d failures=%d\n",
                blocks.load(), audioFailures.load());

    // Teardown while publication is idle: destructor must not leave dangling readers.
    {
        SwaraXtAudioProcessor dying;
        dying.prepareToPlay(44100.0, 64);
        expect(dying.loadBuiltinTuning(5), "tritave before teardown");
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        dying.processBlock(buffer, midi);
        expect(dying.quantizeActiveHz(330.0).valid, "quantizer valid before teardown");
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testQuantizerFamilies();
    testCvPreservationAndRandom();
    testVoiceWord();
    testDenseProductionPitchPaths();
    testQuantizedMidi();
    testStateAndLayout();
    testTuningPublicationConcurrency();
    if (failures == 0)
    {
        std::printf("quantized CV tests: PASSED\n");
        return 0;
    }
    std::printf("quantized CV tests: FAILED (%d)\n", failures);
    return 1;
}
