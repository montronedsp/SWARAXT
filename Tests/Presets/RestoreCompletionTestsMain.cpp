// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#define main originalMission2SafetyMain
#include "HostStateSafetyTestsMain.cpp"
#undef main
#include <stdexcept>

namespace {
juce::MemoryBlock transactionFixture(bool b)
{
    Processor p;
    restore(p, fixture(b));
    p.sequenceState().setRotation(b ? 3 : 1);
    p.sequenceState().setGrooveTemplate(b ? 4 : 2);
    set(p, swaraxt::IDs::arpPattern, b ? 15.0f : 5.0f);
    p.sequenceState().setFreePitch(0, swaraxt::FreeCvPitch::fromMidiNote(b ? 83 : 49));
    p.sequenceState().setScaleDegree(0, b ? 7 : -3);
    p.setFilterQuality(b ? swaraxt::FilterQuality::eco : swaraxt::FilterQuality::high);
    p.setSequenceLocked(b);
    p.setRandomNoteRange(b ? swaraxt::SequenceState::NoteRange { -24, 7 }
                          : swaraxt::SequenceState::NoteRange { -7, 18 });
    return save(p);
}
void completeCohort(Processor& p, Processor& reference)
{
    expect(tree(save(p)).isEquivalentTo(tree(save(reference))), "complete saved cohort matches the reference");
    const auto& expected = reference.getParameters();
    int index = 0;
    for (auto* value : p.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(value);
        auto* other = dynamic_cast<juce::RangedAudioParameter*>(expected[index++]);
        expect(ranged && other && ranged->paramID == other->paramID, "parameter schema is unchanged");
        if (!ranged || !other) continue;
        expect(ranged->getValue() == other->getValue(), "actual ranged parameter belongs to the complete cohort");
        expect(p.getApvts().getRawParameterValue(ranged->paramID)->load()
                   == reference.getApvts().getRawParameterValue(other->paramID)->load(),
               "APVTS raw atomic belongs to the same cohort");
        const auto actualChild = p.getApvts().state.getChildWithProperty("id", ranged->paramID);
        const auto expectedChild = reference.getApvts().state.getChildWithProperty("id", other->paramID);
        expect(actualChild.isValid() && expectedChild.isValid()
                   && actualChild["value"] == expectedChild["value"],
               "live APVTS child agrees without projecting a saved preimage");
    }
    expect(p.sequenceState().toValueTree().isEquivalentTo(reference.sequenceState().toValueTree()),
           "complete sequence including ARP/CV/degree fields matches");
    expect(p.engineForTests().tuningBytesForState() == reference.engineForTests().tuningBytesForState()
               && p.activeTuningName() == reference.activeTuningName(), "complete tuning matches");
    const auto actualRange = p.randomNoteRange(), expectedRange = reference.randomNoteRange();
    expect(p.filterQuality() == reference.filterQuality() && p.sequenceLocked() == reference.sequenceLocked()
               && actualRange.lower == expectedRange.lower && actualRange.upper == expectedRange.upper,
           "quality, sequence lock and random range share the committed cohort");
    expect(p.getCurrentProgram() == reference.getCurrentProgram()
               && p.currentPresetIsUser() == reference.currentPresetIsUser()
               && p.currentPresetName() == reference.currentPresetName(), "program and user metadata match");
}
void automationAfterFailure(Processor& p)
{
    set(p, swaraxt::IDs::osc1Param, 36.0f);
    set(p, swaraxt::IDs::osc1Shape, 1.0f);
    set(p, swaraxt::IDs::osc2Shape, 1.0f);
    const auto saved = tree(save(p));
    expect(static_cast<int>(saved.getChildWithProperty("id", swaraxt::IDs::osc1Param)["value"]) == 36,
           "automation after failure flushes through the current adapter tree");
    expect(static_cast<int>(saved.getChildWithProperty("id", swaraxt::IDs::osc1Shape)["value"]) == 1
               && static_cast<int>(saved.getChildWithProperty("id", swaraxt::IDs::osc2Shape)["value"]) == 1,
           "automation reaches both the interrupted adapter and later adapters");
    Processor reopened; restore(reopened, save(p));
    expect(reopened.getApvts().getRawParameterValue(swaraxt::IDs::osc1Param)->load() == 36.0f,
           "later automation survives save and reopen");
}
struct ParameterFailure final : juce::AudioProcessorValueTreeState::Listener {
    int calls = 0;
    void parameterChanged(const juce::String&, float) override { ++calls; throw std::bad_alloc(); }
};
struct PreemptingFailure final : juce::AudioProcessorParameter::Listener {
    int calls = 0;
    void parameterValueChanged(int, float) override { ++calls; throw std::bad_alloc(); }
    void parameterGestureChanged(int, bool) override {}
};
struct RootFailure final : juce::ValueTree::Listener {
    int calls = 0;
    void valueTreeRedirected(juce::ValueTree&) override { ++calls; throw std::runtime_error("controlled root notification"); }
};
struct NestedNotification final : juce::ValueTree::Listener {
    juce::Identifier property { "completionProbe" };
    int depth = 0, maximumDepth = 0;
    void valueTreeRedirected(juce::ValueTree& value) override { value.sendPropertyChangeMessage(property); }
    void valueTreePropertyChanged(juce::ValueTree& value, const juce::Identifier&) override
    {
        if (depth == 16) return;
        ++depth; maximumDepth = std::max(maximumDepth, depth);
        value.sendPropertyChangeMessage(property);
        --depth;
    }
};
void notificationFailure(const juce::String& mode)
{
    const auto a = transactionFixture(false), b = transactionFixture(true);
    Processor p, reference;
    restore(p, a); p.prepareToPlay(48000, 256);
    restore(reference, a); reference.prepareToPlay(48000, 256); restore(reference, b);
    ParameterFailure parameter;
    PreemptingFailure preempt;
    RootFailure root;
    NestedNotification nested;
    std::vector<juce::String> ids;
    if (mode == "listeners")
        for (auto* value : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(value))
            {
                ids.push_back(ranged->paramID);
                p.getApvts().addParameterListener(ranged->paramID, &parameter);
            }
    auto* shape = p.getApvts().getParameter(swaraxt::IDs::osc1Shape);
    if (mode == "preempt") shape->addListener(&preempt);
    if (mode == "root") p.getApvts().state.addListener(&root);
    struct Arm {
        static void hook(void*, Processor::StatePublicationPoint point)
        {
            if (point == Processor::StatePublicationPoint::bulkEpochPublished) failNextAllocation = true;
        }
    };
    if (mode == "allocation")
    {
        p.getApvts().state.addListener(&nested);
        p.setStatePublicationHook(Arm::hook, nullptr);
    }
    allocationInjected = false;
    bool escaped = false;
    const auto begin = juce::Time::getMillisecondCounterHiRes();
    try { restore(p, b); } catch (...) { escaped = true; }
    const auto duration = juce::Time::getMillisecondCounterHiRes() - begin;
    const bool injected = mode == "allocation" ? allocationInjected
        : mode == "listeners" ? parameter.calls > 0
        : mode == "preempt" ? preempt.calls > 0 : root.calls > 0;
    failNextAllocation = false;
    p.setStatePublicationHook(nullptr, nullptr);
    for (const auto& id : ids) p.getApvts().removeParameterListener(id, &parameter);
    shape->removeListener(&preempt);
    p.getApvts().state.removeListener(&root);
    p.getApvts().state.removeListener(&nested);
    expect(injected && !escaped, "real selected failure is injected and contained");
    expect(p.lastRestoreResult() == Result::incomplete && p.restoreNotification().result == Result::incomplete,
           "completed data with failed notification does not report success");
    {
        Editor editor(p);
        expect(editor.restoreNotificationVisibleForTests()
                   && editor.restoreNotificationTextForTests().contains("sound was loaded")
                   && editor.restoreNotificationTextForTests().contains("Keep the original"),
               "persistent warning describes completed data and failed controls truthfully");
    }
    completeCohort(p, reference);
    expect(sameAudio(render(p), render(reference)), "audio after failure is byte-identical to complete B");
    automationAfterFailure(p);
    restore(p, b);
    expect(p.lastRestoreResult() == Result::success && p.restoreNotification().result == Result::success,
           "explicit valid retry completes notification and resolves the failure");
    completeCohort(p, reference);
    std::printf("COMPLETION mode=%s injected=%d escaped=%d callbacks=%d nestedDepth=%d milliseconds=%.3f\n",
                mode.toRawUTF8(), injected, escaped, parameter.calls + preempt.calls + root.calls,
                nested.maximumDepth, duration);
}
void admissionDuringFailure()
{
    const auto a = transactionFixture(false), b = transactionFixture(true);
    Processor p, reference;
    restore(p, a); p.prepareToPlay(48000, 256);
    restore(reference, a); reference.prepareToPlay(48000, 256);
    struct Pause {
        juce::WaitableEvent entered, release;
        bool expired = false;
        static void hook(void* context, Processor::StatePublicationPoint point)
        {
            if (point != Processor::StatePublicationPoint::restoreBeforeTuningPublish) return;
            auto& self = *static_cast<Pause*>(context);
            self.entered.signal();
            self.expired = !self.release.wait(10000);
            throw std::bad_alloc();
        }
    } pause;
    p.setStatePublicationHook(Pause::hook, &pause);
    bool escaped = false;
    std::thread writer([&] { try { restore(p, b); } catch (...) { escaped = true; } });
    expect(pause.entered.wait(3000), "writer pauses with the prepared transaction active");
    const auto begin = juce::Time::getMillisecondCounterHiRes();
    const auto during = render(p);
    const auto duration = juce::Time::getMillisecondCounterHiRes() - begin;
    const auto expected = render(reference);
    const auto duringState = save(p);
    pause.release.signal(); writer.join();
    p.setStatePublicationHook(nullptr, nullptr);
    expect(!escaped && !pause.expired, "failing writer finishes without escape or watchdog expiry");
    expect(sameAudio(during, expected) && tree(duringState).isEquivalentTo(tree(a)),
           "audio and reentrant save retain complete A while the writer owns admission");
    restore(reference, b);
    completeCohort(p, reference);
    expect(sameAudio(render(p), render(reference)), "after failed notification all of B enters audio together");
    expect(p.lastRestoreResult() == Result::incomplete, "metadata-stage failure remains observable");
    std::printf("COMPLETION admission blocked-writer-audio-ms=%.3f\n", duration);
}
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc == 1)
    {
        for (const auto* mode : { "listeners", "preempt", "root", "allocation" }) notificationFailure(mode);
        admissionDuringFailure();
        return failures ? 1 : 0;
    }
    if (argc != 2) return 2;
    const juce::String mode(argv[1]);
    if (mode == "admission") admissionDuringFailure();
    else if (mode == "listeners" || mode == "preempt" || mode == "root" || mode == "allocation") notificationFailure(mode);
    else return 2;
    return failures ? 1 : 0;
}
