// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shruthi/part.h"
#include "shruthi/patch.h"
#include "shruthi/sequencer_settings.h"
#include "shruthi/system_settings.h"

#include "Engine/PatchBridge.h"
#include "Engine/ParameterCache.h"

#include <JuceHeader.h>

namespace swaraxt {

namespace {

uint8_t clampU8(int value)
{
    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return static_cast<uint8_t>(value);
}

int8_t clampS8(int value)
{
    if (value < -128)
        return -128;
    if (value > 127)
        return 127;
    return static_cast<int8_t>(value);
}

// Parameter ranges are wider than the engine enums (they were sized to a flat
// 0..31 / 0..35 grid), so every index must be clamped to a defined enum value
// before it reaches Shruthi: Voice indexes fixed-size arrays with them.
constexpr int kLastWaveform = static_cast<int>(shruthi::WAVEFORM_LAST) - 1;
constexpr int kLastModulationSource = static_cast<int>(shruthi::kNumModulationSources) - 1;
constexpr int kLastModulationDestination = static_cast<int>(shruthi::kNumModulationDestinations) - 1;

}  // namespace

void PatchBridge::applyCacheToEngine(const ParameterCache& cache)
{
    if (! cache.bound || part_ == nullptr)
        return;

    auto* patch = part_->mutable_patch();

    patch->osc[0].shape = static_cast<uint8_t>(juce::jlimit(0, kLastWaveform, ParameterCache::loadInt(cache.osc1Shape)));
    patch->osc[0].parameter = static_cast<uint8_t>(juce::jlimit(0, 127, ParameterCache::loadInt(cache.osc1Param)));
    patch->osc[0].range = clampS8(ParameterCache::loadInt(cache.osc1Range));
    patch->osc[0].option = static_cast<uint8_t>(juce::jlimit(0, 13, ParameterCache::loadInt(cache.osc1Option)));

    patch->osc[1].shape = static_cast<uint8_t>(juce::jlimit(0, kLastWaveform, ParameterCache::loadInt(cache.osc2Shape)));
    patch->osc[1].parameter = static_cast<uint8_t>(juce::jlimit(0, 127, ParameterCache::loadInt(cache.osc2Param)));
    patch->osc[1].range = clampS8(ParameterCache::loadInt(cache.osc2Range));
    patch->osc[1].option = static_cast<uint8_t>(juce::jlimit(0, 127, ParameterCache::loadInt(cache.osc2Option)));

    patch->mix_balance = static_cast<uint8_t>(ParameterCache::loadInt(cache.mixBalance));
    patch->mix_sub_osc = static_cast<uint8_t>(ParameterCache::loadInt(cache.mixSub));
    patch->mix_noise = static_cast<uint8_t>(ParameterCache::loadInt(cache.mixNoise));
    patch->mix_sub_osc_shape = static_cast<uint8_t>(juce::jlimit(0, 10, ParameterCache::loadInt(cache.mixSubShape)));

    const shruthi::EnvelopeSettings envelopeSettings[2] {
        {
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env1Attack)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env1Decay)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env1Sustain)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env1Release))
        },
        {
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env2Attack)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env2Decay)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env2Sustain)),
            static_cast<uint8_t>(ParameterCache::loadInt(cache.env2Release))
        }
    };
    const bool envelopeRatesChanged =
        patch->env[0].attack != envelopeSettings[0].attack
        || patch->env[0].decay != envelopeSettings[0].decay
        || patch->env[0].sustain != envelopeSettings[0].sustain
        || patch->env[0].release != envelopeSettings[0].release
        || patch->env[1].attack != envelopeSettings[1].attack
        || patch->env[1].decay != envelopeSettings[1].decay
        || patch->env[1].sustain != envelopeSettings[1].sustain
        || patch->env[1].release != envelopeSettings[1].release;
    patch->env[0] = envelopeSettings[0];
    patch->env[1] = envelopeSettings[1];
    if (envelopeRatesChanged)
        part_->mutable_voice()->RefreshEnvelopeRatesFromPatch();

    const uint8_t lfo1Wave = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo1Wave));
    const uint8_t lfo1Rate = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo1Rate));
    const uint8_t lfo1Attack = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo1Attack));
    const uint8_t lfo1Retrig = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo1Retrig));
    const uint8_t lfo2Wave = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo2Wave));
    const uint8_t lfo2Rate = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo2Rate));
    const uint8_t lfo2Attack = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo2Attack));
    const uint8_t lfo2Retrig = static_cast<uint8_t>(ParameterCache::loadInt(cache.lfo2Retrig));

    // Touch() must run after LFO rate/shape writes so Part recomputes phase increments.
    const bool modulationRatesDirty =
        patch->lfo[0].waveform != lfo1Wave
        || patch->lfo[0].rate != lfo1Rate
        || patch->lfo[0].attack != lfo1Attack
        || patch->lfo[0].retrigger_mode != lfo1Retrig
        || patch->lfo[1].waveform != lfo2Wave
        || patch->lfo[1].rate != lfo2Rate
        || patch->lfo[1].attack != lfo2Attack
        || patch->lfo[1].retrigger_mode != lfo2Retrig;

    patch->lfo[0].waveform = lfo1Wave;
    patch->lfo[0].rate = lfo1Rate;
    patch->lfo[0].attack = lfo1Attack;
    patch->lfo[0].retrigger_mode = lfo1Retrig;

    patch->lfo[1].waveform = lfo2Wave;
    patch->lfo[1].rate = lfo2Rate;
    patch->lfo[1].attack = lfo2Attack;
    patch->lfo[1].retrigger_mode = lfo2Retrig;

    for (int row = 0; row < shruthi::kModulationMatrixSize; ++row)
    {
        patch->modulation_matrix.modulation[row].source =
            static_cast<uint8_t>(juce::jlimit(0, kLastModulationSource,
                                              ParameterCache::loadInt(cache.modSource[static_cast<size_t>(row)])));
        patch->modulation_matrix.modulation[row].destination =
            static_cast<uint8_t>(juce::jlimit(0, kLastModulationDestination,
                                              ParameterCache::loadInt(cache.modDest[static_cast<size_t>(row)])));
        patch->modulation_matrix.modulation[row].amount =
            clampS8(ParameterCache::loadInt(cache.modAmount[static_cast<size_t>(row)]));
    }

    auto* settings = part_->mutable_system_settings();
    settings->portamento = static_cast<uint8_t>(ParameterCache::loadInt(cache.perfGlide));
    settings->legato = static_cast<uint8_t>(ParameterCache::loadInt(cache.perfLegato));
    settings->midi_channel = static_cast<uint8_t>(juce::jlimit(0, 16, ParameterCache::loadInt(cache.midiChannel)));
    settings->midi_out_mode = shruthi::MIDI_OUT_OFF;

    auto* seq = part_->mutable_sequencer_settings();
    seq->seq_mode = static_cast<uint8_t>(juce::jlimit(0, 2, ParameterCache::loadInt(cache.seqMode)));
    const bool hostClock = ParameterCache::loadInt(cache.seqClockMode) != 0;
    seq->seq_tempo = hostClock
        ? 0
        : static_cast<uint8_t>(juce::jlimit(40, 240, ParameterCache::loadInt(cache.seqTempo)));
    if (seq->seq_groove_template > 5)
        seq->seq_groove_template = 0;
    seq->seq_groove_amount = static_cast<uint8_t>(ParameterCache::loadInt(cache.seqSwing));
    seq->arp_pattern = static_cast<uint8_t>(ParameterCache::loadInt(cache.arpPattern));
    seq->arp_range = static_cast<uint8_t>(ParameterCache::loadInt(cache.arpOctaves));
    seq->arp_clock_division = static_cast<uint8_t>(juce::jlimit(0, 11, ParameterCache::loadInt(cache.arpGate)));
    part_->SetGatePercent(static_cast<uint8_t>(
        juce::jlimit(5, 100, ParameterCache::loadInt(cache.seqGate))));

    // Keep Part::arp_direction_ coherent with the settings byte.
    // Only call SetParameter when the target changes. After Init/bulk load the
    // byte may already match while arp_direction_ is stale — force once via
    // invalidateArpRuntimeSync() from engine reset.
    const uint8_t arpDir = static_cast<uint8_t>(ParameterCache::loadInt(cache.arpDirection));
    if (arpRuntimeNeedsForceSync_ || seq->arp_direction != arpDir)
    {
        if (seq->arp_direction == arpDir)
            seq->arp_direction = static_cast<uint8_t>(arpDir == 0 ? 1 : 0);
        part_->SetParameter(0, shruthi::PRM_ARP_DIRECTION, arpDir, false);
        arpRuntimeNeedsForceSync_ = false;
    }

    if (modulationRatesDirty)
        part_->Touch(false);
}

}  // namespace swaraxt
