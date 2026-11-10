// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#include "Engine/FreeCvPitch.h"
#include "Tuning/PublishedTuning.h"
#include "Tuning/ScalePitch.h"
#include "avrlib/random.h"
#include "shruthi/audio_out.h"
#include "shruthi/midi_dispatcher.h"
#include "shruthi/part.h"
#include "shruthi/resources.h"
#include "shruthi/storage.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace {
int failures = 0;
void check(bool okay, const char* message) {
  if (!okay) {
    if (failures < 16) std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}
void hashByte(uint64_t& hash, uint8_t value) noexcept {
  hash = (hash ^ value) * UINT64_C(1099511628211);
}
struct Notes {
  std::array<unsigned, 256> active {};
  unsigned ons = 0, offs = 0, bad = 0;
  uint8_t last = 0, external = 0;
  uint64_t hash = UINT64_C(1469598103934665603);
  static void on(void* context, uint8_t note, uint8_t velocity, uint8_t externalNote) noexcept {
    auto& self = *static_cast<Notes*>(context);
    self.bad += self.active[note] != 0;
    ++self.active[note];
    ++self.ons;
    self.last = note;
    self.external = externalNote;
    for (auto byte : {uint8_t{0x90}, note, velocity, externalNote}) hashByte(self.hash, byte);
  }
  static void off(void* context, uint8_t note) noexcept {
    auto& self = *static_cast<Notes*>(context);
    if (self.active[note] == 0) ++self.bad;
    else --self.active[note];
    ++self.offs;
    hashByte(self.hash, 0x80);
    hashByte(self.hash, note);
  }
  bool closed() const {
    for (auto count : active) if (count != 0) return false;
    return bad == 0 && ons == offs;
  }
};
struct Runtime {
  shruthi::HostAudioRing ring;
  avrlib::Random random;
  shruthi::MidiDispatcher midi;
  shruthi::Storage storage;
  shruthi::Part part;
  Runtime() {
    ring.Init();
    random.Seed(0x1234);
    part.Init(ring, random, midi, storage);
    auto* settings = part.mutable_sequencer_settings();
    settings->seq_tempo = 0;
    settings->seq_mode = shruthi::SEQUENCER_MODE_SEQ;
    settings->pattern_size = 16;
    settings->pattern_rotation = 0;
    settings->arp_clock_division = 11;
    for (uint8_t i = 0; i < 16; ++i) part.SetSequenceStep(i, uint8_t(0x80 | (60 + i)), 0x70);
    part.mutable_system_settings()->portamento = 0;
    part.mutable_system_settings()->legato = 0;
    part.mutable_system_settings()->raga = 0;
  }
  void tap(Notes& notes) { midi.setSequencerNoteTap({Notes::on, Notes::off, &notes}); }
};
void tick(shruthi::Part& part) noexcept
#if defined(__clang__)
    [[clang::nonblocking]]
#endif
{
  part.Clock(false);
}
void block(shruthi::Part& part, bool audio) noexcept
#if defined(__clang__)
    [[clang::nonblocking]]
#endif
{
  if (audio) part.ProcessBlock();
  else part.ProcessControlBlock();
}

// Independent signed ROM values and phase-threshold oracle, not Clock::Wrap.
constexpr int grooves[6][16] = {
  {127,127,-127,-127,127,127,-127,-127,127,127,-127,-127,127,127,-127,-127},
  {127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127},
  {-63,-63,127,0,-127,0,0,88,0,0,88,-50,-88,0,88,0},
  {19,44,93,-4,32,-53,-90,-127,117,32,-102,-53,105,-53,93,-53},
  {88,-101,107,-95,88,-88,50,-38,65,-88,101,-95,101,-127,63,-31},
  {70,-84,84,-112,84,-98,112,-98,54,-70,127,-84,127,-112,84,-84}
};
void grooveTests() {
  for (bool audio : {false, true}) for (unsigned groove = 0; groove < 6; ++groove)
    for (uint8_t amount : {uint8_t{0}, uint8_t{64}, uint8_t{127}}) {
      Runtime runtime;
      Notes notes;
      runtime.tap(notes);
      auto& part = runtime.part;
      auto* settings = part.mutable_sequencer_settings();
      settings->seq_tempo = 120;
      settings->seq_groove_template = static_cast<uint8_t>(groove);
      settings->seq_groove_amount = amount;
      part.Start(true);
      part.NoteOn(0, 60, 100);
      check(part.step() == 255 && notes.ons == 0, "Start and held key retain the pre-clock sentinel");
      uint64_t phase = 0;
      const uint32_t increment = pgm_read_dword(shruthi::lut_res_tempo_phase_increment + 80);
      int swing = 0;
      unsigned wraps = 0, expectedStep = 255, firstBlock = 0;
      for (unsigned index = 1; index <= 4000 && wraps < 32; ++index) {
        phase += increment;
        const uint64_t threshold = static_cast<uint64_t>(128 + swing) * (UINT64_C(1) << 24);
        if (phase >= threshold) {
          phase -= threshold;
          const int product = grooves[groove][expectedStep % 16] * amount;
          swing = product >= 0 ? product / 512 : -((-product + 511) / 512);
          expectedStep = (expectedStep + 1) % 16;
          ++wraps;
          if (wraps == 1) firstBlock = index;
        }
        block(part, audio);
        check(part.step() == expectedStep, "internal clock step matches independent block timing");
        check(notes.ons == wraps, "exactly one sequencer note per internal clock wrap");
        if (wraps != 0) check(notes.last == 60 + expectedStep, "startup and ordinary step note order");
        if (audio) {
          float samples[40] {};
          check(runtime.ring.readBlock(samples, 40, part.voice().vca(), .85f) == 40, "startup renders a complete audio block");
          for (float sample : samples) check(std::isfinite(sample), "startup audio remains finite");
        }
      }
      check(wraps == 32, "all ordinary groove indices visited twice within bounded time");
      part.Stop(false);
      check(notes.closed(), "internal clock notes release with matching identities");
      std::printf("groove=%u amount=%u audio=%d first_block=%u wraps=%u\n", groove, unsigned(amount), int(audio), firstBlock, wraps);
    }
}

void fillTuning(swaraxt::tuning::PublishedTuning& tuning) {
  tuning.valid = true;
  tuning.degreeCount = tuning.formalPeriod = 12;
  tuning.referenceNote = 69;
  tuning.referenceHz = 440;
  tuning.periodCents = 1200;
  for (int i = 0; i < 12; ++i) tuning.degreeCents[i] = (i + 1) * 100;
  for (int i = 0; i < 128; ++i) {
    tuning.midiMapped[i] = true;
    tuning.midiDegree[i] = static_cast<int16_t>(i - 60);
  }
  tuning.buildQuantizer();
}
int expectedNote(int sum) {
  if (sum < 0) return sum + 12 * ((11 - sum) / 12);
  if (sum > 127) return sum - 12 * ((sum - 127 + 11) / 12);
  return sum;
}
void transposeTests() {
  swaraxt::tuning::PublishedTuning tuning;
  fillTuning(tuning);
  for (int mode = 0; mode < 4; ++mode) {
    Runtime runtime;
    auto& part = runtime.part;
    part.mutable_sequencer_settings()->pattern_size = 1;
    std::array<int16_t, 16> degrees {};
    degrees.fill(5);
    std::array<int32_t, 16> units {};
    units.fill(60 * 1024 + 128);
    part.setSequencerFreeCv(mode == 1, units.data());
    part.setSequencerScale(mode == 2, degrees.data(), &tuning);
    part.setSequencerQuantizedCv(mode == 3, units.data(), &tuning);
    unsigned cases = 0, wrong = 0;
    uint64_t pitchHash = UINT64_C(1469598103934665603);
    for (int note = 0; note < 128; ++note) for (int transpose = -60; transpose <= 67; ++transpose) {
      Notes notes;
      runtime.tap(notes);
      part.SetSequenceStep(0, static_cast<uint8_t>(0x80 | note), 0x70);
      part.Start(false);
      part.NoteOn(0, static_cast<uint8_t>(transpose + 60), 100);
      tick(part);
      const int sum = note + transpose;
      const int expected = expectedNote(sum);
      wrong += notes.last != expected;
      check(notes.ons == 1 && notes.last == expected, "wide transpose and octave-folded note identity");
      check(notes.last <= 127 && notes.last % 12 == (sum % 12 + 12) % 12, "MIDI bounds and pitch class");
      if (sum >= 0 && sum <= 127) check(notes.last == sum, "all originally in-range sums preserved");
      if (mode == 0 && expected != 0)
        check(part.voice().pitch_value() == expected * 128, "legacy voice pitch follows the folded MIDI identity");
      if (mode != 0) {
        const int coordinate = 60 * 1024 + 128 + transpose * 1024;
        uint16_t pitch = 0;
        if (mode == 1) pitch = swaraxt::FreeCvPitch::toVoicePitch(coordinate);
        if (mode == 2) pitch = swaraxt::ScalePitch::toVoicePitchFromHz(tuning.hzForDegree(5 + transpose));
        if (mode == 3) {
          const auto chosen = tuning.quantizeHz(swaraxt::FreeCvPitch::hzFromUnits(coordinate));
          check(chosen.valid, "quantized CV chooses a valid pitch");
          pitch = swaraxt::ScalePitch::toVoicePitchFromHz(chosen.hz);
          check(notes.external == swaraxt::ScalePitch::nearestMidiNote(chosen.hz), "quantized external note remains its independent pitch identity");
        }
        if (pitch == 0) pitch = 128;
        check(part.voice().pitch_value() == pitch, "separate degree/CV pitch calculation preserved");
        hashByte(pitchHash, static_cast<uint8_t>(part.voice().pitch_value()));
        hashByte(pitchHash, static_cast<uint8_t>(part.voice().pitch_value() >> 8));
      }
      if ((note == 96 && transpose == 36) || (note == 80 && transpose == 48)
          || (note == 127 && transpose == 67) || (note == 60 && transpose == 0))
        std::printf("mode=%d step=%d transpose=%d actual=%u expected=%d\n", mode, note, transpose, unsigned(notes.last), expected);
      part.NoteOff(0, static_cast<uint8_t>(transpose + 60));
      tick(part);
      check(notes.last == expected && notes.ons == 2, "released key retains sequencer transposition");
      part.Stop(false);
      check(notes.closed(), "all four modes match note-on/off identities after key release and stop");
      ++cases;
    }
    // Changed identities across a legato transition must release both notes.
    Notes legato;
    runtime.tap(legato);
    part.mutable_sequencer_settings()->pattern_size = 2;
    part.SetSequenceStep(0, 0x80 | 96, 0x70);
    part.SetSequenceStep(1, 0x80 | 97, 0xf0);
    part.Start(false);
    part.NoteOn(0, 96, 100);
    tick(part);
    check(legato.last == 120, "legato first high note identity");
    tick(part);
    check(legato.last == 121, "legato second high note identity");
    part.Stop(false);
    check(legato.closed(), "legato transition and stop close both note identities");
    std::printf("transpose mode=%d cases=%u wrong_identities=%u pitch_hash=%016llx\n", mode, cases, wrong, static_cast<unsigned long long>(pitchHash));
  }
}

// Filled from untouched e62bb3fd51f1c514c87781d9936093679fc5e7a7 via arp-trace.
constexpr uint64_t arpBaseline[40] = {
  UINT64_C(0xaa3846e721816783), UINT64_C(0xd06b15aca98e51c7), UINT64_C(0xaa3846e721816783), UINT64_C(0x70f37e2c2136e746),
  UINT64_C(0xaa3846e721816783), UINT64_C(0x1be3728788f5d383), UINT64_C(0xaa3846e721816783), UINT64_C(0x0e8e61e038093b56),
  UINT64_C(0xaa3846e721816783), UINT64_C(0x43f1c8ddb677d94a), UINT64_C(0xe55bf78da0649983), UINT64_C(0x2b20508d80e7429b),
  UINT64_C(0x117c7c3613029983), UINT64_C(0x593fff2c518dbcb2), UINT64_C(0xe55bf78da0649983), UINT64_C(0xa0c2429fc7181313),
  UINT64_C(0x545f96aa3d715ff7), UINT64_C(0xb63484f5a8832766), UINT64_C(0xe55bf78da0649983), UINT64_C(0x760d8106648799ee),
  UINT64_C(0x4e7b16de87cd9757), UINT64_C(0xc3cf0caf4acc6e17), UINT64_C(0xcf85d30607585e2f), UINT64_C(0x26ede7e8569db916),
  UINT64_C(0x021cd017fb9c2583), UINT64_C(0x195e3cac3c28a183), UINT64_C(0xf9aa33df24ad65ab), UINT64_C(0x7faf714db3424f8a),
  UINT64_C(0x4e7b16de87cd9757), UINT64_C(0xc70b5617a795bcda), UINT64_C(0x1fb250db9a8a7b83), UINT64_C(0xbb106f278a4f0e0b),
  UINT64_C(0x16fd197a39351b83), UINT64_C(0x1621df685b10a2d2), UINT64_C(0x17abe85b2ceebabb), UINT64_C(0xf4d6878b3ba10222),
  UINT64_C(0x01b2ed1eee3cc10f), UINT64_C(0xe91f2838f5689c9e), UINT64_C(0x1fb250db9a8a7b83), UINT64_C(0x0911d3e6f00c10be)
};
uint64_t arpTrace(uint8_t range, uint8_t direction, unsigned keyCount) {
  Runtime runtime;
  auto& part = runtime.part;
  auto* settings = part.mutable_sequencer_settings();
  settings->seq_mode = shruthi::SEQUENCER_MODE_ARP;
  settings->arp_pattern = 15;
  settings->arp_range = range;
  settings->arp_direction = direction;
  for (unsigned i = 0; i < keyCount; ++i) part.NoteOn(0, std::array<uint8_t, 3>{67, 60, 64}[i], 100);
  part.Start(false);
  Notes notes;
  runtime.tap(notes);
  for (unsigned i = 0; i < 128; ++i) {
    runtime.random.Update();
    tick(part);
    check(notes.last <= 127, "arpeggiator note remains in MIDI bounds");
  }
  part.Stop(false);
  check(notes.ons == 128 && notes.closed(), "arpeggiator direction/range notes pair through stop");
  return notes.hash;
}
void arpValid(bool capture) {
  unsigned index = 0;
  for (uint8_t range = 1; range <= 4; ++range) for (uint8_t direction = 0; direction <= 4; ++direction)
    for (unsigned keys : {1u, 3u}) {
      const auto hash = arpTrace(range, direction, keys);
      std::printf("arp range=%u direction=%u keys=%u hash=%016llx\n", unsigned(range), unsigned(direction), keys, static_cast<unsigned long long>(hash));
      if (!capture) check(hash == arpBaseline[index], "ranges 1-4 exactly match untouched firmware event traces");
      ++index;
    }
}
void arpInvalid(bool zeroOnly) {
  // Zero runs only as a dedicated CTest child with a five-second timeout.
  const std::array<uint8_t, 4> ranges {0, 5, 127, 255};
  for (auto range : ranges) {
    if (zeroOnly && range != 0) continue;
    for (uint8_t direction : std::array<uint8_t, 5>{3, 0, 1, 2, 4}) for (unsigned keys : {1u, 3u}) {
      std::printf("raw_range=%u direction=%u keys=%u\n", unsigned(range), unsigned(direction), keys);
      std::fflush(stdout);
      const auto expectedRange = range == 0 ? 1 : 4;
      const auto index = 2 * ((expectedRange - 1) * 5 + direction) + (keys == 1 ? 0 : 1);
      check(arpTrace(range, direction, keys) == arpBaseline[index], "invalid raw range behaves as its supported boundary");
    }
  }
}
} // namespace
int main(int argc, char** argv) {
#if defined(__linux__)
  if (prctl(PR_SET_DUMPABLE, 0) != 0) return 2;
#endif
  const char* scenario = argc == 1 ? "transpose" : argv[1];
  if (argc > 2) return 2;
  if (std::strcmp(scenario, "transpose") == 0) transposeTests();
  else if (std::strcmp(scenario, "groove") == 0) grooveTests();
  else if (std::strcmp(scenario, "arp-valid") == 0) arpValid(false);
  else if (std::strcmp(scenario, "arp-trace") == 0) arpValid(true);
  else if (std::strcmp(scenario, "arp-invalid") == 0) arpInvalid(false);
  else if (std::strcmp(scenario, "arp-zero") == 0) arpInvalid(true);
  else return 2;
  std::printf("Sequencer safety scenario=%s failures=%d\n", scenario, failures);
  return failures == 0 ? 0 : 1;
}
