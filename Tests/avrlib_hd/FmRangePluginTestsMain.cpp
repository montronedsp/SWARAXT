// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host parameter -> PatchBridge -> HdVoice secondary parameter for Linear FM.
// Negative public ranges must reach the patch intact and render as the lowest
// FM ratio. Non-FM range remains a pitch transpose.

#include <JuceHeader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include "shruthi/patch.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

void set_int(SwaraXtAudioProcessor& processor, const char* id, int value) {
  auto* parameter = processor.getApvts().getParameter(id);
  expect(parameter != nullptr, "parameter exists");
  if (parameter == nullptr) {
    return;
  }
  parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

void prepare(SwaraXtAudioProcessor& processor, int osc1_shape, int osc1_range, int osc1_depth,
             int osc2_shape, int osc2_range, int osc2_depth) {
  processor.prepareToPlay(48000.0, 256);
  set_int(processor, swaraxt::IDs::osc1Shape, osc1_shape);
  set_int(processor, swaraxt::IDs::osc1Range, osc1_range);
  set_int(processor, swaraxt::IDs::osc1Param, osc1_depth);
  set_int(processor, swaraxt::IDs::osc2Shape, osc2_shape);
  set_int(processor, swaraxt::IDs::osc2Range, osc2_range);
  set_int(processor, swaraxt::IDs::osc2Param, osc2_depth);
  set_int(processor, swaraxt::IDs::osc1Option, 0);
  set_int(processor, swaraxt::IDs::osc2Option, 0);
  set_int(processor, swaraxt::IDs::mixNoise, 0);
  set_int(processor, swaraxt::IDs::mixSub, 0);
  set_int(processor, swaraxt::IDs::dspFxProgram, 0);
  set_int(processor, swaraxt::IDs::filterCutoff, 127);
  set_int(processor, swaraxt::IDs::env2Attack, 0);
  set_int(processor, swaraxt::IDs::env2Sustain, 127);
}

std::vector<float> render_note(SwaraXtAudioProcessor& processor, int note) {
  constexpr int kBlocks = 12;
  constexpr int kBlock = 256;
  std::vector<float> out;
  out.reserve(static_cast<size_t>(kBlocks * kBlock));
  for (int n = 0; n < kBlocks; ++n) {
    juce::AudioBuffer<float> audio(2, kBlock);
    juce::MidiBuffer midi;
    if (n == 0) {
      midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 0);
    }
    processor.processBlock(audio, midi);
    for (int i = 0; i < kBlock; ++i) {
      out.push_back(audio.getSample(0, i));
    }
  }
  return out;
}

uint8_t secondary(SwaraXtAudioProcessor& processor, int osc) {
  return processor.engineForTests().shruthiPart().mutable_voice()
      ->debug_oscillator_secondary_parameter(static_cast<uint8_t>(osc));
}

int8_t patch_range(SwaraXtAudioProcessor& processor, int osc) {
  return processor.engineForTests().shruthiPart().patch().osc[osc].range;
}

void test_host_path() {
  SwaraXtAudioProcessor negative;
  prepare(negative, shruthi::WAVEFORM_FM, -48, 64, shruthi::WAVEFORM_FM, -25, 64);
  const auto low = render_note(negative, 60);
  expect(patch_range(negative, 0) == -48, "PatchBridge keeps OSC1 range -48");
  expect(patch_range(negative, 1) == -25, "PatchBridge keeps OSC2 range -25");
  expect(secondary(negative, 0) == 0, "host OSC1 FM -48 renders secondary 0");
  expect(secondary(negative, 1) == 0, "host OSC2 FM -25 renders secondary 0");

  SwaraXtAudioProcessor floor;
  prepare(floor, shruthi::WAVEFORM_FM, -24, 64, shruthi::WAVEFORM_FM, -12, 64);
  const auto floor_audio = render_note(floor, 60);
  expect(secondary(floor, 0) == 0 && secondary(floor, 1) == 12, "host valid low FM secondaries");
  expect(low.size() == floor_audio.size() &&
             std::memcmp(low.data(), floor_audio.data(), low.size() * sizeof(float)) == 0,
         "host audio for -48/-25 matches -24/-12");

  SwaraXtAudioProcessor next;
  prepare(next, shruthi::WAVEFORM_FM, -11, 64, shruthi::WAVEFORM_SAW, -48, 0);
  const auto next_audio = render_note(next, 60);
  expect(secondary(next, 0) == 13, "host FM -11 selects the next ratio");
  expect(secondary(next, 1) == static_cast<uint8_t>(-48 + 24), "host saw -48 is not FM-clamped");
  expect(patch_range(next, 1) == -48, "saw range -48 is stored intact");
  expect(std::memcmp(next_audio.data(), floor_audio.data(), next_audio.size() * sizeof(float)) != 0,
         "host FM -11 does not sound like the lowest ratio");

  SwaraXtAudioProcessor high;
  prepare(high, shruthi::WAVEFORM_FM, 48, 64, shruthi::WAVEFORM_FM, 24, 127);
  render_note(high, 127);
  expect(patch_range(high, 0) == 48, "PatchBridge keeps OSC1 range +48");
  expect(secondary(high, 0) == 48, "host FM +48 saturates at secondary 48");
  expect(secondary(high, 1) == 48, "host FM +24 keeps secondary 48");

  SwaraXtAudioProcessor low_note;
  prepare(low_note, shruthi::WAVEFORM_FM, -36, 1, shruthi::WAVEFORM_FM, 36, 0);
  render_note(low_note, 1);
  expect(secondary(low_note, 0) == 0, "low MIDI note still clamps OSC1");
  expect(secondary(low_note, 1) == 48, "low MIDI note still saturates OSC2");
  expect(low_note.engineForTests().shruthiPart().mutable_voice()
             ->debug_oscillator_increment(0) ==
             low_note.engineForTests().shruthiPart().mutable_voice()
                 ->debug_oscillator_increment(1),
         "both FM oscillators share carrier pitch when only range differs");
}

}  // namespace

int main() {
  test_host_path();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d host FM range checks failed\n", gFailures);
    return 1;
  }
  std::printf("SwaraXTFmRangePluginTests passed\n");
  return 0;
}
