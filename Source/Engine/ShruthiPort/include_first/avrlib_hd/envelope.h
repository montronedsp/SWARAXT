// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi envelope. Created independently for the HD engine;
// not copied from Shruthi avrlib / Shruthi firmware (GPL-3.0, (c) 2009 Emilie
// Gillet).
//
// Two rendering families, selected by the engine:
//   - Faithful (naive): replicates the Classic Shruthi Envelope bit-for-bit
//     (same uint16 phase/increment, same U8MixU16 + env_expo interpolation,
//     same wrap-driven segment advance, same sustain clamp) and exposes the
//     resulting 0..255 gain byte plus a float 0..1 unit mapping.
//   - HD: the same perceptual curve (attack/decay/release increments from the
//     same LUT, the same env_expo shaping table) but with an explicit
//     segment-end threshold instead of uint16 overflow. Deterministic, no
//     carry idiom; the "step" of the Classic attack-to-decay transition is
//     reproduced by clamping at the segment target (architecture doc 4.2).
//
// The faithful renderer addresses the genuine Classic LUTs (env_expo,
// env/portamento increments), so the engine wires them as raw pointers.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md (env/LFO) and HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_ENVELOPE_H_
#define AVRLIB_HD_ENVELOPE_H_

#include <cstdint>

#include "avrlib_hd/types.h"

namespace avrlib_hd {

// Classic envelope stages (shruthi/envelope.h EnvelopeStage), reproduced so
// the faithful state machine advances identically.
enum EnvelopeStage {
  kEnvelopeAttack = 0,
  kEnvelopeDecay = 1,
  kEnvelopeSustain = 2,
  kEnvelopeRelease = 3,
  kEnvelopeDead = 4,
  kNumEnvelopeSegments
};

// Raw Classic tables used by the faithful renderer.
struct EnvelopeTables {
  const uint16_t* portamento_increments;  // lut_res_env_portamento_increments
  const uint8_t* env_expo;                // wav_res_env_expo (257 bytes)
};

class HdEnvelope {
 public:
  HdEnvelope() = default;

  void set_tables(const EnvelopeTables& tables) { tables_ = tables; }

  void Init() {
    stage_target_[kEnvelopeAttack] = 255;
    stage_target_[kEnvelopeRelease] = 0;
    stage_target_[kEnvelopeDead] = 0;
    stage_phase_increment_[kEnvelopeSustain] = 0;
    stage_phase_increment_[kEnvelopeDead] = 0;
  }

  void Trigger(uint8_t stage) {
    if (stage == kEnvelopeDead) {
      value_ = 0;
    }
    start_ = static_cast<uint8_t>(value_ >> 8);
    target_ = stage_target_[stage];
    stage_ = stage;
    phase_ = 0;
    phase_increment_ = stage_phase_increment_[stage];
  }

  void Update(uint8_t attack, uint8_t decay, uint8_t sustain,
              uint8_t release) {
    stage_phase_increment_[kEnvelopeAttack] =
        tables_.portamento_increments[attack];
    stage_phase_increment_[kEnvelopeDecay] =
        tables_.portamento_increments[decay];
    stage_phase_increment_[kEnvelopeRelease] =
        tables_.portamento_increments[release];
    stage_target_[kEnvelopeDecay] = static_cast<uint8_t>(sustain << 1);
    stage_target_[kEnvelopeSustain] = stage_target_[kEnvelopeDecay];
  }

  // Faithful: byte-exact equivalent of the Classic Envelope::Render().
  // Call exactly once per control block, in the same spot the engine calls
  // the Classic envelope render (voice.cc LoadSources).
  uint8_t RenderNaiveByte() {
    phase_ = static_cast<uint16_t>(phase_ + phase_increment_);
    if (phase_ < phase_increment_) {
      value_ = MixU16(start_, target_, 255);
      Trigger(static_cast<uint8_t>(stage_ + 1));
    }
    if (phase_increment_) {
      uint8_t step = Interpolate(tables_.env_expo, phase_);
      value_ = MixU16(start_, target_, step);
    }
    if (stage_ == kEnvelopeSustain) {
      return stage_target_[kEnvelopeDecay];
    }
    return static_cast<uint8_t>(value_ >> 8);
  }

  // Faithful output as float unit gain (0..1). Byte maps losslessly enough
  // for value-linked channels; the byte form is authoritative for parity.
  float RenderNaive() {
    return static_cast<float>(RenderNaiveByte()) * (1.0f / 255.0f);
  }

  // HD: float phase with an explicit segment-end threshold at 2^16; same
  // increments and env_expo shaping as Classic, deterministic and clean.
  // State lives in the unit domain (0..1); the faithful wrap sets the value to
  // the segment target (the Classic uint16 value_ = target*255 drops its >>8),
  // which is exactly the unit target here.
  float RenderHd() {
    phase_f_ += static_cast<float>(phase_increment_);
    if (phase_f_ >= kEndThreshold) {
      value_f_ = static_cast<float>(target_) * (1.0f / 255.0f);
      TriggerHd(static_cast<uint8_t>(stage_ + 1));
    }
    if (phase_increment_) {
      float step = InterpolateF(tables_.env_expo, phase_f_);
      value_f_ = MixF(start_f_, target_f_, step);
    }
    if (stage_ == kEnvelopeSustain) {
      return static_cast<float>(stage_target_[kEnvelopeDecay]) *
             (1.0f / 255.0f);
    }
    return value_f_;
  }

  uint8_t stage() const { return stage_; }
  uint16_t value() const { return value_; }
  bool dead() const { return stage_ == kEnvelopeDead; }

 private:
  void TriggerHd(uint8_t stage) {
    if (stage == kEnvelopeDead) {
      value_f_ = 0.0f;
    }
    start_f_ = value_f_;
    target_f_ = static_cast<float>(stage_target_[stage]) * (1.0f / 255.0f);
    stage_ = stage;
    phase_f_ = 0.0f;
    phase_increment_ = stage_phase_increment_[stage];
  }

  // Exact Classic primitives (avrlib/op.h), replicated.
  static uint16_t MixU16(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(a * (255 - balance) + b * balance);
  }
  static uint8_t MixU8(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>((a * (255 - balance) + b * balance) >> 8);
  }
  static uint8_t Interpolate(const uint8_t* table, uint16_t phase) {
    uint8_t p = static_cast<uint8_t>(phase >> 8);
    return MixU8(table[p], table[p + 1], static_cast<uint8_t>(phase & 0xff));
  }
  static float MixF(float a, float b, float step) {
    return a + (b - a) * (step * (1.0f / 255.0f));
  }
  static float InterpolateF(const uint8_t* table, float phase) {
    int p = static_cast<int>(phase) >> 8;
    float frac = phase - static_cast<float>(p << 8);
    return MixU8(table[p], table[p + 1], static_cast<uint8_t>(frac)) * (1.0f / 255.0f);
  }

  static constexpr float kEndThreshold = 65536.0f;

  EnvelopeTables tables_{};

  // Faithful integer state (mirrors the Classic class layout and semantics).
  uint16_t stage_phase_increment_[kNumEnvelopeSegments]{};
  uint8_t stage_target_[kNumEnvelopeSegments]{};
  uint8_t stage_ = 0;
  uint8_t start_ = 0;
  uint8_t target_ = 0;
  uint16_t phase_increment_ = 0;
  uint16_t phase_ = 0;
  uint16_t value_ = 0;

  // HD float state.
  float start_f_ = 0.0f;
  float target_f_ = 0.0f;
  float phase_f_ = 0.0f;
  float value_f_ = 0.0f;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdEnvelope);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_ENVELOPE_H_