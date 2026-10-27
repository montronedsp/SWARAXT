// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include "Engine/FreeCvPitch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace swaraxt {

struct SequenceSnapshot {
    static constexpr int kNumSteps = 16;

    uint8_t length = 16;
    uint8_t rotation = 0;
    uint8_t grooveTemplate = 0;
    uint8_t arpPattern = 1;
    std::array<uint16_t, kNumSteps> steps {
        0x70b0, 0xd0b0, 0x50bc, 0xd0bc,
        0x7fb0, 0xdfb0, 0x5fbc, 0x5f3c,
        0x7cb0, 0xdcb0, 0x5cbc, 0xdcbc,
        0x74b0, 0x5430, 0x74bc, 0x543c
    };
    // 0 = legacy notes. 1 = free CV. 2 = scale degrees. 3 = quantized free CV.
    uint8_t pitchMode = 0;
    uint8_t freePitchReady = 0;
    uint8_t scaleReady = 0;
    // Untransposed step pitch, 1/1024 semitone from MIDI 0. Unused while legacy.
    std::array<int32_t, kNumSteps> freePitch {};
    // Signed scale degree. Independent of packed notes and free CV.
    std::array<int16_t, kNumSteps> scaleDegree {};

    static constexpr uint16_t pack(uint8_t dataA, uint8_t dataB) noexcept
    {
        return static_cast<uint16_t>(
            static_cast<uint16_t>(dataA)
            | (static_cast<uint16_t>(dataB) << 8));
    }

    static constexpr uint8_t dataA(uint16_t packed) noexcept
    {
        return static_cast<uint8_t>(packed & 0xffu);
    }

    static constexpr uint8_t dataB(uint16_t packed) noexcept
    {
        return static_cast<uint8_t>(packed >> 8);
    }
};

// Control-thread writers serialize through the payload gate. Audio capture
// makes one admission attempt, retaining its previous sequence on contention.
class SequenceState {
 public:
    static_assert(std::atomic<uint8_t>::is_always_lock_free);
    static_assert(std::atomic<uint16_t>::is_always_lock_free);
    static_assert(std::atomic<int16_t>::is_always_lock_free);
    static_assert(std::atomic<int32_t>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
    enum class SnapshotPoint { readerAdmitted, writerMidpoint, writerContended };
    using SnapshotHook = void (*)(void*, SnapshotPoint) noexcept;
    void setSnapshotHook(SnapshotHook hook, void* context) noexcept
    {
        snapshotHook_ = hook;
        snapshotHookContext_ = context;
    }
    void setRevisionForTests(uint32_t evenRevision) noexcept
    {
        jassert((evenRevision & 1u) == 0u);
        beginWrite();
        revision_.store(evenRevision, std::memory_order_relaxed);
        writer_.clear(std::memory_order_release);
    }
#endif

    class Listener {
     public:
        virtual ~Listener() = default;
        virtual void sequenceStateChanged() = 0;
    };

    SequenceState()
    {
        store(defaultSnapshot(), false);
    }

    ~SequenceState()
    {
        auto hub = listenerHub_;
        const juce::ScopedLock lock(hub->lock);
        hub->alive.store(false, std::memory_order_release);
        hub->listeners.clear();
    }

    static SequenceSnapshot defaultSnapshot() noexcept { return {}; }

    enum class Randomize { notes, velocity, events, value, sequence };
    enum class RandomizationType { type1 };
    struct NoteRange {
        int lower = -12;
        int upper = 12;
        static constexpr int extent = 48;
    };
    // Owned degree table. A view must stay valid after the tuning slot that
    // produced it has been published again. The array is the storage; it is
    // copied with the view and never aliases a reusable slot.
    static constexpr int kMaxScaleDegreeCents = 256;
    struct ScaleRandomView {
        int formalPeriod = 12;
        double periodCents = 1200.0;
        int degreeCount = 0;
        double degreeCents[kMaxScaleDegreeCents] {};
    };
    static constexpr int kScaleDegreeLimit = 512;

    static double scaleViewCents(const ScaleRandomView& view, int degree) noexcept
    {
        const int period = view.formalPeriod > 0 ? view.formalPeriod : 1;
        int periodIndex = 0;
        int inside = degree;
        if (inside >= 0)
        {
            periodIndex = inside / period;
            inside %= period;
        }
        else
        {
            periodIndex = -(((-inside) + period - 1) / period);
            inside = degree - periodIndex * period;
        }
        double cents = static_cast<double>(periodIndex) * view.periodCents;
        if (inside > 0 && view.degreeCount > 0)
        {
            const int index = inside - 1;
            if (index >= 0 && index < view.degreeCount)
                cents += view.degreeCents[index];
        }
        return cents;
    }

    static int16_t randomScaleDegree(juce::Random& random, int16_t root, NoteRange range,
                                    const ScaleRandomView* view) noexcept
    {
        std::array<int, 1025> offsets {};
        int count = 0;
        const bool tuned = view != nullptr && view->formalPeriod > 0 && view->periodCents > 0.0
            && view->degreeCount > 0;
        if (tuned)
        {
            const double lo = static_cast<double>(range.lower) * 100.0;
            const double hi = static_cast<double>(range.upper) * 100.0;
            for (int offset = -kScaleDegreeLimit; offset <= kScaleDegreeLimit && count < static_cast<int>(offsets.size()); ++offset)
            {
                const double cents = scaleViewCents(*view, offset) - scaleViewCents(*view, 0);
                if (cents + 1.0e-6 >= lo && cents - 1.0e-6 <= hi)
                    offsets[static_cast<size_t>(count++)] = offset;
            }
        }
        else
        {
            for (int offset = range.lower; offset <= range.upper && count < static_cast<int>(offsets.size()); ++offset)
                offsets[static_cast<size_t>(count++)] = offset;
        }
        if (count <= 0)
            return root;
        const int chosen = root + offsets[static_cast<size_t>(random.nextInt(count))];
        return static_cast<int16_t>(juce::jlimit(-kScaleDegreeLimit, kScaleDegreeLimit, chosen));
    }

    // Uniform continuous CV in [root+lower, root+upper] semitones at 1/1024.
    // Inclusive endpoints; zero-width ranges return the clamped root.
    static int32_t randomFreeCvInRange(juce::Random& random,
                                       int32_t rootUnits,
                                       NoteRange range) noexcept
    {
        const int32_t loOffset =
            static_cast<int32_t>(range.lower) * FreeCvPitch::kUnitsPerSemitone;
        const int32_t hiOffset =
            static_cast<int32_t>(range.upper) * FreeCvPitch::kUnitsPerSemitone;
        int32_t lo = rootUnits + loOffset;
        int32_t hi = rootUnits + hiOffset;
        if (lo < FreeCvPitch::kMinUnits)
            lo = FreeCvPitch::kMinUnits;
        if (hi < FreeCvPitch::kMinUnits)
            hi = FreeCvPitch::kMinUnits;
        if (lo > FreeCvPitch::kMaxUnits)
            lo = FreeCvPitch::kMaxUnits;
        if (hi > FreeCvPitch::kMaxUnits)
            hi = FreeCvPitch::kMaxUnits;
        if (hi < lo)
        {
            const int32_t swap = lo;
            lo = hi;
            hi = swap;
        }
        if (hi == lo)
            return lo;
        // Span fits in int: max extent is 96 semitones * 1024 + 1.
        const int span = static_cast<int>(hi - lo) + 1;
        return lo + random.nextInt(span);
    }

    void randomize(Randomize kind, juce::Random& random, NoteRange range,
                   RandomizationType type = RandomizationType::type1,
                   const ScaleRandomView* scaleView = nullptr)
    {
        juce::ignoreUnused(type); // The production domain contains Type 1 only.
        auto pattern = snapshot();
        range.lower = juce::jlimit(-NoteRange::extent, 0, range.lower);
        range.upper = juce::jlimit(0, NoteRange::extent, range.upper);
        const bool linear = pattern.pitchMode == 1 || pattern.pitchMode == 3;
        const bool scaleMode = pattern.pitchMode == 2;
        // Freeze the root before any step write so mid-pass updates cannot drift.
        const int rootMidi = SequenceSnapshot::dataA(pattern.steps[0]) & 0x7f;
        const int32_t rootCv = (linear && pattern.freePitchReady != 0)
            ? pattern.freePitch[0]
            : FreeCvPitch::fromMidiNote(rootMidi);
        const int16_t rootDegree = pattern.scaleDegree[0];

        // Legacy Type 1 keeps the original pentatonic offset table and RNG draws.
        constexpr std::array<int, 5> scale { 0, 2, 4, 7, 9 };
        std::array<int, 2 * NoteRange::extent + 1> offsets {};
        int count = 0;
        if (! linear)
        {
            for (int offset = range.lower; offset <= range.upper; ++offset)
            {
                const int degree = (offset % 12 + 12) % 12;
                if (std::find(scale.begin(), scale.end(), degree) != scale.end())
                    offsets[static_cast<size_t>(count++)] = offset;
            }
        }

        bool previousGate = false;
        for (int logical = 0; logical < SequenceSnapshot::kNumSteps; ++logical)
        {
            const auto index = static_cast<size_t>((pattern.rotation + logical) & 15);
            auto& packed = pattern.steps[index];
            auto a = SequenceSnapshot::dataA(packed);
            auto b = SequenceSnapshot::dataB(packed);
            if (kind == Randomize::notes || kind == Randomize::sequence)
            {
                if (linear)
                {
                    // Continuous CV only. Packed integer notes stay intact.
                    pattern.freePitch[index] = randomFreeCvInRange(random, rootCv, range);
                    pattern.freePitchReady = 1;
                }
                else if (scaleMode)
                {
                    pattern.scaleDegree[index] = randomScaleDegree(random, rootDegree, range, scaleView);
                    pattern.scaleReady = 1;
                }
                else
                {
                    // Native tuned pitch zero is the muted-scale sentinel, not a
                    // playable random note. Existing/manual pattern bytes stay intact.
                    const int pitch = juce::jlimit(1, 127,
                        rootMidi + offsets[static_cast<size_t>(random.nextInt(count))]);
                    a = static_cast<uint8_t>((a & 0x80) | pitch);
                }
            }
            if (kind == Randomize::velocity || kind == Randomize::sequence)
                b = static_cast<uint8_t>((b & 0x8f) | ((1 + random.nextInt(7)) << 4));
            if (kind == Randomize::events || kind == Randomize::sequence)
            {
                // Start each playable cycle with a fresh note. A tie is legal
                // only after an active step, including rotated short patterns.
                const bool start = logical == 0 || logical == pattern.length;
                const bool gate = start || random.nextInt(5) != 0;
                const bool tie = !start && gate && previousGate && random.nextInt(8) == 0;
                a = static_cast<uint8_t>((a & 0x7f) | (gate ? 0x80 : 0));
                b = static_cast<uint8_t>((b & 0x7f) | (tie ? 0x80 : 0));
                previousGate = gate;
            }
            if (kind == Randomize::value || kind == Randomize::sequence)
                b = static_cast<uint8_t>((b & 0xf0) | random.nextInt(16));
            packed = SequenceSnapshot::pack(a, b);
        }
        store(pattern);
    }

    void randomize(Randomize kind, juce::Random& random)
    {
        randomize(kind, random, NoteRange {});
    }

    bool capture(SequenceSnapshot& snapshot, uint32_t& revision) const noexcept
    {
        // One non-waiting admission attempt. Owning the gate prevents every
        // revisioned writer from overwriting payload during this fixed copy;
        // correctness therefore does not depend on revision values being unique.
        if (writer_.test_and_set(std::memory_order_acquire))
            return false;
#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
        snapshotPoint(SnapshotPoint::readerAdmitted);
#endif

        snapshot.length = length_.load(std::memory_order_relaxed);
        snapshot.rotation = rotation_.load(std::memory_order_relaxed);
        snapshot.grooveTemplate = grooveTemplate_.load(std::memory_order_relaxed);
        // Host automation publishes this independent scalar without the gate.
        // The other fields remain stable throughout its single atomic load.
        snapshot.arpPattern = arpPattern_.load(std::memory_order_acquire);
        snapshot.pitchMode = pitchMode_.load(std::memory_order_relaxed);
        snapshot.freePitchReady = freePitchReady_.load(std::memory_order_relaxed);
        snapshot.scaleReady = scaleReady_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < steps_.size(); ++i)
        {
            snapshot.steps[i] = steps_[i].load(std::memory_order_relaxed);
            snapshot.freePitch[i] = freePitch_[i].load(std::memory_order_relaxed);
            snapshot.scaleDegree[i] = scaleDegree_[i].load(std::memory_order_relaxed);
        }

        revision = revision_.load(std::memory_order_relaxed);
        writer_.clear(std::memory_order_release);
        return true;
    }

    SequenceSnapshot snapshot() const
    {
        SequenceSnapshot result;
        uint32_t ignored = 0;
        for (;;)
        {
            if (capture(result, ignored))
                return result;
            juce::Thread::yield();
        }
    }

    void resetToDefault() { store(defaultSnapshot(), true); }

    void store(const SequenceSnapshot& snapshot, bool notify = true)
    {
        beginWrite();
        length_.store(static_cast<uint8_t>(juce::jlimit(1, 16, static_cast<int>(snapshot.length))),
                      std::memory_order_relaxed);
        rotation_.store(static_cast<uint8_t>(snapshot.rotation & 0x0f),
                        std::memory_order_relaxed);
        grooveTemplate_.store(static_cast<uint8_t>(juce::jlimit(0, 5,
                                      static_cast<int>(snapshot.grooveTemplate))),
                              std::memory_order_relaxed);
        arpPattern_.store(static_cast<uint8_t>(juce::jlimit(0, 15,
                                  static_cast<int>(snapshot.arpPattern))),
                          std::memory_order_relaxed);
        const uint8_t mode = snapshot.pitchMode <= 3 ? snapshot.pitchMode : 1;
        pitchMode_.store(mode, std::memory_order_relaxed);
        freePitchReady_.store(snapshot.freePitchReady != 0 ? 1 : 0, std::memory_order_relaxed);
        scaleReady_.store(snapshot.scaleReady != 0 ? 1 : 0, std::memory_order_relaxed);
        for (size_t i = 0; i < steps_.size(); ++i)
        {
            steps_[i].store(snapshot.steps[i], std::memory_order_relaxed);
            freePitch_[i].store(FreeCvPitch::clampUnits(snapshot.freePitch[i]),
                                std::memory_order_relaxed);
            const int degree = juce::jlimit(-kScaleDegreeLimit, kScaleDegreeLimit,
                                            static_cast<int>(snapshot.scaleDegree[i]));
            scaleDegree_[i].store(static_cast<int16_t>(degree), std::memory_order_relaxed);
#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
            if (i == steps_.size() / 2 - 1)
                snapshotPoint(SnapshotPoint::writerMidpoint);
#endif
        }
        endWrite();
        if (notify)
            notifyListeners();
    }

    // Test writer. Holds the revision odd until release so an audio capture fails
    // without the audio thread waiting on this thread.
    void holdWriteForTest(std::atomic<bool>& entered, const std::atomic<bool>& release)
    {
        beginWrite();
        entered.store(true, std::memory_order_release);
        while (! release.load(std::memory_order_acquire))
        {
        }
        endWrite();
    }

    struct NotifyStats
    {
        uint32_t asyncSchedules = 0;
        uint32_t asyncDeliveries = 0;
    };

    NotifyStats notifyStatsForTests() const noexcept
    {
        return {
            listenerHub_->asyncSchedules.load(std::memory_order_relaxed),
            listenerHub_->asyncDeliveries.load(std::memory_order_relaxed)
        };
    }

    void setLength(int length)
    {
        updateAtomic(length_, static_cast<uint8_t>(juce::jlimit(1, 16, length)));
    }

    void setRotation(int rotation)
    {
        updateAtomic(rotation_, static_cast<uint8_t>(juce::jlimit(0, 15, rotation)));
    }

    void setGrooveTemplate(int grooveTemplate)
    {
        updateAtomic(grooveTemplate_,
                     static_cast<uint8_t>(juce::jlimit(0, 5, grooveTemplate)));
    }

    void setArpPattern(int arpPattern)
    {
        updateAtomic(arpPattern_, static_cast<uint8_t>(juce::jlimit(0, 15, arpPattern)));
    }

    void setArpPatternFromParameter(int arpPattern) noexcept
    {
        arpPattern_.store(static_cast<uint8_t>(juce::jlimit(0, 7, arpPattern)),
                          std::memory_order_release);
    }

    uint8_t arpPattern() const noexcept { return arpPattern_.load(std::memory_order_acquire); }

    void setStep(int index, uint8_t dataA, uint8_t dataB)
    {
        if (! juce::isPositiveAndBelow(index, SequenceSnapshot::kNumSteps))
            return;
        updateAtomic(steps_[static_cast<size_t>(index)], SequenceSnapshot::pack(dataA, dataB));
    }

    void setFreePitch(int index, int32_t units)
    {
        if (! juce::isPositiveAndBelow(index, SequenceSnapshot::kNumSteps))
            return;
        auto pattern = snapshot();
        pattern.freePitch[static_cast<size_t>(index)] = FreeCvPitch::clampUnits(units);
        pattern.freePitchReady = 1;
        store(pattern);
    }

    // First entry into free CV copies the current integer notes. Later
    // toggles keep both the packed notes and the edited CV values.
    void setScaleDegree(int index, int degree)
    {
        if (! juce::isPositiveAndBelow(index, SequenceSnapshot::kNumSteps))
            return;
        auto pattern = snapshot();
        pattern.scaleDegree[static_cast<size_t>(index)] = static_cast<int16_t>(
            juce::jlimit(-kScaleDegreeLimit, kScaleDegreeLimit, degree));
        pattern.scaleReady = 1;
        store(pattern);
    }

    // midiDegree is optional and used only the first time SCALE is entered.
    // Unmapped notes fall back to degree 0. Packed notes are not rewritten.
    void setPitchMode(uint8_t mode, const int16_t* midiDegree = nullptr, const bool* midiMapped = nullptr)
    {
        auto pattern = snapshot();
        if (mode > 3)
            return;
        const uint8_t next = mode;
        if ((next == 1 || next == 3) && pattern.freePitchReady == 0)
        {
            for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
            {
                const int note = SequenceSnapshot::dataA(pattern.steps[static_cast<size_t>(i)]) & 0x7f;
                pattern.freePitch[static_cast<size_t>(i)] = FreeCvPitch::fromMidiNote(note);
            }
            pattern.freePitchReady = 1;
        }
        if (next == 2 && pattern.scaleReady == 0)
        {
            for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
            {
                const int note = SequenceSnapshot::dataA(pattern.steps[static_cast<size_t>(i)]) & 0x7f;
                int degree = 0;
                if (midiDegree != nullptr && note < 128
                    && (midiMapped != nullptr ? midiMapped[note] : midiDegree[note] >= 0))
                    degree = midiDegree[note];
                pattern.scaleDegree[static_cast<size_t>(i)] = static_cast<int16_t>(
                    juce::jlimit(-kScaleDegreeLimit, kScaleDegreeLimit, degree));
            }
            pattern.scaleReady = 1;
        }
        pattern.pitchMode = next;
        store(pattern);
    }

    void addListener(Listener* listener)
    {
        auto hub = listenerHub_;
        const juce::ScopedLock lock(hub->lock);
        if (hub->alive.load(std::memory_order_acquire))
            hub->listeners.add(listener);
    }

    void removeListener(Listener* listener)
    {
        auto hub = listenerHub_;
        const juce::ScopedLock lock(hub->lock);
        hub->listeners.remove(listener);
    }

    juce::ValueTree toValueTree() const
    {
        const auto current = snapshot();
        juce::ValueTree tree("SEQUENCE");
        tree.setProperty("length", current.length, nullptr);
        tree.setProperty("rotation", current.rotation, nullptr);
        tree.setProperty("grooveTemplate", current.grooveTemplate, nullptr);
        tree.setProperty("arpPattern", current.arpPattern, nullptr);
        tree.setProperty("pitchMode", current.pitchMode, nullptr);
        tree.setProperty("freePitchReady", current.freePitchReady, nullptr);
        tree.setProperty("scaleReady", current.scaleReady, nullptr);
        for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
        {
            juce::ValueTree step("STEP");
            step.setProperty("index", i, nullptr);
            step.setProperty("dataA", SequenceSnapshot::dataA(current.steps[static_cast<size_t>(i)]), nullptr);
            step.setProperty("dataB", SequenceSnapshot::dataB(current.steps[static_cast<size_t>(i)]), nullptr);
            step.setProperty("cv", current.freePitch[static_cast<size_t>(i)], nullptr);
            step.setProperty("degree", static_cast<int>(current.scaleDegree[static_cast<size_t>(i)]), nullptr);
            tree.addChild(step, -1, nullptr);
        }
        return tree;
    }

    bool restoreFromValueTree(const juce::ValueTree& tree)
    {
        if (! tree.isValid() || ! tree.hasType("SEQUENCE"))
            return false;

        auto restored = defaultSnapshot();
        restored.length = static_cast<uint8_t>(juce::jlimit(1, 16,
            static_cast<int>(tree.getProperty("length", 16))));
        restored.rotation = static_cast<uint8_t>(juce::jlimit(0, 15,
            static_cast<int>(tree.getProperty("rotation", 0))));
        restored.grooveTemplate = static_cast<uint8_t>(juce::jlimit(0, 5,
            static_cast<int>(tree.getProperty("grooveTemplate", 0))));
        restored.arpPattern = static_cast<uint8_t>(juce::jlimit(0, 15,
            static_cast<int>(tree.getProperty("arpPattern", 1))));
        const int storedMode = static_cast<int>(tree.getProperty("pitchMode", 0));
        // Unknown modes must not become LEGACY: that route emits MIDI.
        // They restore as LINEAR, which stays silent and keeps packed notes.
        restored.pitchMode = static_cast<uint8_t>(storedMode >= 0 && storedMode <= 3 ? storedMode : 1);
        restored.freePitchReady = static_cast<uint8_t>(
            static_cast<int>(tree.getProperty("freePitchReady", 0)) != 0 ? 1 : 0);
        restored.scaleReady = static_cast<uint8_t>(
            static_cast<int>(tree.getProperty("scaleReady", 0)) != 0 ? 1 : 0);

        for (int childIndex = 0; childIndex < tree.getNumChildren(); ++childIndex)
        {
            const auto child = tree.getChild(childIndex);
            if (! child.hasType("STEP"))
                continue;
            const int index = static_cast<int>(child.getProperty("index", -1));
            if (! juce::isPositiveAndBelow(index, SequenceSnapshot::kNumSteps))
                continue;
            const auto dataA = static_cast<uint8_t>(juce::jlimit(0, 255,
                static_cast<int>(child.getProperty("dataA", 0))));
            const auto dataB = static_cast<uint8_t>(juce::jlimit(0, 255,
                static_cast<int>(child.getProperty("dataB", 0))));
            restored.steps[static_cast<size_t>(index)] = SequenceSnapshot::pack(dataA, dataB);
            if (child.hasProperty("cv"))
            {
                const auto cv = static_cast<int>(child.getProperty("cv", 0));
                restored.freePitch[static_cast<size_t>(index)] = FreeCvPitch::clampUnits(cv);
            }
            if (child.hasProperty("degree"))
            {
                restored.scaleDegree[static_cast<size_t>(index)] = static_cast<int16_t>(
                    juce::jlimit(-kScaleDegreeLimit, kScaleDegreeLimit,
                                 static_cast<int>(child.getProperty("degree", 0))));
            }
        }
        if ((restored.pitchMode == 1 || restored.pitchMode == 3) && restored.freePitchReady == 0)
        {
            for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
            {
                const int note = SequenceSnapshot::dataA(restored.steps[static_cast<size_t>(i)]) & 0x7f;
                restored.freePitch[static_cast<size_t>(i)] = FreeCvPitch::fromMidiNote(note);
            }
            restored.freePitchReady = 1;
        }
        if (restored.pitchMode == 2 && restored.scaleReady == 0)
            restored.scaleReady = 1;

        store(restored, true);
        return true;
    }

 private:
    struct ListenerHub
    {
        juce::CriticalSection lock;
        std::atomic<bool> alive { true };
        // Coalesced cross-thread notify: pending means "dirty since last
        // deliver start"; armed means a drain is scheduled or running.
        std::atomic<bool> pending { false };
        std::atomic<bool> armed { false };
        // Test/observability only. Not read by production audio paths.
        std::atomic<uint32_t> asyncSchedules { 0 };
        std::atomic<uint32_t> asyncDeliveries { 0 };
        juce::ListenerList<Listener> listeners;
    };

    template <typename Atomic, typename Value>
    void updateAtomic(Atomic& target, Value value, bool notify = true)
    {
        beginWrite();
        target.store(value, std::memory_order_relaxed);
        endWrite();
        if (notify)
            notifyListeners();
    }

    void notifyListeners()
    {
        // Hub is independently owned. Queued callbacks must never capture
        // SequenceState* or any field storage that dies with this object.
        auto hub = listenerHub_;
        auto* manager = juce::MessageManager::getInstanceWithoutCreating();
        if (manager == nullptr)
        {
            // Shutdown: drop the notification. Delivering on a worker would
            // violate JUCE listener-thread rules and can race teardown.
            return;
        }

        hub->pending.store(true, std::memory_order_release);
        bool expected = false;
        if (! hub->armed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // A drain is already armed. pending ensures it observes this update.
            return;
        }

        if (! manager->isThisTheMessageThread())
        {
            hub->asyncSchedules.fetch_add(1, std::memory_order_relaxed);
            if (! juce::MessageManager::callAsync(
                    [hub] { SequenceState::drainListeners(hub); }))
            {
                hub->armed.store(false, std::memory_order_release);
            }
            return;
        }
        drainListeners(hub);
    }

    static void drainListeners(const std::shared_ptr<ListenerHub>& hub)
    {
        for (;;)
        {
            hub->pending.store(false, std::memory_order_release);
            {
                const juce::ScopedLock lock(hub->lock);
                if (hub->alive.load(std::memory_order_acquire))
                {
                    // SeqPanel::sequenceStateChanged only posts AsyncUpdater work.
                    // Holding the hub lock across the call serializes removeListener.
                    hub->listeners.call([](Listener& listener)
                    {
                        listener.sequenceStateChanged();
                    });
                }
            }
            hub->asyncDeliveries.fetch_add(1, std::memory_order_relaxed);
            hub->armed.store(false, std::memory_order_release);
            if (! hub->pending.load(std::memory_order_acquire))
                return;
            // Update arrived during deliver. Re-arm on this thread if we win.
            bool expected = false;
            if (! hub->armed.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
    }

    // Kept for tests that assert a single delivery step under the hub lock.
    static void deliverListeners(const std::shared_ptr<ListenerHub>& hub)
    {
        drainListeners(hub);
    }

    void beginWrite() noexcept
    {
        while (writer_.test_and_set(std::memory_order_acquire))
        {
#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
            snapshotPoint(SnapshotPoint::writerContended);
#endif
        }
        revision_.fetch_add(1u, std::memory_order_acq_rel);
    }

    void endWrite() noexcept
    {
        revision_.fetch_add(1u, std::memory_order_release);
        writer_.clear(std::memory_order_release);
    }

    mutable std::atomic_flag writer_ = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> revision_ { 0 };
    std::atomic<uint8_t> length_ { 16 };
    std::atomic<uint8_t> rotation_ { 0 };
    std::atomic<uint8_t> grooveTemplate_ { 0 };
    std::atomic<uint8_t> arpPattern_ { 1 };
    std::atomic<uint8_t> pitchMode_ { 0 };
    std::atomic<uint8_t> freePitchReady_ { 0 };
    std::atomic<uint8_t> scaleReady_ { 0 };
    std::array<std::atomic<uint16_t>, SequenceSnapshot::kNumSteps> steps_ {};
    std::array<std::atomic<int32_t>, SequenceSnapshot::kNumSteps> freePitch_ {};
    std::array<std::atomic<int16_t>, SequenceSnapshot::kNumSteps> scaleDegree_ {};
#if defined(SWARAXT_ENABLE_SEQUENCE_SNAPSHOT_TESTS)
    void snapshotPoint(SnapshotPoint point) const noexcept
    {
        if (snapshotHook_ != nullptr) snapshotHook_(snapshotHookContext_, point);
    }
    SnapshotHook snapshotHook_ = nullptr;
    void* snapshotHookContext_ = nullptr;
#endif
    std::shared_ptr<ListenerHub> listenerHub_ { std::make_shared<ListenerHub>() };
};

}  // namespace swaraxt
