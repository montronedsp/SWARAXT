// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi formant (vowel) oscillator. Created independently for
// the HD engine; not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie
// Gillet).
//
// One shared block with two render families, selected by the engine:
//   - Faithful mode replicates the Classic Oscillator::RenderVowel bit-for-bit:
//     the same 9x7 vowel parameter table (offset = vowel<<4-index, low nibble =
//     balance), the same U8U4MixU12/U8U4MixU8 interpolation cadence (every 4th
//     block), the same 16-bit formant phase/increment, the same sign-formant
//     table reads (sine for formants 0,1 and square for formant 2), and the
//     same amplitude modulator S8U8MulShift8(..., ~(integral >> 8)).
//
//     The Classic block sources its noise from random_->state_msb(), which is a
//     *const* accessor: the RNG is never advanced inside RenderVowel, so the
//     noise byte is constant across a block. Faithful mode therefore takes that
//     constant byte (the RNG msb at render time) as an input. The engine wires
//     it to the same value the Classic engine would read.
//
//   - HD mode decouples the accidental formant-alias noise from Classic (the
//     write of formant_amplitude[3] into noise_modulation): three formant
//     oscillators driven from the same table frequencies plus an explicit
//     breath/noise gain, all float. See docs/HD_SHRUTHI_ARCHITECTURE.md 4.3.
//
// The faithful renderers address the genuine Classic resource tables, so the
// table layout below (9 vowels x 7 bytes, and the 256-byte formant sign
// tables) matches the Classic resources.h on purpose.

#ifndef AVRLIB_HD_VOWEL_H_
#define AVRLIB_HD_VOWEL_H_

#include <cmath>
#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

enum class VowelMode : uint8_t {
  kFaithful = 0,  // byte-exact Classic RenderVowel clone
  kHd,            // explicit float formants + breath
  kNumModes
};

struct VowelTables {
  const uint8_t* vowel_data = nullptr;      // wav_res_vowel_data (9 x 7 bytes)
  const uint8_t* formant_sine = nullptr;    // 256 bytes
  const uint8_t* formant_square = nullptr;  // 256 bytes
};

class HdVowel {
 public:
  static constexpr uint8_t kNumVowels = 9;
  static constexpr uint8_t kNumFormants = 3;
  static constexpr uint8_t kControlDecimation = 4;

  HdVowel() = default;

  void set_tables(const VowelTables& tables) { tables_ = tables; }

  // Shruthi Oscillator::Reset() writes the RNG reset value into the state
  // union; through the union layout that lands on a vowel formant increment
  // byte, corrupting the increment until the first control update (every 4th
  // block). This accidental "breath" residue is real device behaviour, so the
  // faithful clone reproduces it.
  //
  // FilteredNoiseState is { uint8_t lp_noise_sample; uint16_t rng_state;
  // uint16_t rng_reset_value; } so rng_reset_value sits at union byte offsets
  // 4..5 (after 1 byte + 1 byte of padding). In VowelSynthesizerState those
  // are exactly the bytes of formant_increment[2]. Reset() sets
  // rng_reset_value = GetByte() + 1 (value stored little-endian), so the byte
  // residue is: formant_increment[2] = (noise_msb + 1).
  void Reset(uint8_t noise_msb = 0) {
    update_ = 0;
    phase_ = 0;
    for (int i = 0; i < kNumFormants; ++i) {
      formant_increment_[i] = 0;
      formant_phase_[i] = 0;
      formant_amplitude_[i] = 0;
      hd_phase_[i] = 0.0f;
    }
    noise_modulation_ = 0;
    noise_state_ = 1;
    formant_increment_[2] = static_cast<uint16_t>(
        static_cast<uint8_t>(noise_msb) + 1u);
  }

  // byte-exact Classic RenderVowel clone. increment16 is the 16-bit integral of
  // the 24-bit oscillator increment; parameter packs (vowel << 4) | balance;
  // noise_msb is the constant rng->state_msb() byte for this block.
  void RenderNaive(uint16_t increment16, uint8_t parameter, uint8_t noise_msb,
                   int num, float* out);

  // HD formants + explicit breath. increment24 is the Classic 24-bit phase
  // increment (16.8) so the formant-to-fundamental ratio reproduces the table
  // tuning; noise_gain is the explicit breath level [0, 1].
  void RenderHd(uint32_t increment24, uint8_t parameter, float noise_gain,
                int num, float* out);

  uint16_t phase_for_tests() const { return phase_; }

  // Classic primitives, replicated exactly (avrlib/op.h host semantics).
  static uint16_t MixU12(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(a * (15 - balance) + b * balance);
  }
  static uint8_t MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(MixU12(a, b, balance) >> 4);
  }
  static int8_t S8U8MulShift8(int8_t a, uint8_t b) {
    return static_cast<int8_t>(static_cast<int16_t>(a) * b >> 8);
  }

  // Interpolate formant increments/amplitudes at the start of every 4th block,
  // packed (vowel << 4) | balance. Fills the running state; called by both
  // renderers when controlDecimation counter hits.
  void UpdateVowel(uint8_t parameter);

  static Sample ToSample(uint8_t byte) {
    return static_cast<Sample>(static_cast<int>(byte) - 128) * (1.0f / 128.0f);
  }

  VowelTables tables_{};
  uint8_t update_ = 0;
  uint16_t formant_increment_[kNumFormants] = {};
  uint16_t formant_phase_[kNumFormants] = {};
  uint8_t formant_amplitude_[kNumFormants] = {};
  uint8_t noise_modulation_ = 0;
  uint16_t phase_ = 0;       // Classic 16-bit oscillator phase integral.
  uint32_t noise_state_ = 1; // HD breath LCG state.
  float hd_phase_[kNumFormants] = {};  // HD per-formant phase (0..1 wrap).

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdVowel);
};

inline void HdVowel::UpdateVowel(uint8_t parameter) {
  if (tables_.vowel_data == nullptr || tables_.formant_sine == nullptr ||
      tables_.formant_square == nullptr) {
    return;
  }
  uint8_t offset_1 = static_cast<uint8_t>(static_cast<uint8_t>(parameter >> 4) * 7);
  uint8_t offset_2 = static_cast<uint8_t>(offset_1 + 7);
  uint8_t balance = static_cast<uint8_t>(parameter & 0x0f);

  for (int i = 0; i < kNumFormants; ++i) {
    uint16_t v = MixU12(tables_.vowel_data[offset_1 + i],
                        tables_.vowel_data[offset_2 + i], balance);
    formant_increment_[i] = static_cast<uint16_t>(v << 3);
  }
  for (int i = 0; i < kNumFormants; ++i) {
    formant_amplitude_[i] =
        MixU8(tables_.vowel_data[offset_1 + 3 + i],
              tables_.vowel_data[offset_2 + 3 + i], balance);
  }
  noise_modulation_ =
      MixU8(tables_.vowel_data[offset_1 + 6], tables_.vowel_data[offset_2 + 6],
            balance);
}

inline void HdVowel::RenderNaive(uint16_t increment16, uint8_t parameter,
                                 uint8_t noise_msb, int num, float* out) {
  update_ = static_cast<uint8_t>((update_ + 1) & 0x03);
  if (update_ == 0) {
    UpdateVowel(parameter);
  }
  const uint8_t* sine = tables_.formant_sine;
  const uint8_t* square = tables_.formant_square;
  uint16_t phase = phase_;
  for (int i = 0; i < num; i += 2) {
    int result = 0;
    for (int f = 0; f < kNumFormants; ++f) {
      formant_phase_[f] =
          static_cast<uint16_t>(formant_phase_[f] + formant_increment_[f]);
      uint8_t index = static_cast<uint8_t>(
          ((formant_phase_[f] >> 8) & 0xf0) | formant_amplitude_[f]);
      const uint8_t* table = f == 2 ? square : sine;
      result += table == nullptr ? 0 : table[index];
    }
    int8_t r = static_cast<int8_t>(static_cast<uint8_t>(result & 0xff));
    uint8_t scale = static_cast<uint8_t>(~(static_cast<uint8_t>(phase >> 8)));
    r = S8U8MulShift8(r, scale);
    phase = static_cast<uint16_t>(phase + increment16);
    int16_t phase_noise = static_cast<int8_t>(noise_msb) *
                          static_cast<int8_t>(noise_modulation_);
    if (static_cast<int32_t>(phase) + phase_noise < increment16) {
      formant_phase_[0] = 0;
      formant_phase_[1] = 0;
      formant_phase_[2] = 0;
    }
    int x = 4 * static_cast<int>(r);
    x = x < -128 ? -128 : (x > 127 ? 127 : x);
    uint8_t byte = static_cast<uint8_t>(x + 128);
    Sample value = ToSample(byte);
    out[i] = value;
    out[i + 1] = value;
  }
  phase_ = phase;
}

inline void HdVowel::RenderHd(uint32_t increment24, uint8_t parameter,
                              float noise_gain, int num, float* out) {
  update_ = static_cast<uint8_t>((update_ + 1) & 0x03);
  if (update_ == 0) {
    UpdateVowel(parameter);
  }
  // Formant-to-fundamental ratios from the table tuning, in Classic 16.16
  // terms: formant runs at formant_increment / 65536 per sample, the
  // fundamental at increment24 / 16777216.
  float ratio[kNumFormants];
  const bool have_fundamental = increment24 != 0;
  for (int f = 0; f < kNumFormants; ++f) {
    ratio[f] = have_fundamental
                   ? static_cast<float>(formant_increment_[f]) * 256.0f /
                         static_cast<float>(increment24)
                   : 0.0f;
  }
  const float two_pi = 6.283185307179586f;
  // Deterministic, symmetric white noise (Park-Miller style LCG), advanced
  // once per sample so the breath has a pseudo-random character.
  const uint32_t kLcgMul = 1664525u;
  const uint32_t kLcgInc = 1013904223u;
  float noise = 0.0f;
  for (int i = 0; i < num; ++i) {
    float y = 0.0f;
    for (int f = 0; f < kNumFormants; ++f) {
      float phase = hd_phase_[f];
      float amp = static_cast<float>(formant_amplitude_[f]) * (1.0f / 128.0f);
      float wave = (f == 2) ? (phase < 0.5f ? 1.0f : -1.0f)
                            : std::sin(two_pi * phase);
      y += amp * wave;
      hd_phase_[f] = phase + ratio[f];
      if (hd_phase_[f] >= 1.0f) {
        hd_phase_[f] -= 1.0f;
      }
    }
    // Explicit breath, replacing the Classic accidental formant-alias noise.
    noise_state_ = kLcgMul * noise_state_ + kLcgInc;
    noise = static_cast<float>(noise_state_ >> 8) * (1.0f / 8388608.0f) -
            1.0f;
    y += noise * noise_gain;
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    out[i] = y;
  }
}

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_VOWEL_H_