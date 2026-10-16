// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linear FM range is a ratio selector. The public domain is -48..+48, but the
// firmware table is addressed by range+24 over -24..+24. Values below -24 used
// to wrap through uint8_t and select the 8:1 entry. This drives HdVoice, the
// shipping voice, from the patch range through to the rendered oscillator.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "avrlib_hd/voice.h"
#include "shruthi/resources.h"

namespace {

int gFailures = 0;

void expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr int kRanges[] = {
    -48, -36, -25, -24, -13, -12, -11, 0, 11, 12, 24, 25, 36, 48};
constexpr int kRangeCount = 14;

struct Expected {
  int public_range;
  int signed_range;
  int secondary;
  int index;
};

// Hand-checked mapping. -48..-12 share the 1:8 entry. -11 is the next entry.
// +12..+48 share the 8:1 entry. The signed value is the pre-uint8 clamp.
constexpr Expected kExpected[] = {
    {-48, -24, 0, 0},  {-36, -24, 0, 0}, {-25, -24, 0, 0}, {-24, -24, 0, 0},
    {-13, -13, 11, 0}, {-12, -12, 12, 0}, {-11, -11, 13, 1}, {0, 0, 24, 12},
    {11, 11, 35, 23},  {12, 12, 36, 24}, {24, 24, 48, 24},  {25, 24, 48, 24},
    {36, 24, 48, 24},  {48, 24, 48, 24},
};

int fm_index(int secondary) {
  if (secondary < 12) {
    return 0;
  }
  if (secondary > 36) {
    return 24;
  }
  return secondary - 12;
}

uint8_t historical_secondary(int range) {
  return static_cast<uint8_t>(static_cast<int>(range) + 24);
}

const Expected* expect_for(int range) {
  for (const Expected& row : kExpected) {
    if (row.public_range == range) {
      return &row;
    }
  }
  return nullptr;
}

struct Tables {
  avrlib_hd::OscillatorTables osc;
  avrlib_hd::EnvelopeTables env;
  avrlib_hd::HdVoiceTables voice;
};

Tables make_tables() {
  Tables t;
  t.osc.waveform_table = shruthi::waveform_table;
  t.osc.sine = shruthi::wav_res_sine;
  t.osc.waves = shruthi::wav_res_waves;
  t.osc.wavetables = shruthi::wav_res_wavetables;
  t.osc.fm_frequency_ratios = shruthi::lut_res_fm_frequency_ratios;
  t.osc.vowel_data = shruthi::wav_res_vowel_data;
  t.osc.formant_sine = shruthi::wav_res_formant_sine;
  t.osc.formant_square = shruthi::wav_res_formant_square;
  t.osc.bandlimited_triangle_0 = shruthi::wav_res_bandlimited_triangle_0;
  t.env.portamento_increments = shruthi::lut_res_env_portamento_increments;
  t.env.env_expo = shruthi::wav_res_env_expo;
  t.voice.oscillator_increments = shruthi::lut_res_oscillator_increments;
  t.voice.portamento_increments = shruthi::lut_res_env_portamento_increments;
  t.voice.distortion = shruthi::wav_res_distortion;
  return t;
}

struct Snap {
  uint8_t secondary[2];
  uint8_t parameter[2];
  uint16_t increment[2];
  uint32_t phase[2];
  std::vector<uint8_t> osc1;
  std::vector<uint8_t> osc2;
};

Snap render(const Tables& tables, int shape1, int range1, int depth1,
            int shape2, int range2, int depth2, int note, int blocks,
            bool modulate_depth) {
  avrlib_hd::Random random;
  random.Seed(0x21);
  avrlib_hd::HdVoice voice;
  voice.Init(&random);
  voice.set_tables(&tables.voice);
  voice.set_envelope_tables(tables.env);
  for (int i = 0; i < 2; ++i) {
    voice.mutable_oscillator(i)->set_tables(tables.osc);
  }
  voice.set_user_wavetable(shruthi::wav_res_waves,
                           avrlib_hd::HdOscillator::kUserWavetableSize);

  avrlib_hd::HdVoicePatch patch;
  patch.osc[0].shape = static_cast<uint8_t>(shape1);
  patch.osc[1].shape = static_cast<uint8_t>(shape2);
  patch.osc[0].parameter = static_cast<uint8_t>(depth1);
  patch.osc[1].parameter = static_cast<uint8_t>(depth2);
  patch.osc[0].range = static_cast<int8_t>(range1);
  patch.osc[1].range = static_cast<int8_t>(range2);
  patch.osc[0].option = avrlib_hd::kMixOpSum;
  patch.env[1].sustain = 127;
  if (modulate_depth) {
    patch.mod[0].amount = 40;
    patch.mod[0].source = avrlib_hd::kModSourceLfo1;
    patch.mod[0].destination = avrlib_hd::kModDestPwm1;
    voice.set_modulation_source(avrlib_hd::kModSourceLfo1, 255);
  }

  voice.NoteOn(static_cast<uint16_t>(note * 128), 100, 0, true);
  Snap snap;
  snap.osc1.reserve(static_cast<size_t>(blocks * avrlib_hd::kAudioBlockSize));
  snap.osc2.reserve(static_cast<size_t>(blocks * avrlib_hd::kAudioBlockSize));
  for (int block = 0; block < blocks; ++block) {
    voice.ProcessBlock(patch, {});
    snap.osc1.insert(snap.osc1.end(), voice.osc1_buffer(),
                     voice.osc1_buffer() + avrlib_hd::kAudioBlockSize);
    snap.osc2.insert(snap.osc2.end(), voice.osc2_buffer(),
                     voice.osc2_buffer() + avrlib_hd::kAudioBlockSize);
  }
  for (int i = 0; i < 2; ++i) {
    snap.secondary[i] = voice.oscillator(i)->secondary_parameter_for_tests();
    snap.parameter[i] = voice.oscillator(i)->parameter_for_tests();
    snap.increment[i] = voice.oscillator(i)->phase_increment_for_tests();
    snap.phase[i] = voice.oscillator(i)->phase_for_tests();
  }
  return snap;
}

bool same_audio(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size()) == 0;
}

void test_rom(const Tables& tables) {
  expect(tables.osc.fm_frequency_ratios[0] == 32, "FM ratio 0 is 1:8 (32)");
  expect(tables.osc.fm_frequency_ratios[1] == 64, "FM ratio 1 is the next entry");
  expect(tables.osc.fm_frequency_ratios[12] == 514, "FM ratio 12 is the centre entry");
  expect(tables.osc.fm_frequency_ratios[23] == 1448, "FM ratio 23 is the last intermediate");
  expect(tables.osc.fm_frequency_ratios[24] == 2048, "FM ratio 24 is 8:1 (2048)");
  expect(fm_index(0) == 0 && fm_index(11) == 0 && fm_index(12) == 0, "indices below 12 saturate low");
  expect(fm_index(36) == 24 && fm_index(255) == 24, "indices above 36 saturate high");
}

void fail_osc(const char* what, int osc, int range, int actual, int wanted) {
  std::fprintf(stderr, "FAIL: %s osc %d range %d actual %d wanted %d\n",
               what, osc, range, actual, wanted);
  ++gFailures;
}

void test_matrix(const Tables& tables) {
  for (const Expected& row : kExpected) {
    const Snap snap = render(tables, avrlib_hd::kRealFm, row.public_range, 64,
                             avrlib_hd::kRealFm, row.public_range, 64, 60, 4, false);
    for (int osc = 0; osc < 2; ++osc) {
      if (static_cast<int>(snap.secondary[osc]) != row.secondary) {
        fail_osc("secondary", osc + 1, row.public_range, snap.secondary[osc], row.secondary);
      }
      const int index = fm_index(snap.secondary[osc]);
      if (index != row.index) {
        fail_osc("lookup index", osc + 1, row.public_range, index, row.index);
      }
      expect(index >= 0 && index <= 24, "FM lookup index stays inside the 25-entry table");
      const int ratio = tables.osc.fm_frequency_ratios[index];
      if (ratio != tables.osc.fm_frequency_ratios[row.index]) {
        fail_osc("ratio", osc + 1, row.public_range, ratio, tables.osc.fm_frequency_ratios[row.index]);
      }
      const int historical = historical_secondary(row.public_range);
      if (row.public_range >= -24 && row.public_range <= 24 &&
          snap.secondary[osc] != historical) {
        fail_osc("historical secondary", osc + 1, row.public_range, snap.secondary[osc], historical);
      }
      if (row.public_range < -24 &&
          (snap.secondary[osc] == historical || snap.secondary[osc] != 0)) {
        fail_osc("wrapped secondary", osc + 1, row.public_range, snap.secondary[osc], 0);
      }
    }
    std::printf("range %+d signed %+d secondary %d index %d ratio %u\n",
                row.public_range, row.signed_range, row.secondary, row.index,
                tables.osc.fm_frequency_ratios[row.index]);
  }
}

void test_audio_classes(const Tables& tables) {
  const Snap low = render(tables, avrlib_hd::kRealFm, -24, 64,
                          avrlib_hd::kRealFm, -24, 64, 60, 8, false);
  const Snap wrapped = render(tables, avrlib_hd::kRealFm, -25, 64,
                              avrlib_hd::kRealFm, -48, 64, 60, 8, false);
  const Snap floor = render(tables, avrlib_hd::kRealFm, -12, 64,
                            avrlib_hd::kRealFm, -36, 64, 60, 8, false);
  const Snap next = render(tables, avrlib_hd::kRealFm, -11, 64,
                           avrlib_hd::kRealFm, -11, 64, 60, 8, false);
  const Snap high = render(tables, avrlib_hd::kRealFm, 24, 64,
                           avrlib_hd::kRealFm, 12, 64, 60, 8, false);
  const Snap saturated = render(tables, avrlib_hd::kRealFm, 48, 64,
                                avrlib_hd::kRealFm, 25, 64, 60, 8, false);
  const Snap centre = render(tables, avrlib_hd::kRealFm, 0, 64,
                             avrlib_hd::kRealFm, 0, 64, 60, 8, false);

  expect(same_audio(wrapped.osc1, low.osc1), "OSC1 -25 matches -24");
  expect(same_audio(floor.osc1, low.osc1), "OSC1 -12 matches -24");
  expect(!same_audio(next.osc1, low.osc1), "OSC1 -11 leaves the lowest ratio");
  expect(same_audio(saturated.osc1, high.osc1), "OSC1 +48 matches +24");
  expect(!same_audio(centre.osc1, high.osc1), "OSC1 centre ratio is not 8:1");
  expect(same_audio(wrapped.osc2, floor.osc2), "OSC2 negative ranges share 1:8");
  expect(same_audio(saturated.osc2, high.osc2), "OSC2 positive saturation shares 8:1");
  expect(low.increment[0] == wrapped.increment[0] &&
             low.increment[0] == saturated.increment[0],
         "FM range does not change carrier pitch");
}

void test_depths_and_notes(const Tables& tables) {
  for (int depth : {0, 1, 64, 127}) {
    const Snap neg = render(tables, avrlib_hd::kRealFm, -25, depth,
                            avrlib_hd::kRealFm, -48, depth, 60, 4, false);
    const Snap floor = render(tables, avrlib_hd::kRealFm, -24, depth,
                              avrlib_hd::kRealFm, -12, depth, 60, 4, false);
    const Snap top = render(tables, avrlib_hd::kRealFm, 48, depth,
                            avrlib_hd::kRealFm, 12, depth, 60, 4, false);
    if (!same_audio(neg.osc1, floor.osc1)) {
      fail_osc("depth low ratio", 1, depth, neg.secondary[0], 0);
    }
    if (neg.secondary[0] != 0 || top.secondary[0] != 48) {
      fail_osc("depth secondary", 1, depth, neg.secondary[0], top.secondary[0]);
    }
    if (depth == 0) {
      expect(same_audio(neg.osc1, top.osc1), "zero depth ignores the FM ratio");
    } else {
      expect(!same_audio(neg.osc1, top.osc1), "nonzero depth makes 1:8 differ from 8:1");
    }
  }
  for (int note : {1, 127}) {
    const Snap neg = render(tables, avrlib_hd::kRealFm, -36, 96,
                            avrlib_hd::kRealFm, 36, 96, note, 4, false);
    const Snap ref = render(tables, avrlib_hd::kRealFm, -24, 96,
                            avrlib_hd::kRealFm, 24, 96, note, 4, false);
    if (neg.secondary[0] != 0 || neg.secondary[1] != 48) {
      fail_osc("note secondary", 1, note, neg.secondary[0], neg.secondary[1]);
    }
    expect(same_audio(neg.osc1, ref.osc1) && same_audio(neg.osc2, ref.osc2),
           "extreme notes match the saturated FM ratios");
  }
}

void test_other_shapes(const Tables& tables) {
  const Snap fm_pitch = render(tables, avrlib_hd::kRealFm, -48, 0,
                               avrlib_hd::kRealFm, 0, 0, 60, 1, false);
  for (int shape = avrlib_hd::kRealSilence; shape <= avrlib_hd::kRealWavetable9; ++shape) {
    if (shape == avrlib_hd::kRealFm) {
      continue;
    }
    for (int range : {-48, -25, -24, 0, 24, 48}) {
      const Snap snap = render(tables, shape, range, 40, shape, range, 40, 60, 1, false);
      const int historical = historical_secondary(range);
      if (snap.secondary[0] != historical || snap.secondary[1] != historical) {
        fail_osc("non-FM secondary", shape, range, snap.secondary[0], historical);
      }
    }
  }
  const Snap saw_low = render(tables, avrlib_hd::kRealSaw, -48, 0,
                              avrlib_hd::kRealSaw, -48, 0, 60, 1, false);
  const Snap saw_mid = render(tables, avrlib_hd::kRealSaw, 0, 0,
                              avrlib_hd::kRealSaw, 0, 0, 60, 1, false);
  expect(saw_low.increment[0] != saw_mid.increment[0], "non-FM range still transposes");
  expect(fm_pitch.increment[0] == fm_pitch.increment[1], "FM range does not transpose");
  expect(saw_low.secondary[0] == historical_secondary(-48), "saw -48 secondary is not FM-clamped");
}

void test_modulation_and_lifetime(const Tables& tables) {
  const Snap plain = render(tables, avrlib_hd::kRealFm, -30, 10,
                            avrlib_hd::kRealSquare, -30, 10, 60, 2, false);
  const Snap modulated = render(tables, avrlib_hd::kRealFm, -30, 10,
                                avrlib_hd::kRealSquare, -30, 10, 60, 2, true);
  expect(modulated.secondary[0] == 0, "modulated FM depth still clamps the ratio");
  expect(modulated.secondary[1] == historical_secondary(-30),
         "OSC2 non-FM secondary ignores the FM clamp");
  expect(modulated.parameter[0] != plain.parameter[0], "LFO reaches FM depth");

  avrlib_hd::Random random;
  random.Seed(0x4d);
  avrlib_hd::HdVoice voice;
  voice.Init(&random);
  voice.set_tables(&tables.voice);
  voice.set_envelope_tables(tables.env);
  for (int i = 0; i < 2; ++i) {
    voice.mutable_oscillator(i)->set_tables(tables.osc);
  }
  avrlib_hd::HdVoicePatch patch;
  patch.osc[0].shape = avrlib_hd::kRealFm;
  patch.osc[1].shape = avrlib_hd::kRealFm;
  patch.osc[0].parameter = 80;
  patch.osc[1].parameter = 80;
  patch.osc[0].range = -40;
  patch.osc[1].range = 40;
  voice.NoteOn(60 * 128, 100, 0, true);
  voice.ProcessBlock(patch, {});
  const uint32_t phase_after_first = voice.oscillator(0)->phase_for_tests();
  voice.NoteOn(60 * 128, 100, 0, true);
  voice.ProcessBlock(patch, {});
  expect(voice.oscillator(0)->secondary_parameter_for_tests() == 0, "retrigger keeps OSC1 at 1:8");
  expect(voice.oscillator(1)->secondary_parameter_for_tests() == 48, "retrigger keeps OSC2 at 8:1");
  const uint32_t phase_after_retrigger = voice.oscillator(0)->phase_for_tests();
  voice.ProcessBlock(patch, {});
  const uint32_t phase_after_next = voice.oscillator(0)->phase_for_tests();
  expect(phase_after_next != phase_after_retrigger, "FM phase advances across blocks");
  expect(phase_after_first != 0 || phase_after_retrigger != 0, "FM produces a carrier phase");

  voice.Init(&random);
  patch.osc[0].range = -25;
  voice.NoteOn(72 * 128, 90, 0, true);
  voice.ProcessBlock(patch, {});
  expect(voice.oscillator(0)->secondary_parameter_for_tests() == 0, "voice re-init still clamps");
  patch.osc[0].range = 0;
  voice.ProcessBlock(patch, {});
  expect(voice.oscillator(0)->secondary_parameter_for_tests() == 24, "range edits publish on the next block");
  expect(voice.oscillator(0)->phase_for_tests() != 0, "a range edit does not clear the oscillator");
}

}  // namespace

int main() {
  const Tables tables = make_tables();
  test_rom(tables);
  test_matrix(tables);
  test_audio_classes(tables);
  test_depths_and_notes(tables);
  test_other_shapes(tables);
  test_modulation_and_lifetime(tables);
  if (gFailures != 0) {
    std::fprintf(stderr, "%d FM range checks failed\n", gFailures);
    return 1;
  }
  std::printf("AvrlibHdFmRangeTests passed\n");
  return 0;
}
