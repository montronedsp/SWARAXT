// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
using State = swaraxt::SequenceState;
using Snapshot = swaraxt::SequenceSnapshot;
using Point = State::SnapshotPoint;
int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition) { ++failures; std::printf("FAIL: %s\n", message); }
}

Snapshot generation(unsigned id)
{
    Snapshot result;
    result.length = static_cast<uint8_t>(1 + id % 16);
    result.rotation = static_cast<uint8_t>(id % 16);
    result.grooveTemplate = static_cast<uint8_t>(id % 6);
    result.arpPattern = static_cast<uint8_t>(id % 16);
    result.pitchMode = static_cast<uint8_t>(id % 4);
    result.freePitchReady = static_cast<uint8_t>(id & 1);
    result.scaleReady = static_cast<uint8_t>((id >> 1) & 1);
    for (size_t i = 0; i != result.steps.size(); ++i)
    {
        result.steps[i] = Snapshot::pack(static_cast<uint8_t>(0x80u | (32u + id)),
                                         static_cast<uint8_t>(16u * id + i));
        result.freePitch[i] = static_cast<int32_t>((100u * id + i) * 64u);
        result.scaleDegree[i] = static_cast<int16_t>(static_cast<int>(20u * id + i) - 100);
    }
    return result;
}

bool equal(const Snapshot& a, const Snapshot& b)
{
    return a.length == b.length && a.rotation == b.rotation && a.grooveTemplate == b.grooveTemplate
        && a.arpPattern == b.arpPattern && a.pitchMode == b.pitchMode
        && a.freePitchReady == b.freePitchReady && a.scaleReady == b.scaleReady
        && a.steps == b.steps && a.freePitch == b.freePitch && a.scaleDegree == b.scaleDegree;
}

void expectEngine(const SwaraXtAudioProcessor& processor, const Snapshot& expected)
{
    const auto& sequence = processor.engineForTests().shruthiPart().sequencer_settings();
    expect(sequence.pattern_size == expected.length && sequence.pattern_rotation == expected.rotation
               && sequence.seq_groove_template == expected.grooveTemplate,
           "engine uses one complete set of sequence controls");
    for (size_t i = 0; i != expected.steps.size(); ++i)
        expect(sequence.steps[i].data_[0] == Snapshot::dataA(expected.steps[i])
                   && sequence.steps[i].data_[1] == Snapshot::dataB(expected.steps[i]),
               "engine uses the complete packed-note generation");
    expect(processor.engineForTests().appliedSequencerPitchModeForTests() == expected.pitchMode,
           "engine pitch mode belongs to the applied notes");
    expect(equal(processor.engineForTests().appliedSequenceForTests(), expected),
           "engine retains the complete controls, mode, notes, CV and scale-degree snapshot");
}

struct Pause {
    Point point;
    juce::WaitableEvent entered, release;
    std::atomic<bool> first { true }, timedOut { false };
    explicit Pause(Point selected) : point(selected) {}
    static void hook(void* context, Point point) noexcept
    {
        auto& self = *static_cast<Pause*>(context);
        if (point != self.point || ! self.first.exchange(false)) return;
        self.entered.signal();
        if (! self.release.wait(3000)) self.timedOut.store(true);
    }
};

void consecutiveGenerations()
{
    State state;
    juce::WaitableEvent ready, consumed;
    std::atomic<bool> timedOut { false };
    std::thread writer([&] {
        for (unsigned id = 1; id != 13; ++id)
        {
            state.store(generation(id), false);
            ready.signal();
            if (! consumed.wait(3000)) { timedOut.store(true); break; }
        }
    });
    for (unsigned id = 1; id != 13; ++id)
    {
        expect(ready.wait(3000), "consecutive generation is published");
        Snapshot captured;
        uint32_t revision = 0;
        expect(state.capture(captured, revision) && equal(captured, generation(id)),
               "all scalar/packed/CV/degree fields correlate with one generation");
        consumed.signal();
    }
    writer.join();
    expect(! timedOut.load(), "publication handshakes finish within their bound");
}

void incompletePublication()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000, 64);
    const auto previous = generation(4);
    auto next = generation(7);
    auto& state = processor.sequenceState();
    state.store(previous, false);
    processor.engineForTests().applyParameters();
    Pause pause(Point::writerMidpoint);
    state.setSnapshotHook(Pause::hook, &pause);
    std::thread writer([&] { state.store(next, false); });
    expect(pause.entered.wait(3000), "writer is paused after half the arrays have changed");
    state.setArpPatternFromParameter(5);
    Snapshot captured = previous;
    uint32_t revision = 0x12345678u;
    for (int attempt = 0; attempt != 32; ++attempt)
        expect(! state.capture(captured, revision) && equal(captured, previous) && revision == 0x12345678u,
               "failed admission is bounded and leaves caller output untouched");
    int direction = 0;
    for (const int size : { 1, 16, 32, 64 })
    {
        auto* parameter = processor.getApvts().getParameter(swaraxt::IDs::arpDirection);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(++direction)));
        juce::AudioBuffer<float> audio(2, size);
        juce::MidiBuffer midi;
        processor.processBlock(audio, midi);
        expectEngine(processor, previous);
        expect(processor.engineForTests().shruthiPart().sequencer_settings().arp_pattern == 5,
               "independent host ARP control remains live during a failed snapshot");
        expect(processor.engineForTests().shruthiPart().sequencer_settings().arp_direction == direction,
               "ARP direction automation remains independent of sequence admission");
    }
    pause.release.signal(); writer.join();
    state.setSnapshotHook(nullptr, nullptr);
    next.arpPattern = 5;
    expect(! pause.timedOut.load() && state.capture(captured, revision) && equal(captured, next),
           "completed publication captures all notes/CV/degrees plus the independent ARP value");
    processor.engineForTests().applyParameters();
    expectEngine(processor, next);
}

void readerOwnershipAndIndependentArp()
{
    State state;
    const auto previous = generation(3), next = generation(9);
    state.store(previous, false);
    struct Schedule {
        Pause reader { Point::readerAdmitted }, writer { Point::writerContended };
        static void hook(void* context, Point point) noexcept
        {
            auto& self = *static_cast<Schedule*>(context);
            Pause::hook(&self.reader, point); Pause::hook(&self.writer, point);
        }
    } schedule;
    state.setSnapshotHook(Schedule::hook, &schedule);
    Snapshot captured;
    uint32_t revision = 0;
    bool accepted = false;
    std::thread reader([&] { accepted = state.capture(captured, revision); });
    expect(schedule.reader.entered.wait(3000), "reader owns the payload gate");
    Snapshot rejected = previous;
    uint32_t rejectedRevision = 99;
    expect(! state.capture(rejected, rejectedRevision) && equal(rejected, previous) && rejectedRevision == 99,
           "a second reader makes one failed attempt without changing its output");
    std::thread writer([&] { state.store(next, false); });
    expect(schedule.writer.entered.wait(3000), "writer actually fails admission while the reader owns data");
    state.setArpPatternFromParameter(6);
    schedule.reader.release.signal(); reader.join();
    auto expected = previous; expected.arpPattern = 6;
    expect(accepted && equal(captured, expected), "reader cannot be overwritten; independent ARP has a single load point");
    schedule.writer.release.signal(); writer.join();
    state.setSnapshotHook(nullptr, nullptr);
    expect(! schedule.reader.timedOut.load() && ! schedule.writer.timedOut.load(), "ownership schedule terminates");
    expect(state.capture(captured, revision) && equal(captured, next), "waiting writer publishes after reader release");
}

void multipleWriters()
{
    State state;
    const auto first = generation(5), second = generation(11);
    struct Schedule {
        Pause first { Point::writerMidpoint }, second { Point::writerContended };
        static void hook(void* context, Point point) noexcept
        {
            auto& self = *static_cast<Schedule*>(context);
            Pause::hook(&self.first, point); Pause::hook(&self.second, point);
        }
    } schedule;
    state.setSnapshotHook(Schedule::hook, &schedule);
    std::thread a([&] { state.store(first, false); });
    expect(schedule.first.entered.wait(3000), "first writer owns an incomplete publication");
    std::thread b([&] { state.store(second, false); });
    expect(schedule.second.entered.wait(3000), "second writer actually contends on the serialization gate");
    Snapshot captured;
    uint32_t revision = 0;
    expect(! state.capture(captured, revision), "reader cannot admit either incomplete writer");
    schedule.first.release.signal(); a.join();
    expect(state.capture(captured, revision) && equal(captured, first), "first writer is complete before the second enters");
    schedule.second.release.signal(); b.join();
    state.setSnapshotHook(nullptr, nullptr);
    expect(state.capture(captured, revision) && equal(captured, second), "second writer publishes one complete successor");
    expect(! schedule.first.timedOut.load() && ! schedule.second.timedOut.load(), "both writer threads terminate before destruction");
}

void rolloverAndEngineTagReuse()
{
    State state;
    state.setRevisionForTests(std::numeric_limits<uint32_t>::max() - 1u);
    state.store(generation(1), false);
    Snapshot captured;
    uint32_t revision = 99;
    expect(state.capture(captured, revision) && revision == 0 && equal(captured, generation(1)),
           "unsigned revision rollover does not mix a captured generation");
    state.store(generation(2), false);
    expect(state.capture(captured, revision) && revision == 2 && equal(captured, generation(2)),
           "publication remains coherent immediately after rollover");
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000, 64);
    const auto previous = generation(4), next = generation(7);
    auto& sequence = processor.sequenceState();
    sequence.store(previous, false);
    processor.engineForTests().applyParameters();
    expect(sequence.capture(captured, revision), "engine baseline revision is captured");
    sequence.setRevisionForTests(revision - 2u);
    sequence.store(next, false);
    uint32_t reused = 0;
    expect(sequence.capture(captured, reused) && reused == revision && equal(captured, next),
           "controlled counter reuse aliases the prior engine tag with a different complete snapshot");
    processor.engineForTests().applyParameters();
    expectEngine(processor, next);
    auto changed = next;
    ++changed.freePitch[0];
    sequence.setRevisionForTests(revision - 2u);
    sequence.store(changed, false);
    processor.engineForTests().applyParameters();
    expectEngine(processor, changed);
    ++changed.scaleDegree[3];
    sequence.setRevisionForTests(revision - 2u);
    sequence.store(changed, false);
    processor.engineForTests().applyParameters();
    expectEngine(processor, changed);
}

void modeTransitionsAndIdenticalPublications()
{
    SwaraXtAudioProcessor processor;
    processor.prepareToPlay(48000, 64);
    const auto setInt = [&](const char* id, int value) {
        auto* parameter = processor.getApvts().getParameter(id);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
    };
    setInt(swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
    setInt(swaraxt::IDs::seqClockMode, 0);
    setInt(swaraxt::IDs::seqGate, 100);
    auto& state = processor.sequenceState();
    auto& engine = processor.engineForTests();
    auto& part = engine.shruthiPart();
    for (uint8_t from = 0; from != 4; ++from)
    {
        for (uint8_t to = 0; to != 4; ++to)
        {
            auto previous = generation(4);
            previous.pitchMode = from;
            state.store(previous, false);
            engine.applyParameters();
            Snapshot captured;
            uint32_t revision = 0;
            expect(state.capture(captured, revision), "capture transition's applied revision");
            auto next = generation(7);
            next.pitchMode = to;
            next.steps.fill(Snapshot::pack(0x80 | 60, 0x70));
            state.setRevisionForTests(revision - 2u);
            state.store(next, false);
            engine.applyParameters();
            expectEngine(processor, next);

            part.Start(false);
            part.Clock(false);
            const auto step = part.step();
            const auto playback = part.playback_step();
            const auto gate = part.gate_counter_for_tests();
            const auto generated = part.generated_note_count_for_tests();
            const auto pitch = part.voice().pitch_value();
            expect(part.running() && generated > 0, "identical-publication probe has active playback");
            for (int repeat = 0; repeat != 2; ++repeat)
            {
                // First revisit the same tag, then republish identical content.
                if (repeat == 1) state.store(next, false);
                engine.applyParameters();
                expectEngine(processor, next);
                expect(part.running() && part.step() == step && part.playback_step() == playback
                           && part.gate_counter_for_tests() == gate
                           && part.generated_note_count_for_tests() == generated
                           && part.voice().pitch_value() == pitch,
                       "identical state preserves playback position, gate, generated notes and pitch");
            }
            part.Stop(false);
        }
    }
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    consecutiveGenerations();
    incompletePublication();
    readerOwnershipAndIndependentArp();
    multipleWriters();
    rolloverAndEngineTagReuse();
    modeTransitionsAndIdenticalPublications();
    std::printf("Sequence snapshot tests: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
