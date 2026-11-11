// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>

#include "Plugin/PluginProcessor.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

using Processor = SwaraXtAudioProcessor;
using State = swaraxt::SequenceState;
using Snapshot = swaraxt::SequenceSnapshot;
using Point = State::SnapshotPoint;

int failures = 0;

void expect(bool ok, const char* message)
{
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
        std::fflush(stdout);
    }
}

void pump()
{
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating())
        manager->runDispatchLoopUntil(1);
}

void setInt(Processor& processor, const char* id, int value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    expect(parameter != nullptr, "parameter exists");
    if (parameter != nullptr)
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

int getInt(const Processor& processor, const char* id)
{
    const auto* raw = processor.getApvts().getRawParameterValue(id);
    return raw != nullptr ? static_cast<int>(std::lround(raw->load())) : -1;
}

uint8_t firmwareArp(const Processor& processor)
{
    return processor.engineForTests().shruthiPart().sequencer_settings().arp_pattern;
}

void process(Processor& processor, int samples = 64)
{
    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    processor.processBlock(audio, midi);
}

struct Pause
{
    Point point;
    juce::WaitableEvent entered, release;
    std::atomic<bool> first { true };
    std::atomic<bool> timedOut { false };
    explicit Pause(Point selected) : point(selected) {}
    static void hook(void* context, Point point) noexcept
    {
        auto& self = *static_cast<Pause*>(context);
        if (point != self.point || ! self.first.exchange(false))
            return;
        self.entered.signal();
        if (! self.release.wait(3000))
            self.timedOut.store(true);
    }
};

void expectArp(const Processor& processor, int expected, const char* message)
{
    expect(getInt(processor, swaraxt::IDs::arpPattern) == expected, message);
    expect(processor.sequenceState().arpPattern() == static_cast<uint8_t>(expected), message);
    expect(processor.sequenceState().snapshot().arpPattern == static_cast<uint8_t>(expected), message);
    expect(firmwareArp(processor) == static_cast<uint8_t>(expected), message);
}

juce::MemoryBlock save(Processor& processor)
{
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    return block;
}

juce::ValueTree treeFrom(const juce::MemoryBlock& block)
{
    auto xml = Processor::getXmlFromBinary(block.getData(), static_cast<int>(block.getSize()));
    return xml != nullptr ? juce::ValueTree::fromXml(*xml) : juce::ValueTree {};
}

juce::MemoryBlock binary(const juce::ValueTree& tree)
{
    juce::MemoryBlock block;
    if (auto xml = tree.createXml())
        Processor::copyXmlToBinary(*xml, block);
    return block;
}

void lostUpdateFromProductionEdit()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 2);
    process(processor);
    expectArp(processor, 2, "APVTS path published pattern 2");

    auto& state = processor.sequenceState();
    Pause pause(Point::editBeforeCommit);
    state.setSnapshotHook(Pause::hook, &pause);

    std::thread editor([&] { state.setFreePitch(3, 60 * swaraxt::FreeCvPitch::kUnitsPerSemitone); });
    expect(pause.entered.wait(3000), "unrelated edit captured pattern 2 and paused before write");
    expect(state.arpPattern() == 2, "paused editor still holds captured pattern 2");

    setInt(processor, swaraxt::IDs::arpPattern, 5);
    expect(state.arpPattern() == 5, "listener published host pattern 5 during the paused edit");
    process(processor);
    expect(firmwareArp(processor) == 5, "firmware adopted host pattern 5 before the editor resumed");

    pause.release.signal();
    editor.join();
    state.setSnapshotHook(nullptr, nullptr);
    expect(! pause.timedOut.load(), "paused editor released within the bound");
    process(processor);

    expect(getInt(processor, swaraxt::IDs::arpPattern) == 5,
           "APVTS retains the automated ARP pattern after the unrelated edit");
    expect(state.arpPattern() == 5,
           "unrelated sequencer edit must not restore the stale ARP snapshot");
    expect(firmwareArp(processor) == 5,
           "firmware ARP follows the host authority after the unrelated edit");
    expect(state.snapshot().freePitchReady == 1, "the intended free-CV edit still published");

    const auto restored = save(processor);
    Processor fresh;
    fresh.setStateInformation(restored.getData(), static_cast<int>(restored.getSize()));
    fresh.prepareToPlay(48000, 64);
    process(fresh);
    expectArp(fresh, 5, "saved state keeps the host ARP pattern, not the stale editor snapshot");
}

void productionEditsPreserveLiveArp()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 2);
    process(processor);

    auto& state = processor.sequenceState();
    const auto run = [&](auto&& edit, const char* name) {
        juce::ignoreUnused(name);
        Pause pause(Point::editBeforeCommit);
        state.setSnapshotHook(Pause::hook, &pause);
        std::thread editor([&] { edit(); });
        expect(pause.entered.wait(3000), "production edit reaches the commit barrier");
        setInt(processor, swaraxt::IDs::arpPattern, 5);
        expect(state.arpPattern() == 5, "host ARP is live at the commit barrier");
        pause.release.signal();
        editor.join();
        state.setSnapshotHook(nullptr, nullptr);
        process(processor);
        expect(state.arpPattern() == 5, "production sequencer edit preserved live ARP");
        setInt(processor, swaraxt::IDs::arpPattern, 2);
        process(processor);
    };

    run([&] { state.setFreePitch(1, 61 * swaraxt::FreeCvPitch::kUnitsPerSemitone); }, "free-cv");
    run([&] { state.setScaleDegree(2, 3); }, "scale-degree");
    run([&] { state.setPitchMode(1); }, "pitch-mode");
    juce::Random random(0xC0FFEEu);
    run([&] { state.randomize(State::Randomize::notes, random); }, "randomize");

    setInt(processor, swaraxt::IDs::arpPattern, 5);
    state.setStep(4, 0x80 | 62, 0x70);
    state.setRotation(3);
    state.setLength(11);
    process(processor);
    expect(state.arpPattern() == 5, "single-field sequencer writes never touch ARP");
}

void twoConcurrentEdits()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 4);
    process(processor);
    auto& state = processor.sequenceState();
    state.setStep(0, 0x80 | 48, 0x70);
    state.setStep(1, 0x80 | 50, 0x70);

    std::atomic<int> ready { 0 };
    juce::WaitableEvent go { true };
    std::thread a([&] {
        ready.fetch_add(1);
        if (! go.wait(3000))
            expect(false, "first concurrent editor was released");
        else
            state.setStep(0, 0x80 | 60, 0x71);
    });
    std::thread b([&] {
        ready.fetch_add(1);
        if (! go.wait(3000))
            expect(false, "second concurrent editor was released");
        else
            state.setFreePitch(1, 64 * swaraxt::FreeCvPitch::kUnitsPerSemitone);
    });
    while (ready.load() < 2)
        juce::Thread::yield();
    setInt(processor, swaraxt::IDs::arpPattern, 9);
    go.signal();
    a.join();
    b.join();
    process(processor);

    const auto snapshot = state.snapshot();
    expect(snapshot.arpPattern == 9, "concurrent edits leave host ARP intact");
    expect(Snapshot::dataA(snapshot.steps[0]) == (0x80 | 60), "first step edit published");
    expect(snapshot.freePitchReady == 1, "second free-CV edit published");
    expect(firmwareArp(processor) == 9, "firmware ARP follows the host after concurrent edits");
}

void restorePrecedence()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 7);
    processor.sequenceState().setLength(9);
    process(processor);
    auto blob = save(processor);

    Processor reopened;
    reopened.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    reopened.prepareToPlay(48000, 64);
    process(reopened);
    expectArp(reopened, 7, "normal save/reopen keeps ARP 7");
    expect(reopened.sequenceState().snapshot().length == 9, "normal save/reopen keeps sequence length");

    auto missingSequence = treeFrom(blob);
    if (const auto child = missingSequence.getChildWithName("SEQUENCE"); child.isValid())
        missingSequence.removeChild(child, nullptr);
    Processor legacyNoSequence;
    const auto missingSequenceBlob = binary(missingSequence);
    legacyNoSequence.setStateInformation(missingSequenceBlob.getData(),
                                         static_cast<int>(missingSequenceBlob.getSize()));
    legacyNoSequence.prepareToPlay(48000, 64);
    process(legacyNoSequence);
    expectArp(legacyNoSequence, 7, "legacy state without SEQUENCE uses APVTS ARP");

    auto missingParam = treeFrom(blob);
    for (int i = missingParam.getNumChildren(); --i >= 0;)
    {
        const auto child = missingParam.getChild(i);
        if (child.hasType("PARAM") && child.getProperty("id").toString() == swaraxt::IDs::arpPattern)
            missingParam.removeChild(i, nullptr);
    }
    if (auto sequence = missingParam.getChildWithName("SEQUENCE"); sequence.isValid())
        sequence.setProperty("arpPattern", 12, nullptr);
    Processor legacyNoParam;
    const auto missingParamBlob = binary(missingParam);
    legacyNoParam.setStateInformation(missingParamBlob.getData(),
                                      static_cast<int>(missingParamBlob.getSize()));
    legacyNoParam.prepareToPlay(48000, 64);
    process(legacyNoParam);
    expect(getInt(legacyNoParam, swaraxt::IDs::arpPattern) == 12, "legacy PARAM-less restore aligns APVTS to SEQUENCE ARP");
    expect(legacyNoParam.sequenceState().arpPattern() == 12, "legacy PARAM-less restore keeps SEQUENCE ARP");
    expect(firmwareArp(legacyNoParam) == 12, "legacy PARAM-less restore publishes SEQUENCE ARP to firmware");

    auto disagreed = treeFrom(blob);
    for (auto child : disagreed)
        if (child.hasType("PARAM") && child.getProperty("id").toString() == swaraxt::IDs::arpPattern)
            child.setProperty("value", 3, nullptr);
    if (auto sequence = disagreed.getChildWithName("SEQUENCE"); sequence.isValid())
        sequence.setProperty("arpPattern", 14, nullptr);
    Processor conflict;
    const auto conflictBlob = binary(disagreed);
    conflict.setStateInformation(conflictBlob.getData(), static_cast<int>(conflictBlob.getSize()));
    conflict.prepareToPlay(48000, 64);
    process(conflict);
    expect(getInt(conflict, swaraxt::IDs::arpPattern) == 3, "APVTS keeps its serialized ARP on disagreement");
    expect(conflict.sequenceState().arpPattern() == 3,
           "host ARP is authoritative when SEQUENCE and APVTS disagree");
    expect(firmwareArp(conflict) == 3, "firmware follows host ARP after a disagreed restore");

    Processor locked;
    locked.prepareToPlay(48000, 64);
    setInt(locked, swaraxt::IDs::arpPattern, 6);
    locked.sequenceState().setLength(8);
    locked.setSequenceLocked(true);
    locked.setCurrentProgram(1);
    process(locked);
    expect(getInt(locked, swaraxt::IDs::arpPattern) == 6, "sequence lock preserves ARP across factory load");
    expect(locked.sequenceState().snapshot().length == 8, "sequence lock preserves length across factory load");

    Processor first, second;
    first.prepareToPlay(48000, 64);
    second.prepareToPlay(48000, 64);
    setInt(first, swaraxt::IDs::arpPattern, 2);
    setInt(second, swaraxt::IDs::arpPattern, 11);
    process(first);
    process(second);
    expectArp(first, 2, "first instance keeps pattern 2");
    expectArp(second, 11, "second instance keeps pattern 11");
}

void automationAroundRestore()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 4);
    process(processor);
    auto blob = save(processor);
    setInt(processor, swaraxt::IDs::arpPattern, 10);
    process(processor);
    processor.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    process(processor);
    expectArp(processor, 4, "restore after live automation returns the saved ARP");
    setInt(processor, swaraxt::IDs::arpPattern, 13);
    process(processor);
    expectArp(processor, 13, "automation after restore reaches SequenceState and firmware");
}

void throwingEditReleasesGate()
{
    State state;
    state.setArpPatternFromParameter(7);
    state.setLength(9);
    try
    {
        state.throwingEditForTests();
        expect(false, "throwing sequencer edit must propagate");
    }
    catch (int value)
    {
        expect(value == 1, "throwing sequencer edit propagates the mutator exception");
    }
    expect(state.arpPattern() == 7, "failed edit leaves ARP unchanged");
    expect(state.snapshot().length == 9, "failed edit leaves sequencer payload unchanged");
    state.setFreePitch(0, 50 * swaraxt::FreeCvPitch::kUnitsPerSemitone);
    expect(state.snapshot().freePitchReady == 1, "writer gate is usable after a throwing edit");
}

void editorOpenAndClosed()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::arpPattern, 2);
    process(processor);
    pump();
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    expect(editor != nullptr, "editor opens");
    pump();
    setInt(processor, swaraxt::IDs::arpPattern, 8);
    process(processor);
    pump();
    expectArp(processor, 8, "ARP automation while the editor is open");
    processor.sequenceState().setStep(5, 0x80 | 55, 0x60);
    process(processor);
    expectArp(processor, 8, "sequence edit with the editor open leaves ARP 8");
    editor.reset();
    pump();
    setInt(processor, swaraxt::IDs::arpPattern, 1);
    process(processor);
    processor.sequenceState().setFreePitch(0, 40 * swaraxt::FreeCvPitch::kUnitsPerSemitone);
    process(processor);
    expectArp(processor, 1, "ARP automation while the editor is closed");
}

}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI initialise;
    const auto run = [](const char* name, void (*fn)()) {
        std::printf("RUN %s\n", name);
        std::fflush(stdout);
        fn();
        std::printf("DONE %s failures=%d\n", name, failures);
        std::fflush(stdout);
    };
    run("lost-update", lostUpdateFromProductionEdit);
    run("production-edits", productionEditsPreserveLiveArp);
    run("concurrent-edits", twoConcurrentEdits);
    run("restore-precedence", restorePrecedence);
    run("automation-around-restore", automationAroundRestore);
    run("throwing-edit", throwingEditReleasesGate);
    run("editor-open-closed", editorOpenAndClosed);
    std::printf("ARP automation tests: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    std::fflush(stdout);
    return failures ? 1 : 0;
}
