// Copyright 2026 MontroneDSP.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// avrlib_hd: HD Shruthi voice. Created independently for the HD engine;
// not copied from Shruthi avrlib / Shruthi firmware (GPL-3.0, (c) 2009
// Emilie Gillet).
//
// Faithful byte-for-byte port of shruthi::Voice (voice.h / voice.cc):
//   - LoadSources: envelope render, CV operators, destination init
//   - ProcessModulationMatrix: matrix rows, wheel scaling, trigger-env
//   - UpdateDestinations: cutoff tracking, osc params, envelope params
//   - RenderOscillators: pitch calc, oscillator increment lookup, render
//   - Mixer: OP_SUM through OP_PING_PONG_SEQ, sub, transient, bitcrush, noise
//
// The voice maintains its own sub-components (HdOscillator, HdEnvelope,
// HdSubOscillator, HdTransientGenerator) and exposes the final mixed
// uint8_t output buffer for parity testing against the Classic Voice.
//
// See docs/HD_SHRUTHI_ARCHITECTURE.md and HD_AVRLIB_PLAN.md.

#ifndef AVRLIB_HD_VOICE_H_
#define AVRLIB_HD_VOICE_H_

#include <cstring>

#include "avrlib_hd/envelope.h"
#include "avrlib_hd/mod_matrix.h"
#include "avrlib_hd/oscillator.h"
#include "avrlib_hd/random.h"
#include "avrlib_hd/sub_oscillator.h"
#include "avrlib_hd/transient_generator.h"
#include "avrlib_hd/types.h"

namespace avrlib_hd {

// Numeric mirrors of Classic src/patch.h Operator, kept so the mixer
// dispatch matches the firmware's switch exactly.
enum MixOperator : uint8_t {
  kMixOpSum = 0,
  kMixOpSync,
  kMixOpRingMod,
  kMixOpXor,
  kMixOpFuzz,
  kMixOpCrush4,
  kMixOpCrush8,
  kMixOpFold,
  kMixOpBits,
  kMixOpDuo,
  kMixOpPingPong2,
  kMixOpPingPong4,
  kMixOpPingPong8,
  kMixOpPingPongSeq,
  kMixOpLast
};

// Classic SubOscillatorAlgorithm enum values: 0..5 are the six sub-osc
// waves, 6..10 are transient generator one-shots.
enum SubOscShape : uint8_t {
  kSubOscShapeSquare1 = 0,
  kSubOscShapeTriangle1 = 1,
  kSubOscShapePulse1 = 2,
  kSubOscShapeSquare2 = 3,
  kSubOscShapeTriangle2 = 4,
  kSubOscShapePulse2 = 5,
  kSubOscShapeClick = 6,
  kSubOscShapeGlitch = 7,
  kSubOscShapeBlow = 8,
  kSubOscShapeMetallic = 9,
  kSubOscShapePop = 10
};

// Classic filter board IDs used by UpdateDestinations for PVK offset and
// SVF coupled mode.
enum FilterBoard : uint8_t {
  kFilterBoardLpf = 0,
  kFilterBoardSsm,
  kFilterBoardSvf,
  kFilterBoardDsp,
  kFilterBoardPvk,
  kFilterBoard4Pm,
  kFilterBoardDly,
  kFilterBoardSp,
  kFilterBoardLast
};

// Classic MainFilterMode for the SVF coupled check.
enum MainFilterMode : uint8_t {
  kFilterModeLp = 0,
  kFilterModeBp,
  kFilterModeHp,
  kFilterModeLpCoupled,
  kFilterModeBpCoupled,
  kFilterModeHpCoupled
};

// Minimal patch subset read by the voice. Populated by the caller from the
// real shruthi::Patch; field layout mirrors the Classic byte layout exactly.
struct HdVoicePatch {
  struct OscSettings {
    uint8_t shape;
    uint8_t parameter;
    int8_t range;
    uint8_t option;
  };
  struct EnvSettings {
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
  };

  OscSettings osc[2]{};
  uint8_t mix_balance = 0;
  uint8_t mix_sub_osc = 0;
  uint8_t mix_noise = 0;
  uint8_t mix_sub_osc_shape = 0;
  uint8_t filter_cutoff = 0;
  uint8_t filter_resonance = 0;
  int8_t filter_env = 0;
  int8_t filter_lfo = 0;
  EnvSettings env[2]{};
  UniModulation mod[kModulationMatrixSize]{};
  UniOperator ops[2]{};
  uint8_t filter_cutoff_2 = 0;
  uint8_t filter_resonance_2 = 0;
  uint8_t filter_1_mode = 0;
};

// Minimal system settings read by the voice.
struct HdVoiceSystemSettings {
  uint8_t expansion_filter_board = 0;
  int8_t octave = 0;
  int8_t master_tuning = 0;
};

// Raw table pointers for the pitch lookup and mixer primitives.
struct HdVoiceTables {
  const uint16_t* oscillator_increments = nullptr;  // lut_res_oscillator_increments
  const uint16_t* portamento_increments = nullptr;  // lut_res_env_portamento_increments
  const uint8_t* distortion = nullptr;              // wav_res_distortion
};

// MIDI pitch constants, matching Classic voice.h.
static constexpr int16_t kVoiceLowestNote = 0 * 128;
static constexpr int16_t kVoiceHighestNote = 128 * 128;
static constexpr int16_t kVoiceOctave = 12 * 128;
static constexpr int16_t kVoicePitchTableStart = 116 * 128;

// MIDI controller numbers used by Voice::ControlChange (midi/midi.h).
static constexpr uint8_t kMidiModulationWheelMsb = 0x01;
static constexpr uint8_t kMidiModulationWheelJoystickMsb = 0x02;
static constexpr uint8_t kMidiFootPedalMsb = 0x04;
static constexpr uint8_t kMidiVolume = 0x07;
static constexpr uint8_t kMidiAssignableCcA = 0x10;
static constexpr uint8_t kMidiAssignableCcB = 0x11;
static constexpr uint8_t kMidiAssignableCcC = kMidiModulationWheelJoystickMsb;
static constexpr uint8_t kMidiAssignableCcD = kMidiFootPedalMsb;

class HdVoice {
 public:
  HdVoice() = default;

  void Init(Random* random) {
    random_ = random;
    osc_[0].set_random(random_);
    osc_[1].set_random(random_);
    osc_[0].set_user_wavetable(user_wavetable_);
    osc_[1].set_user_wavetable(user_wavetable_);
    transient_gen_.Init(random);
    pitch_value_ = 60 << 7;
    for (int i = 0; i < 2; ++i) {
      envelope_[i].Init();
    }
    std::memset(no_sync_, 0, kAudioBlockSize);
    NoteOff();
    ResetAllControllers();
  }

  // Loads the user wavetable data (Classic ResourcesManager::Load).
  void set_user_wavetable(const uint8_t* data, uint16_t size) {
    if (data != nullptr && size > 0) {
      uint16_t copy_size = size < HdOscillator::kUserWavetableSize
                               ? size
                               : HdOscillator::kUserWavetableSize;
      std::memcpy(user_wavetable_, data, copy_size);
    }
  }

  void NoteOn(uint16_t pitch, uint8_t velocity, uint8_t portamento,
              bool trigger) {
    if (pitch == 0) {
      return;
    }
    pitch_target_ = pitch;
    if (trigger || pitch_value_ == 0) {
      for (int i = 0; i < 2; ++i) {
        if (!disable_envelope_auto_retriggering_[i]) {
          envelope_[i].Trigger(kEnvelopeAttack);
        }
      }
      ++trigger_count_;
      gate_ = 255;
      transient_gen_.Trigger();
      modulation_sources_[kModSourceVelocity] =
          static_cast<uint8_t>(velocity << 1);
      modulation_sources_[kModSourceRandom] =
          (random_ != nullptr) ? random_->state_msb() : 0;
      osc_[0].Reset();
      osc_[1].Reset();
    }
    if (portamento) {
      int16_t delta = pitch_target_ - pitch_value_;
      uint16_t increment =
          tables_ && tables_->portamento_increments
              ? tables_->portamento_increments[portamento]
              : 0;
      pitch_increment_ = static_cast<int16_t>(
          (static_cast<int32_t>(delta) * increment) >> 16);
      if (pitch_increment_ == 0) {
        pitch_increment_ = (delta < 0) ? -1 : 1;
      }
    } else {
      pitch_value_ = pitch_target_;
      pitch_increment_ = 1;
    }
  }

  void NoteOff() {
    gate_ = 0;
    envelope_[0].Trigger(kEnvelopeRelease);
    envelope_[1].Trigger(kEnvelopeRelease);
  }

  void Kill() {
    envelope_[0].Trigger(kEnvelopeDead);
    envelope_[1].Trigger(kEnvelopeDead);
  }

  void ControlChange(uint8_t controller, uint8_t value) {
    value <<= 1;
    switch (controller) {
      case kMidiAssignableCcA:
        modulation_sources_[kModSourceCcA] = value;
        break;
      case kMidiAssignableCcB:
        modulation_sources_[kModSourceCcB] = value;
        break;
      case kMidiAssignableCcC:
        modulation_sources_[kModSourceCcC] = value;
        break;
      case kMidiAssignableCcD:
        modulation_sources_[kModSourceCcD] = value;
        break;
      case kMidiModulationWheelMsb:
        modulation_sources_[kModSourceWheel] = value;
        break;
      case kMidiVolume:
        volume_ = value;
        break;
    }
  }

  void Aftertouch(uint8_t value) {
    modulation_sources_[kModSourceAftertouch] =
        static_cast<uint8_t>(value << 1);
  }

  void PitchBend(uint16_t value) {
    modulation_sources_[kModSourcePitchBend] =
        static_cast<uint8_t>(value >> 6);
  }

  void ResetAllControllers() {
    modulation_sources_[kModSourceValue4] = 4;
    modulation_sources_[kModSourceValue8] = 8;
    modulation_sources_[kModSourceValue16] = 16;
    modulation_sources_[kModSourceValue32] = 32;
    modulation_sources_[kModSourcePitchBend] = 128;
    modulation_sources_[kModSourceWheel] = 0;
    modulation_sources_[kModSourceOffset] = 255;
    volume_ = 255;
  }

  void set_volume(uint8_t v) { volume_ = v; }
  void set_bass_note(int16_t note) { pitch_bass_note_ = note; }
  void set_modulation_source(uint8_t i, uint8_t value) {
    modulation_sources_[i] = value;
  }
  void set_tables(const HdVoiceTables* tables) { tables_ = tables; }
  void set_envelope_tables(const EnvelopeTables& tables) {
    envelope_[0].set_tables(tables);
    envelope_[1].set_tables(tables);
  }
  void set_user_wavetable(uint8_t* wt) {
    osc_[0].set_user_wavetable(wt);
    osc_[1].set_user_wavetable(wt);
  }

  uint8_t modulation_source(uint8_t i) const { return modulation_sources_[i]; }
  uint8_t modulation_destination(uint8_t i) const {
    return modulation_destinations_[i];
  }

  void import_modulation_sources(const uint8_t* src) {
    std::memcpy(modulation_sources_, src, sizeof(modulation_sources_));
  }
  void export_modulation_sources(uint8_t* dst) const {
    std::memcpy(dst, modulation_sources_, sizeof(modulation_sources_));
  }
  void export_modulation_destinations(int8_t* dst) const {
    std::memcpy(dst, modulation_destinations_,
                sizeof(modulation_destinations_));
  }
  void export_dst(int16_t* dst) const {
    std::memcpy(dst, dst_, sizeof(dst_));
  }

  void RefreshEnvelopeRates(const HdVoicePatch& patch) {
    envelope_[0].Update(patch.env[0].attack, patch.env[0].decay,
                        patch.env[0].sustain, patch.env[0].release);
    envelope_[1].Update(patch.env[1].attack, patch.env[1].decay,
                        patch.env[1].sustain, patch.env[1].release);
  }

  HdOscillator* mutable_oscillator(int i) { return &osc_[i]; }
  const HdOscillator* oscillator(int i) const { return &osc_[i]; }

  // Accessors for parity testing.
  const uint8_t* output_buffer() const { return output_; }
  const uint8_t* osc1_buffer() const { return osc1_premix_; }
  const uint8_t* osc2_buffer() const { return osc2_buffer_; }
  const uint8_t* sync_state_buffer() const { return sync_state_; }
  int16_t debug_destination14(uint8_t i) const { return dst_[i]; }

  // Core processing: control block + audio render.
  void ProcessBlock(const HdVoicePatch& patch,
                    const HdVoiceSystemSettings& sys) {
    LoadSources(patch, sys);
    ProcessModulationMatrix(patch);
    UpdateDestinations(patch, sys);
    RenderOscillators(patch, sys);
    RenderMixer(patch);
  }

  void ProcessControlBlock(const HdVoicePatch& patch,
                           const HdVoiceSystemSettings& sys) {
    LoadSources(patch, sys);
    ProcessModulationMatrix(patch);
    UpdateDestinations(patch, sys);
  }

  void TriggerEnvelope(uint8_t stage) {
    envelope_[0].Trigger(stage);
    envelope_[1].Trigger(stage);
  }

  void TriggerEnvelope(uint8_t index, uint8_t stage) {
    envelope_[index].Trigger(stage);
  }

  bool amplitude_envelope_dead() const { return envelope_[1].dead(); }
  int16_t cutoff_matrix_delta() const { return cutoff_matrix_delta_; }
  int16_t resonance_matrix_delta() const { return resonance_matrix_delta_; }
  int16_t pitch_value() const { return pitch_value_; }
  uint8_t gate() const { return gate_; }
  uint8_t volume() const { return volume_; }

 private:
  // =========================================================================
  // Primitives (Classic src/avrlib/op.h, replicated exactly).
  // =========================================================================

  static uint8_t U8AddClip(uint8_t value, uint8_t increment, uint8_t max) {
    uint16_t r = static_cast<uint16_t>(value) + increment;
    return r > max ? max : static_cast<uint8_t>(r);
  }
  static uint8_t U14ShiftRight6(uint16_t v) {
    return static_cast<uint8_t>(v >> 6);
  }
  static uint8_t U15ShiftRight7(uint16_t v) {
    return static_cast<uint8_t>(v >> 7);
  }
  static uint8_t ByteFromSample(float sample) {
    // Inverse of HdOscillator::ToSample: (byte - 128) / 128. The float path is
    // exact for these representable values, so the recovered byte is exact.
    return static_cast<uint8_t>(static_cast<int>(sample * 128.0f) + 128);
  }
  static uint16_t U16ShiftRight4(uint16_t v) { return v >> 4; }
  static uint8_t U8ShiftLeft4(uint8_t v) {
    return static_cast<uint8_t>(v << 4);
  }
  static uint16_t U8U8Mul(uint8_t a, uint8_t b) {
    return static_cast<uint16_t>(static_cast<uint16_t>(a) * b);
  }
  static uint8_t U8U8MulShift8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(U8U8Mul(a, b) >> 8);
  }
  static int16_t S8U8Mul(int8_t a, uint8_t b) {
    return static_cast<int16_t>(a * b);
  }
  static int16_t S8S8Mul(int8_t a, int8_t b) {
    return static_cast<int16_t>(a * b);
  }
  static int8_t S8U8MulShift8(int8_t a, uint8_t b) {
    return static_cast<int8_t>((static_cast<int16_t>(a) * b) >> 8);
  }
  static int8_t S8S8MulShift8(int8_t a, int8_t b) {
    return static_cast<int8_t>((static_cast<int16_t>(a) * b) >> 8);
  }
  static uint8_t S16ShiftRight8(int16_t v) {
    return static_cast<uint8_t>(v >> 8);
  }
  static uint8_t S16ClipU14Byte(int16_t value) {
    uint8_t msb = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
    if (msb & 0x80) return 0;
    if (msb & 0x40) return 255;
    return static_cast<uint8_t>(value >> 6);
  }
  static int16_t S16ClipU14(int16_t value) {
    uint8_t msb = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
    if (msb & 0x80) return 0;
    if (msb & 0x40) return 16383;
    return value;
  }
  static uint16_t U16U8MulShift8(uint16_t a, uint8_t b) {
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(a) * b) >> 8);
  }
  static uint16_t U8MixU16(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint16_t>(
        a * (255 - balance) + b * balance);
  }
  static uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance) {
    return static_cast<uint8_t>(U8MixU16(a, b, balance) >> 8);
  }
  static uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t gain_a, uint8_t gain_b) {
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(a) * gain_a +
         static_cast<uint16_t>(b) * gain_b) >> 8);
  }
  static int16_t Clip(int16_t value, int16_t min_val, int16_t max_val) {
    return value < min_val ? min_val : (value > max_val ? max_val : value);
  }

  // =========================================================================
  // LoadSources: envelope render, CV operators, destination init.
  // =========================================================================
  void LoadSources(const HdVoicePatch& patch,
                   const HdVoiceSystemSettings& sys) {
    modulation_sources_[kModSourceEnv1] = envelope_[0].RenderNaiveByte();
    modulation_sources_[kModSourceEnv2] = envelope_[1].RenderNaiveByte();
    modulation_sources_[kModSourceNote] =
        U14ShiftRight6(static_cast<uint16_t>(pitch_value_));
    modulation_sources_[kModSourceGate] = gate_;
    modulation_sources_[kModSourceAudio] = buffer_[0];

    for (int i = 0; i < 2; ++i) {
      if (patch.ops[i].op == kOpCvNone) {
        continue;
      }
      uint8_t x = modulation_sources_[patch.ops[i].operands[0]];
      uint8_t y = modulation_sources_[patch.ops[i].operands[1]];
      uint8_t op = patch.ops[i].op;
      if (op <= kOpCvLe) {
        uint8_t ops[8] = {};
        if (x > y) {
          ops[3] = x; ops[6] = 255;
          ops[4] = y; ops[7] = 0;
        } else {
          ops[3] = y; ops[6] = 0;
          ops[4] = x; ops[7] = 255;
        }
        ops[1] = static_cast<uint8_t>((x >> 1) + (y >> 1));
        ops[2] = U8U8MulShift8(x, y);
        ops[5] = static_cast<uint8_t>(x ^ y);
        modulation_sources_[kModSourceOp1 + i] = ops[op];
      } else if (op == kOpCvQuantize) {
        uint8_t mask = 0;
        uint8_t yy = y;
        while (yy >>= 1) {
          mask = static_cast<uint8_t>(mask >> 1);
          mask = static_cast<uint8_t>(mask | 0x80);
        }
        modulation_sources_[kModSourceOp1 + i] =
            static_cast<uint8_t>(x & mask);
      } else if (op == kOpCvLagProcessor) {
        uint8_t yy = static_cast<uint8_t>((y >> 2) + 1);
        uint16_t v = static_cast<uint16_t>(
            modulation_sources_[kModSourceOp1 + i]) * (256 - yy);
        v += static_cast<uint16_t>(x) * yy;
        modulation_sources_[kModSourceOp1 + i] = static_cast<uint8_t>(v >> 8);
      }
    }

    modulation_destinations_[kModDestVca] = volume_;

    dst_[kModDestFilterCutoff] = static_cast<int16_t>(
        U8U8Mul(patch.filter_cutoff, 128));
    dst_[kModDestPwm1] = static_cast<int16_t>(
        U8U8Mul(patch.osc[0].parameter, 128));
    dst_[kModDestPwm2] = static_cast<int16_t>(
        U8U8Mul(patch.osc[1].parameter, 128));

    dst_[kModDestVco1_2Coarse] = 8192;
    dst_[kModDestVco1_2Fine] = 8192;
    dst_[kModDestVco1] = 8192;
    dst_[kModDestVco2] = 8192;
    dst_[kModDestLfo1] = 8192;
    dst_[kModDestLfo2] = 8192;
    dst_[kModDestTriggerEnv1] = 0;
    dst_[kModDestTriggerEnv2] = 0;

    dst_[kModDestFilterResonance] =
        static_cast<int16_t>(static_cast<uint16_t>(patch.filter_resonance) << 8);
    dst_[kModDestMixBalance] =
        static_cast<int16_t>(static_cast<uint16_t>(patch.mix_balance) << 8);
    dst_[kModDestMixNoise] =
        static_cast<int16_t>(static_cast<uint16_t>(patch.mix_noise) << 8);
    dst_[kModDestMixSubOsc] =
        static_cast<int16_t>(static_cast<uint16_t>(patch.mix_sub_osc) << 8);

    dst_[kModDestAttack] = 8192;
    int16_t* ep = &dst_[kModDestAttack1];
    for (int i = 0; i < 2; ++i) {
      *ep++ = static_cast<int16_t>(U8U8Mul(patch.env[i].attack, 128));
      *ep++ = static_cast<int16_t>(U8U8Mul(patch.env[i].decay, 128));
      *ep++ = static_cast<int16_t>(U8U8Mul(patch.env[i].sustain, 128));
      *ep++ = static_cast<int16_t>(U8U8Mul(patch.env[i].release, 128));
    }
    dst_[kModDestCv1] = 0;
    dst_[kModDestCv2] = 0;
    if (sys.expansion_filter_board >= kFilterBoardSsm) {
      dst_[kModDestCv1] = static_cast<int16_t>(
          U8U8Mul(patch.filter_cutoff_2, 128));
      dst_[kModDestCv2] =
          static_cast<int16_t>(static_cast<uint16_t>(patch.filter_resonance_2) << 8);
    }
  }

  // =========================================================================
  // ProcessModulationMatrix: matrix rows, wheel scaling, trigger-env,
  // multiplicative VCA.
  // =========================================================================
  void ProcessModulationMatrix(const HdVoicePatch& patch) {
    for (int i = 0; i < 2; ++i) {
      disable_envelope_auto_retriggering_[i] = 0;
    }
    for (int i = 0; i < kModulationMatrixSize; ++i) {
      int8_t amount = patch.mod[i].amount;
      if (amount == 0) {
        continue;
      }
      if (i == kModulationMatrixSize - 1) {
        amount = S8U8MulShift8(amount, modulation_sources_[kModSourceWheel]);
      }
      uint8_t source = patch.mod[i].source;
      uint8_t destination = patch.mod[i].destination;
      if (destination >= kModDestTriggerEnv1 &&
          destination <= kModDestTriggerEnv2) {
        disable_envelope_auto_retriggering_[destination - kModDestTriggerEnv1] =
            1;
      }
      uint8_t source_value = modulation_sources_[source];
      if (destination != kModDestVca) {
        int16_t modulation = dst_[destination];
        if (source <= kModSourceLfo2 || source == kModSourcePitchBend ||
            source == kModSourceNote || source == kModSourceAudio) {
          modulation += S8S8Mul(amount, static_cast<int8_t>(
              static_cast<uint8_t>(source_value + 128)));
        } else {
          modulation += S8U8Mul(amount, source_value);
        }
        dst_[destination] = S16ClipU14(modulation);
      } else {
        if (amount < 0) {
          amount = static_cast<int8_t>(-amount);
          source_value = static_cast<uint8_t>(255 - source_value);
        }
        if (amount != 63) {
          source_value = U8Mix(255, source_value,
              static_cast<uint8_t>(static_cast<int16_t>(amount) * 4));
        }
        modulation_destinations_[kModDestVca] = U8U8MulShift8(
            modulation_destinations_[kModDestVca], source_value);
      }
    }
    cutoff_matrix_delta_ = static_cast<int16_t>(
        dst_[kModDestFilterCutoff] -
        static_cast<int16_t>(U8U8Mul(patch.filter_cutoff, 128)));
    resonance_matrix_delta_ = static_cast<int16_t>(
        dst_[kModDestFilterResonance] -
        static_cast<int16_t>(static_cast<uint16_t>(patch.filter_resonance) << 8));
  }

  // =========================================================================
  // UpdateDestinations: cutoff tracking, osc params, envelope params.
  // =========================================================================
  void UpdateDestinations(const HdVoicePatch& patch,
                          const HdVoiceSystemSettings& sys) {
    uint16_t cutoff = static_cast<uint16_t>(dst_[kModDestFilterCutoff]);
    if (patch.osc[0].option != kMixOpDuo) {
      if (sys.expansion_filter_board == kFilterBoardPvk) {
        cutoff = S16ClipU14(static_cast<int16_t>(
            cutoff + pitch_value_ - 8192 + (16 << 7)));
      } else {
        cutoff = S16ClipU14(static_cast<int16_t>(
            cutoff + pitch_value_ - 8192));
      }
    }
    cutoff = S16ClipU14(static_cast<int16_t>(
        cutoff + S8U8Mul(patch.filter_env,
            modulation_sources_[kModSourceEnv1])));
    cutoff = S16ClipU14(static_cast<int16_t>(
        cutoff + S8S8Mul(patch.filter_lfo,
            static_cast<int8_t>(static_cast<uint8_t>(
                modulation_sources_[kModSourceLfo2] + 128)))));

    if (sys.expansion_filter_board == kFilterBoardSvf &&
        patch.filter_1_mode >= kFilterModeLpCoupled) {
      dst_[kModDestCv1] = S16ClipU14(static_cast<int16_t>(
          dst_[kModDestCv1] + cutoff - 8192));
    }

    modulation_destinations_[kModDestFilterCutoff] =
        U14ShiftRight6(cutoff);
    modulation_destinations_[kModDestFilterResonance] =
        U14ShiftRight6(static_cast<uint16_t>(dst_[kModDestFilterResonance]));

    modulation_destinations_[kModDestCv1] =
        U14ShiftRight6(static_cast<uint16_t>(dst_[kModDestCv1]));
    modulation_destinations_[kModDestCv2] =
        U14ShiftRight6(static_cast<uint16_t>(dst_[kModDestCv2]));
    modulation_destinations_[kModDestLfo1] =
        S16ShiftRight8(dst_[kModDestLfo1]);
    modulation_destinations_[kModDestLfo2] =
        S16ShiftRight8(dst_[kModDestLfo2]);

    if (dst_[kModDestTriggerEnv1] > 6000) {
      if (!modulation_destinations_[kModDestTriggerEnv1]) {
        envelope_[0].Trigger(kEnvelopeAttack);
      }
      modulation_destinations_[kModDestTriggerEnv1] = -1;
    } else {
      modulation_destinations_[kModDestTriggerEnv1] = 0;
    }

    if (dst_[kModDestTriggerEnv2] > 6000) {
      if (!modulation_destinations_[kModDestTriggerEnv2]) {
        envelope_[1].Trigger(kEnvelopeAttack);
      }
      modulation_destinations_[kModDestTriggerEnv2] = -1;
    } else {
      modulation_destinations_[kModDestTriggerEnv2] = 0;
    }

    osc_[0].set_parameter(U15ShiftRight7(
        static_cast<uint16_t>(dst_[kModDestPwm1])));
    osc_[0].set_secondary_parameter(
        static_cast<uint8_t>(patch.osc[0].range + 24));
    osc_[1].set_parameter(U15ShiftRight7(
        static_cast<uint16_t>(dst_[kModDestPwm2])));
    osc_[1].set_secondary_parameter(
        static_cast<uint8_t>(patch.osc[1].range + 24));

    int8_t attack_mod = static_cast<int8_t>(
        (U15ShiftRight7(static_cast<uint16_t>(dst_[kModDestAttack])) - 64) * 2);
    int16_t* ep = &dst_[kModDestAttack1];
    for (int i = 0; i < 2; ++i) {
      envelope_[i].Update(
          static_cast<uint8_t>(Clip(
              static_cast<int16_t>(U15ShiftRight7(static_cast<uint16_t>(ep[0]))) -
                  attack_mod,
              0, 127)),
          U15ShiftRight7(static_cast<uint16_t>(ep[1])),
          U15ShiftRight7(static_cast<uint16_t>(ep[2])),
          U15ShiftRight7(static_cast<uint16_t>(ep[3])));
      ep += 4;
    }
  }

  // =========================================================================
  // RenderOscillators: pitch calc, increment lookup, oscillator render.
  // =========================================================================
  void RenderOscillators(const HdVoicePatch& patch,
                         const HdVoiceSystemSettings& sys) {
    int16_t base_pitch = pitch_value_ + pitch_increment_;
    if ((pitch_increment_ > 0) ^ (base_pitch < pitch_target_)) {
      base_pitch = pitch_target_;
      pitch_increment_ = 0;
    }
    pitch_value_ = base_pitch;

    base_pitch += static_cast<int16_t>(
        (dst_[kModDestVco1_2Coarse] - 8192) >> 4);
    base_pitch += static_cast<int16_t>(
        (dst_[kModDestVco1_2Fine] - 8192) >> 7);
    base_pitch += sys.master_tuning;

    for (int i = 0; i < 2; ++i) {
      int16_t pitch = base_pitch;

      if (patch.osc[0].option == kMixOpDuo && i == 0 && pitch_bass_note_) {
        pitch -= pitch_value_;
        pitch += pitch_bass_note_;
      }

      int8_t range = 0;
      if (patch.osc[i].shape != kRealFm) {
        range += patch.osc[i].range;
      }
      range += static_cast<int8_t>(sys.octave * 12);
      pitch = static_cast<int16_t>(pitch + S8U8Mul(range, 128));
      if (i == 1) {
        pitch += patch.osc[1].option;
      }
      pitch += static_cast<int16_t>((dst_[kModDestVco1 + i] - 8192) >> 2);

      while (pitch >= kVoiceHighestNote) {
        pitch -= kVoiceOctave;
      }

      int16_t ref_pitch = static_cast<int16_t>(
          pitch - kVoicePitchTableStart);
      uint8_t num_shifts = 0;
      while (ref_pitch < 0) {
        ref_pitch += kVoiceOctave;
        ++num_shifts;
      }

      uint32_t increment = 0;
      uint16_t pitch_idx = U16ShiftRight4(static_cast<uint16_t>(ref_pitch));
      uint8_t pitch_frac = U8ShiftLeft4(static_cast<uint8_t>(ref_pitch));

      if (tables_->oscillator_increments) {
        uint16_t inc16 = tables_->oscillator_increments[pitch_idx];
        uint16_t inc16_next = tables_->oscillator_increments[pitch_idx + 1];
        uint16_t inc16_interp = static_cast<uint16_t>(
            inc16 + U16U8MulShift8(
                static_cast<uint16_t>(inc16_next - inc16), pitch_frac));
        increment = static_cast<uint32_t>(inc16_interp) << 8;
        while (num_shifts--) {
          increment >>= 1;
        }
      }

      int8_t midi_note = U15ShiftRight7(static_cast<uint16_t>(pitch));
      if (midi_note < 12) {
        midi_note = 12;
      }

      if (i == 0) {
        sub_osc_.set_increment(increment >> 1);
        osc_[0].RenderNaiveFull(
            patch.osc[0].shape, static_cast<uint8_t>(midi_note),
            increment, kAudioBlockSize, osc1_scratch_,
            no_sync_, sync_state_);
        for (int s = 0; s < kAudioBlockSize; ++s) {
          buffer_[s] = ByteFromSample(osc1_scratch_[s]);
        }
      } else {
        uint8_t shape = patch.osc[1].shape;
        if (patch.osc[0].option == kMixOpDuo && !pitch_bass_note_) {
          shape = 0;
        }
        const uint8_t* sync_in =
            (patch.osc[0].option == kMixOpSync) ? sync_state_ : no_sync_;
        osc_[1].RenderNaiveFull(
            shape, static_cast<uint8_t>(midi_note),
            increment, kAudioBlockSize, osc2_scratch_,
            sync_in, dummy_sync_state_);
        for (int s = 0; s < kAudioBlockSize; ++s) {
          osc2_buffer_[s] = ByteFromSample(osc2_scratch_[s]);
        }
      }
    }
    std::memcpy(osc1_premix_, buffer_, kAudioBlockSize);
  }

  // =========================================================================
  // RenderMixer: ops, sub, transient, bitcrush, noise, output.
  // =========================================================================
  void RenderMixer(const HdVoicePatch& patch) {
    uint8_t op = patch.osc[0].option;
    uint8_t enabled_source_bitmask = 0xff;

    static const uint8_t four_step_sequence[4] = {4, 1, 8, 2};
    static const uint8_t eight_step_sequence[8] = {4, 1, 8, 8, 2, 4, 8, 2};

    if (op == kMixOpPingPong2) {
      enabled_source_bitmask =
          (trigger_count_ & 1) ? 0x0e : 0x0d;
    } else if (op == kMixOpPingPong4) {
      enabled_source_bitmask =
          four_step_sequence[trigger_count_ & 3];
    } else if (op == kMixOpPingPong8) {
      enabled_source_bitmask =
          eight_step_sequence[trigger_count_ & 7];
    } else if (op == kMixOpPingPongSeq) {
      enabled_source_bitmask =
          static_cast<uint8_t>(modulation_sources_[kModSourceSeq] >> 4);
    }

    uint8_t osc_2_gain = U14ShiftRight6(
        static_cast<uint16_t>(dst_[kModDestMixBalance]));
    uint8_t osc_1_gain = static_cast<uint8_t>(~osc_2_gain);
    if (enabled_source_bitmask != 0xff && (enabled_source_bitmask & 3) != 3) {
      osc_2_gain = (enabled_source_bitmask & 1) ? 255 : 0;
      osc_1_gain = (enabled_source_bitmask & 2) ? 255 : 0;
    }
    if (!osc_1_gain && !osc_2_gain) {
      std::memset(buffer_, 128, kAudioBlockSize);
    } else {
      switch (op) {
        case kMixOpRingMod:
          for (int i = 0; i < kAudioBlockSize; ++i) {
            uint8_t ring_mod = static_cast<uint8_t>(
                S8S8MulShift8(
                    static_cast<int8_t>(static_cast<uint8_t>(buffer_[i] + 128)),
                    static_cast<int8_t>(static_cast<uint8_t>(osc2_buffer_[i] + 128))) + 128);
            buffer_[i] = U8Mix(buffer_[i], ring_mod, osc_1_gain, osc_2_gain);
          }
          break;
        case kMixOpFuzz:
          for (int i = 0; i < kAudioBlockSize; ++i) {
            buffer_[i] >>= 1;
            buffer_[i] += (osc2_buffer_[i] >> 1);
            buffer_[i] = U8Mix(
                buffer_[i],
                tables_->distortion ? tables_->distortion[buffer_[i]]
                                    : buffer_[i],
                osc_1_gain, osc_2_gain);
          }
          break;
        case kMixOpXor:
          for (int i = 0; i < kAudioBlockSize; ++i) {
            buffer_[i] ^= osc2_buffer_[i];
            buffer_[i] ^= osc_2_gain;
          }
          break;
        case kMixOpFold:
          for (int i = 0; i < kAudioBlockSize; ++i) {
            buffer_[i] >>= 1;
            buffer_[i] += (osc2_buffer_[i] >> 1);
            buffer_[i] = U8Mix(
                buffer_[i],
                static_cast<uint8_t>(buffer_[i] + 128),
                osc_1_gain, osc_2_gain);
          }
          break;
        case kMixOpBits: {
          uint8_t bits_gain = osc_2_gain;
          bits_gain >>= 5;
          bits_gain = static_cast<uint8_t>(255 - ((1 << bits_gain) - 1));
          for (int i = 0; i < kAudioBlockSize; ++i) {
            buffer_[i] >>= 1;
            buffer_[i] += (osc2_buffer_[i] >> 1);
            buffer_[i] &= bits_gain;
          }
          break;
        }
        default:
          for (int i = 0; i < kAudioBlockSize; ++i) {
            buffer_[i] = U8Mix(buffer_[i], osc2_buffer_[i],
                               osc_1_gain, osc_2_gain);
          }
          break;
      }
    }

    uint8_t decimate = 1;
    if (op == kMixOpCrush4) {
      decimate = 4;
    } else if (op == kMixOpCrush8) {
      decimate = 8;
    }

    uint8_t sub_gain = U15ShiftRight7(
        static_cast<uint16_t>(dst_[kModDestMixSubOsc]));
    if (enabled_source_bitmask != 0xff) {
      sub_gain = (enabled_source_bitmask & 4) ? sub_gain : 0;
      if ((enabled_source_bitmask & 3) == 0) {
        sub_gain <<= 1;
      }
    }
    if (patch.mix_sub_osc_shape < kSubOscShapeClick) {
      sub_osc_.RenderNaiveByte(patch.mix_sub_osc_shape, buffer_, sub_gain);
    } else {
      if (sub_gain < 128) {
        sub_gain <<= 1;
      }
      transient_gen_.RenderNaiveByte(
          patch.mix_sub_osc_shape, buffer_, sub_gain);
    }

    if (decimate > 1) {
      uint8_t* buf = buffer_;
      for (int i = 0; i < kAudioBlockSize; i += decimate) {
        uint8_t value = *buf++;
        for (int j = 1; j < decimate; ++j) {
          *buf++ = value;
        }
      }
    }

    uint8_t noise = (random_ != nullptr) ? random_->GetByte() : 0;
    uint8_t noise_gain = S16ShiftRight8(dst_[kModDestMixNoise]);
    if (enabled_source_bitmask != 0xff) {
      noise_gain = (enabled_source_bitmask & 8) ? noise_gain : 0;
      if (enabled_source_bitmask == 8) {
        noise_gain <<= 1;
      }
    }
    uint8_t mix_gain = static_cast<uint8_t>(~noise_gain);
    for (int i = 0; i < kAudioBlockSize; ++i) {
      noise = static_cast<uint8_t>((noise * 73) + 1);
      output_[i] = U8Mix(buffer_[i], noise, mix_gain, noise_gain);
    }
  }

  // =========================================================================
  // State — mirrors Classic Voice layout.
  // =========================================================================
  HdEnvelope envelope_[2]{};
  uint8_t disable_envelope_auto_retriggering_[2]{};
  uint8_t gate_ = 0;
  int16_t dst_[kNumModulationDestinations]{};
  int16_t cutoff_matrix_delta_ = 0;
  int16_t resonance_matrix_delta_ = 0;

  int16_t pitch_increment_ = 0;
  int16_t pitch_target_ = 0;
  int16_t pitch_value_ = 0;
  int16_t pitch_bass_note_ = 0;

  uint8_t modulation_sources_[kNumModulationSources]{};
  int8_t modulation_destinations_[kNumModulationDestinations]{};

  uint8_t buffer_[kAudioBlockSize]{};
  uint8_t osc1_premix_[kAudioBlockSize]{};
  uint8_t osc2_buffer_[kAudioBlockSize]{};
  uint8_t sync_state_[kAudioBlockSize]{};
  uint8_t no_sync_[kAudioBlockSize]{};
  uint8_t dummy_sync_state_[kAudioBlockSize]{};
  float osc1_scratch_[kAudioBlockSize]{};
  float osc2_scratch_[kAudioBlockSize]{};
  uint8_t trigger_count_ = 0;
  uint8_t volume_ = 0;
  uint8_t output_[kAudioBlockSize]{};
  uint8_t user_wavetable_[HdOscillator::kUserWavetableSize + 1]{};

  Random* random_ = nullptr;
  HdOscillator osc_[2]{};
  HdSubOscillator sub_osc_{};
  HdTransientGenerator transient_gen_{};
  const HdVoiceTables* tables_ = nullptr;

  AVRLIB_HD_DISALLOW_COPY_AND_ASSIGN(HdVoice);
};

}  // namespace avrlib_hd

#endif  // AVRLIB_HD_VOICE_H_
