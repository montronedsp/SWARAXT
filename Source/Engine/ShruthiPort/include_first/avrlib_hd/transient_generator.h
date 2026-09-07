// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi transient generator. Created independently for the HD
// engine; not copied from Shruthi avrlib / Shruthi firmware (GPL-3.0,
// (c) 2009 Emilie Gillet).
//
// Classic one-shot shapes (src/patch.h SubOscillatorAlgorithm
// WAVEFORM_SUB_OSC_CLICK..POP=6..10), fed raw by the firmware and treated as
// numeric constants here:
//   kSubOscClick=6, kSubOscGlitch=7, kSubOscBlow=8, kSubOscMetallic=9,
//   kSubOscPop=10.
//
// Two rendering families:
//   - Faithful (naive): bit-for-bit replication of the Classic
//     shruthi::TransientGenerator (the counter-driven one-shot, the
//     gain_/counter_ LCG rng_state_ (rng = rng*73 + counter), the 17-step
//     decimator, and the byte-domain U8Mix against the buffer).
//   - HD: bounded analytic one-shots with the same counter/gain pacing and
//     the same shape family (click, glitch, blow, metallic, pop) as float
//     generators mixed in place.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md (sub-osc/transient) and
// HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_TRANSIENT_GENERATOR_H_
#define AVRLIB_HD_TRANSIENT_GENERATOR_H_

#include <cstdint>

#include "avrlib_hd/random.h"
#include "avrlib_hd/types.h"

namespace avrlib_hd {

enum TransientWave {
  kSubOscClick = 6,
  kSubOscGlitch = 7,
  kSubOscBlow = 8,
  kSubOscMetallic = 9,
  kSubOscPop = 10,
  kSubOscWaveLast
};

class HdTransientGenerator {
 public:
  HdTransientGenerator() = default;

  void Init(Random* random) { random_ = random; }

  void Trigger() { counter_ = 255; }

  // Faithful: byte-exact equivalent of the Classic TransientGenerator::Render
  // (in-place byte mix). Runs while the internal counter is non-zero.
  void RenderNaiveByte(uint8_t shape, uint8_t* buffer, uint8_t amount) {
    if (shape < kSubOscClick) {
      return;
    }
    if (shape > kSubOscPop) {
      shape = static_cast<uint8_t>(kSubOscPop);
    }
    for (int i = 0; i < kAudioBlockSize; ++i) {
      if (!counter_) break;
      uint8_t value = RenderOneNaive(shape);
      uint8_t amplitude =
          static_cast<uint8_t>((static_cast<uint16_t>(gain_) * amount) >> 8);
      buffer[i] = MixU8(buffer[i], value, amplitude);
    }
  }

  // Faithful output as float (-1..1, byte-128)/128 mapped sample-by-sample.
  // amount is a float fraction 0..1 of the 0..255 classic mix amount, so the
  // byte amplitude matches the classic (gain_ * amount_byte) >> 8.
  void RenderNaive(uint8_t shape, float* buffer, int size, float amount) {
    if (shape < kSubOscClick || size <= 0) {
      return;
    }
    if (shape > kSubOscPop) {
      shape = static_cast<uint8_t>(kSubOscPop);
    }
    const uint16_t amount_byte = static_cast<uint16_t>(
        amount * 255.0f + 0.5f);
    for (int i = 0; i < size; ++i) {
      if (!counter_) break;
      uint8_t value = RenderOneNaive(shape);
      uint8_t amplitude = static_cast<uint8_t>(
          (static_cast<uint16_t>(gain_) * amount_byte) >> 8);
      float vf = static_cast<float>(static_cast<int>(value) - 128) *
                 (1.0f / 128.0f);
      buffer[i] = buffer[i] * (1.0f - static_cast<float>(amplitude) *
                                          (1.0f / 255.0f)) +
                  vf * static_cast<float>(amplitude) * (1.0f / 255.0f);
    }
  }

  // HD: analytic one-shots with the same family and gain pacing; bounded in
  // [-1,1], mixed in place with a float amount (0..1).
  void RenderHd(uint8_t shape, float* buffer, int size, float amount) {
    if (shape < kSubOscClick || size <= 0) {
      return;
    }
    if (shape > kSubOscPop) {
      shape = static_cast<uint8_t>(kSubOscPop);
    }
    for (int i = 0; i < size; ++i) {
      if (!counter_) break;
      float v = RenderOneHd(shape);
      float amp = static_cast<float>(gain_) * (1.0f / 255.0f) * amount;
      buffer[i] = buffer[i] * (1.0f - amp) + v * amp;
    }
  }

 private:
  uint8_t RenderOneNaive(uint8_t shape) {
    switch (shape) {
      case kSubOscClick:
        gain_ = counter_;
        counter_ = static_cast<uint8_t>(counter_ - 1);
        return counter_ < 32 ? 255 : 0;
      case kSubOscGlitch:
        gain_ = counter_;
        counter_ = static_cast<uint8_t>(counter_ - 1);
        rng_state_ = static_cast<uint8_t>(static_cast<uint16_t>(rng_state_) *
                                              73 + counter_);
        return rng_state_;
      case kSubOscBlow:
        decimate_ = static_cast<uint8_t>(decimate_ + 2);
        if (decimate_ >= 16) {
          decimate_ = static_cast<uint8_t>(decimate_ - 17);
          rng_state_ = static_cast<uint8_t>(static_cast<uint16_t>(rng_state_) *
                                               73 + counter_);
          if (decimate_ == 0) {
            counter_ = static_cast<uint8_t>(counter_ - 1);
            gain_ = (counter_ & 0x80) ? static_cast<uint8_t>(~counter_)
                                      : counter_;
          }
        }
        return rng_state_;
      case kSubOscMetallic:
        counter_ = static_cast<uint8_t>(counter_ - 1);
        gain_ = counter_ >= 64 ? 255 : static_cast<uint8_t>(counter_ << 2);
        return static_cast<uint8_t>(static_cast<uint16_t>(counter_) * 57);
      default:
        counter_ = static_cast<uint8_t>(counter_ - 1);
        gain_ = counter_ > 0 ? 255 : 0;
        return 0;
    }
  }

  float RenderOneHd(uint8_t shape) {
    switch (shape) {
      case kSubOscClick:
        gain_ = counter_;
        counter_ = static_cast<uint8_t>(counter_ - 1);
        return counter_ < 32 ? 1.0f : 0.0f;
      case kSubOscGlitch:
        gain_ = counter_;
        counter_ = static_cast<uint8_t>(counter_ - 1);
        return random_ != nullptr ? RandomFloat(random_) : 0.0f;
      case kSubOscBlow:
        decimate_ = static_cast<uint8_t>(decimate_ + 2);
        if (decimate_ >= 16) {
          decimate_ = static_cast<uint8_t>(decimate_ - 17);
          hd_rng_state_ = static_cast<uint8_t>(static_cast<uint16_t>(
              hd_rng_state_) * 73 + counter_);
          if (decimate_ == 0) {
            counter_ = static_cast<uint8_t>(counter_ - 1);
            gain_ = (counter_ & 0x80) ? static_cast<uint8_t>(~counter_)
                                      : counter_;
          }
        }
        return static_cast<float>(static_cast<int>(hd_rng_state_) - 128) *
               (1.0f / 128.0f);
      case kSubOscMetallic:
        counter_ = static_cast<uint8_t>(counter_ - 1);
        gain_ = counter_ >= 64 ? 255 : static_cast<uint8_t>(counter_ << 2);
        return static_cast<float>(static_cast<int>(counter_) * 57 % 256 - 128) *
               (1.0f / 128.0f);
      default:
        counter_ = static_cast<uint8_t>(counter_ - 1);
        gain_ = counter_ > 0 ? 255 : 0;
        return -1.0f;
    }
  }

  static float RandomFloat(Random* random) {
    return (static_cast<float>(random->GetByte()) - 128.0f) * (1.0f / 128.0f);
  }

  // Exact Classic primitive (avrlib/op.h U8Mix), replicated.
  static uint8_t MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>((a * (255 - balance) + b * balance) >> 8);
  }

  // Faithful state (Classic layout).
  uint8_t rng_state_ = 0;
  uint8_t decimate_ = 0;
  uint8_t gain_ = 0;
  uint8_t counter_ = 0;

  // HD auxiliary state.
  uint8_t hd_rng_state_ = 0;

  Random* random_ = nullptr;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdTransientGenerator);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_TRANSIENT_GENERATOR_H_