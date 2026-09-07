// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi sub-oscillator. Created independently for the HD
// engine; not copied from Shruthi avrlib / Shruthi firmware (GPL-3.0,
// (c) 2009 Emilie Gillet).
//
// Two rendering families, selected by the engine:
//   - Faithful (naive): replicates the Classic shruthi::SubOscillator
//     bit-for-bit (24-bit phase accumulator with its natural wrap, the shape
//     >= 3 octave-down retag, the pulse/triangle branch, and the byte-domain
//     U8Mix against the incoming buffer) and exposes the resulting mixed byte
//     buffer; the float mapping is (byte - 128) / 128 with linear float mix.
//   - HD: the six documented Sub-Osc waves as analytic square / triangle / 25%
//     pulse at base and -1 octave, free of the 24-bit accumulator, blended
//     into a float buffer with float amount.
//
// Note on the Classic shape mapping: the engine hands the raw
// SubOscillatorAlgorithm enum (0..5 = square/tri/pulse at base and at -1
// octave) to SubOscillator::Render; the "shape >= 3" branch retags
// 3..5 as 0..2 with a halved phase increment, which is exactly the intended
// -1 octave encoding. The faithful renderer is byte-exact for whatever shape
// byte is fed; the HD renderer plays the six waves as named.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md (sub-osc/transient) and
// HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_SUB_OSCILLATOR_H_
#define AVRLIB_HD_SUB_OSCILLATOR_H_

#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

// Normalized HD Sub-Osc waves. Classic firmware passes the raw
// SubOscillatorAlgorithm values (0..5); the HD renderer plays these as named.
enum SubOscWave {
  kSubOscSquare1 = 0,   // square, base octave
  kSubOscTriangle1 = 1,  // triangle, base octave
  kSubOscPulse1 = 2,    // 25% pulse, base octave
  kSubOscSquare2 = 3,   // square, -1 octave
  kSubOscTriangle2 = 4,  // triangle, -1 octave
  kSubOscPulse2 = 5,    // 25% pulse, -1 octave
  kSubOscLast
};

class HdSubOscillator {
 public:
  HdSubOscillator() = default;

  void set_increment(uint32_t increment24) {
    increment_ = increment24 & 0xFFFFFFu;
  }

  void Reset() {
    phase_ = 0;
    phase_f_ = 0.0f;
  }

  // Faithful: byte-exact equivalent of the Classic SubOscillator::Render().
  // The Classic 24-bit accumulator is reproduced with a uint32 masked to 24
  // bits; the buffer is mixed in place in the 0..255 domain.
  void RenderNaiveByte(uint8_t shape, uint8_t* buffer, uint8_t amount) {
    uint32_t inc = increment_;
    if (shape >= 3) {
      inc >>= 1;
      shape = static_cast<uint8_t>(shape - 3);
    }
    const uint8_t pulse_width = shape == 0 ? 0x80 : 0x40;
    const uint16_t sub_gain = amount;
    for (int i = 0; i < kAudioBlockSize; ++i) {
      phase_ = (phase_ + inc) & 0xFFFFFFu;
      uint8_t v;
      if (shape != 1) {
        v = static_cast<uint8_t>(phase_ >> 16) < pulse_width ? 0 : 255;
      } else {
        uint8_t tri = static_cast<uint8_t>(phase_ >> 15);
        v = (phase_ & 0x800000u) ? tri : static_cast<uint8_t>(~tri);
      }
      buffer[i] = MixU8(buffer[i], v, sub_gain);
    }
  }

  // Faithful output as float (-1..1, byte-128)/128, mixed linearly. The byte
  // path is authoritative for parity.
  void RenderNaive(uint8_t shape, float* buffer, int size, float amount) {
    uint32_t inc = increment_;
    if (shape >= 3) {
      inc >>= 1;
      shape = static_cast<uint8_t>(shape - 3);
    }
    const uint8_t pulse_width = shape == 0 ? 0x80 : 0x40;
    for (int i = 0; i < size; ++i) {
      phase_ = (phase_ + inc) & 0xFFFFFFu;
      uint8_t v;
      if (shape != 1) {
        v = static_cast<uint8_t>(phase_ >> 16) < pulse_width ? 0 : 255;
      } else {
        uint8_t tri = static_cast<uint8_t>(phase_ >> 15);
        v = (phase_ & 0x800000u) ? tri : static_cast<uint8_t>(~tri);
      }
      buffer[i] = buffer[i] * (1.0f - amount) +
                  static_cast<float>(static_cast<int>(v) - 128) *
                      (1.0f / 128.0f) * amount;
    }
  }

  // HD: analytic waves, the six named shapes, deterministic and bounded.
  void RenderHd(uint8_t shape, float* buffer, int size, float amount) {
    float inc = static_cast<float>(increment_) * (1.0f / 16777216.0f);
    if (shape >= 3) {
      inc *= 0.5f;
      shape = static_cast<uint8_t>(shape - 3);
    }
    const float keep = 1.0f - amount;
    for (int i = 0; i < size; ++i) {
      phase_f_ += inc;
      if (phase_f_ >= 1.0f) {
        phase_f_ -= 1.0f;
      }
      float v;
      switch (shape) {
        case 0:  // square 50%
          v = phase_f_ < 0.5f ? 1.0f : -1.0f;
          break;
        case 2:  // pulse 25%
          v = phase_f_ < 0.25f ? 1.0f : -1.0f;
          break;
        default:  // triangle
          v = phase_f_ < 0.5f ? 4.0f * phase_f_ - 1.0f
                              : 3.0f - 4.0f * phase_f_;
          break;
      }
      buffer[i] = buffer[i] * keep + v * amount;
    }
  }

 private:
  // Exact Classic primitive (avrlib/op.h U8Mix), replicated.
  static uint8_t MixU8(uint8_t a, uint8_t b, uint16_t balance) {
    return static_cast<uint8_t>((a * (255 - balance) + b * balance) >> 8);
  }

  // Faithful 24-bit accumulator state.
  uint32_t increment_ = 0;
  uint32_t phase_ = 0;

  // HD phase state (0..1).
  float phase_f_ = 0.0f;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdSubOscillator);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_SUB_OSCILLATOR_H_