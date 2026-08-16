// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

#include "Plugin/SwaraXtParameterLayout.h"

namespace swaraxt {

// Cached raw APVTS atomics — avoid per-block string map lookups on the audio thread.
struct ParameterCache {
    void bind(juce::AudioProcessorValueTreeState& apvts)
    {
        auto bindOne = [&](const char* id) -> std::atomic<float>* {
            return apvts.getRawParameterValue(id);
        };

        master = bindOne(IDs::master);
        osc1Shape = bindOne(IDs::osc1Shape);
        osc1Param = bindOne(IDs::osc1Param);
        osc1Range = bindOne(IDs::osc1Range);
        osc1Option = bindOne(IDs::osc1Option);
        osc2Shape = bindOne(IDs::osc2Shape);
        osc2Param = bindOne(IDs::osc2Param);
        osc2Range = bindOne(IDs::osc2Range);
        osc2Option = bindOne(IDs::osc2Option);
        mixBalance = bindOne(IDs::mixBalance);
        mixSub = bindOne(IDs::mixSub);
        mixNoise = bindOne(IDs::mixNoise);
        mixSubShape = bindOne(IDs::mixSubShape);
        env1Attack = bindOne(IDs::env1Attack);
        env1Decay = bindOne(IDs::env1Decay);
        env1Sustain = bindOne(IDs::env1Sustain);
        env1Release = bindOne(IDs::env1Release);
        env2Attack = bindOne(IDs::env2Attack);
        env2Decay = bindOne(IDs::env2Decay);
        env2Sustain = bindOne(IDs::env2Sustain);
        env2Release = bindOne(IDs::env2Release);
        lfo1Wave = bindOne(IDs::lfo1Wave);
        lfo1Rate = bindOne(IDs::lfo1Rate);
        lfo1Attack = bindOne(IDs::lfo1Attack);
        lfo1Retrig = bindOne(IDs::lfo1Retrig);
        lfo1Sync = bindOne(IDs::lfo1Sync);
        lfo1Division = bindOne(IDs::lfo1Division);
        lfo2Wave = bindOne(IDs::lfo2Wave);
        lfo2Rate = bindOne(IDs::lfo2Rate);
        lfo2Attack = bindOne(IDs::lfo2Attack);
        lfo2Retrig = bindOne(IDs::lfo2Retrig);
        lfo2Sync = bindOne(IDs::lfo2Sync);
        lfo2Division = bindOne(IDs::lfo2Division);
        perfGlide = bindOne(IDs::perfGlide);
        perfLegato = bindOne(IDs::perfLegato);
        midiChannel = bindOne(IDs::midiChannel);
        seqMode = bindOne(IDs::seqMode);
        seqTempo = bindOne(IDs::seqTempo);
        seqSwing = bindOne(IDs::seqSwing);
        seqClockMode = bindOne(IDs::seqClockMode);
        seqGate = bindOne(IDs::seqGate);
        arpDirection = bindOne(IDs::arpDirection);
        arpPattern = bindOne(IDs::arpPattern);
        arpOctaves = bindOne(IDs::arpOctaves);
        arpGate = bindOne(IDs::arpGate);
        filterCutoff = bindOne(IDs::filterCutoff);
        filterResonance = bindOne(IDs::filterResonance);
        filterEnvAmount = bindOne(IDs::filterEnvAmount);
        filterKeyTracking = bindOne(IDs::filterKeyTracking);
        filterModAmount = bindOne(IDs::filterModAmount);

        for (int row = 0; row < 12; ++row)
        {
            char sourceId[32], destId[32], amountId[32];
            std::snprintf(sourceId, sizeof(sourceId), "mod.row%d.source", row + 1);
            std::snprintf(destId, sizeof(destId), "mod.row%d.destination", row + 1);
            std::snprintf(amountId, sizeof(amountId), "mod.row%d.amount", row + 1);
            modSource[static_cast<size_t>(row)] = bindOne(sourceId);
            modDest[static_cast<size_t>(row)] = bindOne(destId);
            modAmount[static_cast<size_t>(row)] = bindOne(amountId);
        }

        bound = true;
    }

    static float load(std::atomic<float>* p) noexcept
    {
        return p != nullptr ? p->load(std::memory_order_relaxed) : 0.0f;
    }

    static int loadInt(std::atomic<float>* p) noexcept
    {
        return static_cast<int>(std::lround(load(p)));
    }

    bool bound = false;
    std::atomic<float>* master = nullptr;
    std::atomic<float>* osc1Shape = nullptr;
    std::atomic<float>* osc1Param = nullptr;
    std::atomic<float>* osc1Range = nullptr;
    std::atomic<float>* osc1Option = nullptr;
    std::atomic<float>* osc2Shape = nullptr;
    std::atomic<float>* osc2Param = nullptr;
    std::atomic<float>* osc2Range = nullptr;
    std::atomic<float>* osc2Option = nullptr;
    std::atomic<float>* mixBalance = nullptr;
    std::atomic<float>* mixSub = nullptr;
    std::atomic<float>* mixNoise = nullptr;
    std::atomic<float>* mixSubShape = nullptr;
    std::atomic<float>* env1Attack = nullptr;
    std::atomic<float>* env1Decay = nullptr;
    std::atomic<float>* env1Sustain = nullptr;
    std::atomic<float>* env1Release = nullptr;
    std::atomic<float>* env2Attack = nullptr;
    std::atomic<float>* env2Decay = nullptr;
    std::atomic<float>* env2Sustain = nullptr;
    std::atomic<float>* env2Release = nullptr;
    std::atomic<float>* lfo1Wave = nullptr;
    std::atomic<float>* lfo1Rate = nullptr;
    std::atomic<float>* lfo1Attack = nullptr;
    std::atomic<float>* lfo1Retrig = nullptr;
    std::atomic<float>* lfo1Sync = nullptr;
    std::atomic<float>* lfo1Division = nullptr;
    std::atomic<float>* lfo2Wave = nullptr;
    std::atomic<float>* lfo2Rate = nullptr;
    std::atomic<float>* lfo2Attack = nullptr;
    std::atomic<float>* lfo2Retrig = nullptr;
    std::atomic<float>* lfo2Sync = nullptr;
    std::atomic<float>* lfo2Division = nullptr;
    std::atomic<float>* perfGlide = nullptr;
    std::atomic<float>* perfLegato = nullptr;
    std::atomic<float>* midiChannel = nullptr;
    std::atomic<float>* seqMode = nullptr;
    std::atomic<float>* seqTempo = nullptr;
    std::atomic<float>* seqSwing = nullptr;
    std::atomic<float>* seqClockMode = nullptr;
    std::atomic<float>* seqGate = nullptr;
    std::atomic<float>* arpDirection = nullptr;
    std::atomic<float>* arpPattern = nullptr;
    std::atomic<float>* arpOctaves = nullptr;
    std::atomic<float>* arpGate = nullptr;
    std::atomic<float>* filterCutoff = nullptr;
    std::atomic<float>* filterResonance = nullptr;
    std::atomic<float>* filterEnvAmount = nullptr;
    std::atomic<float>* filterKeyTracking = nullptr;
    std::atomic<float>* filterModAmount = nullptr;
    std::array<std::atomic<float>*, 12> modSource {};
    std::array<std::atomic<float>*, 12> modDest {};
    std::array<std::atomic<float>*, 12> modAmount {};
};

}  // namespace swaraxt
