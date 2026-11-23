// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <memory>
#include <thread>

thread_local bool failNextAllocation = false;
thread_local bool allocationInjected = false;
static void failAllocationIfArmed()
{
    if (failNextAllocation)
    {
        failNextAllocation = false;
        allocationInjected = true;
        throw std::bad_alloc();
    }
}
#if defined(SWARAXT_HOST_RESTORE_WRAP_NEW)
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __real__Znam(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t size)
{
    failAllocationIfArmed();
    return __real__Znwm(size);
}
extern "C" void* __wrap__Znam(std::size_t size)
{
    failAllocationIfArmed();
    return __real__Znam(size);
}
#else
void* operator new(std::size_t size)
{
    failAllocationIfArmed();
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

namespace {
using Processor = SwaraXtAudioProcessor;
using Editor = SwaraXtAudioProcessorEditor;
using Engine = swaraxt::SwaraXtEngine;
using Result = Processor::RestoreResult;
int failures = 0;
void expect(bool ok, const char* message)
{
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
juce::MemoryBlock save(Processor& p)
{
    juce::MemoryBlock b; p.getStateInformation(b); return b;
}
void restore(Processor& p, const juce::MemoryBlock& b)
{
    p.setStateInformation(b.getData(), static_cast<int>(b.getSize()));
}
juce::ValueTree tree(const juce::MemoryBlock& b)
{
    auto xml = Processor::getXmlFromBinary(b.getData(), static_cast<int>(b.getSize()));
    return xml ? juce::ValueTree::fromXml(*xml) : juce::ValueTree {};
}
juce::MemoryBlock binary(const juce::ValueTree& t)
{
    juce::MemoryBlock b; auto xml = t.createXml(); Processor::copyXmlToBinary(*xml, b); return b;
}
void set(Processor& p, const char* id, float value)
{
    auto* parameter = p.getApvts().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
juce::MemoryBlock fixture(bool b)
{
    Processor p; juce::String error;
    expect(p.loadPresetEntry({ {}, {}, b ? 2 : 1, true }, error), "fixture factory context loads");
    set(p, swaraxt::IDs::osc1Param, b ? 91.0f : 27.0f);
    set(p, swaraxt::IDs::filterCutoff, b ? 7319.0f : 1327.0f);
    p.sequenceState().setLength(b ? 7 : 11);
    p.sequenceState().setStep(0, static_cast<uint8_t>(0x80 | (b ? 73 : 48)), b ? 0x70 : 0x50);
    expect(p.loadBuiltinTuning(b ? 3 : 1), "fixture tuning publishes");
    auto t = tree(save(p));
    t.setProperty("presetKind", "user", nullptr);
    t.setProperty("presetName", b ? "Safety B" : "Safety A", nullptr);
    return binary(t);
}
static void callback(Processor& p, juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
#if defined(SWARAXT_HOST_RESTORE_RTSAN)
    [[clang::nonblocking]]
#endif
{
    p.processBlock(audio, midi);
}
bool sameAudio(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
std::vector<float> render(Processor& p)
{
    std::vector<float> output;
    juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
    midi.ensureSize(swaraxt::kMidiOutputReserveBytes);
    for (int block = 0; block != 16; ++block)
    {
        midi.clear();
        if (block == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
        if (block == 12) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 17);
        callback(p, audio, midi);
        for (int i = 0; i != audio.getNumSamples(); ++i)
        {
            const float value = audio.getSample(0, i);
            expect(std::isfinite(value), "rendered sample is finite");
            output.push_back(value);
        }
    }
    float peak = 0;
    for (const auto sample : output) peak = std::max(peak, std::abs(sample));
    expect(peak > 1.0e-6f, "state comparison renders audible audio");
    return output;
}
void unchanged(Processor& p, const juce::MemoryBlock& before)
{
    expect(tree(save(p)).isEquivalentTo(tree(before)), "rejected request preserves the complete normalized saved state");
    Processor fresh; restore(fresh, save(p));
    expect(tree(save(fresh)).isEquivalentTo(tree(before)), "save and reopen after rejection preserve that complete state");
}
void invalidAndEditor(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    Processor p; restore(p, a); p.prepareToPlay(48000, 256);
    const auto before = save(p);
    for (int kind = 0; kind != 4; ++kind)
    {
        auto t = tree(b);
        if (kind == 0) t.setProperty("stateVersion", Processor::kStateVersion + 1, nullptr);
        if (kind == 1) t = juce::ValueTree("OTHER_PLUGIN");
        if (kind == 2) t.setProperty("tuningTable", "3.@@@@", nullptr);
        auto request = kind == 3 ? juce::MemoryBlock("bad", 3) : binary(t);
        restore(p, request);
        expect(p.lastRestoreResult() == Result::invalid && p.restoreNotification().result == Result::invalid,
               "invalid binary/root/version/tuning is permanently rejected and observable");
        unchanged(p, before);
    }
    Processor reference; restore(reference, before); reference.prepareToPlay(48000, 256);
    expect(sameAudio(render(p), render(reference)), "rejected restores leave audio byte-identical to state A");
    const auto first = p.restoreNotification();
    {
        Editor editor(p);
        expect(editor.restoreNotificationVisibleForTests()
                   && editor.restoreNotificationTextForTests().contains("not loaded"),
               "failure recorded with editor closed is immediately visible on open");
    }
    {
        Editor editor(p);
        expect(editor.restoreNotificationVisibleForTests(), "closing/reopening the editor does not acknowledge failure");
        restore(p, juce::MemoryBlock("bad", 3));
        p.acknowledgeRestoreNotification(first);
        expect(p.restoreNotification().result == Result::invalid, "stale acknowledgement cannot clear a newer same-category failure");
        editor.refreshRestoreNotificationForTests();
        editor.dismissRestoreNotificationForTests();
        expect(!editor.restoreNotificationVisibleForTests(), "explicit dismissal acknowledges exactly the displayed failure");
    }
    restore(p, juce::MemoryBlock("bad", 3));
    restore(p, b);
    expect(p.restoreNotification().result == Result::success && tree(save(p)).isEquivalentTo(tree(b)),
           "a later complete valid restore resolves the previous failure without changing serialization");
    Editor editor(p);
    expect(!editor.restoreNotificationVisibleForTests(), "successful restores produce no warning spam");
}
struct TuningPause {
    juce::WaitableEvent entered, release;
    std::atomic<bool> first { true }, timedOut { false };
    static void hook(void* context, Engine::TuningPoint point, int) noexcept
    {
        auto& self = *static_cast<TuningPause*>(context);
        if (point != Engine::TuningPoint::writerSelected || !self.first.exchange(false)) return;
        self.entered.signal();
        if (!self.release.wait(15000)) self.timedOut.store(true);
    }
};
void tuningRejection(const juce::MemoryBlock& a, const juce::MemoryBlock& b, bool busy)
{
    Processor p; restore(p, a); p.prepareToPlay(48000, 256);
    auto& engine = p.engineForTests();
    const swaraxt::tuning::PublishedTuning* pinned = nullptr;
    int first = -1, second = -1;
    TuningPause pause; std::thread writer; bool wrote = false;
    if (busy)
    {
        engine.setTuningHook(TuningPause::hook, &pause);
        writer = std::thread([&] { wrote = p.loadBuiltinTuning(2); });
        expect(pause.entered.wait(3000), "independent tuning writer owns its reservation");
    }
    else
    {
        expect(engine.pinTuningForTests(pinned, first), "first snapshot claim pins its tuning");
        expect(p.loadBuiltinTuning(2), "second tuning publishes");
        expect(engine.pinTuningForTests(pinned, second), "second snapshot claim pins its tuning");
        {
            Engine::TuningReservation unavailable;
            expect(engine.reserveBuiltinTuning(0, unavailable) == Engine::TuningResult::unavailable,
                   "two snapshot claims and the startup audio latch occupy all slots");
        }
    }
    const auto before = save(p);
    restore(p, b);
    const auto reason = busy ? Result::tuningBusy : Result::tuningUnavailable;
    expect(p.lastRestoreResult() == reason && p.restoreNotification().result == reason,
           "reservation rejection retains its exact transient reason and persistent notice");
    unchanged(p, before);
    Processor reference; restore(reference, before); reference.prepareToPlay(48000, 256);
    expect(sameAudio(render(p), render(reference)), "audio after tuning rejection is exactly the retained A cohort");
    if (busy)
    {
        pause.release.signal(); writer.join(); engine.setTuningHook(nullptr, nullptr);
        expect(wrote && !pause.timedOut.load(), "paused independent writer completes without a watchdog timeout");
    }
    else { engine.unpinTuningForTests(first); engine.unpinTuningForTests(second); }
    restore(p, b);
    expect(p.restoreNotification().result == Result::success && tree(save(p)).isEquivalentTo(tree(b)),
           "explicit retry after contention resolves the notice and restores all of B");
}
void nestedRequest(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    Processor p; restore(p, a); p.prepareToPlay(48000, 256);
    struct Listener final : juce::AudioProcessorValueTreeState::Listener {
        Processor& p; const juce::MemoryBlock& incoming; bool called = false;
        Listener(Processor& processor, const juce::MemoryBlock& data) : p(processor), incoming(data) {}
        void parameterChanged(const juce::String&, float) override
        {
            if (called) return; called = true;
            restore(p, incoming);
            expect(p.lastRestoreResult() == Result::reentrant, "nested host request reports reentrant rejection immediately");
        }
    } listener(p, a);
    p.getApvts().addParameterListener(swaraxt::IDs::osc1Shape, &listener);
    restore(p, b);
    p.getApvts().removeParameterListener(swaraxt::IDs::osc1Shape, &listener);
    expect(listener.called && tree(save(p)).isEquivalentTo(tree(b)), "outer transaction still commits a complete B");
    expect(p.restoreNotification().result == Result::reentrant,
           "older outer success cannot hide the newer rejected host request");
    restore(p, b);
    expect(p.restoreNotification().result == Result::success, "subsequent nonreentrant retry resolves the failure");
}
void preparationAndFallback(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    Processor p; restore(p, a); p.prepareToPlay(48000, 256);
    const auto before = save(p);
    struct Failure {
        bool hit = false;
        static void hook(void* context, Processor::StatePublicationPoint point)
        {
            auto& self = *static_cast<Failure*>(context);
            if (point == Processor::StatePublicationPoint::restoreGateLocked)
            {
                self.hit = true;
                throw std::bad_alloc();
            }
        }
    } fault;
    p.setStatePublicationHook(Failure::hook, &fault);
    restore(p, b);
    p.setStatePublicationHook(nullptr, nullptr);
    expect(fault.hit && p.lastRestoreResult() == Result::preparationFailed
               && p.restoreNotification().result == Result::preparationFailed,
           "injected preparation failure is classified before B mutation");
    unchanged(p, before);
    {
        Editor editor(p);
        expect(editor.restoreNotificationTextForTests().contains("memory"),
               "resource failure has an accurate nonmodal recovery message");
    }
    auto fallback = tree(b);
    const juce::MemoryBlock badTuning("bad", 3);
    fallback.setProperty("tuningTable", badTuning.toBase64Encoding(), nullptr);
    restore(p, binary(fallback));
    expect(p.lastRestoreResult() == Result::defaultTuning
               && p.restoreNotification().result == Result::defaultTuning
               && p.activeTuningName() == "12-EDO",
           "legacy invalid-SWT fallback is preserved and its changed tuning is observable");
    {
        Editor editor(p);
        expect(editor.restoreNotificationTextForTests().contains("Standard tuning"),
               "fallback message states which tuning is actually in use");
    }
    restore(p, b);
    expect(tree(save(p)).isEquivalentTo(tree(b)) && p.restoreNotification().result == Result::success,
           "a complete B retry resolves fallback/resource warnings");
}
void retainedProgramState(const juce::MemoryBlock& a, int order)
{
    Processor p;
    if (order == 0) { restore(p, a); p.prepareToPlay(48000, 256); }
    else { p.prepareToPlay(48000, 256); restore(p, a); }
    if (order == 2) { p.releaseResources(); p.prepareToPlay(48000, 256); }
    const auto before = save(p);
    for (int program : { 0, 2, 3, 0 }) p.setCurrentProgram(program);
    expect(tree(save(p)).isEquivalentTo(tree(before)),
           "ambiguous host reassertions cannot replace a restored sound or rename its metadata");
    expect(p.getCurrentProgram() == 1 && p.currentPresetIsUser() && p.currentPresetName() == "Safety A"
               && p.getProgramName(p.getCurrentProgram()) == "Pulse Lead",
           "reported source program and custom preset identity still describe retained A");
    Processor reference;
    if (order == 0) { restore(reference, a); reference.prepareToPlay(48000, 256); }
    else { reference.prepareToPlay(48000, 256); restore(reference, a); }
    if (order == 2) { reference.releaseResources(); reference.prepareToPlay(48000, 256); }
    expect(sameAudio(render(p), render(reference)), "callback ordering/reassertions retain byte-identical A audio");
    unchanged(p, before);
    Editor editor(p);
    expect(!editor.restoreNotificationVisibleForTests(), "ordinary successful restore/reassertion produces no failure warning");
}
void explicitProgramContext(const juce::MemoryBlock& a, bool locked)
{
    Processor p; p.prepareToPlay(48000, 256); restore(p, a);
    p.setSequenceLocked(locked);
    const auto sequence = p.sequenceState().toValueTree();
    const auto tuning = p.engineForTests().tuningBytesForState();
    juce::String error;
    expect(p.loadPresetEntry({ {}, {}, 1, true }, error), "explicit UI selection reloads even the restored source program");
    std::printf("Explicit factory locked=%d program=%d user=%d name=%s cutoff=%.9g timbre=%.9g\n",
                locked, p.getCurrentProgram(), p.currentPresetIsUser(), p.currentPresetName().toRawUTF8(),
                static_cast<double>(p.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load()),
                static_cast<double>(p.getApvts().getRawParameterValue(swaraxt::IDs::osc1Param)->load()));
    expect(p.getCurrentProgram() == 1 && !p.currentPresetIsUser() && p.currentPresetName() == "Pulse Lead"
               && std::abs(p.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load() - 3500.0f) < 0.1f
               && p.getApvts().getRawParameterValue(swaraxt::IDs::osc1Param)->load() == 64.0f,
           "reliable explicit selection applies the requested factory sound and its metadata");
    expect(p.engineForTests().tuningBytesForState() == tuning, "factory selection preserves established independent tuning semantics");
    swaraxt::SequenceState defaults;
    expect(p.sequenceState().toValueTree().isEquivalentTo(locked ? sequence : defaults.toValueTree()),
           "factory selection preserves locked sequence or resets the complete unlocked sequence");
    p.setCurrentProgram(2);
    std::printf("Released guard program=%d cutoff=%.9g\n", p.getCurrentProgram(),
                static_cast<double>(p.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load()));
    expect(p.getCurrentProgram() == 2 && p.currentPresetName() == "Wavetable Tone"
               && std::abs(p.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load() - 5000.0f) < 0.1f,
           "host program selection operates normally after explicit replacement clears the session guard");
    const auto after = save(p);
    unchanged(p, after);
    Processor reference; reference.prepareToPlay(48000, 256); restore(reference, after);
    expect(sameAudio(render(p), render(reference)), "explicit factory selection and fresh reopen render the same exact audio");
}
void invalidProgramAndUndo(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    Processor fresh; fresh.prepareToPlay(48000, 256);
    restore(fresh, juce::MemoryBlock("bad", 3)); fresh.setCurrentProgram(1);
    std::printf("Invalid initial restore program=%d cutoff=%.9g notice=%d\n", fresh.getCurrentProgram(),
                static_cast<double>(fresh.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load()),
                static_cast<int>(fresh.restoreNotification().result));
    expect(fresh.getCurrentProgram() == 1
               && std::abs(fresh.getApvts().getRawParameterValue(swaraxt::IDs::filterCutoff)->load() - 3500.0f) < 0.1f
               && fresh.restoreNotification().result == Result::invalid,
           "invalid initial restore leaves ordinary program operation available and its failure observable");
    Processor p; p.prepareToPlay(48000, 256); restore(p, a);
    restore(p, juce::MemoryBlock("bad", 3)); p.setCurrentProgram(0);
    expect(tree(save(p)).isEquivalentTo(tree(a)), "invalid later restore cannot disarm protection of existing customized A");
    for (const auto* wanted : { &b, &a, &b, &a })
    {
        restore(p, *wanted); p.setCurrentProgram(0);
        expect(tree(save(p)).isEquivalentTo(tree(*wanted)), "live host undo/redo preserves each complete generation and its metadata");
    }
    Processor reference; reference.prepareToPlay(48000, 256); restore(reference, a);
    expect(sameAudio(render(p), render(reference)), "live undo/redo ends at byte-identical A audio");
}
void notificationConcurrency()
{
    Processor p;
    const juce::MemoryBlock invalid("bad", 3);
    restore(p, invalid); const auto old = p.restoreNotification();
    std::thread producer([&] { for (int i = 0; i != 100; ++i) restore(p, invalid); });
    for (int i = 0; i != 100; ++i)
    {
        const auto snapshot = p.restoreNotification();
        expect(snapshot.result == Result::invalid && snapshot.revision >= old.revision,
               "cross-thread UI observations contain one complete notification");
    }
    producer.join();
    p.acknowledgeRestoreNotification(old);
    expect(p.restoreNotification().result == Result::invalid
               && p.restoreNotification().revision == old.revision + 100,
           "concurrent repeated failures remain visible after stale UI acknowledgement");
    p.acknowledgeRestoreNotification(p.restoreNotification());
    expect(p.restoreNotification().result == Result::success, "current acknowledgement clears the completed notice");
}
int faultCase(const juce::String& phase)
{
    const auto a = fixture(false), b = fixture(true);
    Processor p; restore(p, a); p.prepareToPlay(48000, 256);
    const auto before = save(p);
    std::vector<float> values;
    for (auto* parameter : p.getParameters()) values.push_back(parameter->getValue());
    const auto sequence = p.sequenceState().toValueTree();
    const auto tuning = p.engineForTests().tuningBytesForState();
    struct ParameterFailure final : juce::AudioProcessorValueTreeState::Listener {
        bool hit = false;
        void parameterChanged(const juce::String&, float) override { hit = true; throw std::bad_alloc(); }
    } parameterFailure;
    struct SequenceFailure final : swaraxt::SequenceState::Listener {
        bool hit = false;
        void sequenceStateChanged() override { hit = true; throw std::bad_alloc(); }
    } sequenceFailure;
    struct FailurePoint {
        juce::String phase;
        bool hit = false;
        static void hook(void* context, Processor::StatePublicationPoint point)
        {
            auto& self = *static_cast<FailurePoint*>(context);
            if (self.hit) return;
            using Point = Processor::StatePublicationPoint;
            if ((self.phase == "fault-tuning" && point == Point::restoreGateLocked)
                || (self.phase == "fault-snapshot" && point == Point::restorePrepared)
                || (self.phase == "fault-post" && point == Point::restoreBeforeSequence))
            {
                self.hit = true;
                failNextAllocation = true;
            }
            if (self.phase == "fault-metadata" && point == Point::restoreBeforeTuningPublish)
            {
                self.hit = true;
                throw std::bad_alloc();
            }
        }
    } point { phase };
    if (phase == "fault-parameter")
        p.getApvts().addParameterListener(swaraxt::IDs::osc1Shape, &parameterFailure);
    if (phase == "fault-sequence") p.sequenceState().addListener(&sequenceFailure);
    p.setStatePublicationHook(FailurePoint::hook, &point);
    bool escaped = false, injected = false;
    const auto attempt = [&] {
        allocationInjected = false;
        if (phase == "fault-decode") failNextAllocation = true;
        try { restore(p, b); }
        catch (const std::bad_alloc&) { escaped = true; }
        injected = allocationInjected || parameterFailure.hit || sequenceFailure.hit
            || (phase == "fault-metadata" && point.hit);
        failNextAllocation = false;
    };
    if (phase == "fault-post") { std::thread worker(attempt); worker.join(); }
    else attempt();
    p.setStatePublicationHook(nullptr, nullptr);
    p.getApvts().removeParameterListener(swaraxt::IDs::osc1Shape, &parameterFailure);
    p.sequenceState().removeListener(&sequenceFailure);
    const auto after = save(p);
    const bool completeA = tree(after).isEquivalentTo(tree(before));
    const bool completeB = tree(after).isEquivalentTo(tree(b));
    int changed = 0;
    std::size_t index = 0;
    for (auto* parameter : p.getParameters()) changed += parameter->getValue() != values[index++];
    std::printf("RESTORE_FAULT phase=%s injected=%d escaped=%d outcome=%d changedParameters=%d completeA=%d completeB=%d sequenceA=%d tuningA=%d program=%d name=%s\n",
                phase.toRawUTF8(), injected, escaped, static_cast<int>(p.lastRestoreResult()), changed,
                completeA, completeB, sequence.isEquivalentTo(p.sequenceState().toValueTree()),
                tuning == p.engineForTests().tuningBytesForState(), p.getCurrentProgram(),
                p.currentPresetName().toRawUTF8());
    expect(injected, "the selected allocation/listener failure was injected");
    expect(!escaped, "restoration must contain the exceptional failure at the processor callback");
    expect(completeA || completeB, "exceptional restoration must leave one complete saved generation");
    const bool preparation = phase == "fault-decode" || phase == "fault-tuning" || phase == "fault-snapshot";
    const auto outcome = preparation ? Result::preparationFailed : Result::incomplete;
    expect(p.lastRestoreResult() == outcome && p.restoreNotification().result == outcome,
           "failure retains its truthful preparation or uncertain-state category");
    if (preparation)
    {
        expect(completeA, "preparation failure preserves complete A");
        Processor reference; restore(reference, before); reference.prepareToPlay(48000, 256);
        expect(sameAudio(render(p), render(reference)), "preparation failure retains byte-identical A audio");
    }
    else render(p);
    Processor reopened; restore(reopened, after);
    expect(tree(save(reopened)).isEquivalentTo(tree(after)), "host save/reopen exposes exactly the post-failure saved state");
    struct Recovery final : swaraxt::SequenceState::Listener {
        int calls = 0;
        void sequenceStateChanged() override { ++calls; }
    } recovery;
    p.sequenceState().addListener(&recovery);
    restore(p, b);
    p.sequenceState().removeListener(&recovery);
    expect(tree(save(p)).isEquivalentTo(tree(b)) && p.lastRestoreResult() == Result::success,
           "writer gates and tuning ownership admit a later complete B retry");
    expect(recovery.calls > 0, "a valid retry can deliver sequence notifications after failure");
    render(p);
    std::printf("RESTORE_FAULT_RETRY phase=%s completeB=%d sequenceNotifications=%d\n",
                phase.toRawUTF8(), tree(save(p)).isEquivalentTo(tree(b)), recovery.calls);
    return failures ? 1 : 0;
}

}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc == 2) return faultCase(argv[1]);
    if (argc != 1) return 2;
    const auto a = fixture(false), b = fixture(true);
    invalidAndEditor(a, b);
    tuningRejection(a, b, true);
    tuningRejection(a, b, false);
    nestedRequest(a, b);
    preparationAndFallback(a, b);
    notificationConcurrency();
    retainedProgramState(a, 0);
    retainedProgramState(a, 1);
    retainedProgramState(a, 2);
    explicitProgramContext(a, false);
    explicitProgramContext(a, true);
    invalidProgramAndUndo(a, b);
    std::printf("Host state safety tests: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
