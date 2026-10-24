// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using Processor = SwaraXtAudioProcessor;
using Point = Processor::StatePublicationPoint;
int failures = 0;

void expect(bool condition, const char* message)
{
    if (! condition) { std::printf("FAIL: %s\n", message); ++failures; }
}

void setInt(Processor& processor, const char* id, int value)
{
    auto* parameter = processor.getApvts().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

juce::MemoryBlock save(Processor& processor)
{
    juce::MemoryBlock result;
    processor.getStateInformation(result);
    return result;
}

juce::ValueTree decode(const juce::MemoryBlock& data)
{
    const auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize()));
    return xml != nullptr ? juce::ValueTree::fromXml(*xml) : juce::ValueTree {};
}

int parameter(const juce::ValueTree& state, const char* id)
{
    return static_cast<int>(state.getChildWithProperty("id", id).getProperty("value", -999));
}

struct PresetFiles {
    juce::File directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("swaraxt-state-" + juce::Uuid().toString());
    PresetFiles() { expect(directory.createDirectory().wasOk(), "isolated preset directory is created"); }
    ~PresetFiles()
    {
        for (const auto* name : { "Previous", "SavedA", "SavedB", "Imported" })
            file(name).deleteFile();
        directory.deleteFile();
    }
    juce::File file(const char* name) const { return directory.getChildFile(juce::String(name) + ".swaraxtpreset"); }
};

void serializationReentry(bool throwDuringCapture)
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    struct Observer final : juce::ValueTree::Listener {
        Processor& processor;
        bool throwing;
        bool called = false;
        juce::MemoryBlock captured;
        Observer(Processor& p, bool shouldThrow) : processor(p), throwing(shouldThrow) {}
        void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override
        {
            if (called) return;
            called = true;
            if (throwing) throw std::runtime_error("controlled snapshot failure");
            captured = save(processor);
        }
    } observer(processor, throwDuringCapture);
    auto state = processor.getApvts().state;
    state.addListener(&observer);
    bool caught = false;
    try { processor.setCurrentProgram(1); }
    catch (const std::runtime_error&) { caught = true; }
    state.removeListener(&observer);
    expect(observer.called, "pre-transaction serialization invokes the controlled ValueTree observer");
    if (throwDuringCapture)
    {
        expect(caught && parameter(decode(save(processor)), swaraxt::IDs::osc1Param) == 73,
               "failed entry serialization preserves B and releases writer ownership");
        processor.setCurrentProgram(1);
        expect(processor.getCurrentProgram() == 1, "a writer can enter after serialization throws");
    }
    else
    {
        expect(! caught && parameter(decode(observer.captured), swaraxt::IDs::osc1Param) == 73,
               "reentrant save during copyState flush returns B without recursion failure");
    }
}

void reentrantSavesAndMetadata()
{
    PresetFiles files;
    Processor processor;
    processor.prepareToPlay(48000, 64);
    processor.setUserPresetDirectoryForTests(files.directory);
    setInt(processor, swaraxt::IDs::osc1Shape, 10);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    juce::String error;
    expect(processor.saveUserPreset("Previous", true, error), "previous user generation is established");
    const auto previous = save(processor);
    struct Observer final : juce::AudioProcessorValueTreeState::Listener {
        Processor& processor;
        const juce::MemoryBlock& previous;
        bool called = false;
        explicit Observer(Processor& p, const juce::MemoryBlock& state) : processor(p), previous(state) {}
        void parameterChanged(const juce::String&, float) override
        {
            if (called) return;
            called = true;
            const auto state = decode(save(processor));
            expect(parameter(state, swaraxt::IDs::osc1Param) == 73, "parameter-notification save returns B");
            expect(processor.getCurrentProgram() == 0 && processor.currentPresetIsUser()
                       && processor.currentPresetName() == "Previous"
                       && state["presetKind"] == "user" && state["presetName"] == "Previous",
                   "reentrant metadata and serialized metadata describe the same committed generation");
            std::vector<float> values;
            for (auto* value : processor.getParameters()) values.push_back(value->getValue());
            const auto sequence = processor.sequenceState().toValueTree();
            const auto tuning = processor.engineForTests().tuningBytesForState();
            processor.setCurrentProgram(2);
            processor.setStateInformation(previous.getData(), static_cast<int>(previous.getSize()));
            juce::String nestedError;
            expect(! processor.loadPresetEntry({ "Nested", {}, 2, true }, nestedError) && nestedError.isNotEmpty(),
                   "nested writers are rejected without waiting on their own state gate");
            expect(! processor.saveUserPreset("SavedA", true, nestedError) && nestedError.isNotEmpty(),
                   "nested preset saving is rejected before file publication");
            size_t index = 0;
            for (auto* value : processor.getParameters())
            {
                const float actual = value->getValue();
                expect(std::memcmp(&actual, &values[index++], sizeof(float)) == 0,
                       "rejected mutations leave live parameter values untouched");
            }
            expect(processor.sequenceState().toValueTree().isEquivalentTo(sequence)
                       && processor.engineForTests().tuningBytesForState() == tuning,
                   "rejected mutations leave live sequence and tuning untouched");
            expect(parameter(decode(save(processor)), swaraxt::IDs::osc1Param) == 73,
                   "rejected nested writers do not replace the preimage");
        }
    } observer(processor, previous);
    processor.getApvts().addParameterListener(swaraxt::IDs::osc1Shape, &observer);
    processor.setCurrentProgram(1);
    processor.getApvts().removeParameterListener(swaraxt::IDs::osc1Shape, &observer);
    expect(observer.called && processor.getCurrentProgram() == 1 && ! processor.currentPresetIsUser(),
           "the outer factory transaction completes after reentrant reads and rejected nested writers");
    Processor reference;
    reference.setStateInformation(previous.getData(), static_cast<int>(previous.getSize()));
    reference.prepareToPlay(48000, 64);
    reference.setCurrentProgram(1);
    expect(decode(save(processor)).isEquivalentTo(decode(save(reference))) && ! files.file("SavedA").exists(),
           "recursive rejection leaves the same complete final state as the non-reentrant transaction");
}

void userPresetCompletion(bool failFileWrite, bool unchangedProgram = false)
{
    PresetFiles files;
    Processor processor;
    processor.prepareToPlay(48000, 64);
    processor.setUserPresetDirectoryForTests(files.directory);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    struct Pause {
        juce::WaitableEvent entered, release;
        std::atomic<bool> timedOut { false };
        static void hook(void* context, Point point)
        {
            if (point != Point::userPresetFileReady) return;
            auto& self = *static_cast<Pause*>(context);
            self.entered.signal();
            if (! self.release.wait(5000)) self.timedOut.store(true);
        }
    } pause;
    processor.setStatePublicationHook(Pause::hook, &pause);
    bool success = false;
    juce::String error;
    std::thread saver([&] { success = processor.saveUserPreset("SavedB", true, error); });
    expect(pause.entered.wait(3000), "user-preset file phase is reached outside writer admission");
    processor.setCurrentProgram(unchangedProgram ? 0 : 1);
    if (failFileWrite)
    {
        expect(files.file("SavedB").createDirectory().wasOk()
                   && files.file("SavedB").getChildFile("blocked").replaceWithText("fixture"),
               "controlled rename failure creates only an isolated nonempty target directory");
    }
    pause.release.signal();
    saver.join();
    processor.setStatePublicationHook(nullptr, nullptr);
    expect(! pause.timedOut.load(), "preset file barrier completes within its bound");
    if (unchangedProgram)
        expect(processor.getCurrentProgram() == 0 && processor.currentPresetIsUser()
                   && processor.currentPresetName() == "SavedB",
               "an unchanged host program reassertion does not invalidate a successful save");
    else
        expect(processor.getCurrentProgram() == 1 && ! processor.currentPresetIsUser()
                   && processor.currentPresetName() == processor.getProgramName(1),
               "late file completion or failure cannot rename or roll back a newer factory generation");
    expect(success != failFileWrite, "controlled file write has the expected outcome");
    if (! failFileWrite)
    {
        juce::MemoryBlock data;
        expect(files.file("SavedB").loadFileAsData(data), "saved B file is readable");
        const auto state = decode(data);
        expect(parameter(state, swaraxt::IDs::osc1Param) == 73
                   && state["presetKind"] == "user" && state["presetName"] == "SavedB",
               "saved file contains its own coherent B sound and user metadata");
    }
    else
    {
        expect(files.file("SavedB").getChildFile("blocked").deleteFile()
                   && files.file("SavedB").deleteFile(), "failed-write fixture is removed");
    }
}

void overlappingUserPresetSaves(bool newestCompletesFirst)
{
    PresetFiles files;
    Processor processor;
    processor.setUserPresetDirectoryForTests(files.directory);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    struct Pause {
        juce::WaitableEvent entered[2], release[2];
        std::atomic<int> next { 0 };
        std::atomic<bool> timedOut { false };
        static void hook(void* context, Point point)
        {
            if (point != Point::userPresetFileReady) return;
            auto& self = *static_cast<Pause*>(context);
            const int index = self.next.fetch_add(1);
            if (index >= 2) { self.timedOut.store(true); return; }
            self.entered[index].signal();
            if (! self.release[index].wait(5000)) self.timedOut.store(true);
        }
    } pause;
    processor.setStatePublicationHook(Pause::hook, &pause);
    bool success[2] { false, false };
    juce::String error[2];
    std::thread older([&] { success[0] = processor.saveUserPreset("SavedA", true, error[0]); });
    expect(pause.entered[0].wait(3000), "older save reaches file phase");
    setInt(processor, swaraxt::IDs::osc1Param, 91);
    std::thread newer([&] { success[1] = processor.saveUserPreset("SavedB", true, error[1]); });
    expect(pause.entered[1].wait(3000), "newer save reaches file phase");
    const int first = newestCompletesFirst ? 1 : 0;
    pause.release[first].signal();
    if (first == 0) older.join(); else newer.join();
    pause.release[1 - first].signal();
    if (first == 0) newer.join(); else older.join();
    processor.setStatePublicationHook(nullptr, nullptr);
    expect(success[0] && success[1] && ! pause.timedOut.load(), "both controlled saves complete successfully");
    expect(processor.currentPresetName() == "SavedB" && processor.currentPresetIsUser(),
           "the newest save retains its metadata regardless of file completion order");
    for (int index = 0; index != 2; ++index)
    {
        juce::MemoryBlock data;
        expect(files.file(index == 0 ? "SavedA" : "SavedB").loadFileAsData(data), "overlapping save file is readable");
        expect(parameter(decode(data), swaraxt::IDs::osc1Param) == (index == 0 ? 73 : 91),
               "each overlapping save retains its own sound snapshot");
    }
}

void failedLoadsAndRapidSelections()
{
    PresetFiles files;
    Processor processor, reference;
    processor.setUserPresetDirectoryForTests(files.directory);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    juce::String error;
    expect(processor.saveUserPreset("Previous", true, error), "rapid-selection user fixture saves");
    const auto previous = decode(save(processor));
    expect(! processor.loadPresetEntry({ "Missing", files.file("SavedA"), -1, false }, error)
               && error.isNotEmpty() && decode(save(processor)).isEquivalentTo(previous),
           "a missing preset file fails without changing any serialized field");
    expect(files.file("Imported").replaceWithText("malformed fixture"), "malformed preset fixture writes");
    expect(! processor.loadPresetEntry({ "Malformed", files.file("Imported"), -1, false }, error)
               && error.isNotEmpty() && decode(save(processor)).isEquivalentTo(previous),
           "a malformed preset fails without changing any serialized field");
    auto future = previous.createCopy();
    future.setProperty("stateVersion", Processor::kStateVersion + 1, nullptr);
    juce::MemoryBlock data;
    const auto xml = future.createXml();
    juce::AudioProcessor::copyXmlToBinary(*xml, data);
    expect(files.file("Imported").replaceWithData(data.getData(), data.getSize()), "future-version fixture writes");
    expect(! processor.loadPresetEntry({ "Future", files.file("Imported"), -1, false }, error)
               && error.isNotEmpty() && decode(save(processor)).isEquivalentTo(previous),
           "an unsupported preset version fails without changing any serialized field");
    const Processor::PresetEntry choices[] {
        { "Factory one", {}, 1, true }, { "Previous", files.file("Previous"), -1, false },
        { "Factory two", {}, 2, true }
    };
    juce::ValueTree expected[3];
    for (int index = 0; index != 3; ++index)
    {
        expect(reference.loadPresetEntry(choices[index], error), "reference selection loads");
        expected[index] = decode(save(reference));
    }
    for (int index = 0; index != 12; ++index)
        expect(processor.loadPresetEntry(choices[index % 3], error)
                   && decode(save(processor)).isEquivalentTo(expected[index % 3]),
               "rapid user/factory selections preserve each complete sound and its metadata");
}

void lockedTuningAndSessionRoundTrip()
{
    PresetFiles files;
    Processor processor;
    processor.setUserPresetDirectoryForTests(files.directory);
    swaraxt::tuning::TuningSnapshot tuning;
    swaraxt::tuning::ParseError parse;
    const char* scl = "State imported tuning\n3\n400.0\n800.0\n1200.0\n";
    std::vector<uint8_t> bytes;
    expect(swaraxt::tuning::parseScalaAndKeyboard(scl, nullptr, tuning, parse)
               && swaraxt::tuning::writeTuningTable(tuning, bytes, parse), "imported tuning fixture is valid");
    expect(processor.loadTuningBytes(bytes.data(), bytes.size()), "imported tuning loads");
    processor.sequenceState().setStep(0, 0x80 | 67, 0x60);
    processor.setSequenceLocked(true);
    juce::String error;
    expect(processor.saveUserPreset("Imported", true, error), "imported tuning and sequence preset saves");
    const auto session = save(processor);
    processor.sequenceState().setStep(0, 0x80 | 72, 0x50);
    const auto protectedSequence = processor.sequenceState().snapshot().steps;
    expect(processor.loadBuiltinTuning(0), "protected replacement tuning loads");
    expect(processor.loadPresetEntry({ "Imported", files.file("Imported"), -1, false }, error),
           "locked musical preset loads");
    expect(processor.sequenceState().snapshot().steps == protectedSequence && processor.activeBuiltinTuning() == 0,
           "Sequence Lock preserves sequence and its current tuning across musical preset load");
    processor.setSequenceLocked(false);
    expect(processor.loadPresetEntry({ "Imported", files.file("Imported"), -1, false }, error),
           "unlocked musical preset loads");
    expect(processor.activeTuningName() == "State imported tuning" && processor.currentPresetIsUser()
               && processor.currentPresetName() == "Imported", "user sound, imported tuning and metadata commit together");
    Processor restored;
    restored.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    expect(restored.sequenceLocked() && restored.activeTuningName() == "State imported tuning"
               && restored.currentPresetName() == "Imported", "ordinary session restores lock, imported tuning and user name");
    expect(decode(save(restored)).isEquivalentTo(decode(session)), "session round-trip preserves all serialized fields");
}

void editorDuringPublication()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    struct Pause {
        juce::WaitableEvent entered, release;
        std::atomic<bool> timedOut { false };
        static void hook(void* context, Point point)
        {
            if (point != Point::bulkEpochPublished) return;
            auto& self = *static_cast<Pause*>(context);
            self.entered.signal();
            if (! self.release.wait(5000)) self.timedOut.store(true);
        }
    } pause;
    processor.setStatePublicationHook(Pause::hook, &pause);
    std::thread writer([&] { processor.setCurrentProgram(1); });
    expect(pause.entered.wait(3000), "editor test pauses an active state publication");
    { std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor()); }
    pause.release.signal();
    writer.join();
    processor.setStatePublicationHook(nullptr, nullptr);
    expect(! pause.timedOut.load() && processor.getCurrentProgram() == 1,
           "editor opens and closes while publication is active without blocking the writer");
}

void legacyVersions()
{
    PresetFiles files;
    Processor source;
    setInt(source, swaraxt::IDs::osc1Param, 73);
    source.sequenceState().setStep(0, 0x80 | 67, 0x60);
    const auto expected = decode(save(source));
    for (int version : { 0, 1, 2, 3 })
    {
        auto state = expected.createCopy();
        if (version == 0) state.removeProperty("stateVersion", nullptr);
        else state.setProperty("stateVersion", version, nullptr);
        for (const auto* property : { "sequenceLocked", "randomNoteLower", "randomNoteUpper" })
            state.removeProperty(property, nullptr);
        juce::MemoryBlock data;
        const auto xml = state.createXml();
        juce::AudioProcessor::copyXmlToBinary(*xml, data);
        Processor restored;
        restored.setSequenceLocked(true);
        restored.setRandomNoteRange({ -24, 0 });
        restored.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
        expect(decode(save(restored)).isEquivalentTo(expected),
               "legacy host versions retain every parameter, sequence and metadata field with original defaults");
        if (version != 0)
        {
            expect(files.file("Previous").replaceWithData(data.getData(), data.getSize()), "legacy fixture writes");
            juce::String error;
            expect(restored.loadPresetEntry({ "Previous", files.file("Previous"), -1, false }, error)
                       && parameter(decode(save(restored)), swaraxt::IDs::osc1Param) == 73
                       && restored.currentPresetIsUser() && restored.currentPresetName() == "Previous",
                   "legacy user-preset versions load their sound and selected file name");
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
}

void entrySnapshot()
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    const auto a = decode(save(processor));
    const int aValue = parameter(a, swaraxt::IDs::osc1Param);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    setInt(processor, swaraxt::IDs::mixNoise, 17);
    const auto b = decode(save(processor));
    expect(parameter(b, swaraxt::IDs::osc1Param) == 73, "ordinary edits establish serialized B");

    struct Pause {
        juce::WaitableEvent entered, release;
        std::atomic<bool> timedOut { false };
        static void hook(void* context, Point point)
        {
            if (point != Point::bulkEpochPublished) return;
            auto& self = *static_cast<Pause*>(context);
            self.entered.signal();
            if (! self.release.wait(5000)) self.timedOut.store(true);
        }
    } pause;
    processor.setStatePublicationHook(Pause::hook, &pause);
    std::thread writer([&] { processor.setCurrentProgram(1); });
    const bool entered = pause.entered.wait(3000);
    expect(entered, "writer exposes the controlled odd-epoch boundary");
    juce::MemoryBlock captured;
    juce::WaitableEvent saved;
    std::thread saver([&] { captured = save(processor); saved.signal(); });
    const bool completed = saved.wait(2000);
    pause.release.signal();
    writer.join();
    saver.join();
    processor.setStatePublicationHook(nullptr, nullptr);
    const auto during = decode(captured);
    const auto c = decode(save(processor));
    std::printf("entry generations: A=%d B=%d captured=%d C=%d; program during=%d final=%d\n",
                aValue, parameter(b, swaraxt::IDs::osc1Param), parameter(during, swaraxt::IDs::osc1Param),
                parameter(c, swaraxt::IDs::osc1Param), static_cast<int>(during["currentProgram"]),
                static_cast<int>(c["currentProgram"]));
    expect(completed && ! pause.timedOut.load(), "save finishes while writer remains paused");
    expect(parameter(during, swaraxt::IDs::osc1Param) == 73
               && parameter(during, swaraxt::IDs::mixNoise) == 17,
           "entry save returns immediate pre-transaction B, never obsolete A");
    expect(static_cast<int>(during["currentProgram"]) == 0
               && static_cast<int>(c["currentProgram"]) == 1,
           "entry save and completed save retain their own program generation");
}

int notificationReaderChild(const juce::File& marker)
{
    Processor processor;
    processor.prepareToPlay(48000, 64);
    setInt(processor, swaraxt::IDs::osc1Param, 73);
    setInt(processor, swaraxt::IDs::osc1Shape, 9);
    struct Schedule final : juce::AudioProcessorValueTreeState::Listener {
        Processor& processor;
        juce::File marker;
        juce::WaitableEvent readerEven, writerOdd;
        std::atomic<bool> firstNotification { true }, firstRead { true }, timedOut { false };
        juce::MemoryBlock captured;
        Schedule(Processor& p, juce::File file) : processor(p), marker(std::move(file)) {}
        void parameterChanged(const juce::String&, float) override
        {
            if (! firstNotification.exchange(false)) return;
            captured = save(processor);
        }
        static void hook(void* context, Point point)
        {
            auto& self = *static_cast<Schedule*>(context);
            if (point == Point::stateReadBeforeLock && self.firstRead.exchange(false))
            {
                self.marker.appendText("NOTIFICATION_READER_OBSERVED_EVEN\n");
                self.readerEven.signal();
                if (! self.writerOdd.wait(3000)) self.timedOut.store(true);
            }
            else if (point == Point::bulkEpochPublished)
            {
                self.marker.appendText("WRITER_PUBLISHED_ODD_BEFORE_READER_LOCK\n");
                self.writerOdd.signal();
            }
        }
    } schedule(processor, marker);
    processor.setStatePublicationHook(Schedule::hook, &schedule);
    processor.getApvts().addParameterListener(swaraxt::IDs::osc1Shape, &schedule);
    std::thread notification([&] { setInt(processor, swaraxt::IDs::osc1Shape, 10); });
    if (! schedule.readerEven.wait(3000)) std::abort();
    std::thread writer([&] { processor.setCurrentProgram(1); });
    notification.join();
    writer.join();
    processor.getApvts().removeParameterListener(swaraxt::IDs::osc1Shape, &schedule);
    processor.setStatePublicationHook(nullptr, nullptr);
    const auto captured = decode(schedule.captured);
    if (schedule.timedOut.load() || parameter(captured, swaraxt::IDs::osc1Param) != 73
        || parameter(captured, swaraxt::IDs::osc1Shape) != 10 || processor.getCurrentProgram() != 1)
        return 4;
    marker.appendText("NOTIFICATION_READER_AND_WRITER_COMPLETED\n");
    return 0;
}

int writerCycleChild(const juce::File& marker)
{
    Processor processor, source;
    processor.prepareToPlay(48000, 64);
    source.setCurrentProgram(2);
    setInt(source, swaraxt::IDs::osc1Param, 43);
    const auto restored = save(source);
    struct Schedule {
        juce::WaitableEvent programHeld, restoreHeld;
        std::mutex logMutex;
        juce::File marker;
        void log(const char* message)
        {
            const std::lock_guard<std::mutex> lock(logMutex);
            marker.appendText(juce::String(message) + "\n");
        }
        static void hook(void* context, Point point)
        {
            auto& self = *static_cast<Schedule*>(context);
            if (point == Point::programPublicationLocked)
            {
                self.log("PROGRAM_OWNS_PUBLICATION");
                self.programHeld.signal();
                if (self.restoreHeld.wait(1000))
                    self.log("CYCLE_ARMED: program owns publication; restore owns gate");
                else
                    self.log("RESTORE_EXCLUDED_WHILE_PROGRAM_OWNS_PUBLICATION");
            }
            else if (point == Point::restoreGateLocked)
            {
                self.log("RESTORE_OWNS_GATE");
                self.restoreHeld.signal();
            }
        }
    } schedule;
    schedule.marker = marker;
    processor.setStatePublicationHook(Schedule::hook, &schedule);
    std::thread program([&] { processor.setCurrentProgram(1); });
    if (! schedule.programHeld.wait(3000)) { schedule.log("PROGRAM_BARRIER_TIMEOUT"); std::abort(); }
    std::thread restore([&] {
        processor.setStateInformation(restored.getData(), static_cast<int>(restored.getSize()));
    });
    program.join();
    restore.join();
    processor.setStatePublicationHook(nullptr, nullptr);
    const auto final = decode(save(processor));
    if (parameter(final, swaraxt::IDs::osc1Param) != 43 || static_cast<int>(final["currentProgram"]) != 2)
        return 3;
    schedule.log("BOTH_WRITERS_COMPLETED_COHERENTLY");
    return 0;
}

void isolatedCycle(bool notificationReader)
{
    const auto marker = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("swaraxt-state-cycle", ".log", false);
    juce::ChildProcess child;
    const juce::StringArray arguments {
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
        notificationReader ? "--notification-reader" : "--writer-cycle", marker.getFullPathName()
    };
    expect(child.start(arguments), "isolated writer-cycle process starts");
    const bool finished = child.waitForProcessToFinish(10000);
    const auto evidence = marker.loadFileAsString();
    std::printf("controlled writer schedule:\n%s", evidence.toRawUTF8());
    if (! finished)
    {
        child.kill();
        std::printf("writer watchdog terminated blocked child\n");
    }
    expect(finished && child.getExitCode() == 0, "controlled reader/writer schedule completes without a lock-order cycle");
    expect(! evidence.contains("CYCLE_ARMED"), "writers never own the two locks in opposite orders");
    marker.deleteFile();
}

void runCase(const char* name, void (*test)())
{
    std::printf("case: %s\n", name);
    test();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
}

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    if (argc == 3 && juce::String(argv[1]) == "--writer-cycle")
        return writerCycleChild(juce::File(argv[2]));
    if (argc == 3 && juce::String(argv[1]) == "--notification-reader")
        return notificationReaderChild(juce::File(argv[2]));
    runCase("entry snapshot", entrySnapshot);
    runCase("writer cycle", [] { isolatedCycle(false); });
    runCase("notification reader at transaction entry", [] { isolatedCycle(true); });
    runCase("serialization reentry", [] { serializationReentry(false); });
    runCase("serialization exception", [] { serializationReentry(true); });
    runCase("notification reentry and metadata", reentrantSavesAndMetadata);
    runCase("late file completion", [] { userPresetCompletion(false); });
    runCase("late file failure", [] { userPresetCompletion(true); });
    runCase("unchanged program during save", [] { userPresetCompletion(false, true); });
    runCase("older file completes first", [] { overlappingUserPresetSaves(false); });
    runCase("newer file completes first", [] { overlappingUserPresetSaves(true); });
    runCase("failed loads and rapid selections", failedLoadsAndRapidSelections);
    runCase("locked tuning and session round-trip", lockedTuningAndSessionRoundTrip);
    runCase("legacy state and preset versions", legacyVersions);
    if (argc == 2 && juce::String(argv[1]) == "--state-only")
        std::puts("editor case excluded by explicit --state-only selection");
    else
        runCase("editor during publication", editorDuringPublication);
    std::printf("state publication tests: %s (%d failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
