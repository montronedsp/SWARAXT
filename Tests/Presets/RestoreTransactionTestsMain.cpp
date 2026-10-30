// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using Processor = SwaraXtAudioProcessor;
using Engine = swaraxt::SwaraXtEngine;
int failures = 0;
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool trackAllocations = false;
std::atomic<int> callbackAllocations {0};
int allocationHook(int type, void*, size_t, int, long, const unsigned char*, int)
{
    if (trackAllocations && (type == _HOOK_ALLOC || type == _HOOK_REALLOC))
        callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    return 1;
}
#endif
void expect(bool ok, const char* message)
{
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
juce::MemoryBlock save(Processor& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }
void restore(Processor& p, const juce::MemoryBlock& b) { p.setStateInformation(b.getData(), static_cast<int>(b.getSize())); }
juce::ValueTree tree(const juce::MemoryBlock& b)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary(b.getData(), static_cast<int>(b.getSize()));
    return xml ? juce::ValueTree::fromXml(*xml) : juce::ValueTree {};
}
juce::MemoryBlock binary(const juce::ValueTree& t)
{
    juce::MemoryBlock b; auto xml = t.createXml(); juce::AudioProcessor::copyXmlToBinary(*xml, b); return b;
}
void parameter(Processor& p, int value)
{
    auto* v = p.getApvts().getParameter(swaraxt::IDs::osc1Param);
    v->setValueNotifyingHost(v->convertTo0to1(static_cast<float>(value)));
}
float parameter(Processor& p) { return p.getApvts().getRawParameterValue(swaraxt::IDs::osc1Param)->load(); }
void target(Processor& p)
{
    parameter(p, 93); p.sequenceState().setLength(7);
    p.setFilterQuality(swaraxt::FilterQuality::eco);
    expect(p.loadBuiltinTuning(3), "target tuning publishes");
}
struct Files {
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("swara-restore-" + juce::Uuid().toString());
    Files() { expect(directory.createDirectory().wasOk(), "private fixture directory"); }
    ~Files() { directory.deleteRecursively(); }
    juce::File file(const char* name) const { return directory.getChildFile(juce::String(name) + ".swaraxtpreset"); }
};
struct Snapshot {
    juce::MemoryBlock state;
    std::vector<uint8_t> tuning;
    int slot;
    explicit Snapshot(Processor& p) : state(save(p)), tuning(p.engineForTests().tuningBytesForState()),
                                    slot(p.engineForTests().publishedTuningForTests()) {}
    bool matches(Processor& p) const
    {
        return tree(state).isEquivalentTo(tree(save(p))) && tuning == p.engineForTests().tuningBytesForState()
            && slot == p.engineForTests().publishedTuningForTests();
    }
};
struct Pause {
    Engine::TuningPoint point;
    juce::WaitableEvent entered, release;
    std::atomic<bool> once { true };
    explicit Pause(Engine::TuningPoint p) : point(p) {}
    static void hook(void* c, Engine::TuningPoint p, int) noexcept
    {
        auto& self = *static_cast<Pause*>(c);
        if (p == self.point && self.once.exchange(false))
        {
            self.entered.signal();
            if (!self.release.wait(15000)) std::abort();
        }
    }
};
void render(Processor& p, bool note = false)
{
    juce::AudioBuffer<float> b(2, 256); juce::MidiBuffer midi;
    midi.ensureSize(swaraxt::kMidiOutputReserveBytes);
    if (note) midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
#if defined(_MSC_VER) && defined(_DEBUG)
    trackAllocations = true;
#endif
    p.processBlock(b, midi);
#if defined(_MSC_VER) && defined(_DEBUG)
    trackAllocations = false;
#endif
}
void rejection(bool user, bool busy)
{
    Files files; Processor src, p; target(src); auto wanted = save(src);
    auto file = files.file("Target"); expect(file.replaceWithData(wanted.getData(), wanted.getSize()), "target file");
    auto& engine = p.engineForTests();
    const swaraxt::tuning::PublishedTuning* pinned = nullptr;
    int first = -1, second = -1;
    Pause pause(Engine::TuningPoint::writerSelected);
    std::thread writer;
    bool wrote = false;
    if (busy)
    {
        engine.setTuningHook(Pause::hook, &pause);
        writer = std::thread([&] { wrote = p.loadBuiltinTuning(1); });
        expect(pause.entered.wait(15000), "independent writer owns tuning admission");
    }
    else
    {
        expect(engine.pinTuningForTests(pinned, first), "pin first tuning");
        expect(p.loadBuiltinTuning(1), "second tuning");
        expect(engine.pinTuningForTests(pinned, second), "pin second tuning");
        expect(p.loadBuiltinTuning(2), "all three slots protected");
    }
    Snapshot before(p);
    juce::String error;
    if (user) expect(!p.loadPresetEntry({"Target", file, -1, false}, error) && error.isNotEmpty(), "user load rejects with reason");
    else restore(p, wanted);
    expect(p.lastRestoreResult() == (busy ? Processor::RestoreResult::tuningBusy : Processor::RestoreResult::tuningUnavailable), "typed rejection");
    expect(before.matches(p), "rejection preserves complete serialized state, bytes and published slot");
    if (busy)
    {
        pause.release.signal(); writer.join(); engine.setTuningHook(nullptr, nullptr);
        expect(wrote, "earlier writer completes");
    }
    else { engine.unpinTuningForTests(first); engine.unpinTuningForTests(second); }
    restore(p, wanted);
    expect(p.lastRestoreResult() == Processor::RestoreResult::success && parameter(p) == 93
        && p.sequenceState().snapshot().length == 7 && p.filterQuality() == swaraxt::FilterQuality::eco
        && p.engineForTests().tuningBytesForState() == src.engineForTests().tuningBytesForState(), "retry completely restores target");
    Processor fresh; restore(fresh, save(p));
    expect(tree(save(fresh)).isEquivalentTo(tree(save(p))), "fresh-instance complete roundtrip");
}
void malformed()
{
    Files files; Processor src; target(src);
    for (const auto* value : { "-1.", "8388608.", "999999999999999.", "4.AA", "3.@@@@" })
        for (bool user : {false, true})
        {
            Processor p; Snapshot before(p); auto t = tree(save(src)); t.setProperty("tuningTable", value, nullptr);
            auto b = binary(t); juce::String error;
            if (user)
            {
                const auto file = files.file("Bad"); file.replaceWithData(b.getData(), b.getSize());
                expect(!p.loadPresetEntry({"Bad", file, -1, false}, error) && error.isNotEmpty(), "malformed user state rejected");
            }
            else restore(p, b);
            expect(p.lastRestoreResult() == Processor::RestoreResult::invalid && before.matches(p), "bad length rejects before any mutation");
        }
    Processor p; p.loadBuiltinTuning(1); const auto old = p.engineForTests().tuningBytesForState();
    for (const auto* text : { "", "not-base64" })
    {
        auto t = tree(save(src)); t.setProperty("tuningTable", text, nullptr); restore(p, binary(t));
        expect(parameter(p) == 93 && p.engineForTests().tuningBytesForState() == old, "legacy no-tuning action retained");
    }
    auto t = tree(save(src)); juce::MemoryBlock bad("badSWT", 6);
    t.setProperty("tuningTable", bad.toBase64Encoding(), nullptr); restore(p, binary(t));
    expect(p.lastRestoreResult() == Processor::RestoreResult::defaultTuning && p.activeBuiltinTuning() == 0, "checked invalid-SWT fallback");
    {
        Engine::TuningReservation reservation;
        expect(p.engineForTests().reserveBuiltinTuning(2, reservation) == Engine::TuningResult::success, "abortable reservation");
        Snapshot before(p); restore(p, binary(t));
        expect(p.lastRestoreResult() == Processor::RestoreResult::tuningBusy && before.matches(p), "fallback admission failure rejects completely");
    }
    expect(p.loadBuiltinTuning(1), "aborted reservation releases ownership");
}
void callbackBoundary()
{
    Processor p, src; target(src); auto wanted = save(src); p.prepareToPlay(48000, 256); render(p, true);
    auto& engine = p.engineForTests(); const auto previous = engine.stableTuningForTests();
    const auto patch = engine.shruthiPart().patch().osc[0].parameter;
    Pause pause(Engine::TuningPoint::audioExiting); engine.setTuningHook(Pause::hook, &pause);
    std::thread audio([&] { render(p); }); expect(pause.entered.wait(15000), "render owns old cohort");
    struct Waiting {
        juce::WaitableEvent entered;
        static void hook(void* c, Processor::StatePublicationPoint pt)
        { if (pt == Processor::StatePublicationPoint::writerContended) static_cast<Waiting*>(c)->entered.signal(); }
    } waiting;
    p.setStatePublicationHook(Waiting::hook, &waiting);
    std::thread control([&] { restore(p, wanted); });
    expect(waiting.entered.wait(15000), "restore waits off audio for admitted render");
    pause.release.signal(); audio.join(); control.join();
    p.setStatePublicationHook(nullptr, nullptr); engine.setTuningHook(nullptr, nullptr);
    expect(engine.shruthiPart().patch().osc[0].parameter == patch
        && juce::String(engine.stableTuningForTests().name) == previous.name
        && engine.appliedSequenceForTests().length == 16, "completed old callback retained old cohort");
    render(p);
    expect(engine.shruthiPart().patch().osc[0].parameter == 93
        && juce::String(engine.stableTuningForTests().name) == src.activeTuningName()
        && engine.appliedSequenceForTests().length == 7
        && engine.filter().quality() == swaraxt::FilterQuality::eco, "next admitted callback adopts complete target");
}
void resetOnPrepare()
{
    Processor src; target(src); const auto state = save(src);
    for (int mode = 0; mode < 3; ++mode)
    {
        Processor p; p.prepareToPlay(48000, 256); render(p, true); render(p);
        expect(p.engineForTests().shruthiPart().has_held_notes(), "held voice exists before restore");
        restore(p, state);
        if (mode == 1) p.prepareToPlay(48000, 256);
        if (mode == 2) { p.releaseResources(); p.prepareToPlay(96000, 64); }
        render(p);
        expect(!p.engineForTests().shruthiPart().has_held_notes()
            && p.engineForTests().shruthiPart().voice().amplitude_envelope_dead(), "accepted reset survives reprepare");
    }
}
void sequenceAdmission()
{
    Processor p, src; target(src); p.prepareToPlay(48000, 256); render(p, true);
    restore(p, save(src));
    struct SequencePause {
        juce::WaitableEvent entered, release;
        static void hook(void* c, swaraxt::SequenceState::SnapshotPoint pt) noexcept
        {
            if (pt != swaraxt::SequenceState::SnapshotPoint::writerMidpoint) return;
            auto& self = *static_cast<SequencePause*>(c); self.entered.signal();
            if (!self.release.wait(15000)) std::abort();
        }
    } pause;
    const auto pending = p.sequenceState().snapshot();
    p.sequenceState().setSnapshotHook(SequencePause::hook, &pause);
    std::thread writer([&] { p.sequenceState().store(pending, false); });
    expect(pause.entered.wait(15000), "sequence capture temporarily unavailable after accepted restore");
    render(p);
    expect(p.engineForTests().shruthiPart().has_held_notes()
        && p.engineForTests().appliedSequenceForTests().length == 16, "failed sequence admission preserves old cohort and leaves reset pending");
    pause.release.signal(); writer.join(); p.sequenceState().setSnapshotHook(nullptr, nullptr);
    render(p);
    expect(!p.engineForTests().shruthiPart().has_held_notes()
        && p.engineForTests().appliedSequenceForTests().length == 7, "retry consumes reset with complete sequence");
}
void frozenCallback()
{
    Processor p, src; target(src); const auto wanted = save(src);
    p.prepareToPlay(48000, 256); render(p, true); render(p);
    const auto oldTuning = juce::String(p.engineForTests().stableTuningForTests().name);
    struct CommitPause {
        juce::WaitableEvent entered, release;
        static void hook(void* c, Processor::StatePublicationPoint pt)
        {
            if (pt != Processor::StatePublicationPoint::restoreBeforeTuningPublish) return;
            auto& self = *static_cast<CommitPause*>(c); self.entered.signal();
            if (!self.release.wait(15000)) std::abort();
        }
    } pause;
    p.setStatePublicationHook(CommitPause::hook, &pause);
    std::thread writer([&] { restore(p, wanted); });
    expect(pause.entered.wait(15000), "writer paused with new parameters and quality but unpublished tuning");
    for (int i = 0; i < 8; ++i) render(p);
    expect(parameter(p) == 93 && p.filterQuality() == swaraxt::FilterQuality::eco, "new scalar controls are staged");
    expect(p.engineForTests().shruthiPart().patch().osc[0].parameter != 93
        && p.engineForTests().appliedSequenceForTests().length == 16
        && juce::String(p.engineForTests().stableTuningForTests().name) == oldTuning
        && p.engineForTests().filter().quality() == swaraxt::FilterQuality::normal, "busy callbacks retain old patch, sequence, tuning AND quality");
    pause.release.signal(); writer.join(); p.setStatePublicationHook(nullptr, nullptr); render(p);
    expect(p.engineForTests().shruthiPart().patch().osc[0].parameter == 93
        && p.engineForTests().filter().quality() == swaraxt::FilterQuality::eco, "later admission converges without a queue");
}
void createOnlyCollision()
{
    Files files; Processor p; p.setUserPresetDirectoryForTests(files.directory); parameter(p, 31);
    struct Pair {
        juce::WaitableEvent entered[2], release[2]; std::atomic<int> n {0};
        static void hook(void* c, Processor::StatePublicationPoint pt)
        {
            if (pt != Processor::StatePublicationPoint::userPresetFileReady) return;
            auto& self = *static_cast<Pair*>(c); const int i = self.n++;
            if (i >= 2) std::abort();
            self.entered[i].signal();
            if (!self.release[i].wait(15000)) std::abort();
        }
    } pair;
    p.setStatePublicationHook(Pair::hook, &pair);
    bool old = false, newer = false; juce::String firstError, secondError;
    std::thread first([&] { old = p.saveUserPreset("Collision", false, firstError); });
    expect(pair.entered[0].wait(15000), "old save captured"); parameter(p, 91);
    std::thread second([&] { newer = p.saveUserPreset("Collision", false, secondError); });
    expect(pair.entered[1].wait(15000), "newer save captured"); pair.release[1].signal(); second.join();
    pair.release[0].signal(); first.join(); p.setStatePublicationHook(nullptr, nullptr);
    expect(!old && newer && firstError.containsIgnoreCase("already exists"), "create-only loser cannot overwrite winner");
    Processor fresh; juce::String error;
    expect(fresh.loadPresetEntry({"Collision", files.file("Collision"), -1, false}, error)
        && parameter(fresh) == 91, "winner file retains its independently captured sound");
}
}
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previousHook = _CrtSetAllocHook(allocationHook);
#endif
    for (bool user : {false, true}) for (bool busy : {false, true}) rejection(user, busy);
    malformed(); callbackBoundary(); frozenCallback(); sequenceAdmission(); resetOnPrepare(); createOnlyCollision();
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previousHook);
    expect(callbackAllocations.load() == 0, "zero allocations in tested audio callbacks");
    std::printf("Callback allocations: %d\n", callbackAllocations.load());
#endif
    std::printf("Restore transaction tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
