// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <cstdint>

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

    static constexpr uint16_t pack(uint8_t dataA, uint8_t dataB) noexcept
    {
        return static_cast<uint16_t>(dataA)
            | (static_cast<uint16_t>(dataB) << 8);
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

// Message-thread edits are published as atomic snapshots. The audio thread
// either sees one complete revision or keeps its previous sequence for a block.
class SequenceState {
 public:
    class Listener {
     public:
        virtual ~Listener() = default;
        virtual void sequenceStateChanged() = 0;
    };

    SequenceState()
    {
        store(defaultSnapshot(), false);
    }

    static SequenceSnapshot defaultSnapshot() noexcept { return {}; }

    bool capture(SequenceSnapshot& snapshot, uint32_t& revision) const noexcept
    {
        const uint32_t before = revision_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
            return false;

        snapshot.length = length_.load(std::memory_order_relaxed);
        snapshot.rotation = rotation_.load(std::memory_order_relaxed);
        snapshot.grooveTemplate = grooveTemplate_.load(std::memory_order_relaxed);
        snapshot.arpPattern = arpPattern_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < steps_.size(); ++i)
            snapshot.steps[i] = steps_[i].load(std::memory_order_relaxed);

        const uint32_t after = revision_.load(std::memory_order_acquire);
        if (before != after || (after & 1u) != 0u)
            return false;

        revision = after;
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
        for (size_t i = 0; i < steps_.size(); ++i)
            steps_[i].store(snapshot.steps[i], std::memory_order_relaxed);
        endWrite();
        if (notify)
            listeners_.call([](Listener& listener) { listener.sequenceStateChanged(); });
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

    void addListener(Listener* listener) { listeners_.add(listener); }
    void removeListener(Listener* listener) { listeners_.remove(listener); }

    juce::ValueTree toValueTree() const
    {
        const auto current = snapshot();
        juce::ValueTree tree("SEQUENCE");
        tree.setProperty("length", current.length, nullptr);
        tree.setProperty("rotation", current.rotation, nullptr);
        tree.setProperty("grooveTemplate", current.grooveTemplate, nullptr);
        tree.setProperty("arpPattern", current.arpPattern, nullptr);
        for (int i = 0; i < SequenceSnapshot::kNumSteps; ++i)
        {
            juce::ValueTree step("STEP");
            step.setProperty("index", i, nullptr);
            step.setProperty("dataA", SequenceSnapshot::dataA(current.steps[static_cast<size_t>(i)]), nullptr);
            step.setProperty("dataB", SequenceSnapshot::dataB(current.steps[static_cast<size_t>(i)]), nullptr);
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
        }

        store(restored, true);
        return true;
    }

 private:
    template <typename Atomic, typename Value>
    void updateAtomic(Atomic& target, Value value, bool notify = true)
    {
        beginWrite();
        target.store(value, std::memory_order_relaxed);
        endWrite();
        if (notify)
            listeners_.call([](Listener& listener) { listener.sequenceStateChanged(); });
    }

    void beginWrite() noexcept
    {
        while (writer_.test_and_set(std::memory_order_acquire)) {}
        revision_.fetch_add(1u, std::memory_order_acq_rel);
    }

    void endWrite() noexcept
    {
        revision_.fetch_add(1u, std::memory_order_release);
        writer_.clear(std::memory_order_release);
    }

    std::atomic_flag writer_ = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> revision_ { 0 };
    std::atomic<uint8_t> length_ { 16 };
    std::atomic<uint8_t> rotation_ { 0 };
    std::atomic<uint8_t> grooveTemplate_ { 0 };
    std::atomic<uint8_t> arpPattern_ { 1 };
    std::array<std::atomic<uint16_t>, SequenceSnapshot::kNumSteps> steps_ {};
    juce::ListenerList<Listener> listeners_;
};

}  // namespace swaraxt
