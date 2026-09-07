// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi LFO. Created independently for the HD engine; not
// copied from Shruthi avrlib / Shruthi firmware (GPL-3.0, (c) 2009 Emilie
// Gillet).
//
// Two rendering families, selected by the engine:
//   - Faithful (naive): replicates the Classic Shruthi Lfo bit-for-bit
//     (uint16 phase/increment, ramp-to-16383 intensity envelope, cycle wrap
//     detection, sample-and-hold via the exact Classic LFSR, one-shot latch,
//     wav_res_waves bank) and exposes the resulting 0..255 value byte plus a
//     float 0..1 unit mapping.
//   - HD: the same structure with an explicit cycle-end threshold and float
//     intensity, deterministic and without the carry idiom.
//
// The faithful renderer addresses the genuine Classic LUTs (env/portamento
// increments for the intensity ramp, wav_res_waves for the wavetable shapes),
// so the engine wires them as raw pointers.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md (env/LFO) and HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_LFO_H_
#define AVRLIB_HD_LFO_H_

#include <cstdint>

#include "avrlib_hd/random.h"
#include "avrlib_hd/types.h"

namespace avrlib_hd {

// Classic waveform values (shruthi/patch.h LfoWave), reproduced so the engine
// passes the same shape bytes to both the Classic and the HD LFO.
enum LfoWave {
  kLfoTriangle = 0,
  kLfoSquare = 1,
  kLfoSH = 2,
  kLfoRamp = 3,
  kLfoStepSequencer = 4,
  kLfoWave1 = 5,
  kLfoWave2 = 6,
  kLfoWave3 = 7,
  kLfoWave4 = 8,
  kLfoWave5 = 9,
  kLfoWave6 = 10,
  kLfoWave7 = 11,
  kLfoWave8 = 12,
  kLfoWave9 = 13,
  kLfoWave10 = 14,
  kLfoWave11 = 15,
  kLfoWave12 = 16,
  kLfoWave13 = 17,
  kLfoWave14 = 18,
  kLfoWave15 = 19,
  kLfoWave16 = 20,
  kLfoWaveLast = 21
};

// Classic retrigger modes (shruthi/patch.h LfoMode).
enum LfoMode {
  kLfoModeFree = 0,
  kLfoModeSlave = 1,
  kLfoModeMaster = 2,
  kLfoModeOneShot = 3,
  kLfoModeLast = 4
};

// Raw Classic tables used by the faithful renderer.
struct LfoTables {
  const uint16_t* portamento_increments;  // lut_res_env_portamento_increments
  const uint8_t* waves;                   // wav_res_waves (LFO waveform bank)
};

class HdLfo {
 public:
  HdLfo() = default;

  void set_tables(const LfoTables& tables) { tables_ = tables; }

  void Init(Random* random) {
    random_ = random;
  }

  // Classic Lfo::Update equivalent.
  void Update(uint8_t shape, uint16_t phase_increment, uint8_t attack,
              uint8_t retrigger_mode) {
    shape_ = shape;
    if (phase_increment) {
      phase_increment_ = phase_increment;
    }
    intensity_increment_ =
        static_cast<uint16_t>(tables_.portamento_increments[attack] >> 1);
    retrigger_mode_ = retrigger_mode;
  }

  void ResetPhase() {
    phase_ = 0;
    phase_f_ = 0.0f;
    cycle_complete_ = true;
    running_ = true;
  }

  void Trigger() {
    if (retrigger_mode_) {
      ResetPhase();
    }
    intensity_ = 0;
    intensity_f_ = 0.0f;
  }

  void Reset() {
    ResetPhase();
    Trigger();
  }

  // Faithful: byte-exact equivalent of the Classic Lfo::Render(). step_cc is
  // the 16 step-sequencer controller nibbles (0..15); only used when shape_ ==
  // kLfoStepSequencer. Call once per control block, where the engine calls the
  // Classic LFO render.
  uint8_t RenderNaiveByte(const uint8_t* step_cc, uint8_t pattern_size) {
    // Ramp the intensity envelope to 16383 (Classic saturates at 0x3fff).
    uint16_t i = intensity_;
    if (static_cast<uint8_t>(i >> 8) != 0x3f) {
      i = static_cast<uint16_t>(i + intensity_increment_);
      if (static_cast<uint8_t>(i >> 8) >= 0x40) {
        i = 16383;
      }
      intensity_ = i;
    }

    cycle_complete_ = phase_ < phase_increment_;

    uint8_t value;
    switch (shape_) {
      case kLfoRamp:
        value = static_cast<uint8_t>(phase_ >> 8);
        break;

      case kLfoSH:
        if (cycle_complete_ && random_ != nullptr) {
          value_ = random_->GetByte();
        }
        value = value_;
        break;

      case kLfoTriangle:
        value = (phase_ & 0x8000)
            ? static_cast<uint8_t>(phase_ >> 7)
            : static_cast<uint8_t>(~(static_cast<uint8_t>(phase_ >> 7)));
        break;

      case kLfoSquare:
        value = (phase_ & 0x8000) ? 255 : 0;
        break;

      case kLfoStepSequencer: {
        uint8_t step = static_cast<uint8_t>(phase_ >> 8);
        step = static_cast<uint8_t>((step * pattern_size) >> 8);
        value = static_cast<uint8_t>(step_cc[step] << 4);
      } break;

      default: {
        uint8_t offset = static_cast<uint8_t>(shape_ - kLfoWave1);
        if (offset == 0) {
          offset = 3;
        } else {
          offset = static_cast<uint8_t>(offset + 16);
        }
        value = tables_.waves[
            static_cast<uint16_t>(offset * 129) +
            static_cast<uint8_t>(phase_ >> 9)];
      } break;
    }

    phase_ = static_cast<uint16_t>(phase_ + phase_increment_);

    if (retrigger_mode_ == kLfoModeOneShot) {
      if (running_) {
        if (phase_ < phase_increment_) {
          running_ = false;
          if (shape_ == kLfoRamp || shape_ == kLfoTriangle ||
              shape_ == kLfoSquare) {
            value = 255;
          } else if (shape_ == kLfoStepSequencer) {
            value = 0;
          }
          one_shot_value_ = value;
        }
      } else {
        value = one_shot_value_;
      }
      value = shape_ == kLfoStepSequencer
          ? static_cast<uint8_t>(128 + (value >> 1))
          : static_cast<uint8_t>(255 - (value >> 1));
    }

    // Apply the intensity envelope: S8U8MulShift8(v - 128, intensity >> 6)
    // + 128. The Classic source truncates to int8 (mod-256) before the
    // multiply, which for v in [128,255] equals (v - 128) as a positive
    // value; the arithmetic shift below reproduces it exactly.
    int16_t m = static_cast<int16_t>(value) - 128;
    int16_t scaled =
        static_cast<int16_t>((m * static_cast<int16_t>(intensity_ >> 6)) >> 8);
    return static_cast<uint8_t>(scaled + 128);
  }

  // Faithful output as float unit level (0..1). The byte form is
  // authoritative for parity.
  float RenderNaive(const uint8_t* step_cc, uint8_t pattern_size) {
    return static_cast<float>(RenderNaiveByte(step_cc, pattern_size)) *
           (1.0f / 255.0f);
  }

  // HD: float intensity env and explicit cycle end for the one-shot/CV logic.
  // The event timeline mirrors the faithful/Classic render exactly (a cycle is
  // "complete" when the previous phase update wrapped; S&H latches then; the
  // one-shot latches on the wrap of the current update).
  float RenderHd(const uint8_t* step_cc, uint8_t pattern_size) {
    if (intensity_f_ < kIntensityMax) {
      intensity_f_ += static_cast<float>(intensity_increment_);
      if (intensity_f_ > kIntensityMax) {
        intensity_f_ = kIntensityMax;
      }
    }

    const bool wrapped_prev = phase_f_ < static_cast<float>(phase_increment_);
    cycle_complete_ = wrapped_prev;

    float value;
    switch (shape_) {
      case kLfoRamp:
        value = phase_f_ * (1.0f / 65536.0f);
        break;
      case kLfoSH:
        if (wrapped_prev && random_ != nullptr) {
          value_ = random_->GetByte();
        }
        value = static_cast<float>(value_) * (1.0f / 255.0f);
        break;
      case kLfoTriangle: {
        // pc in [0,1): rising sweep over the first half-cycle, falling over
        // the second, symmetric and exactly bounded.
        float pc = phase_f_ * (1.0f / 65536.0f);
        value = pc < 0.5f ? pc * 2.0f : 2.0f - pc * 2.0f;
      } break;
      case kLfoSquare:
        value = phase_f_ < 32768.0f ? 0.0f : 1.0f;
        break;
      case kLfoStepSequencer: {
        uint8_t step = static_cast<uint8_t>(phase_f_ / 256.0f);
        step = static_cast<uint8_t>((step * pattern_size) >> 8);
        value = static_cast<float>(step_cc[step]) * (1.0f / 15.0f);
      } break;
      default: {
        uint8_t offset = static_cast<uint8_t>(shape_ - kLfoWave1);
        if (offset == 0) {
          offset = 3;
        } else {
          offset = static_cast<uint8_t>(offset + 16);
        }
        uint16_t base = static_cast<uint16_t>(offset * 129);
        float x = phase_f_ * (128.0f / 65536.0f);
        int ix = static_cast<int>(x);
        if (ix > 127) ix = 127;
        float frac = x - static_cast<float>(ix);
        uint8_t v0 = tables_.waves[static_cast<uint16_t>(base + ix)];
        uint8_t v1 = tables_.waves[static_cast<uint16_t>(base + ix + 1)];
        value = (static_cast<float>(v0) * (1.0f - frac) +
                 static_cast<float>(v1) * frac) * (1.0f / 255.0f);
      } break;
    }

    phase_f_ += static_cast<float>(phase_increment_);
    const bool wrapped_now = phase_f_ >= kCycleEnd;
    if (wrapped_now) {
      phase_f_ -= kCycleEnd;
    }

    if (retrigger_mode_ == kLfoModeOneShot) {
      if (running_) {
        if (wrapped_now) {
          running_ = false;
          if (shape_ == kLfoRamp || shape_ == kLfoTriangle ||
              shape_ == kLfoSquare) {
            value = 1.0f;
          } else if (shape_ == kLfoStepSequencer) {
            value = 0.0f;
          }
          one_shot_f_ = value;
        }
      } else {
        value = one_shot_f_;
      }
      value = shape_ == kLfoStepSequencer
          ? 0.5f + value * 0.5f
          : 1.0f - value * 0.5f;
    }

    return 0.5f * (1.0f + (2.0f * value - 1.0f) *
                              (intensity_f_ / kIntensityMax));
  }

  bool triggered() const { return cycle_complete_; }
  uint16_t phase_increment_for_tests() const { return phase_increment_; }
  uint16_t phase_for_tests() const { return phase_; }
  uint8_t shape_for_tests() const { return shape_; }

 private:
  static constexpr float kCycleEnd = 65536.0f;
  static constexpr float kIntensityMax = 16383.0f;

  LfoTables tables_{};

  // Faithful integer state (mirrors the Classic class layout and semantics).
  uint16_t phase_increment_ = 0;
  uint16_t intensity_increment_ = 0;
  uint16_t intensity_ = 0;
  uint16_t phase_ = 0;
  uint8_t shape_ = 0;
  uint8_t retrigger_mode_ = 0;
  bool cycle_complete_ = false;
  bool running_ = false;
  uint8_t value_ = 0;
  uint8_t one_shot_value_ = 0;

  // HD float state.
  float intensity_f_ = 0.0f;
  float phase_f_ = 0.0f;
  float one_shot_f_ = 0.0f;

  Random* random_ = nullptr;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdLfo);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_LFO_H_