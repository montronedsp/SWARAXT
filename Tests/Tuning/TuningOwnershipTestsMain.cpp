// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Plugin/PluginProcessor.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

namespace {
using Engine = swaraxt::SwaraXtEngine;
using Point = Engine::TuningPoint;
using Table = swaraxt::tuning::PublishedTuning;
std::atomic<int> failures { 0 };
void expect(bool condition, const char* message)
{
    if (! condition) { failures.fetch_add(1); std::printf("FAIL: %s\n", message); }
}
struct Fixture {
    std::vector<uint8_t> bytes;
    std::unique_ptr<Table> table = std::make_unique<Table>();
};
Fixture fixture(unsigned id)
{
    const int count = 5 + static_cast<int>(id);
    const double period = 1100.0 + 25.0 * id;
    juce::String text = "! ownership\nGeneration " + juce::String(id) + "\n" + juce::String(count) + "\n";
    for (int i = 1; i <= count; ++i) text += juce::String(period * i / count, 10) + "\n";
    swaraxt::tuning::TuningSnapshot parsed, decoded;
    swaraxt::tuning::ParseError error;
    Fixture result;
    expect(swaraxt::tuning::parseScalaAndKeyboard(text.toRawUTF8(), nullptr, parsed, error), "fixture parses");
    parsed.referenceHz = 430.0 + 7.0 * id;
    expect(swaraxt::tuning::writeTuningTable(parsed, result.bytes, error), "fixture encodes");
    expect(swaraxt::tuning::readTuningTable(result.bytes.data(), result.bytes.size(), decoded, error)
               && result.table->fillFrom(decoded), "fixture independently decodes");
    return result;
}
bool exact(double x, double y) noexcept
{
    static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559);
    uint64_t first = 0, second = 0;
    std::memcpy(&first, &x, sizeof(first)); std::memcpy(&second, &y, sizeof(second));
    return first == second;
}
bool equal(const Table& a, const Table& b)
{
    return a.valid == b.valid && a.formalPeriod == b.formalPeriod && a.degreeCount == b.degreeCount
        && a.referenceNote == b.referenceNote && exact(a.referenceHz, b.referenceHz) && exact(a.periodCents, b.periodCents)
        && std::memcmp(a.name, b.name, sizeof(a.name)) == 0
        && std::equal(std::begin(a.degreeCents), std::end(a.degreeCents), std::begin(b.degreeCents), exact)
        && std::equal(std::begin(a.midiDegree), std::end(a.midiDegree), std::begin(b.midiDegree))
        && a.quantizerCount == b.quantizerCount && a.quantizerSaturated == b.quantizerSaturated
        && a.quantizerUsesPeriods == b.quantizerUsesPeriods && a.quantizerCanonical == b.quantizerCanonical
        && std::equal(std::begin(a.quantizerHz), std::end(a.quantizerHz), std::begin(b.quantizerHz), exact)
        && std::equal(std::begin(a.quantizerLogHz), std::end(a.quantizerLogHz), std::begin(b.quantizerLogHz), exact)
        && std::equal(std::begin(a.quantizerDegree), std::end(a.quantizerDegree), std::begin(b.quantizerDegree));
}
bool load(SwaraXtAudioProcessor& p, const Fixture& f) { return p.loadTuningBytes(f.bytes.data(), f.bytes.size()); }
void render(SwaraXtAudioProcessor& p)
{
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    p.processBlock(audio, midi);
}
void expectPinned(Engine& engine, const Table& expected)
{
    const Table* tuning = nullptr;
    int claim = -1;
    const bool pinned = engine.pinTuningForTests(tuning, claim);
    expect(pinned && tuning != nullptr && equal(*tuning, expected), "pinned full payload belongs to one generation");
    engine.unpinTuningForTests(claim);
}
struct Pause {
    Point point;
    int filter;
    juce::WaitableEvent entered, release;
    std::atomic<bool> first { true }, timedOut { false };
    std::atomic<int> slot { -1 };
    explicit Pause(Point selected, int selectedSlot = -1) : point(selected), filter(selectedSlot) {}
    static void hook(void* context, Point point, int slot) noexcept
    {
        auto& p = *static_cast<Pause*>(context);
        if (point != p.point || (p.filter >= 0 && slot != p.filter) || ! p.first.exchange(false)) return;
        p.slot.store(slot);
        p.entered.signal();
        if (! p.release.wait(5000)) p.timedOut.store(true);
    }
};
void audioAcquisitionAndIdle(const Fixture& a, const Fixture& b, const Fixture& c)
{
    SwaraXtAudioProcessor p;
    expect(load(p, a), "publish before first prepare");
    p.prepareToPlay(48000, 64);
    auto& engine = p.engineForTests();
    Pause pause(Point::audioObserved);
    engine.setTuningHook(Pause::hook, &pause);
    std::thread audio([&] { render(p); });
    expect(pause.entered.wait(5000), "audio pauses between publication observation and latch installation");
    expect(load(p, b), "one successor can publish during acquisition");
    expect(! load(p, c), "retirement prevents a second successor from reclaiming the unlatched observation");
    expect(engine.tuningBytesForState() == b.bytes, "exhaustion preserves last accepted serialized tuning");
    pause.release.signal(); audio.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(! pause.timedOut.load() && equal(engine.stableTuningForTests(), *b.table), "first callback safely adopts complete successor");
    expect(load(p, c), "the same failed publication succeeds after the callback advances its latch");
    for (int i = 0; i != 12; ++i) expect(load(p, (i & 1) != 0 ? a : c), "idle publications recycle free slots");
    expect(equal(engine.stableTuningForTests(), *b.table), "idle writers never modify audio-owned stable storage");
    Pause idlePause(Point::audioObserved);
    engine.setTuningHook(Pause::hook, &idlePause);
    std::thread resumedAudio([&] { render(p); });
    expect(idlePause.entered.wait(5000), "callback after inactivity pauses before refreshing its old latch");
    // The callback is now active, so retirement cannot fold as it did while
    // idle. Refreshing the old latch must free a slot before another import.
    expect(! load(p, c) && ! load(p, b), "old latch and retirement preserve all occupied slots after inactivity");
    idlePause.release.signal(); resumedAudio.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(! idlePause.timedOut.load() && equal(engine.stableTuningForTests(), *a.table),
           "callback after inactivity preserves the last accepted complete tuning");
    expect(load(p, c), "publication resumes after the idle callback refreshes its latch");
}
void writerEntryAndCallbackExit(const Fixture& a, const Fixture& b, const Fixture& c)
{
    SwaraXtAudioProcessor p;
    expect(load(p, a), "writer fixture loads"); p.prepareToPlay(48000, 64);
    auto& engine = p.engineForTests();
    Pause writerPause(Point::writerSelected);
    engine.setTuningHook(Pause::hook, &writerPause);
    bool accepted = false;
    std::thread writer([&] { accepted = load(p, b); });
    expect(writerPause.entered.wait(5000), "writer pauses after final recheck before ordinary assignment");
    expect(! load(p, c), "second writer entry is rejected without clearing the owner's admission");
    render(p);
    expect(equal(engine.stableTuningForTests(), *a.table), "callback entry during write preparation uses old valid tuning");
    writerPause.release.signal(); writer.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(accepted && ! writerPause.timedOut.load(), "writer finishes after overlap");
    Pause exitPause(Point::audioExiting);
    engine.setTuningHook(Pause::hook, &exitPause);
    std::thread audio([&] { render(p); });
    expect(exitPause.entered.wait(5000), "callback pauses before releasing active scope");
    expect(load(p, c), "a successor publishes while current stable copy remains audio-owned");
    expectPinned(engine, *c.table);
    expect(equal(engine.stableTuningForTests(), *b.table), "audio and a query can own different complete generations");
    expect(load(p, a), "another successor can publish after the query releases its claim");
    expect(! load(p, b), "active retirement and latch exhaust three slots safely");
    exitPause.release.signal(); audio.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(! exitPause.timedOut.load() && equal(engine.stableTuningForTests(), *b.table), "callback exit preserves the tuning it copied");
    expect(load(p, b), "idle reclamation resumes after callback exit");
}
void queryAbaAndLateClaim(const Fixture& a, const Fixture& b, const Fixture& c)
{
    SwaraXtAudioProcessor p;
    expect(load(p, a), "ABA fixture loads");
    auto& engine = p.engineForTests();
    Pause observed(Point::queryObserved);
    engine.setTuningHook(Pause::hook, &observed);
    auto copied = std::make_unique<Table>();
    std::thread query([&] {
        const Table* table = nullptr; int claim = -1;
        if (engine.pinTuningForTests(table, claim)) *copied = *table;
        engine.unpinTuningForTests(claim);
    });
    expect(observed.entered.wait(5000), "query pauses before claiming observed index");
    expect(load(p, b) && load(p, c), "unclaimed index is recycled and republished");
    expect(engine.publishedTuningForTests() == observed.slot.load(), "actual ABA reuses the observed index");
    observed.release.signal(); query.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(! observed.timedOut.load() && equal(*copied, *c.table), "ABA revalidation imports complete replacement generation");

    expect(load(p, a), "late claim starts with another complete generation");
    struct Schedule {
        Pause query { Point::queryObserved }, writer;
        explicit Schedule(int slot) : writer(Point::writerSelected, slot) {}
        static void hook(void* context, Point point, int slot) noexcept {
            auto& s = *static_cast<Schedule*>(context);
            Pause::hook(&s.query, point, slot); Pause::hook(&s.writer, point, slot);
        }
    } schedule(engine.publishedTuningForTests());
    engine.setTuningHook(Schedule::hook, &schedule);
    std::thread lateQuery([&] {
        const Table* table = nullptr; int claim = -1;
        if (engine.pinTuningForTests(table, claim)) *copied = *table;
        engine.unpinTuningForTests(claim);
    });
    expect(schedule.query.entered.wait(5000), "late query records old publication");
    expect(load(p, b), "remove late query's unclaimed index");
    bool accepted = false;
    std::thread writer([&] { accepted = load(p, c); });
    expect(schedule.writer.entered.wait(5000), "writer passed final scan of the still-unclaimed old index");
    schedule.query.release.signal(); lateQuery.join();
    expect(equal(*copied, *b.table), "late claim rejects unpublished source before accessing ordinary fields");
    schedule.writer.release.signal(); writer.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(accepted && ! schedule.query.timedOut.load() && ! schedule.writer.timedOut.load(), "late-claim schedule terminates");
    expectPinned(engine, *c.table);
}
void claimExhaustion(const Fixture& a, const Fixture& b, const Fixture& c)
{
    SwaraXtAudioProcessor p;
    expect(load(p, a), "claim fixture loads");
    auto& engine = p.engineForTests();
    struct Claims {
        std::atomic<int> arrivals { 0 };
        juce::WaitableEvent entered[2], release[2];
        std::atomic<bool> timedOut { false };
        static void hook(void* context, Point point, int) noexcept {
            if (point != Point::queryValidated) return;
            auto& c = *static_cast<Claims*>(context);
            const int index = c.arrivals.fetch_add(1);
            if (index >= 2) return;
            c.entered[index].signal();
            if (! c.release[index].wait(5000)) c.timedOut.store(true);
        }
    } claims;
    engine.setTuningHook(Claims::hook, &claims);
    std::thread first([&] { expectPinned(engine, *a.table); });
    expect(claims.entered[0].wait(5000), "first query owns its claim");
    expect(load(p, b), "publish while first query remains pinned");
    std::thread second([&] { expectPinned(engine, *b.table); });
    expect(claims.entered[1].wait(5000), "second query owns a different generation");
    expect(exact(p.hzForActiveDegree(0), 0.0) && ! p.quantizeActiveHz(440).valid,
           "exhausted frequency and quantizer queries use existing bounded failure results");
    expect(p.activeTuningName() == "12-EDO" && p.scaleRandomView().degreeCount == 0,
           "exhausted name and scale queries use existing fallback values");
    int16_t fallback[130] {};
    p.copyTuningDegrees(fallback, 130);
    for (int note = 0; note != 130; ++note)
        expect(fallback[note] == (note < 128 ? note - 60 : -1), "exhausted degree query preserves its bounded fallback map");
    expect(! load(p, c) && ! load(p, c), "pinned generations prevent repeated unsafe reuse");
    claims.release[0].signal(); first.join();
    expect(load(p, c), "released claim permits reuse");
    expect(! load(p, a), "remaining claim protects old generation after retirement folds");
    claims.release[1].signal(); second.join();
    engine.setTuningHook(nullptr, nullptr);
    expect(! claims.timedOut.load() && load(p, a), "all claims release and writer admission recovers");
}
void factoriesAndState(const Fixture& custom)
{
    SwaraXtAudioProcessor p;
    for (int prepared = 0; prepared != 2; ++prepared)
    {
        if (prepared != 0) p.prepareToPlay(48000, 64);
        for (int index = 0; index != p.builtinTuningCount(); ++index)
        {
            expect(p.loadBuiltinTuning(index), "every factory tuning publishes before and after preparation");
            const auto bytes = p.engineForTests().tuningBytesForState();
            swaraxt::tuning::TuningSnapshot parsed; swaraxt::tuning::ParseError error;
            auto expected = std::make_unique<Table>();
            expect(swaraxt::tuning::readTuningTable(bytes.data(), bytes.size(), parsed, error) && expected->fillFrom(parsed), "factory decodes");
            expectPinned(p.engineForTests(), *expected);
            int16_t degrees[128] {};
            p.copyTuningDegrees(degrees, 128);
            expect(std::equal(std::begin(degrees), std::end(degrees), std::begin(expected->midiDegree)), "degree query preserves complete factory map");
            expect(p.activeTuningName() == juce::String(expected->name) && p.activeBuiltinTuning() == index, "factory metadata agrees when uncontended");
            expect(exact(p.hzForActiveDegree(-3), expected->hzForDegree(-3)), "degree frequency matches unchanged model exactly");
            const auto chosen = p.quantizeActiveHz(433.0), oracle = expected->quantizeHz(433.0);
            expect(chosen.valid == oracle.valid && chosen.degree == oracle.degree && exact(chosen.hz, oracle.hz), "quantizer result is unchanged");
            if (prepared != 0) { render(p); expect(equal(p.engineForTests().stableTuningForTests(), *expected), "audio stable copy matches complete factory payload"); }
        }
    }
    expect(load(p, custom), "custom tuning publishes");
    const uint8_t invalid[] { 0, 1, 2, 3 };
    expect(! p.loadTuningBytes(invalid, sizeof(invalid)) && ! p.loadTuningBytes(nullptr, 0), "invalid imports are rejected");
    expectPinned(p.engineForTests(), *custom.table);
    expect(p.engineForTests().tuningBytesForState() == custom.bytes, "failed imports preserve accepted serialized data");
    juce::MemoryBlock state;
    p.getStateInformation(state);
    SwaraXtAudioProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    restored.prepareToPlay(48000, 64); render(restored);
    expect(equal(restored.engineForTests().stableTuningForTests(), *custom.table), "embedded tuning restores through actual processor state path");
    expect(p.loadBuiltinTuning(0), "valid import succeeds after invalid input"); render(p);
    expect(equal(restored.engineForTests().stableTuningForTests(), *custom.table), "independent instance retains its own stable tuning");
}
void idleImportAvailability()
{
    constexpr size_t count = 12;
    std::array<Fixture, count> tunings;
    std::array<juce::MemoryBlock, count> states;
    SwaraXtAudioProcessor donor;
    for (size_t i = 0; i != count; ++i)
    {
        tunings[i] = fixture(static_cast<unsigned>(i) + 1u);
        expect(load(donor, tunings[i]), "inactive donor accepts successive distinct tunings");
        donor.getStateInformation(states[i]);
    }
    struct StoppedPlayHead : juce::AudioPlayHead {
        juce::Optional<PositionInfo> getPosition() const override {
            PositionInfo position;
            position.setIsPlaying(false); position.setBpm(120.0);
            position.setPpqPosition(0.0);
            return position;
        }
    };
    const char* names[] { "never-prepared", "prepared-no-callbacks", "stopped-with-callbacks", "processing-suspended" };
    for (int mode = 0; mode != 4; ++mode)
    {
        StoppedPlayHead playHead;
        SwaraXtAudioProcessor processor;
        processor.setPlayHead(&playHead);
        if (mode != 0) processor.prepareToPlay(48000, 64);
        if (mode == 3) { render(processor); processor.suspendProcessing(true); }
        auto stableBefore = std::make_unique<Table>(processor.engineForTests().stableTuningForTests());
        int factories = 0, imports = 0, restores = 0;
        for (size_t i = 0; i != count; ++i)
        {
            const int index = static_cast<int>(i) % processor.builtinTuningCount();
            const bool accepted = processor.loadBuiltinTuning(index);
            factories += accepted ? 1 : 0;
            expect(accepted && processor.activeBuiltinTuning() == index, "inactive factory selection reports the actual published table");
            if (mode == 2) render(processor);
        }
        for (size_t i = 0; i != count; ++i)
        {
            const bool accepted = load(processor, tunings[i]);
            imports += accepted ? 1 : 0;
            expect(accepted && processor.activeBuiltinTuning() == -1, "inactive distinct import reports acceptance");
            expectPinned(processor.engineForTests(), *tunings[i].table);
            expect(processor.activeTuningName() == juce::String(tunings[i].table->name)
                   && processor.engineForTests().tuningBytesForState() == tunings[i].bytes,
                   "inactive query metadata and serialized bytes agree with accepted import");
            if (mode == 2) {
                render(processor);
                expect(equal(processor.engineForTests().stableTuningForTests(), *tunings[i].table), "stopped transport callbacks adopt each tuning");
            }
        }
        for (size_t i = 0; i != count; ++i)
        {
            processor.setStateInformation(states[i].getData(), static_cast<int>(states[i].getSize()));
            const bool restored = processor.engineForTests().tuningBytesForState() == tunings[i].bytes;
            restores += restored ? 1 : 0;
            expect(restored, "inactive host restoration publishes the embedded tuning");
            expectPinned(processor.engineForTests(), *tunings[i].table);
            if (mode == 2) render(processor);
        }
        if (mode != 2)
            expect(equal(processor.engineForTests().stableTuningForTests(), *stableBefore), "inactive imports never mutate the stable audio copy");
        const uint8_t invalid[] { 0, 1, 2 };
        expect(! processor.loadTuningBytes(invalid, sizeof(invalid)) && ! processor.loadBuiltinTuning(-1), "invalid idle requests report failure");
        expectPinned(processor.engineForTests(), *tunings.back().table);
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        juce::MemoryBlock embedded;
        expect(xml != nullptr && embedded.fromBase64Encoding(xml->getStringAttribute("tuningTable"))
               && embedded.getSize() == tunings.back().bytes.size()
               && std::memcmp(embedded.getData(), tunings.back().bytes.data(), embedded.getSize()) == 0,
               "idle host save serializes the actual accepted tuning");
        std::printf("Availability %s: factory=%d/12 import=%d/12 restore=%d/12\n", names[mode], factories, imports, restores);
        processor.suspendProcessing(false);
        processor.setPlayHead(nullptr);
    }
}
}
int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    const auto a = fixture(1), b = fixture(2), c = fixture(3);
    audioAcquisitionAndIdle(a, b, c);
    writerEntryAndCallbackExit(a, b, c);
    queryAbaAndLateClaim(a, b, c);
    claimExhaustion(a, b, c);
    factoriesAndState(c);
    idleImportAvailability();
    std::printf("Tuning ownership tests: %s (%d failures), payload=%zu bytes\n", failures.load() ? "FAILED" : "PASSED", failures.load(), sizeof(Table));
    return failures.load() ? 1 : 0;
}
