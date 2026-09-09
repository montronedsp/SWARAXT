// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi oscillator. Created independently for the HD engine;
// not copied from Shruthi avrlib (GPL-3.0, (c) 2009 Emilie Gillet).
//
// Two rendering families, selected by the engine:
//   - Faithful (naive) waves replicate the Classic Shruthi oscillator's
//     renderers bit-for-bit (same integer arithmetic, same 16.8 phase
//     semantics, same note-based zone selection and same table layout) and
//     only convert the resulting 8-bit sample to float. Output is exactly the
//     Classic sample mapped to [-1, 1]: (byte - 128) / 128.
//   - HD (band-limited) waves are free of the Classic tables: analytic sine,
//     polyBLEP saw and pulse, and a continuous analytic triangle.
//
// The faithful renderers address the genuine Classic tables (waveform_table,
// wav_res_sine, wav_res_waves, wav_res_wavetables, lut_res_fm_frequency_ratios,
// wav_res_vowel_data, wav_res_formant_sine/square, wav_res_bandlimited_triangle_0
// and the user ram wavetable), so the engine wires them as raw pointers. The
// full Classic shape set (patch.h OscillatorAlgorithm 0..34) is covered; the
// exact Classic union-alias behaviour of the vowel sub-block (the formant
// increment hardware write colliding with the filtered-noise reset value) is
// reproduced structurally in Reset().
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md section on the oscillator.

#ifndef AVRLIB_HD_OSCILLATOR_H_
#define AVRLIB_HD_OSCILLATOR_H_

#include <cmath>
#include <cstdint>

#include "avrlib_hd/random.h"
#include "avrlib_hd/types.h"

namespace avrlib_hd {

enum class OscWave : uint8_t {
  kSilence = 0,
  kSaw,        // faithful: Classic RenderSimpleWavetable (saw family)
  kTriangle,   // faithful: Classic RenderSimpleWavetable (triangle family)
  kSquare,     // faithful: Classic param == 0 simple, else band-limited PWM
  kSine,       // HD: analytic sine
  kSawBlp,     // HD: polyBLEP saw
  kSquareBlp,  // HD: polyBLEP pulse (duty from parameter)
  kTriangleHd, // HD: analytic triangle (continuous, no BLEP needed)
  kNumWaves
};

// Numeric order of the Classic patch.h OscillatorAlgorithm, kept raw so the
// faithful engine dispatches exactly like the firmware's Oscillator::Render.
enum RealWaveShape : uint8_t {
  kRealSilence = 0,
  kRealSaw,
  kRealSquare,
  kRealTriangle,
  kRealCzSaw,
  kRealCzReso,
  kRealCzTriangle,
  kRealCzPulse,
  kRealCzSync,
  kRealQuadSawPad,
  kRealFm,
  kRealWavetable1,
  kRealWavetable2,
  kRealWavetable3,
  kRealWavetable4,
  kRealWavetable5,
  kRealWavetable6,
  kRealWavetable7,
  kRealWavetable8,
  kRealWavetableUser,
  kReal8BitLand,
  kRealCrushedSine,
  kRealDirtyPwm,
  kRealFilteredNoise,
  kRealVowel,
  kRealWavetable9,
};

// The Classic tables used by the faithful renderers.
struct OscillatorTables {
  const uint8_t* const* waveform_table = nullptr;  // waveform_table
  const uint8_t* sine = nullptr;             // wav_res_sine
  const uint8_t* waves = nullptr;            // wav_res_waves
  const uint8_t* wavetables = nullptr;       // wav_res_wavetables
  const uint16_t* fm_frequency_ratios = nullptr;  // lut_res_fm_frequency_ratios
  const uint8_t* vowel_data = nullptr;       // wav_res_vowel_data
  const uint8_t* formant_sine = nullptr;     // wav_res_formant_sine
  const uint8_t* formant_square = nullptr;   // wav_res_formant_square
  const uint8_t* bandlimited_triangle_0 = nullptr;  // wav_res_bandlimited_triangle_0
};

// Byte-exact mirror of the Classic OscillatorState union
// (shruthi/oscillator.h). The Classic renderers intentionally share this
// storage between algorithms: the CZ/FM secondary phase, the quad-saw-pad
// phases, the crushed-sine decimation counter and the filtered-noise LFSR
// state all alias each other, and Oscillator::Reset() seeds formant_increment
// [2] through the rng reset value. Reproducing the aliasing is required for
// byte-identical faithful output.
struct HdFilteredNoiseState {
  uint8_t lp_noise_sample;
  uint16_t rng_state;
  uint16_t rng_reset_value;
};
struct HdVowelState {
  uint16_t formant_increment[3];
  uint16_t formant_phase[3];
  uint8_t formant_amplitude[3];
  uint8_t noise_modulation;
  uint8_t update;
};
struct HdCrushedSineState {
  uint8_t decimate;
  uint8_t state;
};
union HdOscillatorState {
  HdVowelState vw;
  HdFilteredNoiseState no;
  uint16_t qs[3];
  HdCrushedSineState cr;
  uint16_t secondary_phase;
};

// 16.8 fixed point. One cycle is 2^16 integral units, the counter is 24 bits.
// Faithful renderers need the exact Classic integer semantics (add + carry +
// wrap at 2^24), so the phase is a packed uint32 (integral << 8 | fractional).
class HdOscillator {
 public:
  static constexpr uint16_t kNumZonesSimple = 6;   // zones 0..5 (+6 = sine pad)
  static constexpr uint16_t kNumZonesPwm = 5;      // zones 0..5 (+5 = sine pad)
  static constexpr uint16_t kUserWavetableSize = 8 * 129;

  // Classic waveform_table offsets for the faithful families.
  static constexpr uint16_t kFamilySquare = 3;     // WAV_RES_BANDLIMITED_SQUARE_0
  static constexpr uint16_t kFamilySaw = 10;       // WAV_RES_BANDLIMITED_SAW_0
  static constexpr uint16_t kFamilySawPwm = 11;    // WAV_RES_BANDLIMITED_SAW_1
  static constexpr uint16_t kFamilyTriangle = 17;  // WAV_RES_BANDLIMITED_TRIANGLE_0

  HdOscillator() = default;

  void set_tables(const OscillatorTables& tables) { tables_ = tables; }
  void set_random(Random* random) { random_ = random; }
  void set_user_wavetable(uint8_t* wavetable) { user_wavetable_ = wavetable; }

  void set_parameter(uint8_t parameter) { parameter_ = parameter; }
  void set_secondary_parameter(uint8_t secondary_parameter) {
    secondary_parameter_ = secondary_parameter;
  }

  // Classic Oscillator::Reset(): only the filtered-noise reset value is
  // re-rolled from the random LFSR. The HdOscillatorState union reproduces the
  // Classic byte aliasing (the reset value's bytes collide with the vowel
  // formant increment 2 / quad-saw phases), so no extra work is needed here.
  void Reset() {
    state_.no.rng_reset_value = static_cast<uint16_t>(
        (random_ != nullptr ? random_->GetByte() : 0x21) + 1);
  }

  // num samples are written; increment is a 24-bit phase increment (16.8).
  // sync_input/sync_output follow the Classic oscillator protocol (a non-zero
  // byte resets the phase before the increment; the carry byte flags a wrap).
  void RenderSilence(int num, float* out);
  void RenderNaive(OscWave wave, uint8_t note, uint32_t increment,
                   uint8_t parameter, int num, float* out,
                   const uint8_t* sync_input = nullptr,
                   uint8_t* sync_output = nullptr);
  // Faithful render over the full shape byte domain (0..34). increment is the
  // packed 24-bit (16.8) phase increment. Note and the oscillator parameters
  // are the class members, exactly like the Classic class state; the FM
  // renderer's doubled parameter and zeroed fractional phase leak across calls
  // and are reproduced.
  void RenderNaiveFull(uint8_t shape, uint8_t note, uint32_t increment,
                       int num, float* out,
                       const uint8_t* sync_input = nullptr,
                       uint8_t* sync_output = nullptr);
  void RenderHd(OscWave wave, uint8_t note, uint32_t increment,
                uint8_t parameter, int num, float* out);

  uint32_t phase_for_tests() const { return phase_; }
  uint8_t parameter_for_tests() const { return parameter_; }
  uint8_t secondary_parameter_for_tests() const { return secondary_parameter_; }
  uint8_t shape_for_tests() const { return shape_; }
  uint16_t phase_increment_for_tests() const { return last_increment_integral_; }

 private:
  // Classic integer primitives, replicated exactly (avrlib/op.h). All faithful
  // renderers evaluate expressions in these 8/16-bit widths (AVR semantics),
  // then only the final conversion to float widens the value.
  static uint8_t Swap4(uint8_t a) {
    return static_cast<uint8_t>((a << 4) | (a >> 4));
  }
  static uint8_t AddClip(uint8_t value, uint8_t increment, uint8_t maximum) {
    uint16_t r = static_cast<uint16_t>(value) + increment;
    return r > maximum ? maximum : static_cast<uint8_t>(r);
  }
  static uint8_t U8U8MulShift8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(U8U8Mul(a, b) >> 8);
  }
  static uint16_t U8U8Mul(uint8_t a, uint8_t b) {
    return static_cast<uint16_t>(static_cast<uint16_t>(a) * b);
  }
  static uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(U8MixU16(a, b, balance) >> 8);
  }
  static uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b) {
    return static_cast<uint8_t>((a * gain_a + b * gain_b) >> 8);
  }
  static uint16_t U8MixU16(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(a * (255 - balance) + b * balance);
  }
  static uint8_t U8U4MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>((a * (15 - balance) + b * balance) >> 4);
  }
  static uint16_t U8U4MixU12(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(a * (15 - balance) + b * balance);
  }
  static uint8_t ReadSample(const uint8_t* table, uint16_t phase) {
    return table[phase >> 8];
  }
  static int8_t S16ClipS8(int16_t value) {
    return value < -128 ? -128 : (value > 127 ? 127 : static_cast<int8_t>(value));
  }
  static int8_t S8U8MulShift8(int8_t a, uint8_t b) {
    return static_cast<int8_t>((int16_t(a) * b) >> 8);
  }

  static uint8_t InterpolateSample(const uint8_t* table, uint16_t phase) {
    return U8Mix(table[phase >> 8], table[1 + (phase >> 8)],
                 static_cast<uint8_t>(phase));
  }
  static uint8_t InterpolateSampleRam(const uint8_t* table, uint16_t phase) {
    return U8Mix(table[phase >> 8], table[1 + (phase >> 8)],
                 static_cast<uint8_t>(phase));
  }
  static uint8_t InterpolateTwoTables(
      const uint8_t* table_a, const uint8_t* table_b, uint16_t phase,
      uint8_t gain_a, uint8_t gain_b) {
    return U8Mix(InterpolateSample(table_a, phase),
                 InterpolateSample(table_b, phase), gain_a, gain_b);
  }

  struct Carry24 {
    uint8_t carry;
    uint32_t value;
  };
  static Carry24 Add24(uint32_t a, uint32_t b) {
    uint32_t sum = a + b;
    return Carry24{static_cast<uint8_t>((sum & 0xff000000u) != 0u),
                   sum & 0xffffffu};
  }

  static Sample ToSample(uint8_t byte) {
    return static_cast<Sample>(static_cast<int>(byte) - 128) * (1.0f / 128.0f);
  }

  // Classic RenderSimpleWavetable for one shape family (saw/triangle/square@0).
  void RenderSimple(uint16_t base_zone, bool saw_shape, uint8_t note,
                    uint32_t increment, uint8_t parameter,
                    const uint8_t* sync_input, uint8_t* sync_output,
                    int num, float* out);
  // Classic band-limited PWM (square with parameter != 0).
  void RenderBandlimitedPwm(uint8_t note, uint32_t increment, uint8_t parameter,
                            const uint8_t* sync_input, uint8_t* sync_output,
                            int num, float* out);
  // Full faithful shape renderers (Classic shruthi/oscillator.cc, replicated).
  void RenderInterpolatedWavetable(uint32_t increment, const uint8_t* si,
                                   uint8_t* so, int num, float* out);
  void RenderSweepingWavetableRam(uint32_t increment, const uint8_t* si,
                                  uint8_t* so, int num, float* out);
  void RenderCzSaw(uint8_t note, uint8_t parameter, uint32_t increment,
                   const uint8_t* si, uint8_t* so, int num, float* out);
  void RenderCzPulseReso(uint8_t shape, uint8_t parameter, uint32_t increment,
                         const uint8_t* si, uint8_t* so, int num, float* out);
  void RenderCzReso(uint8_t shape, uint8_t parameter, uint32_t increment,
                    const uint8_t* si, uint8_t* so, int num, float* out);
  void RenderFm(uint32_t increment, const uint8_t* si, uint8_t* so, int num,
                float* out);
  void Render8BitLand(uint8_t parameter, uint32_t increment, const uint8_t* si,
                      uint8_t* so, int num, float* out);
  void RenderCrushedSine(uint8_t parameter, uint32_t increment,
                         const uint8_t* si, uint8_t* so, int num, float* out);
  void RenderDirtyPwm(uint8_t parameter, uint32_t increment, const uint8_t* si,
                      uint8_t* so, int num, float* out);
  void RenderQuadSawPad(uint8_t parameter, uint32_t increment, const uint8_t* si,
                        uint8_t* so, int num, float* out);
  void RenderVowel(uint32_t increment, int num, float* out);
  void RenderFilteredNoise(uint8_t parameter, const uint8_t* si, int num,
                           float* out);

  OscillatorTables tables_ = {};

  // Faithful per-call state (mirror the Classic class members).
  uint32_t phase_ = 0;
  uint8_t shape_ = 0;
  uint8_t note_ = 0;
  uint8_t parameter_ = 0;
  uint8_t secondary_parameter_ = 0;

  // Faithful state for the stateful sub-blocks. This mirrors the Classic
  // OscillatorState union exactly (including the intentional aliasing between
  // the CZ/FM secondary phase, the quad-saw-pad phases, the crushed-sine
  // decimation and the filtered-noise LFSR state).
  HdOscillatorState state_ = {};

  uint8_t* user_wavetable_ = nullptr;
  Random* random_ = nullptr;
  uint16_t last_increment_integral_ = 0;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdOscillator);
};

// ---------------------------------------------------------------------------
// Faithful renderers.
// ---------------------------------------------------------------------------

inline void HdOscillator::RenderSilence(int num, float* out) {
  for (int i = 0; i < num; ++i) {
    out[i] = 0.0f;
  }
}

inline void HdOscillator::RenderSimple(
    uint16_t base_zone, bool saw_shape, uint8_t note, uint32_t increment,
    uint8_t parameter, const uint8_t* sync_input, uint8_t* sync_output,
    int num, float* out) {
  const uint8_t* const* table = tables_.waveform_table;
  uint8_t balance_index = Swap4(static_cast<uint8_t>(note - 12));
  uint8_t gain_2 = static_cast<uint8_t>(balance_index & 0xf0);
  uint8_t gain_1 = static_cast<uint8_t>(~gain_2);
  uint8_t wave_index = static_cast<uint8_t>(balance_index & 0xf);
  // Match public Classic 1.2.1: only the second zone index is clipped. The
  // first index stays raw, including the triangle high-note walk.
  const uint8_t* wave_1 = table[base_zone + wave_index];
  wave_index = AddClip(wave_index, 1, static_cast<uint8_t>(kNumZonesSimple));
  const uint8_t* wave_2 = table[base_zone + wave_index];

  const uint8_t* si = sync_input;
  uint8_t* so = sync_output;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint16_t p = static_cast<uint16_t>(phase >> 8);
    uint8_t sample = InterpolateTwoTables(wave_1, wave_2, p, gain_1, gain_2);
    // Quantized-FM waveshaping: shift (saw) or clip (triangle/square) the part
    // of the waveform below the parameter.
    if (sample < parameter) {
      if (saw_shape) {
        sample = static_cast<uint8_t>(sample + (parameter >> 1));
      } else {
        sample = parameter;
      }
    }
    out[i] = ToSample(sample);
  }
  phase_ = phase;
}

inline void HdOscillator::RenderBandlimitedPwm(
    uint8_t note, uint32_t increment, uint8_t parameter,
    const uint8_t* sync_input, uint8_t* sync_output, int num, float* out) {
  const uint8_t* const* table = tables_.waveform_table;
  uint8_t balance_index = Swap4(static_cast<uint8_t>(note - 12));
  uint8_t gain_2 = static_cast<uint8_t>(balance_index & 0xf0);
  uint8_t gain_1 = static_cast<uint8_t>(~gain_2);
  uint8_t wave_index = static_cast<uint8_t>(balance_index & 0xf);
  const uint8_t* wave_1 = table[kFamilySawPwm + wave_index];
  wave_index = AddClip(wave_index, 1, static_cast<uint8_t>(kNumZonesPwm));
  const uint8_t* wave_2 = table[kFamilySawPwm + wave_index];

  uint16_t shift = static_cast<uint16_t>(static_cast<uint16_t>(parameter) + 128u)
                   << 8;
  uint8_t scale = static_cast<uint8_t>(192 - (parameter >> 1));
  if (note > 64) {
    uint8_t w = static_cast<uint8_t>((note - 64) << 2);
    scale = U8Mix(scale, 102, w);
    scale = U8Mix(scale, 102, w);
  }

  // The PWM renderer doubles the phase increment and outputs each value twice.
  uint32_t increment_doubled = (increment << 1) & 0xffffffu;
  const uint8_t* si = sync_input;
  uint8_t* so = sync_output;
  uint32_t phase = phase_;
  for (int i = 0; i < num; i += 2) {
    Carry24 c = Add24(phase, increment_doubled);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
      *so++ = 0;
    }
    if (si != nullptr) {
      if (si[0] || si[1]) {
        phase = 0;
      }
      si += 2;
    }
    uint16_t p = static_cast<uint16_t>(phase >> 8);
    uint8_t a = InterpolateTwoTables(wave_1, wave_2, p, gain_1, gain_2);
    a = U8U8MulShift8(a, scale);
    uint16_t p_shifted = static_cast<uint16_t>(p + shift);
    uint8_t b = InterpolateTwoTables(wave_1, wave_2, p_shifted, gain_1, gain_2);
    b = U8U8MulShift8(b, scale);
    uint8_t sample = static_cast<uint8_t>(
        static_cast<int>(a) - static_cast<int>(b) + 128);
    Sample value = ToSample(sample);
    out[i] = value;
    out[i + 1] = value;
  }
  phase_ = phase;
}

inline void HdOscillator::RenderInterpolatedWavetable(
    uint32_t increment, const uint8_t* si, uint8_t* so, int num, float* out) {
  uint8_t index = shape_ >= kRealWavetable9
      ? static_cast<uint8_t>(shape_ - kRealWavetable9 + 8)
      : static_cast<uint8_t>(shape_ - kRealWavetable1);
  const uint8_t* definition =
      tables_.wavetables + static_cast<uint16_t>(index) * 18;
  uint8_t num_steps = definition[0];
  uint16_t pointer = U8U8Mul(static_cast<uint8_t>(parameter_ << 1), num_steps);
  uint16_t wave_index_1 = definition[1 + (pointer >> 8)];
  uint16_t wave_index_2 = definition[2 + (pointer >> 8)];
  uint8_t gain = static_cast<uint8_t>(pointer);
  const uint8_t* wave_1 = tables_.waves +
      static_cast<uint16_t>(static_cast<uint8_t>(wave_index_1)) * 129;
  const uint8_t* wave_2 = tables_.waves +
      static_cast<uint16_t>(static_cast<uint8_t>(wave_index_2)) * 129;

  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint16_t p = static_cast<uint16_t>((phase >> 8) >> 1);
    out[i] = ToSample(InterpolateTwoTables(
        wave_1, wave_2, p, static_cast<uint8_t>(~gain), gain));
  }
  phase_ = phase;
}

inline void HdOscillator::RenderSweepingWavetableRam(
    uint32_t increment, const uint8_t* si, uint8_t* so, int num, float* out) {
  uint8_t balance_index = Swap4(parameter_);
  uint8_t wave_index = static_cast<uint8_t>(balance_index & 0xf);
  uint8_t gain_2 = static_cast<uint8_t>(balance_index & 0xf0);
  uint8_t gain_1 = static_cast<uint8_t>(~gain_2);
  uint16_t offset = static_cast<uint16_t>(wave_index << 7) + wave_index;
  const uint8_t* wave_1 = user_wavetable_ + offset;
  const uint8_t* wave_2 = wave_1;
  if (offset < kUserWavetableSize - 129) {
    wave_2 += 129;
  }

  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint16_t p = static_cast<uint16_t>((phase >> 8) >> 1);
    out[i] = ToSample(U8Mix(
        InterpolateSampleRam(wave_1, p),
        InterpolateSampleRam(wave_2, p), gain_1, gain_2));
  }
  phase_ = phase;
}

inline void HdOscillator::RenderCzSaw(
    uint8_t note, uint8_t parameter, uint32_t increment, const uint8_t* si,
    uint8_t* so, int num, float* out) {
  (void)note;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint8_t phi = static_cast<uint8_t>(phase >> 16);
    uint8_t clipped_phi = phi < 0x20 ? static_cast<uint8_t>(phi << 3) : 0xff;
    uint8_t sample = ReadSample(tables_.sine, U8MixU16(
        phi, clipped_phi, static_cast<uint8_t>(parameter << 1)));
    out[i] = ToSample(sample);
  }
  phase_ = phase;
}

inline void HdOscillator::RenderCzPulseReso(
    uint8_t shape, uint8_t parameter, uint32_t increment, const uint8_t* si,
    uint8_t* so, int num, float* out) {
  (void)shape;
  uint16_t inc = static_cast<uint16_t>(increment >> 8);
  uint16_t increment_2 = static_cast<uint16_t>(
      (inc + static_cast<uint16_t>((static_cast<uint32_t>(inc) * parameter) >> 3))
      << 1);
  uint16_t phase_2 = state_.secondary_phase;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    if (c.carry) {
      phase_2 = 32768;
    }
    phase_2 = static_cast<uint16_t>(phase_2 + increment_2);
    uint8_t result = static_cast<uint8_t>(ReadSample(tables_.sine, phase_2) >> 1);
    result = static_cast<uint8_t>(result + 128);
    int integral = static_cast<int>(static_cast<uint16_t>(phase >> 8));
    uint8_t outbyte;
    if (integral < 0x4000) {
      outbyte = result;
    } else if (integral < 0x8000) {
      outbyte = U8U8MulShift8(
          result, static_cast<uint8_t>(
              ~static_cast<uint16_t>(integral - 0x4000) >> 6));
    } else {
      outbyte = 0;
    }
    out[i] = ToSample(outbyte);
  }
  phase_ = phase;
  state_.secondary_phase = phase_2;
}

inline void HdOscillator::RenderCzReso(
    uint8_t shape, uint8_t parameter, uint32_t increment, const uint8_t* si,
    uint8_t* so, int num, float* out) {
  uint16_t inc = static_cast<uint16_t>(increment >> 8);
  uint16_t increment_2 = static_cast<uint16_t>(
      inc + static_cast<uint16_t>((static_cast<uint32_t>(inc) * parameter) >> 3));
  uint16_t phase_2 = state_.secondary_phase;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    if (c.carry) {
      phase_2 = 0;
    }
    phase_2 = static_cast<uint16_t>(phase_2 + increment_2);
    int integral = static_cast<int>(static_cast<uint16_t>(phase >> 8));
    uint8_t carrier = InterpolateSample(tables_.sine, phase_2);
    uint8_t outbyte;
    if (shape == kRealCzSync) {
      outbyte = (integral < 0x8000) ? carrier : 128;
    } else {
      uint8_t window = 0;
      if (shape == kRealCzReso) {
        window = static_cast<uint8_t>(~static_cast<uint8_t>(integral >> 8));
      } else {
        uint8_t window_2 = static_cast<uint8_t>(integral >> 7);
        if (integral & 0x8000) {
          window_2 = static_cast<uint8_t>(~window_2);
        }
        window = window_2;
      }
      outbyte = U8U8MulShift8(carrier, window);
    }
    out[i] = ToSample(outbyte);
  }
  phase_ = phase;
  state_.secondary_phase = phase_2;
}

inline void HdOscillator::RenderFm(
    uint32_t increment, const uint8_t* si, uint8_t* so, int num, float* out) {
  uint16_t offset = secondary_parameter_;
  if (offset < 12) {
    offset = 0;
  } else if (offset > 36) {
    offset = 24;
  } else {
    offset = static_cast<uint16_t>(offset - 12);
  }
  uint16_t multiplier = tables_.fm_frequency_ratios[offset];
  uint64_t product = static_cast<uint64_t>(static_cast<uint16_t>(increment >> 8)) *
                     multiplier;
  uint16_t increment_2 = static_cast<uint16_t>((product >> 8) & 0xffffu);
  // Classic leaks: parameter_ is doubled and the phase fractional part is
  // zeroed on every call.
  parameter_ = static_cast<uint8_t>(parameter_ << 1);
  phase_ &= ~0xffu;
  uint8_t p = parameter_;
  uint16_t phase_2 = state_.secondary_phase;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    phase_2 = static_cast<uint16_t>(phase_2 + increment_2);
    uint8_t modulator = InterpolateSample(tables_.sine, phase_2);
    uint16_t modulation = static_cast<uint16_t>(modulator * p);
    uint16_t carrier_phase = static_cast<uint16_t>(
        static_cast<uint16_t>(phase >> 8) + modulation);
    out[i] = ToSample(InterpolateSample(tables_.sine, carrier_phase));
  }
  phase_ = phase;
  state_.secondary_phase = phase_2;
}

inline void HdOscillator::Render8BitLand(
    uint8_t parameter, uint32_t increment, const uint8_t* si, uint8_t* so,
    int num, float* out) {
  uint8_t x = parameter;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint8_t integral = static_cast<uint8_t>(phase >> 16);
    int a = static_cast<int>(integral) ^ (static_cast<int>(x) << 1);
    int b = a & ~static_cast<int>(x);
    int r = b + static_cast<int>(x >> 1);
    out[i] = ToSample(static_cast<uint8_t>(r));
  }
  phase_ = phase;
}

inline void HdOscillator::RenderCrushedSine(
    uint8_t parameter, uint32_t increment, const uint8_t* si, uint8_t* so,
    int num, float* out) {
  uint8_t decimate = state_.cr.decimate;
  uint8_t held = state_.cr.state;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    ++decimate;
    if (parameter <= 63) {
      if (decimate >= parameter + 1) {
        decimate = 0;
        held = InterpolateSample(tables_.sine,
                                 static_cast<uint16_t>(phase >> 8));
      }
    } else {
      // Classic compares in int: for parameter > 128 the threshold is negative
      // and the held sample is refreshed on every sample.
      if (decimate >= 128 - static_cast<int>(parameter)) {
        decimate = 0;
        held = InterpolateSample(tables_.bandlimited_triangle_0,
                                 static_cast<uint16_t>(phase >> 8));
      }
    }
    out[i] = ToSample(held);
  }
  phase_ = phase;
  state_.cr.decimate = decimate;
  state_.cr.state = held;
}

inline void HdOscillator::RenderDirtyPwm(
    uint8_t parameter, uint32_t increment, const uint8_t* si, uint8_t* so,
    int num, float* out) {
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    uint8_t integral = static_cast<uint8_t>(phase >> 16);
    uint8_t sample = static_cast<int>(integral) < 127 + parameter ? 0 : 255;
    out[i] = ToSample(sample);
  }
  phase_ = phase;
}

inline void HdOscillator::RenderQuadSawPad(
    uint8_t parameter, uint32_t increment, const uint8_t* si, uint8_t* so,
    int num, float* out) {
  uint16_t inc = static_cast<uint16_t>(increment >> 8);
  uint16_t spread = static_cast<uint16_t>(
      (static_cast<uint32_t>(inc) * parameter) >> 13);
  ++spread;
  uint16_t acc = inc;
  uint16_t increments[3];
  for (int k = 0; k < 3; ++k) {
    acc = static_cast<uint16_t>(acc + spread);
    increments[k] = acc;
  }
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      phase = 0;
    }
    Carry24 c = Add24(phase, increment);
    phase = c.value;
    if (so != nullptr) {
      *so++ = c.carry;
    }
    state_.qs[0] = static_cast<uint16_t>(state_.qs[0] + increments[0]);
    state_.qs[1] = static_cast<uint16_t>(state_.qs[1] + increments[1]);
    state_.qs[2] = static_cast<uint16_t>(state_.qs[2] + increments[2]);
    uint8_t value = static_cast<uint8_t>((phase >> 8) >> 10);
    value = static_cast<uint8_t>(value + (state_.qs[0] >> 10));
    value = static_cast<uint8_t>(value + (state_.qs[1] >> 10));
    value = static_cast<uint8_t>(value + (state_.qs[2] >> 10));
    out[i] = ToSample(value);
  }
  phase_ = phase;
}

inline void HdOscillator::RenderFilteredNoise(
    uint8_t parameter, const uint8_t* si, int num, float* out) {
  uint16_t rng_state = state_.no.rng_state;
  if (rng_state == 0) {
    ++rng_state;
  }
  uint8_t filter_coefficient = static_cast<uint8_t>(parameter << 2);
  if (filter_coefficient <= 4) {
    filter_coefficient = 4;
  }
  uint8_t lp = state_.no.lp_noise_sample;
  // Classic FilteredNoise does not advance the phase (there is no UPDATE_PHASE
  // in the loop), so no sync-output stream is produced here either.
  for (int i = 0; i < num; ++i) {
    if (si != nullptr && *si++) {
      rng_state = state_.no.rng_reset_value;
    }
    const uint16_t feedback = (rng_state & 1u) != 0u
                                  ? static_cast<uint16_t>(0xb400)
                                  : static_cast<uint16_t>(0);
    rng_state = static_cast<uint16_t>((rng_state >> 1) ^ feedback);
    uint8_t noise = static_cast<uint8_t>(rng_state >> 8);
    lp = U8Mix(lp, noise, filter_coefficient);
    uint8_t sample;
    if (parameter >= 64) {
      sample = static_cast<uint8_t>(
          static_cast<int>(noise) - static_cast<int>(lp) - 128);
    } else {
      sample = lp;
    }
    out[i] = ToSample(sample);
  }
  state_.no.rng_state = rng_state;
  state_.no.lp_noise_sample = lp;
}

inline void HdOscillator::RenderVowel(uint32_t increment, int num, float* out) {
  state_.vw.update = static_cast<uint8_t>((state_.vw.update + 1) & 3);
  if (!state_.vw.update) {
    uint8_t offset_1 = static_cast<uint8_t>(
        static_cast<uint8_t>(parameter_ >> 4) * 7);
    uint8_t offset_2 = static_cast<uint8_t>(offset_1 + 7);
    uint8_t balance = static_cast<uint8_t>(parameter_ & 15);
    for (int i = 0; i < 3; ++i) {
      state_.vw.formant_increment[i] = U8U4MixU12(
          tables_.vowel_data[offset_1 + i],
          tables_.vowel_data[offset_2 + i], balance);
      state_.vw.formant_increment[i] =
          static_cast<uint16_t>(state_.vw.formant_increment[i] << 3);
    }
    for (int i = 0; i < 3; ++i) {
      state_.vw.formant_amplitude[i] = U8U4MixU8(
          tables_.vowel_data[offset_1 + 3 + i],
          tables_.vowel_data[offset_2 + 3 + i], balance);
    }
    state_.vw.noise_modulation = U8U4MixU8(
        tables_.vowel_data[offset_1 + 6],
        tables_.vowel_data[offset_2 + 6], balance);
  }
  uint32_t phase = phase_;
  for (int i = 0; i < num; i += 2) {
    int8_t result = 0;
    for (int k = 0; k < 3; ++k) {
      state_.vw.formant_phase[k] = static_cast<uint16_t>(
          state_.vw.formant_phase[k] + state_.vw.formant_increment[k]);
      const uint8_t* ftable =
          (k == 2) ? tables_.formant_square : tables_.formant_sine;
      uint8_t fidx = static_cast<uint8_t>(
          ((state_.vw.formant_phase[k] >> 8) & 0xf0u) |
          state_.vw.formant_amplitude[k]);
      result = static_cast<int8_t>(
          static_cast<int>(result) + static_cast<uint8_t>(ftable[fidx]));
    }
    result = S8U8MulShift8(
        result, static_cast<uint8_t>(~static_cast<uint8_t>(phase >> 16)));
    // The vowel renderer advances only the integral part of the phase (Classic
    // accumulates phase_increment_.integral), skipping the fractional part.
    phase = (phase + (static_cast<uint32_t>(
        static_cast<uint16_t>(increment >> 8)) << 8)) & 0xffffffu;
    int8_t phase_msb = random_ != nullptr
        ? static_cast<int8_t>(random_->state_msb()) : 0;
    int phase_noise = static_cast<int>(phase_msb) *
        static_cast<int>(static_cast<int8_t>(state_.vw.noise_modulation));
    if ((static_cast<int>(static_cast<uint16_t>(phase >> 8)) + phase_noise) <
        static_cast<int>(static_cast<uint16_t>(increment >> 8))) {
      state_.vw.formant_phase[0] = 0;
      state_.vw.formant_phase[1] = 0;
      state_.vw.formant_phase[2] = 0;
    }
    uint8_t x = static_cast<uint8_t>(
        static_cast<int>(S16ClipS8(static_cast<int16_t>(4 * result))) + 128);
    Sample value = ToSample(x);
    out[i] = value;
    out[i + 1] = value;
  }
  phase_ = phase;
}

inline float PolyBlep(float t, float dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

// ---------------------------------------------------------------------------
// HD (band-limited) waves.
// ---------------------------------------------------------------------------

inline void HdOscillator::RenderHd(
    OscWave wave, uint8_t note, uint32_t increment, uint8_t parameter,
    int num, float* out) {
  (void)note;
  const float cycle = 16777216.0f;
  const float dt = static_cast<float>(increment) / cycle;
  uint32_t phase = phase_;
  for (int i = 0; i < num; ++i) {
    phase = (phase + increment) & 0xffffffu;
    const float t = static_cast<float>(phase) / cycle;
    float y = 0.0f;
    switch (wave) {
      case OscWave::kSine: {
        y = std::sin(6.283185307179586f * t);
        break;
      }
      case OscWave::kSawBlp: {
        y = 2.0f * t - 1.0f - PolyBlep(t, dt);
        break;
      }
      case OscWave::kSquareBlp: {
        float duty = 0.5f + (static_cast<float>(parameter) - 128.0f) *
            (0.45f / 128.0f);
        if (duty < 0.05f) {
          duty = 0.05f;
        }
        if (duty > 0.95f) {
          duty = 0.95f;
        }
        y = (t < duty) ? 1.0f : -1.0f;
        y += PolyBlep(t, dt);
        float edge = t + 1.0f - duty;
        if (edge >= 1.0f) {
          edge -= 1.0f;
        }
        y -= PolyBlep(edge, dt);
        break;
      }
      case OscWave::kTriangleHd: {
        y = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        break;
      }
      default:
        break;
    }
    out[i] = y;
  }
  phase_ = phase;
}

// ---------------------------------------------------------------------------
// Dispatch.
// ---------------------------------------------------------------------------

inline void HdOscillator::RenderNaive(
    OscWave wave, uint8_t note, uint32_t increment, uint8_t parameter,
    int num, float* out, const uint8_t* sync_input, uint8_t* sync_output) {
  parameter_ = parameter;
  switch (wave) {
    case OscWave::kSilence:
      RenderSilence(num, out);
      break;
    case OscWave::kSaw:
      RenderNaiveFull(kRealSaw, note, increment, num, out,
                      sync_input, sync_output);
      break;
    case OscWave::kTriangle:
      RenderNaiveFull(kRealTriangle, note, increment, num, out,
                      sync_input, sync_output);
      break;
    case OscWave::kSquare:
      RenderNaiveFull(kRealSquare, note, increment, num, out,
                      sync_input, sync_output);
      break;
    case OscWave::kSine:
    case OscWave::kSawBlp:
    case OscWave::kSquareBlp:
    case OscWave::kTriangleHd:
      RenderHd(wave, note, increment, parameter, num, out);
      break;
    default:
      break;
  }
}

inline void HdOscillator::RenderNaiveFull(
    uint8_t shape, uint8_t note, uint32_t increment, int num, float* out,
    const uint8_t* sync_input, uint8_t* sync_output) {
  shape_ = shape;
  note_ = note;
  last_increment_integral_ = static_cast<uint16_t>((increment >> 8) & 0xffffu);
  if (shape_ == kRealSquare) {
    if (parameter_ == 0) {
      RenderSimple(kFamilySquare, false, note_, increment, parameter_,
                   sync_input, sync_output, num, out);
    } else {
      RenderBandlimitedPwm(note_, increment, parameter_,
                           sync_input, sync_output, num, out);
    }
    return;
  }
  uint8_t index = static_cast<uint8_t>(shape_) > kRealVowel
      ? static_cast<uint8_t>(kRealWavetable1) : shape_;
  switch (index) {
    case kRealSilence:
      RenderSilence(num, out);
      break;
    case kRealSaw:
      RenderSimple(kFamilySaw, true, note_, increment, parameter_,
                   sync_input, sync_output, num, out);
      break;
    case kRealTriangle:
      RenderSimple(kFamilyTriangle, false, note_, increment, parameter_,
                   sync_input, sync_output, num, out);
      break;
    case kRealCzSaw:
      RenderCzSaw(note_, parameter_, increment, sync_input, sync_output, num, out);
      break;
    case kRealCzReso:
    case kRealCzTriangle:
    case kRealCzSync:
      RenderCzReso(shape_, parameter_, increment, sync_input, sync_output,
                   num, out);
      break;
    case kRealCzPulse:
      RenderCzPulseReso(shape_, parameter_, increment, sync_input, sync_output,
                        num, out);
      break;
    case kRealQuadSawPad:
      RenderQuadSawPad(parameter_, increment, sync_input, sync_output, num, out);
      break;
    case kRealFm:
      RenderFm(increment, sync_input, sync_output, num, out);
      break;
    case kRealWavetable1:
    case kRealWavetable2:
    case kRealWavetable3:
    case kRealWavetable4:
    case kRealWavetable5:
    case kRealWavetable6:
    case kRealWavetable7:
    case kRealWavetable8:
      RenderInterpolatedWavetable(increment, sync_input, sync_output, num, out);
      break;
    case kRealWavetableUser:
      RenderSweepingWavetableRam(increment, sync_input, sync_output, num, out);
      break;
    case kReal8BitLand:
      Render8BitLand(parameter_, increment, sync_input, sync_output, num, out);
      break;
    case kRealCrushedSine:
      RenderCrushedSine(parameter_, increment, sync_input, sync_output, num, out);
      break;
    case kRealDirtyPwm:
      RenderDirtyPwm(parameter_, increment, sync_input, sync_output, num, out);
      break;
    case kRealFilteredNoise:
      RenderFilteredNoise(parameter_, sync_input, num, out);
      break;
    case kRealVowel:
      RenderVowel(increment, num, out);
      break;
    default:
      if (shape_ >= kRealWavetable9) {
        RenderInterpolatedWavetable(increment, sync_input, sync_output, num, out);
      } else {
        RenderSilence(num, out);
      }
      break;
  }
}

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_OSCILLATOR_H_