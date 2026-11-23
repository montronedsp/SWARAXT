// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include <JuceHeader.h>
#include "Engine/ParameterCache.h"
#include "Engine/PatchBridge.h"
#include "Plugin/PluginProcessor.h"
#include "Plugin/SwaraXtParameterLayout.h"
#include <array>
#include <cstdio>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace {
int failures = 0;
void check(bool okay, const char* message) {
  if (!okay) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
void set(SwaraXtAudioProcessor& p, const char* id, int value) {
  auto* parameter = p.getApvts().getParameter(id);
  check(parameter != nullptr, "fixture parameter exists");
  if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}
void bridgeRange() {
  SwaraXtAudioProcessor processor;
  processor.prepareToPlay(48000, 128);
  swaraxt::ParameterCache cache;
  cache.bind(processor.getApvts());
  swaraxt::PatchBridge bridge;
  auto& part = processor.engineForTests().shruthiPart();
  bridge.bind(part);
  const std::array<int, 12> inputs {-1024, -128, -1, 0, 1, 2, 3, 4, 5, 127, 255, 1024};
  for (int input : inputs) {
    // APVTS normally limits this parameter; exercise its cached raw boundary.
    cache.arpOctaves->store(static_cast<float>(input), std::memory_order_relaxed);
    bridge.applyCacheToEngine(cache);
    const int actual = part.sequencer_settings().arp_range;
    const int expected = input < 1 ? 1 : (input > 4 ? 4 : input);
    check(actual == expected, "APVTS bridge clamps range before unsigned narrowing");
    std::printf("bridge range_input=%d actual=%d expected=%d\n", input, actual, expected);
  }
  cache.arpOctaves->store(1, std::memory_order_relaxed);
}
struct Host : juce::AudioPlayHead {
  bool playing = true;
  double ppq = 0;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setBpm(120);
    info.setPpqPosition(ppq);
    info.setIsPlaying(playing);
    return info;
  }
};
struct Ledger {
  std::array<int, 16 * 128> held {};
  int ons = 0, first = -1, orphan = 0;
  void accept(const juce::MidiBuffer& midi) {
    for (auto event : midi) {
      const auto message = event.getMessage();
      if (!message.isNoteOnOrOff()) continue;
      const auto index = static_cast<size_t>((message.getChannel() - 1) * 128 + message.getNoteNumber());
      check(event.samplePosition >= 0 && event.samplePosition < 128, "generated MIDI timestamp is within its callback");
      if (message.isNoteOn()) {
        if (ons++ == 0) first = message.getNoteNumber();
        ++held[index];
      } else if (held[index] == 0) ++orphan;
      else --held[index];
    }
  }
  bool closed() const {
    for (auto count : held) if (count != 0) return false;
    return orphan == 0;
  }
};
void publicMidiAndRestore() {
  struct Case { int note, transpose, expected; };
  constexpr Case cases[] {{96,36,120}, {80,48,116}, {127,67,122}, {60,0,60}};
  for (int mode = 0; mode < 4; ++mode) for (int hostClock = 0; hostClock < 2; ++hostClock)
    for (const auto& test : cases) {
      SwaraXtAudioProcessor p;
      Host host;
      if (hostClock) p.setPlayHead(&host);
      check(p.loadBuiltinTuning(0), "12-EDO public MIDI fixture");
      set(p, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_SEQ);
      set(p, swaraxt::IDs::seqClockMode, hostClock);
      set(p, swaraxt::IDs::seqTempo, 120);
      set(p, swaraxt::IDs::seqMidiOut, 1);
      set(p, swaraxt::IDs::arpGate, 11);
      auto sequence = p.sequenceState().snapshot();
      sequence.length = 1;
      sequence.rotation = 0;
      sequence.pitchMode = static_cast<uint8_t>(mode);
      sequence.freePitchReady = sequence.scaleReady = 1;
      sequence.steps.fill(swaraxt::SequenceSnapshot::pack(static_cast<uint8_t>(0x80 | test.note), 0x70));
      sequence.freePitch.fill(60 * 1024);
      sequence.scaleDegree.fill(0);
      p.sequenceState().store(sequence);
      juce::MemoryBlock state;
      p.getStateInformation(state);
      {
        SwaraXtAudioProcessor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const auto actual = restored.sequenceState().snapshot();
        check(actual.steps == sequence.steps && actual.pitchMode == sequence.pitchMode
              && actual.freePitch == sequence.freePitch && actual.scaleDegree == sequence.scaleDegree,
              "high notes and independent pitch coordinates survive sequence state restore");
      }
      p.prepareToPlay(48000, 128);
      juce::AudioBuffer<float> audio(2, 128);
      juce::MidiBuffer midi;
      midi.ensureSize(8192);
      Ledger ledger;
      for (int index = 0; index < 64; ++index) {
        midi.clear();
        if (index == 0) midi.addEvent(juce::MidiMessage::noteOn(1, test.transpose + 60, juce::uint8(100)), 0);
        if (index == 48) midi.addEvent(juce::MidiMessage::noteOff(1, test.transpose + 60), 0);
        p.processBlock(audio, midi);
        ledger.accept(midi);
        host.ppq += 256.0 / 48000;
      }
      host.playing = false;
      if (!hostClock) set(p, swaraxt::IDs::seqMode, shruthi::SEQUENCER_MODE_STEP);
      for (int i = 0; i < 8; ++i) {
        midi.clear();
        p.processBlock(audio, midi);
        ledger.accept(midi);
      }
      const bool emits = mode == 0 || mode == 3;
      check(emits ? ledger.ons > 0 : ledger.ons == 0, "existing public MIDI policy for all four modes");
      if (emits) check(ledger.first == (mode == 0 ? test.expected : 60 + test.transpose), "public legacy/quantized MIDI note identity");
      check(ledger.closed(), "public MIDI note-on/off pairing after clock stop or mode exit");
      std::printf("public mode=%d host_clock=%d step=%d transpose=%d first=%d ons=%d orphan_offs=%d\n",
                  mode, hostClock, test.note, test.transpose, ledger.first, ledger.ons, ledger.orphan);
      p.setPlayHead(nullptr);
    }
}
} // namespace
int main() {
#if defined(__linux__)
  if (prctl(PR_SET_DUMPABLE, 0) != 0) return 2;
#endif
  juce::ScopedJuceInitialiser_GUI init;
  bridgeRange();
  publicMidiAndRestore();
  std::printf("Sequencer safety bridge failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
