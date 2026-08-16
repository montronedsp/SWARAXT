// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace swaraxt {

namespace IDs {
static constexpr const char* master = "master.level";
static constexpr const char* osc1Shape = "osc1.shape";
static constexpr const char* osc1Param = "osc1.parameter";
static constexpr const char* osc1Range = "osc1.range";
static constexpr const char* osc1Option = "osc1.option";
static constexpr const char* osc2Shape = "osc2.shape";
static constexpr const char* osc2Param = "osc2.parameter";
static constexpr const char* osc2Range = "osc2.range";
static constexpr const char* osc2Option = "osc2.option";
static constexpr const char* mixBalance = "mix.balance";
static constexpr const char* mixSub = "mix.sub";
static constexpr const char* mixNoise = "mix.noise";
static constexpr const char* mixSubShape = "mix.subShape";
static constexpr const char* env1Attack = "env1.attack";
static constexpr const char* env1Decay = "env1.decay";
static constexpr const char* env1Sustain = "env1.sustain";
static constexpr const char* env1Release = "env1.release";
static constexpr const char* env2Attack = "env2.attack";
static constexpr const char* env2Decay = "env2.decay";
static constexpr const char* env2Sustain = "env2.sustain";
static constexpr const char* env2Release = "env2.release";
static constexpr const char* lfo1Wave = "lfo1.waveform";
static constexpr const char* lfo1Rate = "lfo1.rate";
static constexpr const char* lfo1Attack = "lfo1.attack";
static constexpr const char* lfo1Retrig = "lfo1.retrigger";
static constexpr const char* lfo1Sync = "lfo1.sync";
static constexpr const char* lfo1Division = "lfo1.division";
static constexpr const char* lfo2Wave = "lfo2.waveform";
static constexpr const char* lfo2Rate = "lfo2.rate";
static constexpr const char* lfo2Attack = "lfo2.attack";
static constexpr const char* lfo2Retrig = "lfo2.retrigger";
static constexpr const char* lfo2Sync = "lfo2.sync";
static constexpr const char* lfo2Division = "lfo2.division";
static constexpr const char* perfGlide = "perf.portamento";
static constexpr const char* perfLegato = "perf.legato";
static constexpr const char* seqMode = "seq.mode";
static constexpr const char* seqTempo = "seq.tempo";
static constexpr const char* seqSwing = "seq.swing";
static constexpr const char* seqClockMode = "seq.clockMode";
static constexpr const char* seqGate = "seq.gate";
static constexpr const char* arpDirection = "arp.direction";
static constexpr const char* arpPattern = "arp.pattern";
static constexpr const char* arpOctaves = "arp.octaves";
static constexpr const char* arpGate = "arp.gate";
static constexpr const char* filterCutoff = "filter_cutoff";
static constexpr const char* filterResonance = "filter_resonance";
static constexpr const char* filterEnvAmount = "filter_env_amount";
static constexpr const char* filterKeyTracking = "filter_key_tracking";
static constexpr const char* filterModAmount = "filter_mod_amount";
static constexpr const char* midiChannel = "midi.channel";
}  // namespace IDs

juce::AudioProcessorValueTreeState::ParameterLayout createSwaraXtParameterLayout();
float getParameterValue(const juce::AudioProcessorValueTreeState& apvts, const char* id);
int getParameterInt(const juce::AudioProcessorValueTreeState& apvts, const char* id);

}  // namespace swaraxt
